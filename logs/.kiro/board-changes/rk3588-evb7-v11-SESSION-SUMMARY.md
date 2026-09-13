# RK3588 EVB7 V11 — 会话交接总结 (Session Handoff)

> 新会话请先读本文件 + 同目录的 `rk3588-evb7-v11.md`(逐条改动 changelog)。
> 目标板:Rockchip RK3588 EVB7 LP4 V11(官方评估板)。

## 0. 总目标与现状

目标:**一个能跑 AMP + 桌面 + WiFi 的系统**(不是必须 Armbian)。

最终决定:**留在官方 BSP 整栈**,不移植到 Armbian(vendor 内核强绑 vendor 用户态
libmali/bcmdhd,Armbian rootfs 反而要补这些,净增工作量)。

| 能力 | 状态 |
|---|---|
| AMP(cpu_l3 跑独立固件) | ✅ 验证通过(共享内存心跳递增) |
| 桌面(Weston + Mali G610 硬件加速) | ✅ 验证通过 |
| WiFi(AP6398S,wlan0 开机自动) | ✅ 验证通过(bcmdhd 模块 + systemd 自动加载) |
| 蓝牙(AP6398S BT / ttyS9) | ⏳ **进行中**(见 §6) |
| NuttX 作为 AMP 核(替换裸机 demo) | 🔬 **已开始移植**(见 §7) |

## 1. 各仓库路径与分支

| 仓库 | 路径 | 分支 |
|---|---|---|
| Armbian 构建框架 | `/media/1t/openvela/armbain` | main |
| 官方内核 6.1.141(实际用它编 boot.img) | `/media/1t/openvela/kernel` | develop-6.1 |
| Armbian vendor 内核 6.1.115(仅参考) | `/media/1t/openvela/linux-rockchip` | — |
| 官方 vendor u-boot 2017.09(带 CONFIG_AMP) | `/media/1t/openvela/u-boot` | next-dev |
| rkbin(DDR/BL31/BL32 blob) | `/media/1t/openvela/rkbin` | — |
| AMP 裸机 demo | `/media/1t/openvela/rk3588-amp-demo` | master |
| rkdeveloptool | `/media/1t/openvela/rkdeveloptool` + 已装入 PATH | — |
| 官方固件(rockdev) | `/media/1t/openvela/rk3588_EVB7/RK3588-EVB7-V11-DEBIAN_V1.0.0_20260620/RK3588-EVB7-V11-LINUX/rockdev` | — |
| NuttX / openvela 工作区 | `/media/1t/openvela/work`(nuttx、apps、vendor…) | — |

## 2. 已提交的 commit

- **armbain**(main):板级配置 + AMP/GPU/WiFi 全过程 changelog
- **kernel**(develop-6.1):
  - `rk3588-evb7-v11-linux-amp.dts` + dts Makefile 注册
  - `arch/arm64/configs/rk3588_g610.config`(Mali G610 Bifrost/CSF)
  - `arch/arm64/configs/rk3588_wifibt.config`(AP6398S WiFi 做模块)
- **u-boot**(next-dev):`configs/rk3588_defconfig` 加 `CONFIG_AMP` + `CONFIG_ROCKCHIP_AMP`
- **rk3588-amp-demo**(master):裸机 demo 全套源码(git init)

工具链:vendor u-boot 用 **linaro 6.3.1**(`/media/1t/openvela/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-...`,make.sh 自动找);内核用系统 `aarch64-linux-gnu-`(11.4)。宿主机依赖:`gcc-aarch64-linux-gnu`、`device-tree-compiler`、`u-boot-tools`、`lz4`、`qemu-user-static`+`binfmt-support`。

## 3. 启动链构成(官方栈 + AMP)

```
BootROM → loader(rk3588_spl_loader_v1.21.114.bin / 官方 MiniLoaderAll.bin)
        → 官方 vendor u-boot(uboot.img,含 CONFIG_AMP + trust FIT)
        → amp_cpus_on() 从 `amp` 分区读 amp.img,拉起 cpu_l3(MPIDR 0x300)@0x30000000
        → 加载 boot.img(FIT: Image + rk3588-evb7-v11-linux-amp.dtb + resource)
        → Linux(7 核)
```
- 串口:UART2 `0xfeb50000`,**波特率 1500000**,console=ttyFIQ0
- 分区:用改过的 `rk3588-amp-demo/parameter-amp.txt`(在 oem 后加 16MB `amp` 分区,userdata 顺延)

## 4. AMP 关键事实

- AMP 核 = cpu_l3,MPIDR **0x300**;固件 load/entry = **0x30000000**
- 内存布局(内核 -amp dts 的 reserved-memory,均 no-map):
  - `amp-core@30000000`(16MB)= AMP 固件区(避开内核/M0/OP-TEE/CMA)
  - `amp-shmem@31000000`(4MB)= rpmsg 共享内存(原 0x8200000 撞 OP-TEE@0x08400000,已挪)
