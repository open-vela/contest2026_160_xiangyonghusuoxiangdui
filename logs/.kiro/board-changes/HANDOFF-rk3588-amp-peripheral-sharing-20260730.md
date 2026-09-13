# Handoff: RK3588 AMP 外设共享机制调研（VOP / NPU / 触摸 / 摄像头）

本文自包含。阅读本文即可继续工作，无需其他上下文。

本次会话是**纯代码考古与方案评估**，没有修改任何文件。产出是下面这些结论和
`path:line` 引用。同期由用户自己完成的代码改动见第 5 节。

---

## 1. Outcome

- 回答了核心问题：RK3588 上「把某个外设分给 AMP 核（NuttX）用」有哪些**内核已有机制**，
  哪些能用、哪些是死路。逐个驱动读源码验证，不是推测。
- **VOP（显示）：可行，机制是 `rockchip,reserved-plane`。** 这是唯一一个 Rockchip 明确
  为「第三方 OS」设计、且有 RK3588 专属代码的图层级共享机制。有两个缺口需自己补。
- **VOP `rockchip,shared-mode`：RK3588 上不可用。** 它是为 RK3576（VOP3）设计的，
  RK3588 缺少 per-VP 中断，且 probe 会因找不到 `vop-virtual-irq` 直接失败。
- **NPU：无任何 AMP 机制，且本质上做不到。** 驱动零划分属性，硬件三核共享 IOMMU /
  SCMI 时钟 / NPUTOP 电源域；更致命的是指令格式在闭源 `librknnrt.so` 里。
- **触摸：无硬件级共享机制，但有 GPIO EXT 中断分组这个硬件能力（未验证）。**
  实际已采用 rpmsg 转发方案并跑通（见第 5 节）。
- **摄像头：有 Thunder Boot ISP，但是顺序交接不是并发共享，且 NuttX 侧实现 ISP 不现实。**
- 建立了一个反复出现的判断模式：**Rockchip 这类 AMP 机制普遍「代码在、dts 无、
  未验证」**。全树搜不到任何 dts 使用 `rockchip,shared-mode`、`rockchip,reserved-plane`、
  `interrupt-pins`、`rockchip,thunder-boot-rkisp`、`rockchip,amp-shared`。

---

## 2. Source tree layout

### Workspace roots

| 路径 | 是什么 | 本次角色 |
|---|---|---|
| `/media/1t/openvela/kernel` | vendor 内核 6.1.115（armbian/linux-rockchip rk-6.1-rkr5.1） | 主要调研对象，所有 `path:line` 都相对此根 |
| `/media/1t/openvela/work/nuttx` | openvela NuttX | rptun / uinput / gt9xx 调研；用户的板级代码在此 |
| `/media/1t/openvela/work/apps` | openvela apps | quickapp / LVGL 调研 |
| `/media/1t/openvela/armbain` | Armbian 构建框架 | 板级配置 + 本 changelog 目录 |
| `/media/1t/openvela/u-boot` | vendor u-boot | AMP 引导（`drivers/cpu/rockchip_amp.c`） |
| `/media/1t/openvela/rk3588-amp-demo` | 自建 AMP 载荷/打包 | FIT 打包、M0 载荷 |

### 本次只读、未修改的关键文件

内核（相对 `/media/1t/openvela/kernel`）：

| 文件 | 说明 |
|---|---|
| `drivers/gpu/drm/rockchip/rockchip_drm_vop2.c` | VOP2 主驱动，约 17000 行。reserved-plane / shared-mode / splice 全在这里 |
| `drivers/gpu/drm/rockchip/rockchip_vop2_reg.c` | VOP2 寄存器表 + 各 SoC 的 win/vp 能力数据 |
| `include/dt-bindings/display/rockchip_vop.h` | `ROCKCHIP_VOP2_*` phys_id 编号、SHARED_MODE 常量、FBD 常量 |
| `arch/arm64/boot/dts/rockchip/rk3588s.dtsi` | vop 节点 :5467、gpio 节点 :8059-8129、rknpu :4156 |
| `arch/arm64/boot/dts/rockchip/rk3576.dtsi` | vop 节点 :3472（对比用，有 per-VP 中断） |
| `arch/arm64/boot/dts/rockchip/rk3588-amp.dtsi` | AMP 通用机制：clocks + amp-irqs + reserved-memory |
| `arch/arm64/boot/dts/rockchip/rk3568-amp.dtsi` | GPIO group AMP 配置的**唯一参考实现** |
| `arch/arm64/boot/dts/rockchip/rk3588-evb7-v11.dtsi` | 触摸 :553-582、Type-C PD :587-596、pinctrl :790-801 |
| `arch/arm64/boot/dts/rockchip/rk3588-evb7-v11-imx415.dtsi` | 摄像头链路 imx415 → csi2_dphy0 → mipi2_csi2 → rkcif → rkisp0 |
| `drivers/soc/rockchip/rockchip_amp.c` | AMP 内核侧：GIC 中断划分 + GPIO group 解析 |
| `drivers/rknpu/*` | NPU 驱动，无 AMP 支持 |
| `drivers/gpio/gpio-rockchip.c` | GPIO per-pin EXT 中断分组（`interrupt-pins`） |
| `drivers/pinctrl/pinctrl-rockchip.h` | `RK_GPIO_IRQ_MAX_NUM` :187、`RK_GPIO_EXP_IRQ_MAX_PIN_NUM` :188 |
| `drivers/i2c/busses/i2c-rk3x.c` | `rockchip,amp-shared` :2008（名不副实，见第 7 节） |
| `drivers/media/platform/rockchip/isp/*` | ISP 驱动 + `rkisp_tb_helper.c` Thunder Boot |
| `include/uapi/linux/rk-isp2-config.h` | Thunder Boot 交接结构体 :2205-2248 |
| `drivers/media/i2c/imx415.c` | 本板 sensor，**有** thunderboot 支持 |
| `drivers/irqchip/irq-gic-v3.c` | :953 消费 `rockchip_amp_need_init_amp_irq()` |

NuttX（相对 `/media/1t/openvela/work/nuttx`）：

| 文件 | 说明 |
|---|---|
| `include/nuttx/rptun/rptun.h` | rptun 接口定义，`struct rptun_ops_s` |
| `drivers/rptun/rptun.c` | rptun 核心，1421 行 |
| `arch/arm64/src/rk3588/rk3588_rptun.c` | cpu_l3 ↔ Linux 链路（用户已实现，注释极详尽） |
| `arch/arm/src/rk3588-m0/rk3588m0_rptun.c` | PMU M0 ↔ Linux 链路 |
| `arch/arm/src/rk3588-m0/rk3588m0_rsctable.c` | resource table，含物理/虚拟地址约定说明 |
| `drivers/input/uinput.c` | `UINPUT_TOUCH` + `UINPUT_RPMSG` 跨核输入事件总线 |
| `drivers/input/gt9xx.c` | Goodix GT9xx 触摸驱动（若走硬件接管路线可直接用） |
| `apps/frameworks/runtimes/quickapp/` | openvela 快应用运行时（Web-like UI，非浏览器） |

