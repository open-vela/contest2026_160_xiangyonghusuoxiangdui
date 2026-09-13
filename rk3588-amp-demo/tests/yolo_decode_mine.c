/* My decode, under test.
 *
 * The functions are pulled verbatim out of amp_fb_show.c by awk, so this tests
 * the shipping code rather than a transcription of it. Only the context they need
 * is provided here: a struct with the same field names, which is all yolo_decode
 * and yolo_nms actually touch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define YOLO_CLASSES     80
#define YOLO_ANCHORS     3
#define YOLO_PROPS       (5 + YOLO_CLASSES)
#define YOLO_HEADS       3
#define YOLO_BOX_THRESH  0.25f
#define YOLO_NMS_THRESH  0.45f
#define YOLO_MAX_CAND    512

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
	uint32_t in_w;
	uint32_t in_h;
	uint32_t n_out;
	int32_t out_zp[YOLO_HEADS];
	float out_scale[YOLO_HEADS];
	void *out_buf[YOLO_HEADS];
	struct yolo_cand cand[YOLO_MAX_CAND];
};

#include "y_tab.inc"
#include "y_dec.inc"
#include "y_nms.inc"

/* The harness reaches in through these, so the extracted functions stay
 * untouched.
 */

static struct detector g_d;

void mine_setup(uint32_t in_w, uint32_t in_h, void **bufs, int32_t *zp,
		float *scale)
{
	uint32_t i;

	memset(&g_d, 0, sizeof(g_d));
	g_d.in_w = in_w;
	g_d.in_h = in_h;
	g_d.n_out = YOLO_HEADS;

	for (i = 0; i < YOLO_HEADS; i++) {
		g_d.out_buf[i] = bufs[i];
		g_d.out_zp[i] = zp[i];
		g_d.out_scale[i] = scale[i];
	}
}

uint32_t mine_decode(void)
{
	uint32_t got = 0;
	uint32_t i;

	for (i = 0; i < YOLO_HEADS; i++)
		got = yolo_decode(&g_d, i, got, YOLO_BOX_THRESH);

	return got;
}

uint32_t mine_nms(uint32_t n)
{
	return yolo_nms(&g_d, n, YOLO_NMS_THRESH);
}

void mine_get(uint32_t i, float *out, int *cls, int *keep)
{
	out[0] = g_d.cand[i].x;
	out[1] = g_d.cand[i].y;
	out[2] = g_d.cand[i].w;
	out[3] = g_d.cand[i].h;
	out[4] = g_d.cand[i].score;
	*cls = g_d.cand[i].cls;
	*keep = g_d.cand[i].keep ? 1 : 0;
}
