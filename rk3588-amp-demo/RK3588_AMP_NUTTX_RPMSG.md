# RK3588 EVB7 V11 — AMP + NuttX(openvela) + rpmsg 移植与调试文档

> 目标：在 RK3588 EVB7 V11 上，让 Linux（官方 BSP）与运行在 **cpu_l3** 上的
> NuttX(openvela) 通过 AMP（非对称多处理）共存，并建立 **rpmsg** 双向通信。

---

## 1. 总体架构

- **主核**：cpu0-2 / cpu4-7 跑 Linux（官方 Debian + BSP 内核 6.1.141）。
- **远端核**：cpu_l3（MPIDR `0x300`，第 4 个 A55 小核）跑 NuttX，作为 AMP remote firmware。
- **启动流程**：u-boot(vendor 2017.09, 带 `CONFIG_AMP`) 通过 FIT(`amp.img`) 把
  NuttX 加载到 `0x30000000` 并在 cpu_l3 上拉起；随后 Linux 在其余核启动。
- **通信**：OpenAMP/rptun + virtio rpmsg，走共享内存 vring + RK3588 mailbox0 门铃。

### 源码树（均在工作区外，用绝对路径访问）
| 路径 | 说明 |
|------|------|
| `/media/1t/openvela/kernel` | 官方 Rockchip BSP 内核 6.1.141 |
| `/media/1t/openvela/u-boot` | vendor u-boot 2017.09（linaro 6.3.1 工具链） |
| `/media/1t/openvela/rkbin` | DDR/BL31/BL32 blobs |
| `/media/1t/openvela/rk3588-amp-demo` | AMP demo（打包 amp.img、放模块/文档） |
| `/media/1t/openvela/work` | openvela(NuttX) 源码树（`nuttx`/`apps`/`prebuilts`） |
| `/media/1t/openvela/armbain` | Armbian fork（板级 conf + changelog） |

---

## 2. 构建与刷机

### NuttX 编译（在 `/media/1t/openvela/work/nuttx`）
```bash
export PATH=/media/1t/openvela/work/prebuilts/gcc/linux-x86_64/aarch64-none-elf/bin:$PATH
./tools/configure.sh -l evb7-amp:nsh      # 一次性配置
make -j4                                   # 产出 nuttx (ELF)
aarch64-none-elf-objcopy -O binary nuttx nuttx.bin
cp nuttx.bin /media/1t/openvela/rk3588-amp-demo/nuttx.bin
/media/1t/openvela/u-boot/tools/mkimage -f /media/1t/openvela/rk3588-amp-demo/amp-nuttx.its \
    -E -p 0xe00 /media/1t/openvela/rk3588-amp-demo/amp.img
```
- `amp-nuttx.its`：`data=./nuttx.bin`，`cpu=0x300`，`load=0x30000000`。

### 刷 amp 分区（板子进 MASKROM）
```bash
cd /media/1t/openvela/rk3588-amp-demo
rkdeveloptool db /media/1t/openvela/u-boot/rk3588_spl_loader_v1.21.114.bin
rkdeveloptool wlx amp amp.img
rkdeveloptool rd
```

### 串口
- UART2 / ttyFIQ0 控制台，波特率 **1500000**。NuttX 与 Linux 日志在此交织。
- 日志偏二进制，分析用 `strings <log> | grep -a ...`。

---

## 3. NuttX AMP 移植（里程碑 1-2，已验证）

从 rk3399/pinephonepro 模板移植，跑在 cpu_l3 作为 UP(单核) remote。

### 芯片层 `arch/arm64/src/rk3588/` + `arch/arm64/include/rk3588/`
- GICD=`0xfe600000`，GICR=`0xfe680000`。
- RAM `0x30000000`/16MB，UART2=`0xfeb50000`（IRQ 365）。
- `get_cpu_id` 保留 Aff0（cpu_l3→0→UP 主核）。
- `NR_IRQS=512`（RK3588 有 480 SPI）。
- MMU 区域：
  - DEVICE `0xf8000000`/128MB
  - DRAM `0x30000000`/16MB
  - AMP_SHMEM `0x31000000`/4MB（`MT_NORMAL_NC`）
  - AMP_RPMSG `0x07c00000`/6MB（`MT_NORMAL_NC`，覆盖 vring+buffer）

### 板级 `boards/arm64/rk3588/evb7-amp/`
- `dramboot.ld` 链接到 `0x30000000`；defconfig（RK3588, UP, SUPPRESS_UART_CONFIG, NSH）。

### 关键修复
- **GICv3 redistributor**（`arm64_gicv3.c`）：`arm64_gic_init()` 原本按
  `up_cpu_index()*stride` 选 rdist（选到 cpu0），改为**按 GICR_TYPER 的 affinity
  匹配 MPIDR** 定位本核 rdist → cpu_l3 的定时器 PPI 才能使能。
- **定时器**：切到 EL1 physical timer（CNTP，INTID 30）。
- **共享内存一致性**：AMP_SHMEM/AMP_RPMSG 用 `MT_NORMAL_NC`（非缓存），否则
  cpu_l3 的写卡在 cache 里，Linux 通过 `/dev/mem` 读到的值"冻结"。

