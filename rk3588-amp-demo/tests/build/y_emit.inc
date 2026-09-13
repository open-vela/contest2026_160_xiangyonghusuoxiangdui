static uint32_t det_emit(struct detector *d, uint32_t n,
			 struct amp_det_box *box)
{
	/* Scale into the frame as the sensor saw it, which is the published frame
	 * with its sides swapped back when the picture is being turned. Doing it
	 * this way round keeps one multiply per edge: scaling straight into the
	 * turned frame would need the two factors crossed over, which is the same
	 * arithmetic written so that nothing downstream can tell which space a
	 * number is in.
	 */
	uint32_t lw = (d->rot != 0) ? d->height : d->width;
	uint32_t lh = (d->rot != 0) ? d->width : d->height;
	float kx = (float)lw / (float)d->lb_w;
	float ky = (float)lh / (float)d->lb_h;
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
		/* Turn the rectangle the same way RGA turned the pixels. Both
		 * angles are clockwise, which is what HAL_TRANSFORM_ROT_90 and
		 * ROT_270 mean - see drmrga.h, where the names are documented as
		 * rotating the source clockwise. Getting this backwards puts every
		 * box 180 degrees from its subject, which looks like a broken model
		 * rather than a sign flip, so the direction is taken from the
		 * header rather than from what looks right on one board.
		 *
		 * A clockwise quarter turn sends (x, y) to (lh - y, x): the
		 * landscape height becomes the published width. Both edges of each
		 * axis move, and they swap ends, so the rectangle is rebuilt rather
		 * than adjusted in place.
		 */
		if (d->rot == 90) {
			float rx0 = (float)lh - y1;
			float ry0 = x0;
			float rx1 = (float)lh - y0;
			float ry1 = x1;
			x0 = rx0;
			y0 = ry0;
			x1 = rx1;
			y1 = ry1;
		} else if (d->rot == 270) {
			float rx0 = y0;
			float ry0 = (float)lw - x1;
			float rx1 = y1;
			float ry1 = (float)lw - x0;
			x0 = rx0;
			y0 = ry0;
			x1 = rx1;
			y1 = ry1;
		}

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
