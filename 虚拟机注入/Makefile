# Makefile for 虚拟机注入 tools
# 用于编译所有故障注入工具

CC = gcc
CFLAGS = -Wall -O2
LDFLAGS_PTHREAD = -lpthread -lm

# 基础注入器
BASIC_TARGETS = cpu_injector mem_injector mem_leak network_injector process_injector reg_injector fault_controller

# Hadoop/CloudStack 注入器
CLUSTER_TARGETS = hadoop_injector cloudstack_injector cluster_controller

# 测试靶子
TEST_TARGETS = target target_v2 target_cpu target_mem

# 所有目标
ALL_TARGETS = $(BASIC_TARGETS) $(CLUSTER_TARGETS) $(TEST_TARGETS)

.PHONY: all clean basic cluster test help

# 默认编译所有
all: $(ALL_TARGETS)
	@echo ""
	@echo "=== 编译完成 ==="
	@echo "基础注入器: $(BASIC_TARGETS)"
	@echo "集群注入器: $(CLUSTER_TARGETS)"
	@echo "测试靶子:   $(TEST_TARGETS)"
	@chmod +x *.sh 2>/dev/null || true

# 仅编译基础工具
basic: $(BASIC_TARGETS)
	@echo "基础注入器编译完成"

# 仅编译集群工具
cluster: $(CLUSTER_TARGETS)
	@echo "集群注入器编译完成"

# 仅编译测试靶子
test: $(TEST_TARGETS)
	@echo "测试靶子编译完成"

# 基础注入器规则
cpu_injector: cpu_injector.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_PTHREAD)

mem_injector: mem_injector.c
	$(CC) $(CFLAGS) -o $@ $<

mem_leak: memleak_injector.c
	$(CC) $(CFLAGS) -o $@ $<

network_injector: network_injector.c
	$(CC) $(CFLAGS) -o $@ $<

process_injector: process_injector.c
	$(CC) $(CFLAGS) -o $@ $<

reg_injector: reg_injector.c
	$(CC) $(CFLAGS) -o $@ $<

fault_controller: fault_controller.c
	$(CC) $(CFLAGS) -o $@ $<

# 集群注入器规则
hadoop_injector: hadoop_injector.c
	$(CC) $(CFLAGS) -o $@ $<

cloudstack_injector: cloudstack_injector.c
	$(CC) $(CFLAGS) -o $@ $<

cluster_controller: cluster_controller.c
	$(CC) $(CFLAGS) -o $@ $<

# 测试靶子规则
target: target.c
	$(CC) $(CFLAGS) -o $@ $<

target_v2: targetv2.c
	$(CC) $(CFLAGS) -o $@ $<

target_cpu: targetv3.c
	$(CC) $(CFLAGS) -o $@ $<

target_mem: targetv4.c
	$(CC) $(CFLAGS) -o $@ $<

# 清理
clean:
	rm -f $(ALL_TARGETS)
	@echo "已清理所有编译产物"

# 帮助
help:
	@echo "故障注入工具集 Makefile"
	@echo ""
	@echo "用法: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all     - 编译所有工具 (默认)"
	@echo "  basic   - 仅编译基础注入器"
	@echo "  cluster - 仅编译集群注入器 (Hadoop/CloudStack)"
	@echo "  test    - 仅编译测试靶子"
	@echo "  clean   - 清理所有编译产物"
	@echo "  help    - 显示此帮助信息"
