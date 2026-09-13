/* The reference decode, also verbatim.
 *
 * Extracted from rknn_yolov5_demo/src/postprocess.cc so that a disagreement can
 * only be my code or theirs, not my paraphrase of theirs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <vector>
#include <set>
#include <algorithm>

#define OBJ_CLASS_NUM     80
#define PROP_BOX_SIZE     (5 + OBJ_CLASS_NUM)
#define NMS_THRESH        0.45
#define BOX_THRESH        0.25
#define OBJ_NUMB_MAX_SIZE 64

#include "vend.inc"

extern "C" {

static std::vector<float> g_boxes;
static std::vector<float> g_probs;
static std::vector<int> g_cls;
static std::vector<int> g_order;

int vend_run(void **bufs, int in_w, int in_h, int32_t *zp, float *scale)
{
	static const int a0[6] = { 10, 13, 16, 30, 33, 23 };
	static const int a1[6] = { 30, 61, 62, 45, 59, 119 };
	static const int a2[6] = { 116, 90, 156, 198, 373, 326 };
	const int *anch[3] = { a0, a1, a2 };
	const int stride[3] = { 8, 16, 32 };
	int total = 0;
	int h;

	g_boxes.clear();
	g_probs.clear();
	g_cls.clear();

	for (h = 0; h < 3; h++) {
		int gw = in_w / stride[h];
		int gh = in_h / stride[h];

		total += process((int8_t *)bufs[h], (int *)anch[h], gh, gw,
				 in_h, in_w, stride[h], g_boxes, g_probs,
				 g_cls, BOX_THRESH, zp[h], scale[h]);
	}

	return total;
}

void vend_get(int i, float *out, int *cls)
{
	out[0] = g_boxes[i * 4 + 0];
	out[1] = g_boxes[i * 4 + 1];
	out[2] = g_boxes[i * 4 + 2];
	out[3] = g_boxes[i * 4 + 3];
	out[4] = g_probs[i];
	*cls = g_cls[i];
}

/* The reference's NMS, driven exactly as its caller drives it: sort all
 * candidates by score descending, then suppress once per class present.
 */

int vend_nms(int n)
{
	std::set<int> present;
	int kept = 0;
	int i;

	g_order.clear();
	for (i = 0; i < n; i++)
		g_order.push_back(i);

	quick_sort_indice_inverse(g_probs, 0, n - 1, g_order);

	for (i = 0; i < n; i++)
		present.insert(g_cls[i]);

	for (std::set<int>::iterator it = present.begin(); it != present.end();
	     ++it)
		nms(n, g_boxes, g_cls, g_order, *it, NMS_THRESH);

	for (i = 0; i < n; i++)
		if (g_order[i] != -1)
			kept++;

	return kept;
}

/* Survivors in the reference's own order, so the harness can compare sets. */

int vend_kept(int i)
{
	return g_order[i];
}

} /* extern "C" */
