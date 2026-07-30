/*
 * amp_npu_probe - is the NPU alive on this board, and how fast is it?
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * This is step one of putting a detector behind the camera. It answers the three
 * things that have to be true before any of the rest is worth writing, and
 * nothing else:
 *
 *   1. Does the kernel driver actually work? The device tree enables rknpu and
 *      the driver is built in, but neither of those was ever confirmed against
 *      hardware. RKNN_QUERY_SDK_VERSION returns a driver version string that can
 *      only come from a live driver, so it either prints one or it does not.
 *
 *   2. What does the model want as input? That number decides what the RGA has
 *      to produce, which decides what the second ISP stream has to be set to.
 *      Guessing it is how one ends up converting to the wrong size for a week.
 *
 *   3. How long does one inference take, per NPU core? The detector's frame rate
 *      follows from this and nothing else, and the answer decides whether the
 *      display and the detector can share one core or need separate ones.
 *
 * Deliberately no camera, no RGA, no shared memory and no rpmsg. Synthetic input
 * is enough to time the hardware, and keeping the first step this small means a
 * failure here is unambiguous instead of being one of five candidates.
 *
 * Two clocks are reported, and the difference matters. Wall time around
 * rknn_run() is what a caller experiences; RKNN_QUERY_PERF_RUN is what the
 * hardware reports for the inference itself. If those diverge, the gap is
 * software overhead and belongs in a different budget than the NPU's.
 *
 * Build:
 *   aarch64-linux-gnu-gcc -O2 -Wall -Wextra \
 *     -I<rknpu2>/runtime/Linux/librknn_api/include \
 *     -o amp_npu_probe amp_npu_probe.c \
 *     -L<rknpu2>/runtime/Linux/librknn_api/aarch64 -lrknnrt
 *
 * On the board librknnrt.so has to be findable - either installed in
 * /usr/lib or reached through LD_LIBRARY_PATH.
 */

/* Before any system header, for sched_getcpu(). */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "rknn_api.h"

#define DEF_MODEL   "/userdata/yolov5s-640-640.rknn"
#define DEF_LOOPS   50

struct corespec {
	const char *name;
	uint32_t mask;
};

/* Every core on its own first, then all three together.
 *
 * Individually because the interesting question for this board is what one core
 * gives us - the display path is already spending a core's worth of time on the
 * A55 side, and a detector that only needs one NPU core leaves the other two for
 * whatever comes next. All three together as the upper bound, to see whether the
 * runtime actually splits work across them or just picks one.
 */

static const struct corespec g_cores[] = {
	{ "auto",  RKNN_NPU_CORE_AUTO },
	{ "core0", RKNN_NPU_CORE_0 },
	{ "core1", RKNN_NPU_CORE_1 },
	{ "core2", RKNN_NPU_CORE_2 },
	{ "0+1+2", RKNN_NPU_CORE_0_1_2 },
};

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)(ts.tv_nsec / 1000);
}

static void *load_file(const char *path, uint32_t *size)
{
	FILE *fp = fopen(path, "rb");
	long len;
	void *buf;

	if (fp == NULL) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return NULL;
	}

	if (fseek(fp, 0, SEEK_END) != 0) {
		perror("fseek");
		fclose(fp);
		return NULL;
	}

	len = ftell(fp);
	if (len <= 0) {
		fprintf(stderr, "%s: empty or unseekable\n", path);
		fclose(fp);
		return NULL;
	}

	rewind(fp);

	buf = malloc((size_t)len);
	if (buf == NULL) {
		fprintf(stderr, "cannot allocate %ld bytes for the model\n", len);
		fclose(fp);
		return NULL;
	}

	if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		fprintf(stderr, "%s: short read\n", path);
		free(buf);
		fclose(fp);
		return NULL;
	}

	fclose(fp);
	*size = (uint32_t)len;
	return buf;
}

