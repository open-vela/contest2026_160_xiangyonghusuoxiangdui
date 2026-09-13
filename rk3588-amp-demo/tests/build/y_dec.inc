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
