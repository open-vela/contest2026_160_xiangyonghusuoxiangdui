// SPDX-License-Identifier: Apache-2.0
/*
 * Put the frames NuttX draws on the panel.
 *
 * NuttX (cpu_l3) owns no display hardware - the VOP's IOMMU and interrupt are
 * shared across all of its video ports, so it cannot be split between cores.
 * What it does instead is draw into amp-shmem@31000000 and say so. This program
 * is the other half: it takes those frames and scans them out through DRM.
 *
 * Two things are deliberately avoided:
 *
 *   - libdrm, because the board's rootfs does not necessarily have it. The half
 *     dozen ioctls needed here are stable uapi, so they are issued directly.
 *   - a framebuffer device, because this kernel has neither CONFIG_FB nor
 *     DRM_FBDEV_EMULATION, so there is no /dev/fb0 to write to.
 *
 * Becoming DRM master requires that nothing else already is, so the graphical
 * session has to be stopped first:
 *
 *   systemctl isolate multi-user.target
 *
 * Stopping the compositor alone is not enough on a systemd rootfs: logind holds
 * DRM master on behalf of the seat and hands the device to whatever compositor
 * the session runs, so killing gnome-shell or weston leaves master taken. The
 * row marked "master" in /sys/kernel/debug/dri/0/clients names the real holder.
 *
 * Build (the DRM uapi headers come from the kernel tree that is running on the
 * board; -idirafter rather than -I so the kernel's linux/types.h does not shadow
 * the C library's, which breaks the build outright):
 *
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra \
 *       -idirafter /media/1t/openvela/kernel/include/uapi \
 *       -o amp_fb_show amp_fb_show.c
 *
 * Run:
 *   ./amp_fb_show              follow the remote, redraw on every new frame
 *   ./amp_fb_show -r 200       also ask the remote for a frame every 200ms,
 *                              which animates its built-in test pattern and
 *                              proves the path without an application on the
 *                              NuttX side
 *   ./amp_fb_show -p           never touch rpmsg, just poll the control block
 *   ./amp_fb_show -n 10        stop after 10 frames
 *
 * The layout below has to match
 * boards/arm64/rk3588/evb7-amp/src/evb7_amp_shm.h in the NuttX tree.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <drm/drm.h>

#define AMP_SHM_BASE       0x31000000UL
#define AMP_SHM_SIZE       (4 * 1024 * 1024)
#define AMP_SHM_MAGIC      0x30424641u    /* "AFB0" */
#define AMP_SHM_NBUFFERS   2

/* The first page is skipped because something outside this project writes three
 * words at its start - see the comment in evb7_amp_shm.h.
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

/* One CRTC driving one connector with one dumb buffer. */

struct display {
	int fd;
	uint32_t crtc_id;
	uint32_t connector_id;
	uint32_t fb_id;
	uint32_t handle;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint64_t size;
	uint8_t *map;
	struct drm_mode_modeinfo mode;
	uint32_t saved_fb_id;                /* what was on the CRTC before  */
	struct drm_mode_modeinfo saved_mode;
	int saved_valid;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

/*
 * Sum a buffer the way the remote does: 32-bit words, wrapping. Read through a
 * volatile pointer so the compiler cannot reuse an earlier load - the memory is
 * written by another core.
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

/****************************************************************************
 * DRM setup
 ****************************************************************************/

/*
 * Pick something to display on: the first connector that is actually connected
 * and reports at least one mode, its preferred mode, and a CRTC that can drive
 * it.
 *
 * Every "get" ioctl here is issued twice, which is how this interface works: the
 * first call with zeroed counts reports how many objects there are, then the
 * caller allocates and asks again. Skipping the first call and guessing a size
 * is the classic way to get a truncated list back without being told.
 */
static int pick_output(struct display *d)
{
	struct drm_mode_card_res res;
	uint32_t *connectors = NULL;
	uint32_t *crtcs = NULL;
	int ret = -1;
	uint32_t i;

	memset(&res, 0, sizeof(res));
	if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("DRM_IOCTL_MODE_GETRESOURCES (count)");
		return -1;
	}

	if (res.count_connectors == 0 || res.count_crtcs == 0) {
		fprintf(stderr, "no connectors (%u) or crtcs (%u)\n",
			res.count_connectors, res.count_crtcs);
		return -1;
	}

