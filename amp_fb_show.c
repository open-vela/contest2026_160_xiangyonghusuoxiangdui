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
 *   ./amp_fb_show -t           also forward touch events to the remote
 *   ./amp_fb_show -t -T /dev/input/event3
 *                              forward touch from a specific device instead of
 *                              auto-detecting one
 *
 * Touch runs in the same program and over the same rpmsg endpoint as the frame
 * signalling, for two reasons. The mechanical one: rpmsg_char binds one channel
 * per announced name and lets a single process open it, so a second channel
 * would need some out-of-band way to tell the two apart, which is no more
 * robust than a cmd field. The substantive one: this is the only component that
 * knows both the panel geometry and the touch controller's range, so the
 * coordinate mapping and its inverse are derived from the same numbers here
 * instead of living in two programs that can drift apart.
 *
 * The remote cannot take the touch hardware itself: the controller is on i2c5
 * with its interrupt on a GPIO bank 3 pin, that bank has one interrupt line for
 * all of its pins, and the Type-C power delivery controller's interrupt is on
 * the same bank.
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
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <drm/drm.h>
#include <linux/input.h>

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
#define AMP_SHM_CMD_TOUCH  4

#define AMP_TOUCH_DOWN     0
#define AMP_TOUCH_MOVE     1
#define AMP_TOUCH_UP       2

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

/* Same 16 bytes as amp_shm_msg, read as a touch event when cmd says so. Keeping
 * the sizes equal means the receiver reads one fixed-size message and then looks
 * at cmd, rather than needing the length before the read.
 */

