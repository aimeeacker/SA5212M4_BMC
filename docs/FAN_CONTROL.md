# 浪潮 SA5212M4 BMC 自定义精细静音调速技术手册与配置指南

本文档为浪潮 (Inspur) SA5212M4 服务器 BMC 固件自定义智能调速系统（`libfanhook.so` 与 `fan_control.conf`）的完整技术规范与参数配置手册。

---

## 目录
- [一、 架构概览与部署规范](#一-架构概览与部署规范)
- [二、 硬件 PWM 拓扑与风道流场设计](#二-硬件-pwm-拓扑与风道流场设计)
- [三、 调速核心逻辑与数学模型](#三-调速核心逻辑与数学模型)
- [四、 参数完全解析手册 (Reference)](#四-参数完全解析手册-reference)
- [五、 典型应用场景配置实战](#五-典型应用场景配置实战)
- [六、 运行时监控与故障排查](#六-运行时监控与故障排查)

---

## 一、 架构概览与部署规范

### 1. 配置文件部署层级
本系统设计了多级自动回退与持久化部署架构：

| 部署路径 | 分区与文件系统 | 持久性与说明 |
| :--- | :--- | :--- |
| `/conf/fan_control.conf` | **JFFS2（可读写 Flash）** | **【推荐首选】** 固件升级或 BMC 重启后修改永不丢失。 |
| `/etc/fan_control.conf` | CramFS（只读 RootFS） | **【出厂默认】** 刷机后的系统初始全局配置。 |
| `/etc/defconfig/fan_control.conf` | CramFS（只读 RootFS） | **【安全备份】** 用于误删或重置时的初始模版。 |

### 2. 零开销按需重载机制 (Zero-Overhead Event-Driven Reload)
- **彻底移除高频轮询**：移除了后台线程每周期对磁盘的 `stat()` / `inotify` 轮询检查，常态运行时无任何额外文件系统开销。
- **切换自动模式触发重载**：
  - 系统仅在**启动时加载一次配置**。
  - 在用户于 Web 控制台或通过 IPMI 切换到 **AUTO（自动）模式**时触发热重载：立即从 `/conf/fan_control.conf` 读取最新参数，重构温控曲线，并瞬间完成 0 延迟风速接管。
  - **如何在线生效最新配置**：
    - 方式 1（Web 后台）：在风扇控制页面将模式切到 Manual 再切回 Auto 即可。
    - 方式 2（IPMI 命令）：执行 `ipmitool raw 0x30 0x7a 0x00`。

---

## 二、 硬件 PWM 拓扑与风道流场设计

### 1. AST2300 硬件 PWM 与转速计对应关系
浪潮 SA5212M4 采用 ASPEED AST2300 BMC 芯片，物理风扇通道与内部硬件引脚的映射关系如下：

```
+-------------------------------------------------------------------------+
|                       浪潮 SA5212M4 机箱内部风道布局                     |
|                                                                         |
|   [ 前置硬盘背板 ]                                                       |
|         |                                                               |
|         v                                                               |
|   +-----------+         +-----------+         +-----------+             |
|   |   Fan 4   |         |   Fan 2   |         |   Fan 0   |             |
|   |  (PWM 0)  |         |  (PWM 1)  |         |  (PWM 5)  |             |
|   | CPU0 直吹 |         | 中置主力  |         | CPU1 直吹 |             |
|   +-----------+         +-----------+         +-----------+             |
|         |                     |                     |                   |
|         v                     v                     v                   |
|   [   CPU 0   ]         [ 供电 VRM / PCH ]    [   CPU 1   ]             |
|   (SDR: 0x19)           [  主板核心区域  ]    (SDR: 0x1a)               |
|         |                                           |                   |
|         v                                           v                   |
|   +-----------+                                                         |
|   |   Fan 6   |                                                         |
|   |  (PWM 4)  |                                                         |
|   | 尾部/PCIe |                                                         |
|   +-----------+                                                         |
|         |                                                               |
|         v                                                               |
|   [ 后窗出风口 ]                                                         |
+-------------------------------------------------------------------------+
```

| 逻辑风扇 | 物理角色与流场覆盖 | 对应硬件 PWM | PDK 引脚号 | 测速 Tach 通道 | 初始控制策略 |
| :---: | :--- | :---: | :---: | :---: | :--- |
| **Fan 0** | CPU 1 辅助直吹风道 | **PWM 5** | 5 | Tach 5 | 跟随 Fan 2 偏置（Fan2 - 15%），最低 1% |
| **Fan 2** | **中置主力风道**（CPU 内侧/主板供电/桥片） | **PWM 1** | 1 | Tach 1 | 全温控主力，常温 18%，按温度阶梯曲线平滑调节 |
| **Fan 4** | CPU 0 辅助直吹风道 | **PWM 0** | 0 | Tach 0 | 跟随 Fan 2 偏置（Fan2 - 15%），最低 1% |
| **Fan 6** | 尾部辅助出风 / PCIe 辅助冷却 | **PWM 4** | 4 | Tach 4 | **严格恒定 1% 底速**（约 1440 RPM，绝不升速） |

### 2. 为什么这样设计？
1. **消除四风扇同频共振啸叫**：
   原厂策略或粗暴调速常将 4 个风扇设为相同占空比，导致 4 台高转速高风压风机在金属机箱内形成剧烈的驻波与共振啸叫。
2. **Fan 2 担当核心主力**：
   Fan 2 位于机箱中心，气流直接吹拂两颗 CPU 的主散热器侧翼，并覆盖发热量巨大的主板供电 MOS 管（VRM）和南桥芯片（PCH）。
3. **Fan 6 锁定 1% 的工程考量**：
   Fan 6 处于出风口侧边，日常待机时风阻大，若转速超过 10% 会产生刺耳的高频哨音。保持 1% 物理底速可提供稳定微弱负压抽风，同时消除噪音。

---

## 三、 调速核心逻辑与数学模型

### 1. 动态差值平移机制 (Dynamic Delta Shift)
系统以 `idle_upper_temp`（默认 68°C）为核心基准锚点。用户调整该参数时，系统内的所有阶梯温区、爬升温区、缓降温区及极冷阈值将自动同向平移：

$$\text{Offset} = \text{idle\_upper\_temp} - 68^\circ\text{C}$$

- 缓降判定阈值：$\text{quiet\_down\_temp\_c} = \text{idle\_upper\_temp} - 2^\circ\text{C}$
- 爬升下限阈值：$\text{smooth\_up\_low\_temp\_c} = \text{idle\_upper\_temp}$
- 爬升上限阈值：$\text{smooth\_up\_high\_temp\_c} = \text{idle\_upper\_temp} + 4^\circ\text{C}$
- 极冷深静音阈值：$\text{cold\_ambient\_temp} = \text{idle\_upper\_temp} - 20^\circ\text{C}$

### 2. 双路对称风道协同算法
在每个调速周期中，系统分别采集两路 CPU 实时温度：
1. **双 CPU 温度对称时（$|T_{\text{cpu0}} - T_{\text{cpu1}}| \le 4^\circ\text{C}$）**：
   - Fan 2 由系统最高温度计算出目标转速 $S_{\text{fan2}}$；
   - Fan 0 与 Fan 4 完全对称同步：
     $$S_{\text{fan0}} = S_{\text{fan4}} = \max(S_{\text{fan2}} - \text{fan\_aux\_offset},\ \text{fan\_standby\_floor})$$
2. **单路 CPU 偏载大温差时（$|T_{\text{cpu0}} - T_{\text{cpu1}}| > 4^\circ\text{C}$）**：
   - 较热侧风扇维持基准辅助转速；
   - 较冷侧风扇根据自身温度进一步向 `fan_standby_floor` 缓慢回落，最大限度降低不必要的风噪。

---

## 四、 参数完全解析手册 (Reference)

配置文件 [src/fan_control.conf](file:///home/aimee/SA5212M4_BMC/src/fan_control.conf) 采用合并数组与矩阵表示，每个参数上方保留一行简洁的英文注释。底层同时兼容数组表示与传统单参数表示：

### 1. 数组与矩阵合并参数速查表
| 参数名称 | 格式与默认值 | 包含子项说明 |
| :--- | :--- | :--- |
| `ramp_steps` | `2, 1` | `[smooth_up_step, quiet_down_step]`：升温爬升步长 (+2%)，降温缓降步长 (-1%) |
| `fan_channels` | `2, 4, 0` | `[main_channel, cpu0_channel, cpu1_channel]`：中置主力、CPU0 直吹、CPU1 直吹通道 |
| `fan_main_speeds` | `18, 6` | `[normal_idle, cold_idle]`：常温基准怠速 (18%)，极冷深静音底速 (6%) |
| `fan_limits` | `1, 100` | `[fan_min, fan_max]`：系统允许绝对最低风速 (1%)，最高风速 (100%) |
| `cpu_sensors` | `0x19, 0x1a` | `[sensor_cpu0, sensor_cpu1]`：CPU0 与 CPU1 温度读取 SDR 编号 |
| `thermal_curve` | `0:18, 3:20, 6:22, 12:28, 18:40, +:70` | 阶梯温控矩阵：`[相对温差:目标风速%]`。支持 Level 0 至 Level 5 6级阶梯 |

> **提示**：温控曲线亦可写作两组独立数组形式：  
> `curve_speeds = 18, 20, 22, 28, 40, 70`  
> `curve_offsets = 3, 6, 12, 18`

---

### 2. 参数逐项解析与取值范围

#### (1) 温度阈值与基准
- `idle_upper_temp = 68`（范围：50 ~ 70）  
  常温标准怠速上限温度 (°C)。CPU < 68°C 时主力风扇维持怠速，整机极致静音。硬性上限保护 70°C。
- `cold_ambient_temp = 48`（范围：20 ~ 60）  
  极冷环境深静音判定阈值 (°C)。环境温度极低且 CPU < 48°C 时，允许 Fan 2 下探至 cold_idle。

#### (2) 采样控制与动态步长
- `check_interval = 30`（范围：1 ~ 60）  
  采样与调速周期 (秒)。
- `hysteresis_c = 2`（范围：1 ~ 10）  
  温度迟滞死区 (°C)。降温时只有低于 `(阈值 - hysteresis_c)` 才会降档，消除临界震荡。
- `ramp_steps = 2, 1`（升温 1~20%，降温 1~10%）  
  升温每周期递增 2%，降温每周期递减 1%，避免阶跃噪音。

#### (3) 风道通道与转速偏置
- `fan_channels = 2, 4, 0`  
  物理风扇通道映射（中置主力=2，CPU0直吹=4，CPU1直吹=0）。
- `fan_main_speeds = 16, 6`  
  Fan 2 基准转速设置：常温待机 16%（约 1900 RPM），极冷最低 6%。
- `fan_aux_offset = 15`（范围：0 ~ 50）  
  辅助风扇相对主风扇负偏置 (%)。双路 CPU 温度对称时：$$\text{Fan 0/4} = \max(\text{Fan 2} - 15\%,\ 1\%)$$
- `fan_standby_floor = 1`（范围：1 ~ 100）  
  辅助风扇与 Fan 6 闲时静音底速 (1%，约 1440 RPM)。Fan 6 始终锁定在此值。
- `fan_limits = 1, 100`  
  全系统绝对转速上下限，防止下溢为 0 或负数。
- `disabled_fan_mask = 0`（范围：0 ~ 255）  
  禁用风扇位掩码。若设为 `64`（即 1 << 6），彻底关闭 Fan 6（输出 0% PWM 并热补丁内存 `g_FanConfig`，彻底屏蔽 0 RPM 丢速告警、黄/红指示灯与 SEL 错误日志）。
- `max_fan_delta = 30`（范围：5 ~ 50）  
  相邻风道最大允许气压差 (%)，防止中置高风压向侧面形成湍流倒灌。

#### (4) 传感器与故障防护
- `cpu_sensors = 0x19, 0x1a`  
  CPU 0 (0x19) 与 CPU 1 (0x1a) SDR 编号。
- `sensor_fail_safe_speed = 50`（范围：20 ~ 100）  
  传感器失效安全兜底风速 (%)。连续 3 次读取失败时自动锁定 50% 保护。
- `log_level = 1`（0: 仅错误, 1: 调速事件, 2: 详细轮询）。

#### (5) 阶梯温控矩阵
- `thermal_curve = 0:16, 3:20, 6:22, 12:28, 18:40, +:70`  
  以 `idle_upper_temp = 68` 为基准：
  - **Level 0**（$< 68^\circ\text{C}$）：**16%**
  - **Level 1**（$68 \sim 71^\circ\text{C}$，偏移 +3）：**20%**
  - **Level 2**（$71 \sim 74^\circ\text{C}$，偏移 +6）：**22%**
  - **Level 3**（$74 \sim 80^\circ\text{C}$，偏移 +12）：**28%**
  - **Level 4**（$80 \sim 86^\circ\text{C}$，偏移 +18）：**40%**
  - **Level 5**（$\ge 86^\circ\text{C}$）：**70%**

---

## 五、 典型应用场景配置实战

### 场景 A：出厂黄金平衡模式（推荐日常使用）
- **特点**：兼顾双路 CPU 60~65°C 散热稳定与桌面级静音。
- **配置要点**：
  ```ini
  idle_upper_temp = 68
  fan_main_speeds = 16, 6
  fan_aux_offset = 15
  thermal_curve = 0:16, 3:20, 6:22, 12:28, 18:40, +:70
  ```
- **实际转速**：Fan 2 = 16%，Fan 0/4 = 1%（16% - 15%），Fan 6 = 1%。

---

### 场景 B：极致静音模式（家用卧室 / 极低负载环境）
- **特点**：进一步压低中置风扇转速，整机逼近被动散热。
- **配置要点**：
  ```ini
  idle_upper_temp = 68
  fan_main_speeds = 14, 6
  fan_aux_offset = 15
  thermal_curve = 0:14, 3:18, 6:22, 12:28, 18:40, +:70
  ```
- **实际转速**：Fan 2 = 14%，Fan 0/4 = 1%（触底），Fan 6 = 1%。

---

### 场景 C：性能加速 / 高环境温度模式（机房或多卡扩展）
- **特点**：提升基准风压，快速排出 PCIe 显卡与 NVMe 扩展卡积热。
- **配置要点**：
  ```ini
  idle_upper_temp = 60
  fan_main_speeds = 24, 10
  fan_aux_offset = 10
  thermal_curve = 0:24, 3:28, 6:32, 12:40, 18:55, +:80
  ```
- **实际转速**：Fan 2 = 24%，Fan 0/4 = 14%，Fan 6 = 1%。

---

### 场景 D：软件彻底关闭 Fan 6（静音停转且不告警）
- **特点**：将 Fan 6 完全停转（0% PWM），同时屏蔽 BMC 的 0 RPM 丢失报警与红色警告灯。
- **配置要点**：
  ```ini
  disabled_fan_mask = 64
  ```

---

## 六、 运行时监控与故障排查

### 1. 实时调速状态查询
系统在每次调速决策后会自动将当前快照写入 `/tmp/fan_control_status`：
```bash
cat /tmp/fan_control_status
```
输出示例：
```ini
TEMP_CPU0=62
TEMP_CPU1=62
TEMP_SYS=62
IDLE_UPPER=68
LEVEL_SYS=0
FAN0_CPU1=3
FAN2_MAIN=18
FAN4_CPU0=3
FAN6_AUX=1
AUX_OFFSET=15
```

### 2. 调速日志排查
查看调速历史与事件日志：
```bash
tail -f /var/log/fan_control.log
```
日志输出范例：
```
[2026-09-11 12:00:00] [FANHOOK] Loaded config /conf/fan_control.conf: idle_upper=68C, ramp=+2%/-1%, main=[18%,6%], aux_offset=15%, levels=[18,20,22,28,40,70]
[2026-09-11 12:00:05] [FANHOOK] Fan speed adjust: Fan0(CPU1)=3%, Fan2(Main)=18%, Fan4(CPU0)=3%, Fan6(Lock)=1% (CPU0=62C, CPU1=62C, aux_offset=15%)
```