static const char *fmt_name(rknn_tensor_format f)
{
	switch (f) {
	case RKNN_TENSOR_NCHW:
		return "NCHW";
	case RKNN_TENSOR_NHWC:
		return "NHWC";
	case RKNN_TENSOR_NC1HWC2:
		return "NC1HWC2";
	default:
		return "?";
	}
}

static const char *type_name(rknn_tensor_type t)
{
	switch (t) {
	case RKNN_TENSOR_FLOAT32:
		return "fp32";
	case RKNN_TENSOR_FLOAT16:
		return "fp16";
	case RKNN_TENSOR_INT8:
		return "int8";
	case RKNN_TENSOR_UINT8:
		return "uint8";
	case RKNN_TENSOR_INT16:
		return "int16";
	default:
		return "?";
	}
}

static void print_attr(const char *what, const rknn_tensor_attr *a)
{
	unsigned i;

	printf("  %s[%u] \"%s\" ", what, a->index, a->name);

	for (i = 0; i < a->n_dims; i++)
		printf("%s%u", i ? "x" : "", a->dims[i]);

	printf(" %s %s, %u bytes", fmt_name(a->fmt), type_name(a->type),
	       a->size);

	if (a->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC)
		printf(", zp %d scale %.6f", a->zp, a->scale);

	printf("\n");
}

/****************************************************************************
 * Name: run_core
 *
 * Description:
 *   Time loops iterations with the given core mask. Reports wall time around
 *   rknn_run() and the hardware's own figure separately, mean and max for each.
 *
 *   Mean as well as max, because a max on its own has already misled this
 *   project twice this week: it cannot tell a loop that is always slow from one
 *   that was descheduled once, and those want completely different responses.
 *
 ****************************************************************************/

