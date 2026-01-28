# 面向 KVM 虚拟化平台的故障注入工具集 (Fault Injection Platform)

## 1. 项目简介
本项目不仅仅是一个单一的工具，而是一套完整的**虚拟化环境可靠性测试解决方案**。它涵盖了从**宿主机内核层 (Host / Hypervisor)** 到 **虚拟机应用层 (Guest OS / User Space)** 的全栈故障注入能力。

项目旨在通过模拟真实的硬件故障（如位翻转）、软件逻辑错误（如死锁、资源耗尽）以及网络异常，来验证云计算平台、操作系统内核以及上层业务的高可用性 (High Availability) 和容错能力。

主要包含以下核心组件：
1.  **kvm注入 (KVM-Side)**: 运行在宿主机 (Host)，基于 Linux 内核模块，针对 KVM Hypervisor 和宿主机内核进行注入。
2.  **虚拟机注入 (VM-Side)**: 运行在虚拟机内部 (Guest) 或针对 QEMU 进程，模拟应用层、OS 层及资源层的故障。
3.  **Hadoop 注入 (新增)**: 针对 Hadoop 分布式集群 (HDFS/YARN) 的专用故障注入工具。
4.  **CloudStack 注入 (新增)**: 针对 CloudStack 云计算平台的故障注入工具。

### 1.1 适用环境
- **宿主机**: Mac ARM (M1/M2/M3) + UTM 虚拟化 或 Linux x86_64 + KVM
- **虚拟机**: Ubuntu 24.04 (支持 ARM64/x86_64)
- **集群**: 3节点 Hadoop 集群 (1 Master + 2 Slave)
- **云平台**: CloudStack (可选)

## 2. 目录结构说明

```
.
├── kvm注入/                    # [核心] 面向 KVM 层的内核级注入工具 (ARM64适配 2.0版)
│   ├── cpu-fi/                 # CPU 寄存器与执行流注入
│   ├── memory-manage-fi/       # 内存管理与回收(kswapd)注入
│   ├── access-control-fi/      # 权限控制与资源访问注入
│   ├── file-fi/                # 文件系统 I/O 注入
│   ├── vm-migration-fi/        # 虚拟机热迁移故障注入
│   └── ... (详见子目录 README)
│
├── 虚拟机注入/                  # [核心] 面向 Guest OS 的应用级注入工具
│   ├── cpu_injector.c          # CPU 争抢与高负载
│   ├── mem_injector.c          # 内存数据篡改 (ptrace)
│   ├── network_injector.c      # 网络故障 (延迟/丢包/断网)
│   ├── process_injector.c      # 进程状态注入 (崩溃/挂起)
│   ├── fault_controller.c      # 综合故障控制器
│   ├── hadoop_injector.c       # [新增] Hadoop 集群故障注入
│   ├── cloudstack_injector.c   # [新增] CloudStack 云平台故障注入
│   ├── cluster_controller.c    # [新增] 集群统一控制器
│   ├── cluster_manage.sh       # [新增] 集群管理脚本
│   ├── cluster.conf            # [新增] 集群配置文件
│   ├── run_cluster.sh          # QEMU 虚拟机启动脚本
│   ├── Makefile                # 编译配置
│   └── ... (详见子目录 README)
│
├── 故障现象.txt                 # 历史测试中观测到的故障现象记录
├── 环境搭建文档.txt              # 基础环境搭建参考手册
└── README.md                   # 本文件
```

## 3. 快速上手指南

### 阶段一：环境准备
请参考 `环境搭建文档.txt` 配置您的 Linux 服务器 (建议 Ubuntu 24.04)。
*   **必要组件**: KVM, QEMU, Libvirt, GCC, Make, Kernel Headers。
*   **架构支持**:
    *   `kvm注入` 模块主要针对 **ARM64** 架构进行了深度适配 (kprobes/寄存器操作)。
    *   `虚拟机注入` 工具主要为通用 C 代码，支持 **x86_64** 和 **ARM64**。
*   **Mac ARM (UTM) 用户**:
    *   使用 UTM 创建 Ubuntu 24.04 虚拟机
    *   在虚拟机内部安装 Hadoop 集群

### 阶段二：编译工具
```bash
# 进入虚拟机注入目录
cd 虚拟机注入/

# 使用 Makefile 一键编译所有工具
make all

# 或者只编译特定模块
make basic      # 基础注入器
make cluster    # Hadoop/CloudStack 注入器
make test       # 测试靶子
```

