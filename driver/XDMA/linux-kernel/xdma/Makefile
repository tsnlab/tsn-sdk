SHELL = /bin/bash
#
# optional makefile parameters:
# - DEBUG=<0|1>,	enable verbose debug print-out in the driver
# - config_bar_num=,	xdma pci config bar number
# - xvc_bar_num=,	xvc pci bar #
# - xvc_bar_offset=,	xvc register base offset
#
ifneq ($(xvc_bar_num),)
	XVC_FLAGS += -D__XVC_BAR_NUM__=$(xvc_bar_num)
endif

ifneq ($(xvc_bar_offset),)
	XVC_FLAGS += -D__XVC_BAR_OFFSET__=$(xvc_bar_offset)
endif

$(warning XVC_FLAGS: $(XVC_FLAGS).)

# A3 (2026-07-26): 신형 kbuild(6.x)는 $(src)가 상대경로라 topdir 산출이 깨져
# libxdma_api.h를 못 찾는다(E 풀빌드 실측; D는 구 오브젝트 잔재로 은폐).
# 외부모듈 빌드에선 KBUILD_EXTMOD(=M= 절대경로)를 우선 사용.
topdir := $(if $(KBUILD_EXTMOD),$(abspath $(KBUILD_EXTMOD)/..),$(shell cd $(src)/.. && pwd))

TARGET_MODULE:=xdma

uname_m := $(shell uname -m)

# A3: EXTRA_CFLAGS는 신형 kbuild(6.12)에서 더 이상 반영되지 않는다(실측:
# topdir가 옳게 풀려도 -I가 컴파일에 안 붙음) -> ccflags-y로 전환.
ccflags-y += -I$(topdir)/include $(XVC_FLAGS)
# FPU/SIMD 컴파일 플래그를 쓰지 않는다(2026-10-07). 종전 x86 의 -msse2 와 arm64 의 +simd 는
# 컴파일러가 IRQ·softirq·start_xmit 코드에서도 SSE/NEON 레지스터를 쓰게 했다. 커널은 이 문맥에서
# 사용자 FPU 상태를 저장하지 않으므로 같은 CPU 의 사용자 프로세스가 손상됐다. double 연산은
# 정수로 바꿨다(alinx_ptp.c, tsn.c). 커널 기본 플래그(-mno-sse 등 / -mgeneral-regs-only)를 쓴다.
ifneq ($(uname_m),x86_64)
	ccflags-y += -D__LIBXDMA_RPI__
endif

ifeq ($(DEBUG),1)
	ccflags-y += -D__LIBXDMA_DEBUG__
endif
ifneq ($(config_bar_num),)
	ccflags-y += -DXDMA_CONFIG_BAR_NUM=$(config_bar_num)
endif
#EXTRA_CFLAGS += -DINTERNAL_TESTING

ifneq ($(KERNELRELEASE),)
	$(TARGET_MODULE)-objs := libxdma.o xdma_cdev.o cdev_ctrl.o cdev_events.o cdev_sgdma.o cdev_xvc.o cdev_bypass.o xdma_mod.o xdma_thread.o xdma_netdev.o alinx_ptp.o alinx_arch.o tsn.o frer.o
	obj-m := $(TARGET_MODULE).o
else
	BUILDSYSTEM_DIR:=/lib/modules/$(shell uname -r)/build
	PWD:=$(shell pwd)
all :
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) clean
	@/bin/rm -f *.ko modules.order *.mod.c *.o *.o.ur-safe .*.o.cmd

install: all
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules_install
	depmod -A

endif
