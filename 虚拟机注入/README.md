# 面向虚拟机内部的故障注入工具集

## 1. 简介
本文件夹包含了一套基于 Linux 用户态接口（ptrace, tc, iptables, kill 等）开发的故障注入工具。
这些工具既可以在 **虚拟机内部 (Guest OS)** 运行以模拟应用层故障，也可以在 **宿主机 (Host OS)** 运行以针对 QEMU 进程进行干扰。

## 2. 工具列表与功能

### 2.1 核心注入器

| 工具文件             | 编译后名称         | 功能描述                                                                              | 原理                                  |
| :------------------- | :----------------- | :------------------------------------------------------------------------------------ | :------------------------------------ |
| `cpu_injector.c`     | `cpu_injector`     | **CPU 高负载注入**。创建多线程执行密集浮点运算，争抢 CPU 时间片。                     | `pthread` + `sqrt/tan` 循环计算       |
| `mem_injector.c`     | `mem_injector`     | **内存数据错误注入**。精准修改目标进程堆栈数据（位翻转、置0/1）。支持特征值扫描模式。 | `ptrace(PTRACE_PEEKDATA/POKEDATA)`    |
| `memleak_injector.c` | `mem_leak`         | **内存泄漏/耗尽注入**。持续申请并写入内存，模拟 OOM (Out Of Memory) 环境。            | `malloc` + `memset` (触发 Page Fault) |
| `network_injector.c` | `network_injector` | **网络故障注入**。模拟网络延迟、丢包、连接中断。                                      | `tc` (Traffic Control) + `iptables`   |
| `process_injector.c` | `process_injector` | **进程状态注入**。让进程崩溃、挂起（假死）或恢复。                                    | `kill(SIGKILL/SIGSTOP/SIGCONT)`       |
| `reg_injector.c`     | `reg_injector`     | **寄存器故障注入 (ARM64)**。修改目标进程的通用寄存器或 PC/SP 指针。                   | `ptrace(PTRACE_GETREGSET/SETREGSET)`  |

### 2.2 控制器与辅助脚本
*   `fault_controller.c`: 一个集成控制器，封装了上述注入器的调用接口，可通过简单的命令行参数调用不同故障。
*   `target*.c`: 系列测试靶子程序，用于验证故障注入是否生效。
    *   `target.c`: 基础靶子，打印 Heap/Stack 变量。
    *   `targetv2.c`: 全能靶子，带有特征值 (`0xDEADBEEF`) 检查。
    *   `targetv3.c`: CPU 敏感靶子，计算 Wall Clock Time 衡量 CPU 性能。
    *   `targetv4.c`: 内存敏感靶子，循环申请内存测试 OOM。

## 3. 编译指南

请在当前目录下执行以下命令编译所有工具：

```bash
# 1. 编译注入工具
gcc -o cpu_injector cpu_injector.c -lpthread -lm
gcc -o mem_injector mem_injector.c
gcc -o mem_leak memleak_injector.c
gcc -o network_injector network_injector.c
gcc -o process_injector process_injector.c
gcc -o reg_injector reg_injector.c
gcc -o fault_controller fault_controller.c

# 2. 编译测试靶子 (用于验证)
gcc -o target target.c
gcc -o target_v2 targetv2.c
gcc -o target_cpu targetv3.c
gcc -o target_mem targetv4.c
```

## 4. 详细使用说明

### 4.1 CPU 负载注入
模拟 CPU 资源耗尽，导致目标进程响应变慢。
```bash
# 格式: ./cpu_injector <PID_Ignored> <持续秒数> [线程数]
./cpu_injector 0 10 4
# 说明: 在接下来的 10 秒内，启动 4 个死循环计算线程跑满 CPU 核心。
```

### 4.2 内存数据篡改 (Memory Corruption)
需要 `sudo` 权限运行，基于 `ptrace` 附加进程。
```bash
# 先启动靶子: ./target_v2
# 获取 PID, 假设为 12345

# 格式: ./mem_injector <PID> <Region> <FaultType> [ByteOffset] [BitOffset]
# Region: 0=Heap, 1=Stack
# FaultType: 0=BitFlip, 1=Stuck0, 2=Stuck1

./mem_injector 12345 0 0 0 0
# 说明: 对 PID 12345 的 堆内存(0) 的第0字节第0位进行 翻转(0)。
```

### 4.3 内存泄漏 (OOM Simulation)
吞噬系统可用内存，迫使系统进入低内存状态或触发 OOM Killer。
```bash
# 格式: ./mem_leak <PID_Ignored> <Size_MB>
./mem_leak 0 1024
# 说明: 尝试吞噬 1024MB (1GB) 内存。
```

### 4.4 网络故障
基于 `tc` 命令，需要 `sudo` 权限。自动识别出口网卡。
```bash
# 格式: ./network_injector <Type> <Param>
# Type: 0=恢复, 1=延迟(Delay), 2=丢包(Loss), 3=断网(Partition/Corrupt)

sudo ./network_injector 1 100ms  # 注入 100ms 延迟
sudo ./network_injector 2 10%    # 注入 10% 丢包
sudo ./network_injector 0        # 清理故障，恢复正常
```

### 4.5 进程控制 (Crash/Hang)
```bash
# 格式: ./process_injector <Target_Name> <Action>
# Action: 1=Crash(Kill), 2=Hang(Stop), 3=Resume(Cont)

sudo ./process_injector target_v2 2
# 说明: 暂停名为 target_v2 的进程 (SIGSTOP)。
```

### 4.6 寄存器注入 (Register FI)
针对 ARM64 架构的寄存器修改。
```bash
# 格式: ./reg_injector <PID> <FaultType> [Delay_Sec]
sudo ./reg_injector 12345 0 0
# 说明: 立即对 PID 12345 的寄存器进行位翻转。
```

## 5. 常见问题
1.  **权限不足**: 大部分注入器 (mem, reg, net, process) 需要 `sudo` 权限。
2.  **ptrace 失败**: 某些系统开启了 Yama LSM 安全模块，禁止 ptrace。解决方法：
    ```bash
    echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope
    ```
3.  **网络注入无效**: 请确保 `ip route` 能正确返回默认网卡，且系统安装了 `iptables` 和 `iproute2` (tc)。
