# Web 控制器 (FastAPI)

一个面向 Hadoop + VM + KVM 注入的中文 Web 控制台，通过 SSH 控制集群，覆盖 `hadoop_injector`、`vm_injection` 与 `kvm_injector` 功能。\n+说明：Hadoop 注入在三台 Linux 节点执行，其余注入在宿主机执行。

## 功能范围
- 集群状态与启动/停止/重启
- Hadoop 进程故障 (crash/hang/resume)
- 网络故障：延迟/抖动、丢包、乱序、隔离、心跳超时
- 资源故障：CPU 压力、内存压力、磁盘填满、I/O 限速
- HDFS/YARN：安全模式、磁盘不足、YARN 不健康
- MapReduce 任务故障
- VM 注入：进程/网络/CPU/内存泄漏/内存注入/寄存器注入
- KVM 注入：软错误、客户 OS 错误行为、性能故障、CPU 热插拔
- 输出限制：后端自动截断输出，避免页面卡顿

## 安装
```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -r web_controller/requirements.txt
```

## 配置
编辑 `web_controller/config.json`：

- `nodes`: master/slave IP、端口与角色
- `hadoop.home`: Hadoop 安装路径 (默认 `/opt/hadoop`)
- `hadoop.injector`: `hadoop_injector` 的绝对路径（当前配置：`/home/venele/grad_project/kvm_injection/hadoop-fi/hadoop_injector`）\n+  请确保该路径在 master 节点上也有效，或改成 master 实际路径
- `hadoop.use_sudo`: 若需要 root 且已配置免密 sudo，则设为 `true`

- `vm.*`: VM 注入工具路径（需与实际编译后的二进制一致）
  - `vm.process_injector`
  - `vm.network_injector`
  - `vm.cpu_injector`
  - `vm.mem_leak`
  - `vm.mem_injector`
  - `vm.reg_injector`
- `vm.use_sudo`: VM 注入是否需要 sudo

- `kvm.injector`: KVM 注入工具路径（当前配置：`/home/venele/grad_project/vm_injection/kvm_injector`）
- `kvm.use_sudo`: KVM 注入是否需要 sudo

- `output.max_lines/max_chars`: 输出截断限制

## 运行
```bash
uvicorn web_controller.app:app --host 0.0.0.0 --port 8080
```

浏览器访问：`http://<宿主机IP>:8080`

## 说明
- 控制器运行在宿主机，Hadoop 注入通过 SSH 在 master/slave 上执行。\n+- VM/KVM 注入默认在本机执行（scope=local），需保证宿主机上已编译对应工具。
- 输出为“截断后结果”，并附带执行命令与退出码，便于定位问题。
