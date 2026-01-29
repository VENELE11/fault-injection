# Makefile for 虚拟机注入 tools (增强版)
# 基于论文《云计算系统故障注入平台的研究与设计》
# 用于编译虚拟机内部/KVM虚拟化层的故障注入工具

CC = gcc
CFLAGS = -Wall -O2
LDFLAGS_PTHREAD = -lpthread -lm

# 基础注入器
BASIC_TARGETS = cpu_injector mem_injector mem_leak network_injector process_injector reg_injector fault_controller

# KVM层注入器 (新增)
KVM_TARGETS = kvm_injector

# 测试靶子
TEST_TARGETS = target target_v2 target_cpu target_mem

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

# KVM层注入器 (论文4.1.2)
kvm_injector: kvm_injector.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS_PTHREAD)

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
	@echo "虚拟机注入工具集 Makefile v2.0 (增强版)"
	@echo "基于论文《云计算系统故障注入平台的研究与设计》"
	@echo ""
	@echo "用法: make [target]"
	@echo ""
	@echo "Targets:"
	@echo "  all     - 编译所有工具 (默认)"
	@echo "  basic   - 仅编译基础注入器"
	@echo "  kvm     - 编译KVM层注入器 (软错误/性能故障/CPU热插拔)"
	@echo "  test    - 仅编译测试靶子"
	@echo "  clean   - 清理所有编译产物"
	@echo "  help    - 显示此帮助信息"
	@echo ""
	@echo "KVM注入器功能 (论文第4章):"
	@echo "  - 软错误注入 (4.1.2.1): 位翻转/交换/覆盖"
	@echo "  - 客户OS错误行为 (4.1.2.2): 数据段修改/异常触发"
	@echo "  - 性能故障 (4.1.2.3): ioctl延迟模拟"
	@echo "  - 维护故障 (4.1.2.4): CPU热插拔"
