# 面向虚拟机/Hadoop/CloudStack的故障注入工具集

## 1. 简介
本文件夹包含了一套基于 Linux 用户态接口（ptrace, tc, iptables, kill 等）开发的故障注入工具。
这些工具支持以下场景：
- **虚拟机内部 (Guest OS)** 运行以模拟应用层故障
- **宿主机 (Host OS)** 运行以针对 QEMU/KVM 进程进行干扰
- **Hadoop 集群** 故障注入（NameNode/DataNode/YARN等）
- **CloudStack 云平台** 故障注入（Management Server/Agent/存储等）

### 1.1 适用环境
- 操作系统: Ubuntu 24.04 (支持ARM64/x86_64)
- 虚拟化: UTM (Mac ARM) / QEMU-KVM
- 集群: 3节点 Hadoop 集群
- 云平台: CloudStack (可选)

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

### 2.2 Hadoop/CloudStack 专用注入器 (新增)

| 工具文件                | 编译后名称            | 功能描述                                                                               |
| :---------------------- | :-------------------- | :------------------------------------------------------------------------------------- |
| `hadoop_injector.c`     | `hadoop_injector`     | **Hadoop集群故障注入**。支持NameNode/DataNode/YARN组件的崩溃、挂起、网络隔离等。       |
| `cloudstack_injector.c` | `cloudstack_injector` | **CloudStack云平台故障注入**。支持Management Server/Agent/MySQL/存储等组件故障模拟。   |
| `cluster_controller.c`  | `cluster_controller`  | **集群统一控制器**。集成VM、Hadoop、CloudStack故障注入的统一交互界面。                  |

### 2.3 控制器与辅助脚本
*   `fault_controller.c`: 一个集成控制器，封装了上述注入器的调用接口，可通过简单的命令行参数调用不同故障。
*   `cluster_manage.sh`: Hadoop集群管理脚本，支持启动/停止/状态检查/故障注入。
*   `cluster.conf`: 集群节点配置文件模板。
*   `run_cluster.sh`: QEMU虚拟机启动脚本（用于UTM环境）。
*   `target*.c`: 系列测试靶子程序，用于验证故障注入是否生效。
    *   `target.c`: 基础靶子，打印 Heap/Stack 变量。
    *   `targetv2.c`: 全能靶子，带有特征值 (`0xDEADBEEF`) 检查。
    *   `targetv3.c`: CPU 敏感靶子，计算 Wall Clock Time 衡量 CPU 性能。
    *   `targetv4.c`: 内存敏感靶子，循环申请内存测试 OOM。

## 3. 编译指南

请在当前目录下执行以下命令编译所有工具：

```bash
# 1. 编译基础注入工具
gcc -o cpu_injector cpu_injector.c -lpthread -lm
gcc -o mem_injector mem_injector.c
gcc -o mem_leak memleak_injector.c
gcc -o network_injector network_injector.c
gcc -o process_injector process_injector.c
gcc -o reg_injector reg_injector.c
gcc -o fault_controller fault_controller.c

# 2. 编译 Hadoop/CloudStack 注入工具 (新增)
gcc -o hadoop_injector hadoop_injector.c
gcc -o cloudstack_injector cloudstack_injector.c
gcc -o cluster_controller cluster_controller.c

# 3. 编译测试靶子 (用于验证)
gcc -o target target.c
gcc -o target_v2 targetv2.c
gcc -o target_cpu targetv3.c
gcc -o target_mem targetv4.c

# 4. 设置脚本可执行权限
chmod +x cluster_manage.sh run_cluster.sh
```

### 3.1 一键编译脚本

你也可以创建一个简单的编译脚本：

```bash
#!/bin/bash
# build_all.sh - 一键编译所有工具

echo "=== 编译故障注入工具集 ==="

# 基础工具
gcc -o cpu_injector cpu_injector.c -lpthread -lm || echo "cpu_injector 编译失败"
gcc -o mem_injector mem_injector.c || echo "mem_injector 编译失败"
gcc -o mem_leak memleak_injector.c || echo "mem_leak 编译失败"
gcc -o network_injector network_injector.c || echo "network_injector 编译失败"
gcc -o process_injector process_injector.c || echo "process_injector 编译失败"
gcc -o reg_injector reg_injector.c || echo "reg_injector 编译失败"
gcc -o fault_controller fault_controller.c || echo "fault_controller 编译失败"

# Hadoop/CloudStack 工具
gcc -o hadoop_injector hadoop_injector.c || echo "hadoop_injector 编译失败"
gcc -o cloudstack_injector cloudstack_injector.c || echo "cloudstack_injector 编译失败"
gcc -o cluster_controller cluster_controller.c || echo "cluster_controller 编译失败"

# 测试靶子
gcc -o target target.c || echo "target 编译失败"
gcc -o target_v2 targetv2.c || echo "target_v2 编译失败"
gcc -o target_cpu targetv3.c || echo "target_cpu 编译失败"
gcc -o target_mem targetv4.c || echo "target_mem 编译失败"

# 脚本权限
chmod +x cluster_manage.sh run_cluster.sh 2>/dev/null

echo "=== 编译完成 ==="
ls -la *_injector *_controller target* 2>/dev/null
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

### 4.7 Hadoop 故障注入 (新增)
针对 Hadoop 集群的专用故障注入工具。

```bash
# 查看所有 Hadoop 进程状态
./hadoop_injector list

