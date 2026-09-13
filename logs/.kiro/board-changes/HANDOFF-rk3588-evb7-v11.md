# 交接总结 — RK3588 EVB7 V11 板级适配 + NuttX AMP 移植

> 新窗口读这一份即可接上。详细逐条历史见同目录 `rk3588-evb7-v11.md`（changelog）。
> 全程用中文交流。终端多行输出常被截断——**一次只跑一条命令**。

---

## 0. 一句话现状

RK3588 EVB7 V11 板已在**官方 BSP 栈**上跑通 **AMP + 桌面 + WiFi + 蓝牙**；并把 **NuttX(openvela) 移植到 AMP 核 cpu_l3** 上稳定运行（启动/timer/调度/共享内存/与 Linux 共存全部验证）。当前正在做 **rpmsg**：共享内存 + mailbox 门铃 + 握手协议已全部打通并验证，正在接 **OpenAMP/rptun**（rptun 驱动骨架已写、尚未编译验证）。

---

## 1. 源码树位置（都在工作区外，用 execute_bash + 绝对路径访问）

| 路径 | 说明 | git 分支 |
|------|------|---------|
| `/media/1t/openvela/armbain` | 本仓库。**Armbian 构建路径已放弃**，现仅存放 changelog + skills；`config/boards/rk3588-evb7-v11.conf` 是死代码，不在任何构建/刷机路径上 | main |
| `/media/1t/openvela/kernel` | 官方 Rockchip BSP 内核 6.1.141 | develop-6.1 |
| `/media/1t/openvela/u-boot` | 官方 vendor u-boot 2017.09（带 CONFIG_AMP）| next-dev |
| `/media/1t/openvela/rkbin` | DDR/BL31/BL32 blobs | - |
| `/media/1t/openvela/rk3588-amp-demo` | AMP 载荷打包（amp.its/amp-nuttx.its/parameter/deploy）| master |
| `/media/1t/openvela/work` | **openvela(NuttX)** 树（repo 管理，`work/nuttx` 是 git）| 0712 |
| `/media/1t/openvela/rk3588_EVB7/.../rockdev` | 官方固件（boot/rootfs/uboot/MiniLoaderAll 等）| - |

- **工具链（NuttX）**：`/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin`（gcc 13.4）。编译前 `export PATH=...:$PATH`。
- **工具链（内核）**：系统 `aarch64-linux-gnu-gcc`（GCC 11.4）。
- **工具链（vendor u-boot）**：linaro 6.3.1 `/media/1t/openvela/prebuilts/gcc/linux-x86/aarch64/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu`。
- `rkdeveloptool` 已装在 PATH。
- **串口日志**：用户反复贴同一个文件 `/home/jinglin/log/ttyUSB1_20260711_182324.log`（UART2/ttyFIQ0 主控制台 @1500000，二进制，用 `strings` + `grep -a` 读，每次重刷后重新读）。

---

## 2. 已完成里程碑（全部真机验证）

### 板级（官方 BSP 栈）
- **AMP**：cpu_l3(MPIDR 0x300) 跑裸机固件，Linux 用 7 核，心跳验证 ✅
- **桌面**：Mali G610(Bifrost) + weston/GNOME ✅（内核 fragment `rk3588_g610.config`）
- **WiFi**：AP6398S/BCM4359，`bcmdhd.ko` 模块化 + `bcmdhd.service`；**NM MAC 随机化必须关**（`/etc/NetworkManager/conf.d/wifi-no-randmac.conf`，`wifi.scan-rand-mac-address=no`）否则连不上 ✅
- **蓝牙**：`brcm-bt.service` 用户态 `brcm_patchram_plus1` attach ttyS9 ✅

### NuttX AMP 移植（在 cpu_l3 上）— 里程碑 1 & 2 完成
- **启动/加载/打印** ✅、**timer + 调度器** ✅、**与 Linux 稳定共存 100 秒+** ✅、**共享内存 cache 一致** ✅

关键修复（都已提交）：
1. **primary 判定**：`get_cpu_id` 取 MPIDR Aff0；RK3588 每核独立 cluster(Aff1=核号,Aff0=0)→cpu_l3 算作 id 0→UP 模式天然 primary（无需改 head.S）。
2. **NR_IRQS**：181→512（RK3588 有 480 SPI；否则 GIC init 在 `arm64_gicv3.c:664` 断言）。`arch/arm64/include/rk3588/irq.h`
3. **GICv3 redistributor 定位（核心修复）**：原 `g_gic_rdists = GICR_BASE + up_cpu_index()*stride`，AMP 上 up_cpu_index()=0 选中 cpu0 的 GICR，导致 cpu_l3 的 timer PPI 从未使能。改为**按 MPIDR affinity 匹配 GICR_TYPER 探测本核 rdist**（cpu_l3→0xfe6c0000）。`arch/arm64/src/common/arm64_gicv3.c` `arm64_gic_init()`。通用修复。
4. **EL1 physical timer**：`arch/arm64/src/common/arm64_arch_timer.c` 用 `CONFIG_ARCH_CHIP_RK3588` 切到 CNTP(INTID 30)，不依赖 CNTVOFF。
5. **共享内存非缓存**：amp-shmem(0x31000000) MMU 映射 `MT_NORMAL_NC`；否则 cpu_l3 的写留 cache，Linux 读 DRAM 读到旧值（曾误判为"卡死"）。`rk3588_boot.c`

