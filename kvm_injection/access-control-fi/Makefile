# 1. 编译当前目录的 access-fi.c (内核模块) 和 access-fi-main.c (工具)
obj-m := access-fi.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# 2. 递归编译子目录 memory-load
DIRS = memory-load

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o access-load access-fi-main.c
    for i in $(DIRS); do \
        (cd $$i && echo "make $$i" && $(MAKE)) || exit 1; \
    done

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f access-load
    for i in $(DIRS); do \
        (cd $$i && echo "cleaning $$i" && $(MAKE) clean) || exit 1; \
    done