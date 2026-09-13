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
