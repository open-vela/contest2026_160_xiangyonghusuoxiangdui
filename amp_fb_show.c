// SPDX-License-Identifier: Apache-2.0
/*
 * Light the panel for NuttX, and forward touch to it.
 *
 * This program used to scan out frames NuttX had drawn into shared memory. It no
 * longer carries pixels at all: NuttX programs the VOP's Esmart3 window itself
 * and the hardware scans out of NuttX's own RAM, so frames never come through
 * here. What is left are the two things NuttX cannot do for itself.
 *
 * Lighting the panel. Bringing up 1080x1920 MIPI-DSI means a D-PHY PLL, the MIPI
 * host controller, the panel's initialisation sequence over DCS, a video port
 * timing generator and four clock trees - about thirteen thousand lines of Linux.
 * None of it is per-frame state; it is set once at modeset and stays set. So this
 * program does a modeset and then stays alive holding it, because exiting would
 * restore the previous CRTC configuration and take the panel down with it.
 *
 * The dumb buffer it creates is never drawn into after the initial clear. It
 * exists because a CRTC cannot be given a mode without a framebuffer on its
 * primary plane, and vp3's primary plane is Cluster3. NuttX's window, Esmart3,
 * sits above it on the same video port, which the device tree reserved for that
 * purpose. So the black buffer here is scaffolding for the modeset, not a
 * picture.
 *
 * Forwarding touch. The controller is on i2c5 with its interrupt on a GPIO
 * bank 3 pin; that bank has one interrupt line for all of its pins, and the
 * Type-C power delivery controller's interrupt is on the same bank. Claiming it
 * from the other core would break charging. This side also happens to be the
 * side that knows both the panel geometry and the controller's range, so the
 * coordinate mapping and its inverse stay derived from the same numbers rather
 * than living in two programs that can drift apart.
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
 *   ./amp_fb_show              light the panel, forward touch, stay alive
 *   ./amp_fb_show -T /dev/input/event3
 *                              use a specific touch device instead of
 *                              auto-detecting one
 *   ./amp_fb_show -q           no per-contact logging
 *
 * It has to keep running. Ctrl-C or SIGTERM restores the CRTC and releases DRM
 * master on the way out, which is what a well-behaved DRM client does, and also
 * what takes the panel down - so backgrounding it is the normal way to use it.
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
/* Bumped with the layout - see the comment in evb7_amp_shm.h for why the version
 * field is not enough on its own.
 */

#define AMP_SHM_MAGIC      0x31424641u    /* "AFB1" */
#define AMP_SHM_VERSION    2

/* The first page is skipped because something outside this project writes three
 * words at its start - see the comment in evb7_amp_shm.h.
 */

#define AMP_SHM_HDR_OFFSET 4096

/* 1, 2 and 3 were the frame protocol - render, ready, ack. They are left unused
 * rather than recycled so that a mismatched pair of binaries reports an unknown
 * command instead of misreading one message as another.
 */

#define AMP_SHM_CMD_TOUCH  4
#define AMP_SHM_CMD_HELLO  5

#define AMP_TOUCH_DOWN     0
#define AMP_TOUCH_MOVE     1
#define AMP_TOUCH_UP       2

/* Geometry only. The remote publishes it so that the coordinate scaling below
 * follows whatever resolution its framebuffer actually is.
 */

struct amp_shm_ctrl {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t bpp;
} __attribute__((packed));

/* Every message on this endpoint is 16 bytes and starts with cmd. This is the
 * header view, used for the hello; the touch structure is the same bytes in
 * full.
 */

