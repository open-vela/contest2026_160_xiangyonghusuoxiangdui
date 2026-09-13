# RK3588 EVB7 V11 — AMP (Linux + NuttX) 项目总览

> 本文档汇总当前工作区中 RK3588 EVB7 V11 板卡的 AMP（非对称多处理）适配工作:
> Linux 跑在 cpu0-2 / cpu4-7,NuttX(openvela) 跑在 cpu_l3(MPIDR 0x300),两者通过
> rockchip mailbox + OpenAMP rpmsg 通信。生成时间: 2026-07。

---

## 1. 系统概述

- **SoC**: Rockchip RK3588 (8 核: 4×A55 + 4×A76)
- **板卡**: RK3588 EVB7 LP4 V11(官方评估板)
- **AMP 拓扑**:
  - Linux(vendor kernel 6.1.141) 占 7 个核(cpu0-2, cpu4-7)
  - NuttX(openvela) 独占 **cpu_l3**(cluster0 core3, MPIDR 0x300),作为 remote 核
- **串口**: UART2 @ `0xfeb50000`,1500000 8N1,`console=ttyFIQ0`(Linux/u-boot/NuttX 共享)
- **交付栈**: 官方 BSP(vendor u-boot + rkbin + vendor kernel + 官方 Debian rootfs)+ 自定义
  kernel 片段(amp dts / G610 / wifibt)+ NuttX AMP 固件 + amp 分区。**未走纯 Armbian 构建**。

---

## 2. 工作区结构(各为独立 git 仓库)

| 路径 | 作用 |
|------|------|
| `/media/1t/openvela/armbain` | Armbian 构建框架 fork;含板卡定义 `config/boards/rk3588-evb7-v11.conf` 和改动记录 `.kiro/board-changes/` |
| `/media/1t/openvela/kernel` | 官方 Rockchip BSP kernel 6.1.141;含 `rk3588-amp.dtsi`、`rockchip_amp.c`、mailbox/rpmsg 驱动 |
| `/media/1t/openvela/work/nuttx` | openvela(NuttX) 源码;RK3588 芯片层 + evb7-amp 板层 + rptun/rpmsg |
| `/media/1t/openvela/work/apps` `/prebuilts` | NuttX apps 与工具链(aarch64-none-elf gcc 13.4) |
| `/media/1t/openvela/u-boot` | vendor u-boot(带 CONFIG_AMP,`rockchip_amp.c` 拉核);产出 uboot.img/loader |
| `/media/1t/openvela/rkbin` | rockchip 预编译 blob(DDR / BL31 v1.54 / BL32(OP-TEE) v1.20)+ trust 打包 ini |
| `/media/1t/openvela/rk3588-amp-demo` | AMP 产物与工具: `nuttx.bin`/`amp.img`/`amp-nuttx.its`、分区表、测试 ko |

> 详尽的逐步改动记录见 **`armbain/.kiro/board-changes/rk3588-evb7-v11.md`**(本项目的权威 changelog)。

---

## 3. 已完成的里程碑

### M1. 板卡 bringup + Armbian 定义
- 新建 `config/boards/rk3588-evb7-v11.conf`(BOARDFAMILY=rockchip-rk3588, vendor 内核, mainline u-boot generic-rk3588)。
- Linux 起到 multi-user;GPU(Mali G610 Bifrost)、WiFi/BT(AP6398S)均已修好(见 changelog)。

### M2. AMP 裸机验证
- 加 `amp` 分区(parameter-amp.txt);vendor u-boot `CONFIG_AMP` 通过 `SIP_AMP_CFG` 拉起 cpu_l3。
- 专用保留区 `amp-core@30000000`(16MB no-map)+ `amp-shmem@31000000`(4MB)。
- 裸机固件在 cpu_l3 持续运行 + 共享内存心跳被 Linux `/dev/mem` 读到。

### M3. NuttX 移植到 cpu_l3(替换裸机)
- 芯片层 `arch/arm64/src/rk3588/`:GICD=0xfe600000 / GICR=0xfe680000;RAM 0x30000000/16MB;
  UART2 借用(SUPPRESS_UART_CONFIG,不重配)。
- **GICv3 redistributor 按 MPIDR 亲和度定位**(修 up_cpu_index()=0 的错):cpu_l3 → 0xfe6c0000,
  timer PPI 才使能。
- **EL1 物理定时器**(CNTP/PPI30)替代虚拟定时器(避开 CNTVOFF 依赖)。
- amp-shmem 映射为 **MT_NORMAL_NC**(非缓存),解决 cpu_l3 写、Linux 读的一致性。
- 结果: NuttX 在 cpu_l3 boots + 调度 + 定时器运行,与 Linux 稳定共存。

### M4. rpmsg 双向通信(NuttX cpu_l3 ↔ Linux)
- rptun/OpenAMP over rockchip mailbox0 + 固定 vring(vring0 0x07c00000 / vring1 0x07c08000,
  buffer pool 0x08000000)。
