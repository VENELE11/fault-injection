obj-m := kswapd-fi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o kswapd-load kswapd-fi-main.c

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f kswapd-load