static int run_core(rknn_context ctx, const struct corespec *cs,
		    rknn_input *inputs, uint32_t n_input, uint32_t n_output,
		    void **out_bufs, const uint32_t *out_sizes, int loops)
{
	uint64_t set_sum = 0;
	uint64_t set_max = 0;
	uint64_t wall_sum = 0;
	uint64_t wall_max = 0;
	uint64_t get_sum = 0;
	uint64_t get_max = 0;
	uint64_t hw_sum = 0;
	uint64_t hw_max = 0;
	int done = 0;
	int i;
	int ret;

	ret = rknn_set_core_mask(ctx, cs->mask);
	if (ret < 0) {
		printf("  %-5s : rknn_set_core_mask failed (%d) - skipped\n",
		       cs->name, ret);
		return 0;
	}

	for (i = 0; i < loops; i++) {
		rknn_output outputs[16];
		rknn_perf_run perf;
		uint64_t t0;
		uint64_t dt;
		uint64_t dset;
		uint64_t dget = 0;

		/* Timed, and it was not in the first version of this - which was
		 * a real gap rather than an omission for brevity.
		 *
		 * The model's input tensor is int8 while an image arriving from
		 * the RGA is uint8, so rknn_inputs_set() converts 1.2MB on the
		 * CPU on every single inference. Leaving that outside the
		 * measurement made the NPU look free when part of the per-frame
		 * cost had simply been placed where nobody was looking - the same
		 * mistake as timing only the conversion in the camera path and
		 * wondering where the time went.
		 */

		t0 = now_us();
		ret = rknn_inputs_set(ctx, n_input, inputs);
		dset = now_us() - t0;

		if (ret < 0) {
			printf("  %-5s : rknn_inputs_set failed (%d)\n",
			       cs->name, ret);
			return -1;
		}

		t0 = now_us();
		ret = rknn_run(ctx, NULL);
		dt = now_us() - t0;

		if (ret < 0) {
			printf("  %-5s : rknn_run failed (%d)\n", cs->name,
			       ret);
			return -1;
		}

		/* The outputs are fetched even though nothing is done with them,
		 * because not fetching them would leave the run unfinished from
		 * the runtime's point of view and time something other than a
		 * complete inference.
		 */

		memset(outputs, 0, sizeof(outputs));
		if (n_output <= 16) {
			uint32_t k;

			for (k = 0; k < n_output; k++) {
				outputs[k].index = k;
				outputs[k].want_float = 0;

				/* Caller-owned buffers, allocated once by the
				 * caller rather than by the runtime on every
				 * inference.
				 *
				 * The first version let the runtime allocate,
				 * which is what the vendor demo does, and the
				 * measurements then showed something odd: the
				 * output fetch got slower the longer the test
				 * ran - 2.1ms early, 3.4ms after a few hundred
				 * inferences - while the NPU's own time did not
				 * move at all. Only the phase that allocates
				 * degraded.
				 *
				 * 2.1MB per call is far above glibc's mmap
				 * threshold, so every get/release pair can
				 * become an mmap/munmap with five hundred page
				 * faults and a zeroing pass. Pre-allocating
				 * removes that from the measurement, and it is
				 * what a detector running continuously has to do
				 * regardless.
				 */

				if (out_bufs != NULL && out_bufs[k] != NULL) {
					outputs[k].is_prealloc = 1;
					outputs[k].buf = out_bufs[k];
					outputs[k].size = out_sizes[k];
				}
			}

			/* Also timed. This model's three output tensors are
			 * 2.1MB between them, and everything downstream -
			 * dequantise, threshold, NMS over 25200 boxes - starts
			 * by reading all of it. Knowing what the fetch alone
			 * costs separates the runtime's share from the
			 * post-processing that has yet to be written.
			 *
			 * want_float stays 0 on purpose: asking for float would
			 * have the runtime convert 2.1MB to 8.4MB and time that
			 * as well, which is a cost a real detector avoids by
			 * dequantising only the boxes that pass the threshold.
			 */

			t0 = now_us();
			ret = rknn_outputs_get(ctx, n_output, outputs, NULL);
			dget = now_us() - t0;

			/* Only released when the runtime owns the buffers.
			 * Releasing pre-allocated ones would hand the caller's
			 * memory to the runtime's free path.
			 */

			if (ret == 0 && (out_bufs == NULL))
				rknn_outputs_release(ctx, n_output, outputs);
		}

		/* Discard the first iteration. The first run pays for lazy
		 * allocation inside the runtime and would set a maximum that
		 * never recurs - the same first-touch effect that polluted the
		 * camera timings until the buffers were pre-faulted.
		 */

		if (i == 0)
			continue;

		set_sum += dset;
		if (dset > set_max)
			set_max = dset;

		get_sum += dget;
		if (dget > get_max)
			get_max = dget;

		wall_sum += dt;
		if (dt > wall_max)
			wall_max = dt;

		memset(&perf, 0, sizeof(perf));
		if (rknn_query(ctx, RKNN_QUERY_PERF_RUN, &perf,
			       sizeof(perf)) == 0 && perf.run_duration > 0) {
			uint64_t hw = (uint64_t)perf.run_duration;

			hw_sum += hw;
			if (hw > hw_max)
				hw_max = hw;
		}

		done++;
	}

	if (done == 0) {
		printf("  %-5s : no completed iterations\n", cs->name);
		return -1;
	}

	{
		uint64_t n = (uint64_t)done;
		uint64_t total = set_sum + wall_sum + get_sum;

		/* The core this ran on, because the CPU phases are memcpy-bound
		 * and this SoC's little and big cores are not interchangeable -
		 * a figure from an A55 and one from an A76 are different
		 * measurements wearing the same label. The camera program
		 * already reported this; leaving it out here was an
		 * inconsistency that cost a round of guessing.
		 */

		printf("  %-5s : cpu%-2d set %" PRIu64 "/%" PRIu64
		       "  run %" PRIu64 "/%" PRIu64
		       "  get %" PRIu64 "/%" PRIu64 " us",
		       cs->name, sched_getcpu(), set_sum / n, set_max,
		       wall_sum / n, wall_max, get_sum / n, get_max);

		if (hw_sum != 0)
			printf("  hw %" PRIu64, hw_sum / n);

		/* The rate that matters is the whole per-inference cost, not the
		 * NPU's share of it. Reporting only the run figure was flattering
		 * to the hardware and useless for sizing a pipeline.
		 */

		printf("  | total %" PRIu64 " us -> %" PRIu64 " fps\n",
		       total / n,
		       total ? (uint64_t)(1000000ull * n / total) : (uint64_t)0);
	}

	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [-m model.rknn] [-n loops]\n"
		"  -m model   .rknn file (default %s)\n"
		"  -n loops   iterations per core (default %d)\n"
		"\n"
		"Reports the driver version, the model's input geometry and the\n"
		"inference time on each NPU core. No camera and no shared\n"
		"memory: this only establishes that the NPU works and how fast\n"
		"it is.\n",
		prog, DEF_MODEL, DEF_LOOPS);
}

