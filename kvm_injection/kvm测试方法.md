# KVM 故障注入工具套件 (ARM64) 测试

### 1. 访问控制故障 (access-control-fi)

- **功能**: 修改 `ioctl` 系统调用参数，干扰 VM 资源访问。

- **测试方法**:

  Bash

  ```
  # 编译与加载
  cd access-control-fi
  make && sudo insmod resource.ko
  
  # 触发 (使用配套工具)
  sudo ./access-load
  ```

- **预期结果**:

  - 虚拟机进程卡死或崩溃。
  - `dmesg` 显示: `[ARM-Res-Fi] Intercepted ioctl ....`

------

### 2. CPU 寄存器故障 (cpu-fi)

- **功能**: 劫持系统调用（如 `clone`），将 PC (Program Counter) 寄存器置零。

- **测试方法**:

  Bash

  ```
  # 编译与加载 (需先修复 cpu-reg.c 指针类型并增加 read 支持)
  cd cpu-fi
  make && sudo insmod cpu-reg-fi.ko
  
  # 配置并触发
  sudo bash -c "echo 1 > /proc/cpu-reg-fi/signal"
  
  # 触发任意进程创建
  ls
  ```

- **预期结果**:

  - `dmesg` 显示: `[ARM-Reg-Fi] Injected SetZero at PC.`
  - 目标进程可能崩溃。

------

### 3. 文件读取故障 (file-read-fi)

- **功能**: 拦截 `vfs_read`，注入 Bad Buffer (`buf=NULL`)。

- **测试方法**:

  Bash

  ```
  # 编译与加载
  cd file-fi/file-read-fi
  make && sudo insmod file-read-fi.ko
  
  # 配置并触发 (使用 load_fi 工具)
  sudo ./load_fi
  # 或手动读文件触发
  cat secret.txt
  ```

- **预期结果**:

  - 终端报错: `cat: secret.txt: Input/output error.`
  - `dmesg` 显示: `[ARM-Fi] vfs_read: Force buf=NULL.`

------

### 4. 内存缺页处理观测 (pt-update-fi)

- **功能**: 拦截 `handle_mm_fault`，验证内存管理路径的可观测性。

- **测试方法**:

  Bash

  ```
  # 编译与加载 (需修改目标为 handle_mm_fault)
  cd memory-fi/pt-update-fi
  make && sudo insmod pt-update-fi.ko
  
  # 触发 (高负载触发缺页)
  stress-ng --vm 1 --vm-bytes 128M -t 5s
  ```

- **预期结果**:

  - `dmesg` 频繁显示: `[PT-Fi] Intercepted handle_mm_fault!.`

------

### 5. KVM 状态查询故障 (kvm-state-fi)

- **功能**: 拦截 `kvm_vcpu_ioctl`，强制返回 `-EIO` 错误。

- **测试方法**:

  Bash

  ```
  # 编译与加载 (需修改目标为 kvm_vcpu_ioctl)
  cd state-query-fi/kvm-state-fi
  make && sudo insmod kvm-state-fi.ko
  
  # 触发
  qemu-system-aarch64 -nographic -M virt -enable-kvm
  ```

- **预期结果**:

  - QEMU 启动失败或崩溃。
  - `dmesg` 显示: `[ARM-State-Fi] Intercepted kvm_vcpu_ioctl -> Force Return -EIO.`

------

### 6. VM 热迁移故障 (vm-migration-fi)

- **功能**: 拦截 `kvm_vm_ioctl` (脏页日志获取)，阻断迁移流程。

- **测试方法**:

  Bash

  ```
  # 编译与加载 (需修改目标为 kvm_vm_ioctl)
  cd vm-migration-fi
  make && sudo insmod vm-migration-fi.ko
  
  # 触发 (使用 QEMU Monitor 伪迁移)
  # 在 QEMU Monitor 中输入: migrate "exec:cat > /dev/null"
  ```

- **预期结果**:

  - QEMU Monitor 提示 `migration failed`.
  - `dmesg` 显示: `[ARM-Mig-Fi] Intercepted kvm_vm_ioctl. Forcing -EIO.`

------

### 7. KVM 版本号故障 (kvm-version-fi)

- **功能**: 拦截 API Version 查询，将版本 12 篡改为 0。

- **测试方法**:

  Bash

  ```
  # 编译与加载
  cd state-query-fi/kvm-version-fi
  make && sudo insmod kvm-version-fi.ko
  
  # 触发
  qemu-system-aarch64 ... -enable-kvm
  ```

- **预期结果**:

  - QEMU 报错 `KVM version too old` 或初始化失败。
  - `dmesg` 显示: `[ARM-Ver-Fi] Intercepted KVM Version Query. Mutated 12 -> 0.`

------

### 8. 文件写入故障 (file-write-fi)

- **功能**: 拦截 `vfs_write`，注入 `Count=0` 或 `Buf=NULL`。

- **测试方法**:

  Bash

  ```
  # 编译与加载 (Makefile 需修正 obj-m 名称)
  cd file-fi/file-write-fi
  make && sudo insmod file-write-fi.ko
  
  # 配置并触发 (极快被消耗，需快速操作)
  sudo bash -c "echo 1 > /proc/file-write-fi/signal"
  echo "test" > testfile
  ```

- **预期结果**:

  - `dmesg` 显示: `[ARM-Fi-Write] vfs_write: Force buf=NULL.`
  - 若注入未被系统进程抢占，命令会报 `Bad address`。

------

### 9. 内存加载故障 (pt-load-fi)

- **功能**: 拦截 `handle_mm_fault`，强制返回 OOM (内存耗尽) 错误。

- **测试方法**:

  Bash

  ```
  # 编译与加载
  cd memory-fi/pt-load-fi
  make && sudo insmod pt-load-fi.ko
  
  # 触发 (危险操作)
  sudo bash -c "echo 0 > /proc/pt-load-fi/type" # Type 0 = OOM
  sudo bash -c "echo 1 > /proc/pt-load-fi/signal"
  ls -R /etc
  ```

- **预期结果**:

  - Shell 卡死、命令被 Kill 或者系统界面无响应。
  - `dmesg` 显示: `FORCE Return: VM_FAULT_OOM.`

------

### 10. 内存回收守护进程故障 (kswapd-fi)

- **功能**: 拦截 `shrink_node`，将参数置空，试图引发 Panic 或阻止回收。

- **测试方法**:

  Bash

  ```
  # 编译与加载 (Makefile 需修复)
  cd memory-manage-fi/kswapd-fi
  make && sudo insmod kswapd-fi.ko
  
  # 触发 (制造内存压力)
  dd if=/dev/zero of=/dev/shm/fillme bs=1M count=<MemSize>
  ```

- **预期结果**:

  - 若内核未能防御空指针，系统将重启（Kernel Panic）。
  - 若内核健壮，`dmesg` 显示 `Force shrink_node(NULL)`
