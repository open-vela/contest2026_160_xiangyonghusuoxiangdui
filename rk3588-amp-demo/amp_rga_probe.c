/*
 * amp_rga_probe - what does the RGA cost for the camera-to-detector conversion?
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The last unmeasured link. Everything else in the detection path now has a real
 * number: the NPU is 17.3ms on one core with almost no CPU, and input conversion
 * plus output fetch plus post-processing together are 1.6ms. What has never been
 * exercised on this board is the step in front of all of it - taking the NV12
 * frame the ISP produces and turning it into the 640x640 RGB888 the model wants.
 *
 * The vendor yolov5 demo did not test it either, which is easy to miss: bus.jpg
 * is already 640x640, so main.cc took its "no resize needed" branch and the RGA
 * path was skipped entirely. A camera frame is neither 640x640 nor RGB, so it
 * cannot be skipped in the real thing.
 *
 * Two conversions are timed, because they are different jobs and the pipeline
 * needs both:
 *
 *   detect  NV12 WxH -> RGB888 640x640     feeds the model
 *   display NV12 WxH -> RGBA8888 512x288   what amp_fb_show does on the CPU today
 *
 * The second is here to answer a question left open by A-16. That conversion
 * currently costs about 11ms of CPU in a vectorised loop; if the RGA does it in
 * one, the whole camera display path stops spending CPU on pixels at all.
 *
 * Written in C against the im2d C entry points - improcess() and
 * wrapbuffer_virtualaddr_t() are declared IM_C_API, so the C++ wrappers with
 * their default arguments are not needed.
 *
 * Build:
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra -I<rga>/include \
 *     -o amp_rga_probe amp_rga_probe.c -L<rga>/libs/Linux/gcc-aarch64 -lrga
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include "im2d.h"
#include "rga.h"

#define DEF_SRC_W   1920
#define DEF_SRC_H   1080
#define DEF_LOOPS   100

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)(ts.tv_nsec / 1000);
}

/****************************************************************************
 * Name: run_case
 *
 * Description:
 *   Time one conversion repeatedly and report mean and max.
 *
 *   Mean as well as max because a maximum on its own has misled this project
 *   four times now, and the first iteration is discarded because it pays for
 *   whatever the library sets up lazily - the same first-touch effect that made
 *   the vendor demo's single run look slower than its ten-run average.
 *
 ****************************************************************************/

static int run_case(const char *what, void *src, int sw, int sh, int sfmt,
		    void *dst, int dw, int dh, int dfmt, int loops)
{
	rga_buffer_t s;
	rga_buffer_t d;
	im_rect srect;
	im_rect drect;
	im_rect prect;
	rga_buffer_t pat;
	uint64_t sum = 0;
	uint64_t max = 0;
	int done = 0;
	int i;

	memset(&srect, 0, sizeof(srect));
	memset(&drect, 0, sizeof(drect));
	memset(&prect, 0, sizeof(prect));
	memset(&pat, 0, sizeof(pat));

	s = wrapbuffer_virtualaddr_t(src, sw, sh, sw, sh, sfmt);
	d = wrapbuffer_virtualaddr_t(dst, dw, dh, dw, dh, dfmt);

	if (s.width == 0 || d.width == 0) {
		printf("  %-8s : wrapbuffer failed\n", what);
		return -1;
	}

	for (i = 0; i < loops; i++) {
		uint64_t t0 = now_us();
		IM_STATUS st = improcess(s, d, pat, srect, drect, prect,
					 IM_SYNC);
		uint64_t dt = now_us() - t0;

		if (st != IM_STATUS_SUCCESS) {
			printf("  %-8s : improcess failed: %s\n", what,
			       imStrError_t(st));
			return -1;
		}

		if (i == 0)
			continue;

		sum += dt;
		if (dt > max)
			max = dt;

		done++;
	}

	if (done == 0) {
		printf("  %-8s : no completed iterations\n", what);
		return -1;
	}

	printf("  %-8s : %4dx%-4d -> %4dx%-4d  %" PRIu64 "/%" PRIu64
	       " us (mean/max)\n",
	       what, sw, sh, dw, dh, sum / (uint64_t)done, max);
	return 0;
}

