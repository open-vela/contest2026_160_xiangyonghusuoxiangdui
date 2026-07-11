# RK3588 EVB7 V11 - AMP minimal validation (official SDK path)

Bring up cpu_l3 (MPIDR 0x300) running a tiny bare-metal firmware that prints to
UART5, while Linux runs on the remaining 7 cores.

## Files
- amp1.c / amp.ld / build.sh -> builds amp1.bin (load/entry 0x00800000)  [BUILT OK: 200B]
- amp.its                    -> FIT descriptor for amp.img (cpu=0x300)
- parameter-amp.txt          -> partition table with an added 16MB `amp` partition

Kernel side (already applied in /media/1t/openvela/kernel):
- arch/arm64/boot/dts/rockchip/rk3588-evb7-v11-linux-amp.dts  (new)
- arch/arm64/boot/dts/rockchip/Makefile                       (registered dtb)

## 0. Prereq
sudo apt install gcc-aarch64-linux-gnu

## 1. Build demo firmware
bash /media/1t/openvela/rk3588-amp-demo/build.sh      # -> amp1.bin

## 2. Kernel: enable AMP + build AMP dtb into boot.img
cd /media/1t/openvela/kernel
make ARCH=arm64 rockchip_linux_defconfig
./scripts/kconfig/merge_config.sh -m .config arch/arm64/configs/rockchip_amp.config
make ARCH=arm64 olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) rk3588-evb7-v11-linux-amp.img
# -> boot.img (uses the -amp devicetree)

## 3. u-boot: build with AMP (trust required)
# NOTE: ./make.sh re-runs `make rk3588_defconfig` each time, wiping any merged
# .config. So bake AMP into the defconfig instead of merging into .config:
cd /media/1t/openvela/u-boot
grep -q '^CONFIG_AMP=y' configs/rk3588_defconfig || echo 'CONFIG_AMP=y' >> configs/rk3588_defconfig
grep -q '^CONFIG_ROCKCHIP_AMP=y' configs/rk3588_defconfig || echo 'CONFIG_ROCKCHIP_AMP=y' >> configs/rk3588_defconfig
# Toolchain: linaro 6.3.1 2017.05 at ../prebuilts/gcc/.../ (auto-found by make.sh)
./make.sh rk3588                 # -> uboot.img (CONFIG_AMP) / trust / MiniLoaderAll.bin

## 4. Package amp.img
/media/1t/openvela/u-boot/tools/mkimage -f /media/1t/openvela/rk3588-amp-demo/amp.its \
    -E -p 0xe00 /media/1t/openvela/rk3588-amp-demo/amp.img

## 5. Flash (MASKROM). Official firmware has NO amp partition -> write new GPT.
#    (rootfs/boot/uboot offsets unchanged; only amp added + userdata shifted.)
cd /media/1t/openvela/rkdeveloptool
UB=/media/1t/openvela/u-boot ; KZ=/media/1t/openvela/kernel ; AMP=/media/1t/openvela/rk3588-amp-demo
sudo ./rkdeveloptool db  "$UB"/MiniLoaderAll.bin        # RAM downloader (DDR init)
sudo ./rkdeveloptool gpt "$AMP"/parameter-amp.txt       # write new partition table (adds `amp`)
sudo ./rkdeveloptool wlx uboot "$UB"/uboot.img          # AMP u-boot (by partition name)
sudo ./rkdeveloptool wlx boot  "$KZ"/boot.img           # -amp kernel + dtb
sudo ./rkdeveloptool wlx amp   "$AMP"/amp.img           # AMP firmware image
sudo ./rkdeveloptool rd
# If your rkdeveloptool lacks `wlx`, use `wl <sector> <file>`:
#   uboot=0x00004000  boot=0x00008000  amp=0x01cb8000

## 6. Verify (serial 1500000, ttyFIQ0)
# - u-boot: "Brought up cpu[300] ..." (from rockchip_amp.c)
# - UART5:  "hello from cpu3 (RK3588 AMP core alive)"
# - Linux boots on 7 cores; `nproc` == 7; dmesg has rockchip-amp/rpmsg.

## Caveat
If cpu3 runs but UART5 prints nothing, UART5 clk-gate (CRU) + pinmux (IOC) must
be initialized inside amp1.c (needs exact RK3588 TRM register offsets).

## 7. Verify WITHOUT UART5 (shared-memory heartbeat)
EVB7 exposes no UART5 pins, so amp1.c also writes a heartbeat into its own
no-map reserved region; read it from Linux via /dev/mem.
- amp1.c writes: 0x30000800 = magic 0x414D5033 ("AMP3"), 0x30000804 = counter++ (forever).
- Safe because 0x30000000 is `no-map` reserved -> Linux never uses that RAM (AMP core
  owns it); and being non-System-RAM, /dev/mem can still read it (STRICT_DEVMEM allows).
- Rebuild + reflash only the amp partition after editing amp1.c:
    bash build.sh
    <u-boot>/tools/mkimage -f amp.its -E -p 0xe00 amp.img
    sudo rkdeveloptool db <u-boot>/rk3588_spl_loader_v1.21.114.bin
    sudo rkdeveloptool wlx amp amp.img
    sudo rkdeveloptool rd
- Read on target (root):
    python3 -c 'import mmap,os,struct,time; f=os.open("/dev/mem",os.O_RDONLY|os.O_SYNC); \
    m=mmap.mmap(f,0x1000,mmap.MAP_SHARED,mmap.PROT_READ,offset=0x30000000); \
    rd=lambda o:struct.unpack("<I",m[o:o+4])[0]; print("magic",hex(rd(0x800))); \
    a=rd(0x804); time.sleep(1); print("count",a,"->",rd(0x804))'
  or busybox devmem 0x30000800 32 / 0x30000804 32.
- PASS: magic==0x414d5033 and counter increments -> cpu3 is running the firmware.