- 资源表 `rk3588_rsctable.c`:pin `DRIVER_OK`、`gfeatures=dfeatures=F_NS|F_CPUNAME|MUST_NOTIFY`
  (因 rockchip Linux 用固定 dts vring、不读我们的表)。
- **TX(NuttX→Linux)**: 事件驱动 —— 写 mailbox B2A_DAT(magic)+B2A_CMD(link_id) 敲门铃。
- **RX(Linux→NuttX)**: **轮询** A2B_STATUS(5ms 内核线程),因中断被 GIC 分组挡住(见下)。
- 抗压稳定的两个关键修复:
  1. **启动清零陈旧 vring**(`rk3588_rptun_init` memset):冷启动 NuttX 早于 Linux 消费 vring,
     残留 avail idx 会导致 buffer held 计数下溢 / RX 工作池耗尽。
  2. **echo 端点 `RPMSG_PRIO_RT`**(且在 `rpmsg_create_ept` 之前设):RX 在轮询线程内联处理,
     不占用异步工作池。
- 已验证: rockchip test 驱动乒乓 rx_count 持续增长、无 assert。

---

## 4. 当前状态(截至本文档)

- **rpmsg RX = 轮询方案**(已提交,稳定)。NuttX 侧同时保留了**完整的中断路径代码**
  (INTID 100 + `up_enable_irq` + IROUTER→cpu_l3 + ISR),当前官方 OP-TEE 下中断被 Group0 挡住,
  自动回落到轮询;将来 secure 侧改分组后 ISR 无需改动即接管。
- NuttX 启动只打印少量 `[AMP]` 状态行(已移除 tick/busy 刷屏心跳)。
- 板卡固件已恢复官方基线(uboot.img/tee.bin/rkbin ini 均为原始),可正常启动。

### 已提交(各自仓库)
- `work/nuttx`: rptun/rpmsg 传输(`433914...`→ 补 sign-off `97e4fd74`)+ 中断路径 `e48555b2`
- `armbain`: changelog 记录(`48ab0f58`、`79d36b6`)

---

## 5. 关键技术结论(踩坑与定论)

### GIC 中断路由(RX 中断化的拦路虎)
- mailbox A2B 给 remote 核的中断 = **GIC INTID 100**(= SPI 68)。
  - amp-irqs 里的数字是 **INTID**(佐证: UART5=368=SPI336+32),NuttX 早期误用 100+32=132(无关线)。
- Linux 的 `gic_dist_init` 会把 amp-irqs 的 INTID 路由到 cpu_l3(IROUTER=0x300),但**分组**留在
  **Group0(secure)**。
- **NS 侧(u-boot NS-EL2 / Linux / NuttX NS-EL1)都改不了 GIC 分组**(实测: Linux 写 IGROUPR
  回读仍 0;BL31 的 `SIP_ACCESS_REG` 白名单拒绝 GICD,返回 -4)。
- Group0 中断作为 FIQ 送 EL3,永远到不了 NS-EL1 的 NuttX。故 RX 改用轮询。
- 三个独立证据坐实"Group0 + NS 改不动": IGROUPR bit=0 读数 / NuttX enable 无效 / Linux 写无效。

### 能改 GIC 分组的地方(唯一出路)
- 只有 **secure 侧**能改分组。链路上的 secure 组件:
  - **BL31(EL3)**: rkbin 预编译 blob,无源码。
  - **BL32/OP-TEE(secure EL1)**: **有源码**,且其 `gic.c` 本就操作 IGROUPR → 可在 secure EL1
    把 INTID 100 设为 Group1NS。**这是保住 AMP(不动 vendor BL31)又打开中断的唯一现实路径**。
- 主线 TF-A: 默认把所有 SPI 设 Group1NS(rockchip secure props 不含 INTID 100),即"迁全套上游"
  可让中断天然可达;但主线 TF-A 无 rockchip 私有 AMP 拉核(`SIP_AMP_CFG`),我们 its `hyp=0`
  起 NS-EL1 依赖它 → 迁上游会丢 AMP 拉核(除非改 NuttX 到 EL2 或在上游 TF-A 补 AMP)。

### OP-TEE 改分组实验(已做,受阻于兼容性)
- 思路/代码就绪: 在主线 OP-TEE `boot_primary_init_intc` 加两行写 IGROUPR/IGRPMODR
  (INTID 100 → Group1NS),`CFG_TZDRAM_START=0x08400000` 编出 tee.bin,经 `RK3588TRUST.ini`
  打进 uboot.img(dumpimage 确认 optee 段 @0x08400000)。
- **失败点**: 主线 OP-TEE 4.x 与 vendor BL31 v1.54 的 BL31↔BL32 交接不兼容,卡在
  `BL31: Initializing BL32`(OP-TEE 连 `I/TC:` 都没打印)。
- **结论**: 要走此路必须用 **vendor OP-TEE 源码(external_optee_optee_os,与 v1.20 同源)**,
  与 vendor BL31 匹配才能启动。

---