---

## 3. Key technical facts

### 3.1 VOP2 基础（全部 VERIFIED）

VOP2 = Video Output Processor v2，第二代显示控制器，DRM 里的 CRTC+plane 硬件实体。
RK3568 起用 VOP2，RK3399 那代是 VOP1（`rockchip_drm_vop.c`，两套驱动并存）。

RK3588 有 4 个 Video Port，数据在 `rockchip_vop2_reg.c:2285` `rk3588_vop_video_ports[]`：

| VP | 最大输出 | dclk 上限 | 特性 |
|---|---|---|---|
| VP0 | 7680x4320 | 2400 MHz | 10bit / HDR10 / Dolby Vision / alpha 缩放，`splice_vp_id = 1` |
| VP1 | 4096x2304 | 600 MHz | 10bit / alpha 缩放 |
| VP2 | 4096x2304 | 600 MHz | 同上，cubic_lut 更大（4913 = 17³） |
| VP3 | 2048x1536 | 200 MHz | 仅 alpha 缩放 |

8 个图层，`rockchip_vop2_reg.c:4371` `rk3588_vop_win_data[]`。phys_id 编号在
`include/dt-bindings/display/rockchip_vop.h:12-24`（注意 SMART0/1 占 4/5，RK3588 上不存在，
所以 mask 有空洞）：

| 图层 | phys_id | reg_done_bit | splice 右窗 |
|---|---|---|---|
| Cluster0 | 0 | 0 | Cluster1 |
| Cluster1 | 1 | 1 | — |
| Cluster2 | 6 | 2 | Cluster3 |
| Cluster3 | 7 | 3 | — |
| Esmart0 | 2 | 4 | Esmart1 |
| Esmart1 | 3 | 5 | — |
| Esmart2 | 8 | 6 | Esmart3 |
| Esmart3 | 9 | 7 | — |

EVB7 上 VP3 接 DSI0/DSI1（`rk3588-evb7-v11.dtsi:872,877`），HDMI 走 route_hdmi0/1（:880,884）。

### 3.2 多 VP 只能拼接，不能叠加（VERIFIED）

- 唯一的多 VP 协作是 **splice mode**：单 VP 输出宽度上限 4096
  （`VOP2_MAX_VP_OUTPUT_WIDTH`，`rockchip_drm_vop2.c:169`），超过就把帧左右劈开，
  VP0 画左半 VP1 画右半。
- 全自动无开关：`vop2_crtc_atomic_enable()` 里 `adjusted_mode->hdisplay > 4096` 即置位
  （:10593-10601），`splice_en = 1` 写在 :10877。
- 配对硬连死：只有 VP0 的数据里有 `splice_vp_id = 1`，所以只能 VP0+VP1。
- 代价：`vop2_crtc_mode_valid()` 里 VP1 所有 mode 返回 `MODE_BAD`（:8831-8834），
  VP1 彻底不能独立输出。反之 VP1 已占用时 >4096 的 mode 被拒（:8836-8840）。
- 额外限制 `vop2_plane_splice_check()` :6213 — 不能用 Cluster two-win 模式、
  不能 90/270 旋转、不能 X 镜像；Cluster 缩小必须居中且系数 < 1.2（:6178）。
- **不存在跨 VP 的 blending 通路。** 混合发生在 VP 内部，8 层通过 port_mux 分配归属。
  想叠加只能 GPU/RGA 先合成再当单层喂给一个 VP。
- 易混淆项：一个 VP 出多屏是 `vcstate->output_if` bitmask（clone/mirror，一对多分发）；
  `ROCKCHIP_OUTPUT_DUAL_CHANNEL_LEFT_RIGHT_MODE` 是一个 VP 从两接口各出一半（接口侧拆分）。
  两者都不是多 VP 合成。

### 3.3 `rockchip,shared-mode`：RK3588 上不可用（VERIFIED，重要结论）

机制本身：同一 VOP IP 被两个 OS 各占一部分 VP 和图层。解析在
`vop2_parse_shared_mode_resources()` `rockchip_drm_vop2.c:16761`。四个属性：

```dts
rockchip,shared-mode = <1>;              /* 1=PRIMARY 2=SECONDARY 0=off */
rockchip,shared-mode-axi-id = <0>;
rockchip,shared-mode-vp-mask = <0x3>;
rockchip,shared-mode-plane-mask = <0x0f>;
```

生效后：`vop2_skip_shared_vp()` :4456 让不在 vp_mask 的 VP 不注册 CRTC；
`vop2_skip_shared_plane()` :4461 让不在 plane_mask 的 win 不创建 DRM plane；
`is_used_axi()` :1266 只处理自己 axi_id 的中断；`vop2_power_off_all_pd()` :5069 直接
return（**子电源域交给 SCP 常开**，硬性依赖）；plane_mask 校验基准换成
`shared_mode_res.plane_mask` :16367。

**为什么 RK3588 用不了，三条独立证据：**

1. **中断无法拆分。** RK3576 的 vop 有 4 个中断
   （`rk3576.dtsi:3482-3489`：`vop-sys`/`vop-vp0`/`vop-vp1`/`vop-vp2`），
   RK3588 只有一根 `GIC_SPI 156`（`rk3588s.dtsi:5471`）。且 `merge_irq` 在非 VOP3 上被
   强制 true（`rockchip_drm_vop2.c:16843-16845`），per-VP irq 分支根本不走。
2. **probe 直接失败。** :17099 处 `shared_mode == PRIMARY` 时无条件查找
   `vop-virtual-irq`，找不到就 return 报错。RK3588 dtsi 里没有这个中断名。而该
   virtual_irq 唯一使用点在 `vop3_sys_isr()` :14895（RK3588 走 `vop2_isr`，用不到）。
   设 SECONDARY 能绕过检查但没有中断转发。
3. **辅助逻辑 RK3576 专用。** `rk3576_shared_mode_esmart_scale_engine()` :15159。

### 3.4 `rockchip,reserved-plane`：RK3588 可行，推荐（VERIFIED）

驱动注释直说用途（`rockchip_drm_vop2.c:841-844`）：

> reserved plane is used by third party OS, reserved plane is always on the top of overlay

DT 形状（属性挂在 VP 的 port 节点，解析在 `vop2_bind()` 的 ports 遍历 :16987）：

