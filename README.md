# Inspur SA5212M4 BMC AI-Crafted Silent Firmware

[English](README.md) | [中文说明](README_CN.md)

This project provides an **AI-reverse-engineered and customized silent BMC firmware** for Inspur SA5212M4 rackmount servers.

It is designed to eliminate the severe, persistent fan noise caused by the aggressive factory fan control policy in standard BMC firmware (4.35.0). By dynamically intercepting the ASPEED AST2300 hardware PWM control layer via a lightweight C hook (`libfanhook.so`), this firmware delivers closed-loop thermal regulation, bringing idle noise down to whisper-quiet levels (**30~35 dB**). The project also features an automated, pure Node.js 24 toolchain for zero-dependency compilation, extraction, cryptographic signing, and fast flashing.

---

## ✨ Key Features

- 🔇 **Intelligent Hardware Fan Hook**: Intercepts `libipmimsghndlr.so` at the hardware register level, bypassing the factory 40%+ minimum duty cycle limitation. Idle fan speed is smoothly regulated down to **16% / 6%**, reducing acoustic noise by more than 80%.
- ⚙️ **Matrix-Based Thermal Curves & Hot-Reloading**: Configurable via a compact array syntax (`/etc/fan_control.conf`), featuring multi-step thermal ramps, smooth ramp-up/down deltas, hysteresis anti-hunting protection, and real-time hot-reloading.
- ⚡ **Pure Node.js 24 Toolchain**: Completely eliminates legacy Python and shell script dependencies. Uses native Node.js 24 for CramFS extraction, 109 device node reconstruction under fakeroot, 128-bit TEA cryptography, FMH checksum verification, and IEEE 802.3 CRC32 computation (`node src/repack.mjs`).
- 🚀 **10-Second UEFI Fast Flash**: Includes standalone UEFI Shell scripts for blazing-fast partial updates (flashing only the 16MB RootFS partition in ~10 seconds) without touching intact bootloader or sensor partitions. Full 32MB recovery is also supported.
- 🔓 **Full Root Shell Access**: Pre-configures `root:admin` and `sysadmin:admin` credentials with standard `/bin/sh` interactive shell access enabled out of the box.

---

## 🚀 Quick Start

### 1. Build and Repack the Firmware

Ensure standard build tools are installed (`gcc`, `arm-linux-gnueabi-gcc`, `fakeroot`, `mkfs.cramfs`):

```bash
# Place the official 32MB factory ROM at the repository root:
# SA5212M4_BMC_4.35.0_Standard_20191025

# Run the pure Node.js 24 repack pipeline
node src/repack.mjs
```

Upon completion, the pipeline generates:
- `SA5212M4_BMC_4.35.0_Standard_Custom.bin` (Complete 32MB SPI Flash image)
- `uefi_flash_pack.zip` (Ready-to-use UEFI Shell flash package)

### 2. Fast Flashing via UEFI Shell (Recommended)

1. Extract all contents from `uefi_flash_pack.zip` into the root directory of a FAT32 USB drive.
2. Plug the USB drive into the server, power on, and press `F11` to enter the **UEFI Shell**.
3. Navigate to the USB filesystem (e.g., `fs0:`) and run the 10-second fast-flash script:
   ```efi
   flash_rootfs.nsh
   ```
4. Once `Flash completed successfully!` is shown, power cycle or reset the server to apply the custom BMC firmware.

---

## 📚 In-Depth Documentation

To keep the repository root clean, comprehensive technical documentation is maintained under `docs/`:

- 📖 **[Deep Firmware Reverse Engineering & Architecture Analysis (Detailed Technical Report)](docs/README.md)**: Full disassembly analysis of Flash partition layout, 128-bit TEA key extraction, FMH checksum algorithms, and assembly-level Hook architecture.
- ⚙️ **[Fan Control Parameters & Matrix Configuration Manual](docs/FAN_CONTROL.md)**: Detailed explanation of `fan_control.conf`, thermal curve matrix syntax, PWM mapping, and tuning guidelines.

---

## 📁 Repository Structure

```
.
├── .github/workflows/repack.yml    # GitHub Actions manual workflow (pure Node.js 24)
├── README.md                       # Project overview & quick start (English)
├── README_CN.md                    # Project overview & quick start (Chinese)
├── docs/
│   ├── README.md                   # Full reverse engineering & architectural analysis
│   └── FAN_CONTROL.md              # Fan control configuration & tuning guide
├── uefi_flash_pack/                # UEFI Shell offline flashing suite
│   ├── boot.bin                    # ASPEED boot microcode
│   ├── socflash.efi                # ASPEED native UEFI flash utility
│   ├── flash_rootfs.nsh            # 10-second fast flash script (RootFS only)
│   └── flash_full.nsh              # 32MB full recovery flash script
└── src/
    ├── Makefile                    # AST2300 cross-compilation rules (ARMv5TE, SYSV hash)
    ├── repack.mjs                  # Pure Node.js 24 build, repack & checksum pipeline
    ├── uncramfs.mjs                # Pure Node.js 24 CramFS extractor
    ├── fan_control_hook.c          # Thermal regulation & PWM routing C hook source
    ├── fan_control.conf            # Compact configuration file with hot-reload support
    ├── libfanhook.so               # Pre-compiled AST2300 shared library
    └── tea_iroot.c                 # 128-bit TEA encrypt/decrypt & uImage generator
```

---

## ⚠️ Disclaimer

This project is created through reverse engineering of official firmware for academic research, noise reduction, and homelab use. Flashing server firmware carries inherent hardware risks. Always keep a backup of your original SPI Flash ROM prior to flashing.
