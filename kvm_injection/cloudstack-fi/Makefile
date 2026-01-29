# Makefile for CloudStack Fault Injection (增强版)
# 
# CloudStack 云平台故障注入工具编译配置

CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lpthread

TARGET = cloudstack_injector

all: $(TARGET)
	@echo "=== CloudStack Fault Injector v2.0 Built ==="
	@echo "功能: 进程故障、系统VM故障、存储故障、资源耗尽、VM操作故障"

$(TARGET): cloudstack_injector.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
