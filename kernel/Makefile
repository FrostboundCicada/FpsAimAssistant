obj-m += aim_touch.o

aim_touch-objs := touch_driver.o

# 内核源码树路径，构建时通过 KDIR 变量传入:
#   make KDIR=/path/to/kernel-source M=$(pwd) modules
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	insmod aim_touch.ko
	chmod 666 /dev/aim_touch

uninstall:
	rmmod aim_touch