---

## 3. rpmsg 进展（当前工作）

### 已验证的关键事实（真机）
- Linux 是 **master/host**，NuttX 是 **remote/slave**；`rpmsg host is online`。
- **vring 地址固定**（Linux dts 定死，非 amp-shmem）：`vring0=0x07c00000`(Linux→NuttX)、`vring1=0x07c08000`(NuttX→Linux)，buffer pool=`0x08000000`(rpmsg-dma, 2MB)。
- **vring 参数**：`RPMSG_BUF_COUNT=64`、`RPMSG_VRING_ALIGN=0x1000`、`RPMSG_VRING_SIZE=0x8000`。
- **握手协议**（真机验证 data error 消失）：mailbox kick 时 `B2A_DAT=RPMSG_MBOX_MAGIC(0x524D5347 "RMSG")` + `B2A_CMD=link_id(0x03)`。Linux `rk_rpmsg_rx_callback` 验证 data==MAGIC，取 link_id，标 remote ready，触发 `vring_interrupt(vq0)`。
- **mailbox 硬件**：`mailbox0@0xfec60000`（rk3368/V1 寄存器）。cpu_l3=B 侧。
  - TX(cpu_l3→Linux)：写 `B2A_DAT(x)`(0x34+x*8) 再写 `B2A_CMD(x)`(0x30+x*8)——**必须写 DAT，写 DAT/CMD 才触发 STATUS+对方中断；只写 CMD 无效**。
  - RX(Linux→cpu_l3)：A2B 中断，`amp-irqs` 把 **SPI 100** 路由到 cpu_l3(affinity 3,0) → NuttX irq = 100+32 = **132**。A2B 寄存器：INTEN 0x00 / STATUS 0x04 / CMD(x) 0x08+x*8 / DAT(x) 0x0c+x*8。
  - rpmsg 用 mailbox0 通道 **0(rx)/3(tx)**（dts `mboxes = <&mailbox0 0 &mailbox0 3>`）。
  - Linux 收(B2A)中断 = GICv3 95/96（=SPI 63/64，注意 /proc/interrupts 显示 INTID=SPI+32）。
- Linux GICv3 驱动有 `CONFIG_ROCKCHIP_AMP=y` 保护（`rockchip_amp_check_amp_irq` 跳过 amp-irqs 的中断），dts `rk3588-amp.dtsi` 有 `rockchip,amp` 节点。

### GIC/中断三类速记
- SGI(0-15) 核间；PPI(16-31) 每核私有(timer 在此，GICR 管)；SPI(32+) 全局共享(mailbox/UART，GICD 路由)。
- RK3588：GICD `0xfe600000`(MMIO)、GICR `0xfe680000` 起每核 +0x20000(MMIO)、CPU interface = `ICC_*_EL1` 系统寄存器。

### rpmsg 当前代码状态（work/nuttx，**已写、尚未编译验证**）
新建（照 `arch/arm/src/mx8mp/mx8mp_rptun.c` + `mx8mp_rsctable.c` 模板）：
- `arch/arm64/src/rk3588/rk3588_rptun.h` / `.c` — rptun 驱动：ops(is_master=false, autostart=true)、`notify`=mailbox 写(DAT=MAGIC+CMD=link_id)、`rk3588_rptun_isr`=A2B 中断(irq 132)读清 STATUS 后 `callback(RPTUN_NOTIFY_ALL)`、`rk3588_rptun_init` enable A2B_INTEN + irq_attach + rptun_initialize。
- `arch/arm64/src/rk3588/rk3588_rsctable.h` / `.c` — resource table：2 vring @0x7c00000/0x7c08000、64 bufs、align 0x1000、carveout(buffer)@0x8000000/2MB、rsc table 放 0x7c0f000。
- `arch/arm64/src/rk3588/Make.defs` — 已加 `ifeq ($(CONFIG_RPTUN),y) CHIP_CSRCS += rk3588_rptun.c rk3588_rsctable.c`。
- `arch/arm64/src/rk3588/rk3588_boot.c` — MMU 已加 `AMP_RPMSG` region(0x07c00000, 6MB, MT_NORMAL_NC) 覆盖 vring+buffer。

