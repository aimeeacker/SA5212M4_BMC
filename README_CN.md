# 浪潮 SA5212M4 BMC AI 定制静音固件

[English](README.md) | [中文说明](README_CN.md)

本项目是由 **AI 深度逆向与重构** 的浪潮 (Inspur) SA5212M4 服务器 BMC 定制静音固件工程。

旨在解决官方原厂固件（4.35.0）风扇调速策略激进、起步转速过高、啸叫噪音巨大的痛点。通过对 ASPEED AST2300 底层 IPMI 守护进程实施动态库劫持（`libfanhook.so`），接管硬件 PWM 寄存器输出，实现毫秒级温控闭环与极低静音运行（日常负载噪音骤降至 30~35 dB），同时配套纯 Node.js 24 实现的固件解包、加解密与自动打包工具链。

---

## ✨ 核心特性

- 🔇 **智能硬件静音调速**：动态劫持 `libipmimsghndlr.so`，突破官方 40%+ 的下限限制，默认待机转速低至 **16% / 6%**，服务器噪音降低 80% 以上。
- ⚙️ **矩阵化温控曲线与热重载**：提供紧凑数组配置（`/etc/fan_control.conf`），支持动态多阶梯温控、步进平滑过渡、迟滞防频振与无感知热重载。
- ⚡ **纯 Node.js 24 工具链**：彻底摒弃 Python 与系统 Shell 脚本依赖，纯 Node.js 24 原生实现 CramFS 提取、109 特殊设备节点还原、128-bit TEA 加解密、FMH 校验和修复与 IEEE 802.3 CRC32 回填。
- 🚀 **10 秒极速 UEFI 刷写**：配套定制 UEFI Shell 刷写脚本，支持仅更新 16MB RootFS 分区（约 10 秒完成），无需擦写全量 32MB Flash。
- 🔓 **开放系统底层权限**：默认配置 `root:admin` 与 `sysadmin:admin` 账户，开放标准 `/bin/sh` 终端登录。

---

## 🚀 快速上手

### 1. 一键编译与固件打包

确保系统已安装基础编译环境（`gcc`, `arm-linux-gnueabi-gcc`, `fakeroot`, `mkfs.cramfs`）：

```bash
# 将官方 32MB 固件放置于根目录: SA5212M4_BMC_4.35.0_Standard_20191025
# 执行纯 Node.js 24 一键构建流水线
node src/repack.mjs
```

构建成功后将生成：
- `SA5212M4_BMC_4.35.0_Standard_Custom.bin`（32MB 全量 BMC 固件）
- `uefi_flash_pack.zip`（UEFI Shell 离线刷写压缩包）

### 2. UEFI Shell 极速刷写 (推荐)

1. 解压 `uefi_flash_pack.zip` 全部文件至 FAT32 格式 U 盘根目录；
2. 插入服务器启动项并按 `F11` 进入 **UEFI Shell**；
3. 切换至 U 盘分区（例如 `fs0:`），执行 10 秒极速刷写：
   ```efi
   flash_rootfs.nsh
   ```
4. 提示 `Flash completed successfully!` 后，重启服务器或冷断电即可生效。

---

## 📚 详细文档导航

为保持根目录简洁，完整的技术逆向细节与参数配置已归档至 `docs/`：

- 📖 **[固件深度逆向与技术架构全剖析 (详细技术文档)](docs/README.md)**：包含 Flash 分区布局、TEA 算法逆向、FMH 校验和规范、汇编级 Hook 原理等全部硬核技术解析。
- ⚙️ **[风扇温控参数与矩阵配置手册](docs/FAN_CONTROL.md)**：包含 `fan_control.conf` 矩阵语法、转速映射、各阶梯温度阈值与调优实战指南。

---

## 📁 仓库核心结构

```
.
├── .github/workflows/repack.yml    # GitHub Actions 手动触发构建流水线 (Node 24)
├── README.md                       # 项目概述与快速上手指南 (英文主文档)
├── README_CN.md                    # 项目概述与快速上手指南 (中文文档)
├── docs/
│   ├── README.md                   # 全流程深度逆向与底层技术剖析文档
│   └── FAN_CONTROL.md              # 风扇温控矩阵与参数配置手册
├── uefi_flash_pack/                # UEFI Shell 离线刷写工具包
│   ├── boot.bin                    # ASPEED 微码
│   ├── socflash.efi                # ASPEED UEFI 刷写程序
│   ├── flash_rootfs.nsh            # 10 秒快速刷写脚本 (仅 RootFS)
│   └── flash_full.nsh              # 32MB 全量恢复刷写脚本
└── src/
    ├── Makefile                    # 交叉编译规则 (ARMv5TE, SYSV hash, glibc 2.4/2.11)
    ├── repack.mjs                  # Node.js 24 固件全流程编译、打包与校验修复主引擎
    ├── uncramfs.mjs                # 纯 Node.js 24 实现的 CramFS 解析与解包工具
    ├── fan_control_hook.c          # 温控接管与 PWM 硬件路由 C 核心源码
    ├── fan_control.conf            # 单行极简英文注释温控配置文件 (支持热重载)
    ├── libfanhook.so               # 预编译 AST2300 兼容动态库
    └── tea_iroot.c                 # 128-bit TEA 加密/解密/uImage 二合一 C 源码
```

---

## ⚠️ 免责声明

本项目固件基于浪潮官方公开固件进行逆向分析与定制修改，仅供个人技术研究、静音改造与学习交流使用。刷写固件存在客观硬件风险，请确保在刷写前备份原始固件。
