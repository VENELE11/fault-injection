# 云平台故障注入工具集

本项目是一套面向云平台可靠性实验的多层次故障注入工具。当前代码同时覆盖 Kubernetes/Chaos Mesh、Hadoop、CloudStack、VM 用户态注入和 KVM/宿主机虚拟化层注入，目标是把原本分散的命令行注入器整合到可配置、可观测、可清理的实验平台中。

## 当前实现范围

| 层次 | 代码位置 | 主要能力 | 当前入口 |
| --- | --- | --- | --- |
| Web 控制面 | `web_controller/` | FastAPI 接口、中文前端、历史记录、功能测试编排 | `./start_frontend.sh` 或 `uvicorn web_controller.app:app --host 0.0.0.0 --port 8080` |
| Kubernetes / Chaos Mesh | `web_controller/k8s_chaos.py` | PodChaos、NetworkChaos、StressChaos、实验状态与清理 | Web 控制台、`/api/action`、`/api/functest` |
| Hadoop 场景注入 | `kvm_injection/hadoop-fi/` | 进程、网络、资源、HDFS/YARN、MapReduce 故障 | Web 单次动作、`hadoop_injector`、`cluster_controller` |
| CloudStack 场景注入 | `kvm_injection/cloudstack-fi/` | 管理组件、Agent、API、网络、存储、数据库、SystemVM 故障 | Web 单次动作、`cloudstack_injector`、`cluster_controller` |
| VM 用户态注入 | `vm_injection/` | 进程、网络、CPU、内存泄漏、内存篡改、寄存器注入 | Web 控制台、`fault_controller`、独立注入器 |
| KVM 虚拟化层注入 | `vm_injection/kvm_injector.c` 与 `kvm_injection/*` | 软错误、客户机异常行为、性能故障、CPU 热插拔、内核模块实验 | Web 控制台、`kvm_injector`、内核模块 |

说明：当前目录名已经统一为英文路径 `web_controller/`、`vm_injection/`、`kvm_injection/`。旧文档中出现的 `虚拟机注入/`、`kvm注入/` 对应现在的 `vm_injection/`、`kvm_injection/`。

## 目录结构

```text
.
├── web_controller/              # FastAPI 后端、静态前端、Chaos Mesh 命令构造、功能测试场景
├── vm_injection/                # VM/Guest 用户态注入器与 kvm_injector
├── kvm_injection/               # KVM 内核模块、Hadoop/CloudStack 注入器、CLI 集群控制器
├── docs/system_architecture.md  # 当前架构图和控制流说明
├── 使用须知.md                   # 环境准备、配置和常用启动流程
├── Hadoop 故障注入测试说明文档.md
├── Hadoop 集群环境恢复与启动手册.md
├── cloudstack测试.md
└── output/doc/答辩讲述要点与讲稿.md
```

## 快速启动 Web 控制台

```bash
cd /Users/venele/Downloads/fault-injection
python3 -m venv .venv
. .venv/bin/activate
pip install -r web_controller/requirements.txt
./start_frontend.sh
```

默认访问地址是 `http://<宿主机IP>:8080`。`start_frontend.sh` 会先尝试通过 `vm_injection/run_cluster.sh` 拉起 `master`、`slave1`、`slave2` 三台 QEMU 虚拟机，再启动 FastAPI。

也可以只启动后端：

```bash
uvicorn web_controller.app:app --host 0.0.0.0 --port 8080
```

## Web 控制器与功能测试

Web 后端读取 `web_controller/config.json`，通过本地命令或 SSH 执行注入器。主要接口：

| 接口 | 作用 |
| --- | --- |
| `/api/config` | 返回节点、动作分组、参数定义 |
| `/api/action` | 执行单个注入动作 |
| `/api/testcases` | 返回当前功能测试用例 |
| `/api/functest` | 执行 baseline -> action -> verify -> cleanup 流程 |
| `/api/history` | 查看历史运行记录 |
| `/api/health` | 健康检查 |

当前 `web_controller/test_scenarios.py` 中的功能测试用例以 Chaos Mesh、VM、KVM 为主。Hadoop 和 CloudStack 的单次动作仍保留在 `web_controller/app.py` 的 `ACTIONS` 中，可通过 Web 按钮或 `/api/action` 执行。

## Chaos Mesh 支持

当前代码通过 `web_controller/k8s_chaos.py` 直接生成并 `kubectl apply` Chaos Mesh CRD：

| 类型 | 资源 | 动作 |
| --- | --- | --- |
| Pod 故障 | `PodChaos` | `pod-kill`、`container-kill` |
| 网络故障 | `NetworkChaos` | 延迟、抖动、丢包、双向目标探针 |
| 资源故障 | `StressChaos` | CPU 压力、内存压力 |
| 管理动作 | `kubectl` | K8s 状态、演示应用部署/删除、Chaos 状态、Chaos 清理 |

默认目标是 `default` 命名空间中带有 `app=nginx-demo` 标签的 Pod。网络实验还使用 `app=fi-net-probe` 的探针 Pod 来观察延迟和丢包效果。`web_controller/config.json` 中的 `kubernetes.kubectl` 可以是普通 `kubectl`，也可以像当前配置一样写成远程命令，例如通过 SSH 调用 master 节点上的 `k3s kubectl`。

## 编译注入器

```bash
# VM 用户态注入器和 kvm_injector
cd vm_injection
make all

# KVM 内核模块、Hadoop、CloudStack 和 cluster_controller
cd ../kvm_injection
make all
```

内核模块依赖当前系统的 Linux headers。Ubuntu 上可安装：

```bash
sudo apt install -y build-essential gcc make linux-headers-$(uname -r) iproute2 iptables
```

## 常用命令入口

```bash
# Hadoop
cd kvm_injection/hadoop-fi
./hadoop_injector list
sudo ./hadoop_injector crash dn
sudo ./hadoop_injector delay slave1 200 50
sudo ./hadoop_injector isolate slave1
sudo ./hadoop_injector hdfs-safe enter

# CloudStack
cd kvm_injection/cloudstack-fi
sudo ./cloudstack_injector list
sudo ./cloudstack_injector hang agent
sudo ./cloudstack_injector api-delay 1000
sudo ./cloudstack_injector network 192.168.1.11 8250
sudo ./cloudstack_injector storage-ro /tmp/cs_secondary

# VM / Guest
cd vm_injection
sudo ./network_injector 1 200ms
sudo ./process_injector nginx 2
sudo ./cpu_injector 0 20 4
sudo ./mem_leak 0 512

# KVM
sudo ./kvm_injector list
sudo ./kvm_injector soft-flip master PC 10
sudo ./kvm_injector perf-delay slave1 50
sudo ./kvm_injector clear
```

## 风险提示

这些工具会修改进程状态、网络规则、cgroup、iptables、KVM 相关行为，部分内核模块可能触发宿主机 Kernel Panic。请只在实验环境运行，注入前先准备快照或可重建镜像；网络、磁盘、cgroup 类故障结束后务必执行清理命令。

更多细节见：

- `使用须知.md`
- `web_controller/README.md`
- `docs/system_architecture.md`
- `Hadoop 故障注入测试说明文档.md`
- `cloudstack测试.md`
