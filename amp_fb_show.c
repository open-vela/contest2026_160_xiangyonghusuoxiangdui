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

/* For sched_getcpu(), which the reports use to say which core the conversion
 * actually ran on. That turned out to matter: this SoC is four A76s and four
 * A55s minus the one given to the remote, the A55 is in-order and clocks well
 * below the A76, and a per-pixel cost that looks impossible on a fast core is
 * unremarkable on a slow one. Guessing which it was is how the last two
 * diagnoses went wrong.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
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
#include <linux/videodev2.h>
#include <linux/input.h>

#define AMP_SHM_BASE       0x31000000UL
#define AMP_SHM_SIZE       (4 * 1024 * 1024)
/* Bumped with the layout - see the comment in evb7_amp_shm.h for why the version
 * field is not enough on its own.
 */

#define AMP_SHM_MAGIC      0x31424641u    /* "AFB1" */
#define AMP_SHM_VERSION    4

/* Version 3 added the camera blocks. The magic is unchanged because the control
 * block below is not: same fields, same order, same offsets, so a binary built
 * against version 2 reads every field it knows about correctly. What it does not
 * do is consume the frames, which the version check catches.
 */

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
#define AMP_SHM_CMD_CAMERA 6
#define AMP_SHM_CMD_DETECT 7

#define AMP_TOUCH_DOWN     0
#define AMP_TOUCH_MOVE     1
#define AMP_TOUCH_UP       2

/* Camera area. Written here, read by the remote - the opposite direction to the
 * control block, which is why it is a separate block rather than extra fields:
 * one structure with a writer on each core is a bug waiting to happen, and
 * keeping them apart also left the control block layout untouched.
 *
 * The slots are 1MB and 1MB-aligned so the offsets are checkable by eye and so
 * changing capture resolution moves nothing. The carveout is 4MB and had two
 * pages in use, so the space costs nothing.
 */

#define AMP_SHM_HDR_SIZE     4096
#define AMP_CAM_DESC_OFFSET  (AMP_SHM_HDR_OFFSET + AMP_SHM_HDR_SIZE)
#define AMP_CAM_NBUFFERS     2
#define AMP_CAM_SLOT_SIZE    0x100000
#define AMP_CAM_BUF0_OFFSET  0x100000
#define AMP_CAM_BUF1_OFFSET  0x200000

#define AMP_CAM_DEF_WIDTH    512
#define AMP_CAM_DEF_HEIGHT   288
#define AMP_CAM_BPP          4         /* XRGB8888, what the remote's fb is */

#define AMP_CAM_MAGIC        0x314d4143u  /* "CAM1" */
#define AMP_CAM_VERSION      1

/* Detection results, in the page after the camera descriptor. Written here,
 * read by the remote.
 *
 * A third block rather than fields added to the camera descriptor, because that
 * one is written thirty times a second and this one about ten, and because one
 * writer per block is the rule that has kept this area debuggable.
 *
 * Shared memory rather than messages - unlike touch, which is also small -
 * because a set of boxes is a snapshot. A consumer needs every box from one
 * frame and none from another, and the count varies; messages here are a fixed
 * sixteen bytes, so a variable-length set would need its own framing on top. The
 * seqlock already used for frames gives the all-or-nothing read for free.
 */

#define AMP_DET_DESC_OFFSET  (AMP_CAM_DESC_OFFSET + 4096)
#define AMP_DET_DESC_SIZE    4096
#define AMP_DET_MAXBOX       64        /* == OBJ_NUMB_MAX_SIZE in postprocess */

#define AMP_DET_MAGIC        0x31544544u  /* "DET1" */
#define AMP_DET_VERSION      1

/* Coordinates are in the published camera frame's space, not the framebuffer's
 * and not the model's. This side knows the two ISP streams and the letterbox the
 * model needed, so it resolves all of that; the remote knows where on screen it
 * drew the frame, so it adds only that. Each side owns the transform it can see.
 */

struct amp_det_box {
	uint16_t x;
	uint16_t y;
	uint16_t w;
	uint16_t h;
	uint8_t  cls;                 /* COCO class id; 0 is person */
	uint8_t  score;               /* confidence, 0..100 */
	uint16_t reserved;
} __attribute__((packed));

struct amp_det_desc {
	uint32_t magic;
	uint32_t version;
	uint32_t seq;
	uint32_t count;
	uint32_t width;
	uint32_t height;
	uint32_t latency_us;          /* end-to-end time for this set */
	uint32_t reserved;
	struct amp_det_box box[AMP_DET_MAXBOX];
} __attribute__((packed));

struct amp_det_msg {
	uint32_t cmd;
	uint32_t seq;
	uint32_t count;               /* hint only; descriptor is truth */
	uint32_t reserved;
} __attribute__((packed));

/* seq and ready are the publication protocol and the order matters. The carveout
 * is mapped non-cacheable on the remote, which is what removes the need for cache
 * maintenance, but non-cacheable says nothing about ordering - so pixels are
 * written, then a barrier, then ready, then a barrier, then seq. The remote reads
 * seq, the frame, then seq again and retries if it moved.
 */

struct amp_cam_desc {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t bpp;
	uint32_t nbuffers;
	uint32_t bufsize;
	uint32_t bufoffset[AMP_CAM_NBUFFERS];
	uint32_t seq;
	uint32_t ready;
} __attribute__((packed));

struct amp_cam_msg {
	uint32_t cmd;
	uint32_t seq;
	uint32_t ready;
	uint32_t reserved;
} __attribute__((packed));

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

/* Every message out of this program goes through here.
 *
 * There are two writers now: the poll loop, which forwards touch and camera
 * notifications, and the detector thread. A sixteen-byte write() into the rpmsg
 * character device is very likely atomic, but "very likely" is not a property to
 * discover from a report of interleaved messages, and the lock costs nothing at
 * the rates involved - a few hundred contacts a second at the very most, thirty
 * frame notifications, ten doorbells.
 *
 * A file-scope lock rather than one threaded through touch_drain, touch_flush and
 * touch_send as a parameter: those three exist to decode input events, and giving
 * each of them a mutex argument would spread a concurrency concern across code
 * that has nothing else to do with it.
 */

static pthread_mutex_t g_rplock = PTHREAD_MUTEX_INITIALIZER;

static bool rpmsg_send(int fd, const void *msg, size_t len)
{
	ssize_t n;

	if (fd < 0)
		return false;

	pthread_mutex_lock(&g_rplock);
	n = write(fd, msg, len);
	pthread_mutex_unlock(&g_rplock);

	return n == (ssize_t)len;
}

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

#ifdef AMP_WITH_RKNN

/* Declared here and defined next to det_dump(), so the handler and the code that
 * acts on it are not separated by two thousand lines.
 */

static void on_dump_signal(int sig);

#endif

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

	if (!rpmsg_send(rpfd, &msg, sizeof(msg)))
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

/* Camera capture
 * ---------------------------------------------------------------------------
 * The ISP is asked for the size we want to publish, and then told what it is
 * actually going to give - S_FMT adjusts rather than fails. Whatever comes back
 * is resampled to the published size in the same pass that converts it, so there
 * is one code path instead of a good case and a clamped case.
 *
 * That matters here rather than being defensive habit. This sensor is 3864x2192
 * and the panel window is 512x288, which is a 7.5x reduction - right at the edge
 * of what the ISP's scaler will do. capture.c only clamps the requested size to
 * [32, input window] with no ratio check, so S_FMT succeeds either way and the
 * only way to know what happened is to read the format back.
 *
 * NV12 in, XRGB8888 out. NV12 because it is the format every path on this ISP
 * supports and the one every Rockchip camera pipeline uses, so it is the least
 * likely to find a driver corner; ISP v30 cannot produce XBGR32 at all, which
 * earlier versions could. The conversion is here rather than on the remote
 * because this side has seven idle A55s and the remote has one core with a 33ms
 * frame budget already fully spent - and because it means the bytes in shared
 * memory are exactly what the remote's framebuffer wants, so that side has no
 * format code at all.
 */

#define CAM_MAX_BUFFERS 4

struct cam_buffer {
	void *start;
	size_t length;
};

struct camera {
	const char *name;
	int fd;
	struct cam_buffer buf[CAM_MAX_BUFFERS];
	unsigned int nbuffers;

	/* Whether frames from this stream go into the carveout.
	 *
	 * There are two streams now. The ISP has independent main and self
	 * paths - the interrupt handler tracks them with separate completion
	 * bits, ISP3X_MI_MP_FRAME and ISP3X_MI_SP_FRAME, each with its own
	 * entry in irq_ends_mask - so one sensor can feed the display at one
	 * size and a detector at another, with the scaling done in hardware
	 * twice rather than in software once.
	 *
	 * Only the display stream publishes. The second one exists to answer
	 * whether the two can run at once at all, which is the last thing that
	 * could still invalidate the plan, so it does the least work that still
	 * exercises the hardware: dequeue, count, requeue.
	 */

	bool publish;

	/* V4L2's own frame counter, and the gaps in it.
	 *
	 * This is the measurement that matters for the dual-stream question.
	 * Frame rate alone cannot answer it: if the ISP drops one frame in ten
	 * under bandwidth pressure, both streams still report a plausible rate
	 * and the sequence numbers are the only place the loss shows up.
	 */

	uint32_t v4l2_seq;
	bool v4l2_seq_valid;
	uint32_t gaps;
	uint32_t gap_frames;

	/* What the ISP is producing */

	uint32_t src_w;
	uint32_t src_h;
	uint32_t src_ystride;

	/* And which YUV the ISP means by it.
	 *
	 * Recorded because the detector converts this to RGB and needs the matrix
	 * and the range. The vendor demo is no guide here: it takes RGB out of
	 * OpenCV and never converts YUV at all, so its RGA use never exercised
	 * this. Getting it wrong does not break the picture, it shifts every colour
	 * slightly - exactly the kind of fault that presents as a model which has
	 * become inexplicably unsure of itself.
	 */

	uint32_t src_colorspace;
	uint32_t src_quant;
	uint32_t src_ycbcr;

	/* What goes into shared memory */

	uint32_t out_w;
	uint32_t out_h;

	volatile struct amp_cam_desc *desc;
	volatile uint8_t *slot[AMP_CAM_NBUFFERS];
	uint32_t next;
	uint32_t seq;
	uint32_t published;

	/* Resampling index tables, one entry per destination pixel/row.
	 *
	 * Built once because the alternative was a 64-bit multiply and divide per
	 * pixel - and the compiler cannot fold them away even in the common case
	 * where source and destination are the same size, because it does not know
	 * that until run time. On an A55, whose divider is not pipelined, that was
	 * tens of cycles on every one of 147456 pixels.
	 */

	uint32_t *xmap;
	uint32_t *ymap;

	/* Ordinary cached memory to convert into, copied to the slot in one go.
	 *
	 * This exists because of how the carveout is mapped on this side. It is
	 * declared no-map, so it is absent from the kernel's linear map, so
	 * phys_mem_access_prot() (arch/arm64/mm/mmu.c:99) takes its first branch
	 * and returns pgprot_noncached() - which on arm64 is MT_DEVICE_nGnRnE, the
	 * strictest attribute there is: non-gathering, non-reordering, no early
	 * write acknowledgement. Note the branch order: the O_SYNC case that would
	 * have given MT_NORMAL_NC is unreachable for a no-map region, so opening
	 * /dev/mem differently does not help.
	 *
	 * Storing pixels straight there meant 147456 individual four-byte
	 * transactions, each waiting for the previous to reach DRAM. Converting
	 * into cached memory and then issuing one memcpy cannot make the mapping
	 * gather, but it does replace those with far fewer and much wider stores.
	 *
	 * The remote, incidentally, maps the same bytes MT_NORMAL_NC, which does
	 * gather. Same memory, and this side got the worse attributes purely
	 * because the region is no-map.
	 */

	uint8_t *stage;

	/* And the same treatment for the source, which turned out to be where the
	 * time actually went.
	 *
	 * The first attempt at this assumed the uncached writes were the problem
	 * and moved only the destination into cached memory. Splitting the
	 * measurement in two disproved it immediately: the conversion still took
	 * 11.6ms writing to ordinary memory, while the bulk copy of the whole
	 * 589KB frame into the carveout took 3.2ms. So the writes were never the
	 * bottleneck - the reads were.
	 *
	 * V4L2 MMAP buffers get there through dma_mmap_attrs(), which for a
	 * non-coherent device returns pgprot_dmacoherent() - MT_NORMAL_NC on
	 * arm64. The conversion then took three separate byte loads per pixel out
	 * of it, 442368 uncached accesses with no cache line to amortise any of
	 * them. Fetching the plane once with a bulk copy turns that into one
	 * streaming read.
	 */

	uint8_t *nv12;
	size_t nv12len;

	/* Maxima and totals both, because a maximum on its own has now sent this
	 * diagnosis down the wrong path twice.
	 *
	 * A worst case says how bad one frame got; it cannot distinguish a loop
	 * that costs ten milliseconds every time from one that costs two and was
	 * descheduled once. Only the mean answers that, and the two together say
	 * whether the cost is intrinsic or a hiccup.
	 */

	uint32_t fetch_us_max;
	uint32_t convert_us_max;
	uint32_t copy_us_max;

	uint64_t fetch_us_sum;
	uint64_t convert_us_sum;
	uint64_t copy_us_sum;

	int cpu;
};

/****************************************************************************
 * Name: cam_probe
 *
 * Description:
 *   Find the ISP capture node by asking each /dev/video* what it is, the same
 *   way the touch device is found rather than by hardcoding a number.
 *
 *   Node numbering here is not stable: rkcif, rkisp and their several paths all
 *   register video nodes, and which one lands on video0 depends on probe order.
 *   Matching on the driver and card names is the only way that survives a kernel
 *   or device-tree change.
 *
 ****************************************************************************/

static int cam_probe(char *path, size_t pathlen, const char *cardwant,
		     bool verbose)
{
	int best = -1;
	int i;

	for (i = 0; i < 32; i++) {
		struct v4l2_capability cap;
		char dev[32];
		int fd;
		bool is_isp;
		bool is_want;

		snprintf(dev, sizeof(dev), "/dev/video%d", i);
		fd = open(dev, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;

		memset(&cap, 0, sizeof(cap));
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
			close(fd);
			continue;
		}

		is_isp = strstr((const char *)cap.driver, "rkisp") != NULL;
		is_want = strstr((const char *)cap.card, cardwant) != NULL;

		if (verbose)
			printf("  %s: driver \"%s\", card \"%s\"%s\n",
			       dev, cap.driver, cap.card,
			       (is_isp && is_want) ? "  <- using this" : "");

		close(fd);

		/* Which path is wanted is now the caller's choice.
		 *
		 * The display takes the main path: it has the scaler and the
		 * full set of YUV formats. The detector takes the self path,
		 * which also has a scaler and can run at a different size at the
		 * same time - that being the whole point of having two.
		 *
		 * Matching on the card name rather than the node number is not
		 * fastidiousness. On this board rkcif registers eleven nodes
		 * before rkisp registers any, so the main path lands on
		 * /dev/video11 and the self path on /dev/video12; both numbers
		 * move if either driver's probe order changes.
		 */

		if (is_isp && is_want && best < 0) {
			best = i;
			snprintf(path, pathlen, "%s", dev);
		}
	}

	return best;
}

/****************************************************************************
 * Name: cam_open
 ****************************************************************************/

