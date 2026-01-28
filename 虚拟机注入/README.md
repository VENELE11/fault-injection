# 面向虚拟机内部的故障注入工具集

## 1. 简介
本文件夹包含了一套基于 Linux 用户态接口（ptrace, tc, iptables 等）开发的故障注入工具。
这些工具主要用于：
- **虚拟机内部 (Guest OS)** 运行以模拟应用层故障
- **宿主机 (Host OS)** 运行以针对 QEMU/KVM 进程进行干扰

> **注意**: Hadoop 和 CloudStack 的故障注入工具已移至 `kvm注入/` 目录，请参考该目录下的 README.md。

### 1.1 适用环境
- 操作系统: Ubuntu 24.04 (支持ARM64/x86_64)
- 虚拟化: UTM (Mac ARM) / QEMU-KVM

## 2. 工具列表与功能

### 2.1 核心注入器

| 工具文件             | 编译后名称         | 功能描述                                                                              |
| :------------------- | :----------------- | :------------------------------------------------------------------------------------ |
| `cpu_injector.c`     | `cpu_injector`     | **CPU 高负载注入**。创建多线程执行密集浮点运算，争抢 CPU 时间片。                     |
| `mem_injector.c`     | `mem_injector`     | **内存数据错误注入**。精准修改目标进程堆栈数据（位翻转、置0/1）。支持特征值扫描模式。 |
| `memleak_injector.c` | `mem_leak`         | **内存泄漏/耗尽注入**。持续申请并写入内存，模拟 OOM (Out Of Memory) 环境。            |
| `network_injector.c` | `network_injector` | **网络故障注入**。模拟网络延迟、丢包、连接中断。                                      |
| `process_injector.c` | `process_injector` | **进程状态注入**。让进程崩溃、挂起（假死）或恢复。                                    |
| `reg_injector.c`     | `reg_injector`     | **寄存器故障注入 (ARM64)**。修改目标进程的通用寄存器或 PC/SP 指针。                   |

### 2.2 控制器与辅助脚本
*   `fault_controller.c`: 一个集成控制器，封装了上述注入器的调用接口。
*   `run_cluster.sh`: QEMU虚拟机启动脚本（用于UTM环境）。
*   `start_kvm.sh`: KVM虚拟机启动脚本。
*   `target*.c`: 系列测试靶子程序，用于验证故障注入是否生效。

## 3. 编译指南

请在当前目录下执行以下命令编译所有工具：

```bash
# 使用 Makefile 一键编译
make all

# 或者只编译基础工具
make basic

# 编译测试靶子
make test
```

## 4. 详细使用说明

### 4.1 CPU 负载注入
```bash
./cpu_injector 0 10 4
# 启动 4 个线程，持续 10 秒的 CPU 高负载
```

### 4.2 网络故障
```bash
sudo ./network_injector 1 100ms  # 注入 100ms 延迟
sudo ./network_injector 2 10%    # 注入 10% 丢包
sudo ./network_injector 0        # 清理故障
```

### 4.3 进程控制
```bash
sudo ./process_injector nginx 1  # 终止进程
sudo ./process_injector nginx 2  # 暂停进程
sudo ./process_injector nginx 3  # 恢复进程
```

## 5. Hadoop/CloudStack 故障注入

Hadoop 和 CloudStack 的故障注入工具已移至 `kvm注入/` 目录。请参考：
- `kvm注入/hadoop-fi/` - Hadoop 集群故障注入
- `kvm注入/cloudstack-fi/` - CloudStack 云平台故障注入
- `kvm注入/README.md` - 详细使用说明
