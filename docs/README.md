# 浪潮 SA5212M4 服务器 BMC 固件逆向分析与风扇静音改造指南

本项目针对 **浪潮（Inspur）英信 SA5212M4 服务器** 的 BMC 固件（`SA5212M4_BMC_4.35.0_Standard_20191025`）进行了全量只读逆向工程分析。记录了固件结构、签名校验机制、根文件系统解密、自动风扇控制逻辑、SSH/Shell 获取机制、Web 控制台 HTTPS/证书机制以及自定义温控曲线与禁用特定风扇的工程实现方案。

---

## 目录
- [一、 固件概况与物理架构](#一-固件概况与物理架构)
- [二、 镜像签名与安全启动分析](#二-镜像签名与安全启动分析)
- [三、 根文件系统解密与提取机制](#三-根文件系统解密与提取机制)
- [四、 核心风扇控制体系与调速原理深度剖析](#四-核心风扇控制体系与调速原理深度剖析)
- [五、 空风扇位防暴走机制与安全禁用风扇方案](#五-空风扇位防暴走机制与安全禁用风扇方案)
- [六、 SSH 远程登录直接启用 Bash 方案](#六-ssh-远程登录直接启用-bash-方案)
- [七、 Web 控制台 HTTPS 强制机制与 SSL 证书安全替换方案](#七-web-控制台-https-强制机制与-ssl-证书安全替换方案)
- [八、 智能温控曲线、PWM硬件映射与实时接管深度方案](#八-智能温控曲线pwm硬件映射与实时接管深度方案)
- [九、 固件刷写指南：Web 控制台升级与 UEFI Shell 极速刷写](#九-固件刷写指南web-控制台升级与-uefi-shell-极速刷写)
- [十、 一键构建系统与 GitHub Actions 自动化 CI 流水线 (Node.js 24)](#十-一键构建系统与-github-actions-自动化-ci-流水线-nodejs-24)
- [十一、 仓库结构与工具集说明](#十一-仓库结构与工具集说明)

---

## 一、 固件概况与物理架构

* **适用硬件**：浪潮英信 SA5212M4 机架式服务器（双路 Intel Xeon E5-2600 v3/v4，ASPEED AST2300 BMC 芯片）
* **固件版本**：`4.35.0.0` (Build Date: Oct 2019)
* **镜像规格**：33,554,432 字节（精确 32MB SPI Flash 全量备份映像）
* **操作系统**：Linux 2.6.28.10-ami (ARMv5TE / ARM926EJ-S)
* **管理框架**：AMI MegaRAC SP-X 架构

### 分区与 FMH (Flash Module Header) 布局详解

整个 32MB Flash 镜像采用标准的 AMI FMH 模块化管理机制。
每个分区头部带有 `$MODULE$`（64 字节结构头），尾部带有 `0x55 0xAA` 标志。

> [!NOTE]
> **关于 FMH 地址与载荷地址相差 64 KiB (`0x10000`) 的技术原委**：
> SPI NOR Flash 的物理擦除块（Erase Block）大小为 64 KiB。对于可读写文件系统分区（JFFS2，如 `conf`、`bkupconf`、`www`、`lmedia`）以及压缩只读镜像（`iroot`），FMH 头部必须独占整整一个 64KB 块（即 `0x140000 - 0x14FFFF`，除 64 字节头外其余均为 `0xFF` 填充）。这样 Linux 内核挂载与写入/擦除 JFFS2 块时，才不会误伤位于该扇区头部的 FMH 引导头。实际载荷（Payload）从下一个 64KB 边界（即 `0x150000`）正式开始。
> 而对于 `osimage`（内核）和 `ast2300e`（元数据），由于内核无需挂载为可写文件系统，其有效载荷紧随 64 字节 FMH 头之后（偏移 `+0x40`）。

| 模块名称 | FMH 起始 (Offset) | 分区范围 (Alloc Range) | 分区总大小 | 载荷偏移 (Rel Offset) | 载荷物理地址 (Payload Addr) | 有效载荷大小 | 文件系统 / 封装格式 |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `cboot` | `0x0000_0000` | `0x0000_0000 - 0x0004_0000` | 256 KB | `+0x0000` | `0x0000_0000` | 256 KB | U-Boot 引导程序 (FMH 位于 `0xFF80`) |
| `conf` | `0x0004_0000` | `0x0004_0000 - 0x000C_0000` | 512 KB | `+0x10000` | `0x0005_0000` | 448 KB | JFFS2（**可读写配置区**，断电不丢失） |
| `bkupconf` | `0x000C_0000` | `0x000C_0000 - 0x0014_0000` | 512 KB | `+0x10000` | `0x000D_0000` | 448 KB | JFFS2（备份配置区） |
| `iroot` | `0x0014_0000` | `0x0014_0000 - 0x0114_0000` | 16 MB | `+0x10000` | `0x0015_0000` | 15.93 MB | **128-bit TEA 加密 uImage / CramFS** |
| `osimage` | `0x0114_0000` | `0x0114_0000 - 0x012E_0000` | 1.625 MB | `+0x0040` | `0x0114_0040` | 1.59 MB | Linux 2.6.28.10 ARM 内核 uImage |
| `www` | `0x012E_0000` | `0x012E_0000 - 0x0170_0000` | 4.125 MB | `+0x10000` | `0x012F_0000` | 4.02 MB | JFFS2（Web 控制台静态资源与 CGI） |
| *(保留空间)*| *(N/A)* | `0x0170_0000 - 0x018F_0000` | 1.94 MB | *(N/A)* | *(N/A)* | 1.94 MB | 全 `0xFF` 闲置未分配空间 |
| `lmedia` | `0x018F_0000` | `0x018F_0000 - 0x01FF_0000` | 7 MB | `+0x10000` | `0x0190_0000` | 6.93 MB | JFFS2（虚拟媒体与 Java KVM 扩展） |
| `ast2300e` | `0x01FF_0000` | `0x01FF_0000 - 0x0200_0000` | 64 KB | `+0x0040` | `0x01FF_0040` | 105 B | 平台元数据与版本描述信息 |

---

## 二、 镜像签名与安全启动分析

**结论：该固件无任何 RSA 非对称加密签名，未启用硬件级 Secure Boot。**

1. **证书与公钥扫描**：全镜像未发现任何 X.509 证书、RSA 公钥、PKCS#1/#7/#8 格式数据（零 `BEGIN PUBLIC KEY` / `BEGIN CERTIFICATE`）。
2. **U-Boot 校验逻辑反汇编**：
   * 逆向分析 U-Boot 的 `bootfmh` 命令处理函数（地址 `0x4081c108` 起始）：
   * 仅校验 FMH 头部的 **8-bit 累加和补码**（`CalculateChecksum8`，确保 `sum(FMH[0..63]) == 0`）；
   * 仅校验 uImage 自带的 **CRC32**；
   * U-Boot 内部未编译嵌入任何非对称加解密算法库（无 OpenSSL、mbedtls、RSA、SHA256 等）。
3. **重打包可行性**：固件支持随意修改重打包，只要按规范重新计算 FMH 8-bit Checksum 和 CRC32 即可正常引导，不会触发验签拦截。

---

## 三、 根文件系统解密与提取机制

`iroot` 模块的有效载荷位于 Flash 偏移 `0x150000` 处。常规提取工具（binwalk / unsquashfs）均无法识别，其原因在于原厂在 U-Boot 中内置了专有加密解密层：

* **加密算法**：**128-bit TEA (Tiny Encryption Algorithm)** 标准分组解密
  * 解密常量（Delta）：`0x9E3779B9`
  * 初始轮数常数：`0xC6EF3720`（32 轮迭代）
* **硬编码密钥**：直接明文存储在 U-Boot 数据段：
  * Key 字符串：`"baudrate=115200\0"`（16 字节）
  * 对应 32-bit 字：`0x64756162, 0x65746172, 0x3531313d, 0x00303032`
* **载荷真实格式**：
  * 解密后前 64 字节为标准 uImage 头（Type 3: RAMDISK，大小 0xfee000 = 16,703,488 字节）；
  * uImage 载荷为标准的 **CramFS**（魔数 `0x28cd3d45`）。
* **提取脚本**：仓库的 `src/` 目录下提供了 `tea_iroot.c` 和 `uncramfs.mjs`（纯 Node.js 24 实现），编译运行后可快速解密并解压出全部 rootfs 文件树。

---

## 四、 核心风扇控制体系与调速原理深度剖析

通过对 `libipmimsghndlr.so.2.368.0` 和底层库的全量反汇编，我们逆向还原了浪潮风扇自动调速的核心引擎：

### 1. 软件架构层级
* **硬件驱动**：`/lib/modules/generic/misc/pwmtach.ko` 与 `pwmtach_hw.ko`（注册 `/dev/pwmtach`）。
* **底层接口库**：`/usr/local/lib/libpwmtach.so.1.5.0`（封装 `set_pwm_dutycycle`，直接操作 AST2300 寄存器）。
* **核心守护进程**：`/usr/local/bin/IPMIMain`
* **核心控制线程**：`libipmimsghndlr.so.2.368.0` 中的常驻后台线程 `InspurFanControlTask`（虚拟地址 `0x0006aab0`）。

### 2. 数据表 `fan_sensor_contol_table` 的真实结构（0x79d68）
反汇编证实，`.data` 段中的 `fan_sensor_contol_table`（文件偏移 `0x79d68`，虚拟地址 `0x81d68`）**并不是温度-转速映射曲线表**，它由三个部分拼接而成：
1. **传感器注册名单（0x00 ~ 0x23，共 36 字节）**：
   三元组数组 `[Type, SensorID, LUN]`，用于传给 `updateSensorValue`（`0x6a860`）告知 BMC 需要轮询读取哪些传感器（如 CPU0=`0x19`, CPU1=`0x1a`, 进风口=`0x03`, PCH=`0x09`, FanTach=`0x30/0x32/0x34/0x36`）。
2. **运行时历史状态缓存（0x24 ~ 0x47，共 32 字节）**：
   8 个 32-bit 整型，用于保存上一次各 Zone 的 PWM 状态。
3. **PID Clamp 参数结构体 `Fan_Clamp`（0x48 ~ 0xAB，即 0x81db0 处）**：
   定义了 5 组 PID 限制参数：`{ float Kp=3.0f, float Ki=0.0f, float Max=100.0f, uint32_t Min=30, uint32_t Reserved=0 }`。

### 3. 双算法并行竞争决策模型（Max-Policy）
风扇最终输出由两种算法在后台同时计算，并无条件选取两者中的**最大值（`max()`）**：

* **算法 A（硬编码分段线性算式 `---line---`，位于 `0x69db8`）**：
  **完全由 ARM 算术机器码硬编码**，不是查表！
  ```arm
  0x69e20: lsl  r0, r6, #2        ; r0 = Temp * 4
  0x69e28: sub  r0, r0, #0x5f     ; PWM = Temp * 4 - 95
  0x69e34: lsl  r2, r6, #1        ; r2 = Temp * 2
  0x69e38: add  r0, r2, #5        ; PWM = Temp * 2 + 5
  0x69e78: sub  r0, r6, #0x32     ; PWM = Temp - 50
  0x69e7c: sub  r0, r6, #0x1e     ; PWM = Temp - 30
  0x69f28: add  r0, r6, r6, lsl #1; r0 = Temp * 3
  0x69f30: sub  r0, r0, #0x2d     ; PWM = Temp * 3 - 45
  ```
  末端硬编码钳位 `[10%, 100%]`。
* **算法 B（PID 增量动态调节 `---clamp---`，位于 `0x6a084`）**：
  目标温度硬编码在指令中（CPU 为 65℃，内存为 58℃）。超温时根据 `Fan_Clamp` 的 `Kp = 3.0` 进行 PID 浮点超额补偿。
* **决策合并**：
  在 `0x6b7b4`、`0x6b8b8`、`0x6ba4c` 处执行 `cmp r0, r7; movge r7, r0`，即无条件取两者最大值：
  ```text
  Target_PWM = max(Line_PWM, Clamp_PWM)
  ```
  并在初始化阶段为所有通道硬编码了初始下限值 `0x1E`（30% 怠速）和上限值 `0x50`（80%）：
  ```arm
  0x6abbc: mov r1, #0x1e    ; Fan 0 初始下限 30%
  0x6abec: mov ip, #0x1e    ; Fan 1 初始下限 30%
  0x6ac1c: mov r7, #0x1e    ; Fan 2 初始下限 30%
  ... (Fan 3~7 均相同)
  ```
**这就是即便待机温度极低，风扇转速也绝对无法降到 30% 以下的根源。**

---

## 五、 空风扇位防暴走机制与安全禁用风扇方案

### 1. 为什么机箱实际仅 0/2/4/6 有风扇，1/3/5/7 为空却不触发失速暴走（`FanTroubled`）？
浪潮 SA5212M4 是 2U 机架服务器，设有 4 个物理风扇模组插槽（Slot 1 ~ 4）。AST2300 BMC 预留了双转子通道：
* Slot 1 对应 Fan 0（前）/ Fan 1（后）
* Slot 2 对应 Fan 2（前）/ Fan 3（后）
* Slot 3 对应 Fan 4（前）/ Fan 5（后）
* Slot 4 对应 Fan 6（前）/ Fan 7（后）

当配备单转子风扇时，只有偶数通道（0/2/4/6）工作，奇数通道物理悬空。BMC 之所以不报 0 RPM 失速暴走（拉升至 100% 全速），是因为固件的三层协同过滤：

1. **`libipmipdk.so.1.44.0` 中的硬件存在性配置表 `g_FanConfig`（偏移 `0x2512c + 0x1c`）**：
   ```c
   uint32_t g_FanConfig[7] = {
       1, // Fan 0: 1 (已配置 / 存在)
       0, // Fan 1: 0 (未配置 / 空)
       1, // Fan 2: 1 (已配置 / 存在)
       0, // Fan 3: 0 (未配置 / 空)
       1, // Fan 4: 1 (已配置 / 存在)
       0, // Fan 5: 0 (未配置 / 空)
       1  // Fan 6: 1 (已配置 / 存在)
   };
   ```
   函数 `PDK_GetFanStatus(fan_id)`（`0x1bb00`）首先检查此表。若值为 0，**直接跳到 `0x1bc6c` 返回状态码 2（`DEVICE_NOT_PRESENT`），彻底短路跳过后续所有的测速计读取与故障判定**！
2. **`libipmimsghndlr.so.2.368.0` 中的传感器轮询白名单**：
   在 `fan_sensor_contol_table`（`0x79d68`）中，仅登记了测速计 `0x30`(Fan0)、`0x32`(Fan2)、`0x34`(Fan4)、`0x36`(Fan6)。`updateSensorValue` 周期采集时根本不向硬件查询奇数风扇。
3. **`get_fan_status`（`0x6c25c`）健康监控循环**：
   代码逻辑硬编码只遍历 0/2/4/6。当调用 `PDK_GetFanStatus` 返回 2 时，直接跳过（`beq #0x6c2a8`），**绝不累加丢速错误计数器，绝不置位 `FanTroubled` 标志**。

### 2. 安全禁用现存风扇（如拔掉/停用 Fan 6）的实施方案
若要主动禁用 0/2/4/6 中的某一个风扇且不触发暴走，只需复制原厂对 1/3/5/7 的处理方式：
1. **修改 `libipmipdk.so` 的 `g_FanConfig`**：
   将 `0x2512c + 0x1c + (目标风扇ID * 4)` 处的 `01 00 00 00` 修改为 `00 00 00 00`（例如禁用 Fan 6，修改 `0x25160` 处）。
2. **同步在 `fan_sensor_contol_table` 中剔除测速计**：
   在 `libipmimsghndlr.so` 的 `0x79d68` 表中将对应风扇的 Tach 条目（如 `04 36 ff`）抹去。
修改后该风扇插槽将被 BMC 视为“未配置插槽”，即使停转也不会引发任何告警。

---

## 六、 SSH 远程登录直接启用 Bash 方案

### 1. 默认 SSH 登录无法进入 Shell 的原因
查看 `/etc/passwd`：
```text
sysadmin:x:0:0:sysadmin:/root:/usr/local/bin/defshell
```
深入反汇编 `/usr/local/bin/defshell` 发现：
1. 若来自物理串口 `console`，直接运行 `/bin/sh`；
2. 若来自 SSH 虚拟终端（pts），它会强制读取配置文件 **`/conf/default_sh`**；
3. **若未找到配置，它输出 `Configuration %s is not found` 并强制执行 `/bin/false`**，导致 SSH 连接被立刻掐断！

### 2. 固件级解锁方案（刷机永久生效）
解包 rootfs 后进行如下修改：
1. **修改 `/etc/passwd`**：将登录 shell 改为系统自带的 BusyBox shell：
   ```text
   sysadmin:x:0:0:sysadmin:/root:/bin/sh
   root:x:0:0:root:/root:/bin/sh
   ```
2. **创建 bash 别名软链接**：
   ```bash
   cd bin && ln -s busybox bash
   ```
3. **修改默认密码**：
   查看 `/etc/defconfig/shadow`，原厂默认密码哈希为 `sysadmin:$1$A17c6z5w$fjBLueH75zrBTl8Ujoylu1:2:0:99999:7:::`。
   可替换为自选密码的 MD5 哈希（例如 `admin` 的哈希为 `$1$admin$e.vV46XwW8w7O4WvA9Wz7/`），或置空密码实现免密。
4. **旁路 defshell**：
   直接用 `/bin/sh` 覆盖或软链接替换 `/usr/local/bin/defshell`。

### 3. 在线免刷固件解锁（利用可读写 `/conf` 分区）
在可写分区 `/conf` 中建立文件 `/conf/default_sh`，内容为：
```ini
[default]
default_shell = /bin/sh
```
原厂 `defshell` 检测到此文件后将直接放行进入 `/bin/sh`。

---

## 七、 Web 控制台 HTTPS 强制机制与 SSL 证书安全替换方案

### 1. Web 是否强制 HTTPS 重定向？
**结论：默认强制重定向至 HTTPS。**
* 守护进程 `/usr/local/bin/webgo`（基于 GoAhead WebServer）虽然同时监听 HTTP（80 端口）与 HTTPS（443 端口）。
* 但代码中开启了全局安全跳转标志 `g_AlwaysSecure`（VA `0x0003c438`）。在请求分发逻辑（VA `0x19078`）中，当收到来自 80 端口的非加密 HTTP 请求时，无条件调用 `websRedirectAlwaysSecure`（VA `0x12c40`），使用 `https://%s/%s` 格式化目标 URL 并回传 HTTP 302 临时重定向响应报文。
* 因此，所有对 HTTP 端口的访问都会被自动强制收敛至 HTTPS。

### 2. 证书加载路径与回退优先级
通过对 `webgo` 的 SSL 上下文初始化函数（VA `0x2a15c ~ 0x2a470`）进行深度反汇编：
```arm
0x2a20c: mov r0, #3
0x2a210: bl  __xstat                     ; 检查 /etc/actualcert.pem 是否存在
0x2a214: ldr r6, =server.pem            ; 系统预设证书
0x2a218: ldr r1, =/etc/actualcert.pem   ; 用户自定义证书
0x2a220: ldr r3, =/etc/actualprivkey.pem; 用户自定义私钥
0x2a224: ldr r8, =privkey.pem           ; 系统预设私钥
0x2a228: cmp r0, #0
0x2a230: moveq r6, r1                   ; 若 /etc/actualcert.pem 存在，加载 actualcert.pem
0x2a240: moveq r8, r3                   ; 若存在，加载 actualprivkey.pem
```

* **双层优先级设计**：
  * **第一优先（用户配置）**：系统首先检测 `/etc/actualcert.pem`（软链接至 `/conf/actualcert.pem`）。若存在，则优先加载 `/conf/actualcert.pem` 与 `/conf/actualprivkey.pem`。
  * **第二优先（原厂兜底）**：若上述文件不存在，自动回退加载 `/usr/local/www/certs/server.pem` 与 `/usr/local/www/certs/privkey.pem`。
* **安全校验防崩溃机制**：
  加载私钥后，`webgo` 立即调用 OpenSSL 标准 API `SSL_CTX_check_private_key(ctx)`（VA `0x2a298`）核验私钥与公钥证书是否成对匹配。若不匹配，仅在日志报警并终止 SSL 模块，不会导致系统死锁。

### 3. 在线免刷固件替换证书操作指引（推荐）
得益于 `/conf` 是断电不丢失的可读写 JFFS2 闪存分区，无需重新打包烧录固件，可通过 SSH 在线完成证书替换：

1. **证书与私钥格式规范**：
   * **证书格式**：标准 PEM 格式（Base64 编码，`-----BEGIN CERTIFICATE-----`）。若包含中级 CA 证书，可直接合并为一个文件（服务器证书在前，中间证书在后）。
   * **私钥要求**：无密码保护（No Passphrase，如 `-----BEGIN RSA PRIVATE KEY-----`）。推荐 **RSA 2048 位**（AST2300 的 ARM926 芯片算力较弱，建议避免 4096 位高强度密钥，以免 SSL 握手导致 CPU 负载飙升）。
2. **部署并写入 `/conf`**：
   ```bash
   # 写入证书与私钥
   cat your_cert.pem > /conf/actualcert.pem
   cat your_key.pem  > /conf/actualprivkey.pem

   # 设置安全文件权限
   chmod 644 /conf/actualcert.pem
   chmod 600 /conf/actualprivkey.pem

   # 重启 Web 服务生效
   /etc/init.d/webgo.sh restart
   ```
3. **回滚方案（安全兜底）**：
   若证书损坏或配置错误导致 Web 无法访问，只需 SSH 登录删除 `/conf/actualcert.pem`，重启 `webgo` 即可瞬间回退至原厂预设自签证书。

---

## 八、 智能温控曲线、PWM硬件映射与实时接管深度方案

### 1. 方案选型评估
由于原厂温度曲线完全硬编码在 `0x69db8` 指令中，仅修改数据表无法自定义曲线。项目经过持续迭代，实现了最平滑、最安全的生产级接管方案：

* **完整调速线程接管与平滑静音算法移植（`src/fan_control_hook.c`）**：
  直接替换 `libipmimsghndlr.so.2.368.0` 中的 `InspurFanControlTask`（虚拟地址 `0x0006aab0`）线程入口。
  * **完整移植温控算法 (与 `setfanlevel.sh` 精确对齐)**：
    - **6 档阶梯基准温控曲线**：`<58C: 18%`, `58-69C: 20%`, `70-72C: 22%`, `73-78C: 28%`, `79-84C: 40%`, `>=85C: 70%`；
    - **静音缓降（Quiet Ramp-Down）**：`<66C` 时每个周期步降 1%，避免风扇突然减速造成的风噪声落差；
    - **平滑爬升（Smooth Ramp-Up）**：`68C~72C` 之间以 2% 步长温和升速，消除风扇啸叫突变；
    - **高温应急直控（Direct Curve）**：`>72C` 立即直达曲线目标转速，确保高负载高热时的绝对硬件安全；
    - **死区滞留（Deadband Hold）**：`66C~68C` 保持当前转速不变；
    - **迟滞防震荡（Hysteresis = 2C）**：降档需达到 `(下限温度 - 2C)`，彻底消除温控阈值边界处的转速反复横跳；
    - **从属联动转速差（fan_aux_offset = 15%）**：两 CPU 温度相近时，Fan 0 与 Fan 4 严格保持 `Fan 2 - 15%` 转速差（常温怠速仅 3%~5%，最低 1%），大幅消减机箱风噪；
    - **Fan 6 恒定 1% 底速锁定**：辅助出风通道不随温度提速，严格恒定为 1% 静音底速 (~1440 RPM)；
    - **看门狗保活喂狗与 CPLD 心跳**：循环内每秒向 `/var/pipe/watchdogQ` 写入保活数据包，并保持 GPIO 65 CPLD 心跳同步，防止 `watchdogapp` 误判重启；
    - **JFFS2 配置文件热重载**：支持读取并监听 `/conf/fan_control.conf`（详细参数手册见 [docs/FAN_CONTROL.md](file:///home/aimee/SA5212M4_BMC/docs/FAN_CONTROL.md)，简洁模板见 [src/fan_control.conf](file:///home/aimee/SA5212M4_BMC/src/fan_control.conf)），参数修改保存后 5 秒内自动热生效，无需重启 BMC 或重刷固件！
  * **交叉编译与兼容性保障**：
    - 针对 AST2300 ARM926EJ-S（ARMv5TE）优化编译，生成 SYSV `.hash` 节区；
    - 强制 32 位时间格式（`-U_TIME_BITS -D_TIME_BITS=32`），消除 GLIBC_2.34+ 依赖，全符号兼容 `GLIBC_2.4`。

### 2. ASPEED AST2300 硬件 PWM 通道拓扑映射实证

通过对底层硬件支持库 `libipmipdk.so.1.44.0` 反汇编，逆向还原了 `PDK_GetFanPWMNo(int fan_id)` 的物理映射路由。浪潮 SA5212M4 主板上的风扇插槽编号与 AST2300 驱动物理 PWM 通道具有明确对应关系：

| 逻辑风扇名称 | 插槽编号 (Fan ID) | AST2300 硬件 PWM 通道 (PWM No) | 硬件风道布局与负责区域 | 极值安全转速限制与联动规则 |
| :--- | :--- | :--- | :--- | :--- |
| **Fan 0** | `0` | **PWM 5** | 进风辅助区 / CPU1 直吹 | 联动 `Fan 2 - 15%`，允许降至 **1%** (待机 3%~5%) |
| **Fan 2** | `2` | **PWM 1** | CPU1 / 主核心散热风道 | **中置主力风扇**，允许降至 **6%**（基准怠速 18%） |
| **Fan 4** | `4` | **PWM 0** | CPU0 / 内存主散热风道 | 联动 `Fan 2 - 15%`，允许降至 **1%** (待机 3%~5%) |
| **Fan 6** | `6` | **PWM 4** | 扩展卡 / 电源辅助出风 | **恒定锁定 1%** 静音底速 (~1440 RPM) |

> [!IMPORTANT]
> 在 `src/fan_control_hook.c` 中，不仅完整导出了 `PDK_GetFanPWMNo` 符号供其他 IPMI 模块查询，并在底层通过 `get_pwm_for_fan(fan_id)` 实现了精确到每个硬件通道的独立写值，彻底解决误写通道导致风扇不响应或常开 100% 的隐患。

### 3. Web 后台与 IPMI 实时无缝模式接管 (Real-Time Auto Takeover)

#### (1) 痛点分析
在原厂架构中，用户在 Web 控制台将风扇模式从 Manual（手动）切换回 Auto（自动）时，由于原厂后台线程存在 5~10 秒的轮询周期及内部转速缓存滞后，前端页面刷新时往往仍显示旧的手动转速，造成“切回自动后不更新转速”的假象。

#### (2) 逆向突破与实现机制
逆向发现，Web 前端发起的模式切换由 AMI MegaRAC 的 IPMI OEM Cmd `0x7A` 处理，底层最终调用 `inspur_set_fan_control_mode(int mode)`：
* `mode = 0`：自动模式（Auto）
* `mode = 1`：手动模式（Manual）

在 Hook 库中，我们直接拦截并导出：
* `InspurSetFanControlMode` (IPMI OEM Cmd 0x7a)
* `InspurGetFanControlMode` (IPMI OEM Cmd 0x7b)
* `InspurSetFanPwmDuty` (IPMI OEM Cmd 0x78)
* `inspur_set_fan_control_mode` / `inspur_get_fan_control_mode`

**实时接管技术**：
当收到 `mode = 0`（切回自动模式）时：
1. 立即同步清空 `g_fan0_speed`、`g_fan2_speed`、`g_fan4_speed`、`g_fan6_speed` 及全部迟滞状态缓存；
2. 在当前请求处理线程中**同步、即刻（0ms 延迟）**触发一次温度轮询与硬件 PWM 计算写入；
3. 将最新转速写回共享内存状态数组 `g_inspur_fan_state`。

**效果**：当 Web 前端在点击“Save”后刷新页面，获取到的风扇转速**立竿见影变为最新计算的自动平滑转速 (Fan2=18%, Fan0/4=3%, Fan6=1%)**，真正做到零感知、零延迟接管。

### 4. 转速下限突破与防报警 Tachometer 滤波
* **Fan 2 怠速 18%，下限放宽至 6%**：相比原厂硬编码的 30% 怠速，噪音直降 20dB 以上。
* **Fan 0/4 联动 `Fan 2 - 15%`，下限放宽至 1%**：常温待机仅 3%，高负载按需平滑提升。
* **Fan 6 恒定 1% 静音底速**：消除原厂对空置或辅助通道的啸叫拉高。
* **防报警滤波**：拦截 `PDK_GetFanConfig` 与 `PDK_GetFanStatus`，对未插装或低转速风扇实施精准状态返回，杜绝 BMC 误触发风扇告警或红灯报警。

---

## 九、 固件刷写指南：Web 控制台升级与 UEFI Shell 极速刷写

### 1. 两种刷写方式对比

修改并重打包后的固件完全支持以下两种刷写方式，均无需拆机或外部编程器：

| 刷写方式 | 所需时间 | 刷写工具 | 适用场景 | 优势与风险 |
| :--- | :--- | :--- | :--- | :--- |
| **方式 A：BMC Web 控制台升级** | 3 ~ 5 分钟 | 浏览器上传 32MB 固件 | 日常维护、远程升级 | **最安全**。U-Boot 写保护硬件级防砖，自动保留现有网络与配置。 |
| **方式 B：UEFI Shell 极速刷写** | **约 10 秒** | `socflash.efi` (U盘启动) | 本地维护、极速迭代 | **极速省时**。仅擦写 16MB RootFS，瞬时完成。 |

### 2. 方式 A：BMC Web 控制台在线升级详细步骤

1. 打开浏览器登录 BMC Web 管理页面；
2. 导航至：**维护 (Maintenance) -> 固件更新 (Firmware Update)**；
3. 点击“进入更新模式 (Enter Update Mode)”；
4. 选择并上传 `SA5212M4_BMC_4.35.0_Standard_Custom.bin`（32MB 全量镜像）；
5. 点击 **Verify (校验)**：升级程序将核验 FMH 分区表与 CRC32，校验通过后显示目标固件版本为 `4.35.0`；
6. 确认开始刷写，等待进度条走完且 BMC 自动重启生效。现有网络 IP 与密码配置自动保留。

### 3. 方式 B：UEFI Shell 极速离线刷写详细步骤 (推荐本地维护)

利用工程生成的 `uefi_flash_pack.zip`：
1. 将 `uefi_flash_pack.zip` 解压到 FAT32 格式 U 盘根目录（内含 `socflash.efi`, `boot.bin`, `iroot_payload.bin`, `flash_rootfs.nsh`, `flash_full.nsh`）；
2. 将 U 盘插入服务器，开机按 `F11` 或 `Del` 键，选择进入 **UEFI Built-in Shell**；
3. 切换到 U 盘所在盘符（输入 `fs0:` 或 `fs1:` 回车，输入 `ls` 确认文件）：
   ```efi
   fs0:
   ls
   ```
4. **10 秒极速刷写 (推荐：仅更新系统 RootFS)**：
   运行 `flash_rootfs.nsh` 脚本：
   ```efi
   flash_rootfs.nsh
   ```
   该脚本通过 `socflash.efi` 精确将 `iroot_payload.bin` 写入 Flash 偏移 `0x150000`。无需擦除无关扇区，全程仅耗时约 10 秒！
5. **全量 32MB 刷写**：
   若需全量 32MB 完整重刷，可执行 `flash_full.nsh`。
6. 刷写成功后，服务器断电重启使 BMC 运行新定制固件。

---

## 十、 一键构建系统与 GitHub Actions 自动化 CI 流水线 (Node.js 24)

#### 1. Node.js 24 一键编译与打包主引擎 (`src/repack.mjs`)

项目提供了基于 **Node.js 24** 的高内聚固件全流程构建主引擎。本地一条命令即可自动完成所有步骤：
```bash
node src/repack.mjs
```
该引擎自动串联 8 大核心阶段：
1. **构建依赖检查**：自动检测 `gcc`, `arm-linux-gnueabi-gcc`, `fakeroot`, `mkfs.cramfs` 等（纯 Node.js 24 流程，无需 Python 或 Zip 扩展）；
2. **基准固件验证**：检测 32MB 基准固件 (`SA5212M4_BMC_4.35.0_Standard_20191025`)；
3. **主机加解密工具编译**：自动构建 `tea_iroot`（支持加解密一体化与别名机制）；
4. **RootFS 校验与权限配置**：还原 109 个 Linux 特殊设备节点，赋予 root/sysadmin `/bin/sh` Shell 与默认密码；
5. **AST2300 劫持库交叉编译与 ELF 校验**：编译 `libfanhook.so`（ARMv5TE, soft-float, SYSV hash, GLIBC_2.4），并校验动态符号；
6. **CramFS 封装与 128-bit TEA 加密**：生成 16MB uImage 格式 `iroot` 载荷；
7. **镜像合成与校验和修复**：写入 32MB Flash 镜像，保留原厂版本 `4.35.0`，自动修复 8 个 FMH Checksums 与整包 CRC32；
8. **UEFI 离线刷写包打包与 SHA-256 报告**：纯 Node.js 同步打包 `uefi_flash_pack.zip` 并输出各发布物 SHA-256 哈希。

### 2. GitHub Actions 自动化流水线 (`.github/workflows/repack.yml`)

仓库配置了针对该项目的 GitHub Actions 工作流：
* **触发机制**：**仅支持手动触发（`on: workflow_dispatch`）**。每次 `git push` 时**绝不**触发任何自动化运行，彻底杜绝无谓的 Runner 额度消耗；
* **运行环境**：采用最新 `ubuntu-latest` 搭配官方 `actions/setup-node@v4`（指定 `node-version: '24'`）；
* **构建产物 (Artifacts)**：构建成功后自动归档并在 GitHub Actions 页面提供 90 天有效期的发布包下载：
  - `SA5212M4_BMC_4.35.0_Standard_Custom.bin`（32MB 全量 Web 刷写固件）
  - `uefi_flash_pack.zip`（UEFI Shell 极速离线刷写包）

---

## 十一、 仓库结构与工具集说明

```
.
├── .github/
│   └── workflows/
│       └── repack.yml                         # GitHub Actions 手动编译打包工作流 (Node 24)
├── README.md                                  # 全流程技术分析与实施文档
├── docs/
│   └── FAN_CONTROL.md                         # 自定义精细静音调速完整技术规范与参数配置手册
├── uefi_flash_pack/                           # UEFI Shell 刷写包工程模板
│   ├── boot.bin                               # ASPEED 引导微码
│   ├── socflash.efi                           # ASPEED UEFI 刷写程序
│   ├── flash_rootfs.nsh                       # 10秒快速刷写脚本 (仅写 RootFS)
│   └── flash_full.nsh                         # 全量 32MB 刷写脚本
└── src/
    ├── Makefile                               # 交叉编译构建脚本 (ARMv5TE, SYSV hash, glibc 2.4/2.11 兼容)
    ├── repack.mjs                             # Node.js 24 驱动的一键编译、打包与校验全流程引擎 (纯 Node.js 实现)
    ├── uncramfs.mjs                           # 纯 Node.js 24 实现的 CramFS 镜像解析与解包工具
    ├── fan_control_hook.c                     # 完整温控接管、PWM通道路由与实时接管 Hook 源码 (生产级)
    ├── fan_control.conf                       # 可修改温控配置文件 (单行极简英文注释，支持热重载)
    ├── libfanhook.so                          # 交叉编译完成的 AST2300 动态库
    ├── tea_iroot.c                            # 128-bit TEA 加解密二合一 C 源码 (含 uImage 校验)
    ├── decrypt_iroot -> tea_iroot             # 软链接快捷工具
    └── encrypt_iroot -> tea_iroot             # 软链接快捷工具
```
