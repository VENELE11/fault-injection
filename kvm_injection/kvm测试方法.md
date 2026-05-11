# KVM 内核模块测试方法

本文档保留 KVM/内核模块级测试步骤。Web 控制台中常用的 KVM 动作由 `vm_injection/kvm_injector` 提供；本文件面向 `kvm_injection/*-fi/` 下的内核模块实验。

## 0. 通用准备

```bash
cd /Users/venele/Downloads/fault-injection/kvm_injection
make all
uname -r
sudo grep -E "kernel_clone|vfs_read|vfs_write|handle_mm_fault|kvm_dev_ioctl|kvm_vm_ioctl" /proc/kallsyms
```

加载模块前先确认目标函数在 `/proc/kallsyms` 中存在。不同内核版本的函数名可能变化，Ubuntu 24.04 的 Linux 6.8+ 尤其需要检查。

## 1. CPU 寄存器故障

目录：`cpu-fi/`

```bash
cd cpu-fi
make
sudo insmod cpu-reg.ko
dmesg | tail
sudo ./cpu-reg-main 1
sudo rmmod cpu_reg
```

预期：`dmesg` 出现 CPU/寄存器注入日志，目标进程创建路径可能异常。

## 2. 文件读取故障

目录：`file-fi/file-read-fi/`

```bash
cd file-fi/file-read-fi
make
sudo insmod file-read-fi.ko
sudo ./file-read-fi-main 1
cat /etc/hosts
sudo rmmod file_read_fi
```

预期：读取路径返回异常或 `dmesg` 出现拦截日志。

## 3. 文件写入故障

目录：`file-fi/file-write-fi/`

```bash
cd file-fi/file-write-fi
make
sudo insmod file-write-fi.ko
sudo ./file-write-fi-main 1
echo test > /tmp/fi_write_test
sudo rmmod file_write_fi
```

预期：写入失败或 `dmesg` 出现 VFS 写路径注入日志。

## 4. 缺页处理故障

目录：`memory-fi/pt-load-fi/`

```bash
cd memory-fi/pt-load-fi
make
sudo insmod pt-load-fi.ko
sudo ./pt-load-fi-main 0
ls -R /etc >/dev/null
sudo rmmod pt_load_fi
```

预期：内存访问路径返回 OOM 或出现命令卡顿。该测试风险较高。

## 5. 页表/TLB 更新观测

目录：`memory-fi/pt-update-fi/`

```bash
cd memory-fi/pt-update-fi
make
sudo insmod pt-update-fi.ko
stress-ng --vm 1 --vm-bytes 128M -t 5s
dmesg | tail -n 30
sudo rmmod pt_update_fi
```

预期：缺页或页表更新路径出现模块日志。

## 6. KVM 状态查询故障

目录：`state-query-fi/kvm-state-fi/`

```bash
cd state-query-fi/kvm-state-fi
make
sudo insmod kvm-state-fi.ko
qemu-system-aarch64 -nographic -M virt -enable-kvm
sudo rmmod kvm_state_fi
```

预期：QEMU 初始化或状态查询失败，`dmesg` 出现 KVM 状态拦截日志。

## 7. KVM 版本号故障

目录：`state-query-fi/kvm-version-fi/`

```bash
cd state-query-fi/kvm-version-fi
make
sudo insmod kvm-version-fi.ko
sudo ./kvm-version-fi-main 1
qemu-system-aarch64 -nographic -M virt -enable-kvm
sudo rmmod kvm_version_fi
```

预期：依赖 KVM API 版本的工具初始化失败或输出异常。

## 8. 热迁移故障

目录：`vm-migration-fi/`

```bash
cd vm-migration-fi
make
sudo insmod vm-migration-fi.ko
# 在 QEMU monitor 中触发迁移，例如 migrate "exec:cat > /dev/null"
sudo rmmod vm_migration_fi
```

预期：迁移失败或脏页日志相关路径异常。

## 9. 访问控制故障

目录：`access-control-fi/`

```bash
cd access-control-fi
make
sudo insmod resource.ko
sudo ./resource-main
sudo rmmod resource
```

预期：KVM ioctl 或资源访问路径被拒绝，QEMU 可能报错或停止。

## 10. 内存回收/管理故障

目录：`memory-manage-fi/`

```bash
cd memory-manage-fi
make
sudo insmod memory.ko
sudo ./memory-main
sudo rmmod memory
```

预期：KVM 内存区域或回收路径出现注入日志。该类模块风险高，建议在可恢复虚拟机中测试。

## 11. 用户态 KVM 注入器补充

常规演示优先使用 `vm_injection/kvm_injector`：

```bash
cd ../vm_injection
sudo ./kvm_injector list
sudo ./kvm_injector soft-flip master PC 10
sudo ./kvm_injector perf-delay slave1 50
sudo ./kvm_injector perf-clear slave1
sudo ./kvm_injector clear
```

## 12. 清理 checklist

```bash
lsmod | grep -E 'fi|kvm'
dmesg | tail -n 80
sudo ../vm_injection/kvm_injector clear 2>/dev/null || true
```

确认已卸载本次加载的 `.ko`，并恢复 CPU、cgroup、tc、iptables 等系统状态。