### rpmsg 下一步 TODO（未做）
1. **board defconfig 开** `CONFIG_RPTUN=y` + OpenAMP（`CONFIG_OPENAMP` 等）——先确认 openamp 库在 apps 里（`apps/` 下找 open-amp，尚未确认存在/路径）。
2. **board bringup 调用** `rk3588_rptun_init(...)`（`boards/arm64/rk3588/evb7-amp/src/evb7_amp_bringup.c`），并去掉/保留现有心跳。
3. **编译**（见第 4 节命令），修编译错误。
4. **真机验证**：Linux 侧出现 rpmsg endpoint / `/dev/rpmsg*`，两边互发消息。
5. 风险：rockchip rpmsg 的 virtio 握手细节可能和标准 OpenAMP 有偏差（Linux 不读 remote 的 resource table，vring 地址两边各自固定），需真机调 vring 对齐 + name service。cache 一致已解决。

---

## 4. 构建 & 刷机命令（known-good）

### 编译 NuttX + 打包 amp.img
```bash
cd /media/1t/openvela/work/nuttx
export PATH=/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH
# 首次配置： ./tools/configure.sh -l evb7-amp:nsh
make -j4
aarch64-none-elf-objcopy -O binary nuttx nuttx.bin
cp nuttx.bin /media/1t/openvela/rk3588-amp-demo/nuttx.bin
/media/1t/openvela/u-boot/tools/mkimage -f /media/1t/openvela/rk3588-amp-demo/amp-nuttx.its -E -p 0xe00 /media/1t/openvela/rk3588-amp-demo/amp.img
```

### 刷 amp 分区（板子进 MASKROM）
```bash
cd /media/1t/openvela/rk3588-amp-demo
rkdeveloptool db /media/1t/openvela/u-boot/rk3588_spl_loader_v1.21.114.bin
rkdeveloptool wlx amp amp.img
rkdeveloptool rd
```
- 只改 cpu3 固件时只需重刷 amp 分区；改内核则重刷 boot 分区。
- NuttX 链接/加载地址 **0x30000000**（amp.its `cpu=0x300 load=0x30000000`）。NuttX RAM=16MB@0x30000000。

### 验证（Linux 侧，串口 shell）
```bash
busybox devmem 0x31000004 32   # NuttX timer 心跳(应递增)
busybox devmem 0x31000008 32   # 忙等心跳
dmesg | grep -iE 'rpmsg|mailbox|virtio'
cat /proc/interrupts | grep -i mailbox
```
- 串口会看到 NuttX 打印 `[AMP] tick N`（走**共享 UART2 0xfeb50000**，`SUPPRESS_UART_CONFIG` 借用 Linux console，non-cacheable 轮询写 THR@+0x00 / poll LSR@+0x14 bit5）。

---

## 5. 关键 git 提交（各仓库 HEAD 附近）
- **work/nuttx (分支 0712)**：`38586ad` amp-shmem 非缓存；`37e4358` mailbox 握手；`a2f86e6` mailbox 门铃；`3eb040b` GICv3 rdist 修复；`48276ec` physical timer；`1da3bfb` rk3588 chip+board。rptun 骨架文件**尚未 commit**。
- **armbain (main)**：`86af956` 握手；`18cb95e` cache 一致；`5438fc9` 里程碑2；等。changelog=`.kiro/board-changes/rk3588-evb7-v11.md`。
- **rk3588-amp-demo (master)**：`c57cb6b` mailbox probe；`a753fba` deploy 包；amp-nuttx.its。

## 6. 提交纪律
- 只提交**真机验证过**的（用户在板上确认后）。
- 各外部树在自己仓库提交；armbain 只放 board conf + changelog。
- 排除构建产物（`log/`、`*.o/.elf/.bin`、`amp.img`、`nuttx.bin`），**不要 `git add .`**，显式加路径。
- 每轮真机验证后更新 `.kiro/board-changes/rk3588-evb7-v11.md`。

## 7. 部署包（rk3588-amp-demo/deploy/，官方 rootfs 用）
`bcmdhd.service`(WiFi) / `brcm-bt.service`(BT) / `wifi-no-randmac.conf` / `selftest.sh`(→rk3588-selftest) / `install.sh`。刷机后 `cp -r deploy /root/ && cd /root/deploy && ./install.sh`。

## 8. 现实约束
- EVB7 的 **UART5 无引脚**（原 AMP demo console），故 NuttX 借用共享 UART2 打印。
- UART5 pin(gpio4-29) 被 rockchip-amp 占用 → SD 卡槽(sdmmc)不可用；eMMC 启动不受影响。
- 板子跑的是**官方 BSP Debian**（不是 Armbian）+ 我们的 AMP 附加。
