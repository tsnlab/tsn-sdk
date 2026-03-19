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

topdir := $(shell cd $(src)/.. && pwd)

TARGET_MODULE:=xdma

uname_m := $(shell uname -m)

ccflags-y := -I$(topdir)/include $(XVC_FLAGS)
ifeq ($(uname_m),x86_64)
	ccflags-y += -msse2
else
	ccflags-y += -march=armv8-a+simd -D__LIBXDMA_RPI__
	KBUILD_CFLAGS := $(filter-out -mgeneral-regs-only, $(KBUILD_CFLAGS))
	KBUILD_CFLAGS += -ffreestanding -isystem $(shell $(CC) -print-file-name=include)
endif

ifeq ($(DEBUG),1)
	ccflags-y += -D__LIBXDMA_DEBUG__
endif
ifneq ($(config_bar_num),)
	ccflags-y += -DXDMA_CONFIG_BAR_NUM=$(config_bar_num)
endif
#ccflags-y += -DINTERNAL_TESTING

ifneq ($(KERNELRELEASE),)
	$(TARGET_MODULE)-objs := libxdma.o xdma_cdev.o cdev_ctrl.o cdev_events.o cdev_sgdma.o cdev_xvc.o cdev_bypass.o xdma_mod.o xdma_thread.o xdma_netdev.o alinx_ptp.o alinx_arch.o tsn.o
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