- 裸机 demo(`rk3588-amp-demo/`):`amp_start.S`(先设 SP=0x30200000 再进 C)+ `amp1.c`
  (打印 UART5 0xfeb80000 + 心跳:magic 0x414D5033 @0x30000800,counter++ @0x30000804)
- 验证法(无 UART5 引脚):Linux 侧 `busybox devmem 0x30000804 32` 多次,数值递增 = cpu3 在跑
- u-boot amp.img 打包:`<u-boot>/tools/mkimage -f amp.its -E -p 0xe00 amp.img`,`amp.its` 里 cpu=0x300 load=0x30000000

## 5. GPU / 桌面(已解决)

- 官方 rootfs 已有 GNOME + weston + Xorg + **libmali**(libmali-valhall-g610-g29p1)
- 坑:`rockchip_linux_defconfig` 默认开老 GPU 驱动(MALI400/MIDGARD),RK3588 G610 需
  **`CONFIG_MALI_BIFROST` + `MALI_CSF_SUPPORT`**(默认关)→ 无 /dev/mali0 → weston "No mali devices found"
- 修:`rk3588_g610.config`(BIFROST=y、CSF=y、PLATFORM_NAME="rk",关 MALI400/MIDGARD)
- 起桌面:`systemctl enable/start S49weston`(weston);或 `set-default graphical.target`+gdm(GNOME)

## 6. WiFi / 蓝牙(WiFi 已解决,BT 进行中)

WiFi 芯片 **AP6398S(BCM4359)**,SDIO bcmdhd;BT 走 **/dev/ttyS9 (uart9)**,固件 `/lib/firmware/brcm/BCM4359C0.hcd`。

**WiFi 已解决**:
- 坑:defconfig 把 bcmdhd 编成**内建**,3.2s 就自启(早于 rfkill-wlan probe)→ WL_REG_ON=-1 → SDIO timeout → 无 wlan0
- Kconfig 真相:`menuconfig BCMDHD` 只是 bool 门控;真正模块/内建开关是 **`config AP6XXX`(tristate)**,
  `rkwifi/Makefile: obj-$(CONFIG_AP6XXX) += bcmdhd/`
- 修:`rk3588_wifibt.config` 设 **`CONFIG_AP6XXX=m`**(bcmdhd 做模块 = 官方做法),后加载避开时序竞争
- 部署:reflash boot.img;把我们编的 `.../rkwifi/bcmdhd/bcmdhd.ko` 放到板子 `/lib/modules/`(散装路径,
  无版本目录/未 depmod → 用 `insmod` 全路径);开机自动加载用 systemd oneshot 服务 `bcmdhd.service`
  (`ExecStart=/sbin/insmod /lib/modules/bcmdhd.ko`,After=systemd-modules-load.service)—— 已验证 wlan0 开机自动出现
- 小警告:`clm_bcm4359c0_ag.blob` 路径找错(找 `/` 而非 /lib/firmware),WiFi 仍可用(无 CLM 只是 regulatory 受限)

**蓝牙(未完成,下一步在这)**:
- 内核侧已就绪:`BT=y, BT_HCIUART=y, BT_HCIUART_H4=y, RFKILL_RK=y`
- 官方 rootfs 的 BT/WiFi 加载入口 = **`/usr/lib/systemd/system/wifibt-init.service` → `/usr/bin/wifibt-init.sh start`**
- `wifibt-init.sh` 里 BT attach 分支(按芯片厂):
  - Broadcom:`init_bt_brcm_uart()` 用 **`brcm_patchram_plus1 --enable_hci --no2bytes ... --patchram $FIRMWARE_DIR/ $WIFIBT_BT_TTY`**(FIRMWARE_DIR 会进 `/lib/firmware/brcm`)
  - 还有 `insmod_bt()`:`try_insmod btrtl / btbcm`,`do_insmod hci_uart`
- 现象:日志里 BT_RFKILL 电源在 t=47/49 被反复拉起(attach 在试)但**没生成 hci0**
- **下一步排查方向**:
  1. 确认 `brcm_patchram_plus1` 存在、`$WIFIBT_BT_TTY` 是否设为 `/dev/ttyS9`(RK 配置来源:芯片类型/wifibt 配置文件)
  2. 因为我们换了内核:`btbcm.ko`/`hci_uart.ko` 官方是散装模块(vermagic 对不上)→ 要么把这些编成模块并放我们编的版本,要么内核里 `BT_BCM`/`hci_uart` 内建(检查 `CONFIG_BT_BCM`)
  3. 手动验证:`hciattach -n /dev/ttyS9 bcm43xx 1500000 flow -`(或 brcm_patchram_plus1 指向 BCM4359C0.hcd),看 `hciconfig -a` 是否出 hci0
  4. 板上诊断:`systemctl status wifibt-init`、`journalctl -u wifibt-init -b`、`ps aux|grep -E 'patchram|hciattach'`、`ls /dev/ttyS9`、`rfkill list`

