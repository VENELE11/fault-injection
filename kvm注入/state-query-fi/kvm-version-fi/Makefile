obj-m := kvm-version-fi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o ver-load kvm-version-fi-main.c
clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f ver-load