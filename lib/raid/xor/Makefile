# SPDX-License-Identifier: GPL-2.0

ccflags-y			+= -I $(src)

obj-$(CONFIG_XOR_BLOCKS)	+= xor.o

xor-y				+= xor-core.o
xor-y				+= xor-8regs.o
xor-y				+= xor-32regs.o
xor-y				+= xor-8regs-prefetch.o
xor-y				+= xor-32regs-prefetch.o

ifeq ($(CONFIG_XOR_BLOCKS_ARCH),y)
CFLAGS_xor-core.o		+= -I$(src)/$(SRCARCH)
endif

xor-$(CONFIG_ARM)		+= arm/xor.o
ifeq ($(CONFIG_ARM),y)
xor-$(CONFIG_KERNEL_MODE_NEON)	+= arm/xor-neon.o arm/xor-neon-glue.o
endif
xor-$(CONFIG_ARM64)		+= arm64/xor-neon.o arm64/xor-neon-glue.o
xor-$(CONFIG_CPU_HAS_LSX)	+= loongarch/xor_simd.o
xor-$(CONFIG_CPU_HAS_LSX)	+= loongarch/xor_simd_glue.o
xor-$(CONFIG_ALTIVEC)		+= powerpc/xor_vmx.o powerpc/xor_vmx_glue.o
xor-$(CONFIG_RISCV_ISA_V)	+= riscv/xor.o riscv/xor-glue.o
obj-y				+= tests/

CFLAGS_arm/xor-neon.o		+= $(CC_FLAGS_FPU)
CFLAGS_REMOVE_arm/xor-neon.o	+= $(CC_FLAGS_NO_FPU)

CFLAGS_arm64/xor-neon.o		+= $(CC_FLAGS_FPU)
CFLAGS_REMOVE_arm64/xor-neon.o	+= $(CC_FLAGS_NO_FPU)

CFLAGS_powerpc/xor_vmx.o	+= -mhard-float -maltivec \
				   $(call cc-option,-mabi=altivec) \
				   -isystem $(shell $(CC) -print-file-name=include)
