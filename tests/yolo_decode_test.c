/* Feed both decodes the same bytes and compare.
 *
 * The tensors are built so that almost every cell is below the confidence
 * threshold and a known handful are above it. Uniform random data would put
 * roughly three quarters of all cells over the line - thres_i8 works out to -64
 * for this model's scale - which would produce fourteen thousand candidates for
 * the first head alone, overrun the 512-entry cap and turn the comparison into a
 * test of the cap rather than of the arithmetic.
 *
 * Sparse peaks also make the index arithmetic the thing under test: reading one
 * cell off in any dimension lands on background and produces a different
 * candidate set immediately.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define HEADS 3
#define CLASSES 80
#define PROPS (5 + CLASSES)
#define ANCHORS 3

void mine_setup(uint32_t in_w, uint32_t in_h, void **bufs, int32_t *zp,
		float *scale);
uint32_t mine_decode(void);
uint32_t mine_nms(uint32_t n);
void mine_get(uint32_t i, float *out, int *cls, int *keep);

int vend_run(void **bufs, int in_w, int in_h, int32_t *zp, float *scale);
void vend_get(int i, float *out, int *cls);
int vend_nms(int n);
int vend_kept(int i);

static uint32_t g_rng = 12345;

static uint32_t rnd(void)
{
	g_rng = g_rng * 1103515245u + 12345u;
	return (g_rng >> 8) & 0xffffff;
}

static int fails;

static void check(const char *what, int ok, const char *detail)
{
	printf("%-4s %-38s %s\n", ok ? "ok" : "FAIL", what, detail);
	if (!ok)
		fails++;
}

int main(int argc, char **argv)
{
	/* Seed from the command line, so the same binary can be run over many
	 * data sets. One seed proves the arithmetic agrees on one arrangement of
	 * peaks; an index error that happens to be masked by a particular layout
	 * would survive that and not survive twenty.
	 */

	if (argc > 1)
		g_rng = (uint32_t)strtoul(argv[1], NULL, 0);


	const int stride[HEADS] = { 8, 16, 32 };
	const int in_w = 640;
	const int in_h = 640;
	int32_t zp[HEADS] = { -128, -128, -128 };
	float scale[HEADS] = { 0.003922f, 0.003922f, 0.003922f };
	void *bufs[HEADS];
	int8_t *raw[HEADS];
	int npeak = 0;
	int h;
	int vn;
	uint32_t mn;
	uint32_t i;
	int worst_cls = 0;
	double worst = 0.0;
	char msg[160];

	for (h = 0; h < HEADS; h++) {
		int gw = in_w / stride[h];
		int gh = in_h / stride[h];
		size_t n = (size_t)ANCHORS * PROPS * gw * gh;
		int k;

		raw[h] = malloc(n);
		bufs[h] = raw[h];

		/* Background: everything comfortably under thres_i8 = -64. */

		for (i = 0; i < n; i++)
			raw[h][i] = (int8_t)(-128 + (int)(rnd() % 50));

		/* Peaks. Objectness and one class raised above the threshold, box
		 * fields given values in the range a real head produces.
		 */

		for (k = 0; k < 14; k++) {
			int a = (int)(rnd() % ANCHORS);
			int gi = (int)(rnd() % (uint32_t)gh);
			int gj = (int)(rnd() % (uint32_t)gw);
			int cls = (int)(rnd() % CLASSES);
			int gl = gw * gh;
			int base = (PROPS * a) * gl + gi * gw + gj;
			int p;

			for (p = 0; p < 4; p++)
				raw[h][base + p * gl] =
					(int8_t)(-100 + (int)(rnd() % 180));

			raw[h][base + 4 * gl] =
				(int8_t)(-60 + (int)(rnd() % 180));
			raw[h][base + (5 + cls) * gl] =
				(int8_t)(-55 + (int)(rnd() % 175));
			npeak++;
		}

		/* Clusters, so that NMS has something to suppress.
		 *
		 * Scattered peaks almost never overlap, which makes a survivor
		 * comparison vacuous - both sides keep everything and agreeing on
		 * that proves nothing. These put four peaks of one class in
		 * adjacent cells with near-identical box sizes, which decodes to
		 * four boxes a few pixels apart and forces real suppression. This
		 * is the part worth proving, because the single-pass NMS here is
		 * a deliberate departure from the reference's per-class loop.
		 */

		for (k = 0; k < 3; k++) {
			int a = (int)(rnd() % ANCHORS);
			int cls = (int)(rnd() % CLASSES);
			int gi = 4 + (int)(rnd() % (uint32_t)(gh - 8));
			int gj = 4 + (int)(rnd() % (uint32_t)(gw - 8));
			int gl = gw * gh;
			int wq = 40 + (int)(rnd() % 30);
			int m;

			for (m = 0; m < 4; m++) {
				int ii = gi + (m & 1);
				int jj = gj + ((m >> 1) & 1);
				int base = (PROPS * a) * gl + ii * gw + jj;

				/* Centre near the cell centre and a shared size,
				 * so neighbouring cells produce boxes that
				 * genuinely overlap.
				 */

				raw[h][base + 0 * gl] = (int8_t)0;
				raw[h][base + 1 * gl] = (int8_t)0;
				raw[h][base + 2 * gl] = (int8_t)wq;
				raw[h][base + 3 * gl] = (int8_t)wq;

				/* Distinct objectness per member, so the score
				 * order is unambiguous and a tie cannot make the
				 * two sorts disagree about which one survives.
				 */

				raw[h][base + 4 * gl] = (int8_t)(40 + m * 12);
				raw[h][base + (5 + cls) * gl] =
					(int8_t)(60 + m * 9);
				npeak++;
			}
		}
	}

	snprintf(msg, sizeof(msg), "%d peaks injected", npeak);
	check("test data built", npeak == 78, msg);

	mine_setup((uint32_t)in_w, (uint32_t)in_h, bufs, zp, scale);
	mn = mine_decode();
	vn = vend_run(bufs, in_w, in_h, zp, scale);

	snprintf(msg, sizeof(msg), "mine %u, reference %d", mn, vn);
	check("same candidate count", (int)mn == vn, msg);

	if ((int)mn != vn)
		goto done;

	/* Same order, because both iterate anchor then row then column. A
	 * mismatch in the traversal would show up here as well as in the values.
	 */

	for (i = 0; i < mn; i++) {
		float a[5];
		float b[5];
		int ca;
		int cb;
		int keep;
		int f;

		mine_get(i, a, &ca, &keep);
		vend_get((int)i, b, &cb);

		if (ca != cb) {
			snprintf(msg, sizeof(msg),
				 "candidate %u: mine cls %d, reference %d", i,
				 ca, cb);
			check("class ids agree", 0, msg);
			goto done;
		}

		for (f = 0; f < 5; f++) {
			double d = fabs((double)a[f] - (double)b[f]);
			double s = fabs((double)b[f]);
			double rel = s > 1e-6 ? d / s : d;

			if (rel > worst) {
				worst = rel;
				worst_cls = f;
			}
		}
	}

	/* A tolerance rather than bitwise equality, and the reason is in the
	 * reference: it writes "* 2.0 - 0.5" with double literals, so the
	 * intermediate is computed in double and then stored to a float, while this
	 * port keeps the whole expression in float. That is a double-rounding
	 * difference of at most an ulp or so. Anything structurally wrong - an index
	 * off by one, the wrong anchor, a stray sigmoid - moves these numbers by
	 * whole percentages, not by an ulp.
	 */

	snprintf(msg, sizeof(msg), "worst relative delta %.3g on field %d",
		 worst, worst_cls);
	check("box and score values agree", worst < 1e-5, msg);

	/* NMS. Compared as sets: both suppress the same boxes, but the surviving
	 * order depends on the sort, and neither sort is stable.
	 */

	{
		int vkept = vend_nms(vn);
		uint32_t mkept = mine_nms(mn);
		int mine_keep_n = 0;
		double mine_sum = 0.0;
		double vend_sum = 0.0;

		snprintf(msg, sizeof(msg), "mine %u, reference %d", mkept,
			 vkept);
		check("same survivor count", (int)mkept == vkept, msg);

		/* That suppression happened at all. Without this the two checks
		 * above pass trivially on data where nothing overlaps, which is
		 * how a broken NMS would go unnoticed.
		 */

		snprintf(msg, sizeof(msg), "%d of %d candidates suppressed",
			 vn - vkept, vn);
		check("NMS actually suppressed boxes", vkept < vn, msg);

		/* The same boxes, identified by a sum over their coordinates.
		 * Comparing sets element-wise would need a canonical order that
		 * neither side produces; a sum over all survivors is order
		 * independent and would move if any different box had survived.
		 */

		for (i = 0; i < mn; i++) {
			float a[5];
			int ca;
			int keep;

			mine_get(i, a, &ca, &keep);
			if (!keep)
				continue;

			mine_keep_n++;
			mine_sum += (double)a[0] + a[1] + a[2] + a[3] +
				    ca * 1000.0;
		}

		for (i = 0; i < (uint32_t)vn; i++) {
			int idx = vend_kept((int)i);
			float b[5];
			int cb;

			if (idx < 0)
				continue;

			vend_get(idx, b, &cb);
			vend_sum += (double)b[0] + b[1] + b[2] + b[3] +
				    cb * 1000.0;
		}

		snprintf(msg, sizeof(msg), "mine %.4f, reference %.4f",
			 mine_sum, vend_sum);
		check("same set of survivors",
		      fabs(mine_sum - vend_sum) < 0.05 &&
		      mine_keep_n == vkept, msg);
	}

done:
	for (h = 0; h < HEADS; h++)
		free(raw[h]);

	printf("%s\n", fails == 0 ? "decode matches the reference" :
	       "DECODE DIVERGES FROM THE REFERENCE");

	return fails != 0;
}