/****************************************************************************
 * Name: run_v4l2_case
 *
 * Description:
 *   Time the RGA doing exactly what the detector asks of it, on a buffer that
 *   came from exactly where the detector's buffers come from.
 *
 *   This exists because the detector measured 4649us for a conversion this probe
 *   measured at 2659us - while writing less, since the detector only fills the
 *   letterboxed centre and this probe's other cases fill the whole 640x640. Doing
 *   less work and taking 75 percent longer needs an explanation, and there were
 *   two candidates: everything else on the system competing for DDR, or the
 *   source buffer.
 *
 *   The difference in the source is not incidental. The other cases convert out
 *   of malloc'd memory. The detector converts out of a V4L2 MMAP buffer, which
 *   arrives through dma_mmap_attrs() and is therefore MT_NORMAL_NC and possibly
 *   VM_PFNMAP - and is handed to librga as a virtual address, so something has to
 *   pin those 338 pages and build an RGA MMU mapping for them. Whether that
 *   happens once or on every call is not documented, and is exactly the kind of
 *   thing that shows up as a constant per-call cost.
 *
 *   Buffers are rotated through DQBUF/QBUF rather than one being dequeued and
 *   reused, because reuse would hide a per-call mapping cost if librga caches by
 *   address - and because rotating is what the detector does. Only improcess() is
 *   timed; the dequeue sits outside, so this measures the same span the detector
 *   reports.
 *
 ****************************************************************************/

static int run_v4l2_case(const char *what, int fd, struct v4l2_buffer *bufs,
			 void **starts, unsigned int nbufs, int sw, int sh,
			 int sstride, void *dst, int dw, int dh, int dfmt,
			 int pad_x, int pad_y, int lb_w, int lb_h, int loops)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	rga_buffer_t d;
	im_rect srect;
	im_rect drect;
	im_rect prect;
	rga_buffer_t pat;
	uint64_t sum = 0;
	uint64_t max = 0;
	int done = 0;
	int i;

	(void)bufs;

	memset(&srect, 0, sizeof(srect));
	memset(&drect, 0, sizeof(drect));
	memset(&prect, 0, sizeof(prect));
	memset(&pat, 0, sizeof(pat));

	srect.x = 0;
	srect.y = 0;
	srect.width = sw;
	srect.height = sh;

	/* The letterboxed centre, as the detector asks for it. Passing the whole
	 * destination here would measure a different, larger job and make the
	 * comparison with the detector meaningless.
	 */

	drect.x = pad_x;
	drect.y = pad_y;
	drect.width = lb_w;
	drect.height = lb_h;

	d = wrapbuffer_virtualaddr_t(dst, dw, dh, dw, dh, dfmt);
	if (d.width == 0) {
		printf("  %-8s : wrapbuffer failed on the destination\n", what);
		return -1;
	}

	for (i = 0; i < loops; i++) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		rga_buffer_t s;
		IM_STATUS st;
		uint64_t t0;
		uint64_t dt;

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.m.planes = planes;
		buf.length = VIDEO_MAX_PLANES;

		if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
			if (errno == EINTR)
				continue;

			printf("  %-8s : DQBUF: %s\n", what, strerror(errno));
			return -1;
		}

		if (buf.index >= nbufs) {
			printf("  %-8s : DQBUF returned index %u\n", what,
			       buf.index);
			return -1;
		}

		s = wrapbuffer_virtualaddr_t(starts[buf.index], sw, sh, sstride,
					     sh, RK_FORMAT_YCbCr_420_SP);

		if (s.width == 0) {
			printf("  %-8s : wrapbuffer failed on the source\n",
			       what);
			return -1;
		}

		t0 = now_us();
		st = improcess(s, d, pat, srect, drect, prect, IM_SYNC);
		dt = now_us() - t0;

		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
			printf("  %-8s : QBUF: %s\n", what, strerror(errno));

		if (st != IM_STATUS_SUCCESS) {
			printf("  %-8s : improcess failed: %s\n", what,
			       imStrError_t(st));
			return -1;
		}

		if (i == 0)
			continue;

		sum += dt;
		if (dt > max)
			max = dt;

		done++;
	}

	if (done == 0) {
		printf("  %-8s : no completed iterations\n", what);
		return -1;
	}

	printf("  %-8s : %4dx%-4d -> %4dx%-4d at %d,%d  %" PRIu64 "/%" PRIu64
	       " us (mean/max)\n",
	       what, sw, sh, lb_w, lb_h, pad_x, pad_y, sum / (uint64_t)done,
	       max);
	return 0;
}

/****************************************************************************
 * Name: v4l2_start
 *
 * Description:
 *   Bring up one capture node the way amp_fb_show does, including reading the
 *   format back.
 *
 *   Read back rather than assumed because the driver clamps sizes without
 *   saying so - capture.c only bounds the request against the input window and
 *   returns success - so the stride that comes back is the only stride worth
 *   handing to the RGA.
 *
 ****************************************************************************/

