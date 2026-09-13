#!/usr/bin/env python3
#
# repack_bootimg - put a new device tree into an existing boot.img.
#
# SPDX-License-Identifier: Apache-2.0
#
# This exists because changing the Linux device tree on this board means
# reflashing boot.img, and how to produce one was never written down. The only
# recorded route was a full Armbian image build, which rebuilds everything to
# change two hex digits in a reserved-memory node.
#
# The board's boot.img is an Android boot image whose "second" area holds a
# Rockchip resource image, and the device tree is a file inside that. So the
# minimal change is: unpack the resource image, swap the .dtb, repack, and
# reassemble with the original header.
#
# The header page is copied whole and only its size fields are patched. It carries
# the load addresses, the kernel command line, the board name and an id hash, none
# of which this has any business regenerating - losing the command line would
# produce an image that flashes cleanly and then cannot find its root filesystem.
#
# Usage:
#   repack_bootimg.py --in boot.img --dtb new.dtb --out boot.img.new
#   repack_bootimg.py --in boot.img --list
#
# The rkbin resource_tool does the resource image half.

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

RESOURCE_TOOL = "/media/1t/openvela/rkbin/tools/resource_tool"

HDR = "<8s8I16s512s32s"          # magic, sizes/addrs, name, cmdline, id


def read_header(data):
    if data[:8] != b"ANDROID!":
        raise SystemExit("not an Android boot image")

    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
     second_addr, tags_addr, page_size) = struct.unpack_from("<8I", data, 8)

    return {
        "kernel_size": kernel_size,
        "kernel_addr": kernel_addr,
        "ramdisk_size": ramdisk_size,
        "ramdisk_addr": ramdisk_addr,
        "second_size": second_size,
        "second_addr": second_addr,
        "tags_addr": tags_addr,
        "page_size": page_size,
    }


def offsets(h):
    ps = h["page_size"]

    def pages(n):
        return (n + ps - 1) // ps

    off_kernel = ps
    off_ramdisk = off_kernel + pages(h["kernel_size"]) * ps
    off_second = off_ramdisk + pages(h["ramdisk_size"]) * ps
    end = off_second + pages(h["second_size"]) * ps
    return off_kernel, off_ramdisk, off_second, end


