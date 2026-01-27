obj-m := pt-update-fi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o pt-update pt-update-fi-main.c

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f pt-update