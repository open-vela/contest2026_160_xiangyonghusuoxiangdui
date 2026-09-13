// SPDX-License-Identifier: Apache-2.0
/*
 * Find out which parts of the AMP shared carveout are touched by something
 * other than us.
 *
 * A 16-bit-wide writer was observed changing three words near the start of
 * amp-shmem@31000000 while the region is properly reserved (it is absent from
 * /proc/iomem's System RAM and present in /proc/device-tree/reserved-memory),
 * so whatever writes it is not the Linux allocator. Rather than keep guessing
 * at its identity, this measures its footprint: fill the whole region with a
 * pattern derived from each word's own address, wait, and report every word
 * that changed.
 *
 * A word that differs was written by someone else. A word whose address-derived
 * pattern is intact was not, which is the part that matters - the question this
 * answers is which offsets are safe to put frames in.
 *
 * Do not request frames from the remote while this runs: that would rewrite the
 * buffers legitimately and every word in them would report as changed.
 *
 * Build:
 *   aarch64-linux-gnu-gcc -O2 -o amp_shm_scan amp_shm_scan.c
 *
 * Run:
 *   ./amp_shm_scan            fill, wait 10s, scan
 *   ./amp_shm_scan -w 60      wait a minute instead
 *   ./amp_shm_scan -s         scan only, do not fill (see what changed since
 *                             the last run)
 */

#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define AMP_SHM_BASE   0x31000000UL
#define AMP_SHM_SIZE   (4 * 1024 * 1024)

/* Pattern is a function of the offset, so a word that was written from the
 * wrong place, or copied from elsewhere in the region, still shows up.
 */
static inline uint32_t pattern_of(uint32_t off)
{
	return off ^ 0xa5a5a5a5u;
}

#define MAX_REPORT 64

int main(int argc, char **argv)
{
	unsigned int wait_s = 10;
	bool fill = true;
	int memfd, opt;
	void *map;
	volatile uint32_t *w;
	uint32_t off;
	unsigned int changed = 0;
	unsigned int reported = 0;
	uint32_t first = 0xffffffffu, last = 0;

	while ((opt = getopt(argc, argv, "w:sh")) != -1) {
		switch (opt) {
		case 'w':
			wait_s = atoi(optarg);
			break;
		case 's':
			fill = false;
			break;
		default:
			printf("usage: %s [-w seconds] [-s]\n", argv[0]);
			return 1;
		}
	}

	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) {
		perror("open /dev/mem");
		return 1;
	}

	map = mmap(NULL, AMP_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   memfd, AMP_SHM_BASE);
	if (map == MAP_FAILED) {
		perror("mmap");
		close(memfd);
		return 1;
	}

	w = map;

	if (fill) {
		printf("filling 0x%08lx..0x%08lx with an address pattern\n",
		       AMP_SHM_BASE, AMP_SHM_BASE + AMP_SHM_SIZE - 1);

		for (off = 0; off < AMP_SHM_SIZE; off += 4)
			w[off / 4] = pattern_of(off);

		printf("waiting %u s\n", wait_s);
		sleep(wait_s);
	} else {
		printf("scanning without filling first\n");
	}

	for (off = 0; off < AMP_SHM_SIZE; off += 4) {
		uint32_t got = w[off / 4];
		uint32_t want = pattern_of(off);

		if (got == want)
			continue;

		changed++;

		if (off < first)
			first = off;
		last = off;

		if (reported < MAX_REPORT) {
			printf("  +0x%06x (phys 0x%08lx): expected 0x%08x, "
			       "found 0x%08x", off, AMP_SHM_BASE + off,
			       want, got);

			/*
			 * A word where only part of the halfword changed tells
			 * us the access width the other agent uses, which is
			 * worth seeing per offset rather than in aggregate.
			 */
			if ((got & 0xffff0000u) == (want & 0xffff0000u))
				printf("  [low halfword only]");
			else if ((got & 0x0000ffffu) == (want & 0x0000ffffu))
				printf("  [high halfword only]");

			printf("\n");
			reported++;
		}
	}

	printf("\n%u of %u words changed", changed, AMP_SHM_SIZE / 4);

	if (changed) {
		printf(", first +0x%06x, last +0x%06x", first, last);
		if (reported < changed)
			printf(" (only the first %u listed)", reported);
	}

	printf("\n");

	if (!changed)
		printf("nothing outside this process wrote to the region\n");

	munmap(map, AMP_SHM_SIZE);
	close(memfd);

	return changed ? 1 : 0;
}