struct amp_shm_hdr {
	uint32_t cmd;
	uint32_t pad[3];
} __attribute__((packed));

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
		"usage: %s [-d card] [-T dev] [-q]\n"
		"  -d card    DRM device (default /dev/dri/card0)\n"
		"  -T dev     touch event device (default: auto-detect)\n"
		"  -q         do not log each contact\n"
		"  -t         accepted and ignored (touch is always on now)\n"
		"\n"
		"Lights the panel and forwards touch to the AMP core. Keeps\n"
		"running: exiting restores the previous CRTC configuration,\n"
		"which takes the panel down.\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *card = "/dev/dri/card0";
	const char *rpmsg_dev = "/dev/rpmsg0";
	struct display d;
	volatile struct amp_shm_ctrl *ctrl;
	volatile uint8_t *shm;
	struct amp_shm_hdr hello = { .cmd = AMP_SHM_CMD_HELLO };
	int memfd;
	int rpfd = -1;
	bool quiet = false;
	const char *touch_dev = NULL;
	struct touch_src ts;
	uint32_t fb_w;
	uint32_t fb_h;
	int opt;
	int ret = EXIT_FAILURE;

	memset(&ts, 0, sizeof(ts));
	ts.fd = -1;

	while ((opt = getopt(argc, argv, "d:T:qth")) != -1) {
		switch (opt) {
		case 'd':
			card = optarg;
			break;
		case 'T':
			touch_dev = optarg;
			break;
		case 'q':
			quiet = true;
			break;
		case 't':

			/* Accepted and ignored. It used to enable touch
			 * forwarding, which is now the whole job. Rejecting it
			 * would make the program refuse to start for anyone
			 * whose habit or script still passes it - and refusing
			 * to start means no modeset, so the panel stays dark:
			 * an expensive way to enforce a spelling.
			 */

			break;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	/* Line buffering, because this program now runs indefinitely.
	 *
	 * stdio picks full buffering when stdout is not a terminal, and nothing
	 * here flushes: the loop at the end never returns, so every startup
	 * message would sit in a 4KB buffer until the process was killed. The
	 * previous version got away with it by printing a frame report every
	 * couple of seconds, which eventually filled the buffer. This one prints
	 * nothing after setup, so with output redirected or piped it would look
	 * exactly like a program that had failed silently.
	 */

	setvbuf(stdout, NULL, _IOLBF, 0);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	/* The carveout is declared no-map, so it is not in the kernel's linear
	 * map and /dev/mem is the way to reach it. Only the control block is
	 * read - the frame buffers that used to follow it are gone.
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

	/* A version mismatch means the two sides disagree about the layout of
	 * everything after the magic, which is worth stopping for rather than
	 * reading plausible-looking geometry out of the wrong offsets.
	 */

	if (ctrl->version != AMP_SHM_VERSION) {
		fprintf(stderr,
			"remote speaks version %u, this program speaks %u -"
			" rebuild whichever is older\n",
			ctrl->version, AMP_SHM_VERSION);
		return EXIT_FAILURE;
	}

	fb_w = ctrl->width;
	fb_h = ctrl->height;

	printf("remote framebuffer: %ux%u, %u bpp (touch is scaled to this)\n",
	       fb_w, fb_h, ctrl->bpp);

	/* Open the channel touch will go out on, and say hello. The hello is no
	 * longer load-bearing - nothing is sent from the remote any more - but it
	 * puts one unambiguous line in both logs saying the channel came up,
	 * which is the cheapest way to tell "no touch because the channel is
	 * down" from "no touch because nobody is touching".
	 *
	 * Not fatal if it fails, and that is a correction rather than leniency.
	 * Lighting the panel and forwarding touch are independent jobs, and the
	 * first cut of this made the second a precondition for the first: without
	 * rpmsg_char loaded the program returned before the modeset, so a missing
	 * module took the display with it. The remote drives its own window, so
	 * something was still on screen - which made it look like touch had
	 * broken on its own.
	 */

	rpfd = open(rpmsg_dev, O_RDWR);
	if (rpfd < 0)
		fprintf(stderr,
			"%s: %s - touch will NOT be forwarded. Load rpmsg_char"
			" and restart this program.\n",
			rpmsg_dev, strerror(errno));
	else if (write(rpfd, &hello, sizeof(hello)) != sizeof(hello))
		perror("write hello");

	memset(&d, 0, sizeof(d));
	d.fd = open(card, O_RDWR | O_CLOEXEC);
	if (d.fd < 0) {
		perror(card);
		close(rpfd);
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
		close(rpfd);
		return EXIT_FAILURE;
	}

	if (pick_output(&d) < 0)
		goto out;

	printf("output: connector %u, crtc %u, mode %ux%u@%u \"%.*s\"\n",
	       d.connector_id, d.crtc_id, d.width, d.height, d.mode.vrefresh,
	       (int)sizeof(d.mode.name), d.mode.name);

	if (save_crtc(&d) < 0)
		goto out;

	/* Black, and never drawn into again. This buffer is what lets the CRTC
	 * take a mode; the remote's window sits above it.
	 */

	if (create_dumb_fb(&d) < 0)
		goto out;

	if (set_crtc(&d, d.fb_id, &d.mode, 1) < 0)
		goto out;

	/* Also not fatal. The panel is already up by this point, and holding it up
	 * is the job that cannot be done from anywhere else.
	 */

	if (rpfd >= 0 && touch_open(&ts, touch_dev) < 0)
		fprintf(stderr, "touch will NOT be forwarded\n");

	printf("panel is up, %s - Ctrl-C takes it down\n",
	       ts.fd >= 0 ? "forwarding touch" :
	       "touch NOT forwarded (see above)");

	/* Nothing to do but forward contacts. The remote drives its own window
	 * from here on, so there is no frame loop and no reason to wake up other
	 * than an input event - or, with no touch device, no reason to wake up at
	 * all beyond noticing a signal.
	 */

	while (!g_stop) {
		struct pollfd pfd;
		int n;

		if (ts.fd < 0) {
			pause();
			continue;
		}

		pfd.fd = ts.fd;
		pfd.events = POLLIN;
		pfd.revents = 0;

		n = poll(&pfd, 1, 1000);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			goto out;
		}

		if (n > 0 && (pfd.revents & POLLIN))
			touch_drain(&ts, rpfd, fb_w, fb_h, !quiet);
	}

	ret = EXIT_SUCCESS;

out:
	if (ts.fd >= 0)
		close(ts.fd);

	if (rpfd >= 0)
		close(rpfd);

	display_cleanup(&d);
	return ret;
}
