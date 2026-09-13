# 从零搭建：RK3588 EVB7 V11 三域 AMP 系统

> 目标读者：拿到一块空白（或只有官方固件的）RK3588 EVB7 V11，要复现出
> **Linux(7×A55) + NuttX(cpu_l3) + NuttX(PMU Cortex-M0)** 三域并行、带摄像头 +
> NPU 识别 + LVGL UI 的完整系统。
>
> 逐条历史与每个决策的来龙去脉见同目录 `rk3588-evb7-v11.md`（主 changelog，6600+ 行）。
> 本文只讲"怎么做"，不讲"为什么这么做" —— 但凡有静默失败风险的地方都会明确标出。
>
> ⚠️ **本项目不使用 Armbian 构建路径**（早期用过，已放弃，见 changelog
> `[ABANDONED] Armbian integration`）。板上跑的是 Rockchip 官方 BSP Debian rootfs +
> 我们自建的 kernel/u-boot/AMP 载荷。本仓库（armbain）只承担 changelog 与 skills。

---

## 0. 成品形态

| 域 | 核 | 系统 | 交互入口 |
|----|----|------|---------|
| 1 | A55 ×7 | Linux（官方 BSP Debian，内核 6.1.141） | 串口 `ttyFIQ0` / adb / ssh |
| 2 | A55 ×1（cpu_l3，MPIDR 0x300） | NuttX AArch64 + LVGL UI | Linux 侧 `/dev/ttyNSH0` |
| 3 | Cortex-M0（PMU） | NuttX ARMv6-M | Linux 侧 `/dev/ttyNSH1` |

三个域共用一根 UART2 物理调试线；两个 NuttX 的 nsh 通过 rpmsg 隧道到 Linux，不占物理串口。

---

## 1. 硬件与官方资料

**开发板**：RK3588 EVB7 V11（带 MIPI-DSI 屏 1080x1920、imx415 MIPI 摄像头、AP6398S WiFi/BT）

**串口**：UART2，**波特率 1500000** 8N1，Linux 控制台设备 `ttyFIQ0`（fiq-debugger 接管）。
接 USB-TTL 后一般是 `/dev/ttyUSB0`/`ttyUSB1`。

**启动介质**：eMMC。**SD 卡启动无输出**，直接刷 eMMC 才行；且 SD 卡槽（`fe2c0000.mmc/sdmmc`）
因为 UART5 引脚（uart5m0/gpio4-29）被 `rockchip-amp` 占用而不可用 —— eMMC 启动不受影响。

**官方固件包**（必须先有，作为基线）：
```
/media/1t/openvela/rk3588_EVB7/RK3588-EVB7-V11-DEBIAN_V1.0.0_20260620/RK3588-EVB7-V11-LINUX/rockdev/
├── MiniLoaderAll.bin      547 KB   官方 loader
├── uboot.img              4 MB
├── boot.img               40 MB
├── rootfs.img             6.5 GB   官方 Debian（含 GNOME/weston/libmali/bcmdhd 固件）
├── oem.img                12 MB
├── userdata.img           8 MB
├── misc.img / recovery.img
├── parameter.txt                   官方分区表（无 amp 分区）
└── update.img             6.6 GB   整包
```
同目录还有 `Rockchip_RK3588_EVB7_User_Guide_V1.0_20230620_CN.pdf`、datasheet、
`upgrade_tool_v2.55_for_linux`、`FactoryTool_v2.0`。

**TRM**（M0 移植必需，datasheet 里没有任何寄存器地址）：
`Rockchip RK3588 TRM V1.0-Part1/Part2-20220309.pdf`。没有 TRM 也能做到第 2 域，
第 3 域（M0）的 INTMUX/地址重映射必须靠它。

---

## 2. 主机环境准备

### 2.1 系统与基础工具

```bash
sudo apt install -y build-essential git bc bison flex libssl-dev libncurses-dev \
    device-tree-compiler python3 python3-pip u-boot-tools \
    gcc-aarch64-linux-gnu g++-aarch64-linux-gnu gcc-arm-none-eabi \
    android-tools-adb picocom
```

### 2.2 rkdeveloptool（刷机必需）

```bash
git clone https://github.com/rockchip-linux/rkdeveloptool
cd rkdeveloptool
sudo apt install -y libudev-dev libusb-1.0-0-dev autoconf automake libtool pkg-config
autoreconf -i && ./configure && make -j$(nproc)
sudo cp rkdeveloptool /usr/local/bin/
rkdeveloptool -v      # 确认在 PATH 里
```

USB 免 sudo（可选）：
```bash
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="2207", MODE="0666"' | \
    sudo tee /etc/udev/rules.d/99-rockchip.rules
sudo udevadm control --reload
```

### 2.3 四套工具链（对应关系不能混）

| 用途 | 工具链 | 位置 |
|---|---|---|
| Linux 内核 + Linux 用户态程序 | 系统 `aarch64-linux-gnu-gcc`（GCC 11.4） | apt 装 |
| **vendor u-boot** | **linaro 6.3.1-2017.05** | `/media/1t/openvela/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu` |
| NuttX cpu_l3（AArch64） | `aarch64-none-elf` GCC 13.4 | `/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin` |
| NuttX M0（ARMv6-M） | `arm-none-eabi` | `/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin` |

