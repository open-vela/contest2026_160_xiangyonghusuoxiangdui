/*
 * amp_isp_range - read, and optionally change, the YUV range the ISP emits.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * There is a mismatch on this board, and it was found the long way round. The
 * detector converts the ISP's NV12 to RGB with the RGA; the display path does the
 * same conversion on the CPU. Both assume BT.601 limited range - the CPU one
 * visibly so, subtracting 16 from Y and scaling by 298/256, and the RGA one was
 * established by taking a dumped frame, converting it offline under each of the
 * four candidate matrices, and finding that BT.601 limited matched to a mean
 * absolute error of 0.40 levels.
 *
 * The ISP, meanwhile, reports full range. rkisp.c:4914 sets that as the subdev
 * default and capture.c:897 copies it into every capture format, overwriting
 * whatever the application asked for - so the mismatch cannot be fixed from the
 * capture node.
 *
 * Decoding full-range data as limited applies a gain of 255/219 and an offset of
 * -18.6: everything below Y=16 crushes to black, everything above 235 clips to
 * white, and the whole picture gets 16 percent more contrast than it should. On
 * measured frames that clipped between 2 and 7 percent of pixels.
 *
 * It can be fixed at the source. rkisp.c:3088 forces full range only when the
 * request is V4L2_QUANTIZATION_DEFAULT; an explicit value is taken as given. So
 * one SUBDEV_S_FMT on RKISP_ISP_PAD_SOURCE_PATH makes the sensor side agree with
 * the two decoders that already exist, rather than changing either of them - and
 * the display path's conversion is a vectorised loop with a regression test
 * counting 334 NEON instructions, which is not a thing to touch for this.
 *
 * Reads by default and only writes when asked, because this changes a device that
 * something else may be streaming from.
 *
 * Build:
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra \
 *     -idirafter <kernel>/include/uapi -o amp_isp_range amp_isp_range.c
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

/* From the driver's own enum rkisp_isp_pad (rkisp.h:114). Hardcoded because it
 * is not exported to userspace, and named here so a reader can check it against
 * that header rather than wonder where 2 came from.
 */

#define RKISP_ISP_PAD_SOURCE_PATH 2

static const char *quant_name(unsigned int q)
{
	switch (q) {
	case V4L2_QUANTIZATION_DEFAULT:
		return "default";
	case V4L2_QUANTIZATION_FULL_RANGE:
		return "full";
	case V4L2_QUANTIZATION_LIM_RANGE:
		return "limited";
	default:
		return "?";
	}
}

static const char *cs_name(unsigned int c)
{
	switch (c) {
	case V4L2_COLORSPACE_SMPTE170M:
		return "SMPTE170M/BT.601";
	case V4L2_COLORSPACE_REC709:
		return "REC709/BT.709";
	case V4L2_COLORSPACE_DEFAULT:
		return "default";
	default:
		return "other";
	}
}

/****************************************************************************
 * Name: find_isp_subdev
 *
 * Description:
 *   Locate the ISP subdev's device node through the media controller.
 *
 *   By entity name rather than by guessing a /dev/v4l-subdevN, because subdev
 *   nodes carry no identifying information of their own and their numbering moves
 *   with probe order - the same reason the capture nodes in amp_fb_show are found
 *   by card name instead of by number.
 *
 ****************************************************************************/

static int find_isp_subdev(char *path, size_t pathlen)
{
	int m;

	for (m = 0; m < 8; m++) {
		struct media_entity_desc ent;
		char mpath[32];
		int mfd;
		int id;

		snprintf(mpath, sizeof(mpath), "/dev/media%d", m);

		mfd = open(mpath, O_RDWR);
		if (mfd < 0)
			continue;

		for (id = 0; ; id++) {
			int s;

			memset(&ent, 0, sizeof(ent));
			ent.id = id | MEDIA_ENT_ID_FLAG_NEXT;

			if (ioctl(mfd, MEDIA_IOC_ENUM_ENTITIES, &ent) < 0)
				break;

			id = ent.id;

			if (strstr(ent.name, "isp-subdev") == NULL)
				continue;

			printf("%s: entity %u \"%s\", %u pads, dev %u:%u\n",
			       mpath, ent.id, ent.name, ent.pads,
			       ent.dev.major, ent.dev.minor);

			/* Match the major:minor the media controller reports
			 * against the subdev nodes, which is the only reliable
			 * way across kernel versions.
			 */

			for (s = 0; s < 32; s++) {
				struct stat st;
				char sp[32];

				snprintf(sp, sizeof(sp), "/dev/v4l-subdev%d",
					 s);

				if (stat(sp, &st) < 0)
					continue;

				if (major(st.st_rdev) != ent.dev.major ||
				    minor(st.st_rdev) != ent.dev.minor)
					continue;

				snprintf(path, pathlen, "%s", sp);
				close(mfd);
				return 0;
			}

			printf("  no /dev/v4l-subdev* matches %u:%u\n",
			       ent.dev.major, ent.dev.minor);
		}

		close(mfd);
	}

	return -1;
}

