// SPDX-License-Identifier: Apache-2.0
/*
 * Linux side of the AMP shared-frame ground work.
 *
 * Reads the frames NuttX (cpu_l3) draws into amp-shmem@31000000 and reports
 * back what it saw, so both cores checksum the same physical bytes through
 * different mappings and the two numbers can be compared.
 *
 * Nothing here needs a new kernel driver:
 *   - the pixels are reached with /dev/mem, since the carveout is declared
 *     no-map and is therefore not part of the kernel's linear map;
 *   - the signalling uses /dev/rpmsgN from the in-tree rpmsg_char driver, which
 *     binds automatically because the remote announces the channel under the
 *     one name in that driver's id table, "rpmsg-raw".
 *
 * Build:
 *   aarch64-linux-gnu-gcc -O2 -o amp_shm_test amp_shm_test.c
 *
 * Run (rpmsg_char must be loaded so /dev/rpmsg0 exists):
 *   ./amp_shm_test            request 5 frames and verify each
 *   ./amp_shm_test -n 100     soak
 *   ./amp_shm_test -p         passive: verify whatever is already there
 *
 * The layout below has to match
 * boards/arm64/rk3588/evb7-amp/src/evb7_amp_shm.h in the NuttX tree.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/mman.h>

#define AMP_SHM_BASE       0x31000000UL
#define AMP_SHM_SIZE       (4 * 1024 * 1024)
#define AMP_SHM_MAGIC      0x30424641u    /* "AFB0" */
#define AMP_SHM_NBUFFERS   2

/*
 * The first page is skipped because something outside this project writes three
 * words at its start. Measured with amp_shm_scan: over 60s exactly +0x0c, +0x10
 * and +0x14 changed and the other 1048573 words did not. The control block used
 * to sit at offset 0 and had its height, stride and bpp fields overwritten,
 * while the frame buffers themselves checksummed correctly.
 */
#define AMP_SHM_HDR_OFFSET 4096

#define AMP_SHM_CMD_RENDER 1
#define AMP_SHM_CMD_READY  2
#define AMP_SHM_CMD_ACK    3

struct amp_shm_ctrl {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t bpp;
	uint32_t nbuffers;
	uint32_t bufsize;
	uint32_t bufoffset[AMP_SHM_NBUFFERS];
	uint32_t frame_seq;
	uint32_t ready_index;
	uint32_t ready_sum;
} __attribute__((packed));

struct amp_shm_msg {
	uint32_t cmd;
	uint32_t seq;
	uint32_t index;
	uint32_t sum;
} __attribute__((packed));

/*
 * Sum the buffer the same way the remote does: 32-bit words, wrapping. Read
 * through a volatile pointer so the compiler cannot decide to reuse an earlier
 * load - the memory is written by another core and is not ordinary DRAM as far
 * as this process is concerned.
 */
static uint32_t sum_buffer(const volatile uint32_t *buf, size_t bytes)
{
	uint32_t sum = 0;
	size_t words = bytes / 4;
	size_t i;

	for (i = 0; i < words; i++)
		sum += buf[i];

	return sum;
}

static void dump_ctrl(const struct amp_shm_ctrl *c)
{
	unsigned int i;

	printf("control block at 0x%08lx:\n", AMP_SHM_BASE + AMP_SHM_HDR_OFFSET);
	printf("  magic     0x%08x %s\n", c->magic,
	       c->magic == AMP_SHM_MAGIC ? "(ok)" : "(UNEXPECTED)");
	printf("  version   %u\n", c->version);
	printf("  geometry  %ux%u, %u bpp, stride %u\n",
	       c->width, c->height, c->bpp, c->stride);
	printf("  buffers   %u x %u bytes\n", c->nbuffers, c->bufsize);
	for (i = 0; i < c->nbuffers && i < AMP_SHM_NBUFFERS; i++)
		printf("    buf%u    offset 0x%06x (phys 0x%08lx)\n",
		       i, c->bufoffset[i], AMP_SHM_BASE + c->bufoffset[i]);
	printf("  frame_seq %u, ready_index %u, ready_sum 0x%08x\n",
	       c->frame_seq, c->ready_index, c->ready_sum);
}