> ⚠️ **vendor u-boot 2017.09 必须用 linaro 6.3.1**，系统 GCC 11.4 编不过。这个 toolchain
> 随官方 SDK 的 `prebuilts` 提供。
>
> NuttX 的两套工具链随 openvela 的 `work/prebuilts` 提供（repo 同步下来就有）。

### 2.4 进入 MASKROM 的方法

按住板上 MASKROM 键（或 RECOVERY，见 User Guide）再上电/复位，然后：
```bash
rkdeveloptool ld        # 应看到 DevNo=1 ... Maskrom
```
看到 `Loader` 而不是 `Maskrom` 说明进的是 loader 模式，多数命令仍可用，但首次改分区表
建议在 MASKROM 下做。

---

## 3. 源码树布局

所有树都在工作区之外，用绝对路径访问。

```
/media/1t/openvela/
├── kernel/                  Rockchip BSP 内核 6.1.141      分支 develop-6.1
├── u-boot/                  vendor u-boot 2017.09          分支 next-dev
├── rkbin/                   DDR/BL31/BL32 blobs            —
├── prebuilts/               linaro 6.3.1（u-boot 用）      —
├── work/                    openvela（repo 管理）
│   ├── nuttx/               NuttX 内核                     分支 0712
│   ├── apps/                NuttX 应用                     分支 0801
│   └── prebuilts/           aarch64-none-elf / arm-none-eabi
├── rk3588-amp-demo/         AMP 载荷打包 + Linux 用户态 + 部署   分支 master
├── rknn-toolkit2/           RKNPU2 SDK（NPU 头文件与运行库）
├── rk3588_EVB7/             官方固件与文档
└── armbain/                 本仓库：changelog + skills
```

**必须自己准备的**（不在版本控制里）：
- `rkbin`：`git clone https://github.com/rockchip-linux/rkbin`（或用官方 SDK 里那份，
  官方 BSP rkbin 是 DDR v1.21 / BL31 v1.54，AMP 依赖 BL31 的 vendor SIP）
- `rknn-toolkit2`：`git clone https://github.com/airockchip/rknn-toolkit2`
- YOLOv5 模型 `yolov5s-640-640.rknn` 与 `coco_80_labels_list.txt`（来自 rknn_model_zoo）

---

## 3.1 改动仓库汇总（GitHub 链接待补充）

下表列出本项目**发生过代码改动**的全部仓库。`GitHub 链接`一列留空，请自行填入各仓库对应的
远端地址（fork/上游均可）。

| 仓库 | 本地路径 | 分支 | 改动内容概述 | GitHub 链接 |
|------|---------|------|-------------|------------|
| kernel（Rockchip BSP 6.1.141） | `/media/1t/openvela/kernel` | develop-6.1 | AMP dts（`rk3588-evb7-v11-linux-amp.dts`：cpu_l3 摘核、amp/rpmsg 保留区、reserved-plane、关 vop_mmu）；kernel fragment `rk3588_g610.config` / `rk3588_wifibt.config`；新驱动 `drivers/tty/rpmsg_nsh_tty.c`；`rpmsg_char`/`rpmsg_ctrl`/`rockchip_rpmsg_test` 配置 | https://github.com/C-Ackerman/kernel/tree/rk3588-evb7-v11 |
| u-boot（vendor 2017.09） | `/media/1t/openvela/u-boot` | next-dev | `configs/rk3588_defconfig` 追加 `CONFIG_AMP=y` + `CONFIG_ROCKCHIP_AMP=y` | https://github.com/C-Ackerman/u-boot/tree/rk3588-evb7-v11 |
| work/nuttx（openvela NuttX） | `/media/1t/openvela/work/nuttx` | 0712 | 新增 arm64 chip 层 `arch/arm64/src/rk3588/`（GICv3 rdist 修复、EL1 物理 timer、rptun、rsctable、共享内存非缓存）+ board `boards/arm64/rk3588/evb7-amp/`（fb/touch/shm/vop/cam）；新增 armv6-m chip 层 `arch/arm/src/rk3588-m0/` + board `boards/arm/rk3588-m0/evb7-m0/`；通用驱动 `drivers/serial/uart_rpmsg.c` re-announce 改动 | https://github.com/C-Ackerman/nuttx-openvela/tree/rk3588-evb7-v11 |
| work/apps（openvela apps） | `/media/1t/openvela/work/apps` | 0801 | 新增 `examples/ampcam`（相机帧读取+画框）、`examples/ampui`（LVGL 主页+相机页） | https://github.com/C-Ackerman/nuttx-apps-openvela/tree/rk3588-evb7-v11 |
| rk3588-amp-demo（AMP 载荷 + Linux 用户态） | `/media/1t/openvela/rk3588-amp-demo` | master | amp.img 打包（`amp-m0-nuttx.its` / `parameter-amp.txt`）；Linux 侧 `amp_fb_show.c`（采集/NPU/显示/触摸转发）；M0 裸机固件 `m0/`；诊断工具 `amp_shm_*.c` / `amp_rga_probe.c` / `amp_isp_range.c` / `amp_npu_probe.c`；部署 `deploy/`（systemd unit + install.sh）；`tests/` | https://github.com/open-vela/contest2026_160_xiangyonghusuoxiangdui |

> ⚠️ **rkbin**（`/media/1t/openvela/rkbin`）曾在一轮尝试中改过 `RKTRUST/RK3588TRUST.ini`
> 指向 patched OP-TEE，但因 vendor BL31 与主线 OP-TEE 不兼容**已完全还原**，本质无改动，
> 故未列入上表。其内容（DDR v1.21 / BL31 v1.54 blob）保持官方原样。

