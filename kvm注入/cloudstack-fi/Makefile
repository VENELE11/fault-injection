# Makefile for CloudStack Fault Injection
# CloudStack 云平台故障注入工具编译配置

CC = gcc
CFLAGS = -Wall -O2

TARGET = cloudstack_injector

all: $(TARGET)
	@echo "=== CloudStack Fault Injector Built ==="

$(TARGET): cloudstack_injector.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: all clean
