obj-m := cpu-reg.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o cpu-load cpu-reg-main.c

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f cpu-load