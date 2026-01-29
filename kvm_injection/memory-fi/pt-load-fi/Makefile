obj-m := pt-load-fi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o pt-load pt-load-fi-main.c

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f pt-load