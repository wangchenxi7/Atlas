MODULE = bks_drv

WORK_DIR := $(shell dirname $(abspath $(lastword $(MAKEFILE_LIST))))
RSWAP_DIR := ${WORK_DIR}/../remoteswap/client

ifneq ($(KERNELRELEASE),)
obj-m  := $(MODULE).o
$(MODULE)-y := bks.o
$(MODULE)-y += bks_ioctl.o

ccflags-y := -g -Wall -Werror -Wno-unused-function -Wno-declaration-after-statement \
	-I$(RSWAP_DIR) -I$(WORK_DIR)/../include

else

ifeq ($(KDIR),)

DEFAULT_KDIR := /lib/modules/$(shell uname -r)/build

KDIR := $(DEFAULT_KDIR)

endif 

export KBUILD_EXTRA_SYMBOLS=$(RSWAP_DIR)/Module.symvers
ifeq ("$(wildcard $(KBUILD_EXTRA_SYMBOLS))","")
$(error $(KBUILD_EXTRA_SYMBOLS) does not exist, please make sure you have built the remoteswap client)
endif

all:
	$(MAKE) -C $(KDIR) M=$$PWD modules

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean

endif
