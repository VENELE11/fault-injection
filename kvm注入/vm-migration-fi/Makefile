obj-m := vm-migration-fi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o mig-load vm-migration-fi-main.c

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f mig-load