static int cam_open(struct camera *c, const char *name, const char *cardwant,
		    const char *want, uint32_t req_w, uint32_t req_h,
		    bool verbose)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	char found[32];
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	unsigned int i;

	memset(c, 0, sizeof(*c));
	c->fd = -1;
	c->name = name;

	if (want == NULL) {
		if (verbose)
			printf("looking for the ISP %s node:\n", cardwant);

		if (cam_probe(found, sizeof(found), cardwant, verbose) < 0) {
			fprintf(stderr,
				"no rkisp %s node found. Is the camera"
				" pipeline enabled and the module fitted?\n",
				cardwant);
			return -1;
		}

		want = found;
	}

	/* Non-blocking, and that is load-bearing rather than tidiness.
	 *
	 * cam_drain() decides it has taken every ready frame when DQBUF answers
	 * EAGAIN, and DQBUF only answers EAGAIN on a non-blocking descriptor - on
	 * a blocking one it waits for the next frame instead. Open this without
	 * the flag and the drain never returns to the poll loop: it sits there
	 * consuming frames at the capture rate forever, which starves touch (the
	 * loop never gets to look at the other descriptor again) and makes Ctrl-C
	 * ineffective, because the signal only interrupts DQBUF with EINTR and
	 * g_stop is tested one level up.
	 *
	 * The probing loop above already had the flag; this is the copy that
	 * mattered and did not.
	 */

	c->fd = open(want, O_RDWR | O_NONBLOCK);
	if (c->fd < 0) {
		fprintf(stderr, "%s: %s\n", want, strerror(errno));
		return -1;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	fmt.fmt.pix_mp.width = req_w;
	fmt.fmt.pix_mp.height = req_h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;

	if (ioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
		fprintf(stderr, "%s: S_FMT: %s\n", want, strerror(errno));
		goto fail;
	}

	if (fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12) {
		fprintf(stderr,
			"%s: asked for NV12, got %.4s - this program only"
			" converts NV12\n",
			want, (const char *)&fmt.fmt.pix_mp.pixelformat);
		goto fail;
	}

	c->src_w = fmt.fmt.pix_mp.width;
	c->src_h = fmt.fmt.pix_mp.height;
	c->src_ystride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	c->src_colorspace = fmt.fmt.pix_mp.colorspace;
	c->src_quant = fmt.fmt.pix_mp.quantization;
	c->src_ycbcr = fmt.fmt.pix_mp.ycbcr_enc;
	if (c->src_ystride == 0)
		c->src_ystride = c->src_w;

	printf("%s [%s]: capturing NV12 %ux%u, y stride %u\n",
	       want, c->name, c->src_w, c->src_h, c->src_ystride);

	/* The colour space, reported by number and by name where the name is one
	 * that matters to the conversion. Printed for both streams even though only
	 * the detector converts, because a disagreement between the two paths would
	 * itself be worth knowing.
	 */

	printf("%s: colorspace %u (%s), quantization %u (%s), ycbcr_enc %u\n",
	       c->name, c->src_colorspace,
	       c->src_colorspace == V4L2_COLORSPACE_REC709 ? "REC709" :
	       c->src_colorspace == V4L2_COLORSPACE_SMPTE170M ? "SMPTE170M/601" :
	       c->src_colorspace == V4L2_COLORSPACE_DEFAULT ? "DEFAULT" : "other",
	       c->src_quant,
	       c->src_quant == V4L2_QUANTIZATION_FULL_RANGE ? "full" :
	       c->src_quant == V4L2_QUANTIZATION_LIM_RANGE ? "limited" :
	       "default", c->src_ycbcr);

	if (c->src_w != req_w || c->src_h != req_h)
		printf("  the ISP adjusted this from the requested %ux%u\n",
		       req_w, req_h);

	memset(&req, 0, sizeof(req));
	req.count = CAM_MAX_BUFFERS;
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;

	if (ioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "%s: REQBUFS: %s\n", want, strerror(errno));
		goto fail;
	}

	if (req.count < 2) {
		fprintf(stderr, "%s: only got %u buffers, need at least 2\n",
			want, req.count);
		goto fail;
	}

	c->nbuffers = req.count;

	for (i = 0; i < c->nbuffers; i++) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = planes;
		buf.length = VIDEO_MAX_PLANES;

		if (ioctl(c->fd, VIDIOC_QUERYBUF, &buf) < 0) {
			fprintf(stderr, "%s: QUERYBUF %u: %s\n", want, i,
				strerror(errno));
			goto fail;
		}

		c->buf[i].length = buf.m.planes[0].length;
		c->buf[i].start = mmap(NULL, c->buf[i].length,
				       PROT_READ | PROT_WRITE, MAP_SHARED,
				       c->fd, buf.m.planes[0].m.mem_offset);
		if (c->buf[i].start == MAP_FAILED) {
			fprintf(stderr, "%s: mmap buffer %u: %s\n", want, i,
				strerror(errno));
			c->buf[i].start = NULL;
			goto fail;
		}

		if (ioctl(c->fd, VIDIOC_QBUF, &buf) < 0) {
			fprintf(stderr, "%s: QBUF %u: %s\n", want, i,
				strerror(errno));
			goto fail;
		}
	}

	if (ioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr,
			"%s: STREAMON: %s. The pipeline is linked but not"
			" streaming - check the sensor is fitted and its subdev"
			" accepted a format.\n",
			want, strerror(errno));
		goto fail;
	}

	return 0;

fail:
	for (i = 0; i < CAM_MAX_BUFFERS; i++) {
		if (c->buf[i].start != NULL)
			munmap(c->buf[i].start, c->buf[i].length);
	}

	if (c->fd >= 0)
		close(c->fd);

	c->fd = -1;
	return -1;
}

/****************************************************************************
 * Name: cam_publish_setup
 *
 * Description:
 *   Fill in the descriptor's fixed fields and hand out the slot pointers. Done
 *   before the magic is written, so the remote cannot find the magic and then
 *   read geometry that has not been stored yet - the same ordering the control
 *   block uses.
 *
 ****************************************************************************/

static int cam_publish_setup(struct camera *c, volatile uint8_t *shm,
			     uint32_t out_w, uint32_t out_h)
{
	uint32_t i;

	c->out_w = out_w;
	c->out_h = out_h;

	/* Exactly what the conversion reads: the Y plane, then the interleaved
	 * chroma plane at half the height.
	 */

	bool resample = (c->src_w != out_w || c->src_h != out_h);

	c->nv12len = (size_t)c->src_ystride * c->src_h * 3 / 2;

	c->stage = malloc((size_t)out_w * out_h * AMP_CAM_BPP);
	c->nv12 = malloc(c->nv12len);

	/* The tables are allocated only when they are needed, and their absence is
	 * what selects the vectorised path. A null pointer is doing double duty as
	 * a mode flag, which is worth naming: the alternative was a separate
	 * boolean that could disagree with whether the tables exist.
	 */

	if (resample) {
		c->xmap = malloc(out_w * sizeof(*c->xmap));
		c->ymap = malloc(out_h * sizeof(*c->ymap));
	}

	if (c->stage == NULL || c->nv12 == NULL ||
	    (resample && (c->xmap == NULL || c->ymap == NULL))) {
		fprintf(stderr, "camera: out of memory for %ux%u staging\n",
			out_w, out_h);
		free(c->xmap);
		free(c->ymap);
		free(c->stage);
		free(c->nv12);
		c->xmap = NULL;
		c->ymap = NULL;
		c->stage = NULL;
		c->nv12 = NULL;
		return -1;
	}

	/* Touch every page now, so the first frame is not paying for demand
	 * paging inside a timed section. Nearly a megabyte of fresh heap is a few
	 * hundred faults, which is exactly the sort of one-off that sets a worst
	 * case nobody can then explain.
	 */

	memset(c->stage, 0, (size_t)out_w * out_h * AMP_CAM_BPP);
	memset(c->nv12, 0, c->nv12len);

	/* The divides happen here, once, instead of per pixel per frame. When the
	 * ISP gave us exactly what we asked for these are the identity, which is
	 * the common case on this board - but the table costs a few kilobytes and
	 * removes the question.
	 */

	if (resample) {
		for (i = 0; i < out_w; i++)
			c->xmap[i] =
				(uint32_t)((uint64_t)i * c->src_w / out_w);

		for (i = 0; i < out_h; i++)
			c->ymap[i] =
				(uint32_t)((uint64_t)i * c->src_h / out_h);
	}
	c->desc = (volatile struct amp_cam_desc *)(shm + AMP_CAM_DESC_OFFSET);
	c->slot[0] = shm + AMP_CAM_BUF0_OFFSET;
	c->slot[1] = shm + AMP_CAM_BUF1_OFFSET;

	c->desc->version = AMP_CAM_VERSION;
	c->desc->width = out_w;
	c->desc->height = out_h;
	c->desc->stride = out_w * AMP_CAM_BPP;
	c->desc->bpp = AMP_CAM_BPP;
	c->desc->nbuffers = AMP_CAM_NBUFFERS;
	c->desc->bufsize = out_w * AMP_CAM_BPP * out_h;
	c->desc->bufoffset[0] = AMP_CAM_BUF0_OFFSET;
	c->desc->bufoffset[1] = AMP_CAM_BUF1_OFFSET;
	c->desc->seq = 0;
	c->desc->ready = 0;

	__sync_synchronize();

	c->desc->magic = AMP_CAM_MAGIC;

	__sync_synchronize();

	printf("camera: publishing %ux%u XRGB8888, %u bytes per frame,"
	       " descriptor at 0x%08lx%s\n",
	       out_w, out_h, out_w * out_h * AMP_CAM_BPP,
	       AMP_SHM_BASE + AMP_CAM_DESC_OFFSET,
	       resample ? " (resampling, scalar path)" :
	       " (no resampling, vectorised path)");
	return 0;
}

/****************************************************************************
 * Name: nv12_to_xrgb
 *
 * Description:
 *   Convert and resample in one pass. BT.601 limited range, integer
 *   coefficients, point sampling on both axes.
 *
 *   Point sampling rather than averaging because this runs per frame and the
 *   result is a preview: the difference is visible only on fine detail, and the
 *   cost of a box filter is four times the reads. If the picture ever needs to be
 *   better than a preview, the right answer is the RGA doing it in hardware, not
 *   a better loop here.
 *
 ****************************************************************************/

static void nv12_to_xrgb(const uint8_t *src, uint32_t sh, uint32_t sstride,
			 uint8_t *dst, uint32_t dw, uint32_t dh,
			 const uint32_t *xmap, const uint32_t *ymap)
{
	const uint8_t *ysrc = src;
	const uint8_t *uvsrc = src + (size_t)sstride * sh;
	uint32_t dy;

	for (dy = 0; dy < dh; dy++) {
		uint32_t sy = ymap[dy];
		const uint8_t *yrow = ysrc + (size_t)sy * sstride;
		const uint8_t *uvrow = uvsrc + (size_t)(sy / 2) * sstride;
		uint32_t *out = (uint32_t *)(dst + (size_t)dy * dw * 4);
		uint32_t dx;

		for (dx = 0; dx < dw; dx++) {
			uint32_t sx = xmap[dx];
			int c = (int)yrow[sx] - 16;
			int d = (int)uvrow[(sx & ~1u)] - 128;
			int e = (int)uvrow[(sx & ~1u) + 1] - 128;
			int r = (298 * c + 409 * e + 128) >> 8;
			int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
			int b = (298 * c + 516 * d + 128) >> 8;

			if (r < 0)
				r = 0;
			else if (r > 255)
				r = 255;

			if (g < 0)
				g = 0;
			else if (g > 255)
				g = 255;

			if (b < 0)
				b = 0;
			else if (b > 255)
				b = 255;

			/* Opaque alpha, even though the path this frame is on
			 * ignores it: the VOP mixer for this layer runs with the
			 * source alpha pinned to the global 0xff, so what is in
			 * the top byte makes no difference on screen today.
			 *
			 * It is set anyway because the obvious next consumer is
			 * an LVGL image object, and LVGL at 32-bit colour does
			 * honour per-pixel alpha when it composites one. A frame
			 * carrying zero there would be perfectly correct in this
			 * path and completely invisible in that one, which is a
			 * trap worth not leaving behind for the sake of a
			 * constant.
			 */

			out[dx] = 0xff000000u | ((uint32_t)r << 16) |
				  ((uint32_t)g << 8) | (uint32_t)b;
		}
	}
}

/****************************************************************************
 * Name: nv12_to_xrgb_1to1
 *
 * Description:
 *   The same conversion with the resampling removed, for when the ISP gave
 *   exactly the geometry that was asked for - which is what happens on this
 *   board.
 *
 *   This exists because the index table above cannot be vectorised. Asking gcc
 *   for the assembly settles it: the general loop is 46 scalar instructions per
 *   pixel with no NEON at all, even at -O3, because reading the source through
 *   xmap[] is a gather and the compiler cannot prove it is a linear stride. Take
 *   the indirection out and the same arithmetic vectorises into 298 NEON
 *   instructions covering many pixels at a time.
 *
 *   Which is the awkward part of the previous change: the table was introduced
 *   to remove a 64-bit divide per pixel, and the divide was indeed removed, but
 *   the measured cost did not move - because the divide was never what made this
 *   loop scalar, and neither version could vectorise. The table cost nothing and
 *   bought nothing.
 *
 *   Two pixels per iteration so that the chroma pair is read once and all three
 *   source streams advance linearly; a single-pixel loop with dx & ~1 leaves the
 *   chroma reads looking irregular enough to block vectorisation again.
 *
 *   The arithmetic is a duplicate of the general path and must stay bit-identical
 *   to it. That is checked rather than hoped for: the host-side test runs both
 *   over the same frame and compares every byte.
 *
 ****************************************************************************/

/* noinline, and it is doing real work here rather than documenting intent.
 *
 * Left to itself gcc inlines both conversions into main() - the whole program
 * collapses into four symbols - and the vectoriser then gives up on this loop.
 * Compiled as its own function with runtime bounds it produces 334 NEON
 * instructions; inlined into main() it produces none. The call happens once per
 * frame, so the overhead is not measurable, and being a separate function is the
 * thing that makes the loop vectorisable at all.
 *
 * This was nearly missed: a standalone probe of the same loop appeared to
 * vectorise beautifully, but the probe called it with constant dimensions, so gcc
 * had specialised it. The number that matters came from compiling the real file.
 */

__attribute__((noinline))
static void nv12_to_xrgb_1to1(const uint8_t *src, uint32_t sh,
			      uint32_t sstride, uint8_t *dst, uint32_t dw,
			      uint32_t dh)
{
	const uint8_t *uvsrc = src + (size_t)sstride * sh;
	uint32_t dy;

	for (dy = 0; dy < dh; dy++) {
		const uint8_t *yrow = src + (size_t)dy * sstride;
		const uint8_t *uvrow = uvsrc + (size_t)(dy / 2) * sstride;
		uint32_t *out = (uint32_t *)(dst + (size_t)dy * dw * 4);
		uint32_t dx;

		for (dx = 0; dx + 1 < dw; dx += 2) {
			int d = (int)uvrow[dx] - 128;
			int e = (int)uvrow[dx + 1] - 128;
			int c0 = (int)yrow[dx] - 16;
			int c1 = (int)yrow[dx + 1] - 16;
			int r0 = (298 * c0 + 409 * e + 128) >> 8;
			int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
			int b0 = (298 * c0 + 516 * d + 128) >> 8;
			int r1 = (298 * c1 + 409 * e + 128) >> 8;
			int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
			int b1 = (298 * c1 + 516 * d + 128) >> 8;

			if (r0 < 0)
				r0 = 0;
			else if (r0 > 255)
				r0 = 255;

			if (g0 < 0)
				g0 = 0;
			else if (g0 > 255)
				g0 = 255;

			if (b0 < 0)
				b0 = 0;
			else if (b0 > 255)
				b0 = 255;

			if (r1 < 0)
				r1 = 0;
			else if (r1 > 255)
				r1 = 255;

			if (g1 < 0)
				g1 = 0;
			else if (g1 > 255)
				g1 = 255;

			if (b1 < 0)
				b1 = 0;
			else if (b1 > 255)
				b1 = 255;

			out[dx] = 0xff000000u | ((uint32_t)r0 << 16) |
				  ((uint32_t)g0 << 8) | (uint32_t)b0;
			out[dx + 1] = 0xff000000u | ((uint32_t)r1 << 16) |
				      ((uint32_t)g1 << 8) | (uint32_t)b1;
		}

		/* Odd width leaves one pixel over. It shares the chroma pair with
		 * the column to its left, exactly as the general path would.
		 */

		if (dx < dw) {
			uint32_t ux = dx & ~1u;
			int d = (int)uvrow[ux] - 128;
			int e = (int)uvrow[ux + 1] - 128;
			int c = (int)yrow[dx] - 16;
			int r = (298 * c + 409 * e + 128) >> 8;
			int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
			int b = (298 * c + 516 * d + 128) >> 8;

			if (r < 0)
				r = 0;
			else if (r > 255)
				r = 255;

			if (g < 0)
				g = 0;
			else if (g > 255)
				g = 255;

			if (b < 0)
				b = 0;
			else if (b > 255)
				b = 255;

			out[dx] = 0xff000000u | ((uint32_t)r << 16) |
				  ((uint32_t)g << 8) | (uint32_t)b;
		}
	}
}

/****************************************************************************
 * Name: cam_drain
 *
 * Description:
 *   Take every frame the ISP has ready, convert the newest into the next slot,
 *   publish it and tell the remote.
 *
 ****************************************************************************/

