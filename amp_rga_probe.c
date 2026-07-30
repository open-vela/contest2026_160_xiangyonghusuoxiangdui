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
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-w width] [-h height] [-n loops]\n"
		"  -w/-h  source NV12 size (default %dx%d)\n"
		"  -n     iterations (default %d, first discarded)\n"
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
	size_t srcsz;
	int ret = EXIT_FAILURE;
	int opt;

	setvbuf(stdout, NULL, _IOLBF, 0);

	while ((opt = getopt(argc, argv, "w:h:n:H")) != -1) {
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

	printf("\nThe detect figure is what sits in front of the NPU's 17.3ms."
	       "\nThe display figure is what the CPU currently spends about"
	       " 11ms on.\n");

	ret = EXIT_SUCCESS;

out:
	free(src);
	free(rgb);
	free(rgba);
	return ret;
}
