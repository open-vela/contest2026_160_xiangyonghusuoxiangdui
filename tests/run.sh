#!/bin/bash
#
# Host-side tests for the detection path in amp_fb_show.c.
#
# These run on the build machine, not the board. Everything here is pure
# arithmetic - decode, NMS, coordinate transform - so it needs no NPU, no RGA and
# no camera, which is the whole reason it is worth having: these are the parts
# whose failure mode is boxes in the wrong place, and no amount of looking at the
# board's console can tell a wrong box from a right one.
#
# The functions under test are extracted from amp_fb_show.c with awk rather than
# copied into the test. A copy tests the copy: it goes stale the moment the real
# code changes, and it goes stale silently.
#
# The reference side is extracted the same way out of the vendor's
# postprocess.cc, so a disagreement can only be our code or theirs, never a
# paraphrase of theirs. This is what caught the IoU convention: the reference
# computes overlap with a +1 on every edge, and without it three random tensors
# in twenty produced a different set of survivors. If you are tempted to remove
# that +1 as a typo, run this first.
#
# Usage: tests/run.sh [seeds]
#
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
src=$root/amp_fb_show.c
seeds=${1:-200}

# The vendor demo, for the reference implementation. Overridable because it lives
# outside this repository and its checkout location is nobody's business but the
# person running this.

: "${RKNN_DEMO:=/media/1t/openvela/rknn-toolkit2/rknpu2/examples/rknn_yolov5_demo}"
vend=$RKNN_DEMO/src/postprocess.cc

build=$here/build
mkdir -p "$build"

fail=0

say() { printf '%s\n' "$*"; }

if [ ! -f "$src" ]; then
	say "cannot find $src"
	exit 1
fi

if [ ! -f "$vend" ]; then
	say "cannot find $vend"
	say "set RKNN_DEMO to the rknn_yolov5_demo directory"
	exit 1
fi

# Our side, verbatim.

awk '/^static const int g_yolo_anchor/,/^};$/'      "$src" >  "$build/y_tab.inc"
awk '/^static const int g_yolo_stride/,/;$/'        "$src" >> "$build/y_tab.inc"
awk '/^static uint32_t yolo_decode/,/^}$/'          "$src" >  "$build/y_dec.inc"
awk '/^static int yolo_cmp/,/^}$/'                  "$src" >  "$build/y_nms.inc"
awk '/^static uint32_t yolo_nms/,/^}$/'             "$src" >> "$build/y_nms.inc"
awk '/^static uint32_t det_emit/,/^}$/'             "$src" >  "$build/y_emit.inc"

# Theirs, verbatim. __clip has to come first: qnt_f32_to_affine calls it, and C++
# will not forward-declare it for us.

{
	awk '/^inline static int32_t __clip/,/^}$/'                 "$vend"
	awk '/^static int8_t qnt_f32_to_affine/,/^}$/'              "$vend"
	grep -E '^static float deqnt_affine_to_f32'                 "$vend"
	awk '/^static float CalculateOverlap/,/^}$/'                "$vend"
	awk '/^static int nms\(/,/^}$/'                             "$vend"
	awk '/^static int quick_sort_indice_inverse/,/^}$/'         "$vend"
	awk '/^static int process\(/,/^}$/'                         "$vend"
} > "$build/vend.inc"

# Every extraction has to have produced something. An awk range that stops
# matching after a refactor yields an empty file, and an empty file compiles
# into a test that passes by testing nothing - which is worse than a test that
# fails, because nobody looks at it again.

for f in y_tab y_dec y_nms y_emit vend; do
	if [ ! -s "$build/$f.inc" ]; then
		say "FAIL extraction produced an empty $f.inc"
		fail=1
	fi
done

if [ "$(grep -cE '^(inline )?static' "$build/vend.inc")" != 7 ]; then
	say "FAIL expected 7 reference functions, got" \
	    "$(grep -cE '^(inline )?static' "$build/vend.inc")"
	fail=1
fi

[ "$fail" = 0 ] || exit 1

say "== decode and NMS against the vendor reference =="

gcc -O1 -Wall -I"$build" -c "$here/yolo_decode_mine.c" \
	-o "$build/mine.o" || exit 1
g++ -O1 -w -I"$build" -c "$here/yolo_decode_vendor.cpp" \
	-o "$build/vend.o" || exit 1
gcc -O1 -Wall -c "$here/yolo_decode_test.c" -o "$build/main.o" || exit 1
g++ -o "$build/yolo_decode_test" "$build/mine.o" "$build/vend.o" \
	"$build/main.o" -lm || exit 1

"$build/yolo_decode_test" 1 || fail=1

# Then quietly over many seeds. One seed proves the two agree on one arrangement
# of peaks; the IoU difference showed up in three seeds out of twenty, so a
# single seed had an 85 percent chance of missing it.

bad=0
nosup=0

for s in $(seq 1 "$seeds"); do
	out=$("$build/yolo_decode_test" "$s")

	case "$out" in
	*"decode matches"*) ;;
	*)
		bad=$((bad + 1))
		[ "$bad" -gt 3 ] || { say "--- seed $s ---"; say "$out"; }
		;;
	esac

	case "$out" in
	*"ok"*"NMS actually suppressed"*) ;;
	*) nosup=$((nosup + 1)) ;;
	esac
done

say "$seeds seeds: $bad disagreed, $nosup exercised no suppression"

[ "$bad" = 0 ] || fail=1

# A seed where nothing overlapped would make the survivor comparison vacuous -
# both sides keep everything, and agreeing on that proves nothing about NMS.

[ "$nosup" = 0 ] || fail=1

say
say "== det_emit and the inverse coordinate transform =="

gcc -O1 -Wall -Wextra -I"$build" -o "$build/det_emit_test" \
	"$here/det_emit_test.c" -lm || exit 1

"$build/det_emit_test" || fail=1

say
if [ "$fail" = 0 ]; then
	say "all detection tests pass"
else
	say "DETECTION TESTS FAILED"
fi

exit $fail