```dts
&vp3 {
    rockchip,plane-mask = <((1 << ROCKCHIP_VOP2_ESMART2) | (1 << ROCKCHIP_VOP2_ESMART3))>;
    rockchip,primary-plane = <ROCKCHIP_VOP2_ESMART2>;
    rockchip,reserved-plane = <ROCKCHIP_VOP2_ESMART3>;   /* 给 NuttX */
};
```

**已实现的部分：**

- **cfg_done 隔离 —— RK3588 专属代码，整个机制的核心。**
  `rk3588_vop2_cfg_done()` `rockchip_drm_vop2.c:1947-1966`：

  ```c
  if (vp->reserved_plane_phy_id != ROCKCHIP_VOP2_PHY_ID_INVALID) {
      val = vp->win_cfg_done_bits;
      VOP_CTRL_SET(vop2, win_cfg_done, val);
      VOP_CTRL_SET(vop2, wb_cfg_done, 1);
      VOP_MODULE_SET(vop2, vp, sys_cfg_done, 1);
  } else {
      vop2_writel(vop2, 0, val);   /* 全局 REG_CFG_DONE */
  }
  ```

  平时写全局 `REG_CFG_DONE` 会把对面核写了一半的 win 寄存器一起生效 → 花屏。
  有 reserved plane 时改为按位提交。`win_cfg_done` 映射到
  `RK3588_SYS_WIN_REG_CFG_DONE`，在 `rk3588_vop_ctrl` 里存在（`rockchip_vop2_reg.c:5023`，
  结构体起始 :5020）。`win_cfg_done_bits` 在 win 挂到 VP 时累加（:2308、:7339）。
- **运行时可交接。** `RESERVED_PLANE_MASK` 是 CRTC 上的 DRM bitmask 属性
  （创建 :15430，挂载 :15916）。写 0 收回，写非 0 则 `ilog2(val)` 交出（get :14276，
  set :14417-14421）。

**RK3588 上的两个缺口（需自己补）：**

1. **「置顶」只在 VOP3 路径实现。** 逻辑在 `vop3_setup_layer_sel_for_vp()` :12577-12582
   （`nr_layers -= 1` 然后把 reserved win 放最高层）。RK3588 走另一分支（:13256-13258）：
   `vop2_setup_port_mux()` + `vop2_setup_layer_mixer_for_vp()`。已完整读过
   `vop2_setup_layer_mixer_for_vp()`（:12459-12518），**无任何 `reserved_plane_phy_id` 引用**。
   所以 RK3588 上该 plane 的 zpos 不会自动置顶，靠 `vop2_layer_map_initial()` 静态分配
   （:4591）或自己补代码。
2. **IOMMU bypass 是 RK3576 专属。** :4861-4867 的注释说明 reserved plane 模式需要
   IOMMU bypass（对面核用物理地址），RK3576 靠 `rkmmu_v2_en = 0` 退回 rkiommu 1.0。
   整段在 `if (vop2->version == VOP_VERSION_RK3576)` 内。RK3588 无对等机制，
   `enable_reserved_plane` 全局标志在 RK3588 上是死变量。
   解法：Linux 侧 vop 节点去掉 `iommus`，或保证那块内存在 VOP 页表里恒等映射。

**最容易踩死的坑（VERIFIED）：** `vop2_plane_mask_check()` 失败不是致命错误，
而是**静默丢弃你的配置**（:17041-17043）：

```c
if (!vop2_plane_mask_check(vop2)) {
    DRM_WARN("use default plane mask\n");
    vop2_plane_mask_assign(vop2, vop_out_node);
}
```

而检查里有一条（:16374）：所有 VP 的 plane-mask 并集必须精确等于
`vop2_data->plane_mask_base`。**所以 reserved plane 必须留在 `rockchip,plane-mask` 里**，
否则驱动打个不起眼的 WARN 就用默认分配覆盖，那个 plane 又回到 Linux 手里。

**隔离强度预期：** 不做硬隔离。已搜遍所有 `reserved_plane` 引用点，
`vop2_plane_atomic_check()` 里无拦截 —— Linux 照样为该 win 创建 DRM plane，
userspace 硬要用也用得上。`RESERVED_PLANE_MASK` 的作用是让 compositor 自己避开。

**注意参数类型陷阱：** `rockchip,reserved-plane` 收的是**单个 phys_id 不是 bitmask**
（`of_property_read_u32` → `vp->reserved_plane_phy_id`）。和同名 DRM 属性相反。
写成 `<(1 << ROCKCHIP_VOP2_ESMART3)>` 会被当成 phys_id 8（= Esmart2），静默错位。

**AMP 下 VOP 划分的额外注意点（VERIFIED）：** 别碰 VP0/VP1 这对（splice 绑定，
且 VP0 是唯一有 HDR10/DoVi/8K 的）。VP2 或 VP3 更适合切出去。`vop_mmu`、
`RK3588_PD_VOP`、`sys_grf`/`vop_grf` 是整个 IP 共享的。`need_ovl_lock` 在 RK3588 上
为 true（:16827-16829），说明 overlay 寄存器本身需要串行化，而该锁只在 Linux 内部有效。

### 3.5 `ROCKCHIP_DRM_FBD_FROM_RTOS`：只有宏，无实现（VERIFIED）

`include/dt-bindings/display/rockchip_vop.h:45-47` 定义了三个 Fast Boot Display 常量
（FROM_UBOOT / FROM_UBOOT_TO_RTOS / FROM_RTOS），注释描述「crtc/connector/panel 在
RTOS 里初始化，Linux 只接管逻辑状态」—— 正是理想机制。但全树搜 `ROCKCHIP_DRM_FBD`
只有 header 自身命中，**没有任何 .c/.dts 消费它**。预留接口，用不了。

### 3.6 NPU：无机制且本质做不到（VERIFIED）

- `drivers/rknpu/` 全目录搜 shared/reserved/amp/rtos/third，命中只有三类无关项：
  `of_reserved_mem`（CMA 池）、`IRQF_SHARED`（Linux 驱动间普通共享中断）、
  ioctl 结构体里叫 `reserved` 的对齐填充。
- 驱动读的 DT 属性只有 `rockchip,supported-hw` 和 `dynamic-power-coefficient`
  （`rknpu_devfreq.c:186,208,416,773`），全是 OPP/devfreq 用的。**零资源划分属性。**
- `core_mask = 0x7`（`rknpu_drv.c:197`）是写死的 SoC 能力常量，不可配。
- `rknpu_ioctl.h:161,277` 的 `core_mask` 是**单任务选核**（RKNN API 把推理钉到 core0/1/2），
  同一 OS 内的调度，不是跨 OS 划分。易误读。