	connectors = calloc(res.count_connectors, sizeof(*connectors));
	crtcs = calloc(res.count_crtcs, sizeof(*crtcs));
	if (!connectors || !crtcs) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}

	res.connector_id_ptr = (uint64_t)(uintptr_t)connectors;
	res.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
	res.encoder_id_ptr = 0;
	res.fb_id_ptr = 0;
	res.count_encoders = 0;
	res.count_fbs = 0;

	if (ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
		perror("DRM_IOCTL_MODE_GETRESOURCES");
		goto out;
	}

	for (i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector conn;
		struct drm_mode_modeinfo *modes;
		uint32_t *encoders;
		uint32_t j;

		memset(&conn, 0, sizeof(conn));
		conn.connector_id = connectors[i];
		if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
			fprintf(stderr, "connector %u: GETCONNECTOR: %s\n",
				connectors[i], strerror(errno));
			continue;
		}

		printf("connector %u: type %u-%u, connection %u, %u mode(s),"
		       " encoder %u, %u possible encoder(s)\n",
		       conn.connector_id, conn.connector_type,
		       conn.connector_type_id, conn.connection,
		       conn.count_modes, conn.encoder_id, conn.count_encoders);

		/* connection == 1 is "connected". A connector with no modes is
		 * useless even if it claims to be connected.
		 */

		if (conn.connection != 1 || conn.count_modes == 0)
			continue;

		modes = calloc(conn.count_modes, sizeof(*modes));
		encoders = conn.count_encoders ?
			calloc(conn.count_encoders, sizeof(*encoders)) : NULL;
		if (!modes) {
			free(encoders);
			continue;
		}

		conn.modes_ptr = (uint64_t)(uintptr_t)modes;
		conn.encoders_ptr = (uint64_t)(uintptr_t)encoders;
		conn.props_ptr = 0;
		conn.prop_values_ptr = 0;
		conn.count_props = 0;

		if (ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 ||
		    conn.count_modes == 0) {
			free(modes);
			free(encoders);
			continue;
		}

		/* Mode 0 is the driver's preferred mode. */

		d->mode = modes[0];
		free(modes);

		/* Which encoder to go through.
		 *
		 * conn.encoder_id is only set while something is actually
		 * driving this connector, and after the compositor exits it is
		 * zero - which is exactly the state this program runs in. So the
		 * attached encoder is a preference, not a requirement, and the
		 * connector's own list of possible encoders is the fallback.
		 */

		for (j = 0; j <= conn.count_encoders; j++) {
			struct drm_mode_get_encoder enc;
			uint32_t encoder_id;
			uint32_t bit;

			if (j == 0) {
				encoder_id = conn.encoder_id;
			} else {
				if (!encoders)
					break;
				encoder_id = encoders[j - 1];
			}

			if (encoder_id == 0)
				continue;

			memset(&enc, 0, sizeof(enc));
			enc.encoder_id = encoder_id;
			if (ioctl(d->fd, DRM_IOCTL_MODE_GETENCODER, &enc) < 0)
				continue;

			/* Prefer the CRTC already attached, since u-boot has
			 * already lit this panel and reusing its pipe avoids a
			 * full modeset. Otherwise take the first CRTC this
			 * encoder can reach.
			 */

			if (enc.crtc_id != 0) {
				d->crtc_id = enc.crtc_id;
				break;
			}

			for (bit = 0; bit < res.count_crtcs; bit++) {
				if (enc.possible_crtcs & (1u << bit)) {
					d->crtc_id = crtcs[bit];
					break;
				}
			}

			if (d->crtc_id != 0)
				break;
		}

		free(encoders);

		if (d->crtc_id == 0) {
			fprintf(stderr,
				"connector %u: connected with %u mode(s) but no"
				" encoder leads to a crtc\n",
				conn.connector_id, conn.count_modes);
			continue;
		}

		d->connector_id = conn.connector_id;
		d->width = d->mode.hdisplay;
		d->height = d->mode.vdisplay;
		ret = 0;
		break;
	}

	if (ret < 0)
		fprintf(stderr,
			"no usable connector found (%u connector(s),"
			" %u crtc(s) present)\n",
			res.count_connectors, res.count_crtcs);