static void cam_drain(struct camera *c, int rpfd, bool verbose)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	for (; ; ) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		struct amp_cam_msg msg;
		struct timespec tf;
		struct timespec t0;
		struct timespec t1;
		struct timespec t2;
		uint32_t slot;
		long us;

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.m.planes = planes;
		buf.length = VIDEO_MAX_PLANES;

		if (ioctl(c->fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				return;

			/* Retrying on EINTR is right, but not unconditionally:
			 * the signal that interrupted this is usually the one
			 * asking the program to stop, and g_stop is only tested
			 * in the loop this function has to return to first.
			 */

			if (errno == EINTR) {
				if (g_stop)
					return;

				continue;
			}

			fprintf(stderr, "%s: DQBUF: %s\n", c->name,
				strerror(errno));
			return;
		}

		/* The ISP's own frame counter, checked before anything else is
		 * done with the buffer.
		 *
		 * This is the measurement the dual-stream question turns on. Rate
		 * cannot answer it: if bandwidth pressure makes the ISP drop one
		 * frame in ten, both streams still report a believable rate and
		 * the loss is visible only as a step in this counter.
		 */

		if (c->v4l2_seq_valid && buf.sequence != c->v4l2_seq + 1) {
			c->gaps++;
			c->gap_frames += buf.sequence - c->v4l2_seq - 1;
		}

		c->v4l2_seq = buf.sequence;
		c->v4l2_seq_valid = true;

		/* Count-only stream: straight back into the queue.
		 *
		 * Nothing is converted and nothing is published, because the
		 * question being asked is whether the hardware can produce two
		 * streams at once - not what the second one is worth. Doing the
		 * least work here also means a drop measured on this stream is
		 * the ISP's, not this program's.
		 */

		if (!c->publish) {
			c->published++;

			if (ioctl(c->fd, VIDIOC_QBUF, &buf) < 0)
				fprintf(stderr, "%s: QBUF: %s\n", c->name,
					strerror(errno));

			continue;
		}

		slot = c->next;

		clock_gettime(CLOCK_MONOTONIC, &tf);

		/* One streaming read out of the capture buffer, replacing three
		 * uncached byte loads per pixel.
		 */

		memcpy(c->nv12, c->buf[buf.index].start,
		       c->nv12len < c->buf[buf.index].length ?
		       c->nv12len : c->buf[buf.index].length);

		clock_gettime(CLOCK_MONOTONIC, &t0);

		if (c->xmap == NULL)
			nv12_to_xrgb_1to1(c->nv12, c->src_h, c->src_ystride,
					  c->stage, c->out_w, c->out_h);
		else
			nv12_to_xrgb(c->nv12, c->src_h, c->src_ystride,
				     c->stage, c->out_w, c->out_h, c->xmap,
				     c->ymap);

		clock_gettime(CLOCK_MONOTONIC, &t1);

		/* One bulk copy into the carveout. The volatile qualifier is cast
		 * away deliberately: it is there to stop the compiler caching
		 * individual reads and writes, and what is wanted here is exactly
		 * the opposite - the widest stores the C library will use. The
		 * ordering that matters is supplied by the barrier below, not by
		 * the qualifier.
		 */

		memcpy((void *)c->slot[slot], c->stage,
		       (size_t)c->out_w * c->out_h * AMP_CAM_BPP);

		clock_gettime(CLOCK_MONOTONIC, &t2);

		/* Pixels first, then the index, then the sequence number, with a
		 * barrier between each. The carveout is non-cacheable on the
		 * remote so no flushing is needed, but non-cacheable does not
		 * imply ordered: without these the remote could see a new
		 * sequence number pointing at a slot that is still half written.
		 */

		__sync_synchronize();

		c->desc->ready = slot;

		__sync_synchronize();

		c->seq++;
		c->desc->seq = c->seq;

		__sync_synchronize();

		c->next = (slot + 1) % AMP_CAM_NBUFFERS;
		c->published++;

		/* Timed apart so the measurement says which half costs what,
		 * rather than leaving it to be guessed from one total.
		 */

		us = (t0.tv_sec - tf.tv_sec) * 1000000 +
		     (t0.tv_nsec - tf.tv_nsec) / 1000;
		if (us > (long)c->fetch_us_max)
			c->fetch_us_max = (uint32_t)us;
		c->fetch_us_sum += (uint64_t)us;

		us = (t1.tv_sec - t0.tv_sec) * 1000000 +
		     (t1.tv_nsec - t0.tv_nsec) / 1000;
		if (us > (long)c->convert_us_max)
			c->convert_us_max = (uint32_t)us;
		c->convert_us_sum += (uint64_t)us;

		us = (t2.tv_sec - t1.tv_sec) * 1000000 +
		     (t2.tv_nsec - t1.tv_nsec) / 1000;
		if (us > (long)c->copy_us_max)
			c->copy_us_max = (uint32_t)us;
		c->copy_us_sum += (uint64_t)us;

		/* Sampled per frame rather than once, because nothing stops the
		 * scheduler moving this between a little core and a big one.
		 */

		c->cpu = sched_getcpu();

		if (ioctl(c->fd, VIDIOC_QBUF, &buf) < 0)
			perror("QBUF");

		/* The notification carries the index, but the remote reads the
		 * descriptor rather than trusting it - so a message lost because
		 * the rpmsg pool was busy costs one dropped frame, not a frame
		 * torn out of the slot being written.
		 */

		if (rpfd >= 0) {
			memset(&msg, 0, sizeof(msg));
			msg.cmd = AMP_SHM_CMD_CAMERA;
			msg.seq = c->seq;
			msg.ready = slot;

			if (!rpmsg_send(rpfd, &msg, sizeof(msg)) && verbose)
				fprintf(stderr, "camera: notify seq %u: %s\n",
					c->seq, strerror(errno));
		}
	}
}

/* Inference
 * ---------------------------------------------------------------------------
 * Compiled in only with -DAMP_WITH_RKNN, and dlopen'd even then.
 *
 * Two separate decisions, for two different reasons.
 *
 * The compile-time switch keeps this file buildable without a checkout of
 * rknn-toolkit2 next to it. Only type definitions are needed at build time -
 * rknn_tensor_attr, rga_buffer_t and friends - but needing them unconditionally
 * would mean this program could not be built at all on a machine that has the
 * board but not the vendor SDK, and the display and touch paths would become
 * hostage to a dependency they do not use.
 *
 * The dlopen keeps the *binary* free of them. Linking librknnrt and librga would
 * put them in DT_NEEDED, and a system without the RKNN runtime installed could
 * then not even load the program - the loader fails before main(). Same failure
 * mode as making the rpmsg open fatal, one layer lower down: an optional feature
 * taking the essential ones with it. With dlopen, a missing library is a message
 * and no detector, and the camera still draws.
 *
 * The libraries are looked up by plain name so LD_LIBRARY_PATH decides, which is
 * how they are actually deployed on this board - copied next to the binary rather
 * than installed.
 */

#ifdef AMP_WITH_RKNN

#include <dlfcn.h>

#include <math.h>

#include "rknn_api.h"
#include "im2d.h"
#include "rga.h"

/* Only the entry points this needs, and the RGA ones are the C variants -
 * improcess() and wrapbuffer_virtualaddr_t() are declared IM_C_API, so the C++
 * wrappers with their default arguments never come into it.
 */

struct rknn_fns {
	void *lib;
	int (*init)(rknn_context *, void *, uint32_t, uint32_t, rknn_init_extend *);
	int (*destroy)(rknn_context);
	int (*query)(rknn_context, rknn_query_cmd, void *, uint32_t);
	int (*inputs_set)(rknn_context, uint32_t, rknn_input *);
	int (*run)(rknn_context, rknn_run_extend *);
	int (*outputs_get)(rknn_context, uint32_t, rknn_output *,
			   rknn_output_extend *);
	int (*outputs_release)(rknn_context, uint32_t, rknn_output *);
	int (*set_core_mask)(rknn_context, rknn_core_mask);
};

struct rga_fns {
	void *lib;
	rga_buffer_t (*wrap)(void *, int, int, int, int, int);
	IM_STATUS (*process)(rga_buffer_t, rga_buffer_t, rga_buffer_t, im_rect,
			     im_rect, im_rect, int);

	/* The buffer import path, which is what makes the conversion cheap.
	 *
	 * Passing improcess() a virtual address means the driver walks this
	 * process's page tables, pins the pages and builds an RGA MMU mapping over
	 * them, then tears it all down again - on every call, because the RGA has
	 * its own MMU and cannot use a CPU address. The probe measured that at
	 * 2346us for a 1280x720 NV12 frame, 7us per page against 1.2 for ordinary
	 * memory: a V4L2 capture buffer is dma-coherent and possibly VM_PFNMAP, so
	 * it cannot take the fast get_user_pages path.
	 *
	 * Importing once per buffer replaces that with nothing, because a V4L2 MMAP
	 * buffer is already dma-buf backed - the ISP DMAs into it, so its
	 * scatter-gather table exists and its pages are already pinned.
	 *
	 * Only the im_handle_param_t forms are declared outside __cplusplus, and
	 * those are the unmangled symbols in the library.
	 *
	 * Resolved but not required: an older librga without them costs 2.3ms a
	 * frame, not the detector.
	 */

	rga_buffer_handle_t (*import_fd)(int, im_handle_param_t *);
	rga_buffer_handle_t (*import_va)(void *, im_handle_param_t *);
	rga_buffer_t (*wrap_handle)(rga_buffer_handle_t, int, int, int, int,
				    int);
	IM_STATUS (*release_handle)(rga_buffer_handle_t);
};

/* yolov5 output geometry. Not derived at run time because it is a property of
 * the exported model rather than of the hardware, and getting it from the tensor
 * shapes would mean inferring the anchor set - which is not in the model at all.
 *
 * These are the values the vendor post-processing uses for this model file; the
 * decode below has to match them exactly or every box lands in the wrong place.
 */

#define YOLO_CLASSES     80
#define YOLO_ANCHORS     3
#define YOLO_PROPS       (5 + YOLO_CLASSES)   /* x y w h obj + classes */
#define YOLO_HEADS       3

/* The reference's thresholds, kept rather than retuned. A detector that boxes
 * different things than the vendor demo would make any disagreement about
 * correctness impossible to attribute - it could be the port or it could be the
 * threshold, and there would be no way to tell which.
 */

#define YOLO_BOX_THRESH  0.25f
#define YOLO_NMS_THRESH  0.45f

static const int g_yolo_anchor[YOLO_HEADS][YOLO_ANCHORS * 2] = {
	{  10,  13,  16,  30,  33,  23 },   /* stride 8  */
	{  30,  61,  62,  45,  59, 119 },   /* stride 16 */
	{ 116,  90, 156, 198, 373, 326 },   /* stride 32 */
};

static const int g_yolo_stride[YOLO_HEADS] = { 8, 16, 32 };

/* One candidate surviving the confidence threshold, before NMS. */

struct yolo_cand {
	float x;                        /* left, in model pixels */
	float y;                        /* top */
	float w;
	float h;
	float score;
	int cls;
	bool keep;
};

#define YOLO_MAX_CAND 512

#endif /* AMP_WITH_RKNN */

/* Detection publishing
 * ---------------------------------------------------------------------------
 * The producer for the results block. What fills the boxes is separate from how
 * they are published, and this is the publishing half.
 *
 * It runs on its own thread, which is not a preference. One inference takes
 * 17.4ms on a single NPU core, measured; the poll loop below also forwards touch
 * and publishes camera frames. Detecting ten times a second from inside that loop
 * would freeze touch for 17ms out of every 100 - a sixth of the time
 * unresponsive, which is exactly the kind of "works but feels broken" that took
 * five attempts to find in A-15.
 *
 * Nothing is dynamically linked for it. The libraries the real detector needs -
 * librknnrt and librga - will be dlopen'd when that lands, not linked, because
 * this binary currently depends on nothing but libc and that is worth keeping:
 * linking them would mean a system without the RKNN runtime cannot even load the
 * program, and the display and touch paths would go down with a feature they do
 * not use. Same reason the rpmsg open was made non-fatal.
 */

struct detector {
	volatile struct amp_det_desc *desc;
	pthread_t thread;
	bool running;
	bool stop;

	/* Synthetic mode: boxes made up here rather than detected.
	 *
	 * This exists to test the transport on its own and it is worth keeping
	 * afterwards. With inference in the loop a screen with no boxes has six
	 * possible causes - RGA, the model, post-processing, this publish, the
	 * doorbell, the remote's read - and no way to tell them apart. With
	 * synthetic boxes there is one thing being tested, so if they appear the
	 * whole transport is proven and anything that breaks later is in the
	 * inference chain.
	 */

	bool synthetic;

	int rpfd;

	uint32_t width;
	uint32_t height;
	uint32_t published;
	uint32_t notify_fail;

#ifdef AMP_WITH_RKNN

	/* Real inference. The camera this reads from is its own, not the one the
	 * poll loop drains: this thread is the only consumer of those frames, so
	 * handing them over would be a synchronisation problem invented for no
	 * reason.
	 */

	struct camera *cam;
	struct rknn_fns rk;
	struct rga_fns rga;
	rknn_context ctx;
	void *model;

	uint32_t in_w;                  /* model input, from the tensor attrs */
	uint32_t in_h;
	uint8_t *in_buf;                /* RGB888 the RGA writes and RKNN reads */

	uint32_t n_out;
	int32_t out_zp[YOLO_HEADS];
	float out_scale[YOLO_HEADS];
	uint32_t out_size[YOLO_HEADS];
	void *out_buf[YOLO_HEADS];

	/* Letterbox, computed once from the capture and model sizes. Kept rather
	 * than recomputed so the forward transform used by the RGA and the inverse
	 * used on the boxes cannot drift apart.
	 */

	float lb_scale;
	uint32_t lb_pad_x;
	uint32_t lb_pad_y;
	uint32_t lb_w;                  /* the image inside the letterbox */
	uint32_t lb_h;

	struct yolo_cand cand[YOLO_MAX_CAND];

	/* Phases timed apart, for the reason every other measurement in this
	 * program is: a single total cannot say which half to fix.
	 */

	uint32_t rga_us_max;
	uint32_t run_us_max;
	uint32_t post_us_max;
	uint64_t rga_us_sum;
	uint64_t run_us_sum;
	uint64_t post_us_sum;
	uint32_t inferences;
	uint32_t boxes_max;

	/* How close the candidate list came to its ceiling.
	 *
	 * Reported because the ceiling is a silent one: yolo_decode() stops
	 * collecting when the array is full, so a scene busy enough to fill it
	 * would lose its lowest-scoring candidates before NMS ever saw them. That
	 * is a defensible thing to do and an indefensible thing to do quietly.
	 */

	uint32_t cands_max;

	/* Minimum published confidence, as the whole percent the transport
	 * carries.
	 *
	 * This exists because the thresholds inside the decode are applied to the
	 * two factors separately - objectness at 0.25 and best class at 0.25 - and
	 * never to the product that becomes the score. A box whose factors are both
	 * 0.28 passes and is published at 8 percent. The reference behaves the same
	 * way and does not filter afterwards either, which is defensible for its own
	 * purpose: it draws onto a still image for someone to inspect. Overlaid live
	 * on a person it is not, and it was measured on this board - 47 of 107
	 * detections came out under 25 percent, and five classes that were not in
	 * the room at all appeared exclusively there.
	 *
	 * Applied after NMS on purpose. The decode and the suppression are proven
	 * equivalent to the reference over 200 random tensors, and folding a
	 * threshold into either of them would invalidate that; a separate stage
	 * afterwards leaves it intact.
	 */

	uint32_t min_score;
	uint32_t filtered;

	/* Where to write diagnostic frames, or NULL. See det_dump(). */

	const char *dump;
	uint32_t dumps;

	/* dma-buf conversion, and the self-check that decides whether to trust it.
	 *
	 * The concern is cache coherency, and it is specific: in_buf is ordinary
	 * cached memory, the RGA writes it by DMA, and the CPU reads it immediately
	 * afterwards when rknn_inputs_set() marshals the input. Something has to
	 * invalidate the CPU's copy in between. With a per-call wrap the driver
	 * does it while importing; whether it still does with a persistent import is
	 * an implementation detail of a closed library, and librga exposes no way to
	 * ask for cache maintenance on its own - imsync() waits on a fence,
	 * c_RkRgaFlush() submits work.
	 *
	 * So it is measured rather than reasoned about. For the first frames the
	 * conversion runs twice, once each way, into two buffers that are then
	 * compared byte for byte. That test needs no scene control and no focus,
	 * which matters: an earlier attempt at this was judged by counting
	 * detections, and the counts turned out to be dominated by a defocused lens
	 * rather than by anything in the code.
	 *
	 * If the two disagree the fast path is abandoned automatically. An
	 * optimisation that cannot prove itself should not be the one that ships.
	 */

	bool zerocopy_want;
	bool zerocopy;
	bool zerocopy_checked;
	uint32_t verify_frames;
	uint32_t verify_bad;
	size_t verify_maxdiff;

	int cam_dmafd[CAM_MAX_BUFFERS];
	rga_buffer_handle_t cam_rgah[CAM_MAX_BUFFERS];
	rga_buffer_handle_t in_rgah;

	/* A second destination, used only while checking. */

	uint8_t *in_buf2;
	rga_buffer_handle_t in2_rgah;

#endif
};