验证：NuttX 启动、经共享 UART2 打印、调度器+定时器稳定运行，与 Linux 共存。

---

## 4. rpmsg 集成（里程碑 3，双向已验证 + 遗留问题）

### 4.1 传输层基础（已验证）
- 共享内存一致（NC 映射）。
- **mailbox 门铃**：cpu_l3 写 `B2A_DAT(0)` + `B2A_CMD(0)` → Linux rpmsg RX 回调触发。
  **必须同时写 DAT 和 CMD**（RK mailbox 靠 DAT 写触发；只写 CMD 无效）。
- **握手**：`B2A_DAT=RPMSG_MBOX_MAGIC(0x524D5347)` + `B2A_CMD=link_id(0x03)`。

### 4.2 mailbox 通道映射（rk3588-amp.dtsi: `mboxes = <&mailbox0 0 &mailbox0 3>`）
| 方向 | 通道 | 说明 |
|------|------|------|
| TX：cpu_l3 → Linux | **B2A 通道 0** | Linux 的 "rpmsg-rx" |
| RX：Linux → cpu_l3 | **A2B 通道 3** | Linux 的 "rpmsg-tx" |
- mailbox0 中断：SPI 61-64（=Linux irq 93-96）= B2A 通道 0-3（Linux 收）。
- A2B（远端收）中断：SPI 100（amp-irqs 路由到 cpu3）。

### 4.3 rptun / 资源表（`arch/arm64/src/rk3588/`）
- `rk3588_rptun.c`：rptun_ops（`is_master=false`, `is_autostart=true`），
  notify 写 B2A 门铃，RX 轮询线程；`rk3588_rptun_init`。
- `rk3588_rsctable.c`：资源表，NUM_VRINGS=2、BUF_COUNT=64、VRING_ALIGN=0x1000、
  vring0=`0x07c00000`、vring1=`0x07c08000`、carveout=`0x08000000`/2MB。
- `rk3588_addrenv.c`：`up_addrenv_pa_to_va/_va_to_pa` 恒等实现（VA==PA 平坦映射），
  OpenAMP/libmetal 链接必需。

### 4.4 与 mainline rockchip rpmsg 互通的关键适配（都在资源表里）
rockchip Linux 不读/写远端资源表（用固定 dts vring 地址），所以凡是标准 virtio
DRIVER 该写的字段，远端都要自己**预置**：

1. **RO 段崩溃修复**：`get_resource()` 必须返回**可写**拷贝（`0x07c0f000`，在
   AMP_RPMSG NC RW 区），不能返回 `.rodata` 里的 `const` 模板——OpenAMP 会写回
   notifyid，写只读段直接 data abort。
2. **status = `DRIVER_OK(0x04)`**：否则 create_device 一直 `-EAGAIN`。
3. **gfeatures = dfeatures = `F_NS | F_CPUNAME`(0x9)**：DEVICE 角色 OpenAMP 不做
   自动协商，gfeatures 停在 0 会导致 `features=0`、`DEBUGASSERT(F_CPUNAME)` 失败。
   - 只开 `F_NS`(线格式与 mainline 一致) + `F_CPUNAME`(仅本地读 cpuname，不上线)。
   - **不开** F_ACK/F_BUFSZ/F_BUFADDR/F_PRIORITY（会破坏与 mainline 互通）。
4. **`VIRTIO_RING_F_MUST_NOTIFY`(bit30)**：OpenAMP 默认按对端 vring 的
   `NO_INTERRUPT` 标志抑制门铃；rockchip 需要**无条件 kick**，置此位强制每次都敲。

### 4.5 启动竞争 + 单次门铃不可靠（已解决）
- NuttX 启动极快，若在 Linux 探测 mailbox（~3.4s）之前通告，门铃丢失。
  → **延迟 6s 再通告**（`echo_announce_thread`）。
- 单次 B2A 写有时不置状态位（首次 kick 常失败）。
  → **每秒重发 NS 通告**直到绑定（复刻手动心跳的重复 kick）。

### 4.6 Linux 侧测试驱动
内核未编 `RPMSG_CHAR/CTRL/ROCKCHIP_TEST`，故编成模块测试：
```bash
# 在 /media/1t/openvela/kernel，.config 设 CONFIG_RPMSG_ROCKCHIP_TEST=m
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j4 modules_prepare
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j4 drivers/rpmsg/rockchip_rpmsg_test.ko
# 产物: drivers/rpmsg/rockchip_rpmsg_test.ko (vermagic 6.1.141 SMP aarch64)
# 已放到 rk3588-amp-demo/rockchip_rpmsg_test.ko
```
- 该驱动匹配 channel 名 **`rpmsg-ap3-ch0`**（故 NuttX 通告此名），probe 时发
  `LINUX_TEST_MSG` 做 ping-pong，收到回显打印 `rx msg ... rx_count N`。
- 板子上：`insmod /tmp/rockchip_rpmsg_test.ko`。