- `bypass_irq_handler` / `bypass_soft_reset`（`rknpu_drv.c:65-72`）是调试 module param。
- **硬件耦合是根因。** `rk3588s.dtsi:4156-4186`：3 核共享一个 IOMMU
  （`iommus = <&rknpu_mmu>`）、一套 PC/task queue、`SCMI_CLK_NPU`（**主时钟由 SCP 固件仲裁**）、
  `RK3588_PD_NPUTOP`（三核都依赖）、一套 devfreq/OPP 带调压。任一方调频调压三核全受影响。
- **更硬的墙：NPU 无公开寄存器规格。** RKNN/RKLLM 实际工作在闭源 `librknnrt.so` 里
  （生成寄存器序列和命令列表），内核驱动基本只是提交队列 + 内存管理。
  Rockchip 无 RTOS 版 NPU 驱动。
- SIP 侧也没有：`RK_AMP_SUB_FUNC_*`（`include/linux/rockchip/rockchip_sip.h:175-182`）
  只有 CPU 上下电和模式配置，无设备仲裁。

### 3.7 通用 AMP 设备移交机制（VERIFIED）

`rockchip_amp` 节点，三部分组成：

- u-boot 侧 `/media/1t/openvela/u-boot/drivers/cpu/rockchip_amp.c`
- 内核侧 `/media/1t/openvela/kernel/drivers/soc/rockchip/rockchip_amp.c`
  （compatible `"rockchip,amp"` :757）
- dts `arch/arm64/boot/dts/rockchip/rk3588-amp.dtsi`

三个要素：

1. `clocks = <...>` —— u-boot 使能，Linux 不再管
2. `amp-irqs = /bits/ 64 <GIC_AMP_IRQ_CFG_ROUTE(irq, prio, aff)>` —— GIC SPI 路由到 AMP 核。
   宏定义在 `include/dt-bindings/soc/rockchip-amp.h`（就是把三个数并排展开）。
   内核侧导出 `rockchip_amp_check_amp_irq()` / `_get_irq_prio()` / `_get_irq_cpumask()` /
   `_need_init_amp_irq()` / `_get_irq_aff()`（:286-324），由 GIC 驱动消费：
   `irq-gic-v3.c:953`、`irq-gic.c:531`、`irq-gic-common.c:118`
3. reserved-memory 节点划内存

**重要：`amp-irqs` 里的数字是 GIC INTID，不是 SPI 号。** INTID = SPI + 32。
已验证：rk3588-amp.dtsi 的 UART5 项是 368 = SPI 336 + 32。

本板 `rk3588-amp.dtsi` 现有 amp-irqs：GPIO EXT 314-318、UART5 368、MAILBOX 100，
全部路由到 `CPU_GET_AFFINITY(3, 0)`（cpu_l3）。

### 3.8 GPIO EXT 中断分组：硬件能力存在（部分 UNVERIFIED）

**RK3588 每个 GPIO bank 有两条中断输出线**（`rk3588s.dtsi:8059-8129`，VERIFIED）：

| bank | 主线 SPI | EXT 线 SPI | EXT INTID |
|---|---|---|---|
| gpio0 | 277 | 282 | 314 |
| gpio1 | 278 | 283 | 315 |
| gpio2 | 279 | 284 | 316 |
| gpio3 | 280 | **285** | **317** |
| gpio4 | 281 | 286 | 318 |

而 `rk3588-amp.dtsi:22-27` 的 `/* GPIO EXT */` 块已把 314-318 全部路由到 cpu3。

哪些引脚走 EXT 线由 **BL31 通过 SIP 调用**决定（`gpio-rockchip.c:958`，VERIFIED 代码存在）：

```c
res = sip_smc_gpio_config(GPIO_SET_GROUP_INFO, bank->bank_num,
                          index, bank->irq_pins[index]);
```

Linux 侧入口是 `interrupt-pins` 属性（:950），配合 `interrupt-affinity`（:877）。
index 0 是整 bank 主线所以跳过（`rockchip_gpio_init_irq_pins()` 开头
`if (index == 0) return 0;`）。SIP 常量在 `include/linux/rockchip/rockchip_sip.h:256-264`
（`GPIO_GET_GROUP_INFO = 0`、`GPIO_SET_GROUP_INFO = 1`）。

约束：`RK_GPIO_EXP_IRQ_MAX_PIN_NUM = 2`（`pinctrl-rockchip.h:188`），每条 EXT 线最多 2 个引脚。
`RK_GPIO_IRQ_MAX_NUM = 4`（:187），RK3588 dts 只声明 2 条。

**UNVERIFIED / 风险：**
- 全树无任何 dts 使用 `interrupt-pins`。
- 驱动在 SIP 返回 `SIP_RET_DENIED` 时提示 `please modify syscfg.dts`（:967），
  说明 BL31 侧有白名单，可能要改 TF-A。
- `gpio-rockchip.c` 现有的 `interrupt-affinity` 路径是给 **Linux CPU** 用的
  （`irq_force_affinity()` + `of_cpu_node_to_id()`，且 `if (cpu == 0) return 0;`），
  不是 AMP 移交路径。AMP 要走 `amp-irqs`。
- GPIO/pinctrl 驱动**零 AMP 感知**（搜 `drivers/gpio/gpio-rockchip.c` 和
  `drivers/pinctrl/pinctrl-rockchip.c` 无 amp 命中），没有让 Linux 跳过某个引脚的机制。

rk3568 的 GPIO group AMP 配置是唯一参考实现，见 `rk3568-amp.dtsi:33-110`
（`gpio-group-banks`、`group-irq-en`、`bank-type-cfg`、`prio-group0/1`）。
RK3588 的 amp.dtsi **没有** `gpio-group` 节点，只用了简单的整条 EXT 线路由。

### 3.9 EVB7 触摸硬件（VERIFIED）

`rk3588-evb7-v11.dtsi:553-582`：

| 项 | 值 |
|---|---|
| 总线 | i2c5 |
| 控制器 | `goodix,gt1x` @0x14；另有 `hyn,3240` @0x5a（同引脚，二选一） |
| 中断 | gpio3 `RK_PC0` = pin **16**，level low |
| 复位 | gpio3 `RK_PC1` = pin 17 |
| 供电 | `vcc3v3_lcd_n` |
| pinctrl | `touch_gpio`（:791）、`ts_int_active`（:797） |

`RK_PC0 = 16`、`RK_PC1 = 17`（`include/dt-bindings/pinctrl/rockchip.h:28-29`）。

