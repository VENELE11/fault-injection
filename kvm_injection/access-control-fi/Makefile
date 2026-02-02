# 修复后的 Makefile，对应 resource.c 和 resource-main.c
obj-m := resource.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# 递归编译子目录
DIRS = memory-load

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o access-load resource-main.c
    for i in $(DIRS); do \
        (cd $$i && echo "make $$i" && $(MAKE)) || exit 1; \
    done

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f access-load
    for i in $(DIRS); do \
        (cd $$i && echo "cleaning $$i" && $(MAKE) clean) || exit 1; \
    done