/****************************************************************************
 * Name: det_publish
 *
 * Description:
 *   Make one set of boxes visible to the remote, then ring the doorbell.
 *
 *   The sequence number goes odd, then the payload is written, then it goes even
 *   again. That is a full seqlock rather than the single bump the frames use, and
 *   the reason is that there is only one detection block: the frames are
 *   double-buffered so a reader never touches memory being written, while this is
 *   updated in place and a reader could otherwise copy half of each of two sets
 *   without the sequence number ever looking wrong.
 *
 ****************************************************************************/

static void det_publish(struct detector *d, const struct amp_det_box *box,
			uint32_t count, uint32_t latency_us)
{
	struct amp_det_msg msg;
	uint32_t i;
	uint32_t seq;

	if (d->desc == NULL || count > AMP_DET_MAXBOX)
		return;

	seq = d->desc->seq;

	/* Odd: a write is in progress. Anything reading now will see this and
	 * start over rather than take a half-updated set.
	 */

	d->desc->seq = seq + 1;

	__sync_synchronize();

	for (i = 0; i < count; i++) {
		d->desc->box[i].x = box[i].x;
		d->desc->box[i].y = box[i].y;
		d->desc->box[i].w = box[i].w;
		d->desc->box[i].h = box[i].h;
		d->desc->box[i].cls = box[i].cls;
		d->desc->box[i].score = box[i].score;
		d->desc->box[i].reserved = 0;
	}

	d->desc->count = count;
	d->desc->width = d->width;
	d->desc->height = d->height;
	d->desc->latency_us = latency_us;

	__sync_synchronize();

	d->desc->seq = seq + 2;

	__sync_synchronize();

	d->published++;

	if (d->rpfd < 0)
		return;

	memset(&msg, 0, sizeof(msg));
	msg.cmd = AMP_SHM_CMD_DETECT;
	msg.seq = seq + 2;
	msg.count = count;

	/* Through the same serialised path as everything else, because the poll
	 * loop writes touch and frame notifications to this descriptor while this
	 * thread is running.
	 */

	if (!rpmsg_send(d->rpfd, &msg, sizeof(msg)))
		d->notify_fail++;
}

#ifdef AMP_WITH_RKNN

/****************************************************************************
 * Name: det_load_libs
 *
 * Description:
 *   Resolve everything needed from librknnrt and librga, or fail with the name
 *   of what was missing.
 *
 *   Every symbol is looked up before any is used, so a partial load cannot get
 *   as far as running an inference with a null pointer in the middle of it. The
 *   name that failed is printed because "detector will not run" without it sends
 *   the reader to the wrong library.
 *
 ****************************************************************************/

#define DLSYM(h, dst, name)                                             \
	do {                                                            \
		*(void **)(&(dst)) = dlsym((h), (name));                \
		if ((dst) == NULL) {                                    \
			fprintf(stderr, "detector: %s: %s\n", (name),   \
				dlerror());                             \
			return -1;                                      \
		}                                                       \
	} while (0)

static int det_load_libs(struct detector *d)
{
	d->rk.lib = dlopen("librknnrt.so", RTLD_NOW);
	if (d->rk.lib == NULL) {
		fprintf(stderr,
			"detector: librknnrt.so: %s\n"
			"  It is not linked on purpose - set LD_LIBRARY_PATH to"
			" wherever it was copied.\n", dlerror());
		return -1;
	}

	d->rga.lib = dlopen("librga.so", RTLD_NOW);
	if (d->rga.lib == NULL) {
		fprintf(stderr, "detector: librga.so: %s\n", dlerror());
		return -1;
	}

	DLSYM(d->rk.lib, d->rk.init, "rknn_init");
	DLSYM(d->rk.lib, d->rk.destroy, "rknn_destroy");
	DLSYM(d->rk.lib, d->rk.query, "rknn_query");
	DLSYM(d->rk.lib, d->rk.inputs_set, "rknn_inputs_set");
	DLSYM(d->rk.lib, d->rk.run, "rknn_run");
	DLSYM(d->rk.lib, d->rk.outputs_get, "rknn_outputs_get");
	DLSYM(d->rk.lib, d->rk.outputs_release, "rknn_outputs_release");
	DLSYM(d->rk.lib, d->rk.set_core_mask, "rknn_set_core_mask");

	DLSYM(d->rga.lib, d->rga.wrap, "wrapbuffer_virtualaddr_t");
	DLSYM(d->rga.lib, d->rga.process, "improcess");

	/* Plain dlsym, not DLSYM: see struct rga_fns. Missing these is a slower
	 * conversion, not a missing detector.
	 */

	*(void **)(&d->rga.import_fd) = dlsym(d->rga.lib, "importbuffer_fd");
	*(void **)(&d->rga.import_va) = dlsym(d->rga.lib,
					      "importbuffer_virtualaddr");
	*(void **)(&d->rga.wrap_handle) = dlsym(d->rga.lib,
						"wrapbuffer_handle_t");
	*(void **)(&d->rga.release_handle) = dlsym(d->rga.lib,
						   "releasebuffer_handle");

	return 0;
}

/****************************************************************************
 * Name: det_load_model
 *
 * Description:
 *   Load the .rknn file, learn the input geometry from the model rather than
 *   assuming it, and pin inference to one NPU core.
 *
 *   One core because three were measured: 17.4ms on one, 10.0ms on all three.
 *   Three times the hardware buys 1.74x, so pinning costs 30 percent latency and
 *   leaves two cores entirely free - and the target rate is ten a second against
 *   a single-core ceiling near fifty.
 *
 ****************************************************************************/

static int det_load_model(struct detector *d, const char *path, uint32_t core)
{
	rknn_input_output_num io;
	rknn_tensor_attr attr;
	rknn_sdk_version ver;
	FILE *fp;
	long len;
	uint32_t i;

	fp = fopen(path, "rb");
	if (fp == NULL) {
		fprintf(stderr, "detector: %s: %s\n", path, strerror(errno));
		return -1;
	}

	if (fseek(fp, 0, SEEK_END) != 0 || (len = ftell(fp)) <= 0) {
		fprintf(stderr, "detector: %s: cannot size the model\n", path);
		fclose(fp);
		return -1;
	}

	rewind(fp);
	d->model = malloc((size_t)len);

	if (d->model == NULL ||
	    fread(d->model, 1, (size_t)len, fp) != (size_t)len) {
		fprintf(stderr, "detector: %s: short read\n", path);
		fclose(fp);
		return -1;
	}

	fclose(fp);

	if (d->rk.init(&d->ctx, d->model, (uint32_t)len, 0, NULL) < 0) {
		fprintf(stderr,
			"detector: rknn_init failed. Check the driver came up:"
			" dmesg | grep rknpu\n");
		return -1;
	}

	memset(&ver, 0, sizeof(ver));
	if (d->rk.query(d->ctx, RKNN_QUERY_SDK_VERSION, &ver,
			sizeof(ver)) == 0)
		printf("detector: rknn api %s, driver %s\n", ver.api_version,
		       ver.drv_version);

	memset(&io, 0, sizeof(io));
	if (d->rk.query(d->ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) < 0) {
		fprintf(stderr, "detector: IN_OUT_NUM failed\n");
		return -1;
	}

	/* This decode is written for exactly one output layout: three heads with
	 * three anchors each. Checked rather than assumed, because a different
	 * model file would otherwise be decoded as if it were this one and produce
	 * boxes that are wrong in a way nothing flags.
	 */

	if (io.n_input != 1 || io.n_output != YOLO_HEADS) {
		fprintf(stderr,
			"detector: model has %u inputs and %u outputs; this"
			" decode handles 1 and %d\n",
			io.n_input, io.n_output, YOLO_HEADS);
		return -1;
	}

	d->n_out = io.n_output;

	memset(&attr, 0, sizeof(attr));
	attr.index = 0;
	if (d->rk.query(d->ctx, RKNN_QUERY_INPUT_ATTR, &attr,
			sizeof(attr)) < 0) {
		fprintf(stderr, "detector: INPUT_ATTR failed\n");
		return -1;
	}

	/* NHWC, so dims are 1,H,W,C. Taken from the model instead of hardcoded
	 * 640x640: the file is what decides, and a mismatch here would show up as
	 * a letterbox computed for the wrong target.
	 */

	if (attr.n_dims != 4 || attr.dims[3] != 3) {
		fprintf(stderr,
			"detector: input is not NHWC with 3 channels\n");
		return -1;
	}

	d->in_h = attr.dims[1];
	d->in_w = attr.dims[2];

	printf("detector: model input %ux%ux3\n", d->in_w, d->in_h);

	d->in_buf = malloc((size_t)d->in_w * d->in_h * 3);
	if (d->in_buf == NULL) {
		fprintf(stderr, "detector: out of memory for the input\n");
		return -1;
	}

	/* The letterbox border, filled once. It never changes, so the per-frame
	 * RGA operation only has to write the centre - which saves a second RGA
	 * call every frame for a region whose contents are a constant.
	 *
	 * 114 is the grey yolov5 was trained to pad with.
	 */

	/* 128, which is what the vendor demo pads with - cv::Scalar(128,128,128)
	 * is the default argument to its letterbox().
	 *
	 * Not 114, which is the yolov5 upstream convention and what this used at
	 * first. The difference matters more than fourteen levels sounds: at
	 * 1280x720 into a 640x640 square the padding is 140 rows top and bottom,
	 * so 44 percent of what the model sees is this constant. And the thresholds
	 * in this file were taken from that demo, so its padding belongs with them -
	 * the same argument as the +1 in the IoU. Adopting half of a tuned set is
	 * how a port ends up subtly worse than what it was ported from.
	 */

	memset(d->in_buf, 128, (size_t)d->in_w * d->in_h * 3);

	/* The comparison buffer, allocated and pre-filled the same way so that a
	 * difference between the two can only come from the conversion and never
	 * from their starting contents.
	 */

	d->in_buf2 = malloc((size_t)d->in_w * d->in_h * 3);
	if (d->in_buf2 == NULL) {
		fprintf(stderr, "detector: out of memory for the input\n");
		return -1;
	}

	memset(d->in_buf2, 128, (size_t)d->in_w * d->in_h * 3);

	for (i = 0; i < d->n_out; i++) {
		memset(&attr, 0, sizeof(attr));
		attr.index = i;
		if (d->rk.query(d->ctx, RKNN_QUERY_OUTPUT_ATTR, &attr,
				sizeof(attr)) < 0) {
			fprintf(stderr, "detector: OUTPUT_ATTR %u failed\n", i);
			return -1;
		}

		if (attr.type != RKNN_TENSOR_INT8) {
			fprintf(stderr,
				"detector: output %u is not int8; this decode"
				" dequantises affine int8 only\n", i);
			return -1;
		}

		/* That head i really is the stride the anchor table assumes.
		 *
		 * The decode pairs output i with g_yolo_stride[i] and
		 * g_yolo_anchor[i]. Nothing in the model file states that pairing,
		 * so if an export ever ordered its heads the other way every box
		 * would be decoded at the wrong scale with the wrong anchors -
		 * and the result would be plausible-looking boxes in wrong places,
		 * which is the failure mode least likely to be recognised for what
		 * it is. The grid size is the one thing that distinguishes them.
		 */

		{
			uint32_t gw = d->in_w / (uint32_t)g_yolo_stride[i];
			uint32_t gh = d->in_h / (uint32_t)g_yolo_stride[i];

			if (attr.n_dims != 4 ||
			    attr.dims[1] != YOLO_ANCHORS * YOLO_PROPS ||
			    attr.dims[2] != gh || attr.dims[3] != gw) {
				fprintf(stderr,
					"detector: output %u is %ux%ux%ux%u,"
					" expected 1x%dx%ux%u for stride %d\n",
					i, attr.dims[0], attr.dims[1],
					attr.dims[2], attr.dims[3],
					YOLO_ANCHORS * YOLO_PROPS, gh, gw,
					g_yolo_stride[i]);
				return -1;
			}
		}

		d->out_zp[i] = attr.zp;
		d->out_scale[i] = attr.scale;

		/* size_with_stride, not size: the runtime writes padded rows -
		 * 1638400 against a declared 1632000 for the first head - and a
		 * buffer sized to the smaller figure is written past its end.
		 */

		d->out_size[i] = attr.size_with_stride > attr.size ?
				 attr.size_with_stride : attr.size;
		d->out_buf[i] = malloc(d->out_size[i]);

		if (d->out_buf[i] == NULL) {
			fprintf(stderr, "detector: out of memory for output"
				" %u\n", i);
			return -1;
		}

		memset(d->out_buf[i], 0, d->out_size[i]);
	}

	if (d->rk.set_core_mask(d->ctx, (rknn_core_mask)core) < 0)
		fprintf(stderr,
			"detector: set_core_mask(%u) failed - inference will"
			" run wherever the runtime puts it\n", core);

	return 0;
}

/****************************************************************************
 * Name: det_letterbox_setup
 *
 * Description:
 *   Work out once how the capture maps into the model's square input.
 *
 *   Both the forward mapping - what the RGA is told to do - and the inverse -
 *   what the boxes go through afterwards - come from these three numbers. Keeping
 *   them in one place is the point: computing the scale twice is how the picture
 *   and the boxes end up disagreeing by a few percent, which looks like a
 *   detector that is slightly bad rather than arithmetic that is wrong.
 *
 ****************************************************************************/

static void det_letterbox_setup(struct detector *d, uint32_t sw, uint32_t sh)
{
	float sx = (float)d->in_w / (float)sw;
	float sy = (float)d->in_h / (float)sh;

	d->lb_scale = sx < sy ? sx : sy;
	d->lb_w = (uint32_t)((float)sw * d->lb_scale);
	d->lb_h = (uint32_t)((float)sh * d->lb_scale);

	/* Even padding, as the reference does. An odd remainder goes to the far
	 * side, which matters only for a pixel but matters for matching the
	 * inverse transform exactly.
	 */

	d->lb_pad_x = (d->in_w - d->lb_w) / 2;
	d->lb_pad_y = (d->in_h - d->lb_h) / 2;

	printf("detector: letterbox %ux%u -> %ux%u at %u,%u inside %ux%u"
	       " (scale %.4f)\n",
	       sw, sh, d->lb_w, d->lb_h, d->lb_pad_x, d->lb_pad_y,
	       d->in_w, d->in_h, (double)d->lb_scale);
}

/* Class names, for the log only.
 *
 * Optional on purpose: a missing labels file prints class numbers instead of
 * refusing to detect. The names never reach the remote - the transport carries
 * the class index - so this is a readability aid for whoever is watching the
 * console, not part of the pipeline.
 */

static char g_yolo_label[YOLO_CLASSES][24];
static bool g_yolo_labelled;

static void det_load_labels(const char *path)
{
	char line[64];
	FILE *fp;
	int i = 0;

	fp = fopen(path, "r");
	if (fp == NULL) {
		printf("detector: %s: %s - the log will show class numbers\n",
		       path, strerror(errno));
		return;
	}

	while (i < YOLO_CLASSES && fgets(line, sizeof(line), fp) != NULL) {
		size_t n = strcspn(line, "\r\n");

		line[n] = '\0';
		if (n == 0)
			continue;

		/* Truncation is intended and stated in the format, rather than
		 * left to snprintf: a plain %s here makes the compiler warn about
		 * a 64-byte line reaching a 24-byte field, and silencing that by
		 * widening the field would be responding to the diagnostic
		 * instead of to what it is pointing at. No COCO name is close to
		 * this long; a longer one is a wrong file, and a clipped name in
		 * the log is the right way to find that out.
		 */

		snprintf(g_yolo_label[i], sizeof(g_yolo_label[i]), "%.*s",
			 (int)(sizeof(g_yolo_label[i]) - 1), line);
		i++;
	}

	fclose(fp);

	if (i == YOLO_CLASSES) {
		g_yolo_labelled = true;
	} else {
		printf("detector: %s has %d names, expected %d - the log will"
		       " show class numbers\n", path, i, YOLO_CLASSES);
	}
}

static const char *det_class_name(int cls, char *tmp, size_t len)
{
	if (g_yolo_labelled && cls >= 0 && cls < YOLO_CLASSES)
		return g_yolo_label[cls];

	snprintf(tmp, len, "cls%d", cls);
	return tmp;
}

/****************************************************************************
 * Name: det_dump
 *
 * Description:
 *   Write out what the camera produced and what the model was given.
 *
 *   This exists because two rounds of this task were spent inferring image
 *   quality from detection statistics, and both inferences were wrong. Counting
 *   boxes cannot separate a degraded image from a harder scene from a threshold
 *   set too high: they all show up as fewer boxes. One look at the actual pixels
 *   answers in seconds what a day of arithmetic could not.
 *
 *   Both ends, deliberately. The capture goes out as the Y plane in PGM and the
 *   model input as PPM, so the fault is localised to before or after the RGA
 *   rather than merely established to exist. A sane PGM with a wrong PPM is the
 *   conversion; a wrong PGM is the sensor, the ISP or the scene.
 *
 *   Greyscale for the source because Y alone answers framing, exposure and
 *   focus, and writing it needs no colour conversion - so this cannot fail in
 *   the same way the thing it is diagnosing might.
 *
 *   PPM and PGM because they need no library and every viewer opens them. A
 *   diagnostic that needs tooling to read is one that gets skipped.
 *
 ****************************************************************************/