**关键约束：同一 bank 上还有 Type-C PD 控制器中断。**
`rk3588-evb7-v11.dtsi:587-596`：`usbc0: husb311@4e` 在 i2c6，但
`interrupt-parent = <&gpio3>; interrupts = <RK_PB6 IRQ_TYPE_LEVEL_LOW>;`
—— 即 gpio3 pin 14。所以整个 bank 3 的主中断线不能抢，否则影响充电。
（理论上 EXT 分组机制可只把 pin 16 划走而不影响 PB6，但该路径未验证，见 3.8。）

### 3.10 摄像头 Thunder Boot ISP（VERIFIED，但是顺序交接）

配置项 `CONFIG_VIDEO_ROCKCHIP_THUNDER_BOOT_ISP`
（`drivers/media/platform/rockchip/isp/Kconfig:73-77`），**不限 ISP 版本**，
只 `depends on ROCKCHIP_THUNDER_BOOT`，所以 RK3588（ISP v30）架构上可用。

当前 `.config` 里 `# CONFIG_ROCKCHIP_THUNDER_BOOT is not set`（`.config:6106`），没开。

设计意图直写在 UAPI 注释（`include/uapi/linux/rk-isp2-config.h:2234`）：

> struct rkisp_thunderboot_resmem - shared buffer for thunderboot with risc-v side

交接结构 `struct rkisp_thunderboot_resmem_head`（:2207-2231）传完整 pipeline 状态：
`enable/complete/frm_total/hdr_mode/rtt_mode`、`width/height/camera_num/camera_index`、
`exp_time[3]`/`exp_gain[3]`/`exp_time_reg[3]`/`exp_gain_reg[3]`（让 Linux 3A 无缝接上）、
`pre_buf_num`/`pre_buf_addr[4]`/`pre_buf_timestamp[4]`。

ioctl：`RKISP_CMD_GET_TB_HEAD`/`SET_TB_HEAD`（+27/+28，:95-98）、
`GET_SHARED_BUF`/`FREE_SHARED_BUF`（+2/+3，:25-28）、
`GET_SHM_BUFFD`（+6，实现 `rkisp_tb_helper.c:257`，把 reserved 内存包成 dma-buf）。

开启后 Linux 侧行为：`hw_dev->is_thunderboot` 置位（`isp/hw.c:1346`）；
**不注册 ISP 中断**（:1447 `if (!hw_dev->is_thunderboot) rkisp_register_irq(hw_dev);`）；
所有 ISR 直接 `return IRQ_HANDLED`（:141、:198、:233）；时钟被 loader-protect
（`rkisp_tb_clocks_loader_protect()`，`rkisp_tb_helper.c:172`），交接完成才
`rkisp_tb_unprotect_clk()`（`rkisp.c:5157`）；CIF/ISP initcall 提前到
`subsys_initcall_sync`（`cif/hw.c:2088-2090`）。

握手结果 `rkisp_tb_set_state(RKISP_TB_OK / RKISP_TB_NG)`。**NG 是回退路径**：
sensor 驱动见到 NG 就清 `is_thunderboot` 并自己 power on 从头初始化
（`imx415.c:1476-1480`、:2546-2549）—— 对面核没出图则 Linux 自动降级正常启动。

**本板 sensor 支持：** `drivers/media/i2c/imx415.c` 有 15 处 thunderboot 处理，
EVB7 用的正是 `sony,imx415`（`rk3588-evb7-v11-imx415.dtsi:55-56`）。
开关是 sensor 节点 DT 属性，宏定义 `imx415.c:175`：

```c
#define RKMODULE_CAMERA_FASTBOOT_ENABLE "rockchip,camera_fastboot"
```

```dts
&imx415 { rockchip,camera_fastboot = <1>; };
```

**为什么不是想要的东西：**
1. **顺序交接非并发共享**，交接后 ISP 归 Linux。
2. 需要 `rockchip,thunder-boot-rkisp` 节点（`rkisp_tb_helper.c:222-227` 匹配该 compatible）。
   全 `arch/arm64/boot/dts/rockchip/` 搜不到，只有 rv1126b 的 thunder-boot-mmc/sfc。
3. **NuttX 侧工作量不现实**：要实现 CSI2 D-PHY → MIPI CSI-2 → CIF/VICAP → ISP pipeline
   → 3A。ISP v30 寄存器规格不公开，`rkisp` 驱动两万多行还只是控制面，3A 在闭源
   `librkaiq` 里。

摄像头链路（`rk3588-evb7-v11-imx415.dtsi`）：
imx415@1a(i2c) → csi2_dphy0 → mipi2_csi2 → rkcif_mipi_lvds2 → rkcif_mipi_lvds2_sditf → rkisp0_vir0。

### 3.11 rptun 是什么（VERIFIED）

**R**emote **P**roc **TUN**nel（`drivers/rptun/Kconfig:7`、
`Documentation/components/drivers/special/rptun.rst`）。NuttX 里把 OpenAMP 的
remoteproc + rpmsg 绑到具体硬件「门铃 + 共享内存」上的适配层。

一句话：你提供「怎么捅对面核」和「共享内存在哪」，rptun 把上面整套消息通道搭好。

栈：

```
应用   uart_rpmsg / rpmsgfs / rpmsg socket / 自定义 endpoint
         ↓ rpmsg endpoint
OpenAMP  rpmsg_virtio → virtqueue → vring
         ↓
       remoteproc
         ↓
rptun  drivers/rptun/rptun.c          ← 这一层
         ↓ struct rptun_ops_s          ← 板级实现
硬件   mailbox 门铃 + reserved DDR
```

`struct rptun_ops_s`（`include/nuttx/rptun/rptun.h`）实际必须的五个：

| ops | 作用 | 本板实现（`rk3588_rptun.c`） |
|---|---|---|
| `get_cpuname` | 对端名，决定 `/dev/rptun/<name>` | 返回 `priv->cpuname` |
| `get_resource` | 交出 resource table | `rk3588_copy_rsc_table()`，返回**可写副本** |
| `is_master` | master 初始化 vring | `false`，Linux 是 host |
| `notify` | 敲门铃 | 写 mailbox0 B2A_DAT 魔数 + B2A_CMD link_id |
| `register_callback` | 收下回调 | 存 `priv->callback` |

可选：`start`/`stop`（本板空实现，u-boot 已启核）、`get_addrenv`（PA→VA，M0 侧需要、
A 核侧 NULL 因恒等映射）、`get_firmware`、`reset`、`set_phase`/`get_phase`、`is_autostart`。

