# Makefile for Hadoop Fault Injection
# Hadoop 集群故障注入工具编译配置

CC = gcc
CFLAGS = -Wall -O2

TARGET = hadoop_injector

all: $(TARGET)
	@echo "=== Hadoop Fault Injector Built ==="

$(TARGET): hadoop_injector.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