---

## 3.2 官方固件用了哪些（官方 vs 自建对照）

本项目是**官方 BSP 基线 + 自建组件替换**的混合。下表把散落在 §1 / §5 / §6.1 的信息汇总成一处。

**官方固件下载地址**：https://redmine.rock-chips.com/urllist （Rockchip Redmine 固件列表 Linux6.12 debain）
https://meta.box.lenovo.com/v/link/view/a6912849a31742ecb7474509135a3f6b

固件包本地目录（下称 `$ROCKDEV`）：
`/media/1t/openvela/rk3588_EVB7/RK3588-EVB7-V11-DEBIAN_V1.0.0_20260620/RK3588-EVB7-V11-LINUX/rockdev/`

### 保留使用的官方组件

| 分区 / 组件 | 官方文件 | 说明 |
|------|---------|------|
| **rootfs** | `$ROCKDEV/rootfs.img`（约 6.5GB） | 官方 Debian，含 GNOME / weston / **libmali-valhall-g610**（GPU）/ **bcmdhd 固件 + nvram**（WiFi）/ **BCM4359C0.hcd**（蓝牙）。整个用户态都来自它 |
| **oem** | `$ROCKDEV/oem.img` | 原样刷入 |
| **userdata** | `$ROCKDEV/userdata.img` | 重新分区后**必须重刷官方这份**，否则 systemd 掉进 emergency mode（见 §5 ⚠️） |
| **trust blobs** | rkbin：DDR v1.21 / **BL31 v1.54** / BL32(OP-TEE) v1.20 | AMP 拉核依赖 BL31 的 vendor SIP；打进自建 uboot.img，blob 本身保持官方（见 §6.1） |

### 被自建组件替换的（不再用官方）

| 分区 / 组件 | 官方文件 | 替换成 |
|------|---------|--------|
| loader / SPL | `MiniLoaderAll.bin` | 自编 vendor u-boot 产出的 `rk3588_spl_loader_v1.21.114.bin`（§6.1） |
| **uboot** | `uboot.img` | 自编 AMP 版（`CONFIG_AMP` + vendor BL31 + OP-TEE 打成 FIT，§6.1） |
| **boot** | `boot.img` | 自编（AMP dts + G610/wifibt fragment 内核 + resource，§6.2） |
| 分区表 | `parameter.txt` | `parameter-amp.txt`（新增 16MB `amp` 分区，§5） |
| **kernel** | boot.img 内的官方内核 | 官方 BSP **源码** 6.1.141 自行重编（加 AMP dts + 三个 config fragment，§6.2） |
| **amp**（新分区） | 官方无此分区 | 自建 AMP 载荷 `amp.img`（cpu_l3 NuttX + M0 NuttX，§7） |

> 一句话：**官方贡献 = rootfs（整个用户态）+ oem + userdata + rkbin 的 DDR/BL31/BL32 三个 blob**；
> u-boot、内核、分区表、AMP 载荷全部自建。
>
> ⚠️ **首次刷机会先刷一遍全套官方固件**（§4），那一步只是为了确认板子是好的、拿到"已知好"
> 的回退基线，**不是最终形态**。最终形态按 §5 的命令混合刷入。

---

## 4. 第一步：刷官方固件，确认板子是好的

**不要跳过这一步。** 后面所有改动都是在这个基线上做增量，而且很多失败（花屏、起不来）
需要能回退到"已知好"的状态来区分是硬件还是我们的改动。

```bash
cd /media/1t/openvela/rk3588_EVB7/RK3588-EVB7-V11-DEBIAN_V1.0.0_20260620/RK3588-EVB7-V11-LINUX/rockdev
rkdeveloptool db MiniLoaderAll.bin
rkdeveloptool ul MiniLoaderAll.bin          # 装 loader 到 eMMC
rkdeveloptool gpt parameter.txt
rkdeveloptool wlx uboot     uboot.img
rkdeveloptool wlx boot      boot.img
rkdeveloptool wlx rootfs    rootfs.img
rkdeveloptool wlx oem       oem.img
rkdeveloptool wlx userdata  userdata.img
rkdeveloptool rd
```

串口（1500000）应看到启动到 `root@debian:/#`。桌面应该能亮。

> ⚠️ **`ul` 不能省。** 只 `db` 是把 loader 下载到内存跑一次，不写 eMMC。如果 eMMC 里
> 残留着别的 SPL（比如早期 Armbian 那份），会出现复位循环。这个坑 changelog 里记着。

---

## 5. 第二步：改分区表，加 16MB `amp` 分区

AMP 载荷（两个 NuttX 固件打成一个 FIT）需要一个独立分区，官方分区表里没有。

分区表文件已在仓库里：`/media/1t/openvela/rk3588-amp-demo/parameter-amp.txt`
—— 相对官方 `parameter.txt` 只做了两件事：

```
0x00008000@0x01cb8000(amp)        新增 16MB amp 分区
-@0x01cc0000(userdata:grow)       userdata 起点从 0x01cb8000 后移到 0x01cc0000
```
`uboot / misc / boot / recovery / backup / rootfs / oem` 偏移全部不变。

改完分区表要重刷全部分区（GPT 变了）：