static volatile sig_atomic_t g_det_dump;

static void on_dump_signal(int sig)
{
	(void)sig;
	g_det_dump = 1;
}

static void det_dump(struct detector *d, const uint8_t *nv12, uint32_t seq)
{
	char path[256];
	FILE *fp;
	uint32_t y;

	snprintf(path, sizeof(path), "%s-%03u-src.pgm", d->dump, seq);
	fp = fopen(path, "wb");

	if (fp == NULL) {
		fprintf(stderr, "detector: %s: %s\n", path, strerror(errno));
		return;
	}

	fprintf(fp, "P5\n%u %u\n255\n", d->cam->src_w, d->cam->src_h);

	/* Row at a time, because the capture is padded: bytesperline is not
	 * reliably the width, and writing it as one block would shear the image
	 * and invent a defect that is not there.
	 */

	for (y = 0; y < d->cam->src_h; y++)
		fwrite(nv12 + (size_t)y * d->cam->src_ystride, 1,
		       d->cam->src_w, fp);

	fclose(fp);

	/* The whole NV12 as well, raw.
	 *
	 * The PGM above answers framing, exposure and focus, which is what it was
	 * added for. It cannot answer which matrix and range the RGA used to decode
	 * the colour, because that needs the chroma - and that question came up
	 * immediately afterwards, with the ISP reporting full-range YUV and the RGA
	 * left on IM_COLOR_SPACE_DEFAULT. With both planes the conversion can be
	 * recomputed offline under each candidate and compared against the PPM,
	 * which settles it without another A/B run on the board.
	 *
	 * Written with the padding removed so the file is exactly w*h*3/2 and any
	 * tool can read it as plain NV12.
	 */

	snprintf(path, sizeof(path), "%s-%03u-src.nv12", d->dump, seq);
	fp = fopen(path, "wb");

	if (fp != NULL) {
		for (y = 0; y < d->cam->src_h; y++)
			fwrite(nv12 + (size_t)y * d->cam->src_ystride, 1,
			       d->cam->src_w, fp);

		for (y = 0; y < d->cam->src_h / 2; y++)
			fwrite(nv12 + (size_t)d->cam->src_ystride *
			       d->cam->src_h + (size_t)y * d->cam->src_ystride,
			       1, d->cam->src_w, fp);

		fclose(fp);
	}

	snprintf(path, sizeof(path), "%s-%03u-in.ppm", d->dump, seq);
	fp = fopen(path, "wb");

	if (fp == NULL) {
		fprintf(stderr, "detector: %s: %s\n", path, strerror(errno));
		return;
	}

	fprintf(fp, "P6\n%u %u\n255\n", d->in_w, d->in_h);
	fwrite(d->in_buf, 1, (size_t)d->in_w * d->in_h * 3, fp);
	fclose(fp);

	printf("detector: dumped %s-%03u-{src.pgm,src.nv12,in.ppm}\n",
	       d->dump, seq);
}

/****************************************************************************
 * Name: det_import_buffers
 *
 * Description:
 *   Export each capture buffer as a dma-buf and import it, along with both model
 *   input buffers, into the RGA driver once.
 *
 *   All or nothing. A partial import would leave some frames on the fast path and
 *   some on the slow one, and a mean over a mixture of two different operations
 *   is the sort of number that sends a diagnosis in the wrong direction.
 *
 ****************************************************************************/

static int det_import_buffers(struct detector *d)
{
	struct camera *c = d->cam;
	im_handle_param_t param;
	unsigned int i;

	if (d->rga.import_fd == NULL || d->rga.import_va == NULL ||
	    d->rga.wrap_handle == NULL || d->rga.release_handle == NULL) {
		printf("detector: librga has no buffer import - conversion will"
		       " map pages on every call\n");
		return -1;
	}

	/* Zero, not -1, means "no descriptor": the detector struct is zeroed when
	 * it is created and det_release() can be reached without this function
	 * having run, so any other sentinel risks closing fd 0.
	 */

	for (i = 0; i < CAM_MAX_BUFFERS; i++)
		d->cam_dmafd[i] = 0;

	memset(&param, 0, sizeof(param));
	param.width = d->in_w;
	param.height = d->in_h;
	param.format = RK_FORMAT_RGB_888;

	d->in_rgah = d->rga.import_va(d->in_buf, &param);
	d->in2_rgah = d->rga.import_va(d->in_buf2, &param);

	if (d->in_rgah == 0 || d->in2_rgah == 0) {
		printf("detector: importing the model input failed -"
		       " conversion will map pages on every call\n");
		goto undo;
	}

	/* Strides, not visible sizes. The ISP's row pitch is not reliably the
	 * width - S_FMT is read back precisely because the driver clamps silently.
	 */

	memset(&param, 0, sizeof(param));
	param.width = c->src_ystride;
	param.height = c->src_h;
	param.format = RK_FORMAT_YCbCr_420_SP;

	for (i = 0; i < c->nbuffers; i++) {
		struct v4l2_exportbuffer exp;

		memset(&exp, 0, sizeof(exp));
		exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		exp.index = i;
		exp.plane = 0;
		exp.flags = O_CLOEXEC;

		if (ioctl(c->fd, VIDIOC_EXPBUF, &exp) < 0) {
			printf("detector: EXPBUF %u: %s - conversion will map"
			       " pages on every call\n", i, strerror(errno));
			goto undo;
		}

		d->cam_dmafd[i] = exp.fd;
		d->cam_rgah[i] = d->rga.import_fd(exp.fd, &param);

		if (d->cam_rgah[i] == 0) {
			printf("detector: importing capture buffer %u failed -"
			       " conversion will map pages on every call\n", i);
			goto undo;
		}
	}

	printf("detector: %u capture buffers imported as dma-buf, checking the"
	       " conversion against the mapped path\n", c->nbuffers);

	return 0;

undo:
	for (i = 0; i < CAM_MAX_BUFFERS; i++) {
		if (d->cam_rgah[i] != 0)
			d->rga.release_handle(d->cam_rgah[i]);

		d->cam_rgah[i] = 0;

		if (d->cam_dmafd[i] > 0)
			close(d->cam_dmafd[i]);

		d->cam_dmafd[i] = 0;
	}

	if (d->in_rgah != 0)
		d->rga.release_handle(d->in_rgah);

	if (d->in2_rgah != 0)
		d->rga.release_handle(d->in2_rgah);

	d->in_rgah = 0;
	d->in2_rgah = 0;

	return -1;
}

/****************************************************************************
 * Name: det_rga_into
 *
 * Description:
 *   Scale and colour-convert one captured frame into the model's input, in
 *   hardware, with the addressing mode and the destination given by the caller.
 *
 *   This is the one place in this program where the RGA earns its keep. The
 *   display path deliberately does not use it: there the CPU costs 3.9ms against
 *   the RGA's 0.6ms, a tenth of one core, and collecting it would mean making the
 *   RGA write into a carveout mapped Device-nGnRnE. Here the alternative is
 *   1280x720 NV12 resampled to 640x640 RGB888, a different order of work, and the
 *   destination is ordinary memory with none of the carveout's problems.
 *
 *   Only the letterboxed centre is written. The border was filled once at setup
 *   and never changes, so repainting it thirty times a second would be a second
 *   hardware operation per frame for a constant.
 *
 *   Parameterised so the self-check can run both paths over the same frame. Both
 *   ends switch together and never separately: handle mode is a property of the
 *   whole request rather than of one buffer, and mixing a handle with a virtual
 *   address was measured returning IM_STATUS_FAILED on every single call.
 *
 ****************************************************************************/

static int det_rga_into(struct detector *d, const uint8_t *nv12, uint32_t idx,
			uint8_t *dstbuf, rga_buffer_handle_t dsth,
			bool use_handle)
{
	rga_buffer_t src;
	rga_buffer_t dst;
	rga_buffer_t pat;
	im_rect srect;
	im_rect drect;
	im_rect prect;
	IM_STATUS st;

	memset(&pat, 0, sizeof(pat));
	memset(&prect, 0, sizeof(prect));

	/* The source stride is the ISP's, not the width. S_FMT is read back
	 * because the driver clamps sizes without saying so, which means the two
	 * are not reliably equal - and using the width here would shear the image
	 * in a way that still looks like a picture.
	 */

	if (use_handle) {
		src = d->rga.wrap_handle(d->cam_rgah[idx], (int)d->cam->src_w,
					 (int)d->cam->src_h,
					 (int)d->cam->src_ystride,
					 (int)d->cam->src_h,
					 RK_FORMAT_YCbCr_420_SP);

		dst = d->rga.wrap_handle(dsth, (int)d->in_w, (int)d->in_h,
					 (int)d->in_w, (int)d->in_h,
					 RK_FORMAT_RGB_888);
	} else {
		src = d->rga.wrap((void *)nv12, (int)d->cam->src_w,
				  (int)d->cam->src_h,
				  (int)d->cam->src_ystride, (int)d->cam->src_h,
				  RK_FORMAT_YCbCr_420_SP);

		dst = d->rga.wrap(dstbuf, (int)d->in_w, (int)d->in_h,
				  (int)d->in_w, (int)d->in_h,
				  RK_FORMAT_RGB_888);
	}

	/* Nothing sets color_space_mode here, and that is a finding rather than an
	 * omission.
	 *
	 * The RGA decodes this as BT.601 limited range. That was not read out of a
	 * document - it was established by dumping a frame and its converted output,
	 * converting the frame offline under all four candidate matrices, and finding
	 * BT.601 limited matching to a mean absolute error of 0.40 levels while the
	 * next candidate was three times worse.
	 *
	 * The ISP emits full range (rkisp.c:4914), so there is a genuine mismatch:
	 * a limited-range decode of full-range data applies a gain of 255/219 and an
	 * offset of -18.6, clipping the top 2 percent of pixels and stretching
	 * contrast by 16 percent. Neither end can be moved from here:
	 *
	 *   - Setting src.color_space_mode is ignored by this improcess() path. That
	 *     was measured the same way: a run with IM_YUV_TO_RGB_BT601_FULL still
	 *     decoded as limited. An option to set it existed briefly and was
	 *     removed, because an option that does nothing is worse than none.
	 *
	 *   - Asking the ISP for limited range does take on the subdev - see
	 *     amp_isp_range - but does not reach the detect stream. In one run the
	 *     display node reported limited and the detect node full, and the
	 *     captured Y still reached 247, above the limited-range ceiling of 235.
	 *     The value is read at three different moments by three different pieces
	 *     of the driver and rkaiq_3A rewrites it in between, so making it stick
	 *     means winning a race against a daemon we do not control.
	 *
	 * Left as it is deliberately. The measured cost is small next to what
	 * matching the reference's padding recovered - peak person confidence went
	 * from 69 to 89 percent on that change alone - and both remaining routes are
	 * worse than the problem.
	 */

	srect.x = 0;
	srect.y = 0;
	srect.width = (int)d->cam->src_w;
	srect.height = (int)d->cam->src_h;

	drect.x = (int)d->lb_pad_x;
	drect.y = (int)d->lb_pad_y;
	drect.width = (int)d->lb_w;
	drect.height = (int)d->lb_h;

	st = d->rga.process(src, dst, pat, srect, drect, prect, IM_SYNC);
	if (st != IM_STATUS_SUCCESS) {
		fprintf(stderr, "detector: RGA failed (%d)%s\n", (int)st,
			use_handle ? " on the dma-buf path" : "");
		return -1;
	}

	return 0;
}

/****************************************************************************
 * Name: det_verify_zerocopy
 *
 * Description:
 *   Convert one frame both ways and compare the results byte for byte.
 *
 *   This is the arbiter for the whole optimisation, and it is deliberately not a
 *   detection count. Counting boxes conflates the model, the scene, the lens and
 *   the confidence threshold: an earlier attempt at exactly this question was
 *   decided by box counts that turned out to be dominated by a defocused lens,
 *   and the conclusion drawn from them was wrong twice over. Two buffers and a
 *   comparison have none of those confounds - the same frame either converts to
 *   the same pixels or it does not, whatever the lens is doing.
 *
 *   Run over many frames rather than one, because the failure being looked for is
 *   stale cache lines and staleness depends on what happens to be resident. A
 *   single agreement would prove nothing.
 *
 *   The concern is specific: in_buf is ordinary cached memory, the RGA writes it
 *   by DMA, and the CPU reads it immediately afterwards when rknn_inputs_set()
 *   marshals the input. Something has to invalidate the CPU's copy in between.
 *   With a per-call wrap the driver does it while importing; whether a persistent
 *   import still does is an implementation detail of a closed library, and librga
 *   offers no way to request cache maintenance on its own.
 *
 ****************************************************************************/

static void det_verify_zerocopy(struct detector *d, const uint8_t *nv12,
				uint32_t idx)
{
	size_t len = (size_t)d->in_w * d->in_h * 3;
	size_t diff = 0;
	size_t i;

	/* The mapped path into in_buf first: that is the reference, and it is also
	 * what this frame's inference will use while the check is still running.
	 */

	if (det_rga_into(d, nv12, idx, d->in_buf, 0, false) < 0)
		return;

	if (det_rga_into(d, nv12, idx, d->in_buf2, d->in2_rgah, true) < 0) {
		printf("detector: the dma-buf conversion failed outright -"
		       " staying on the mapped path\n");
		d->zerocopy_checked = true;
		d->zerocopy = false;
		return;
	}

	for (i = 0; i < len; i++)
		if (d->in_buf[i] != d->in_buf2[i])
			diff++;

	if (diff > 0) {
		d->verify_bad++;

		if (diff > d->verify_maxdiff)
			d->verify_maxdiff = diff;
	}

	d->verify_frames++;

	if (d->verify_frames < 60)
		return;

	d->zerocopy_checked = true;

	if (d->verify_bad == 0) {
		d->zerocopy = true;

		printf("detector: dma-buf conversion matches the mapped path on"
		       " %u frames - using it\n", d->verify_frames);
	} else {
		d->zerocopy = false;

		printf("detector: dma-buf conversion differs on %u of %u"
		       " frames, worst %zu of %zu bytes - staying on the mapped"
		       " path\n", d->verify_bad, d->verify_frames,
		       d->verify_maxdiff, len);
		printf("detector:   cache maintenance is the likely reason; the"
		       " fix is a dma-heap destination plus DMA_BUF_IOCTL_SYNC,"
		       " not this\n");
	}
}


/****************************************************************************
 * Name: det_infer
 *
 * Description:
 *   Hand the prepared input to the NPU and collect the three heads.
 *
 *   The outputs are asked for as raw int8, want_float clear, because the decode
 *   below dequantises only the few values that pass the confidence threshold.
 *   Letting the runtime convert all 2.1 million to float first would be work
 *   spent on numbers that are about to be discarded.
 *
 ****************************************************************************/

static int det_infer(struct detector *d)
{
	rknn_output out[YOLO_HEADS];
	rknn_input in;
	uint32_t i;
	int ret;

	memset(&in, 0, sizeof(in));
	in.index = 0;
	in.type = RKNN_TENSOR_UINT8;
	in.fmt = RKNN_TENSOR_NHWC;
	in.size = d->in_w * d->in_h * 3;
	in.buf = d->in_buf;

	/* pass_through clear, so the runtime applies the model's own input
	 * quantisation. Setting it would move that arithmetic here in exchange for
	 * nothing - it is 645us either way, and the runtime's version cannot
	 * disagree with the model about zero point and scale.
	 */

	in.pass_through = 0;

	ret = d->rk.inputs_set(d->ctx, 1, &in);
	if (ret < 0) {
		fprintf(stderr, "detector: inputs_set: %d\n", ret);
		return -1;
	}

	ret = d->rk.run(d->ctx, NULL);
	if (ret < 0) {
		fprintf(stderr, "detector: run: %d\n", ret);
		return -1;
	}

	memset(out, 0, sizeof(out));
	for (i = 0; i < d->n_out; i++) {
		out[i].index = i;
		out[i].want_float = 0;
		out[i].is_prealloc = 1;
		out[i].buf = d->out_buf[i];
		out[i].size = d->out_size[i];
	}

	ret = d->rk.outputs_get(d->ctx, d->n_out, out, NULL);
	if (ret < 0) {
		fprintf(stderr, "detector: outputs_get: %d\n", ret);
		return -1;
	}

	/* Released even though the buffers are ours. The header is explicit that
	 * with is_prealloc set it will not free them, so this is here for whatever
	 * internal bookkeeping a get creates, not for the memory.
	 */

	d->rk.outputs_release(d->ctx, d->n_out, out);

	return 0;
}