## 6. 构建与刷机(known-good)

### NuttX 构建 + 打包 amp.img(在 `work/nuttx`)
```bash
export PATH=/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH
# (改过 Kconfig 才需要) make distclean && ./tools/configure.sh -l evb7-amp:nsh
make -j4
aarch64-none-elf-objcopy -O binary nuttx nuttx.bin
cp nuttx.bin /media/1t/openvela/rk3588-amp-demo/nuttx.bin
/media/1t/openvela/u-boot/tools/mkimage -f /media/1t/openvela/rk3588-amp-demo/amp-nuttx.its \
    -E -p 0xe00 /media/1t/openvela/rk3588-amp-demo/amp.img
```

### 刷 amp 分区(板卡进 MASKROM)
```bash
cd /media/1t/openvela/rk3588-amp-demo
rkdeveloptool db /media/1t/openvela/u-boot/rk3588_spl_loader_v1.21.114.bin
rkdeveloptool wlx amp amp.img
rkdeveloptool rd
```

### Linux 侧 rpmsg 压测
```bash
insmod /tmp/rockchip_rpmsg_test.ko     # 源码: kernel/drivers/rpmsg/rockchip_rpmsg_test.c
dmesg | grep rx_count | tail
```

### 恢复官方 uboot(实验失败时)
```bash
cd /media/1t/openvela/u-boot
rkdeveloptool db rk3588_spl_loader_v1.21.114.bin
rkdeveloptool wlx uboot _amp_backup/uboot.img.orig
rkdeveloptool rd
```

---

## 7. 未来方向(设计已讨论,尚未实现)

### 7.1 RX 中断化
- 用 **vendor OP-TEE 源码**在 secure EL1 把 INTID 100 设 Group1NS → NuttX ISR 自动接管(代码已就绪)。

### 7.2 UART 独占 + 命令切换(方案 B)
- UART 任一时刻单一物理归属,另一方退到 rpmsg 虚拟 console;握手切换(TAKE/RELEASE + ACK)。
- 平时归 Linux,按命令切给 NuttX;主要工作在 Linux 侧让出/恢复 fiq-debugger console。

### 7.3 NuttX 联网(借道 Linux)
- **usrsock over rpmsg**(NuttX 侧现成 `drivers/usrsock/usrsock_rpmsg.c`):socket 调用经 rpmsg
  转发给 Linux 执行。需 Linux 侧 usrsock-rpmsg server。
- 或 rpmsg 虚拟网卡(`drivers/net/rpmsgdrv.c`)+ Linux 网桥/NAT。

### 7.4 NuttX 显示(方案 B: 共享内存 framebuffer + Linux DRM 合成)
- NuttX 画到 AMP 保留区的 shmem framebuffer(双缓冲,NC 映射),Linux 用 DRM 把它作为
  plane / 独立 VP 上屏;rpmsg 传 `FRAME_READY/FRAME_DONE` 帧同步。
- NuttX 侧写 shmem fb 驱动(`fb.c` vtable)+ LVGL/NX;Linux 侧写 DRM 合成程序(主要工作)。
- 分阶段: ①填色到 shmem → ②Linux DRM 静态显示(验证像素+一致性)→ ③rpmsg 帧同步 → ④GUI。

---

## 8. 关键文件索引

| 功能 | 文件 |
|------|------|
| NuttX rptun(mailbox/vring/中断+轮询) | `work/nuttx/arch/arm64/src/rk3588/rk3588_rptun.c` |
| NuttX 资源表(vring/features) | `work/nuttx/arch/arm64/src/rk3588/rk3588_rsctable.c` |
| NuttX 板级 bringup(rpmsg echo server) | `work/nuttx/boards/arm64/rk3588/evb7-amp/src/evb7_amp_bringup.c` |
| NuttX MMU 区域 | `work/nuttx/arch/arm64/src/rk3588/rk3588_boot.c` |
| GICv3 rdist 修复 | `work/nuttx/arch/arm64/src/common/arm64_gicv3.c` |
| kernel AMP dts(amp-irqs/vring/reserved) | `kernel/arch/arm64/boot/dts/rockchip/rk3588-amp.dtsi` |
| kernel AMP 驱动(解析 amp-irqs) | `kernel/drivers/soc/rockchip/rockchip_amp.c` |
| kernel GICv3(amp irq 路由钩子) | `kernel/drivers/irqchip/irq-gic-v3.c` |
| Linux rpmsg 测试驱动 | `kernel/drivers/rpmsg/rockchip_rpmsg_test.c` |
| u-boot AMP 拉核 | `u-boot/drivers/cpu/rockchip_amp.c` |
| u-boot SMC(SIP_AMP_CFG/ACCESS_REG) | `u-boot/arch/arm/mach-rockchip/rockchip_smccc.c` |
| trust 打包配方(BL31/BL32) | `rkbin/RKTRUST/RK3588TRUST.ini` |
| 权威改动记录 | `armbain/.kiro/board-changes/rk3588-evb7-v11.md` |