struct amp_touch_msg {
	uint32_t cmd;
	uint32_t seq;
	uint16_t x;
	uint16_t y;
	uint8_t  id;
	uint8_t  state;
	uint16_t pressure;
	uint32_t reserved;
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

static uint64_t now_ms(void)
{
	struct timespec tv;

	clock_gettime(CLOCK_MONOTONIC, &tv);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_nsec / 1000000;
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
 * Touch
 ****************************************************************************/

/* Contacts tracked at once. The panel's controller is configured for five in
 * the device tree; the extra slots cost nothing and keep a controller that
 * reports more from writing past the array.
 */

#define AMP_TOUCH_SLOTS 10

/* One tracked contact.
 *
 * "present" is what the device last said; "reported" is what the remote has been
 * told. Keeping the two apart is what makes the decoding idempotent: this board's
 * driver repeats a contact's tracking id and position in every report while a
 * finger rests, and deriving "down" from the arrival of a tracking id turns a
 * single tap into hundreds of down events with no release. Down is a transition
 * from not-present to present, which can only happen once per contact.
 */

struct touch_slot {
	int32_t x;                   /* last position the device gave      */
	int32_t y;
	int32_t sent_x;              /* last position the remote was told  */
	int32_t sent_y;
	bool present;                /* device says this contact exists    */
	bool reported;               /* a down was sent and no up yet      */
	bool seen;                   /* appeared in the frame being built  */
	uint32_t moves;
	uint16_t down_x;
	uint16_t down_y;
};

/* Fields of the contact currently being assembled, for the older protocol where
 * contacts are separated by SYN_MT_REPORT rather than addressed by slot.
 */

struct touch_group {
	int32_t id;
	int32_t x;
	int32_t y;
	bool has_id;
	bool has_pos;
};

struct touch_src {
	int fd;
	char name[80];
	bool mt;                     /* has ABS_MT_* axes at all           */
	bool proto_a;                /* contacts separated by SYN_MT_REPORT */
	int cur_slot;
	int32_t min_x;
	int32_t max_x;
	int32_t min_y;
	int32_t max_y;
	struct touch_slot slot[AMP_TOUCH_SLOTS];
	struct touch_group group;
	uint32_t seq;
};

static bool bit_set(const unsigned long *bits, unsigned int bit)
{
	return (bits[bit / (8 * sizeof(long))] >>
		(bit % (8 * sizeof(long)))) & 1UL;
}

/*
 * Decide whether a device is the touch panel, and learn its coordinate range
 * while we are at it.
 *
 * Asking the device rather than being told which node it is matters here: the
 * board's device tree declares two different touch controllers on the same bus
 * sharing the same interrupt and reset pins, so which one probes - and therefore
 * what the event node number is - is not knowable from the source tree.
 */
static int touch_probe(struct touch_src *ts, const char *path, bool verbose)
{
	unsigned long abs_bits[(ABS_CNT + 8 * sizeof(long) - 1) /
			       (8 * sizeof(long))];
	unsigned long key_bits[(KEY_CNT + 8 * sizeof(long) - 1) /
			       (8 * sizeof(long))];
	struct input_absinfo absinfo;
	int fd;
	int axis_x;
	int axis_y;

	fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -1;

	memset(abs_bits, 0, sizeof(abs_bits));
	memset(key_bits, 0, sizeof(key_bits));
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

	if (bit_set(abs_bits, ABS_MT_POSITION_X) &&
	    bit_set(abs_bits, ABS_MT_POSITION_Y)) {
		ts->mt = true;
		axis_x = ABS_MT_POSITION_X;
		axis_y = ABS_MT_POSITION_Y;
	} else if (bit_set(abs_bits, ABS_X) && bit_set(abs_bits, ABS_Y) &&
		   bit_set(key_bits, BTN_TOUCH)) {
		ts->mt = false;
		axis_x = ABS_X;
		axis_y = ABS_Y;
	} else {
		close(fd);
		return -1;
	}

	memset(&absinfo, 0, sizeof(absinfo));
	if (ioctl(fd, EVIOCGABS(axis_x), &absinfo) < 0) {
		close(fd);
		return -1;
	}

	ts->min_x = absinfo.minimum;
	ts->max_x = absinfo.maximum;

	memset(&absinfo, 0, sizeof(absinfo));
	if (ioctl(fd, EVIOCGABS(axis_y), &absinfo) < 0) {
		close(fd);
		return -1;
	}

	ts->min_y = absinfo.minimum;
	ts->max_y = absinfo.maximum;

	/* A range of zero would make the scaling below divide by zero, and it
	 * also means the driver never filled the axis in - not a usable device.
	 */

	if (ts->max_x <= ts->min_x || ts->max_y <= ts->min_y) {
		if (verbose)
			fprintf(stderr,
				"%s: unusable axis range x[%d,%d] y[%d,%d]\n",
				path, ts->min_x, ts->max_x, ts->min_y,
				ts->max_y);
		close(fd);
		return -1;
	}

	ts->name[0] = '\0';
	ioctl(fd, EVIOCGNAME(sizeof(ts->name) - 1), ts->name);

	ts->fd = fd;
	ts->cur_slot = 0;
	return 0;
}

static int touch_open(struct touch_src *ts, const char *want)
{
	struct dirent *ent;
	DIR *dir;
	int ret = -1;

	memset(ts, 0, sizeof(*ts));
	ts->fd = -1;

	if (want != NULL) {
		if (touch_probe(ts, want, true) < 0) {
			fprintf(stderr,
				"%s: not a usable touch device (needs absolute"
				" axes with a real range)\n", want);
			return -1;
		}

		printf("touch: %s \"%s\", %s, x[%d,%d] y[%d,%d]\n",
		       want, ts->name, ts->mt ? "multitouch" : "single touch",
		       ts->min_x, ts->max_x, ts->min_y, ts->max_y);
		return 0;
	}

	dir = opendir("/dev/input");
	if (dir == NULL) {
		perror("opendir /dev/input");
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		char path[sizeof("/dev/input/") + sizeof(ent->d_name)];

		if (strncmp(ent->d_name, "event", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

		if (touch_probe(ts, path, false) < 0)
			continue;

		printf("touch: %s \"%s\", %s, x[%d,%d] y[%d,%d]\n",
		       path, ts->name,
		       ts->mt ? "multitouch" : "single touch",
		       ts->min_x, ts->max_x, ts->min_y, ts->max_y);
		ret = 0;
		break;
	}

	closedir(dir);

	if (ret < 0)
		fprintf(stderr,
			"no touch device found under /dev/input - pass one with"
			" -T, or check /proc/bus/input/devices\n");

	return ret;
}

/*
 * Raw controller coordinates to the remote's framebuffer coordinates.
 *
 * This is the inverse of the scaling the blit does, and it lives in the same
 * program for that reason: the remote is fixed at half the panel's resolution,
 * so a touch at the middle of the glass has to land at the middle of a 540x960
 * buffer, and the two conversions have to agree. The remote is told framebuffer
 * coordinates so it never has to know a panel or a controller exists.
 */
static void touch_scale(const struct touch_src *ts, int32_t rx, int32_t ry,
			uint32_t fb_w, uint32_t fb_h,
			uint16_t *x, uint16_t *y)
{
	int64_t sx = (int64_t)(rx - ts->min_x) * fb_w /
		     (ts->max_x - ts->min_x + 1);
	int64_t sy = (int64_t)(ry - ts->min_y) * fb_h /
		     (ts->max_y - ts->min_y + 1);

	if (sx < 0)
		sx = 0;
	if (sy < 0)
		sy = 0;
	if (sx > (int64_t)fb_w - 1)
		sx = fb_w - 1;
	if (sy > (int64_t)fb_h - 1)
		sy = fb_h - 1;

	*x = (uint16_t)sx;
	*y = (uint16_t)sy;
}

static void touch_send(struct touch_src *ts, int rpfd, uint8_t id,
		       uint8_t state, uint16_t x, uint16_t y)
{
	struct amp_touch_msg msg;

	memset(&msg, 0, sizeof(msg));
	msg.cmd = AMP_SHM_CMD_TOUCH;
	msg.seq = ++ts->seq;
	msg.x = x;
	msg.y = y;
	msg.id = id;
	msg.state = state;

	if (write(rpfd, &msg, sizeof(msg)) != sizeof(msg))
		perror("write touch event");
}

/*
 * Commit the contact being assembled, for the older protocol.
 *
 * The tracking id is used as the contact's identity when the device supplies one
 * - this board's driver does - so a finger keeps the same id across reports even
 * though the protocol has no slots. Without an id there is nothing better than
 * the position within the report, which is only stable while the number of
 * contacts does not change.
 */
static void touch_group_commit(struct touch_src *ts)
{
	struct touch_group *g = &ts->group;
	struct touch_slot *s;
	int key;
	int i;

	if (!g->has_pos) {
		/* An empty group is how "no contacts" is spelled. Nothing to
		 * commit; the frame-end sweep turns it into a release.
		 */

		memset(g, 0, sizeof(*g));
		return;
	}

	if (g->has_id && g->id >= 0 && g->id < AMP_TOUCH_SLOTS) {
		key = g->id;
	} else {
		key = -1;
		for (i = 0; i < AMP_TOUCH_SLOTS; i++) {
			if (!ts->slot[i].seen) {
				key = i;
				break;
			}
		}

		if (key < 0) {
			memset(g, 0, sizeof(*g));
			return;
		}
	}

	s = &ts->slot[key];
	s->x = g->x;
	s->y = g->y;
	s->present = true;
	s->seen = true;

	memset(g, 0, sizeof(*g));
}

/*
 * Turn one completed report into messages.
 *
 * Everything is derived from a transition rather than from an event arriving:
 * not-present to present is a down, present with a new position is a move,
 * present to not-present is an up. That is what makes a driver which repeats the
 * full state every report - as this board's does - produce one down per tap
 * instead of one per report, and it is also why a resting finger generates no
 * traffic at all.
 *
 * Order is down, then move, then up. A consumer that sees an up before the down
 * of the same contact cannot reconstruct a tap, and the release carries the last
 * known position for the same reason: "a contact ended" without a location
 * cannot be turned into a click.
 *
 * Only the start and end of a contact are logged, with the move count folded
 * into the release line. A line per move would push a hundred lines a second
 * onto a serial console that the remote also logs to, drowning out everything
 * else at the exact moment there is something to watch.
 */
static void touch_flush(struct touch_src *ts, int rpfd, uint32_t fb_w,
			uint32_t fb_h, bool verbose)
{
	int i;

	/* In the older protocol a contact exists only if it was listed in this
	 * report, so anything not seen has been released. The slot protocol is
	 * the opposite - state persists until a tracking id is cleared - so the
	 * sweep must not run there.
	 */

	if (ts->proto_a) {
		for (i = 0; i < AMP_TOUCH_SLOTS; i++) {
			if (!ts->slot[i].seen)
				ts->slot[i].present = false;
		}
	}

	for (i = 0; i < AMP_TOUCH_SLOTS; i++) {
		struct touch_slot *s = &ts->slot[i];
		uint16_t x;
		uint16_t y;

		s->seen = false;

		if (!s->present && !s->reported)
			continue;

		touch_scale(ts, s->x, s->y, fb_w, fb_h, &x, &y);

		if (s->present && !s->reported) {
			touch_send(ts, rpfd, i, AMP_TOUCH_DOWN, x, y);
			s->reported = true;
			s->sent_x = s->x;
			s->sent_y = s->y;
			s->moves = 0;
			s->down_x = x;
			s->down_y = y;

			if (verbose)
				printf("touch id %d down (%u,%u)\n", i, x, y);
		} else if (s->present) {
			if (s->x == s->sent_x && s->y == s->sent_y)
				continue;

			touch_send(ts, rpfd, i, AMP_TOUCH_MOVE, x, y);
			s->sent_x = s->x;
			s->sent_y = s->y;
			s->moves++;
		} else {
			touch_send(ts, rpfd, i, AMP_TOUCH_UP, x, y);
			s->reported = false;

			if (verbose)
				printf("touch id %d up (%u,%u) from (%u,%u)"
				       " after %u move(s)\n", i, x, y,
				       s->down_x, s->down_y, s->moves);
		}
	}
}

/*
 * Drain the event device.
 *
 * Both multitouch protocols are decoded, and which one is in use is discovered at
 * runtime rather than from the device's capability bits. That is not caution for
 * its own sake: this board's driver calls input_mt_init_slots() unconditionally,
 * so ABS_MT_SLOT appears in the capability bitmap even when the driver is
 * reporting with SYN_MT_REPORT. The usual "advertises ABS_MT_SLOT, therefore uses
 * slots" inference is simply wrong here. Seeing a SYN_MT_REPORT is proof; the
 * bitmap is not.
 */
static void touch_drain(struct touch_src *ts, int rpfd, uint32_t fb_w,
			uint32_t fb_h, bool verbose)
{
	struct input_event ev[64];
	ssize_t n;
	size_t i;

	while ((n = read(ts->fd, ev, sizeof(ev))) > 0) {
		for (i = 0; i < (size_t)n / sizeof(ev[0]); i++) {
			struct touch_slot *s = &ts->slot[ts->cur_slot];

			switch (ev[i].type) {
			case EV_ABS:
				switch (ev[i].code) {
				case ABS_MT_SLOT:
					if (ev[i].value >= 0 &&
					    ev[i].value < AMP_TOUCH_SLOTS)
						ts->cur_slot = ev[i].value;
					break;

				case ABS_MT_TRACKING_ID:
					if (ts->proto_a) {
						ts->group.id = ev[i].value;
						ts->group.has_id =
							ev[i].value >= 0;
						break;
					}

					if (ev[i].value >= 0) {
						s->present = true;
						s->seen = true;
					} else {
						s->present = false;
					}
					break;

				case ABS_MT_POSITION_X:
					if (ts->proto_a) {
						ts->group.x = ev[i].value;
						ts->group.has_pos = true;
					} else {
						s->x = ev[i].value;
						s->seen = true;
					}
					break;

				case ABS_MT_POSITION_Y:
					if (ts->proto_a) {
						ts->group.y = ev[i].value;
						ts->group.has_pos = true;
					} else {
						s->y = ev[i].value;
						s->seen = true;
					}
					break;

				case ABS_X:
					if (!ts->mt)
						ts->slot[0].x = ev[i].value;
					break;

				case ABS_Y:
					if (!ts->mt)
						ts->slot[0].y = ev[i].value;
					break;

				default:
					break;
				}
				break;

			case EV_KEY:

				/* BTN_TOUCH is only load-bearing on a device
				 * with no multitouch axes. The older protocol
				 * sends it too, but there the presence of a
				 * contact is already decided by whether it was
				 * listed in the report, and trusting both would
				 * make a two-finger release ambiguous.
				 */

				if (ev[i].code != BTN_TOUCH || ts->mt)
					break;

				ts->slot[0].present = ev[i].value != 0;
				break;

			case EV_SYN:
				if (ev[i].code == SYN_MT_REPORT) {
					ts->proto_a = true;
					touch_group_commit(ts);
				} else if (ev[i].code == SYN_REPORT) {
					/* The current slot is deliberately not
					 * reset here. In the slot protocol the
					 * kernel only emits ABS_MT_SLOT when it
					 * changes, so a contact on slot 1 keeps
					 * reporting without repeating the slot
					 * number; resetting to 0 each report
					 * would attribute those positions to the
					 * wrong finger.
					 */

					touch_flush(ts, rpfd, fb_w, fb_h,
						    verbose);
				}
				break;

			default:
				break;
			}
		}
	}

	if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		perror("read touch device");
}

/****************************************************************************
 * Main
 ****************************************************************************/

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d card] [-r ms] [-n frames] [-p] [-q] [-t]"
		" [-T dev]\n"
		"  -d card    DRM device (default /dev/dri/card0)\n"
		"  -r ms      also request a frame from the remote every ms\n"
		"  -n frames  stop after this many frames (default: run until"
		" interrupted)\n"
		"  -p         passive: poll the control block, never open"
		" /dev/rpmsg0\n"
		"  -q         one line per frame instead of per-frame detail\n"
		"  -t         forward touch events to the remote\n"
		"  -T dev     touch event device (default: auto-detect)\n",
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
	bool want_touch = false;
	const char *touch_dev = NULL;
	struct touch_src ts;
	long shown = 0;
	uint32_t last_seq;
	uint64_t next_request;
	int opt;
	int ret = EXIT_FAILURE;

	memset(&ts, 0, sizeof(ts));
	ts.fd = -1;

	while ((opt = getopt(argc, argv, "d:r:n:pqtT:h")) != -1) {
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
		case 't':
			want_touch = true;
			break;
		case 'T':
			touch_dev = optarg;
			want_touch = true;
			break;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* Touch needs the rpmsg channel; there is nowhere else to send it. */

	if (want_touch && passive) {
		fprintf(stderr,
			"-t needs the rpmsg channel, so it cannot be combined"
			" with -p\n");
		return EXIT_FAILURE;
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

	if (want_touch) {
		if (rpfd < 0) {
			fprintf(stderr,
				"touch has nowhere to go without %s - is"
				" rpmsg_char loaded?\n", rpmsg_dev);
			goto out;
		}

		if (touch_open(&ts, touch_dev) < 0)
			goto out;
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

	next_request = now_ms();

	while (!g_stop && (limit == 0 || shown < limit)) {
		uint32_t seq;
		uint32_t index;
		uint32_t remote_sum;
		uint32_t local_sum;

		if (rpfd >= 0) {
			struct pollfd pfd[2];
			struct amp_shm_msg msg;
			int rp_i;
			int ts_i = -1;
			int nfds = 0;
			int timeout;
			uint64_t t;
			int n;

			/* Render requests are paced by the clock, not by loop
			 * iterations. The loop now also wakes for touch events,
			 * and asking for a frame on every wakeup would flood the
			 * remote the moment a finger moves.
			 */

			if (request_ms > 0) {
				t = now_ms();

				if (t >= next_request) {
					struct amp_shm_msg req = {
						.cmd = AMP_SHM_CMD_RENDER,
					};

					if (write(rpfd, &req, sizeof(req)) !=
					    sizeof(req))
						perror("write render request");

					next_request = t + request_ms;
				}

				t = now_ms();
				timeout = next_request > t ?
					  (int)(next_request - t) : 0;
			} else {
				timeout = 1000;
			}

			pfd[nfds].fd = rpfd;
			pfd[nfds].events = POLLIN;
			rp_i = nfds++;

			if (ts.fd >= 0) {
				pfd[nfds].fd = ts.fd;
				pfd[nfds].events = POLLIN;
				ts_i = nfds++;
			}

			n = poll(pfd, nfds, timeout);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				perror("poll");
				goto out;
			}

			if (ts_i >= 0 && (pfd[ts_i].revents & POLLIN))
				touch_drain(&ts, rpfd, ctrl->width,
					    ctrl->height, !quiet);

			if (!(pfd[rp_i].revents & POLLIN))
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
	if (ts.fd >= 0)
		close(ts.fd);

	if (rpfd >= 0)
		close(rpfd);

	display_cleanup(&d);
	return ret;
}