static int v4l2_start(const char *dev, int w, int h, void **starts,
		      size_t *lengths, unsigned int want, int *sw, int *sh,
		      int *sstride)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	struct v4l2_requestbuffers req;
	struct v4l2_capability cap;
	struct v4l2_format fmt;
	unsigned int i;
	int fd;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", dev, strerror(errno));
		return -1;
	}

	memset(&cap, 0, sizeof(cap));
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
		printf("source node %s: driver \"%s\", card \"%s\"\n", dev,
		       cap.driver, cap.card);

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = type;
	fmt.fmt.pix_mp.width = (uint32_t)w;
	fmt.fmt.pix_mp.height = (uint32_t)h;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;

	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
		fprintf(stderr, "%s: S_FMT: %s\n", dev, strerror(errno));
		close(fd);
		return -1;
	}

	*sw = (int)fmt.fmt.pix_mp.width;
	*sh = (int)fmt.fmt.pix_mp.height;
	*sstride = (int)fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

	if (*sstride == 0)
		*sstride = *sw;

	if (*sw != w || *sh != h)
		printf("  asked for %dx%d, got %dx%d\n", w, h, *sw, *sh);

	memset(&req, 0, sizeof(req));
	req.type = type;
	req.memory = V4L2_MEMORY_MMAP;
	req.count = want;

	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
		fprintf(stderr, "%s: REQBUFS: %s\n", dev, strerror(errno));
		close(fd);
		return -1;
	}

	for (i = 0; i < req.count; i++) {
		struct v4l2_buffer buf;
		struct v4l2_plane planes[VIDEO_MAX_PLANES];

		memset(&buf, 0, sizeof(buf));
		memset(planes, 0, sizeof(planes));
		buf.type = type;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;
		buf.m.planes = planes;
		buf.length = VIDEO_MAX_PLANES;

		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
			fprintf(stderr, "QUERYBUF %u: %s\n", i,
				strerror(errno));
			close(fd);
			return -1;
		}

		lengths[i] = buf.m.planes[0].length;
		starts[i] = mmap(NULL, lengths[i], PROT_READ | PROT_WRITE,
				 MAP_SHARED, fd, buf.m.planes[0].m.mem_offset);

		if (starts[i] == MAP_FAILED) {
			fprintf(stderr, "mmap %u: %s\n", i, strerror(errno));
			close(fd);
			return -1;
		}

		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) {
			fprintf(stderr, "QBUF %u: %s\n", i, strerror(errno));
			close(fd);
			return -1;
		}
	}

	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) {
		fprintf(stderr, "%s: STREAMON: %s\n", dev, strerror(errno));
		close(fd);
		return -1;
	}

	printf("  streaming NV12 %dx%d, y stride %d, %u buffers\n", *sw, *sh,
	       *sstride, req.count);

	return fd;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-w width] [-h height] [-n loops] [-V dev]\n"
		"  -w/-h  source NV12 size (default %dx%d)\n"
		"  -n     iterations (default %d, first discarded)\n"
		"  -V dev capture node to also time a real V4L2 buffer from,\n"
		"         e.g. /dev/video12 - streams it and converts out of\n"
		"         the dequeued buffers, letterboxed as the detector\n"
		"         does, which is the only case that matches what the\n"
		"         detector actually measures\n"
		"\n"
		"Times the two conversions the camera path needs: NV12 to\n"
		"RGB888 640x640 for the detector, and NV12 to RGBA8888\n"
		"512x288 for the display.\n",
		prog, DEF_SRC_W, DEF_SRC_H, DEF_LOOPS);
}