int main(int argc, char **argv)
{
	struct v4l2_subdev_format fmt;
	char path[64];
	bool set_limited = false;
	bool set_full = false;
	int fd;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--limited") == 0)
			set_limited = true;
		else if (strcmp(argv[i], "--full") == 0)
			set_full = true;
		else {
			fprintf(stderr,
				"usage: %s [--limited | --full]\n"
				"  no argument: report what the ISP emits\n"
				"  --limited:   ask for BT.601 limited range,"
				" which is what both\n"
				"               of this project's YUV decoders"
				" already assume\n"
				"  --full:      ask for full range, the"
				" driver's default\n",
				argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (set_limited && set_full) {
		fprintf(stderr, "--limited and --full are exclusive\n");
		return EXIT_FAILURE;
	}

	if (find_isp_subdev(path, sizeof(path)) < 0) {
		fprintf(stderr, "no ISP subdev found\n");
		return EXIT_FAILURE;
	}

	printf("using %s pad %d\n", path, RKISP_ISP_PAD_SOURCE_PATH);

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return EXIT_FAILURE;
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad = RKISP_ISP_PAD_SOURCE_PATH;

	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt) < 0) {
		fprintf(stderr, "SUBDEV_G_FMT: %s\n", strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	printf("current: %ux%u code 0x%04x, colorspace %u (%s),"
	       " quantization %u (%s)\n",
	       fmt.format.width, fmt.format.height, fmt.format.code,
	       fmt.format.colorspace, cs_name(fmt.format.colorspace),
	       fmt.format.quantization, quant_name(fmt.format.quantization));

	if (!set_limited && !set_full) {
		printf("\nNothing changed. The decoders in amp_fb_show assume"
		       " BT.601 limited;\nif the line above says full, they are"
		       " crushing blacks and clipping whites.\n");
		close(fd);
		return EXIT_SUCCESS;
	}

	fmt.format.quantization = set_limited ? V4L2_QUANTIZATION_LIM_RANGE :
				  V4L2_QUANTIZATION_FULL_RANGE;

	/* Asked for explicitly as well, because the driver only keeps a
	 * quantization request when it is not DEFAULT, and it normalises anything
	 * that is not one of its three known colour spaces to SMPTE170M anyway.
	 * Saying so makes the request unambiguous rather than relying on that.
	 */

	fmt.format.colorspace = V4L2_COLORSPACE_SMPTE170M;

	if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &fmt) < 0) {
		fprintf(stderr, "SUBDEV_S_FMT: %s\n", strerror(errno));
		fprintf(stderr,
			"  the ISP may refuse while a stream is running - stop"
			" amp_fb_show first\n");
		close(fd);
		return EXIT_FAILURE;
	}

	/* Read back rather than trusting the write. The driver adjusts requests
	 * silently in several places in this path, and a request that was quietly
	 * declined would otherwise look like one that took.
	 */

	memset(&fmt, 0, sizeof(fmt));
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad = RKISP_ISP_PAD_SOURCE_PATH;

	if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &fmt) < 0) {
		fprintf(stderr, "SUBDEV_G_FMT after set: %s\n",
			strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	printf("now:     %ux%u code 0x%04x, colorspace %u (%s),"
	       " quantization %u (%s)\n",
	       fmt.format.width, fmt.format.height, fmt.format.code,
	       fmt.format.colorspace, cs_name(fmt.format.colorspace),
	       fmt.format.quantization, quant_name(fmt.format.quantization));

	if (fmt.format.quantization !=
	    (set_limited ? V4L2_QUANTIZATION_LIM_RANGE :
	     V4L2_QUANTIZATION_FULL_RANGE)) {
		printf("\nThe request did not stick.\n");
		close(fd);
		return EXIT_FAILURE;
	}

	printf("\nTook. This lives on the subdev, so a capture that is already"
	       " streaming will\nnot change - restart amp_fb_show, then check"
	       " its startup line reports the\nsame range.\n");

	close(fd);
	return EXIT_SUCCESS;
}