int main(int argc, char **argv)
{
	const char *rpmsg_dev = "/dev/rpmsg0";
	int frames = 5;
	bool passive = false;
	int memfd, rpfd = -1;
	void *map;
	volatile struct amp_shm_ctrl *ctrl;
	int failures = 0;
	int i, opt;

	while ((opt = getopt(argc, argv, "n:d:ph")) != -1) {
		switch (opt) {
		case 'n':
			frames = atoi(optarg);
			break;
		case 'd':
			rpmsg_dev = optarg;
			break;
		case 'p':
			passive = true;
			break;
		default:
			printf("usage: %s [-n frames] [-d /dev/rpmsgN] [-p]\n",
			       argv[0]);
			return 1;
		}
	}

	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) {
		perror("open /dev/mem");
		return 1;
	}

	/*
	 * The remote maps this area non-cacheable so that neither side needs
	 * cache maintenance. Map it the same way here: O_SYNC above asks for an
	 * uncached mapping of the physical range, which is what keeps a read
	 * here from being served out of this CPU's cache with data the remote
	 * wrote before.
	 */
	map = mmap(NULL, AMP_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   memfd, AMP_SHM_BASE);
	if (map == MAP_FAILED) {
		perror("mmap");
		close(memfd);
		return 1;
	}

	/* Buffer offsets stay relative to the region base, so only the control
	 * block itself moves past the reserved head.
	 */
	ctrl = (volatile struct amp_shm_ctrl *)((char *)map + AMP_SHM_HDR_OFFSET);

	if (ctrl->magic != AMP_SHM_MAGIC) {
		printf("magic is 0x%08x, expected 0x%08x - the remote has not "
		       "initialised the area (is NuttX running?)\n",
		       ctrl->magic, AMP_SHM_MAGIC);
		munmap(map, AMP_SHM_SIZE);
		close(memfd);
		return 1;
	}

	dump_ctrl((const struct amp_shm_ctrl *)ctrl);

	if (passive) {
		uint32_t idx = ctrl->ready_index;
		uint32_t mine;

		if (ctrl->frame_seq == 0) {
			printf("\nno frame published yet\n");
		} else {
			mine = sum_buffer((volatile uint32_t *)
					  ((char *)map + ctrl->bufoffset[idx]),
					  ctrl->bufsize);
			printf("\nframe %u buf%u: remote 0x%08x, local 0x%08x -> %s\n",
			       ctrl->frame_seq, idx, ctrl->ready_sum, mine,
			       mine == ctrl->ready_sum ? "MATCH" : "MISMATCH");
			failures = (mine != ctrl->ready_sum);
		}

		goto out;
	}

	rpfd = open(rpmsg_dev, O_RDWR);
	if (rpfd < 0) {
		fprintf(stderr, "open %s: %s\n", rpmsg_dev, strerror(errno));
		fprintf(stderr,
			"load the rpmsg_char module first, or use -p to check "
			"the memory path on its own\n");
		munmap(map, AMP_SHM_SIZE);
		close(memfd);
		return 1;
	}

	printf("\nrequesting %d frame(s) over %s\n", frames, rpmsg_dev);

	for (i = 0; i < frames; i++) {
		struct amp_shm_msg req = { AMP_SHM_CMD_RENDER, 0, 0, 0 };
		struct amp_shm_msg rsp;
		struct pollfd pfd = { .fd = rpfd, .events = POLLIN };
		uint32_t mine;
		ssize_t n;

		if (write(rpfd, &req, sizeof(req)) != sizeof(req)) {
			perror("write");
			failures++;
			break;
		}

		if (poll(&pfd, 1, 2000) <= 0) {
			printf("frame %d: no reply within 2s\n", i);
			failures++;
			continue;
		}

		n = read(rpfd, &rsp, sizeof(rsp));
		if (n != sizeof(rsp)) {
			printf("frame %d: short reply (%zd bytes)\n", i, n);
			failures++;
			continue;
		}

		if (rsp.cmd != AMP_SHM_CMD_READY) {
			printf("frame %d: unexpected cmd %u\n", i, rsp.cmd);
			failures++;
			continue;
		}

		/*
		 * Checksum what the remote says it wrote, reading the same
		 * physical bytes through this process's own mapping.
		 */
		mine = sum_buffer((volatile uint32_t *)
				  ((char *)map + ctrl->bufoffset[rsp.index]),
				  ctrl->bufsize);

		printf("frame %u buf%u: remote 0x%08x, local 0x%08x -> %s\n",
		       rsp.seq, rsp.index, rsp.sum, mine,
		       mine == rsp.sum ? "MATCH" : "MISMATCH");

		if (mine != rsp.sum)
			failures++;

		/* Report back, so the remote logs the comparison too. */
		req.cmd = AMP_SHM_CMD_ACK;
		req.seq = rsp.seq;
		req.index = rsp.index;
		req.sum = mine;

		if (write(rpfd, &req, sizeof(req)) != sizeof(req))
			perror("write ack");
	}

out:
	printf("\n%s (%d failure(s))\n", failures ? "FAILED" : "PASSED",
	       failures);

	if (rpfd >= 0)
		close(rpfd);

	munmap(map, AMP_SHM_SIZE);
	close(memfd);

	return failures ? 1 : 0;
}