# 终止 NameNode
sudo ./hadoop_injector crash nn

# 挂起 DataNode
sudo ./hadoop_injector hang dn

# 恢复 DataNode
sudo ./hadoop_injector resume dn

# 进入 HDFS 安全模式
./hadoop_injector hdfs-safe enter

# 隔离某个节点的网络
sudo ./hadoop_injector network 192.168.64.11

# 清理网络隔离
sudo ./hadoop_injector network-clear 192.168.64.11

# 组件代号：nn=NameNode, dn=DataNode, rm=ResourceManager, nm=NodeManager
```

### 4.8 CloudStack 故障注入 (新增)
针对 CloudStack 云平台的专用故障注入工具。

```bash
# 查看 CloudStack 服务状态
./cloudstack_injector list

# 终止 Management Server
sudo ./cloudstack_injector crash ms

# 注入 API 延迟 (500ms)
sudo ./cloudstack_injector api-delay 500

# 清理 API 延迟
sudo ./cloudstack_injector api-delay-clear

# 限制数据库连接
sudo ./cloudstack_injector db-limit

# 断开 Agent 连接
sudo ./cloudstack_injector agent-disconnect 192.168.1.20

# 组件代号：ms=Management Server, agent=Agent, mysql=MySQL
```

### 4.9 集群统一控制器 (新增)
交互式的统一控制界面，集成 VM、Hadoop、CloudStack 故障注入。

```bash
# 启动统一控制器
sudo ./cluster_controller

# 会显示交互菜单，支持：
# [1] 虚拟机故障注入
# [2] Hadoop故障注入
# [3] CloudStack故障注入
# [4] 预设故障场景
# [5] 查看集群状态
# [6] 一键恢复所有故障
```

### 4.10 集群管理脚本 (新增)
用于管理 Hadoop 集群的 Shell 脚本。

```bash
# 查看集群状态
./cluster_manage.sh status

# 启动集群
./cluster_manage.sh start

# 停止集群
./cluster_manage.sh stop

# 注入网络分区（隔离某个节点）
./cluster_manage.sh inject-network 192.168.64.11

# 注入网络延迟
./cluster_manage.sh inject-delay 200ms

# 清理网络故障
./cluster_manage.sh clear-network
./cluster_manage.sh clear-delay

# 运行 HDFS 测试
./cluster_manage.sh test
```

## 5. 常见问题
1.  **权限不足**: 大部分注入器 (mem, reg, net, process, hadoop, cloudstack) 需要 `sudo` 权限。
2.  **ptrace 失败**: 某些系统开启了 Yama LSM 安全模块，禁止 ptrace。解决方法：
    ```bash
    echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope
    ```
3.  **网络注入无效**: 请确保 `ip route` 能正确返回默认网卡，且系统安装了 `iptables` 和 `iproute2` (tc)。
4.  **Hadoop 进程未找到**: 确保 Hadoop 已正确启动，且 `jps` 命令可用。可以手动检查：
    ```bash
    jps -l | grep -E "NameNode|DataNode|ResourceManager|NodeManager"
    ```
5.  **SSH 连接失败**: 检查 SSH 密钥配置和防火墙设置：
    ```bash
    ssh-copy-id root@192.168.64.10  # 配置免密登录
    ```

## 6. 集群配置

### 6.1 配置文件格式 (cluster.conf)
```
# 节点名,IP地址,SSH端口,角色
master,192.168.64.10,22,NameNode,ResourceManager
slave1,192.168.64.11,22,DataNode,NodeManager
slave2,192.168.64.12,22,DataNode,NodeManager
```

### 6.2 UTM 虚拟机网络配置
如果使用 UTM 端口转发，配置示例：
```
master,localhost,2220,NameNode,ResourceManager
slave1,localhost,2221,DataNode,NodeManager
slave2,localhost,2222,DataNode,NodeManager
```

## 7. 风险警告

⚠️ **重要提示**:
- 故障注入工具会修改系统/进程状态，请在测试环境中使用
- 建议对虚拟机创建快照后再进行测试
- `crash` 操作会直接终止进程，可能导致数据丢失
- 网络隔离可能导致集群脑裂，请及时清理