```bash
ROCKDEV=/media/1t/openvela/rk3588_EVB7/RK3588-EVB7-V11-DEBIAN_V1.0.0_20260620/RK3588-EVB7-V11-LINUX/rockdev
cd /media/1t/openvela/rk3588-amp-demo

rkdeveloptool db $ROCKDEV/MiniLoaderAll.bin
rkdeveloptool ul $ROCKDEV/MiniLoaderAll.bin
rkdeveloptool gpt parameter-amp.txt
rkdeveloptool wlx uboot    /media/1t/openvela/u-boot/uboot.img       # 我们编的 AMP 版，见 §6.1
rkdeveloptool wlx boot     /media/1t/openvela/kernel/boot.img        # 我们编的 AMP dts 版，见 §6.2
rkdeveloptool wlx amp      amp.img                                   # AMP 载荷，见 §7
rkdeveloptool wlx rootfs   $ROCKDEV/rootfs.img
rkdeveloptool wlx oem      $ROCKDEV/oem.img
rkdeveloptool wlx userdata $ROCKDEV/userdata.img
rkdeveloptool rd
```

> ⚠️ **`userdata` 必须重刷官方 `userdata.img`**。重新分区后那个分区的文件系统失效，
> 不刷会让 systemd 掉进 emergency mode（`local-fs.target` failed）。

分区名不要猜，先看：
```bash
rkdeveloptool ppt
```

最终分区布局（关键两个）：
```
NO  LBA        Name    大小
02  0x0008000  boot    64MB    boot.img 约 40.6MB
07  0x01CB8000 amp     16MB    amp.img  约 1.1MB
```

---

## 6. 编译各仓库

### 6.1 u-boot（vendor 2017.09，必须开 AMP）

AMP 拉核靠 vendor u-boot 的 `CONFIG_AMP` + `CONFIG_ROCKCHIP_AMP`。
**这两项已烤进 `configs/rk3588_defconfig`**（commit `6ee99c8`）——
因为 `./make.sh` 每次会重跑 defconfig，把 merged `.config` 冲掉，用 fragment 会丢。

```bash
cd /media/1t/openvela/u-boot
export PATH=/media/1t/openvela/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin:$PATH
./make.sh rk3588
```

产物：
```
uboot.img                          FIT: BL31 + OP-TEE(BL32) + u-boot
rk3588_spl_loader_v1.21.114.bin    SPL loader（后面所有 rkdeveloptool db 都用它）
```

确认 AMP 真的开了：
```bash
grep -E 'CONFIG_(AMP|ROCKCHIP_AMP)=y' configs/rk3588_defconfig     # 应有两行
grep -E 'CONFIG_(AMP|ROCKCHIP_AMP)=y' .config                      # 编完后的 .config 也应有
```

> AMP 需要 trust（BL31/trust.img）。DDR 与 BL31 blob 来自 `rkbin`，
> `RKTRUST/RK3588TRUST.ini` 与 `RKBOOT/RK3588MINIALL.ini` 保持官方即可，不要改。
> （changelog 里有一轮尝试换成主线 OP-TEE，vendor BL31 与之不兼容，启动卡死，已还原。）

### 6.2 Linux 内核（6.1.141）

```bash
cd /media/1t/openvela/kernel
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# 基础 defconfig + 两个我们加的 fragment
make rockchip_linux_defconfig
./scripts/kconfig/merge_config.sh -m -O . .config \
    arch/arm64/configs/rk3588_g610.config \
    arch/arm64/configs/rk3588_wifibt.config
make olddefconfig

make -j$(nproc) Image modules
make rockchip/rk3588-evb7-v11-linux-amp.dtb
```

两个 fragment 的作用：

| fragment | 内容 |
|---|---|
| `rk3588_g610.config` | 关 MALI400/MIDGARD，开 `MALI_BIFROST` + `MALI_CSF_SUPPORT` + `MALI_PLATFORM_NAME="rk"` —— RK3588 的 G610 是 Valhall/Bifrost，默认 defconfig 选错驱动会让 weston 报 "No mali devices found" |
| `rk3588_wifibt.config` | `CONFIG_AP6XXX=m`（bcmdhd 编成模块，官方设计）+ `WL_ROCKCHIP=y` + 关掉 `WIFI_LOAD_DRIVER_WHEN_KERNEL_BOOTUP` —— 编成 built-in 会早于 rfkill-wlan probe，导致 `WL_REG_ON=-1` 起不来 |

rpmsg 用户态接口（`RPMSG_CHAR` / `RPMSG_CTRL` / `RPMSG_NSH_TTY`，都是 `=m`）已经在
`rockchip_linux_defconfig` 里（commit `22c9d47b6`），不需要额外 fragment。

**打 boot.img**（Android boot image：header + kernel Image + Rockchip RSCE resource）：

```bash
cd /media/1t/openvela/kernel
./scripts/resource_tool arch/arm64/boot/dts/rockchip/rk3588-evb7-v11-linux-amp.dtb \
    logo.bmp logo_kernel.bmp
./scripts/mkbootimg --kernel arch/arm64/boot/Image --second resource.img -o boot.img
```

> ⚠️ **改了 dts 就必须重打并重刷 boot.img**，否则 Linux 侧看到的还是旧的 carveout 尺寸。
> `boot.img` 是确定性产物，同输入可复现（md5 可以当锚点）。

