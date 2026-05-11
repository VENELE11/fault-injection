# KVM / Hadoop / CloudStack 故障注入工具

`kvm_injection/` 包含三类内容：

1. KVM/内核模块实验：`cpu-fi`、`memory-fi`、`file-fi`、`state-query-fi`、`vm-migration-fi` 等。
2. Hadoop 场景注入器：`hadoop-fi/hadoop_injector.c`。
3. CloudStack 场景注入器：`cloudstack-fi/cloudstack_injector.c`。

Web 控制器中的 `kvm_*` 动作主要调用 `vm_injection/kvm_injector`；本目录保留更底层的内核模块源码和 Hadoop/CloudStack 业务注入工具。

## 1. 编译

```bash
cd /Users/venele/Downloads/fault-injection/kvm_injection
make all
```

`Makefile` 会编译：

- `cluster_controller`
- `hadoop-fi/hadoop_injector`
- `cloudstack-fi/cloudstack_injector`
- 各子目录内核模块或触发器

如果内核模块编译失败，先安装内核头文件：

```bash
sudo apt install -y linux-headers-$(uname -r)
```

## 2. KVM/内核模块概览

| 目录 | 关注点 | 典型影响 |
| --- | --- | --- |
| `cpu-fi` | 进程创建、寄存器、执行流 | vCPU 线程异常、进程崩溃 |
| `file-fi/file-read-fi` | `vfs_read` | 读取失败、I/O 错误 |
| `file-fi/file-write-fi` | `vfs_write` | 写入失败、只读或空间不足语义 |
| `memory-fi/pt-load-fi` | 缺页处理 | OOM、访问异常 |
| `memory-fi/pt-update-fi` | TLB / 页表更新路径 | 状态不一致、访问异常 |
| `memory-manage-fi` | KVM 内存区域管理 | 虚拟机内存映射异常 |
| `access-control-fi` | KVM ioctl / 资源访问 | QEMU 管理操作被拒绝 |
| `state-query-fi` | KVM 状态和版本查询 | 状态失真、初始化失败 |
| `vm-migration-fi` | 热迁移脏页相关路径 | 迁移失败或数据不一致 |

内核符号在不同 Linux 版本中可能变化。加载模块前建议检查：

```bash
sudo grep "kernel_clone" /proc/kallsyms
sudo grep "vfs_read" /proc/kallsyms
sudo grep "handle_mm_fault" /proc/kallsyms
```

## 3. Hadoop 注入器

```bash
cd kvm_injection/hadoop-fi
make
./hadoop_injector list
sudo ./hadoop_injector crash dn
sudo ./hadoop_injector delay slave1 200 50
sudo ./hadoop_injector loss slave1 10
sudo ./hadoop_injector reorder slave1 30
sudo ./hadoop_injector isolate slave1
sudo ./hadoop_injector hdfs-safe enter
sudo ./hadoop_injector yarn-unhealthy slave1 on
```

详细命令、验证和恢复见 `../Hadoop 故障注入测试说明文档.md`。

## 4. CloudStack 注入器

```bash
cd kvm_injection/cloudstack-fi
make
sudo ./cloudstack_injector list
sudo ./cloudstack_injector hang agent
sudo ./cloudstack_injector api-delay 1000
sudo ./cloudstack_injector network 192.168.1.11 8250
sudo ./cloudstack_injector storage-ro /tmp/cs_secondary
sudo ./cloudstack_injector vm-migrate-fail
```

详细命令、模拟环境和清理见 `../cloudstack测试.md`。

## 5. 统一 CLI 控制器

```bash
cd kvm_injection
sudo ./cluster_controller
```

菜单入口覆盖：

- VM 故障注入
- Hadoop 故障注入
- CloudStack 故障注入
- 预设故障场景
- 集群状态查看
- 一键恢复

`cluster_manage.sh` 也保留了脚本化集群管理入口：

```bash
./cluster_manage.sh status
./cluster_manage.sh start
./cluster_manage.sh stop
sudo ./cluster_manage.sh inject-delay 100ms
sudo ./cluster_manage.sh clear-delay
```

## 6. 与 Web 控制器的关系

- Hadoop/CloudStack 单次动作在 `web_controller/app.py` 的 `ACTIONS` 中。
- Chaos Mesh 不在本目录实现，相关代码在 `web_controller/k8s_chaos.py`。
- KVM Web 动作主要使用 `vm_injection/kvm_injector`，而本目录的 `*-fi` 子目录用于更底层的 LKM 实验。

## 7. 风险提示

- 内核模块会 hook 高频路径，可能导致宿主机无响应或 Kernel Panic。
- 文件系统和内存相关模块可能造成数据损坏。
- 只在可重建实验环境运行，并在测试后卸载模块、清理网络和 cgroup 规则。
