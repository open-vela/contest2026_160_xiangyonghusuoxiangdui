/* Host-side test for the touch decoding in amp_fb_show.c.
 *
 * Includes the real source so the code under test is the shipped code, feeds
 * synthetic evdev reports through a pipe, and reads back the messages that
 * would have gone to the remote.
 */

#define main amp_fb_show_main
#include "../amp_fb_show.c"
#undef main

#include <assert.h>

static int g_in[2];
static int g_out[2];

static void feed(int type, int code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	assert(write(g_in[1], &ev, sizeof(ev)) == sizeof(ev));
}

static int drain_msgs(struct amp_touch_msg *out, int max)
{
	int n = 0;

	while (n < max) {
		ssize_t r = read(g_out[0], &out[n], sizeof(out[n]));

		if (r != (ssize_t)sizeof(out[n]))
			break;
		n++;
	}

	return n;
}

static const char *state_name(uint8_t s)
{
	return s == AMP_TOUCH_DOWN ? "down" :
	       s == AMP_TOUCH_UP ? "up" : "move";
}

int main(void)
{
	struct touch_src ts;
	struct amp_touch_msg msg[32];
	int n;
	int i;

	assert(pipe(g_in) == 0);
	assert(pipe(g_out) == 0);
	fcntl(g_in[0], F_SETFL, O_NONBLOCK);
	fcntl(g_out[0], F_SETFL, O_NONBLOCK);

	memset(&ts, 0, sizeof(ts));
	ts.fd = g_in[0];
	ts.mt = true;

	/* Controller reports panel coordinates; remote framebuffer is half. */

	ts.min_x = 0;
	ts.max_x = 1079;
	ts.min_y = 0;
	ts.max_y = 1919;

	printf("== single tap in the middle ==\n");
	feed(EV_ABS, ABS_MT_SLOT, 0);
	feed(EV_ABS, ABS_MT_TRACKING_ID, 100);
	feed(EV_ABS, ABS_MT_POSITION_X, 540);
	feed(EV_ABS, ABS_MT_POSITION_Y, 960);
	feed(EV_SYN, SYN_REPORT, 0);
	feed(EV_ABS, ABS_MT_TRACKING_ID, -1);
	feed(EV_SYN, SYN_REPORT, 0);

	touch_drain(&ts, g_out[1], 540, 960, true);
	n = drain_msgs(msg, 32);
	for (i = 0; i < n; i++)
		printf("  msg %d: id %u %s (%u,%u)\n", i, msg[i].id,
		       state_name(msg[i].state), msg[i].x, msg[i].y);

	assert(n == 2);
	assert(msg[0].state == AMP_TOUCH_DOWN);
	assert(msg[0].x == 270 && msg[0].y == 480);  /* halved */
	assert(msg[1].state == AMP_TOUCH_UP);
	assert(msg[1].x == 270 && msg[1].y == 480);  /* position on release */

	printf("== protocol B: tracking id repeated every report ==\n");
	memset(&ts.slot, 0, sizeof(ts.slot));
	ts.cur_slot = 0;

	/* Some drivers restate the whole contact in every report instead of only
	 * the deltas. Down must still happen once.
	 */

	for (i = 0; i < 5; i++) {
		feed(EV_ABS, ABS_MT_SLOT, 0);
		feed(EV_ABS, ABS_MT_TRACKING_ID, 100);
		feed(EV_ABS, ABS_MT_POSITION_X, 540);
		feed(EV_ABS, ABS_MT_POSITION_Y, 960);
		feed(EV_SYN, SYN_REPORT, 0);
	}

	feed(EV_ABS, ABS_MT_TRACKING_ID, -1);
	feed(EV_SYN, SYN_REPORT, 0);

	touch_drain(&ts, g_out[1], 540, 960, true);
	n = drain_msgs(msg, 32);
	for (i = 0; i < n; i++)
		printf("  msg %d: id %u %s (%u,%u)\n", i, msg[i].id,
		       state_name(msg[i].state), msg[i].x, msg[i].y);

	/* One down, one up. No moves: the finger never actually moved. */

	assert(n == 2);
	assert(msg[0].state == AMP_TOUCH_DOWN);
	assert(msg[1].state == AMP_TOUCH_UP);

	printf("== two fingers, slot number not repeated ==\n");
	memset(&ts.slot, 0, sizeof(ts.slot));
	ts.cur_slot = 0;

	feed(EV_ABS, ABS_MT_SLOT, 0);
	feed(EV_ABS, ABS_MT_TRACKING_ID, 200);
	feed(EV_ABS, ABS_MT_POSITION_X, 0);
	feed(EV_ABS, ABS_MT_POSITION_Y, 0);
	feed(EV_ABS, ABS_MT_SLOT, 1);
	feed(EV_ABS, ABS_MT_TRACKING_ID, 201);
	feed(EV_ABS, ABS_MT_POSITION_X, 1079);
	feed(EV_ABS, ABS_MT_POSITION_Y, 1919);
	feed(EV_SYN, SYN_REPORT, 0);

	/* Second report: only slot 1 moves, and the kernel does not repeat
	 * ABS_MT_SLOT because it has not changed. This is the case that breaks
	 * if the current slot is reset at SYN_REPORT.
	 */

	feed(EV_ABS, ABS_MT_POSITION_X, 1000);
	feed(EV_SYN, SYN_REPORT, 0);

	touch_drain(&ts, g_out[1], 540, 960, true);
	n = drain_msgs(msg, 32);
	for (i = 0; i < n; i++)
		printf("  msg %d: id %u %s (%u,%u)\n", i, msg[i].id,
		       state_name(msg[i].state), msg[i].x, msg[i].y);

	assert(n == 3);
	assert(msg[0].id == 0 && msg[0].state == AMP_TOUCH_DOWN);
	assert(msg[0].x == 0 && msg[0].y == 0);
	assert(msg[1].id == 1 && msg[1].state == AMP_TOUCH_DOWN);
	assert(msg[1].x == 539 && msg[1].y == 959);   /* clamped to fb edge */
	assert(msg[2].id == 1 && msg[2].state == AMP_TOUCH_MOVE);
	assert(msg[2].x == 500);

	printf("== protocol A as the board's gt1x driver reports it ==\n");
	memset(&ts.slot, 0, sizeof(ts.slot));
	memset(&ts.group, 0, sizeof(ts.group));
	ts.cur_slot = 0;
	ts.proto_a = false;
	ts.mt = true;

	/* The range the board actually reports, which is the controller's own
	 * resolution and not the panel's - EVIOCGABS gave x[0,720] y[0,1280]
	 * against a 1080x1920 screen.
	 */

	ts.max_x = 720;
	ts.max_y = 1280;

	/* gt1x with gtp_ics_slot_report absent: BTN_TOUCH, then pressure, major,
	 * tracking id and position per contact, each ended by SYN_MT_REPORT, and
	 * the whole state restated in every report. Release is BTN_TOUCH=0 with
	 * an empty group - no coordinates, no tracking id.
	 */

	for (i = 0; i < 4; i++) {
		feed(EV_KEY, BTN_TOUCH, 1);
		feed(EV_ABS, ABS_MT_PRESSURE, 40);
		feed(EV_ABS, ABS_MT_TOUCH_MAJOR, 40);
		feed(EV_ABS, ABS_MT_TRACKING_ID, 0);
		feed(EV_ABS, ABS_MT_POSITION_X, 360);
		feed(EV_ABS, ABS_MT_POSITION_Y, 640);
		feed(EV_SYN, SYN_MT_REPORT, 0);
		feed(EV_SYN, SYN_REPORT, 0);
	}

	/* Now it moves. */

	feed(EV_KEY, BTN_TOUCH, 1);
	feed(EV_ABS, ABS_MT_TRACKING_ID, 0);
	feed(EV_ABS, ABS_MT_POSITION_X, 380);
	feed(EV_ABS, ABS_MT_POSITION_Y, 640);
	feed(EV_SYN, SYN_MT_REPORT, 0);
	feed(EV_SYN, SYN_REPORT, 0);

	/* Release. */

	feed(EV_KEY, BTN_TOUCH, 0);
	feed(EV_SYN, SYN_MT_REPORT, 0);
	feed(EV_SYN, SYN_REPORT, 0);

	touch_drain(&ts, g_out[1], 540, 960, true);
	n = drain_msgs(msg, 32);
	for (i = 0; i < n; i++)
		printf("  msg %d: id %u %s (%u,%u)\n", i, msg[i].id,
		       state_name(msg[i].state), msg[i].x, msg[i].y);

	assert(ts.proto_a);
	assert(n == 3);
	assert(msg[0].state == AMP_TOUCH_DOWN);
	assert(msg[0].x == 269 && msg[0].y == 479);  /* 360/721*540 */
	assert(msg[1].state == AMP_TOUCH_MOVE);
	assert(msg[1].x == 284);
	assert(msg[2].state == AMP_TOUCH_UP);
	assert(msg[2].x == 284 && msg[2].y == 479);  /* last known position */

	printf("== protocol A, two contacts, one lifted ==\n");
	memset(&ts.slot, 0, sizeof(ts.slot));
	memset(&ts.group, 0, sizeof(ts.group));

	feed(EV_KEY, BTN_TOUCH, 1);
	feed(EV_ABS, ABS_MT_TRACKING_ID, 0);
	feed(EV_ABS, ABS_MT_POSITION_X, 100);
	feed(EV_ABS, ABS_MT_POSITION_Y, 100);
	feed(EV_SYN, SYN_MT_REPORT, 0);
	feed(EV_ABS, ABS_MT_TRACKING_ID, 1);
	feed(EV_ABS, ABS_MT_POSITION_X, 600);
	feed(EV_ABS, ABS_MT_POSITION_Y, 1200);
	feed(EV_SYN, SYN_MT_REPORT, 0);
	feed(EV_SYN, SYN_REPORT, 0);

	/* Second contact lifts: it simply stops being listed. */

	feed(EV_KEY, BTN_TOUCH, 1);
	feed(EV_ABS, ABS_MT_TRACKING_ID, 0);
	feed(EV_ABS, ABS_MT_POSITION_X, 100);
	feed(EV_ABS, ABS_MT_POSITION_Y, 100);
	feed(EV_SYN, SYN_MT_REPORT, 0);
	feed(EV_SYN, SYN_REPORT, 0);

	touch_drain(&ts, g_out[1], 540, 960, true);
	n = drain_msgs(msg, 32);
	for (i = 0; i < n; i++)
		printf("  msg %d: id %u %s (%u,%u)\n", i, msg[i].id,
		       state_name(msg[i].state), msg[i].x, msg[i].y);

	assert(n == 3);
	assert(msg[0].id == 0 && msg[0].state == AMP_TOUCH_DOWN);
	assert(msg[1].id == 1 && msg[1].state == AMP_TOUCH_DOWN);
	assert(msg[2].id == 1 && msg[2].state == AMP_TOUCH_UP);

	printf("== single-touch device: BTN_TOUCH + ABS_X/Y ==\n");
	memset(&ts.slot, 0, sizeof(ts.slot));
	memset(&ts.group, 0, sizeof(ts.group));
	ts.cur_slot = 0;
	ts.mt = false;
	ts.proto_a = false;
	ts.max_x = 1079;
	ts.max_y = 1919;

	feed(EV_KEY, BTN_TOUCH, 1);
	feed(EV_ABS, ABS_X, 270);
	feed(EV_ABS, ABS_Y, 480);
	feed(EV_SYN, SYN_REPORT, 0);
	feed(EV_ABS, ABS_X, 280);
	feed(EV_SYN, SYN_REPORT, 0);
	feed(EV_KEY, BTN_TOUCH, 0);
	feed(EV_SYN, SYN_REPORT, 0);

	touch_drain(&ts, g_out[1], 540, 960, true);
	n = drain_msgs(msg, 32);
	for (i = 0; i < n; i++)
		printf("  msg %d: id %u %s (%u,%u)\n", i, msg[i].id,
		       state_name(msg[i].state), msg[i].x, msg[i].y);

	assert(n == 3);
	assert(msg[0].state == AMP_TOUCH_DOWN && msg[0].x == 135);
	assert(msg[1].state == AMP_TOUCH_MOVE && msg[1].x == 140);
	assert(msg[2].state == AMP_TOUCH_UP && msg[2].x == 140);

	printf("\nall touch decoding checks passed\n");
	return 0;
}
