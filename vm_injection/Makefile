# Makefile for 虚拟机注入 tools (增强版)
# 
# 用于编译虚拟机内部/KVM虚拟化层的故障注入工具

CC = gcc
CFLAGS = -Wall -O2
LDFLAGS_PTHREAD = -lpthread -lm

# 基础注入器
BASIC_TARGETS = cpu_injector mem_injector mem_leak network_injector process_injector reg_injector fault_controller

# KVM层注入器 (新增)
KVM_TARGETS = kvm_injector

# 测试靶子 (综合 + 分类)
TEST_TARGETS = target target_cpu target_mem target_reg target_net target_res

# 所有目标
ALL_TARGETS = $(BASIC_TARGETS) $(KVM_TARGETS) $(TEST_TARGETS)

.PHONY: all clean basic kvm test help

# 默认编译所有
all: $(ALL_TARGETS)
	@echo ""
	@echo "=== 编译完成 (增强版) ==="
	@echo "基础注入器: $(BASIC_TARGETS)"
	@echo "KVM层注入器: $(KVM_TARGETS)"
	@echo "测试靶子:   $(TEST_TARGETS)"
	@echo ""
	@echo "功能: 软错误、客户OS错误行为、性能故障、CPU热插拔"
	@chmod +x *.sh 2>/dev/null || true

# 仅编译基础工具
basic: $(BASIC_TARGETS)
	@echo "基础注入器编译完成"

# 编译KVM层注入器
kvm: $(KVM_TARGETS)
	@echo "KVM层注入器编译完成"

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

# KVM层注入器
kvm_injector: kvm_injector.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_PTHREAD)

# 测试靶子规则
target: target.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_PTHREAD)

target_cpu: target_cpu.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_PTHREAD)

target_mem: target_mem.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_PTHREAD)

# target_reg 必须用 -O0 禁止优化，否则寄存器注入效果不明显
target_reg: target_reg.c
	$(CC) -Wall -O0 -o $@ $< $(LDFLAGS_PTHREAD)

target_net: target_net.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_PTHREAD)

target_res: target_res.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_PTHREAD)

# 清理
clean:
	rm -f $(ALL_TARGETS)
	@echo "已清理所有编译产物"

# 帮助
help:
	@echo "虚拟机注入工具集 Makefile v3.0"
	@echo ""
	@echo "用法: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all     - 编译所有工具 (默认)"
	@echo "  basic   - 仅编译基础注入器"
	@echo "  kvm     - 编译KVM层注入器"
	@echo "  test    - 仅编译测试靶子"
	@echo "  clean   - 清理所有编译产物"
	@echo "  help    - 显示此帮助信息"
	@echo ""
	@echo "测试靶场:"
	@echo "  target_cpu - CPU注入测试 (配合 cpu_injector)"
	@echo "  target_mem - 内存注入测试 (配合 mem_injector)"
	@echo "  target_reg - 寄存器注入测试 (配合 reg_injector)"
	@echo "  target_net - 网络注入测试 (配合 network_injector)"
	@echo "  target_res - 资源耗尽测试 (配合 mem_leak)"
	@echo "  target     - 综合测试靶场"