## 7. NuttX 作为 AMP 核(新方向,进行中)

裸机 demo 已验证通路;下一步把 AMP 核换成 **openvela/NuttX + OpenAMP**(用 amp-shmem 做 rpmsg)。
已开始 RK3588 NuttX 移植(编辑器里打开的文件):
- `work/nuttx/arch/arm64/src/rk3588/`:`rk3588_rptun.c/.h`(rpmsg/rptun)、`rk3588_rsctable.c/.h`(资源表)、`rk3588_serial.c`
- `work/nuttx/boards/arm64/rk3588/evb7-amp/src/evb7_amp_bringup.c`
- `work/nuttx/arch/arm64/src/common/`:`arm64_gicv3.c`、`arm64_arch_timer.c`
- `work/nuttx/arch/arm64/include/rk3588/irq.h`
- 参考:`work/nuttx/arch/arm/src/mx8mp/mx8mp_rptun.c`(i.MX8MP 的 rptun 实现)
- NuttX 原本只有 rk3399 移植(`arch/arm64/src/rk3399`),rk3588 是新建
- SMC/AMP 参考:`u-boot/arch/arm/mach-rockchip/rockchip_smccc.c`(SIP_AMP_CFG)
- 关键约束:NuttX 跑在 cpu3、load 0x30000000、rpmsg 共享内存用 amp-shmem@31000000;
  与 Linux 侧 rk3588-amp.dtsi 的 rpmsg@7c00000 + rpmsg-dma@8000000 对接

## 8. 常用烧写命令(MASKROM,rkdeveloptool 已在 PATH)

```bash
UB=/media/1t/openvela/u-boot; KZ=/media/1t/openvela/kernel; AMP=/media/1t/openvela/rk3588-amp-demo
ROCK=/media/1t/openvela/rk3588_EVB7/RK3588-EVB7-V11-DEBIAN_V1.0.0_20260620/RK3588-EVB7-V11-LINUX/rockdev
sudo rkdeveloptool db  $UB/rk3588_spl_loader_v1.21.114.bin   # 内存下载器
sudo rkdeveloptool ul  $UB/rk3588_spl_loader_v1.21.114.bin   # 写 loader(首次/换 loader 才需)
sudo rkdeveloptool gpt $AMP/parameter-amp.txt                # 写分区表(含 amp 分区)
sudo rkdeveloptool wlx uboot $UB/uboot.img                   # AMP u-boot
sudo rkdeveloptool wlx boot  $KZ/boot.img                    # -amp 内核+dtb
sudo rkdeveloptool wlx amp   $AMP/amp.img                    # AMP 固件
sudo rkdeveloptool wlx rootfs $ROCK/rootfs.img               # 官方 rootfs(需要时)
sudo rkdeveloptool wlx userdata $ROCK/userdata.img           # 重分区后必须刷,否则 emergency
sudo rkdeveloptool rd
```
只改内核时:重编 `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- rk3588-evb7-v11-linux-amp.img` → 只 `wlx boot`。

## 9. 重编内核(带 AMP + GPU + WiFi 三个片段)

```bash
cd /media/1t/openvela/kernel
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- rockchip_linux_defconfig
for f in rockchip_amp rk3588_g610 rk3588_wifibt; do
  ./scripts/kconfig/merge_config.sh -m .config arch/arm64/configs/$f.config
done
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) rk3588-evb7-v11-linux-amp.img
# 产物:boot.img(内核根目录);WiFi 模块:drivers/net/wireless/rockchip_wlan/rkwifi/bcmdhd/bcmdhd.ko
```

## 10. 踩过的坑速查

- host 无 arm64 binfmt → 装 qemu-user-static + binfmt-support(Armbian 构建需要)
- vendor u-boot make.sh 找不到工具链 → 需 linaro 6.3 在 `../prebuilts/...`(已就位)
- `./make.sh rk3588` 每次重跑 defconfig → AMP 配置要**烤进 rk3588_defconfig**,不能只 merge 到 .config
- 缺 dtc/lz4/mkimage → apt 装 device-tree-compiler / lz4 / u-boot-tools
- SD 卡无输出、直刷 eMMC → 用 MASKROM
- 复位循环 → `db` 只到内存,要 `ul` 才持久化 loader(旧 Armbian SPL 与官方 uboot.img 不匹配)
- AMP overlap → 固件挪到高地址专属 no-map 区 0x30000000
- 裸机 counter 卡 1 → 没设栈指针,加 amp_start.S 设 SP
- 桌面黑/无 GPU → G610 要 BIFROST/CSF,不是 MALI400/MIDGARD
- wlan0 不出现 → bcmdhd 内建时序竞争,改 AP6XXX=m 做模块 + systemd 后加载
- 串口 .S/Makefile 用 heredoc tab 被吞 → 用 printf 逐行写

---
最后更新:本会话结束时。BT 是唯一未收尾项;NuttX AMP 核移植为进阶方向。