### 4.7 验证结果
- **NuttX→Linux 单向**：Linux `creating channel rpmsg-ap3-ch0 addr 0x400`，
  `/sys/bus/rpmsg/devices/virtio0.rpmsg-ap3-ch0.-1.1024` 出现。✅
- **双向 pingpong**：曾稳定往返 **5697 次**（`rx_count 5697`）。✅

---

## 5. 遗留问题

### 5.1 反向中断路由（当前用轮询绕过）
Linux devmem 读 mailbox0 GICD 状态：
- `ISENABLER` bit(SPI100)=1（已使能）
- `IROUTER(100)=0x500`（路由到 **cpu5**，非 cpu3；写 0x300 会被改回）
- `IGROUPR` bit=0（**Group0/安全组**，NS-EL1 的 NuttX 收不到）
- `A2B_STATUS` bit3=1 卡死（Linux 写了、NuttX 没消费）

结论：rockchip AMP 的远端中断投递依赖 **BL31(EL3)** 配置 GIC 分组/安全态并路由，
NuttX(NS-EL1) 无权改。**当前用 A2B_STATUS 轮询（5ms）替代中断**——cpu_l3 是
NuttX 独占核，轮询开销可忽略。若要用中断需改 BL31/ATF。

### 5.2 极限压测下 RX 工作池耗尽（调试中）
rockchip 测试驱动是**紧密回环压测**（收到即回发）。NuttX 侧 rpmsg RX 默认走
**异步工作队列**（`freerx` 池 = vring 描述符数 = 64）。tight flood 下工作项来不及
回收 → `DEBUGASSERT(work != NULL)`（`drivers/rpmsg/rpmsg.c:1052`）。
- 已试：`rpmsg_trysend`（非阻塞 echo）、`RPMSG_PRIO_RT`（想走同步内联，实测未生效，
  原因待上板加诊断确认）、nrx×4（256，仍未扛住 → 持续性耗尽）。
- 当前正加诊断（打印 ept addr/priority、echo_ept_cb 调用情况）定位 RT 未内联的原因。
- 备选：让 echo 不回环（打破风暴）以做稳定演示；正解是让 RT 内联生效（不耗池）。

> 注：**双向 rpmsg 本身已验证可用**（5697 次往返）；上述崩溃仅出现在
> rockchip 官方压测驱动的病态满速回环场景，是流控边界问题，非 rpmsg 功能缺陷。

---

## 6. 关键文件清单

### NuttX（`/media/1t/openvela/work/nuttx`）
- `arch/arm64/src/rk3588/rk3588_rptun.c` / `.h` — rptun 驱动 + RX 轮询
- `arch/arm64/src/rk3588/rk3588_rsctable.c` / `.h` — 资源表（含所有互通适配）
- `arch/arm64/src/rk3588/rk3588_addrenv.c` — 恒等地址转换
- `arch/arm64/src/rk3588/rk3588_boot.c` — MMU 区域（含 AMP_RPMSG）
- `arch/arm64/src/rk3588/Make.defs` — RPTUN 下编译上述文件
- `arch/arm64/src/common/arm64_gicv3.c` — redistributor 按 MPIDR 定位
- `arch/arm64/src/common/arm64_arch_timer.c` — RK3588 用 EL1 物理定时器
- `boards/arm64/rk3588/evb7-amp/src/evb7_amp_bringup.c` — echo 服务 + 延迟通告
- `boards/arm64/rk3588/evb7-amp/configs/nsh/defconfig` — RPTUN/RPMSG/OPENAMP 等
- `drivers/rpmsg/rpmsg_virtio.c` — （调试中）RX 工作池 nrx 加大

### Demo（`/media/1t/openvela/rk3588-amp-demo`）
- `amp-nuttx.its` / `amp.img` / `nuttx.bin`
- `rockchip_rpmsg_test.ko` — Linux 侧测试模块
- 本文档

### Armbian fork（`/media/1t/openvela/armbain`）
- `config/boards/rk3588-evb7-v11.conf` — 板级配置
- `.kiro/board-changes/rk3588-evb7-v11.md` — 逐条变更 changelog（最详细的历史）

---

## 7. Linux 侧常用诊断命令
```bash
# rpmsg / channel
dmesg | grep -iE "rpmsg|virtio|mailbox|creating channel"
ls -l /sys/bus/rpmsg/devices/ /sys/bus/virtio/devices/

# mailbox 中断计数（irq 93 = B2A ch0 = 收 NuttX 的门铃）
cat /proc/interrupts | grep -i mailbox

# mailbox 寄存器（V1，base 0xfec60000）
busybox devmem 0xfec60028 32   # B2A_INTEN
busybox devmem 0xfec6002c 32   # B2A_STATUS
busybox devmem 0xfec60004 32   # A2B_STATUS (bit3 = Linux→cpu3)

# GICD（base 0xfe600000）SPI100=INTID132
busybox devmem 0xfe600110 32   # ISENABLER (bit4)
busybox devmem 0xfe606420 32   # IROUTER(132)
busybox devmem 0xfe600090 32   # IGROUPR (bit4)
```