**需要 push 到板上的内核模块**（都是 `.ko`，构建产物，不入库）：
```
drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd/bcmdhd.ko    → /lib/modules/bcmdhd.ko
drivers/rpmsg/rpmsg_char.ko                                    → /userdata/
drivers/rpmsg/rpmsg_ctrl.ko                                    → /userdata/（当前用不到）
drivers/tty/rpmsg_nsh_tty.ko                                   → /userdata/
```

### 6.3 NuttX — cpu_l3（AArch64）

```bash
cd /media/1t/openvela/work/nuttx
export PATH=/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH

make distclean
./tools/configure.sh -l evb7-amp:nsh
make -j8
aarch64-none-elf-size nuttx
```

产物 `nuttx.bin`（链接/加载地址 `0x30000000`）。

自查 MMU 表真的进了二进制（不是"编过了"）：
```bash
addr=$(aarch64-none-elf-nm nuttx | grep -w g_mmu_regions | awk '{print $1}')
aarch64-none-elf-objdump -s -j .rodata --start-address=0x$addr \
    --stop-address=$((0x$addr + 0x90)) nuttx
# g_mmu_regions 里应见 ...00000031 00000000 00000031 00000000 00008000...
#   = amp-shmem base 0x31000000 / size 0x00800000（8MB）
```

### 6.4 NuttX — PMU Cortex-M0（ARMv6-M）

> ⚠️ **两个 NuttX 共用同一棵树**，切配置必须 `make distclean`，会顶掉另一个的 `.config`。
> 先把已验证的 `nuttx.bin` 备份出去。两个 defconfig 都已提交，可随时切回。

```bash
cd /media/1t/openvela/work/nuttx
cp nuttx.bin /tmp/nuttx-cpul3.bin              # 先保住 cpu_l3 的产物

export PATH=/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
make distclean
./tools/configure.sh -l evb7-m0:nsh
make -j8

cp nuttx.bin /media/1t/openvela/rk3588-amp-demo/m0/nuttx-m0.bin
```

改了 defconfig 之后要存回去：
```bash
make savedefconfig
cp defconfig boards/arm/rk3588-m0/evb7-m0/configs/nsh/defconfig && rm -f defconfig
```

> ⚠️ 改 `configs/*/defconfig` 后**必须** `make distclean` + `configure.sh` 重新生成
> `.config`，否则旧 `.config` 里的 `# CONFIG_XXX is not set` 不会变。
> 而且 `olddefconfig` **不会**把已经写成 `=y` 的项按新默认改掉 —— 要关掉必须删行。

### 6.5 Linux 用户态：`amp_fb_show`（采集 + 识别 + 显示 + 触摸转发）

```bash
cd /media/1t/openvela/rk3588-amp-demo
RK=/media/1t/openvela/rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/include
RGA=/media/1t/openvela/rknn-toolkit2/rknpu2/examples/3rdparty/rga/include

# 带 NPU 的完整版
aarch64-linux-gnu-gcc -O2 -ftree-vectorize -Wall -Wextra -pthread -DAMP_WITH_RKNN \
    -I$RK -I$RGA -idirafter /media/1t/openvela/kernel/include/uapi \
    -o amp_fb_show amp_fb_show.c

# 无厂商头的默认构建也必须能过（去掉 -DAMP_WITH_RKNN -I$RK -I$RGA）
aarch64-linux-gnu-gcc -O2 -ftree-vectorize -Wall -Wextra -pthread \
    -idirafter /media/1t/openvela/kernel/include/uapi -o /tmp/amp_fb_show_nornn amp_fb_show.c

# 动态依赖只应有 libc + ld（rknn/rga 是 dlopen 的，不链进去）
aarch64-linux-gnu-objdump -p amp_fb_show | grep NEEDED
```

**主机侧回归测试**（不需要板子）：
```bash
./tests/run.sh        # 200 种子 yolo 解码对比 + emit 12 项（含旋转/角点）+ 触摸解码
```

其他诊断工具（同样的编译方式，都是单文件）：
`amp_shm_test.c` `amp_shm_scan.c` `amp_rga_probe.c` `amp_isp_range.c` `amp_npu_probe.c`

---

## 7. 打包 AMP 载荷 `amp.img`

一个 FIT 里放两个固件，u-boot 按 `type` 分流拉核：

| image | 目标 | 关键属性 |
|---|---|---|
| `amp1` | cpu_l3（A55） | `type="firmware"` `arch="arm64"` `cpu=<0x300>` `load=<0x30000000>` → 走 `psci_cpu_on` |
| `mcu` | PMU M0 | `type="standalone"` `arch="arm"` `thumb=<1>` `load=<0x07a00000>` **`uc_start`/`uc_end`** `exsram_start=<0x07b00000>` |

```bash
cd /media/1t/openvela/rk3588-amp-demo
cp /media/1t/openvela/work/nuttx/nuttx.bin nuttx.bin        # cpu_l3（§6.3 那次的产物）
# m0/nuttx-m0.bin 已在 §6.4 拷好
/media/1t/openvela/u-boot/tools/mkimage -f amp-m0-nuttx.its -E -p 0xe00 amp.img
```

核对：
```bash
/media/1t/openvela/u-boot/tools/mkimage -l amp.img      # 看两个 image 的 load 与 sha256
fdtget -t x amp.img /images/mcu uc_start uc_end load
```