rptun 帮做的：`rptun_initialize()` :1305 → `remoteproc_init()` 挂 ops、注册
`/dev/rptun/<cpuname>`（:1339-1340）、拉起 `rptun` kthread（:913/:921）。
该线程 `rptun_start_thread()` :874 建 vdev、配 vring、`rproc_virtio_set_shm_io()` :528、
跑 name-service 握手。对外给 `RPTUNIOC_START/STOP/RESET/WAIT` 和
`rptun_boot()`/`rptun_poweroff()`/`rptun_reset()`/`rptun_wait()`。

**本板两条独立 rptun 链路，共用一个 mailbox 的状态寄存器：**

| 文件 | 链路 | mailbox 通道 |
|---|---|---|
| `arch/arm64/src/rk3588/rk3588_rptun.c` | cpu_l3(NuttX) ↔ Linux | TX B2A ch0，RX A2B ch3 → INTID 100 |
| `arch/arm/src/rk3588-m0/rk3588m0_rptun.c` | PMU M0 ↔ Linux | ch1/ch2 |

这是 `rk3588_rptun_ack()` 里「只清自己 channel」注释的由来 —— 早期版本「防御性」清
全部四个 channel，偷掉了 M0 的门铃。

### 3.12 NuttX 侧现成的跨核输入机制（VERIFIED）

`drivers/input/uinput.c` + `CONFIG_UINPUT_TOUCH` + `CONFIG_UINPUT_RPMSG`
（Kconfig:131-157）：

- endpoint 名 `rpmsg-uinput-%s`（uinput.c:47）
- payload 是裸的 `struct touch_sample_s`（`uinput_touch_notify()` :328-332）
- 写设备节点 → 同时本地派发 + 广播给所有 rpmsg peer
  （`uinput_touch_write()` → `uinput_rpmsg_notify()` :348 + `uinput_touch_notify()` :351）
- 收到 rpmsg → `uinput_rpmsg_ept_cb()` :212-217 → `ctx->notify()` 注入本地

**缺口：这套是给 NuttX↔NuttX 设计的，Linux 侧无对端实现**，需自己写内核模块或
基于 `rpmsg_char` 的用户态守护，把 evdev 转成 `touch_sample_s`。

### 3.13 NuttX 上没有浏览器（VERIFIED）

- 跑不了 Chromium/WebKit：多进程沙箱、完整 GPU 栈、几百 MB 内存、ICU/GTK 依赖，
  NuttX 的 flat/protected 内存模型和 libc 覆盖面撑不住。
- 上游 apps 无 HTML 渲染引擎。已查 `apps/netutils`（只有 webclient/webserver/thttpd/
  libcurl4nx 等 HTTP 客户端服务端）、`apps/graphics`（lvgl/nxwidgets/nxwm/twm4nx）、
  `apps/interpreters`（quickjs/duktape/lua/luajit/python/wasm3/wamr/toywasm/bas/ficl）。
- **想用 Web 技术写 UI 的话手上就有**：`apps/frameworks/runtimes/quickapp/` 是 openvela
  的快应用运行时 —— QuickJS 引擎 + LVGL 渲染 + Yoga flex 排版，类 HTML/CSS 声明式页面、
  路由、生命周期，带 CDP 远程调试。加载 `.rpk` 包而非任意网页，本质是 Web-like 框架
  不是浏览器。核心库以预编译静态库形式提供（`quickapp`/`gui_wrapper`/
  `quickappfeatures`/`quickapp_inspector`），应用入口 `vapp`（独立）/ `vappxms`（XMS 集成）。
  同目录还有 `typescript`、`wasm`、`ash`、`feature`、`services` 运行时。
- 理论可移植的 HTML 引擎候选：NetSurf（libcss/libdom/libhubbub 纯 C 依赖少，
  官方最低内存约 32MB，已移植到 3DS）或 litehtml（C++，只排版需自接绘制后端）。
  **无人在 NuttX 上做过**，需自写前端接口层，且无 JS 只能看静态页。

### 3.14 RKLLM 是什么（VERIFIED via web，非本仓库）

瑞芯微官方 LLM 部署 SDK，仓库 `airockchip/rknn-llm`。把 LLM 跑在 RK 芯片 NPU 上。
支持 RK3588/RK3588S/RK3576/RK3562。两部分：**RKLLM-Toolkit**（x86 主机侧 Python 工具，
HuggingFace 模型 → 量化 → `.rkllm`）+ **RKLLM Runtime**（板侧 `librkllmrt.so`，
经 rknpu 驱动在 NPU 推理，C API）。

注意点：toolchain 与 runtime 版本必须对齐（不同版本转出的 `.rkllm` 不通用）；
需内核 `CONFIG_ROCKCHIP_RKNPU`；社区封装 `rkllama` 提供类 Ollama 接口。
与 **RKNN**（rknn-toolkit2 / `librknnrt.so`，面向 CNN 视觉模型）不是一回事，
共用 NPU 驱动但运行时库分开。

本地有相关树：`/media/1t/openvela/rknn-toolkit2/`、`/media/1t/openvela/librga/`。

---

## 4. Build and run

本次会话未构建、未烧写。以下命令来自本 changelog 目录的既有记录
（`rk3588-evb7-v11.md` 开头「Build & flash (known-good)」节），标记为**该文件声称
已验证**，本次会话未重新执行。

Armbian 镜像构建（cwd `/media/1t/openvela/armbain`）：

```bash
./compile.sh build BOARD=rk3588-evb7-v11 BRANCH=vendor RELEASE=bookworm \
    BUILD_MINIMAL=yes BUILD_DESKTOP=no
```

一次性主机准备（amd64 上构建 arm64 rootfs）：

```bash
sudo apt install qemu-user-static binfmt-support
# 或 docker run --privileged --rm tonistiigi/binfmt --install all
```

烧写（eMMC via MASKROM）：

```bash
rkdeveloptool db <rockdev>/MiniLoaderAll.bin
rkdeveloptool wl 0 output/images/Armbian-...Rk3588-evb7-v11_bookworm_vendor_6.1.115_minimal.img
rkdeveloptool rd
```

观察：
- Linux 串口 UART2 @ **1500000** 8N1，`console=ttyFIQ0`
- AMP 核（cpu_l3 / NuttX）串口 UART5，寄存器基址 `0xfeb80000`，
  amp-irqs 里 UART5 是 INTID 368
- 本次会话中用户打开的日志样例：`/home/jinglin/log/ttyUSB1_20260718_192847.log`
- 启动介质 eMMC（SD 启动无输出，直刷 eMMC 可用）

NuttX 构建命令本次未涉及，未记录于此 —— 需从
`/media/1t/openvela/work/` 下的 `build.sh` 确认（UNVERIFIED）。

---

## 5. Current state

### 本次会话（调研）
**未修改任何文件。** 产出即本文档。

### 同期用户已完成的代码（读取文件系统与 git 得到，VERIFIED）

