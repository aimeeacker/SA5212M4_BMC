# ==============================================================================
# 浪潮 SA5212M4 BMC 工具与劫持库编译构建 Makefile
# 目标硬件: ASPEED AST2300 (ARM926EJ-S / ARMv5TE, armel, soft-float, glibc 2.11)
# ==============================================================================

CC := arm-linux-gnueabi-gcc
READELF := arm-linux-gnueabi-readelf
STRIP := arm-linux-gnueabi-strip

# 固件解包后的目标根文件系统 (提供 glibc 2.11 库文件与系统头文件)
SYSROOT ?= $(abspath $(CURDIR)/../rootfs)

# 编译参数:
# 1. 强制 ARMv5TE 软浮点与 SYSV 传统 Hash 格式 (保证老版本 ld-linux.so 正常解析)
# 2. 禁用 64 位时间宏，强制使用 32 位时间类型 (-U_TIME_BITS -D_TIME_BITS=32)，防止引入 GLIBC_2.34+ 依赖
# 3. 严格指向固件 rootfs/lib 链接，确保 glibc 版本符号锁定在 GLIBC_2.4
CFLAGS = -shared -fPIC -O2 \
         -std=gnu99 -D__GLIBC_USE_ISOC2X=0 \
         -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
         -march=armv5te -mtune=arm926ej-s -marm -mfloat-abi=soft \
         -Wall -Wextra \
         -Wl,--hash-style=sysv \
         -U_TIME_BITS -D_TIME_BITS=32 -D_FILE_OFFSET_BITS=32 \
         --sysroot=$(SYSROOT) \
         -L$(SYSROOT)/lib

LDFLAGS = -ldl -lpthread

TARGET = libfanhook.so
SRC = fan_control_hook.c

all: $(TARGET) tea_iroot

$(TARGET): $(SRC)
	@echo "===> Compiling $(TARGET) for AST2300 (ARMv5TE, glibc 2.11 compatible)..."
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	@echo "===> Build successful: $@"

tea_iroot: tea_iroot.c
	@echo "===> Compiling tea_iroot host tool..."
	gcc -O2 $< -lz -o $@
	ln -sf tea_iroot encrypt_iroot
	ln -sf tea_iroot decrypt_iroot

encrypt_iroot: tea_iroot

decrypt_iroot: tea_iroot

verify: $(TARGET)
	@echo "======================================================="
	@echo "  Verifying $(TARGET) ELF Architecture & Compatibility"
	@echo "======================================================="
	@echo "[1] Checking CPU Architecture (Expected: v5TE):"
	$(READELF) -A $(TARGET) | grep "Tag_CPU_arch"
	@echo ""
	@echo "[2] Checking Hash Style (SYSV .hash section for old linker):"
	$(READELF) -S $(TARGET) | grep -E " \.hash "
	@echo ""
	@echo "[3] Checking Required GLIBC Versions (Must be GLIBC_2.4 - 2.11):"
	$(READELF) -V $(TARGET) | grep "Name: GLIBC_"
	@echo ""
	@echo "[4] Checking Dynamic Dependencies (DT_NEEDED):"
	$(READELF) -d $(TARGET) | grep "NEEDED"
	@echo ""
	@echo "[5] Checking Exported Hook Symbols:"
	$(READELF) --dyn-syms $(TARGET) | grep -E "InspurFanControlTask|set_pwm_dutycycle|PDK_GetFanConfig|PDK_GetFanStatus"
	@echo "======================================================="

clean:
	rm -f $(TARGET) tea_iroot encrypt_iroot decrypt_iroot

.PHONY: all clean verify