out:
	free(connectors);
	free(crtcs);
	return ret;
}

static int create_dumb_fb(struct display *d)
{
	struct drm_mode_create_dumb create;
	struct drm_mode_map_dumb map;
	struct drm_mode_fb_cmd cmd;
	void *ptr;

	memset(&create, 0, sizeof(create));
	create.width = d->width;
	create.height = d->height;
	create.bpp = 32;

	if (ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB");
		return -1;
	}

	d->handle = create.handle;
	d->pitch = create.pitch;
	d->size = create.size;

	/* Legacy ADDFB rather than ADDFB2: depth 24 with 32 bits per pixel is
	 * XRGB8888, which is what the remote's ARGB8888 buffer becomes once the
	 * alpha byte is ignored. No format enum to get wrong.
	 */

	memset(&cmd, 0, sizeof(cmd));
	cmd.width = d->width;
	cmd.height = d->height;
	cmd.pitch = d->pitch;
	cmd.bpp = 32;
	cmd.depth = 24;
	cmd.handle = d->handle;

	if (ioctl(d->fd, DRM_IOCTL_MODE_ADDFB, &cmd) < 0) {
		perror("DRM_IOCTL_MODE_ADDFB");
		return -1;
	}

	d->fb_id = cmd.fb_id;

	memset(&map, 0, sizeof(map));
	map.handle = d->handle;
	if (ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("DRM_IOCTL_MODE_MAP_DUMB");
		return -1;
	}

	ptr = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   d->fd, map.offset);
	if (ptr == MAP_FAILED) {
		perror("mmap dumb buffer");
		return -1;
	}

	d->map = ptr;
	memset(d->map, 0, d->size);
	return 0;
}

static int save_crtc(struct display *d)
{
	struct drm_mode_crtc crtc;

	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = d->crtc_id;
	if (ioctl(d->fd, DRM_IOCTL_MODE_GETCRTC, &crtc) < 0) {
		perror("DRM_IOCTL_MODE_GETCRTC");
		return -1;
	}

	d->saved_fb_id = crtc.fb_id;
	d->saved_mode = crtc.mode;
	d->saved_valid = crtc.mode_valid;
	return 0;
}

static int set_crtc(struct display *d, uint32_t fb_id,
		    const struct drm_mode_modeinfo *mode, int mode_valid)
{
	struct drm_mode_crtc crtc;
	uint32_t connector = d->connector_id;

	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = d->crtc_id;
	crtc.fb_id = fb_id;
	crtc.set_connectors_ptr = (uint64_t)(uintptr_t)&connector;
	crtc.count_connectors = 1;
	crtc.mode = *mode;
	crtc.mode_valid = mode_valid;

	if (ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0) {
		perror("DRM_IOCTL_MODE_SETCRTC");
		return -1;
	}

	return 0;
}

