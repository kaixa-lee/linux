# SPDX-License-Identifier: GPL-2.0-only
#
# Makefile for the Dallas's 1-wire bus.
#

obj-$(CONFIG_W1)	+= wire.o
wire-objs		:= w1.o w1_int.o w1_family.o w1_netlink.o w1_io.o

obj-y			+= masters/ slaves/

KERN_DIR = ~/imx/linux-imx
MODULE_DIR = $(shell pwd)

module:
	make -C $(KERN_DIR) M=$(MODULE_DIR) modules

clean:
	rm *.o *.mod.* *.symvers *.ko masters/*.ko masters/.*.cmd slaves/*.ko slaves/.*.cmd .*.cmd .*.o.d