`/media/1t/openvela/work/nuttx` 分支 `0712`，**工作区干净**（`git status --short` 无输出）。
最近三次提交：

```
2bca7fb3ee2 boards/rk3588/evb7-amp: report maxpoint 1, so touch samples are not discarded
3dd1bdc5173 boards/rk3588/evb7-amp: turn on LVGL's performance monitor, fix the frame stats
af0c158d0b4 boards/rk3588/evb7-amp: delete the shared-frame protocol, keep the touch channel
```

板级源码 `/media/1t/openvela/work/nuttx/boards/arm64/rk3588/evb7-amp/src/`：

| 文件 | 大小 | 说明 |
|---|---|---|
| `evb7_amp_vop.c` | 21841 | VOP 直接操作 |
| `evb7_amp_fb.c` | 25957 | framebuffer 层 |
| `evb7_amp_touch.c` | 8120 | **触摸，走 rpmsg 转发路线** |
| `evb7_amp_shm.c` / `.h` | 10339 / 13144 | 共享内存与协议 |
| `evb7_amp_bringup.c` | 5974 | 板级 bringup |
| `evb7_amp.h` | 3944 | 板级头 |
| `evb7_amp_appinit.c` / `_boardinit.c` / `_reset.c` | — | 早期骨架 |

**触摸方案已定并落地：** `evb7_amp_touch.c` 头部注释记录了决策依据 ——
i2c5 和 gpio3 都不能在 Linux 运行时干净接管，因为 bank 3 的一条中断线覆盖所有引脚，
且 Type-C PD 控制器中断在同一 bank，抢了会破坏充电。因此不接管硬件，
改为消费 Linux 已解码的事件，做成 `/dev/input0` 的一个「无硬件的 lower half」，
使 LVGL/NX 和普通应用无需修改。

该文件记录了一个**框架层未写明的不变量**（值得留意）：
`maxpoint` 必须等于每个 sample 实际携带的 `npoints`，否则读者静默丢事件。
上半部按 `SIZEOF_TOUCH_SAMPLE_S(sample->npoints)` 入队
（`touchscreen_upper.c:374`），`read()` 返回可用字节数（:237），
而 LVGL 后端把字节数与 `maxpoint` 尺寸做**相等**比较，不等就丢弃。
最后一次提交 `2bca7fb3ee2` 正是修这个（report maxpoint 1）。

已知副作用：触摸依赖 Linux 存活。Linux 停止转发时该设备静默，
且无法区分「没人触摸」与「对面核不说话了」。

`af0c158d0b4` 删掉了 shared-frame 协议只留触摸通道 —— 说明显示路径已不走
共享帧协议（推测走 VOP 直接操作，UNVERIFIED，需读 `evb7_amp_vop.c` 确认）。

### 既有文档

`/media/1t/openvela/armbain/.kiro/board-changes/`：

| 文件 | 大小 | 说明 |
|---|---|---|
| `rk3588-evb7-v11.md` | 257KB | **主 changelog**，含 build/flash known-good、AMP 端口全过程 |
| `rk3588-evb7-v11-AMP-overview.md` | 10KB | AMP 架构总览 |
| `HANDOFF-rk3588-evb7-v11.md` | 10KB | 早期交接（7月12） |
| `rk3588-evb7-v11-SESSION-SUMMARY.md` | 10KB | 早期会话总结 |
| `HANDOFF-rk3588-amp-peripheral-sharing-20260730.md` | 本文件 | |

主 changelog 记录的既有状态：AMP bare-metal validation **PASSED**
（cpu3 跑固件，Linux 在 7 核上）。AMP 载荷加载地址经修订，
详见该文件「AMP memory region (IMPORTANT - revised after first boot)」节。

---

## 6. Next steps

按优先级排序。

1. **决定显示路径是否改用 `reserved-plane`。**
   当前 `evb7_amp_vop.c` 是 NuttX 直接操作 VOP（推测），没有走内核的
   reserved-plane 协商。若要改：
   - Linux dts 里给选定 VP（建议 VP2 或 VP3，**不要 VP0/VP1**）加
     `rockchip,reserved-plane = <phys_id>`，**并确保该 plane 仍在
     `rockchip,plane-mask` 里**（否则 3.4 节那个静默覆盖的坑）
   - 验证 `rk3588_vop2_cfg_done()` 走进了按位提交分支（可加打印或看
     `RK3588_SYS_WIN_REG_CFG_DONE` 写入）
   - 补 RK3588 的置顶逻辑（`vop2_setup_layer_mixer_for_vp()` 里加
     reserved_plane 处理，参考 `vop3_setup_layer_sel_for_vp()` :12577-12582）
   - 处理 IOMMU：去掉 vop 节点的 `iommus`，或恒等映射
   - 收益 vs 现状：能和 Linux 共屏叠加、cfg_done 不再互相踩。
     若当前直接操作方案已稳定且不需要与 Linux 共屏，**可以不动**。

2. **摄像头（若需要）：走 rpmsg 帧共享，不要在 NuttX 实现 ISP。**
   Linux 侧正常用 `rkisp` 出图，通过 rpmsg 传 `{paddr, size, w, h, fmt, seq, timestamp}`,
   NuttX 直接映射读，零拷贝。`evb7_amp_shm.c` 基础设施已在。
   注意 `af0c158d0b4` 刚删掉了 shared-frame 协议，若要做需重新设计
   （或从该提交恢复参考）。

3. **NPU（若需要）：走 rpmsg 推理服务。**
   Linux 侧持有 NPU 正常跑 RKNN/RKLLM，接收 NuttX 的推理请求。
   数据零拷贝：两边约定 shm 里的 tensor 布局，Linux 侧把那块物理内存
   注册成 NPU 的 dma-buf（`rknpu_gem.c` 支持 dma-buf import，UNVERIFIED 具体 API）。
   开销一次 mailbox 往返，相对推理耗时（毫秒到几十毫秒）可忽略。

4. **触摸：补 Linux 侧转发端（若尚未完成）。**
   NuttX 侧已就绪。Linux 侧需要把 evdev 事件转成 `touch_sample_s` 发到 rpmsg。
   可选复用 NuttX 的 `uinput` 协议（endpoint 名 `rpmsg-uinput-<name>`），
   或用自定义 endpoint。**需确认当前 Linux 侧转发是怎么实现的** ——
   本次会话未查证，`evb7_amp_touch.c` 只描述了 NuttX 侧。

