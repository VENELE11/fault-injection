obj-m := kvm-state-fi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o state-load kvm-state-fi-main.c
clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f state-load