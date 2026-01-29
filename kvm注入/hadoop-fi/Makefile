# Makefile for Hadoop Fault Injection (增强版)
# 
# Hadoop 集群故障注入工具编译配置

CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -lpthread -lm

TARGET = hadoop_injector

all: $(TARGET)
	@echo "=== Hadoop Fault Injector v2.0 Built ==="
	@echo "功能: 进程故障、网络故障(tc/netfilter)、资源耗尽、心跳超时"

$(TARGET): hadoop_injector.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
