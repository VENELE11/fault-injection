# VM 用户态故障注入工具

`vm_injection/` 提供运行在宿主机或 Guest OS 内的用户态注入器，同时包含当前 Web 控制器使用的 `kvm_injector`。这些工具依赖 Linux 用户态机制，如 signal、ptrace、tc、iptables、cgroup 和 `/proc`。

## 1. 编译

```bash
cd /Users/venele/Downloads/fault-injection/vm_injection
make all
```

编译目标：

| 目标 | 源码 | 功能 |
| --- | --- | --- |
| `process_injector` | `process_injector.c` | 按进程名执行 `SIGKILL`、`SIGSTOP`、`SIGCONT` |
| `network_injector` | `network_injector.c` | delay、loss、partition、corrupt、clear |
| `cpu_injector` | `cpu_injector.c` | CPU 压力，支持线程数和模式 |
| `mem_leak` | `memleak_injector.c` | 持续占用指定 MB 内存 |
| `mem_injector` | `mem_injector.c` | ptrace 修改目标进程 heap/stack 或指定地址 |
| `reg_injector` | `reg_injector.c` | ARM64 寄存器故障注入 |
| `fault_controller` | `fault_controller.c` | 交互式用户态控制器 |
| `kvm_injector` | `kvm_injector.c` | KVM 用户态故障入口 |
| `target_*` | `靶子程序/` | 注入验证靶子程序 |

## 2. 进程注入

```bash
sudo ./process_injector nginx 1  # crash
sudo ./process_injector nginx 2  # hang
sudo ./process_injector nginx 3  # resume
```

参数：`<process_name> <action_type 1|2|3>`。

## 3. 网络注入

```bash
sudo ./network_injector 1 200ms  # delay
sudo ./network_injector 2 10%    # loss
sudo ./network_injector 3 8080   # partition, 封锁 TCP 目的端口
sudo ./network_injector 4 1%     # corrupt
sudo ./network_injector 0        # clear
```

工具会自动选择默认出网网卡，并在每次注入前清理已有 `tc root qdisc` 和 OUTPUT 链中的封锁规则。

## 4. CPU 与内存压力

```bash
sudo ./cpu_injector 0 20 4
sudo ./mem_leak 0 512
```

`cpu_injector` 参数是 `<PID> <Duration_Sec> [Threads] [Mode]`。PID 为 0 表示全局压力，不绑定某个目标进程。

`mem_leak` 参数是 `<PID_ignored> <Size_MB>`。

## 5. 内存篡改

```bash
sudo ./mem_injector -p 1234 -r heap -t flip -b 0
sudo ./mem_injector -p 1234 -r stack -t set0 -b 7
sudo ./mem_injector -p 1234 -a 0x7fff0000 -t byte -b 0
sudo ./mem_injector -p 1234 -r heap -s deadbeef -t flip -b 3
```

常用参数：

| 参数 | 说明 |
| --- | --- |
| `-p` | 目标 PID |
| `-r` | 区域，`heap` 或 `stack` |
| `-a` | 手动地址 |
| `-s` | 十六进制特征值扫描 |
| `-t` | `flip`、`set0`、`set1`、`byte` |
| `-b` | 目标位 |

## 6. 寄存器注入

```bash
sudo ./reg_injector 1234 X0 flip1 0
sudo ./reg_injector 1234 PC invalidpc
sudo ./reg_injector 1234 X0 zeroall
sudo ./reg_injector 1234 SP add1 -1 -w 500
```

Web 控制器主要使用 `pid reg reg_type bit`，并支持延迟、循环和间隔参数。

## 7. KVM 用户态入口

`kvm_injector` 当前也在本目录编译：

```bash
sudo ./kvm_injector list
sudo ./kvm_injector soft-flip master PC 10
sudo ./kvm_injector soft-swap slave1 X0
sudo ./kvm_injector soft-zero slave2 SP 0
sudo ./kvm_injector guest-divzero master
sudo ./kvm_injector perf-delay slave1 50
sudo ./kvm_injector perf-stress slave1 20 4
sudo ./kvm_injector cpu-offline 2
sudo ./kvm_injector clear
```

更多 KVM 内核模块实验见 `kvm_injection/README.md` 和 `kvm_injection/kvm测试方法.md`。

## 8. Web 控制器映射

`web_controller/app.py` 中的 VM group 会调用本目录二进制：

- `vm_process` -> `process_injector`
- `vm_network` -> `network_injector`
- `vm_cpu` -> `cpu_injector`
- `vm_mem_leak` -> `mem_leak`
- `vm_mem_inject` -> `mem_injector`
- `vm_reg_inject` -> `reg_injector`
- `kvm_*` -> `kvm_injector`

## 9. 风险提示

- `ptrace` 和寄存器注入可能直接让目标进程崩溃。
- `network_injector 0` 会清理默认网卡 qdisc，并 flush OUTPUT 链，实验机上使用。
- `mem_leak` 可能触发 OOM。
- KVM 操作可能影响正在运行的虚拟机。