/****************************************************************************
 * Name: yolo_decode
 *
 * Description:
 *   Turn one output head into candidate boxes.
 *
 *   The threshold is compared in the quantised domain, which is the whole reason
 *   this is affordable on a CPU. Converting every one of 255x80x80 int8 values to
 *   float and then comparing would be 1.6 million conversions for the first head
 *   alone; mapping the threshold into int8 once makes the common case a single
 *   byte compare, and only survivors get dequantised.
 *
 *   No sigmoid anywhere, which looks wrong against the yolov5 paper and is not:
 *   this export has it baked in, so the dequantised values already are
 *   probabilities. Applying it again would squash every score towards 0.5 and
 *   quietly halve the detection rate - a failure that would look like a weak
 *   model rather than a bug.
 *
 ****************************************************************************/

static uint32_t yolo_decode(struct detector *d, uint32_t head, uint32_t got,
			    float thresh)
{
	const int8_t *in = d->out_buf[head];
	const int *anchor = g_yolo_anchor[head];
	int stride = g_yolo_stride[head];
	int32_t zp = d->out_zp[head];
	float scale = d->out_scale[head];
	uint32_t grid_w = d->in_w / (uint32_t)stride;
	uint32_t grid_h = d->in_h / (uint32_t)stride;
	uint32_t grid_len = grid_w * grid_h;
	int8_t thres_i8;
	float q;
	uint32_t a;
	uint32_t i;
	uint32_t j;

	/* The threshold, mapped into int8 exactly as the model's own values were.
	 * Clipped because a small scale can put it outside the representable
	 * range, and wrapping would silently turn a high threshold into a low one.
	 */

	q = thresh / scale + (float)zp;
	if (q < -128.0f)
		q = -128.0f;
	if (q > 127.0f)
		q = 127.0f;

	thres_i8 = (int8_t)q;

	for (a = 0; a < YOLO_ANCHORS; a++) {
		for (i = 0; i < grid_h; i++) {
			for (j = 0; j < grid_w; j++) {
				const int8_t *p;
				uint32_t base;
				int8_t obj;
				int8_t best;
				int bestk;
				int k;
				float bx;
				float by;
				float bw;
				float bh;

				base = (YOLO_PROPS * a) * grid_len +
				       i * grid_w + j;

				obj = in[base + 4 * grid_len];
				if (obj < thres_i8)
					continue;

				p = in + base;

				/* Best class before any box arithmetic, so a cell
				 * whose objectness passed but whose every class
				 * failed costs only the scan.
				 */

				best = p[5 * grid_len];
				bestk = 0;

				for (k = 1; k < YOLO_CLASSES; k++) {
					int8_t v = p[(5 + k) * grid_len];

					if (v > best) {
						best = v;
						bestk = k;
					}
				}

				if (best <= thres_i8)
					continue;

				if (got >= YOLO_MAX_CAND)
					return got;

				bx = ((float)p[0] - (float)zp) * scale;
				by = ((float)p[grid_len] - (float)zp) * scale;
				bw = ((float)p[2 * grid_len] - (float)zp) *
				     scale;
				bh = ((float)p[3 * grid_len] - (float)zp) *
				     scale;

				bx = (bx * 2.0f - 0.5f + (float)j) *
				     (float)stride;
				by = (by * 2.0f - 0.5f + (float)i) *
				     (float)stride;

				bw = bw * 2.0f;
				bh = bh * 2.0f;
				bw = bw * bw * (float)anchor[a * 2];
				bh = bh * bh * (float)anchor[a * 2 + 1];

				d->cand[got].x = bx - bw / 2.0f;
				d->cand[got].y = by - bh / 2.0f;
				d->cand[got].w = bw;
				d->cand[got].h = bh;
				d->cand[got].cls = bestk;
				d->cand[got].keep = true;
				d->cand[got].score =
					(((float)best - (float)zp) * scale) *
					(((float)obj - (float)zp) * scale);
				got++;
			}
		}
	}

	return got;
}

/****************************************************************************
 * Name: yolo_cmp
 ****************************************************************************/

static int yolo_cmp(const void *a, const void *b)
{
	const struct yolo_cand *x = a;
	const struct yolo_cand *y = b;

	if (x->score > y->score)
		return -1;
	if (x->score < y->score)
		return 1;

	return 0;
}

/****************************************************************************
 * Name: yolo_nms
 *
 * Description:
 *   Suppress overlapping boxes of the same class, highest score first.
 *
 *   One pass with a class comparison, rather than the reference's loop over all
 *   80 classes calling a suppressor for each. The result is identical -
 *   suppression only ever happens between boxes of the same class, so a single
 *   ordered pass that skips mismatched pairs does exactly the same work - and it
 *   avoids 80 traversals of a list that usually holds a handful of entries.
 *
 ****************************************************************************/

static uint32_t yolo_nms(struct detector *d, uint32_t n, float thresh)
{
	uint32_t kept = 0;
	uint32_t i;
	uint32_t j;

	qsort(d->cand, n, sizeof(d->cand[0]), yolo_cmp);

	for (i = 0; i < n; i++) {
		float x0;
		float y0;
		float x1;
		float y1;
		float a0;

		if (!d->cand[i].keep)
			continue;

		kept++;

		x0 = d->cand[i].x;
		y0 = d->cand[i].y;
		x1 = x0 + d->cand[i].w;
		y1 = y0 + d->cand[i].h;

		/* Areas and overlaps carry the reference's +1, which treats the
		 * coordinates as inclusive pixel indices - a box from 10 to 20
		 * covering eleven pixels rather than ten.
		 *
		 * For continuous box coordinates that is arguably wrong, and it is
		 * kept anyway, because it is not independent of the threshold
		 * above it. The +1 inflates every IoU slightly, most of all for
		 * small boxes, and 0.45 was chosen against inflated numbers.
		 * Dropping the +1 while keeping 0.45 silently makes suppression
		 * less aggressive, which shows up as duplicate boxes on one
		 * object - and it was measured: without this, three of twenty
		 * random tensors produced a different set of survivors than the
		 * reference. Changing the convention is a retune, not a cleanup.
		 */

		a0 = (x1 - x0 + 1.0f) * (y1 - y0 + 1.0f);

		for (j = i + 1; j < n; j++) {
			float jx0;
			float jy0;
			float jx1;
			float jy1;
			float ix;
			float iy;
			float inter;
			float uni;

			if (!d->cand[j].keep ||
			    d->cand[j].cls != d->cand[i].cls)
				continue;

			jx0 = d->cand[j].x;
			jy0 = d->cand[j].y;
			jx1 = jx0 + d->cand[j].w;
			jy1 = jy0 + d->cand[j].h;

			ix = fminf(x1, jx1) - fmaxf(x0, jx0) + 1.0f;
			iy = fminf(y1, jy1) - fmaxf(y0, jy0) + 1.0f;

			if (ix <= 0.0f || iy <= 0.0f)
				continue;

			inter = ix * iy;
			uni = a0 + (jx1 - jx0 + 1.0f) * (jy1 - jy0 + 1.0f) -
			      inter;

			if (uni > 0.0f && inter / uni > thresh)
				d->cand[j].keep = false;
		}
	}

	return kept;
}

/****************************************************************************
 * Name: det_emit
 *
 * Description:
 *   Map surviving boxes out of the model's letterboxed square into the published
 *   frame's coordinates.
 *
 *   Two transforms collapsed into one factor. The boxes are in 640x640 model
 *   pixels and the remote needs them in the 512x288 frame it is drawing. Going
 *   via the 1280x720 capture would mean two multiplies and two roundings, and the
 *   letterboxed image width is by construction exactly what the published width
 *   maps to, so a single factor covers both stages and there is no intermediate
 *   to round twice.
 *
 *   Clamped to the letterboxed image rather than to the whole model input. The
 *   reference clamps to model_in_w/h, which in a padded dimension lets a box
 *   reach into the border and emerge beyond the source height: 640 clamped then
 *   divided by 0.5 gives 1280 where the capture is only 720 tall. Nothing is ever
 *   detected in the padding so this costs nothing, and a box hanging off the
 *   bottom of the frame is exactly the sort of thing that would be blamed on the
 *   remote's drawing code.
 *
 ****************************************************************************/

static uint32_t det_emit(struct detector *d, uint32_t n,
			 struct amp_det_box *box)
{
	float kx = (float)d->width / (float)d->lb_w;
	float ky = (float)d->height / (float)d->lb_h;
	uint32_t out = 0;
	uint32_t i;

	for (i = 0; i < n && out < AMP_DET_MAXBOX; i++) {
		float x0;
		float y0;
		float x1;
		float y1;
		float s;

		if (!d->cand[i].keep)
			continue;

		x0 = d->cand[i].x - (float)d->lb_pad_x;
		y0 = d->cand[i].y - (float)d->lb_pad_y;
		x1 = x0 + d->cand[i].w;
		y1 = y0 + d->cand[i].h;

		x0 = fmaxf(0.0f, fminf(x0, (float)d->lb_w));
		y0 = fmaxf(0.0f, fminf(y0, (float)d->lb_h));
		x1 = fmaxf(0.0f, fminf(x1, (float)d->lb_w));
		y1 = fmaxf(0.0f, fminf(y1, (float)d->lb_h));

		x0 *= kx;
		y0 *= ky;
		x1 *= kx;
		y1 *= ky;

		/* Anything that clamped away to nothing is dropped rather than
		 * published as a zero-sized box. The remote would draw a dot,
		 * which reads as a broken detector rather than as a box that was
		 * entirely off-frame.
		 */

		if (x1 - x0 < 1.0f || y1 - y0 < 1.0f)
			continue;

		s = d->cand[i].score * 100.0f;
		if (s < 0.0f)
			s = 0.0f;
		if (s > 100.0f)
			s = 100.0f;

		/* Compared after the conversion to whole percent, so what gets
		 * dropped is exactly the number that would have been published.
		 * Filtering the float instead would let a box the log calls 35%
		 * be rejected by a threshold of 35.
		 */

		if ((uint32_t)s < d->min_score) {
			d->filtered++;
			continue;
		}

		box[out].x = (uint16_t)x0;
		box[out].y = (uint16_t)y0;
		box[out].w = (uint16_t)(x1 - x0);
		box[out].h = (uint16_t)(y1 - y0);
		box[out].cls = (uint8_t)d->cand[i].cls;
		box[out].score = (uint8_t)s;
		box[out].reserved = 0;
		out++;
	}

	return out;
}

/****************************************************************************
 * Name: det_real
 *
 * Description:
 *   One captured frame, all the way to a set of boxes.
 *
 *   Every phase timed separately. That is not thoroughness for its own sake: on
 *   this task a single total has already pointed the diagnosis at the wrong
 *   component twice, and the budget here is a sum of parts with very different
 *   sizes - 2.7ms of RGA, 18ms of NPU including input marshalling, and a
 *   post-processing pass whose cost is unknown until it runs. If the frame rate
 *   ever disappoints, only the split says which one moved.
 *
 ****************************************************************************/

static uint32_t det_real(struct detector *d, struct amp_det_box *box,
			 const uint8_t *nv12, uint32_t idx)
{
	struct timespec t0;
	struct timespec t1;
	struct timespec t2;
	struct timespec t3;
	uint32_t cand = 0;
	uint32_t out;
	uint32_t i;
	long us;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	/* While the check is running the frame is converted twice, so the RGA
	 * timing for those frames is meaningless - which is why the check is
	 * bounded and reports once rather than running forever.
	 */

	if (d->zerocopy_want && !d->zerocopy_checked) {
		det_verify_zerocopy(d, nv12, idx);
	} else if (det_rga_into(d, nv12, idx, d->in_buf, d->in_rgah,
				d->zerocopy) < 0)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &t1);

	if (det_infer(d) < 0)
		return 0;

	clock_gettime(CLOCK_MONOTONIC, &t2);

	for (i = 0; i < d->n_out; i++)
		cand = yolo_decode(d, i, cand, YOLO_BOX_THRESH);

	yolo_nms(d, cand, YOLO_NMS_THRESH);

	out = det_emit(d, cand, box);

	clock_gettime(CLOCK_MONOTONIC, &t3);

	/* Dumped after the timing is taken, so writing two files to storage does
	 * not appear as a conversion that suddenly cost 30ms.
	 *
	 * One automatic dump a couple of seconds in, so there is always something
	 * to look at, and then one per SIGUSR1 - because the frame worth seeing is
	 * the one where a person was in view and no box appeared, and only whoever
	 * is looking at the screen knows when that was.
	 */

	if (d->dump != NULL) {
		bool now = g_det_dump != 0;

		if (d->inferences == 60 && d->dumps == 0)
			now = true;

		if (now) {
			g_det_dump = 0;
			det_dump(d, nv12, ++d->dumps);
		}
	}

	/* inputs_set and run are not split here even though the probe split them:
	 * rknn_run is synchronous and the two are adjacent, so the pair is what a
	 * caller could act on. The RGA and the post-processing are the halves worth
	 * separating, because they are the two that could actually be removed - one
	 * by a different capture format, the other by the zero-copy API.
	 */

	us = (t1.tv_sec - t0.tv_sec) * 1000000 +
	     (t1.tv_nsec - t0.tv_nsec) / 1000;
	d->rga_us_sum += (uint64_t)us;
	if ((uint32_t)us > d->rga_us_max)
		d->rga_us_max = (uint32_t)us;

	us = (t2.tv_sec - t1.tv_sec) * 1000000 +
	     (t2.tv_nsec - t1.tv_nsec) / 1000;
	d->run_us_sum += (uint64_t)us;
	if ((uint32_t)us > d->run_us_max)
		d->run_us_max = (uint32_t)us;

	us = (t3.tv_sec - t2.tv_sec) * 1000000 +
	     (t3.tv_nsec - t2.tv_nsec) / 1000;
	d->post_us_sum += (uint64_t)us;
	if ((uint32_t)us > d->post_us_max)
		d->post_us_max = (uint32_t)us;

	d->inferences++;
	d->cands_max = cand > d->cands_max ? cand : d->cands_max;

	if (out > d->boxes_max)
		d->boxes_max = out;

	return out;
}

/****************************************************************************
 * Name: det_capture
 *
 * Description:
 *   Take the newest frame from the detector's own stream, run it, and put the
 *   buffer back.
 *
 *   The newest, not the oldest. This thread runs at a third of the capture rate,
 *   so everything queued behind the latest frame is stale by definition. Draining
 *   to the end and using the last one keeps the boxes as current as the NPU
 *   allows; taking them in order would build a lag that grows without bound and
 *   looks like a slow detector rather than a backed-up queue.
 *
 ****************************************************************************/

static uint32_t det_capture(struct detector *d, struct amp_det_box *box)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct camera *c = d->cam;
	struct v4l2_buffer newest;
	struct v4l2_plane nplanes[VIDEO_MAX_PLANES];
	bool have = false;
	uint32_t count = 0;

	for (; ; ) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.m.planes = planes;
		buf.length = VIDEO_MAX_PLANES;

		if (ioctl(c->fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EAGAIN)
				break;

			if (errno == EINTR) {
				if (d->stop || g_stop)
					break;

				continue;
			}

			fprintf(stderr, "%s: DQBUF: %s\n", c->name,
				strerror(errno));
			break;
		}

		if (c->v4l2_seq_valid && buf.sequence != c->v4l2_seq + 1) {
			c->gaps++;
			c->gap_frames += buf.sequence - c->v4l2_seq - 1;
		}

		c->v4l2_seq = buf.sequence;
		c->v4l2_seq_valid = true;
		c->published++;

		/* A superseded frame goes straight back, so the ISP keeps its
		 * ring full while this one is being worked on.
		 */

		if (have && ioctl(c->fd, VIDIOC_QBUF, &newest) < 0)
			fprintf(stderr, "%s: QBUF: %s\n", c->name,
				strerror(errno));

		memcpy(&newest, &buf, sizeof(newest));
		memcpy(nplanes, planes, sizeof(nplanes));
		newest.m.planes = nplanes;
		have = true;
	}

	if (!have)
		return 0;

	if (newest.index < c->nbuffers)
		count = det_real(d, box, c->buf[newest.index].start,
				 newest.index);

	if (ioctl(c->fd, VIDIOC_QBUF, &newest) < 0)
		fprintf(stderr, "%s: QBUF: %s\n", c->name, strerror(errno));

	return count;
}

/****************************************************************************
 * Name: det_release
 *
 * Description:
 *   Give back everything the detector acquired.
 *
 *   Written to be safe to call twice, and called from the setup failure path as
 *   well as from the shutdown one. Setup can fail after the context exists and
 *   after ten megabytes of tensor buffers have been allocated, and that path does
 *   not reach det_stop() - the thread never started, so there is nothing to stop.
 *   Leaving the NPU context open in particular is worse than leaking the memory:
 *   the process stays alive afterwards, drawing camera frames, still holding a
 *   core that nothing else can then use.
 *
 ****************************************************************************/