### 阶段三：Guest 侧故障模拟 (虚拟机内部)
如果您希望测试虚拟机内部运行的数据库、Web 服务器或业务进程的健壮性：
1.  进入目录: `cd 虚拟机注入/`
2.  编译工具: `make all`
3.  运行注入:
    *   **内存泄漏**: `./mem_leak 0 1024` (吞噬 1GB 内存)
    *   **网络延迟**: `sudo ./network_injector 1 200ms` (增加 200ms 延迟)
    *   **进程崩溃**: `sudo ./process_injector nginx 1` (杀掉 nginx 进程)

### 阶段四：Hadoop 集群故障注入 (新增)
如果您希望测试 Hadoop 集群的容错能力：
```bash
# 查看 Hadoop 进程状态
./hadoop_injector list

# 终止 NameNode (测试 HDFS 可用性)
sudo ./hadoop_injector crash nn

# 隔离 DataNode 节点 (测试网络分区)
sudo ./hadoop_injector network 192.168.64.11

# 进入 HDFS 安全模式
./hadoop_injector hdfs-safe enter

# 使用集群管理脚本
./cluster_manage.sh status
./cluster_manage.sh inject-delay 100ms
```

### 阶段五：CloudStack 故障注入 (新增)
如果您希望测试 CloudStack 云平台的高可用性：
```bash
# 查看 CloudStack 服务状态
./cloudstack_injector list

# 注入 API 延迟
sudo ./cloudstack_injector api-delay 500

# 终止 Management Server
sudo ./cloudstack_injector crash ms

# 断开 Agent 连接
sudo ./cloudstack_injector agent-disconnect 192.168.1.20
```

### 阶段六：使用统一控制器
如果您希望使用交互式界面：
```bash
# 启动统一控制器
sudo ./cluster_controller

# 支持的功能：
# [1] 虚拟机故障注入
# [2] Hadoop 故障注入
# [3] CloudStack 故障注入
# [4] 预设故障场景
# [5] 查看集群状态
# [6] 一键恢复所有故障
```

### 阶段七：Host 侧底层故障模拟 (KVM/内核)
如果您希望测试虚拟化平台本身的隔离性、热迁移机制或宿主机稳定性：
1.  进入目录: `cd kvm注入/`
2.  编译模块: `make` (确保已安装内核头文件)
3.  加载模块并注入:
    *   详见该目录下的 `README.md`。
    *   例如注入 **虚机热迁移** 故障，或 **KVM 寄存器** 状态伪造。

## 4. 故障现象与观测
在 `故障现象.txt` 中，您可以找到各类注入可能引发的典型症状，例如：
*   **CPU 注入**: 导致 virsh 命令卡死，最终系统 Crash。
*   **网络注入**: SSH 连接频繁断开。
*   **权限注入**: 系统日志出现大量 `Permission denied` 或 `FATAL` 错误。
*   **Hadoop 故障**: HDFS 文件系统不可用、YARN 任务失败、节点被标记为死亡。
*   **CloudStack 故障**: API 响应超时、虚拟机创建失败、Agent 断连告警。

## 5. 注意事项
*   ⚠️ **高风险**: `kvm注入` 修改内核行为，极易导致宿主机 Kernel Panic (死机)。请务必在测试环境中运行，严禁用于生产环境。
*   **版本兼容**: 部分 KVM 注入模块依赖特定内核符号 (如 `kernel_clone` vs `_do_fork`)，如果加载失败，请根据 `kvm注入/README.md` 中的指南适配当前内核版本。
*   **快照建议**: 在进行任何故障注入前，建议对虚拟机创建快照，以便快速恢复。
*   **网络隔离**: 使用网络故障注入后，请及时清理 iptables 规则，避免影响正常通信。

## 6. 集群配置指南

### 6.1 3节点 Hadoop 集群配置
推荐的节点配置：

| 节点 | IP地址 | 角色 |
|------|--------|------|
| master | 192.168.64.10 | NameNode, ResourceManager, SecondaryNameNode |
| slave1 | 192.168.64.11 | DataNode, NodeManager |
| slave2 | 192.168.64.12 | DataNode, NodeManager |

### 6.2 配置文件
修改 `虚拟机注入/cluster.conf` 以匹配您的实际环境：
```
master,192.168.64.10,22,NameNode,ResourceManager
slave1,192.168.64.11,22,DataNode,NodeManager
slave2,192.168.64.12,22,DataNode,NodeManager
```

### 6.3 UTM (Mac ARM) 网络配置
如果使用 UTM 端口转发模式：
```
master,localhost,2220,NameNode,ResourceManager
slave1,localhost,2221,DataNode,NodeManager
slave2,localhost,2222,DataNode,NodeManager
```
