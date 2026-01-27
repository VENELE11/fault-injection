# 1. 编译当前目录的 memory.c (内核模块) 和 memory-main.c (工具)
obj-m := memory.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# 2. 定义子目录
DIRS = kswapd-fi

all:
    # 编译当前目录模块
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o mm-fi-load memory-main.c
    # 递归编译子目录
    for i in $(DIRS); do \
        (cd $$i && echo "make $$i" && $(MAKE)) || exit 1; \
    done

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f mm-fi-load
    for i in $(DIRS); do \
        (cd $$i && echo "cleaning $$i" && $(MAKE) clean) || exit 1; \
    done