> ⚠️⚠️ **`uc_start`/`uc_end` 不是可选项。** u-boot 的 `standalone_handler()` 只有在
> `sram_start|exsram_start|experi_start|uc_start|uc_end` 至少有一个时才调
> `fit_standalone_ext_release()`（RK3588 唯一实现的那个）。缺了就静默落进
> `__weak fit_standalone_release()` 空桩，**照样打印 `...OK`**，而 CODE_START 从未编程、
> M0 复位从未解除。这个坑吃掉了两轮上板。
>
> ⚠️ **`amp.img` 的 md5 不可复现** —— `mkimage` 在 FIT 头写构建时间戳（字节 43-45）。
> 要验证内容，锚点是里面 `nuttx.bin` 的 sha256（`mkimage -l` 会打印），不是文件 md5。

---

## 8. 刷机

### 8.1 首次全量

见 §5（改分区表那套）。

### 8.2 日常增量（绝大多数情况）

```bash
SPL=/media/1t/openvela/u-boot/rk3588_spl_loader_v1.21.114.bin

# 只改了 NuttX（任一域）→ 只刷 amp
rkdeveloptool db $SPL
rkdeveloptool wlx amp /media/1t/openvela/rk3588-amp-demo/amp.img
rkdeveloptool rd

# 改了 dts 或内核 → 还要刷 boot
rkdeveloptool db $SPL
rkdeveloptool wlx boot /media/1t/openvela/kernel/boot.img
rkdeveloptool wlx amp  /media/1t/openvela/rk3588-amp-demo/amp.img
rkdeveloptool rd

# 只改了 Linux 用户态程序 → 不用刷机，adb push 即可
adb push amp_fb_show /userdata/amp_fb_show && adb shell "chmod +x /userdata/amp_fb_show"
```

### 8.3 回读校验（比"写到 100%"可靠）

LBA 数 = `ceil(字节数 / 512)`：
```bash
rkdeveloptool rl 0x8000    79368 /tmp/rb_boot.bin && head -c 40636416 /tmp/rb_boot.bin | md5sum
rkdeveloptool rl 0x1CB8000  2167 /tmp/rb_amp.bin  && head -c  1108992 /tmp/rb_amp.bin  | md5sum
```

刷完确认板上真的是新配置，而不是相信自己刷对了：
```bash
adb shell "od -An -tx1 /proc/device-tree/reserved-memory/amp-shmem@31000000/reg"
# 期望 00 00 00 00 31 00 00 00  00 00 00 00 00 80 00 00
#      → base 0x31000000, size 0x00800000（8MB）
```

---

## 9. 板上部署（rootfs 侧）

`wlx boot`/`wlx amp` 是分区级写入，不动 rootfs。但 **`wl 0 <整镜像>` 会覆盖 rootfs**，
下面这些东西会全部消失，症状回到"触摸不转发 + 抢不到 DRM"。

### 9.1 推文件

```bash
# 内核模块
adb push /media/1t/openvela/kernel/drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd/bcmdhd.ko /lib/modules/
adb push /media/1t/openvela/kernel/drivers/rpmsg/rpmsg_char.ko      /userdata/
adb push /media/1t/openvela/kernel/drivers/tty/rpmsg_nsh_tty.ko     /userdata/

# 用户态程序
adb push /media/1t/openvela/rk3588-amp-demo/amp_fb_show /userdata/
adb shell "chmod +x /userdata/amp_fb_show"

# NPU 运行时包（约 23MB，不入库）
adb shell "mkdir -p /userdata/npu-a17"
adb push /media/1t/openvela/rk3588-amp-demo/npu-a17/. /userdata/npu-a17/
#   需要：librknnrt.so  librga.so  yolov5s-640-640.rknn  coco_80_labels_list.txt  model/

# 部署脚本与 systemd unit
adb push /media/1t/openvela/rk3588-amp-demo/deploy/. /userdata/deploy/
```

> ⚠️ **`adb push` 目录嵌套陷阱**：`adb push deploy /userdata/deploy` 在目标已存在时会变成
> `/userdata/deploy/deploy/`，于是跑到的是旧脚本。**用 `adb push deploy/. /userdata/deploy/`**。
> 这个坑导致过一轮"install 完点击没响应"的误诊。

### 9.2 安装

```bash
adb shell "cd /userdata/deploy && ./install.sh"
adb shell reboot
```

`install.sh` 做的事：
- 装 4 个 systemd unit：`bcmdhd`（WiFi 模块）、`brcm-bt`（蓝牙 patchram attach）、
  `amp-rpmsg`（insmod rpmsg 模块）、`amp-camera`（`amp_fb_show -C -r 90 -A -M ... -Z`）
- 装 `/etc/NetworkManager/conf.d/wifi-no-randmac.conf`（**不关 MAC 随机化 WiFi 连不上**）
- 装 `/usr/local/bin/rk3588-selftest`
- **enable 前逐个 `systemd-analyze verify`** —— `systemctl enable` 只建符号链接、不读文件内容，
  一个丢了 `[Unit]` 头的 unit 能被完美 enable，然后那一节里所有排序指令被静默丢弃
- `systemctl set-default multi-user.target` —— 桌面持有 DRM master，`amp_fb_show` 会直接
  退出并报 `cannot become DRM master`

### 9.3 NuttX 侧不需要部署

`ampui` 由 NuttX 的 ROMFS `rcS` 自启动（`CONFIG_ETC_ROMFS`），已编进固件。

---

## 10. 验证清单

### 10.1 u-boot 阶段（串口，1500000）

