obj-m := file-read-fi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o load_fi file-write-fi-main.c

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f load_fi