int main(int argc, char **argv)
{
	int sw = DEF_SRC_W;
	int sh = DEF_SRC_H;
	int loops = DEF_LOOPS;
	void *src = NULL;
	void *rgb = NULL;
	void *rgba = NULL;
	const char *v4l2_dev = NULL;
	size_t srcsz;
	int vfd = -1;
	int ret = EXIT_FAILURE;
	int opt;

	setvbuf(stdout, NULL, _IOLBF, 0);

	while ((opt = getopt(argc, argv, "w:h:n:V:H")) != -1) {
		switch (opt) {
		case 'w':
			sw = atoi(optarg);
			break;
		case 'h':
			sh = atoi(optarg);
			break;
		case 'n':
			loops = atoi(optarg);
			if (loops < 2)
				loops = 2;
			break;
		case 'V':
			v4l2_dev = optarg;
			break;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	if (sw <= 0 || sh <= 0 || (sw & 1) || (sh & 1)) {
		fprintf(stderr, "source size must be positive and even\n");
		return EXIT_FAILURE;
	}

	printf("rga version: %s\n", querystring(RGA_VERSION));

	srcsz = (size_t)sw * sh * 3 / 2;
	src = malloc(srcsz);
	rgb = malloc(640 * 640 * 3);
	rgba = malloc(512 * 288 * 4);

	if (src == NULL || rgb == NULL || rgba == NULL) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}

	/* Mid-grey luma, neutral chroma. Touching every page here rather than
	 * inside a timed section, for the same reason the camera staging buffers
	 * are pre-faulted.
	 */

	memset(src, 0x80, srcsz);
	memset(rgb, 0, 640 * 640 * 3);
	memset(rgba, 0, 512 * 288 * 4);

	printf("source: NV12 %dx%d (%zu bytes), %d iterations each"
	       " (first discarded)\n\n", sw, sh, srcsz, loops);

	if (run_case("detect", src, sw, sh, RK_FORMAT_YCbCr_420_SP,
		     rgb, 640, 640, RK_FORMAT_RGB_888, loops) < 0)
		goto out;

	if (run_case("display", src, sw, sh, RK_FORMAT_YCbCr_420_SP,
		     rgba, 512, 288, RK_FORMAT_RGBA_8888, loops) < 0)
		goto out;

	/* With -V, the same conversion again out of a real capture buffer.
	 *
	 * All three cases run in one process, back to back, on purpose. The two
	 * numbers being compared have to come from the same thermal and clock
	 * state: comparing a figure from this run against one recorded in a
	 * changelog days ago is how a difference gets attributed to the wrong
	 * cause, which has already happened once on this task.
	 *
	 * The decomposition:
	 *
	 *   detect     malloc source, nothing streaming
	 *   isp-load   malloc source, the ISP now running - so this minus the
	 *              above is what the capture DMA alone costs the RGA
	 *   v4l2-buf   capture buffer as the source, same ISP load - so this
	 *              minus isp-load is what the buffer alone costs
	 */

	if (v4l2_dev != NULL) {
		void *starts[8];
		size_t lengths[8];
		unsigned int nbufs = 4;
		int vsw = 0;
		int vsh = 0;
		int vstride = 0;
		float scale;
		int lb_w;
		int lb_h;

		printf("\n");

		vfd = v4l2_start(v4l2_dev, sw, sh, starts, lengths, nbufs,
				 &vsw, &vsh, &vstride);
		if (vfd < 0)
			goto out;

		printf("\n");

		/* The same conversion, still out of malloc'd memory, but with the
		 * ISP now writing frames. Isolates the capture DMA from the
		 * buffer.
		 */

		if (run_case("isp-load", src, sw, sh, RK_FORMAT_YCbCr_420_SP,
			     rgb, 640, 640, RK_FORMAT_RGB_888, loops) < 0)
			goto out;

		/* Letterbox exactly as the detector computes it, so the job
		 * being timed is the detector's job and not a larger one.
		 */

		scale = 640.0f / (float)vsw;
		if (640.0f / (float)vsh < scale)
			scale = 640.0f / (float)vsh;

		lb_w = (int)((float)vsw * scale);
		lb_h = (int)((float)vsh * scale);

		if (run_v4l2_case("v4l2-buf", vfd, NULL, starts, nbufs, vsw,
				  vsh, vstride, rgb, 640, 640,
				  RK_FORMAT_RGB_888, (640 - lb_w) / 2,
				  (640 - lb_h) / 2, lb_w, lb_h, loops) < 0)
			goto out;

		printf("\nisp-load minus detect is what the capture DMA costs"
		       " the RGA.\nv4l2-buf minus isp-load is what the source"
		       " buffer costs, and it is\nthe one that has a fix:"
		       " VIDIOC_EXPBUF plus wrapbuffer_fd() instead of a\n"
		       "virtual address, so nothing has to be pinned per"
		       " call.\n");
	} else {
		printf("\nThe detect figure is what sits in front of the NPU's"
		       " 17.3ms.\nThe display figure is what the CPU currently"
		       " spends about 11ms on.\nPass -V /dev/videoN to also"
		       " time a real capture buffer, which is what\nthe"
		       " detector actually converts and is measurably slower.\n");
	}

	ret = EXIT_SUCCESS;

out:
	if (vfd >= 0) {
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		ioctl(vfd, VIDIOC_STREAMOFF, &type);
		close(vfd);
	}

	free(src);
	free(rgb);
	free(rgba);
	return ret;
}