```
AMP: Brought up cpu[300] with state 0x12, entry 0x30000000 ...OK    ← cpu_l3
Handle standalone: 'nuttx-pmu-m0' at 0x07a00000 ...OK               ← M0（见下警告）
```
两条 `Sysmem Warn ... overlap` 是"镜像落在自己的保留区内"，正常。

> ⚠️ M0 那条 `...OK` **即使没真拉起来也会打印**（`__weak` 空桩 `return 0`）。
> M0 必须靠 §10.3 的 devmem 复核。

### 10.2 Linux 侧

```bash
nproc                                     # 7，不是 8 → cpu_l3 已交给 AMP
dmesg | grep -i 'smp: Brought up'         # Brought up 1 node, 7 CPUs
dmesg | grep -i 'rpmsg host is online'    # 两条：virtio0(cpu_l3) + virtio1(M0)
ls /sys/bus/rpmsg/devices/
ls -l /dev/ttyNSH0 /dev/ttyNSH1 /dev/rpmsg0
busybox devmem 0xfec60000 32              # A2B_INTEN = 0x0C（bit3 cpu_l3 + bit2 M0）
systemctl get-default                     # multi-user.target
systemctl status amp-rpmsg amp-camera --no-pager
cat /sys/kernel/debug/dri/0/clients       # master 列应为空（桌面没在占）
```

### 10.3 M0 活体检查（不用连 shell）

```bash
busybox devmem 0x07ae0000 32    # magic，应为 0x414D5030 ("AMP0")
busybox devmem 0x07ae0004 32    # 秒计数，连读两次应 +1 ← 关键判据
busybox devmem 0xfd7f0a00 32    # PMU1CRU_SOFTRST_CON00，bit13=0 = 已解复位
busybox devmem 0xFD58A060 32    # PMU1GRF_SOC_STS：bit8 lockup / bit9 sleeping / bit7 halted
```
`0x07ae0004` 每秒 +1 由 `sleep(1)` 驱动 —— 递增同时证明 **SysTick 在投递 + 调度器在跑**。

### 10.4 两个 NuttX 的 nsh

```bash
picocom --imap lfcrlf --omap crlf /dev/ttyNSH0      # cpu_l3
picocom --imap lfcrlf --omap crlf /dev/ttyNSH1      # M0
```
退出 `Ctrl-A Ctrl-Q`。`--omap crlf` 必须加（否则 Enter 发的 `\r` nsh 不认，得按 Ctrl-J）。

分清是哪个域：`uname -a` 看架构（`aarch64` vs `arm`），或 `free` 看堆（约 16MB vs 约 800KB）。

### 10.5 摄像头 + 识别

```bash
journalctl -u amp-camera -f
# 期望：panel is up, forwarding touch, publishing camera frames, ... publishing detections
#       camera 约 28fps、detect 约 28fps、detector 约 27/s、0 doorbells failed
```
屏上应看到相机画面 + 人物框；主页有呼吸心跳点。相机横装，画面是侧倒的。

手动跑（调试用，注意**必须带全 flag**）：
```bash
adb shell
cd /userdata/npu-a17 && export LD_LIBRARY_PATH=$PWD:$LD_LIBRARY_PATH
/userdata/amp_fb_show -C -r 90 -A -M yolov5s-640-640.rknn -Z
```
> ⚠️ 少 `-A -M` 就只有显示、没有检测。changelog 里有一轮把"没有框"当回归查了一整轮，
> 原因就是给的验证命令少了这两个 flag。

---

## 11. 回退

```bash
SPL=/media/1t/openvela/u-boot/rk3588_spl_loader_v1.21.114.bin
BK=/media/1t/openvela/rk3588-amp-demo/_a55_backup

# 回退到 A55-only 的 AMP 载荷（M0 不拉）
rkdeveloptool db $SPL && rkdeveloptool wlx amp $BK/amp.img.a55-nuttx-v9 && rkdeveloptool rd

# 回退 boot.img（按要退到哪一步选）
rkdeveloptool db $SPL && rkdeveloptool wlx boot $BK/boot.img.pre-8mb-carveout && rkdeveloptool rd

# 回退官方 u-boot（AMP 全关）
rkdeveloptool db $SPL
rkdeveloptool wlx uboot /media/1t/openvela/u-boot/_amp_backup/uboot.img.orig
rkdeveloptool rd

# 完全回官方固件：重跑 §4
```

`_a55_backup/`（`/media/1t/openvela/rk3588-amp-demo/_a55_backup/`）里的备份：

| 文件 | 退回到哪一步之前 |
|---|---|
| `amp.img.a55-nuttx-v9` | M0 还没被拉起（只有 cpu_l3） |
| `nuttx.bin.a55-v9` | 同上，单独的 cpu_l3 固件 |
| `nuttx.bin.pre-isr-split` | cpu_l3 的 RX 还是 ISR 里直调 OpenAMP callback |
| `boot.img.pre-m0-rpmsg` | dts 还没加 M0 那条 rpmsg link |
| `boot.img.pre-reserved-plane` | dts 还没把 Esmart3 从 Linux 手里摘出来 |
| `boot.img.pre-8mb-carveout` | `amp_shmem` 还是 4MB（全屏旋转之前） |
| `rpmsg_nsh_tty.ko.4f22002e` | tty UAF 修复那版（重连稳定） |

`/media/1t/openvela/u-boot/_amp_backup/` 里是 `uboot.img.orig` / `tee.bin.orig` /
`rk3588_spl_loader_v1.21.114.bin`。

