/* det_emit(), under test.
 *
 * Two things had no host coverage until now: the confidence filter, and the
 * inverse coordinate transform. The second is the more important of the two -
 * it decides where every box lands, and its failure mode is boxes drawn in the
 * wrong place, which on a screen reads as a bad detector rather than as bad
 * arithmetic. It is also the one piece of this pipeline that no amount of
 * looking at the console can check.
 *
 * The geometry is the board's real one: 1280x720 captured, letterboxed into
 * 640x640 as 640x360 at y=140, published as 512x288.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define YOLO_CLASSES  80
#define YOLO_HEADS    3
#define YOLO_MAX_CAND 512
#define AMP_DET_MAXBOX 64

struct amp_det_box {
	uint16_t x;
	uint16_t y;
	uint16_t w;
	uint16_t h;
	uint8_t cls;
	uint8_t score;
	uint8_t reserved;
};

struct yolo_cand {
	float x;
	float y;
	float w;
	float h;
	float score;
	int cls;
	bool keep;
};

struct detector {
	uint32_t width;
	uint32_t height;
	uint32_t lb_pad_x;
	uint32_t lb_pad_y;
	uint32_t lb_w;
	uint32_t lb_h;
	uint32_t min_score;
	uint32_t filtered;
	struct yolo_cand cand[YOLO_MAX_CAND];
};

#include "y_emit.inc"

static int fails;

static void check(const char *what, int ok, const char *detail)
{
	printf("%-4s %-40s %s\n", ok ? "ok" : "FAIL", what, detail);
	if (!ok)
		fails++;
}

static struct detector d;

static void setup(uint32_t minscore)
{
	memset(&d, 0, sizeof(d));
	d.width = 512;
	d.height = 288;
	d.lb_w = 640;
	d.lb_h = 360;
	d.lb_pad_x = 0;
	d.lb_pad_y = 140;
	d.min_score = minscore;
}

static void put(int i, float x, float y, float w, float h, float score, int cls)
{
	d.cand[i].x = x;
	d.cand[i].y = y;
	d.cand[i].w = w;
	d.cand[i].h = h;
	d.cand[i].score = score;
	d.cand[i].cls = cls;
	d.cand[i].keep = true;
}

int main(void)
{
	struct amp_det_box box[AMP_DET_MAXBOX];
	char msg[200];
	uint32_t n;
	int i;

	/* The whole letterboxed image should become the whole published frame.
	 * If the two scale factors were ever derived differently, this is where
	 * the mismatch shows.
	 */

	setup(0);
	put(0, 0.0f, 140.0f, 640.0f, 360.0f, 0.9f, 0);
	n = det_emit(&d, 1, box);
	snprintf(msg, sizeof(msg), "%u box: %u,%u %ux%u", n, box[0].x, box[0].y,
		 box[0].w, box[0].h);
	check("full image maps to full frame",
	      n == 1 && box[0].x == 0 && box[0].y == 0 && box[0].w == 512 &&
	      box[0].h == 288, msg);

	/* Dead centre. Horizontally 320 of 640 is the middle; vertically 320 sits
	 * halfway down the 140..500 band the image occupies, so both must land on
	 * the centre of a 512x288 frame. This is the check that catches the pad
	 * being applied to the wrong axis, which a symmetric test cannot.
	 */

	setup(0);
	put(0, 320.0f - 32.0f, 320.0f - 32.0f, 64.0f, 64.0f, 0.9f, 0);
	n = det_emit(&d, 1, box);

	/* Asserted on the box centre rather than its corner. The centre is the
	 * property that matters and the one that has an exact expected value;
	 * writing the expected corner down means deriving it by hand, which is how
	 * a correct transform gets "corrected" to match a mistaken expectation.
	 */

	{
		int cx = box[0].x + box[0].w / 2;
		int cy = box[0].y + box[0].h / 2;

		snprintf(msg, sizeof(msg),
			 "%u,%u %ux%u, centre %d,%d (want 256,144)", box[0].x,
			 box[0].y, box[0].w, box[0].h, cx, cy);
		check("centre box lands centred",
		      n == 1 && abs(cx - 256) <= 1 && abs(cy - 144) <= 1 &&
		      box[0].w == 51 && box[0].h == 51, msg);
	}

	/* A box entirely inside the top padding. Nothing is ever detected there,
	 * but a spurious candidate must not be published as a sliver along the top
	 * edge - and must not come out with the reference's looser clamp, which
	 * would let it reach past the bottom of the source.
	 */

	setup(0);
	put(0, 100.0f, 10.0f, 80.0f, 60.0f, 0.9f, 0);
	n = det_emit(&d, 1, box);
	snprintf(msg, sizeof(msg), "published %u", n);
	check("box inside the padding is dropped", n == 0, msg);

	/* Clamping at the bottom. A box running off the end of the image must stop
	 * at the frame edge, not past it.
	 */

	setup(0);
	put(0, 0.0f, 400.0f, 640.0f, 400.0f, 0.9f, 0);
	n = det_emit(&d, 1, box);
	snprintf(msg, sizeof(msg), "y %u h %u, bottom %u (frame 288)", box[0].y,
		 box[0].h, box[0].y + box[0].h);
	check("overhanging box clamps to the frame",
	      n == 1 && box[0].y + box[0].h <= 288, msg);

	/* The filter, on the published integer. 34 must go and 35 must stay when
	 * the threshold is 35 - the boundary is the only part of a threshold worth
	 * testing.
	 */

	setup(35);
	put(0, 100.0f, 200.0f, 60.0f, 60.0f, 0.349f, 0);   /* 34% */
	put(1, 200.0f, 200.0f, 60.0f, 60.0f, 0.35f, 1);    /* 35% */
	put(2, 300.0f, 200.0f, 60.0f, 60.0f, 0.99f, 2);    /* 99% */
	n = det_emit(&d, 3, box);
	snprintf(msg, sizeof(msg), "kept %u, dropped %u, scores %u %u", n,
		 d.filtered, n > 0 ? box[0].score : 0,
		 n > 1 ? box[1].score : 0);
	check("filter keeps 35 and drops 34",
	      n == 2 && d.filtered == 1 && box[0].score == 35 &&
	      box[1].score == 99, msg);

	/* Threshold 0 must publish everything, so the option can still be used to
	 * see what the model actually emits.
	 */

	setup(0);
	put(0, 100.0f, 200.0f, 60.0f, 60.0f, 0.07f, 0);
	n = det_emit(&d, 1, box);
	snprintf(msg, sizeof(msg), "kept %u score %u", n,
		 n > 0 ? box[0].score : 0);
	check("threshold 0 publishes everything",
	      n == 1 && box[0].score == 7, msg);

	/* Suppressed candidates never reach the output regardless of score. */

	setup(0);
	put(0, 100.0f, 200.0f, 60.0f, 60.0f, 0.99f, 0);
	d.cand[0].keep = false;
	n = det_emit(&d, 1, box);
	snprintf(msg, sizeof(msg), "published %u", n);
	check("NMS-suppressed box is not published", n == 0, msg);

	/* The transport ceiling. More survivors than the block can carry must be
	 * truncated, and because det_emit walks a score-sorted list the ones kept
	 * are the highest - losing the weakest rather than an arbitrary 64.
	 */

	setup(0);
	for (i = 0; i < 100; i++)
		put(i, (float)(i * 6), 200.0f, 20.0f, 20.0f,
		    0.99f - (float)i * 0.005f, i % YOLO_CLASSES);

	n = det_emit(&d, 100, box);

	{
		int ordered = 1;
		uint32_t k;

		for (k = 1; k < n; k++)
			if (box[k].score > box[k - 1].score)
				ordered = 0;

		snprintf(msg, sizeof(msg),
			 "%u published, %u%% down to %u%%, ordered %d", n,
			 box[0].score, box[n - 1].score, ordered);
		check("truncates at capacity, keeping the best",
		      n == AMP_DET_MAXBOX && box[0].score == 99 && ordered &&
		      box[0].score > box[n - 1].score, msg);
	}

	printf("%s\n", fails == 0 ? "emit and transform correct" :
	       "EMIT/TRANSFORM WRONG");

	return fails != 0;
}