static void det_release(struct detector *d)
{
	uint32_t i;

	/* RGA handles before the memory they describe, and before dlclose(). A
	 * handle outliving its buffer would leave the driver holding a mapping onto
	 * pages this process no longer owns.
	 */

	for (i = 0; i < CAM_MAX_BUFFERS; i++) {
		if (d->cam_rgah[i] != 0 && d->rga.release_handle != NULL)
			d->rga.release_handle(d->cam_rgah[i]);

		d->cam_rgah[i] = 0;

		if (d->cam_dmafd[i] > 0)
			close(d->cam_dmafd[i]);

		d->cam_dmafd[i] = 0;
	}

	if (d->rga.release_handle != NULL) {
		if (d->in_rgah != 0)
			d->rga.release_handle(d->in_rgah);

		if (d->in2_rgah != 0)
			d->rga.release_handle(d->in2_rgah);
	}

	d->in_rgah = 0;
	d->in2_rgah = 0;
	d->zerocopy = false;

	free(d->in_buf2);
	d->in_buf2 = NULL;

	if (d->ctx != 0) {
		d->rk.destroy(d->ctx);
		d->ctx = 0;
	}

	free(d->model);
	d->model = NULL;

	free(d->in_buf);
	d->in_buf = NULL;

	for (i = 0; i < YOLO_HEADS; i++) {
		free(d->out_buf[i]);
		d->out_buf[i] = NULL;
	}

	/* The libraries last, once every pointer into them is gone. */

	if (d->rga.lib != NULL) {
		dlclose(d->rga.lib);
		d->rga.lib = NULL;
	}

	if (d->rk.lib != NULL) {
		dlclose(d->rk.lib);
		d->rk.lib = NULL;
	}
}

#endif /* AMP_WITH_RKNN */

/****************************************************************************
 * Name: det_synth
 *
 * Description:
 *   Two boxes that move, so that both the transport and its latency are visible.
 *
 *   Moving rather than static on purpose: a static box proves bytes arrive, but a
 *   moving one also shows how far behind the boxes are, which is the one
 *   characteristic of this design that has to be seen to be judged. One box
 *   sweeps horizontally and the other is fixed, so a stuck transport and a stuck
 *   detector look different - if the sweeping box freezes while the fixed one is
 *   still drawn, publishing stopped rather than the remote's reading.
 *
 ****************************************************************************/

static uint32_t det_synth(struct detector *d, struct amp_det_box *box,
			  uint32_t tick)
{
	uint32_t bw = d->width / 6;
	uint32_t bh = d->height / 2;
	uint32_t span;
	uint32_t pos;

	if (bw == 0 || bh == 0)
		return 0;

	span = d->width - bw;
	pos = tick % (2 * span);
	if (pos >= span)
		pos = 2 * span - pos;   /* back and forth, not a jump */

	box[0].x = (uint16_t)pos;
	box[0].y = (uint16_t)((d->height - bh) / 2);
	box[0].w = (uint16_t)bw;
	box[0].h = (uint16_t)bh;
	box[0].cls = 0;                 /* person, as a real detection would be */
	box[0].score = (uint8_t)(60 + (tick % 40));
	box[0].reserved = 0;

	box[1].x = (uint16_t)(d->width - bw - 2);
	box[1].y = 2;
	box[1].w = (uint16_t)bw;
	box[1].h = (uint16_t)(bh / 2);
	box[1].cls = 0;
	box[1].score = 99;
	box[1].reserved = 0;

	return 2;
}

/****************************************************************************
 * Name: det_thread
 ****************************************************************************/

static void *det_thread(void *arg)
{
	struct detector *d = arg;
	uint32_t tick = 0;
#ifdef AMP_WITH_RKNN
	time_t last_log = 0;
#endif

	/* Ten a second, which is the rate the design assumes and roughly what one
	 * NPU core sustains once RGA and post-processing are added. Deliberately
	 * not the display's thirty: the boxes are allowed to lag, and making the
	 * picture wait for a producer running at a third of its rate would be the
	 * wrong way round.
	 */

	while (!d->stop && !g_stop) {
		struct amp_det_box box[AMP_DET_MAXBOX];
		struct timespec t0;
		struct timespec t1;
		uint32_t count = 0;
		long us;

		clock_gettime(CLOCK_MONOTONIC, &t0);

		if (d->synthetic) {
			count = det_synth(d, box, tick);
		}
#ifdef AMP_WITH_RKNN
		else {
			/* Waited for here rather than in the main loop's poll,
			 * because this thread owns the stream. A frame arriving
			 * while an inference is in flight simply waits in the
			 * ring - which is the behaviour wanted, since only the
			 * newest one will be used.
			 */

			struct pollfd pfd;

			pfd.fd = d->cam->fd;
			pfd.events = POLLIN;
			pfd.revents = 0;

			if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN))
				count = det_capture(d, box);
		}
#endif

		clock_gettime(CLOCK_MONOTONIC, &t1);

		us = (t1.tv_sec - t0.tv_sec) * 1000000 +
		     (t1.tv_nsec - t0.tv_nsec) / 1000;

		det_publish(d, box, count, (uint32_t)us);

		/* Say what was found, at most once a second.
		 *
		 * Not every set: ten a second with a handful of boxes each would
		 * bury everything else on the console. Not never either - with no
		 * log at all, an empty screen cannot be told apart from a
		 * detector that is running and finding nothing, and those have
		 * completely different causes.
		 */

#ifdef AMP_WITH_RKNN
		if (!d->synthetic && count > 0 && t1.tv_sec != last_log) {
			char tmp[16];
			uint32_t k;

			last_log = t1.tv_sec;

			printf("detector: %u in %ldms:", count, us / 1000);

			for (k = 0; k < count && k < 6; k++)
				printf(" %s(%u%%)",
				       det_class_name(box[k].cls, tmp,
						      sizeof(tmp)),
				       box[k].score);

			printf("%s\n", count > 6 ? " ..." : "");
		}
#endif

		tick++;

		/* Only the synthetic path paces itself. Real inference is already
		 * slower than the target interval, so sleeping after it would
		 * subtract from a rate that is the thing being measured.
		 */

		if (d->synthetic)
			usleep(100000);
	}

	return NULL;
}

/****************************************************************************
 * Name: det_start
 ****************************************************************************/

static int det_start(struct detector *d, volatile uint8_t *shm, uint32_t width,
		     uint32_t height, int rpfd, bool synthetic,
		     struct camera *cam, const char *model, const char *labels,
		     uint32_t core, uint32_t min_score,
		 const char *dump, bool zerocopy)
{
	d->desc = (volatile struct amp_det_desc *)(shm + AMP_DET_DESC_OFFSET);
	d->width = width;
	d->height = height;
	d->rpfd = rpfd;
	d->synthetic = synthetic;
	d->stop = false;

	if (!synthetic) {
#ifdef AMP_WITH_RKNN
		d->min_score = min_score;
		d->dump = dump;
		d->zerocopy_want = zerocopy;
		/* The range the ISP emits against the range the conversion
		 * assumes, stated once at startup.
		 *
		 * Printed because this took several rounds to find and cost two
		 * broken attempts at unrelated things along the way. The RGA
		 * decodes as BT.601 limited and so does the display path's CPU
		 * loop; if the ISP says full, both are clipping highlights and
		 * crushing shadows, and nothing else in the log would ever say
		 * so. See det_rga_into() for why it is not simply corrected.
		 */

		if (cam->src_quant == V4L2_QUANTIZATION_FULL_RANGE)
			printf("detector: NOTE the ISP emits full-range YUV but"
			       " the conversion decodes BT.601 limited -"
			       " highlights clip\n");

		if (cam == NULL || cam->fd < 0) {
			fprintf(stderr,
				"detector: real inference needs the second ISP"
				" stream, which did not open\n");
			return -1;
		}

		d->cam = cam;

		if (det_load_libs(d) < 0 ||
		    det_load_model(d, model, core) < 0) {
			det_release(d);
			return -1;
		}

		det_load_labels(labels);
		det_letterbox_setup(d, cam->src_w, cam->src_h);

		/* Stated at startup, because a threshold is the first thing to
		 * suspect when the screen has fewer boxes than expected - and
		 * silence here would make it the last thing anyone checked.
		 */

		printf("detector: publishing boxes at %u%% confidence and"
		       " above\n", d->min_score);

		/* Result ignored: failing to import costs 2.3ms a frame and
		 * nothing else, and det_import_buffers() has already said which
		 * reason applied. Clearing the request keeps the self-check from
		 * running against handles that were never created.
		 */

		if (d->zerocopy_want && det_import_buffers(d) < 0)
			d->zerocopy_want = false;
#else
		(void)cam;
		(void)model;
		(void)labels;
		(void)core;
		(void)min_score;
		(void)dump;
		(void)zerocopy;


		fprintf(stderr,
			"detector: this binary was built without inference"
			" support.\n"
			"  Rebuild with -DAMP_WITH_RKNN and the vendor headers on"
			" the include path,\n"
			"  or use -a for the synthetic transport self-test.\n");
		return -1;
#endif
	}

	/* Geometry and an even sequence number before the magic, so the remote
	 * cannot find the magic and then read a block that has not been set up -
	 * the same ordering the other two blocks use.
	 */

	d->desc->version = AMP_DET_VERSION;
	d->desc->seq = 0;
	d->desc->count = 0;
	d->desc->width = width;
	d->desc->height = height;
	d->desc->latency_us = 0;

	__sync_synchronize();

	d->desc->magic = AMP_DET_MAGIC;

	__sync_synchronize();

	if (pthread_create(&d->thread, NULL, det_thread, d) != 0) {
		perror("pthread_create");
		d->desc->magic = 0;
		__sync_synchronize();
		d->desc = NULL;
		return -1;
	}

	d->running = true;

	printf("detector: %s, publishing %ux%u boxes at 0x%08lx, up to %d per"
	       " set\n",
	       synthetic ? "SYNTHETIC (transport self-test, no inference)" :
	       "inference", width, height,
	       AMP_SHM_BASE + AMP_DET_DESC_OFFSET, AMP_DET_MAXBOX);

	return 0;
}

/****************************************************************************
 * Name: det_stop
 ****************************************************************************/

static void det_stop(struct detector *d)
{
	if (!d->running)
		return;

	d->stop = true;
	pthread_join(d->thread, NULL);
	d->running = false;

	/* Clear the magic, so the remote stops believing there is a detector.
	 * Without this a restart would leave the last set frozen on screen, which
	 * looks exactly like a detector that has stalled.
	 */

	if (d->desc != NULL) {
		d->desc->magic = 0;
		__sync_synchronize();
	}

	printf("detector: %u sets published, %u doorbells failed\n",
	       d->published, d->notify_fail);

#ifdef AMP_WITH_RKNN

	/* Means alongside maxima, for the reason every measurement in this program
	 * now reports both: a worst case cannot tell a phase that always costs
	 * 20ms from one that costs 3ms and was descheduled once, and that
	 * distinction has already misdirected this work five times.
	 */

	if (d->inferences > 0) {
		uint32_t n = d->inferences;

		printf("detector: %u inferences | rga %lu/%u  npu %lu/%u  post"
		       " %lu/%u us (mean/max)\n",
		       n,
		       (unsigned long)(d->rga_us_sum / n), d->rga_us_max,
		       (unsigned long)(d->run_us_sum / n), d->run_us_max,
		       (unsigned long)(d->post_us_sum / n), d->post_us_max);

		printf("detector: at most %u boxes published, %u candidates"
		       " before NMS (ceiling %d)\n",
		       d->boxes_max, d->cands_max, YOLO_MAX_CAND);

		/* How much the confidence filter threw away, so its setting can
		 * be judged instead of guessed at. A count in the thousands
		 * against a handful published means the threshold is doing the
		 * work; a count of zero means it is not earning its place.
		 */

		printf("detector: %u boxes dropped below %u%% confidence\n",
		       d->filtered, d->min_score);
	}

	det_release(d);

#endif
}

/****************************************************************************
 * Name: cam_close
 ****************************************************************************/

static void cam_close(struct camera *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	unsigned int i;

	if (c->fd < 0)
		return;

	ioctl(c->fd, VIDIOC_STREAMOFF, &type);

	/* Clear the magic so the remote stops believing the slots hold anything.
	 * Without this a restart of this program would leave the last frame
	 * frozen on screen, which looks exactly like a stalled pipeline.
	 */

	if (c->desc != NULL) {
		c->desc->magic = 0;
		__sync_synchronize();
	}

	for (i = 0; i < CAM_MAX_BUFFERS; i++) {
		if (c->buf[i].start != NULL)
			munmap(c->buf[i].start, c->buf[i].length);
	}

	free(c->xmap);
	free(c->ymap);
	free(c->stage);
	free(c->nv12);
	c->xmap = NULL;
	c->ymap = NULL;
	c->stage = NULL;
	c->nv12 = NULL;

	close(c->fd);
	c->fd = -1;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-d card] [-T dev] [-q] [-C] [-V dev] [-W w] [-H h]\n"
		"  -d card    DRM device (default /dev/dri/card0)\n"
		"  -T dev     touch event device (default: auto-detect)\n"
		"  -q         do not log each contact\n"
		"  -t         accepted and ignored (touch is always on now)\n"
		"  -C         capture from the camera and publish frames\n"
		"  -V dev     capture node (default: find the rkisp mainpath)\n"
		"  -W w       published frame width (default %u)\n"
		"  -H h       published frame height (default %u)\n"
		"  -D         also run a second ISP stream, counting only\n"
		"  -S dev     second stream node (default: find the selfpath)\n"
		"  -R WxH     second stream size (default 1280x720)\n"
		"  -a         publish synthetic detections - a self-test of the\n"
		"             results transport, with no inference at all\n"
		"  -A         run real inference on the second stream. Takes it\n"
		"             over from -D: the detector thread owns that queue\n"
		"  -M file    .rknn model (default yolov5s-640-640.rknn)\n"
		"  -L file    class name list, for the log only\n"
		"  -N core    NPU core: 0, 1, 2, auto or all (default 0)\n"
		"  -c pct     publish only boxes at this confidence or above\n"
		"             (default 35; 0 shows everything the model emits)\n"

		"  -Z         try converting out of imported dma-buf handles\n"
		"             instead of mapping pages every call. Checked\n"
		"             against the mapped path over 60 frames first, and\n"
		"             abandoned if the two disagree by a single byte\n"
		"  -P prefix  write what the camera produced and what the model\n"
		"             was given, as prefix-NNN-src.pgm and -in.ppm.\n"
		"             One dump two seconds in, then one per SIGUSR1 -\n"
		"             so the frame where a box was missing can be caught\n"
		"\n"
		"Lights the panel and forwards touch to the AMP core. With -C it\n"
		"also captures from the ISP, converts each frame to XRGB8888 and\n"
		"publishes it in the shared carveout for the AMP core to draw.\n"
		"Keeps running: exiting restores the previous CRTC\n"
		"configuration, which takes the panel down.\n",
		prog, AMP_CAM_DEF_WIDTH, AMP_CAM_DEF_HEIGHT);
}

