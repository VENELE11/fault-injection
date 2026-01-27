# 面向 KVM 虚拟化层的故障注入工具 (ARM64) 详细说明

## 1. 简介
本项目是一个面向 ARM64 架构 KVM 虚拟化环境的内核级故障注入工具集。通过 Linux Kernel Module (LKM) 和 kprobes/kretprobes 技术，在 KVM 宿主机（Host）侧对特定 KVM 或内核函数进行动态追踪与拦截，通过修改寄存器返回值或函数执行流，模拟底层异常。

## 2. 详细环境兼容性说明 (针对 Ubuntu 24.04 等新系统)

本工具依赖 Linux 内核的 `kprobes` 机制，因此对内核符号（Symbol）有强依赖。

*   **推荐内核**: Linux 4.19 ~ 5.15
*   **Ubuntu 24.04 (Linux 6.8+) 注意事项**:
    Ubuntu 24.04 使用 Linux 6.8+ 内核，**可能会有影响**。
    1.  **函数名变动**: 新版内核可能重命名或内联 (inline) 部分函数。例如 `_do_fork` 在新内核均完全替换为 `kernel_clone` (本项目已适配 `kernel_clone`)。
    2.  **符号导出**: 虽然 kprobes 可以 hook 未导出的函数，但必须在 `/proc/kallsyms` 中存在。
    3.  **检查方法**: 在运行前，请务必执行以下命令检查目标函数是否存在于当前内核中：
        ```bash
        sudo grep "函数名" /proc/kallsyms
        # 例如：
        sudo grep "kernel_clone" /proc/kallsyms
        sudo grep "vfs_read" /proc/kallsyms
        ```
        如果 grep 没有输出，说明该内核版本下函数名已变更，故障注入将无法生效（模块加载可能会报错 `Unknown symbol`）。

## 3. 模块功能详解与注入点

| 模块目录                   | 注入目标函数 (Hook Point)                    | 故障原理解析                                                                       | 预期现象                                                                                 |
| :------------------------- | :------------------------------------------- | :--------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------- |
| **cpu-fi**                 | `kernel_clone`                               | 在进程创建 (fork/clone) 这一关键时刻，修改 ARM64 通用寄存器 (X0-X30) 或 SP/PC 值。 | 新创建的 vCPU 线程或进程崩溃、计算错误、非法指令异常。                                   |
| **file-fi/read**           | `vfs_read`                                   | 拦截虚拟文件系统读操作，强制将返回值修改为 `-EIO` (输入输出错误) 或 `-EINTR`。     | 虚拟机读取镜像文件或配置文件时报错，导致启动失败或 IO 挂起。                             |
| **file-fi/write**          | `vfs_write`                                  | 拦截写操作，强制返回 `-ENOSPC` (空间不足) 或 `-EROFS` (只读)。                     | 虚拟机写入日志或磁盘时失败，可能导致 guest OS 报文件系统错误。                           |
| **memory-fi/load**         | `handle_mm_fault`                            | 拦截缺页异常处理函数，通过 `kretprobe` 注入错误，阻止页面映射建立。                | 虚拟机访问特定内存时触发 Segmentation Fault 或 Guest OS 卡死。                           |
| **memory-fi/update**       | `flush_tlb_mm`                               | 拦截 TLB 刷新函数，直接调用 `jprobes` (模拟) 或修改执行逻辑，阻止 TLB 更新。       | 出现陈旧的 TLB 表项，导致 Guest OS 访问已释放或移动的内存，引发数据不一致。              |
| **memory-manage-fi**       | `kvm_set_memory_region`<br>`gfn_to_hva_many` | 拦截 KVM 建立内存区域映射的接口，模拟映射失败。                                    | 虚拟机初始化内存条失败，或无法识别新添加的内存设备。                                     |
| **kswapd-fi**              | `shrink_node`                                | 拦截内核内存回收核心函数，通过延时 (`mdelay`) 或直接返回阻止回收。                 | 宿主机在内存压力大时无法回收内存，可能导致 Host OOM (Out of Memory) 进而杀掉 QEMU 进程。 |
| **access-control-fi**      | `kvm_vm_ioctl`                               | 拦截 KVM 的 ioctl 入口，针对特定命令字返回 `-EPERM` 或 `-EACCES`。                 | QEMU 发起的某项管理操作（如创建 VCPU、设置内存）被拒绝。                                 |
| **state-query-fi/state**   | `kvm_arch_vcpu_ioctl_get_regs`               | 拦截寄存器查询接口，修改返回结构体中的 PSTATE（状态）或 PC 指针。                  | 外部监控工具（如 `virsh` dump）获取到虚假的 vCPU 状态，掩盖真实故障。                    |
| **state-query-fi/version** | `kvm_dev_ioctl`                              | 拦截 KVM 设备控制接口，伪造返回版本号或是错误码。                                  | 导致依赖特定 KVM API 版本的用户态工具初始化失败。                                        |
| **vm-migration-fi**        | `kvm_vm_ioctl_get_dirty_log`                 | 拦截脏页日志获取接口，篡改返回的脏页位图（Bitmap）。                               | 热迁移过程中，甚至内存已变化但未被标记为脏，导致目的端虚拟机内存数据损坏/丢失。          |

## 4. 详细使用步骤

### 第一步：编译
在根目录 `kvm注入/` 下执行：
```bash
make
```
确保没有报错。如果报错 `fatal error: linux/kprobes.h: No such file`，请安装内核头文件：
```bash
# Ubuntu
sudo apt-get install linux-headers-$(uname -r)
```

### 第二步：选择并加载模块
以 **CPU 寄存器故障** 为例：

1.  **加载内核模块**
    ```bash
    cd cpu-fi
    sudo insmod cpu-reg.ko
    # 查看 dmesg 确认加载成功
    dmesg | tail
    # 应显示: [cpu_fi] kprobe registered at kernel_clone ...
    ```

2.  **运行用户态触发器**
    你需要知道目标进程的 PID（通常是 qemu-kvm 进程）。
    ```bash
    ps -ef | grep qemu
    # 假设 PID 是 12345
    
    # 注入 PC 指针翻转故障 (类型 1)
    sudo ./cpu-reg-main 12345 1
    ```

3.  **观察现象**
    查看 QEMU 进程是否崩溃，或 Guest OS 内部是否报 CPU 异常。

### 第三步：卸载模块
测试结束后，务必卸载模块，否则它会一直驻留在内存中拦截函数。
```bash
sudo rmmod cpu_reg
```

## 5. 风险警告 (Risk Warning)

1.  **Host Crash**: 注入 `vfs_read`, `handle_mm_fault`, `shrink_node` 等核心高频函数非常危险。如果逻辑错误（如死循环、错误的指针修改）可能导致宿主机 Kernel Panic，系统重启。
2.  **数据丢失**: 文件系统注入 (`vfs_write`) 可能导致宿主机文件损坏。**请务必在测试环境/虚拟机嵌套环境中运行，严禁在生产物理机运行。**
3.  **Ubuntu 24 兼容性**: 如果 `insmod` 报错 `Unknown symbol in module`，请使用文本编辑器打开对应的 `.c` 文件，将 `#define TARGET_FUNC "..."` 修改为当前内核版本对应的正确函数名 (可通过 `/proc/kallsyms` 查找)。