5. **补 rptun 的 suspend/resume 韧性（仅当引入 Linux S3 或 CPU hotplug 时）。**
   `rk3588_rptun.c` 的 `rk3588_rptun_notify()` 里有一段被**故意注释掉**的
   `rk3588_rptun_irq_rearm()`。当前依赖启动期 30s keepalive 覆盖 Linux 一次性
   `gic_dist_init`（~15s）。若将来引入 S3/hotplug（会重跑 gic_dist_init），
   必须取消该注释，否则首次 resume 后 RX 中断永久失效。文件内注释已写明。

6. **未解的未知项：**
   - `interrupt-pins` + BL31 syscfg 白名单具体怎么改（若将来想真接管触摸引脚）
   - RK3588 上 reserved-plane 的 IOMMU 方案哪种更省事（去 iommus vs 恒等映射）
   - NuttX 构建命令未记录（见第 4 节末）

---

## 7. Traps

按「会静默浪费时间」排序。

1. **`vop2_plane_mask_check()` 失败只打 `DRM_WARN` 然后用默认 plane 分配覆盖你的 dts**
   （`rockchip_drm_vop2.c:17041-17043`）。检查条件之一是所有 VP 的 plane-mask
   并集必须精确等于 `vop2_data->plane_mask_base`（:16374）。
   把 reserved plane 从 plane-mask 剔掉 → 配置被丢弃 → plane 回到 Linux 手里，
   dmesg 里只有一行 `use default plane mask`。

2. **`rockchip,reserved-plane` 收 phys_id 不是 bitmask。**
   写 `<(1 << ROCKCHIP_VOP2_ESMART3)>`（= 512）会被当成 phys_id，静默错位到别的层。
   而同名 DRM 属性 `RESERVED_PLANE_MASK` 是 bitmask。两者相反。

3. **`amp-irqs` 里的数字是 GIC INTID 不是 SPI 号**（INTID = SPI + 32）。
   验证锚点：rk3588-amp.dtsi 的 UART5 项 368 = SPI 336 + 32。
   算错就是中断路由到错误的 CPU 或不存在的中断。

4. **Linux 的 `gic_dist_init()`（~15s）会 disable 所有 SPI 并清 mailbox A2B_INTEN，
   且 rockchip-amp 只恢复 IROUTER 不重新 enable。**
   NuttX 侧必须自己 re-arm。`rk3588_rptun.c` 用一个有界 keepalive 线程
   （100ms × 300 轮 = 30s 后退出）覆盖这个一次性事件。
   任何新加的、路由到 AMP 核的 GIC 中断（例如 GPIO EXT INTID 317）都会遇到
   **完全相同**的问题，必须同样处理。
   表现症状：启动初期中断正常，~15s 后永久静默。

5. **NuttX 通用的 `arm64_gic_irq_enable()` 会把 IROUTER 写成 `up_cpu_index()`**
   （UP build 里是 0 = cpu0 = Linux），把中断从 AMP 核抢走。
   `up_enable_irq()` 之后必须重写 IROUTER 到目标 affinity
   （本板 cpu_l3 是 `0x300`）。见 `rk3588_rptun.c` 的
   `putreg64b(RK3588_CPU_L3_AFF, RK3588_GICD_IROUTER(...))`。

6. **mailbox0 的四个 A2B channel 共享一个 status 寄存器。**
   「防御性」地清全部四个会偷掉 M0 链路（ch1/ch2）的门铃，
   M0 的 handler 随后发现无事可做，永远不通知它的 OpenAMP 去看 rings。
   只清自己的 channel。启动期这个窗口在 M0 链路上曾累计 93 个门铃。

7. **中断被 disarm 期间到达的门铃，仅靠 re-arm 恢复不了** ——
   mailbox status 位还是置着但中断已经错过。re-arm 后必须主动查一次 status
   并补一次 `nxsem_post`。见 `rk3588_rptun_irq_keepalive()` 里那段。

8. **冷启动时 reserved DDR 里可能残留上一次 NuttX 运行的 vring 状态。**
   NuttX 会在 Linux 初始化 vring 之前很早就 pin 住
   `VIRTIO_CONFIG_STATUS_DRIVER_OK`，于是读到过期的 `avail->idx` 并重放几十个
   孤儿 descriptor（实测同一个 name-service buffer 被派发 20+ 次），
   触发 OpenAMP buffer held-counter 下溢断言（`rpmsg_virtio.c`）和
   RX work pool 耗尽断言（`rpmsg.c`）。
   `rk3588_rptun_init()` 开头 memset vring 区域正是为此。

9. **resource table 里的地址必须全部是物理地址，不能混用窗口视图。**
   OpenAMP 把 resource-table 地址当物理地址并过 `up_addrenv_pa_to_va()`。
   早期版本在表里放了 M0 的窗口视图（0x60000000+），因为那些地址落在翻译窗口外
   被原样透传，**看起来能工作**，但导致 vring 地址已翻译而 buffer 地址未翻译 ——
   一张表两种约定。诊断发现 `pa_to_va` 被喂进了 `0x60008000`（vring1 自己的视图）。

10. **`touch_sample_s` 的 maxpoint/npoints 必须相等，否则读者静默丢事件。**
    框架未写明。上半部按 `SIZEOF_TOUCH_SAMPLE_S(sample->npoints)` 入队
    （`touchscreen_upper.c:374`），LVGL 后端拿字节数与 `maxpoint` 尺寸做相等比较。
    症状：设备存在、有数据写入、应用收不到事件。

11. **`rockchip,amp-shared`（`i2c-rk3x.c:2008`）名不副实。**
    它只是在 Thunder Boot 期间挂着 `IRQ_NOAUTOEN`，等 `rk3x_i2c_tb_cb()`
    回调里才 `enable_irq()`（:1920）。是**启动期顺序交接**不是并发共享，
    依赖 `CONFIG_ROCKCHIP_THUNDER_BOOT_SERVICE`，且全树无 dts 使用。

12. **`rknpu_ioctl.h` 的 `core_mask` 不是 AMP 划分**，是单任务选核
    （同一 OS 内的调度）。`rk3588_rknpu_config.core_mask = 0x7` 是写死的能力常量。

13. **看起来对但没有实现的东西**（全树只有宏/代码，无 dts 使用，未验证）：
    `rockchip,shared-mode`、`rockchip,reserved-plane`、`interrupt-pins`、
    `rockchip,thunder-boot-rkisp`、`rockchip,amp-shared`、`ROCKCHIP_DRM_FBD_*`。
    遇到这类机制先搜 `arch/*/boot/dts/` 确认有无参考实现，再决定投入。

14. **`VIDEO_ROCKCHIP_THUNDER_BOOT_ISP` 不限 ISP 版本**（只依赖
    `ROCKCHIP_THUNDER_BOOT`），容易误以为 RK3588 不支持；但反过来，
    "Kconfig 允许" 不等于 "有人验证过"。