def pad_to_page(data, page_size):
    rem = len(data) % page_size
    return data + b"\0" * (page_size - rem) if rem else data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--dtb", help="new device tree, or - to keep the existing"
                                  " one (a round-trip test of this tool)")
    ap.add_argument("--out")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()

    data = open(args.src, "rb").read()
    h = read_header(data)
    ok, orr, osec, end = offsets(h)

    print("%s: %d bytes, page %d" % (args.src, len(data), h["page_size"]))
    print("  kernel   %10d at 0x%08x (file offset %d)"
          % (h["kernel_size"], h["kernel_addr"], ok))
    print("  ramdisk  %10d at 0x%08x" % (h["ramdisk_size"], h["ramdisk_addr"]))
    print("  second   %10d at 0x%08x (file offset %d)"
          % (h["second_size"], h["second_addr"], osec))

    if end != len(data):
        # Not fatal - some images carry a signature past the last page - but it
        # has to be said, because anything after the end is about to be dropped.
        print("  NOTE %d bytes past the computed end will not be carried over"
              % (len(data) - end))

    resource = data[osec:osec + h["second_size"]]
    if resource[:4] != b"RSCE":
        raise SystemExit("second area is not a Rockchip resource image")

    tmp = tempfile.mkdtemp(prefix="repack-bootimg-")
    try:
        rpath = os.path.join(tmp, "resource.img")
        open(rpath, "wb").write(resource)

        # resource_tool unpacks into ./out/ under the current directory, so it
        # gets its own. --verbose names each entry as it goes, which is the only
        # place the order and the exact stored names are visible - and both
        # matter: the bootloader looks the device tree up by the name
        # "rk-kernel.dtb", so packing it as "out/rk-kernel.dtb" would produce an
        # image that flashes and then cannot find its device tree.
        r = subprocess.run([RESOURCE_TOOL, "--unpack",
                            "--image=resource.img", "--verbose"],
                           cwd=tmp, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            raise SystemExit("resource_tool --unpack failed")

        names = []
        for line in (r.stdout + r.stderr).splitlines():
            mark = "try to dump entry:"
            if mark in line:
                names.append(line.split(mark, 1)[1].strip())

        if not names:
            raise SystemExit("could not read the entry list from"
                             " resource_tool --verbose")

        unpacked = os.path.join(tmp, "out")
        entries = [(n, os.path.getsize(os.path.join(unpacked, n)))
                   for n in names]

        print("  resource image contains, in order:")
        for name, size in entries:
            print("    %-40s %9d" % (name, size))

        if args.list:
            return 0

        if not args.dtb or not args.out:
            raise SystemExit("--dtb and --out are required unless --list")

        dtbs = [n for n, _ in entries if n.endswith(".dtb")]
        if len(dtbs) != 1:
            raise SystemExit("expected exactly one .dtb, found %d: %s"
                             % (len(dtbs), dtbs))

        target = dtbs[0]
        old_size = os.path.getsize(os.path.join(unpacked, target))

        if args.dtb != "-":
            shutil.copyfile(args.dtb, os.path.join(unpacked, target))

        new_size = os.path.getsize(os.path.join(unpacked, target))
        print("  %s %s: %d -> %d bytes"
              % ("keeping" if args.dtb == "-" else "replacing", target,
                 old_size, new_size))

        # Same names, same order, and run from the directory they were unpacked
        # into. --root looks like it should do this and does not: the tool
        # resolves the file arguments against the current directory regardless,
        # so packing from anywhere else fails to open them. The names must stay
        # bare because they are what the bootloader looks the device tree up by.
        packed = os.path.join(tmp, "new-resource.img")
        cmd = [RESOURCE_TOOL, "--pack", "--image=" + packed]
        cmd += [n for n, _ in entries]
        r = subprocess.run(cmd, cwd=unpacked, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout)
            print(r.stderr)
            raise SystemExit("resource_tool --pack failed")

        new_resource = bytearray(
			open(os.path.join(tmp, "new-resource.img"), "rb").read())
        print("  resource image %d -> %d bytes"
              % (len(resource), len(new_resource)))

        # Bring the index table back to exactly what it was, except for the one
        # entry that changed.
        #
        # resource_tool leaves two bytes of uninitialised stack in each index
        # entry - the same 0x7ffd in all of them, which is the top half of a
        # Linux stack address - and this repack writes zeroes there instead. That
        # is almost certainly harmless. "Almost certainly" is not what one wants
        # to know about a boot image, so the bytes go back, and then the only
        # difference left between the two images is the device tree itself and the
        # length recorded for it. Anything else differing means the layout moved
        # and this tool should not be guessing about it.
        table = ps_res = 512
        table_end = table + 3 * ps_res

        if len(new_resource) == len(resource):
            restored = 0
            per_entry = []

            for e in range(len(entries)):
                base = table + e * ps_res
                changed = []

                for i in range(base, min(base + ps_res, len(resource))):
                    if new_resource[i] == resource[i]:
                        continue

                    if new_resource[i] == 0 and resource[i] != 0:
                        new_resource[i] = resource[i]
                        restored += 1
                    else:
                        changed.append(i - base)

                per_entry.append(changed)

            print("  index table: %d uninitialised byte(s) restored" % restored)

            for e, (name, _sz) in enumerate(entries):
                print("    entry %d %-24s %d byte(s) changed"
                      % (e, name, len(per_entry[e])))

            # The entry whose file was replaced is expected to change: its
            # digest and its recorded length. Every other entry must be
            # untouched, and that - rather than a count - is the property worth
            # checking, because a difference leaking into another entry means
            # the payload offsets moved and this tool would be writing an image
            # whose layout it has not reasoned about.
            for e, (name, _sz) in enumerate(entries):
                if name == target:
                    continue

                if per_entry[e]:
                    raise SystemExit(
                        "entry %d (%s) changed at %s - the layout moved,"
                        " refusing to write" % (e, name, per_entry[e]))
        else:
            print("  resource image changed size, index table not compared")

        new_resource = bytes(new_resource)

        ps = h["page_size"]
        kernel = data[ok:ok + h["kernel_size"]]
        ramdisk = data[orr:orr + h["ramdisk_size"]]

        # The header page verbatim, with only the second area's length changed.
        # Everything else in it - command line, board name, id - stays.
        hdr = bytearray(data[:ps])
        struct.pack_into("<I", hdr, 8 + 4 * 4, len(new_resource))

        out = bytes(hdr)
        out += pad_to_page(kernel, ps)
        out += pad_to_page(ramdisk, ps) if h["ramdisk_size"] else b""
        out += pad_to_page(new_resource, ps)

        open(args.out, "wb").write(out)
        print("  wrote %s, %d bytes" % (args.out, len(out)))

        # Read the result back through the same parser. A repacked image that
        # this cannot itself describe is not one to flash.
        h2 = read_header(out)
        _, _, osec2, end2 = offsets(h2)
        if end2 != len(out):
            raise SystemExit("readback: computed end %d, file %d"
                             % (end2, len(out)))
        if out[osec2:osec2 + 4] != b"RSCE":
            raise SystemExit("readback: second area is not RSCE")
        if out[ps:ps + 4] != data[ps:ps + 4]:
            raise SystemExit("readback: kernel head changed")

        print("  readback ok: kernel unchanged, second is RSCE, sizes"
              " consistent")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
