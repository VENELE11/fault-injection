# 面向 KVM 虚拟化平台的故障注入工具集 (Fault Injection Platform)

## 1. 项目简介
本项目不仅仅是一个单一的工具，而是一套完整的**虚拟化环境可靠性测试解决方案**。它涵盖了从**宿主机内核层 (Host / Hypervisor)** 到 **虚拟机应用层 (Guest OS / User Space)** 的全栈故障注入能力。

项目旨在通过模拟真实的硬件故障（如位翻转）、软件逻辑错误（如死锁、资源耗尽）以及网络异常，来验证云计算平台、操作系统内核以及上层业务的高可用性 (High Availability) 和容错能力。

主要包含两大核心组件：
1.  **kvm注入 (KVM-Side)**: 运行在宿主机 (Host)，基于 Linux 内核模块，针对 KVM Hypervisor 和宿主机内核进行注入。
2.  **虚拟机注入 (VM-Side)**: 运行在虚拟机内部 (Guest) 或针对 QEMU 进程，模拟应用层、OS 层及资源层的故障。

## 2. 目录结构说明

```
.
├── kvm注入/                # [核心] 面向 KVM 层的内核级注入工具 (ARM64适配 2.0版)
│   ├── cpu-fi/             # CPU 寄存器与执行流注入
│   ├── memory-manage-fi/   # 内存管理与回收(kswapd)注入
│   ├── access-control-fi/  # 权限控制与资源访问注入
│   ├── file-fi/            # 文件系统 I/O 注入
│   └── ... (详见子目录 README)
│
├── 虚拟机注入/              # [核心] 面向 Guest OS 的应用级注入工具
│   ├── cpu_injector.c      # CPU 争抢与高负载
│   ├── mem_injector.c      # 内存数据篡改 (ptrace)
│   ├── network_injector.c  # 网络故障 (延迟/丢包/断网)
│   ├── fault_controller.c  # 综合故障控制器
│   └── ... (详见子目录 README)
│
├── 故障现象.txt             # 历史测试中观测到的故障现象记录 (Kernel Panic, SSH断连等)
├── 环境搭建文档.txt          # 基础环境 (KVM/Libvirt/Bridge) 搭建参考手册
└── (其他历史版本/副本文件)    # 根目录下散落的 .c 文件为历史备份，通过上述两个文件夹查看最新版
```

## 3. 快速上手指南

### 阶段一：环境准备
请参考 `环境搭建文档.txt` 配置您的 Linux 服务器 (建议 CentOS/Ubuntu)。
*   **必要组件**: KVM, QEMU, Libvirt, GCC, Make, Kernel Headers。
*   **架构支持**:
    *   `kvm注入` 模块主要针对 **ARM64** 架构进行了深度适配 (kprobes/寄存器操作)。
    *   `虚拟机注入` 工具主要为通用 C 代码，支持 **x86_64** 和 **ARM64**。

### 阶段二：Guest 侧故障模拟 (虚拟机内部)
如果您希望测试虚拟机内部运行的数据库、Web 服务器或业务进程的健壮性：
1.  进入目录: `cd 虚拟机注入/`
2.  编译工具: 参考该目录下的 `README.md` 进行编译。
3.  运行注入:
    *   **内存泄漏**: `./mem_leak 0 1024` (吞噬 1GB 内存)
    *   **网络延迟**: `sudo ./network_injector 1 200ms` (增加 200ms 延迟)
    *   **进程崩溃**: `sudo ./process_injector nginx 1` (杀掉 nginx 进程)

### 阶段三：Host 侧底层故障模拟 (KVM/内核)
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

## 5. 注意事项
*   ⚠️ **高风险**: `kvm注入` 修改内核行为，极易导致宿主机 Kernel Panic (死机)。请务必在测试环境中运行，严禁用于生产环境。
*   **版本兼容**: 部分 KVM 注入模块依赖特定内核符号 (如 `kernel_clone` vs `_do_fork`)，如果加载失败，请根据 `kvm注入/README.md` 中的指南适配当前内核版本。