static void display_cleanup(struct display *d)
{
	/* Put back whatever was on the CRTC. Without this the panel keeps
	 * scanning out a buffer that is about to be freed, which on this SoC
	 * shows up as a frozen or garbled screen rather than an error.
	 */

	if (d->saved_fb_id != 0 && d->saved_valid)
		set_crtc(d, d->saved_fb_id, &d->saved_mode, 1);

	if (d->map)
		munmap(d->map, d->size);

	if (d->fb_id) {
		uint32_t fb_id = d->fb_id;

		ioctl(d->fd, DRM_IOCTL_MODE_RMFB, &fb_id);
	}

	if (d->handle) {
		struct drm_mode_destroy_dumb destroy;

		memset(&destroy, 0, sizeof(destroy));
		destroy.handle = d->handle;
		ioctl(d->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}

	if (d->fd >= 0)
		close(d->fd);
}

/****************************************************************************
 * Blit
 ****************************************************************************/

/*
 * Nearest-neighbour scale from the shared buffer to the dumb buffer.
 *
 * The remote draws at half the panel's resolution because two full-size
 * ARGB8888 frames do not fit in the 4MB carveout, so some scaling is always
 * needed. With the panel at 1080x1920 and the remote at 540x960 this is an exact
 * 2x, but the ratio is computed rather than assumed so a different mode still
 * produces a picture instead of a crash.
 *
 * Fixed point, 16 fractional bits: at these sizes the step fits comfortably and
 * there is no rounding drift across a row.
 */
static void blit_scaled(struct display *d, const volatile uint32_t *src,
			uint32_t src_w, uint32_t src_h, uint32_t src_stride)
{
	uint32_t x_step = (src_w << 16) / d->width;
	uint32_t y_step = (src_h << 16) / d->height;
	uint32_t src_words = src_stride / 4;
	uint32_t y;

	for (y = 0; y < d->height; y++) {
		const volatile uint32_t *srow =
			src + ((y * y_step) >> 16) * src_words;
		uint32_t *drow = (uint32_t *)(d->map + (size_t)y * d->pitch);
		uint32_t xacc = 0;
		uint32_t x;

		for (x = 0; x < d->width; x++) {
			drow[x] = srow[xacc >> 16];
			xacc += x_step;
		}
	}
}

/****************************************************************************
 * Main
 ****************************************************************************/

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d card] [-r ms] [-n frames] [-p] [-q]\n"
		"  -d card    DRM device (default /dev/dri/card0)\n"
		"  -r ms      also request a frame from the remote every ms\n"
		"  -n frames  stop after this many frames (default: run until"
		" interrupted)\n"
		"  -p         passive: poll the control block, never open"
		" /dev/rpmsg0\n"
		"  -q         one line per frame instead of per-frame detail\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *card = "/dev/dri/card0";
	const char *rpmsg_dev = "/dev/rpmsg0";
	struct display d;
	volatile struct amp_shm_ctrl *ctrl;
	volatile uint8_t *shm;
	int memfd;
	int rpfd = -1;
	int request_ms = 0;
	long limit = 0;
	bool passive = false;
	bool quiet = false;
	long shown = 0;
	uint32_t last_seq;
	int opt;
	int ret = EXIT_FAILURE;

	while ((opt = getopt(argc, argv, "d:r:n:pqh")) != -1) {
		switch (opt) {
		case 'd':
			card = optarg;
			break;
		case 'r':
			request_ms = atoi(optarg);
			break;
		case 'n':
			limit = atol(optarg);
			break;
		case 'p':
			passive = true;
			break;
		case 'q':
			quiet = true;
			break;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* The carveout is declared no-map, so it is not in the kernel's linear
	 * map and /dev/mem is the way to reach it.
	 */

	memfd = open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) {
		perror("open /dev/mem");
		return EXIT_FAILURE;
	}

	shm = mmap(NULL, AMP_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   memfd, AMP_SHM_BASE);
	close(memfd);
	if (shm == MAP_FAILED) {
		perror("mmap shared area");
		return EXIT_FAILURE;
	}

	ctrl = (volatile struct amp_shm_ctrl *)(shm + AMP_SHM_HDR_OFFSET);

	if (ctrl->magic != AMP_SHM_MAGIC) {
		fprintf(stderr,
			"magic is 0x%08x, expected 0x%08x - the remote has not"
			" initialised the area (is NuttX running?)\n",
			ctrl->magic, AMP_SHM_MAGIC);
		return EXIT_FAILURE;
	}

	printf("remote frame source: %ux%u, %u bpp, stride %u, %u buffers\n",
	       ctrl->width, ctrl->height, ctrl->bpp, ctrl->stride,
	       ctrl->nbuffers);

	memset(&d, 0, sizeof(d));
	d.fd = open(card, O_RDWR | O_CLOEXEC);
	if (d.fd < 0) {
		perror(card);
		return EXIT_FAILURE;
	}

	/* The first client to open the device becomes master, so this only fails
	 * if something else already is - almost always a running compositor.
	 * Saying so beats letting SETCRTC fail with EACCES later.
	 */

	if (ioctl(d.fd, DRM_IOCTL_SET_MASTER, 0) < 0) {
		fprintf(stderr,
			"cannot become DRM master (%s) - something else holds"
			" it. Check /sys/kernel/debug/dri/0/clients for the"
			" row marked master, then stop that session:\n"
			"    systemctl isolate multi-user.target\n",
			strerror(errno));
		close(d.fd);
		return EXIT_FAILURE;
	}

	if (pick_output(&d) < 0)
		goto out;

	printf("output: connector %u, crtc %u, mode %ux%u@%u \"%.*s\"\n",
	       d.connector_id, d.crtc_id, d.width, d.height, d.mode.vrefresh,
	       (int)sizeof(d.mode.name), d.mode.name);

	if (save_crtc(&d) < 0)
		goto out;

	if (create_dumb_fb(&d) < 0)
		goto out;

	if (set_crtc(&d, d.fb_id, &d.mode, 1) < 0)
		goto out;

	if (!passive) {
		rpfd = open(rpmsg_dev, O_RDWR);
		if (rpfd < 0)
			fprintf(stderr,
				"%s: %s - falling back to polling the control"
				" block\n", rpmsg_dev, strerror(errno));
	}

	/* Show whatever is already there, so a still frame drawn before this
	 * program started is not invisible until the next update.
	 */

	last_seq = ctrl->frame_seq;
	if (last_seq != 0) {
		blit_scaled(&d,
			    (const volatile uint32_t *)
				(shm + ctrl->bufoffset[ctrl->ready_index]),
			    ctrl->width, ctrl->height, ctrl->stride);
		printf("frame %u (buf%u) on screen\n", last_seq,
		       ctrl->ready_index);
	}

	while (!g_stop && (limit == 0 || shown < limit)) {
		uint32_t seq;
		uint32_t index;
		uint32_t remote_sum;
		uint32_t local_sum;

		if (request_ms > 0 && rpfd >= 0) {
			struct amp_shm_msg req = {
				.cmd = AMP_SHM_CMD_RENDER,
			};

			if (write(rpfd, &req, sizeof(req)) != sizeof(req))
				perror("write render request");
		}

		if (rpfd >= 0) {
			struct pollfd pfd = { .fd = rpfd, .events = POLLIN };
			struct amp_shm_msg msg;
			int timeout = request_ms > 0 ? request_ms : 1000;
			int n;

			n = poll(&pfd, 1, timeout);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				perror("poll");
				goto out;
			}

			if (n == 0)
				continue;

			if (read(rpfd, &msg, sizeof(msg)) != sizeof(msg))
				continue;

			if (msg.cmd != AMP_SHM_CMD_READY)
				continue;

			seq = msg.seq;
			index = msg.index;
			remote_sum = msg.sum;
		} else {
			/* No rpmsg: watch the control block. The remote updates
			 * frame_seq last, after the pixels and the checksum, so
			 * seeing a new sequence means the rest is already there.
			 */

			seq = ctrl->frame_seq;
			if (seq == last_seq) {
				usleep(request_ms > 0 ?
				       request_ms * 1000 : 16000);
				continue;
			}

			index = ctrl->ready_index;
			remote_sum = ctrl->ready_sum;
		}

		if (index >= ctrl->nbuffers) {
			fprintf(stderr, "frame %u: bad buffer index %u\n",
				seq, index);
			continue;
		}

		{
			const volatile uint32_t *src =
				(const volatile uint32_t *)
					(shm + ctrl->bufoffset[index]);

			/* Checksum before scaling, so a mismatch points at the
			 * transport rather than at this program's arithmetic.
			 * The remote recomputes the sum on every publish, which
			 * makes this a real end-to-end check of the bytes rather
			 * than a comparison of two cached numbers.
			 */

			local_sum = sum_buffer(src, ctrl->bufsize);
			blit_scaled(&d, src, ctrl->width, ctrl->height,
				    ctrl->stride);
		}

		last_seq = seq;
		shown++;

		if (!quiet || local_sum != remote_sum)
			printf("frame %u buf%u: remote 0x%08x, local 0x%08x"
			       " -> %s\n", seq, index, remote_sum, local_sum,
			       local_sum == remote_sum ? "MATCH" : "MISMATCH");

		if (rpfd >= 0) {
			struct amp_shm_msg ack = {
				.cmd = AMP_SHM_CMD_ACK,
				.seq = seq,
				.index = index,
				.sum = local_sum,
			};

			if (write(rpfd, &ack, sizeof(ack)) != sizeof(ack))
				perror("write ack");
		}
	}

	printf("%ld frame(s) displayed\n", shown);
	ret = EXIT_SUCCESS;

out:
	if (rpfd >= 0)
		close(rpfd);

	display_cleanup(&d);
	return ret;
}