int main(int argc, char **argv)
{
	const char *card = "/dev/dri/card0";
	const char *rpmsg_dev = "/dev/rpmsg0";
	struct display d;
	volatile struct amp_shm_ctrl *ctrl;
	volatile uint8_t *shm;
	struct amp_shm_hdr hello = { .cmd = AMP_SHM_CMD_HELLO };
	struct camera cam;
	struct camera det;
	time_t cam_last_report = 0;
	uint32_t cam_last_count = 0;
	uint32_t det_last_count = 0;
#ifdef AMP_WITH_RKNN
	uint32_t det_last_inf = 0;
#endif
	const char *cam_dev = NULL;
	const char *det_dev = NULL;
	uint32_t det_w = 1280;
	uint32_t det_h = 720;
	bool want_detect = false;
	struct detector ai;
	bool want_synth = false;
	bool want_ai = false;
	const char *ai_model = "yolov5s-640-640.rknn";
	const char *ai_labels = "model/coco_80_labels_list.txt";
	const char *ai_dump = NULL;
	bool ai_zerocopy = false;

	/* Core 0, not auto. Measured: 17.4ms on one core against 10.0ms on all
	 * three, so three times the hardware buys 1.74x. Pinning costs 30 percent
	 * more latency than the best case and leaves two cores entirely free, which
	 * is the right trade when the target is ten frames a second against a
	 * single-core ceiling near fifty.
	 */

	uint32_t ai_core = 1;

	/* 35 percent, chosen from measurement rather than taste. On this board 107
	 * detections split cleanly: real people came out at 60 to 91 percent, and
	 * everything under about 20 was furniture that was not there. 35 removes 56
	 * percent of the boxes and five phantom classes entirely, at the cost of
	 * three person detections that were all in the 25 to 34 band.
	 */

	uint32_t ai_minscore = 35;
	uint32_t cam_w = AMP_CAM_DEF_WIDTH;
	uint32_t cam_h = AMP_CAM_DEF_HEIGHT;
	bool want_camera = false;
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

	memset(&cam, 0, sizeof(cam));
	memset(&det, 0, sizeof(det));
	memset(&ai, 0, sizeof(ai));
	cam.fd = -1;
	det.fd = -1;

	while ((opt = getopt(argc, argv,
			     "d:T:qthCV:W:H:DS:R:aAM:L:N:c:P:Z")) != -1) {
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
		case 'C':
			want_camera = true;
			break;
		case 'V':
			cam_dev = optarg;
			want_camera = true;
			break;
		case 'W':
			cam_w = (uint32_t)strtoul(optarg, NULL, 0);
			want_camera = true;
			break;
		case 'H':
			cam_h = (uint32_t)strtoul(optarg, NULL, 0);
			want_camera = true;
			break;
		case 'D':
			want_detect = true;
			want_camera = true;
			break;
		case 'a':

			/* Synthetic detections. Needs the camera because the
			 * boxes are in the published frame's coordinate space
			 * and there is nothing to draw them over otherwise, but
			 * deliberately does not need the second ISP stream:
			 * this tests the results transport, nothing else.
			 */

			want_synth = true;
			want_camera = true;
			break;
		case 'A':

			/* Real inference. Implies the second stream, because the
			 * detector reads from it directly - and unlike -D that
			 * stream is then the thread's, not the poll loop's.
			 */

			want_ai = true;
			want_detect = true;
			want_camera = true;
			break;
		case 'M':
			ai_model = optarg;
			want_ai = true;
			want_detect = true;
			want_camera = true;
			break;
		case 'L':
			ai_labels = optarg;
			break;
		case 'P':
			ai_dump = optarg;
			break;
		case 'Z':

			/* Opt-in, and it still has to pass its own check before
			 * it is used. Two earlier attempts at this optimisation
			 * shipped straight into the default path and both broke
			 * a working detector - once by losing cache maintenance,
			 * once by mixing addressing modes. Default off plus a
			 * self-check is the difference between measuring an idea
			 * and betting on it.
			 */

			ai_zerocopy = true;
			break;
		case 'c': {
			/* Range-checked, because a threshold above 100 would
			 * suppress every box and look exactly like a detector
			 * that had stopped finding anything.
			 */

			unsigned long v;
			char *end;

			v = strtoul(optarg, &end, 10);
			if (*end != '\0' || v > 100) {
				fprintf(stderr,
					"-c wants a percentage, 0 to 100\n");
				return EXIT_FAILURE;
			}

			ai_minscore = (uint32_t)v;
			break;
		}
		case 'N':

			/* Named rather than a raw bitmask. The mask that means
			 * "core 2" is 4, and a -N 2 that quietly selected core 1
			 * would make a per-core comparison wrong in a way the
			 * output would not show.
			 */

			if (strcmp(optarg, "auto") == 0) {
				ai_core = 0;
			} else if (strcmp(optarg, "all") == 0) {
				ai_core = 7;
			} else if (optarg[0] >= '0' && optarg[0] <= '2' &&
				   optarg[1] == '\0') {
				ai_core = 1u << (optarg[0] - '0');
			} else {
				fprintf(stderr,
					"-N wants 0, 1, 2, auto or all\n");
				return EXIT_FAILURE;
			}

			break;
		case 'S':
			det_dev = optarg;
			want_detect = true;
			want_camera = true;
			break;
		case 'R': {
			/* "WxH", parsed strictly. A size that silently became
			 * something else would make the whole measurement
			 * meaningless while still producing numbers.
			 */
			unsigned long w;
			unsigned long h;
			char *end;

			w = strtoul(optarg, &end, 10);
			if (*end != 'x' || w == 0) {
				fprintf(stderr, "-R wants WxH, e.g. 1280x720\n");
				return EXIT_FAILURE;
			}

			h = strtoul(end + 1, &end, 10);
			if (*end != '\0' || h == 0) {
				fprintf(stderr, "-R wants WxH, e.g. 1280x720\n");
				return EXIT_FAILURE;
			}

			det_w = (uint32_t)w;
			det_h = (uint32_t)h;
			want_detect = true;
			want_camera = true;
			break;
		}
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

#ifdef AMP_WITH_RKNN

	/* SIGUSR1 asks the detector for a frame dump rather than killing the
	 * program, which is what the default disposition would do - and losing the
	 * run is a poor response to being asked for a diagnostic. Installed
	 * unconditionally so that a stray signal is harmless whether -P was given
	 * or not.
	 */

	signal(SIGUSR1, on_dump_signal);

#endif

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
	else if (!rpmsg_send(rpfd, &hello, sizeof(hello)))
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

	/* Also not fatal, and for the same reason touch is not: the panel is up by
	 * now, and holding it up is the job nothing else can do. A camera that
	 * will not start should leave a working display and working touch behind
	 * it, not take them down - that was the mistake with rpmsg being a
	 * precondition for the modeset, where a missing kernel module produced a
	 * dark panel and looked like a touch fault.
	 */

	if (want_camera) {
		if (cam_open(&cam, "display", "mainpath", cam_dev, cam_w,
			     cam_h, !quiet) < 0) {
			fprintf(stderr, "camera will NOT be published\n");
		} else {
			struct timespec t;

			cam.publish = true;

			if (cam_publish_setup(&cam, shm, cam_w, cam_h) < 0) {
				cam_close(&cam);
				fprintf(stderr,
					"camera will NOT be published\n");
			}

			/* Start the reporting interval here rather than leaving
			 * it at zero, or the first report divides by a guessed
			 * five seconds and prints a rate that never happened.
			 */

			clock_gettime(CLOCK_MONOTONIC, &t);
			cam_last_report = t.tv_sec;

			if (rpfd < 0)
				fprintf(stderr,
					"camera: no rpmsg channel, frames are"
					" published but the remote will not be"
					" told - it polls the descriptor, so"
					" expect latency\n");
		}
	}

	/* The second stream, opened only to find out whether it can exist.
	 *
	 * Nothing downstream of it is written yet - no RGA, no inference, no
	 * results. It dequeues, counts and requeues, because the one thing that
	 * could still invalidate the whole detector plan is the ISP not managing
	 * two paths at once under this sensor's timing, and that is cheaper to
	 * find out now than after the protocol is written around it.
	 *
	 * Failing to open it is not fatal for the same reason nothing else here
	 * is: the panel, touch and the display stream are all working by this
	 * point, and a second capture that will not start should not take them
	 * down with it.
	 */

	if (want_detect) {
		if (cam_open(&det, "detect", "selfpath", det_dev, det_w, det_h,
			     !quiet) < 0) {
			fprintf(stderr,
				"second stream will NOT run - the detector"
				" would have to share the display stream\n");
		} else {
			printf("detect stream: counting only, no conversion,"
			       " no inference\n");
		}
	}

	/* The detector, last, and only if the camera came up: its boxes are in the
	 * published frame's coordinate space, so without a frame they describe a
	 * space that does not exist.
	 *
	 * Not fatal either, for the reason everything else here is not fatal.
	 */

	if ((want_synth || want_ai) && cam.fd >= 0) {
		/* Real inference wins if both were asked for. -a exists to test
		 * the transport with nothing else moving, so running it alongside
		 * the real thing would put two producers on one block.
		 */

		if (want_ai && want_synth)
			fprintf(stderr,
				"-A and -a together: running real inference,"
				" ignoring -a\n");

		if (det_start(&ai, shm, cam.out_w, cam.out_h, rpfd, !want_ai,
			      &det, ai_model, ai_labels, ai_core,
			      ai_minscore, ai_dump, ai_zerocopy) < 0)
			fprintf(stderr, "detector will NOT run\n");
	} else if (want_synth || want_ai) {
		fprintf(stderr,
			"detector needs the camera - boxes are in the published"
			" frame's coordinates\n");
	}

	printf("panel is up, %s%s%s%s - Ctrl-C takes it down\n",
	       ts.fd >= 0 ? "forwarding touch" :
	       "touch NOT forwarded (see above)",
	       cam.fd >= 0 ? ", publishing camera frames" : "",
	       det.fd >= 0 ? ", counting a second stream" : "",
	       ai.running ? ", publishing detections" : "");

	/* Two things can wake this up now: a contact, and a captured frame. Both
	 * on one poll rather than a thread each, because neither does enough work
	 * to be worth the synchronisation - a contact is a dozen bytes and a frame
	 * is one conversion pass - and because a single loop keeps the ordering
	 * obvious if they ever start interfering.
	 *
	 * With neither present there is nothing to wait for but a signal, which is
	 * what pause() is for.
	 */

	while (!g_stop) {
		struct pollfd pfd[3];
		int nfds = 0;
		int tsidx = -1;
		int camidx = -1;
		int detidx = -1;
		int n;

		if (ts.fd >= 0) {
			tsidx = nfds;
			pfd[nfds].fd = ts.fd;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			nfds++;
		}

		if (cam.fd >= 0) {
			camidx = nfds;
			pfd[nfds].fd = cam.fd;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			nfds++;
		}

		/* Not polled here when the detector has it.
		 *
		 * Two consumers on one V4L2 queue would race for buffers, and the
		 * loser would either block the display loop or hand the detector
		 * a buffer that had already been requeued. The thread is the sole
		 * consumer in that mode, so this loop must not touch the fd at
		 * all - which is also why -A and -D are different options rather
		 * than one that does both.
		 *
		 * Keyed on whether the detector is actually running, not on
		 * whether it was asked for. If it failed to start - no runtime,
		 * no model file - the stream would otherwise be left with no
		 * consumer at all, filling its ring once and then sitting there
		 * looking like an ISP that had stopped. This way a failed
		 * detector degrades to exactly what -D does.
		 */

		if (det.fd >= 0 && !ai.running) {
			detidx = nfds;
			pfd[nfds].fd = det.fd;
			pfd[nfds].events = POLLIN;
			pfd[nfds].revents = 0;
			nfds++;
		}

		if (nfds == 0) {
			pause();
			continue;
		}

		n = poll(pfd, nfds, 1000);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			goto out;
		}

		if (n == 0)
			continue;

		if (tsidx >= 0 && (pfd[tsidx].revents & POLLIN))
			touch_drain(&ts, rpfd, fb_w, fb_h, !quiet);

		if (camidx >= 0 && (pfd[camidx].revents & POLLIN))
			cam_drain(&cam, rpfd, !quiet);

		/* Drained after the display stream, deliberately. If the two ever
		 * compete for time in this loop, the one that matters should win.
		 */

		if (detidx >= 0 && (pfd[detidx].revents & POLLIN))
			cam_drain(&det, -1, !quiet);

		/* Say something periodically while capturing, because otherwise
		 * this program is completely silent once it starts and the only
		 * way to find out whether frames are moving is to stop it and
		 * read the summary. A producer that has quietly stalled looks
		 * exactly like one that is working.
		 *
		 * Printed regardless of -q: that flag is documented as
		 * suppressing the per-contact log, and one line every few seconds
		 * is not that.
		 */

		if (cam.fd >= 0) {
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);

			if (now.tv_sec - cam_last_report >= 5) {
				uint32_t d = cam.published - cam_last_count;
				long secs = now.tv_sec - cam_last_report;

				uint32_t n = cam.published ? cam.published : 1;

				printf("camera: %u frames (%lu.%lu fps) cpu%d |"
				       " fetch %lu/%u convert %lu/%u"
				       " publish %lu/%u us | gaps %u/%u\n",
				       cam.published,
				       (unsigned long)(d / secs),
				       (unsigned long)((d * 10 / secs) % 10),
				       cam.cpu,
				       (unsigned long)(cam.fetch_us_sum / n),
				       cam.fetch_us_max,
				       (unsigned long)(cam.convert_us_sum / n),
				       cam.convert_us_max,
				       (unsigned long)(cam.copy_us_sum / n),
				       cam.copy_us_max,
				       cam.gaps, cam.gap_frames);

				/* Reported on its own line rather than folded
				 * into the one above, because the two streams
				 * are being compared and a reader should not
				 * have to disentangle which number belongs to
				 * which.
				 */

				if (det.fd >= 0) {
					uint32_t dd = det.published -
						      det_last_count;

					printf("detect: %u frames (%lu.%lu fps)"
					       " %ux%u | gaps %u/%u\n",
					       det.published,
					       (unsigned long)(dd / secs),
					       (unsigned long)((dd * 10 / secs)
							       % 10),
					       det.src_w, det.src_h,
					       det.gaps, det.gap_frames);

					det_last_count = det.published;
				}

#ifdef AMP_WITH_RKNN

				/* The detector, on its own line and printed
				 * whether or not it found anything.
				 *
				 * This exists because the phase timings were
				 * unreachable in practice. They were collected
				 * per frame and printed only by det_stop(), and
				 * this program is killed rather than asked to
				 * exit - so four sets of measurements were
				 * gathered and never once displayed. Exactly the
				 * mistake of collecting counters and not
				 * printing them, one layer further out.
				 *
				 * Unconditional on the box count for a second
				 * reason: the per-detection log is gated on
				 * finding something, so an empty room produces
				 * total silence, which is indistinguishable from
				 * a detector that has died. Liveness should be
				 * stated, not inferred from the capture counter
				 * still climbing - that inference requires
				 * knowing which thread increments it.
				 *
				 * The counters are read without a lock. They are
				 * written by the inference thread, so a mean
				 * computed here can be off by at most one
				 * sample if a read lands between a sum and the
				 * count. That is worth accepting: the
				 * alternative is a mutex in the inference path
				 * for the benefit of a printf.
				 */

				if (ai.running && ai.inferences > 0) {
					uint32_t n = ai.inferences;
					uint32_t di = n - det_last_inf;

					printf("detector: %u inferences"
					       " (%lu.%lu/s) | rga %lu/%u"
					       " npu %lu/%u post %lu/%u us |"
					       " %u boxes max, %u dropped"
					       " <%u%%\n",
					       n,
					       (unsigned long)(di / secs),
					       (unsigned long)((di * 10 / secs)
							       % 10),
					       (unsigned long)(ai.rga_us_sum /
							       n),
					       ai.rga_us_max,
					       (unsigned long)(ai.run_us_sum /
							       n),
					       ai.run_us_max,
					       (unsigned long)(ai.post_us_sum /
							       n),
					       ai.post_us_max,
					       ai.boxes_max, ai.filtered,
					       ai.min_score);

					/* Transport health and the candidate
					 * ceiling, on their own line for the
					 * same reason the second capture stream
					 * gets one: a reader should not have to
					 * work out which number belongs to
					 * which concern.
					 *
					 * These three were left behind when the
					 * timings were moved off det_stop() -
					 * which never runs, since this program
					 * is killed rather than asked to exit.
					 * notify_fail is the one that mattered:
					 * a counter that only ever records a
					 * failure, printed nowhere.
					 *
					 * The ceiling is here because
					 * yolo_decode() stops collecting when
					 * the array fills and says nothing.
					 * That is a reasonable thing to do and
					 * an unreasonable thing to do quietly -
					 * a scene busy enough to hit it loses
					 * its weakest candidates before NMS
					 * ever sees them.
					 */

					printf("detector: %u sets, %u"
					       " doorbells failed | %u of %d"
					       " candidates max\n",
					       ai.published, ai.notify_fail,
					       ai.cands_max, YOLO_MAX_CAND);

					det_last_inf = n;
				}

#endif

				cam_last_report = now.tv_sec;
				cam_last_count = cam.published;
			}
		}
	}

	ret = EXIT_SUCCESS;

out:
	/* Stopped first, so the thread is not still publishing into a block whose
	 * magic is about to be cleared, and not still writing to a descriptor
	 * that is about to be closed.
	 */

	det_stop(&ai);

	if (det.fd >= 0) {
		printf("detect: %u frames, %u gaps totalling %u lost frames\n",
		       det.published, det.gaps, det.gap_frames);
		cam_close(&det);
	}

	if (cam.fd >= 0) {
		uint32_t n = cam.published ? cam.published : 1;

		printf("camera: %u frames published on cpu%d | fetch %lu/%u"
		       " convert %lu/%u publish %lu/%u us (mean/max)"
		       " | %u gaps totalling %u lost frames\n",
		       cam.published, cam.cpu,
		       (unsigned long)(cam.fetch_us_sum / n), cam.fetch_us_max,
		       (unsigned long)(cam.convert_us_sum / n),
		       cam.convert_us_max,
		       (unsigned long)(cam.copy_us_sum / n), cam.copy_us_max,
		       cam.gaps, cam.gap_frames);
		cam_close(&cam);
	}

	if (ts.fd >= 0)
		close(ts.fd);

	if (rpfd >= 0)
		close(rpfd);

	display_cleanup(&d);
	return ret;
}