int main(int argc, char **argv)
{
	const char *model_path = DEF_MODEL;
	int loops = DEF_LOOPS;
	rknn_context ctx = 0;
	rknn_sdk_version ver;
	rknn_input_output_num io;
	rknn_input *inputs = NULL;
	void **out_bufs = NULL;
	uint32_t *out_sizes = NULL;
	void *model = NULL;
	uint32_t model_size = 0;
	unsigned i;
	int ret = EXIT_FAILURE;
	int opt;

	setvbuf(stdout, NULL, _IOLBF, 0);

	while ((opt = getopt(argc, argv, "m:n:h")) != -1) {
		switch (opt) {
		case 'm':
			model_path = optarg;
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

	model = load_file(model_path, &model_size);
	if (model == NULL)
		return EXIT_FAILURE;

	printf("model: %s, %u bytes\n", model_path, model_size);

	if (rknn_init(&ctx, model, model_size, 0, NULL) < 0) {
		fprintf(stderr,
			"rknn_init failed. If this is the first thing that\n"
			"failed, check the driver came up:\n"
			"    dmesg | grep rknpu\n"
			"A working driver prints \"Initialized rknpu\".\n");
		free(model);
		return EXIT_FAILURE;
	}

	/* The decisive check. drv_version can only be filled in by a driver that
	 * answered an ioctl, so a version here means the whole path - device tree
	 * node, driver probe, character device, runtime - is live.
	 */

	memset(&ver, 0, sizeof(ver));
	if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) < 0) {
		fprintf(stderr, "RKNN_QUERY_SDK_VERSION failed\n");
		goto out;
	}

	printf("api: %s\ndriver: %s\n", ver.api_version, ver.drv_version);

	memset(&io, 0, sizeof(io));
	if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) < 0) {
		fprintf(stderr, "RKNN_QUERY_IN_OUT_NUM failed\n");
		goto out;
	}

	printf("tensors: %u in, %u out\n", io.n_input, io.n_output);

	inputs = calloc(io.n_input, sizeof(*inputs));
	if (inputs == NULL) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}

	for (i = 0; i < io.n_input; i++) {
		rknn_tensor_attr attr;

		memset(&attr, 0, sizeof(attr));
		attr.index = i;
		if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &attr,
			       sizeof(attr)) < 0) {
			fprintf(stderr, "input attr %u failed\n", i);
			goto out;
		}

		print_attr("in ", &attr);

		/* Synthetic input, and that is the point: this measures the
		 * hardware, not the model's accuracy. A mid-grey frame keeps the
		 * numbers away from any special-cased all-zero path.
		 *
		 * uint8 NHWC because that is what an image-input model wants and
		 * what the RGA will hand it later; asking the runtime to convert
		 * from something else would time the conversion too.
		 */

		inputs[i].index = i;
		inputs[i].type = RKNN_TENSOR_UINT8;
		inputs[i].fmt = RKNN_TENSOR_NHWC;
		inputs[i].size = attr.n_elems;
		inputs[i].pass_through = 0;
		inputs[i].buf = malloc(inputs[i].size);

		if (inputs[i].buf == NULL) {
			fprintf(stderr, "cannot allocate %u input bytes\n",
				inputs[i].size);
			goto out;
		}

		memset(inputs[i].buf, 0x80, inputs[i].size);
	}

	out_bufs = calloc(io.n_output, sizeof(*out_bufs));
	out_sizes = calloc(io.n_output, sizeof(*out_sizes));

	if (out_bufs == NULL || out_sizes == NULL) {
		fprintf(stderr, "out of memory\n");
		goto out;
	}

	for (i = 0; i < io.n_output; i++) {
		rknn_tensor_attr attr;

		memset(&attr, 0, sizeof(attr));
		attr.index = i;
		if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &attr,
			       sizeof(attr)) < 0) {
			fprintf(stderr, "output attr %u failed\n", i);
			goto out;
		}

		print_attr("out", &attr);

		/* And the format the hardware actually produces, which is not
		 * necessarily the one the model declares.
		 *
		 * This is here because rknn_outputs_get() costs 3.3ms for 2.1MB -
		 * about 640 MB/s - while the camera path reads 221KB out of an
		 * equally uncached V4L2 buffer at 3.4 GB/s. A five-fold gap on
		 * what should be the same kind of copy says it is not a copy.
		 *
		 * Two guesses were already wrong about this number: CPU frequency
		 * (disproved by only the first case improving) and allocator
		 * churn (disproved by is_prealloc changing nothing). So this
		 * prints the native attributes rather than reasoning about them:
		 * if they differ from the declared ones, get() is doing a layout
		 * conversion and the cost is arithmetic, not memory bandwidth.
		 *
		 * The header hints at the answer - RKNN_QUERY_NATIVE_OUTPUT_ATTR
		 * has the alias RKNN_QUERY_NATIVE_NC1HWC2_OUTPUT_ATTR - but a
		 * name in a header is not a measurement.
		 */

		{
			rknn_tensor_attr nat;

			memset(&nat, 0, sizeof(nat));
			nat.index = i;
			if (rknn_query(ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR, &nat,
				       sizeof(nat)) == 0)
				print_attr("nat", &nat);
		}

		/* size_with_stride rather than size: the runtime writes padded
		 * rows for these tensors (1632000 declared, 1638400 with
		 * stride), and a buffer sized to the smaller figure would be
		 * written past the end.
		 */

		out_sizes[i] = attr.size_with_stride > attr.size ?
			       attr.size_with_stride : attr.size;
		out_bufs[i] = malloc(out_sizes[i]);

		if (out_bufs[i] == NULL) {
			fprintf(stderr, "cannot allocate %u output bytes\n",
				out_sizes[i]);
			goto out;
		}

		/* Faulted in now, not inside a timed section. */

		memset(out_bufs[i], 0, out_sizes[i]);
	}

	printf("\ninference, %d iterations each (first discarded):\n", loops);

	for (i = 0; i < sizeof(g_cores) / sizeof(g_cores[0]); i++) {
		if (run_core(ctx, &g_cores[i], inputs, io.n_input, io.n_output,
			     out_bufs, out_sizes, loops) < 0)
			goto out;
	}

	printf("\nNPU is alive. The input geometry above is what the RGA has to"
	       " produce\nand the per-core time is what the detector's frame"
	       " rate follows from.\n");

	ret = EXIT_SUCCESS;

out:
	if (inputs != NULL) {
		for (i = 0; i < io.n_input; i++)
			free(inputs[i].buf);

		free(inputs);
	}

	if (out_bufs != NULL) {
		for (i = 0; i < io.n_output; i++)
			free(out_bufs[i]);

		free(out_bufs);
	}

	free(out_sizes);

	rknn_destroy(ctx);
	free(model);
	return ret;
}
