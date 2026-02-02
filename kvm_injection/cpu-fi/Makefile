obj-m := cpu-reg.o
KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# 强制添加编译选项：忽略警告
ccflags-y := -Wno-error -Wno-format -Wno-unused-variable -Wno-declaration-after-statement

all:
    $(MAKE) -C $(KDIR) M=$(PWD) modules
    gcc -o cpu-load cpu-reg-main.c

clean:
    $(MAKE) -C $(KDIR) M=$(PWD) clean
    rm -f cpu-load