---

## 12. 高频踩坑速查

| 症状 | 真因 | 出处 |
|---|---|---|
| 复位循环 | 只 `db` 没 `ul`，eMMC 里残留旧 SPL | §4 |
| systemd emergency mode | 重新分区后没刷官方 `userdata.img` | §5 |
| M0 打印 `...OK` 但内存全 `0xFFFFFFFF` | `.its` 缺 `uc_start/uc_end`，落进 `__weak` 空桩 | §7 |
| weston `No mali devices found` | 内核选了 MALI400/MIDGARD，G610 要 `MALI_BIFROST` | §6.2 |
| 没有 wlan0 | bcmdhd 编成 built-in，早于 rfkill-wlan probe | §6.2 |
| WiFi 能扫不能连（`config-failed`） | NetworkManager MAC 随机化，bcmdhd 拒绝在 up 状态改 MAC | §9.2 |
| 没有 hci0 | 官方 `wifibt-init.sh` 跳过用户态 attach（假设 serdev 内核） | `brcm-bt.service` |
| `cannot become DRM master` | 桌面持有 DRM master | §9.2 |
| 触摸没反应 | `amp_fb_show` 没跑（触摸是它转发的），或 `/dev/rpmsg0` 不存在 | §10.5 |
| 跑约 1027 帧后 NuttX 断言 | MMU 映射尺寸短了（`rk3588_boot.c` 的 `AMP_SHMEM` 没跟 dts 一起改到 8MB） | changelog A-18 |
| 框压扁且错 90° | 只转了像素没转坐标（`det_emit()` 的 `rot`） | changelog A-18 |
| `/dev/ttyNSH0` 二次连接卡 D 状态 | `rpmsg_nsh_tty` 的 `tty_port_get()` 漏了，kref 不平衡 → UAF | changelog（已修） |
| 改了 defconfig 不生效 | 没 `make distclean` + `configure.sh`；`olddefconfig` 不改已写成 `=y` 的项 | §6.4 |
| `amp.img` md5 每次都不同 | FIT 头有构建时间戳，本来就不可复现 | §7 |

### 三个反复出现的方法论教训

1. **一句 `...OK` 不等于功能真的跑了。** M0 空桩、mailbox 驱动的 `version: 0x0100`
   （是驱动里的常量，probe 全程只 ioremap）、`open() -> 0`（真实含义是 fd 0/1/2 全空）
   —— 三次同类误判。先确认目标代码路径是否真被执行，再解释"执行结果为何不对"。
2. **软复位不清 DRAM。** `no-map` 区的旧 magic 会伪装成当前状态。存在性判据要换成
   **单调递增的计数器**。
3. **一个"改尺寸"的需求要先 `grep` 出这个数字被写在几个地方。** `AMP_SHM_SIZE` 在三处
   （dts / MMU 表 / 头文件），三种失败方式完全不同：dts 大声报错、MMU 静默到越界才炸、
   头文件决定两侧算术。

---

## 13. 复现顺序（一页速查）

```bash
# 0) 主机准备：工具链 + rkdeveloptool                        §2
# 1) 刷官方固件，确认板子好                                  §4
# 2) 编 u-boot（AMP 已在 defconfig）
cd /media/1t/openvela/u-boot
export PATH=/media/1t/openvela/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin:$PATH
./make.sh rk3588

# 3) 编内核 + 打 boot.img
cd /media/1t/openvela/kernel
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make rockchip_linux_defconfig
./scripts/kconfig/merge_config.sh -m -O . .config \
    arch/arm64/configs/rk3588_g610.config arch/arm64/configs/rk3588_wifibt.config
make olddefconfig && make -j$(nproc) Image modules
make rockchip/rk3588-evb7-v11-linux-amp.dtb
./scripts/resource_tool arch/arm64/boot/dts/rockchip/rk3588-evb7-v11-linux-amp.dtb logo.bmp logo_kernel.bmp
./scripts/mkbootimg --kernel arch/arm64/boot/Image --second resource.img -o boot.img

# 4) 编 NuttX cpu_l3
cd /media/1t/openvela/work/nuttx
export PATH=/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH
make distclean && ./tools/configure.sh -l evb7-amp:nsh && make -j8
cp nuttx.bin /media/1t/openvela/rk3588-amp-demo/nuttx.bin

# 5) 编 NuttX M0（同一棵树，必须 distclean）
export PATH=/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin:$PATH
make distclean && ./tools/configure.sh -l evb7-m0:nsh && make -j8
cp nuttx.bin /media/1t/openvela/rk3588-amp-demo/m0/nuttx-m0.bin

# 6) 打 amp.img
cd /media/1t/openvela/rk3588-amp-demo
/media/1t/openvela/u-boot/tools/mkimage -f amp-m0-nuttx.its -E -p 0xe00 amp.img

# 7) 编 amp_fb_show                                          §6.5
# 8) 刷机（首次带 gpt parameter-amp.txt）                    §5 / §8
# 9) push 模块 + npu-a17 + deploy，跑 install.sh，reboot      §9
# 10) 按验证清单逐条确认                                     §10
```

---

*本文基于 `rk3588-evb7-v11.md`（主 changelog）整理。所有命令都出自已在真机上跑通过的记录；
遇到与实际不符之处，以 changelog 里对应里程碑的记录为准，并请回头更新本文。*
