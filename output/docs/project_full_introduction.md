# 云平台故障注入工具代码与配置说明（实现原理版）

本文只介绍项目中的代码与配置文件，不介绍论文、截图、参考文献、答辩材料、Word/PDF 产物，也不介绍 `vm_injection/靶子程序/` 下的靶子程序。靶子程序虽然也是 C 代码，但按本次要求已从说明范围中移除。

## 1. 代码范围

本版覆盖以下代码与配置：

| 范围 | 文件类型 |
| --- | --- |
| 顶层启动与状态 | `start_frontend.sh`、`.active_faults.json` |
| Web 控制面 | `web_controller/*.py`、`web_controller/*.json`、`requirements.txt`、`static/*.html/js/css`、`tests/*.py` |
| VM 用户态注入器 | `vm_injection/*.c`、`Makefile`、启动脚本；不含 `靶子程序/` |
| KVM/集群注入器 | `kvm_injection/**/*.c`、`Makefile`、`cluster.conf`、`cluster_manage.sh` |

不纳入范围：

- 顶层 Markdown 使用说明、论文相关 Markdown。
- `output/`、`tmp/`、`截图/` 中的论文、图片、参考文献和文档处理产物。
- `.venv/`、`.pytest_cache/`、`__pycache__/`、`.DS_Store` 等缓存或本地环境文件。
- `vm_injection/靶子程序/`。

## 2. 总体架构与实现原理

项目采用“Web 控制面 + 用户态注入器 + 内核探针注入器 + 集群专项注入器”的分层设计。

```text
浏览器前端
  -> FastAPI 后端
      -> Kubernetes/Chaos Mesh 命令
      -> VM 用户态注入器
      -> KVM/QEMU 用户态注入器
      -> Hadoop/CloudStack 专项注入器
      -> KVM 内核模块控制程序
  -> SQLite 历史库
```

### 2.1 Web 控制面原理

Web 层不直接实现底层故障，而是做四件事：

1. 读取 `web_controller/config.json`，获得节点、路径、sudo、K8s、Hadoop 等运行配置。
2. 将前端参数映射为命令模板或动态构造函数。
3. 在本机执行命令，或通过 SSH 在远程节点执行命令。
4. 收集 stdout、stderr、退出码、耗时、检查结果，并写入 SQLite 历史库。

核心设计点：

- `ACTIONS` 定义单次动作。
- `FUNC_TESTS` 定义完整实验流程。
- `run_command()` 是所有本地命令的统一执行出口。
- `run_on_node()` 负责本地/远程分发。
- `db.py` 将运行结果拆成 run 主记录和阶段 result 明细。

### 2.2 Kubernetes 故障原理

Kubernetes 层使用 Chaos Mesh，不手写控制器。后端通过 `k8s_chaos.py` 生成 Chaos Mesh 自定义资源：

- `PodChaos`：杀死 Pod。
- `ContainerKill` 或等价容器级 chaos：杀死容器并观察重启。
- `NetworkChaos`：注入延迟、丢包等网络异常。
- `StressChaos`：注入 CPU/内存压力。

实现链路：

```text
前端参数
  -> k8s_chaos.py 生成 manifest
  -> kubectl apply -f -
  -> Chaos Mesh controller/daemon 执行故障
  -> kubectl get/top/log/curl/ping 做前后检查
```

这种实现的好处是复用成熟 Chaos Mesh 能力，Web 后端只负责参数化和实验编排。

### 2.3 VM 用户态故障原理

`vm_injection/` 的注入器主要工作在 Linux 用户态：

| 故障类型 | 技术原理 |
| --- | --- |
| 进程 crash/hang/resume | 向目标 PID 发送 `SIGKILL`、`SIGSTOP`、`SIGCONT` 等信号。 |
| 网络延迟/丢包 | 使用 `tc qdisc netem` 修改网卡队列规则。 |
| CPU 压力 | 创建多个忙循环线程消耗 CPU。 |
| 内存泄漏/内存压力 | 循环 `malloc()` 并触摸页面，制造真实物理内存占用。 |
| 进程内存篡改 | 使用 `ptrace(PTRACE_ATTACH/PEEKDATA/POKEDATA)` 读写目标进程地址空间。 |
| 寄存器篡改 | 使用 `ptrace` 获取和写回 ARM64 `user_pt_regs`。 |
| QEMU/KVM 目标识别 | 遍历 `/proc/<pid>/cmdline`，识别 `qemu-system-*`、`-name` 和 qcow2 镜像名。 |

### 2.4 Hadoop/CloudStack 故障原理

Hadoop 和 CloudStack 注入器面向分布式系统组件，不修改组件源码，而是通过系统行为模拟故障：

- 进程故障：组件进程 crash、hang、resume。
- 网络故障：延迟、丢包、乱序、隔离。
- 资源故障：CPU、内存、磁盘填满、IO 变慢。
- 服务语义故障：HDFS 安全模式、YARN unhealthy、CloudStack API 延迟、数据库连接限制、存储只读、SystemVM 异常。
- 多节点执行：本地执行或 SSH 到指定节点执行同一注入器。

### 2.5 KVM 内核层故障原理

`kvm_injection/` 中的内核模块采用 Linux kprobe/kretprobe 机制。它和 Web、K8s、VM 用户态、Hadoop、CloudStack 一样，都是项目的一层故障实现方式；区别在于它的注入点位于内核函数调用路径。

#### kprobe

`kprobe` 在目标内核函数执行前触发 pre-handler。项目中用于拦截：

- `kernel_clone`
- `kvm_vm_ioctl`
- `vfs_read`
- `vfs_write`
- `handle_mm_fault`
- `kvm_set_memory_region`
- `gfn_to_hva_many`
- `shrink_node`

典型流程：

```text
insmod xxx.ko
  -> module_init 注册 kprobe
  -> 用户态 main 写 /proc 控制参数
  -> 内核执行目标函数
  -> handler_pre 读取 pt_regs 和控制参数
  -> 按故障类型修改寄存器/参数/计数/状态
  -> 目标函数继续执行
```

#### kretprobe

`kretprobe` 在目标内核函数返回时触发 ret-handler，适合修改返回值或观察返回状态。项目中用于拦截：

- `handle_mm_fault`
- `kvm_vcpu_ioctl`
- `kvm_dev_ioctl`
- `kvm_vm_ioctl`

典型流程：

```text
目标函数进入
  -> entry_handler 可记录上下文
目标函数返回
  -> ret_handler 读取返回值
  -> 按配置修改返回寄存器或记录异常
```

#### procfs 控制面

多数内核模块都会创建 `/proc/<模块名>/...` 控制文件。用户态 `*-main.c` 程序把命令行参数写入这些文件，内核 handler 再读取全局配置。

常见控制项：

| 控制项 | 含义 |
| --- | --- |
| `signal` / `sig` | 是否启用故障。 |
| `times` / `time` | 触发次数或第几次触发。 |
| `type` | 故障类型，例如翻转、置零、置一、返回错误。 |
| `pos` | 注入位置，例如 bit 位置、参数位置。 |
| `style` | 注入风格，例如固定、随机。 |
| `class` | 故障类别。 |

这种设计把“控制逻辑”放在用户态，把“拦截与篡改”放在内核态，便于实验时动态调整参数。

### 2.6 各部分实现原理展开

本项目每一部分都有自己的“注入点”和“控制面”。不能只把 KVM 的 kprobe 看成原理，其他部分也都有明确的实现机制。

| 部分 | 控制入口 | 注入点 | 实现方式 | 观测/清理方式 |
| --- | --- | --- | --- | --- |
| 顶层启动 | `start_frontend.sh` | QEMU VM 启动流程、Web 服务进程 | Shell 检查进程表、启动 VM、启动 uvicorn | VM 日志、FastAPI health |
| Web 后端 | `app.py` API | 命令执行流程 | FastAPI + Pydantic + 命令模板 + SSH + subprocess | stdout/stderr、退出码、SQLite |
| 前端 | `static/app.js` | 用户交互与结果解释 | 配置驱动渲染、fetch API、日志解析、指标卡片 | 页面历史、前后对比 |
| 历史库 | `db.py` | 实验结果持久化 | SQLite 主从表结构，run/result 拆分 | `/api/history`、历史页 |
| K8s | `k8s_chaos.py` | Pod、容器、网络、资源 | 生成 Chaos Mesh CRD manifest，`kubectl apply -f -` | `kubectl get/top`、curl/ping、删除 Chaos 对象 |
| VM 进程故障 | `process_injector.c` | Linux 进程状态 | `kill()` 信号：crash/hang/resume | `ps`、进程状态、恢复信号 |
| VM 网络故障 | `network_injector.c` | Linux 网卡队列 | `tc qdisc netem` 延迟/丢包 | `tc qdisc del`、ping/curl |
| VM 资源故障 | `cpu_injector.c`、`memleak_injector.c` | CPU 调度、物理内存 | 忙循环线程、内存分配并触摸页面 | 结束注入进程、资源监控 |
| VM 内存/寄存器故障 | `mem_injector.c`、`reg_injector.c` | 目标进程地址空间和寄存器 | `ptrace` attach/read/write/detach | 目标输出、异常退出、手工恢复 |
| QEMU/KVM 用户态 | `kvm_injector.c` | QEMU 进程、guest 行为 | `/proc/<pid>/cmdline` 识别 VM，组合信号、压力、辅助工具 | VM 列表、进程状态、清理函数 |
| Hadoop | `hadoop_injector.c` | HDFS/YARN/MapReduce 组件 | SSH、信号、tc、磁盘/IO/资源命令、Hadoop CLI | HDFS/YARN 命令、jps、清理命令 |
| CloudStack | `cloudstack_injector.c` | 管理服务、Agent、DB、存储、SystemVM | 信号、tc、数据库/存储命令、资源压力 | CloudStack 状态命令、恢复命令 |
| KVM 内核模块 | `*.ko` + `*-main.c` | KVM/VFS/MM 内核函数 | kprobe/kretprobe + procfs 控制 + `pt_regs` 修改 | dmesg、procfs、rmmod |
| 测试 | `tests/*.py` | 后端函数和 API | pytest、TestClient、mock config、临时 SQLite | 测试断言、临时目录清理 |

#### 顶层启动实现原理

`start_frontend.sh` 的核心不是简单运行 Web 服务，而是先保证实验环境可用。它用进程表判断三台 VM 是否已经运行，避免重复启动；如果缺失，就调用 `vm_injection/run_cluster.sh` 后台启动。最后再启动 uvicorn。这里的关键点是“VM 生命周期”和“Web 生命周期”被放进同一个启动入口，用户只需执行一个脚本。

#### 配置驱动实现原理

`web_controller/config.json` 把环境差异从代码里抽出来。节点 IP、SSH 端口、用户名、工具路径、sudo 策略、K8s namespace、Hadoop 命令都不写死在函数里，而是在运行时读取。`build_context()` 再把配置与用户参数合并。这样同一套 Web 后端可以迁移到不同机器，只要改配置文件。

#### Web 后端实现原理

后端采用“声明式动作 + 统一执行器”：

1. `ACTIONS` 和 `FUNC_TESTS` 描述能做什么。
2. 参数声明告诉前端如何生成表单，也告诉后端如何校验。
3. 命令可以是字符串模板、命令列表或 Python 构造函数。
4. `normalize_cmds()` 将不同来源统一为命令列表。
5. `run_command()` 和 `run_on_node()` 执行命令。
6. 结果统一为 dict，包含 command、stdout、stderr、returncode、duration、timeout。

这种设计的好处是新增场景时不需要新增一整套 API。多数情况下只需要添加动作定义或测试定义。

#### 前端实现原理

前端不是固定页面，而是“后端配置驱动页面”：

1. 页面加载时调用 `/api/config` 和 `/api/testcases`。
2. 根据分组和场景定义动态创建卡片。
3. 根据参数 schema 动态创建 input/select/checkbox。
4. 用户点击运行后，前端收集参数并调用 API。
5. API 返回结构化结果后，前端解析 stdout/stderr 中的指标。
6. 指标被渲染为网络、资源、K8s、HDFS、MapReduce 等 focus card。

因此前端的核心工作不是写死实验按钮，而是把后端声明转换成可操作界面，并把命令日志转换成可理解结果。

#### 历史库实现原理

历史库采用 run/result 两层结构：

- `fault_runs` 表保存一次实验的概要：标题、分组、参数、状态、时间。
- `fault_results` 表保存每个阶段的命令结果：before、inject、after、cleanup。

这种结构适合故障注入实验，因为一次实验通常不是单条命令，而是“前置检查 -> 注入 -> 等待 -> 后置检查 -> 清理”的序列。

#### Kubernetes 实现原理

K8s 部分把故障抽象交给 Chaos Mesh：

- Web 参数决定 selector、namespace、duration、故障强度。
- `k8s_chaos.py` 生成 Chaos Mesh CRD。
- `kubectl apply -f -` 把 manifest 交给 Kubernetes。
- Chaos Mesh controller 负责调度，daemon 在节点上执行实际故障。
- 清理时删除 Chaos 对象，控制器撤销故障。

也就是说，项目自己不实现 Pod kill 或网络丢包底层逻辑，而是实现 Chaos Mesh 的“配置生成、执行编排和结果观测”。

#### VM 用户态实现原理

VM 用户态部分按 Linux 能力拆分：

- 进程状态用 signal 控制。
- 网络队列用 `tc netem` 控制。
- CPU 资源用 busy loop 消耗。
- 内存资源用分配并触摸页面消耗。
- 进程内存和寄存器用 `ptrace` 修改。
- VM 目标用 QEMU 进程命令行识别。

这种方式的特点是部署轻，不需要加载内核模块；缺点是故障注入点主要位于用户态或宿主机可观测层。

#### Hadoop 实现原理

Hadoop 注入器把分布式系统故障拆成组件级动作：

- NameNode、DataNode、ResourceManager、NodeManager 通过进程名识别。
- 组件 crash/hang/resume 通过信号实现。
- 节点间网络异常通过 `tc` 或隔离规则实现。
- HDFS 语义异常通过安全模式、磁盘填满、IO 变慢等方式实现。
- YARN 异常通过 NodeManager unhealthy、心跳超时等方式实现。
- MapReduce 异常通过 map/reduce task 进程故障实现。

重点是：它模拟的是 Hadoop 组件真实运行时会遇到的外部条件，而不是修改 Hadoop 源码。

#### CloudStack 实现原理

CloudStack 注入器围绕云管理平台的关键依赖设计：

- 管理进程和 agent 用进程信号模拟 crash/hang。
- API 延迟模拟控制平面响应变慢。
- 数据库限制/锁模拟管理服务依赖异常。
- 存储卸载、只读、填满模拟主存储/二级存储异常。
- SystemVM 故障模拟虚拟路由器、console proxy、secondary storage VM 异常。
- VM 创建/迁移失败模拟云平台生命周期操作失败。

它的核心思路是从 CloudStack 的依赖链入手，而不是只杀进程。

#### KVM 内核模块实现原理

KVM 内核模块的共同结构是：

1. `module_init` 创建 procfs 控制项。
2. 注册 kprobe 或 kretprobe 到目标内核函数。
3. 用户态 `*-main.c` 写 procfs 参数。
4. handler 根据参数决定是否触发故障。
5. 故障可能修改 `pt_regs`、返回值、计数状态，或只记录日志。
6. `module_exit` 注销探针并清理 procfs。

这和 VM 用户态的 signal/ptrace 不同：KVM 模块直接作用在内核函数路径上，因此能模拟更底层的虚拟化、文件、内存管理、状态查询异常。

#### 构建系统实现原理

项目同时包含普通用户态程序和内核模块，因此有两类构建方式：

- 用户态 C 程序：普通 `gcc` 编译，生成可执行文件。
- 内核模块：通过 `/lib/modules/$(uname -r)/build` 的 Kbuild 编译，生成 `.ko`。

顶层 Makefile 负责递归调度，子目录 Makefile 负责具体目标。

#### 测试实现原理

后端测试不依赖真实集群，而是通过 mock 配置和临时 SQLite 验证逻辑：

- monkeypatch 配置路径。
- 用临时目录隔离历史库。
- 用 FastAPI TestClient 调 API。
- 对命令构造、参数校验、QEMU 解析、历史写入做单元测试。

这种测试覆盖的是控制逻辑，不覆盖真实故障效果。真实故障效果仍需要在实验环境中集成验证。

## 3. 顶层代码与配置

### 3.1 `start_frontend.sh`

启动 Web 控制台前的总入口脚本。它负责先启动 VM 集群，再启动 FastAPI。

| 函数/逻辑 | 作用 |
| --- | --- |
| `set -euo pipefail` | 遇到未定义变量、命令失败、管道失败立即退出，避免半启动状态。 |
| `ROOT_DIR` | 解析项目根目录。 |
| `VM_DIR` | 指向 `vm_injection/`，用于调用 VM 启动脚本。 |
| `LOG_DIR` | 保存虚拟机启动日志。 |
| `is_vm_running(node)` | 通过 `ps -ww -eo args=` 搜索 QEMU 命令行，匹配 `-name` 或镜像名，判断指定 VM 是否已经运行。 |
| `start_vm(node)` | 若 VM 未运行，则进入 `vm_injection/`，后台执行 `run_cluster.sh <node>` 并写日志。 |
| uvicorn 查找 | 优先使用 `.venv/bin/uvicorn`，找不到时使用系统 `uvicorn`。 |
| 服务启动 | 执行 `uvicorn web_controller.app:app --host 0.0.0.0 --port 8080`。 |

实现原理：脚本先保证实验 VM 存在，再提供 Web 控制面。QEMU 识别采用命令行特征匹配，因此 VM 的 `-name` 和镜像命名必须保持稳定。

### 3.2 `.active_faults.json`

当前为空数组 `[]`。它是一个预留状态文件，语义上用于记录活动故障。当前实际历史持久化由 `web_controller/db.py` 的 SQLite 实现，因此该文件不是主状态源。

## 4. `web_controller/` 代码与配置

### 4.1 `web_controller/config.json`

Web 控制台运行配置。它决定后端如何找到节点、工具路径和默认参数。

| 配置块 | 作用 |
| --- | --- |
| `cluster` | 节点定义，包括名称、IP、SSH 端口、用户、角色、是否本机。 |
| `paths` | 注入器、脚本、Hadoop/CloudStack 工具路径。 |
| `sudo` | 是否使用 sudo，以及是否使用非交互式 sudo。 |
| `hadoop` | Hadoop 用户、组件名、启动/停止命令、HDFS/YARN 检查命令。 |
| `k8s` | Kubernetes namespace、Chaos Mesh namespace、kubectl 路径、示例应用选择器。 |
| `defaults` | 前端和后端共用默认值，如默认节点、默认时长、默认延迟、默认丢包率。 |

实现原理：`app.py` 每次通过 `load_config()` 读取 JSON，再由 `build_context()` 把配置与请求参数合并为命令模板上下文。

### 4.2 `web_controller/requirements.txt`

Python 依赖配置：

| 依赖 | 用途 |
| --- | --- |
| `fastapi` | Web API 框架。 |
| `uvicorn` | ASGI 运行器。 |
| `pydantic` | 请求模型和字段校验。 |
| `pytest` | 单元测试。 |
| `httpx`/测试依赖 | FastAPI TestClient 和接口测试支持。 |

### 4.3 `web_controller/__init__.py`

包初始化文件。当前无业务逻辑，用于让 `web_controller` 成为 Python package，支持 `uvicorn web_controller.app:app` 和 pytest 导入。

### 4.4 `web_controller/app.py`

FastAPI 主程序，是 Web 控制面的核心。

#### 4.4.1 全局结构

| 名称 | 作用 |
| --- | --- |
| `app` | FastAPI 应用实例。 |
| `ROOT` | `web_controller/` 目录。 |
| `CONFIG_PATH` | 默认配置文件路径。 |
| `STATIC_DIR` | 静态前端目录。 |
| `MAX_OUTPUT_CHARS` | stdout/stderr 最大保留长度。 |
| `GROUPS` | 前端分组定义。 |
| `PARAM_ENUMS` | 枚举参数白名单。 |
| `NUM_RANGES` | 数值参数合法范围。 |
| `ACTIONS` | 单次故障动作定义表。 |

实现原理：`ACTIONS` 将“页面按钮”抽象成“参数声明 + 命令构造 + 清理命令 + 检查命令”。前端无需硬编码底层命令，只需渲染后端返回的动作/测试定义。

#### 4.4.2 请求模型

| 类 | 作用 |
| --- | --- |
| `ActionRequest` | `/api/action` 请求模型，包含 `action` 和 `params`。 |
| `FuncTestRequest` | `/api/functest` 请求模型，包含 `test_id`、`params`、`nodes`、`sudo` 等。 |

Pydantic 会在路由入口完成 JSON 解析和基础类型转换，后续再由业务校验函数做安全校验。

#### 4.4.3 配置与节点函数

| 函数 | 作用 |
| --- | --- |
| `load_config()` | 读取并解析 `config.json`。 |
| `get_nodes(config)` | 返回配置中的节点列表。 |
| `get_master_node(config)` | 查找 master 节点，失败时回退第一个节点。 |
| `get_worker_nodes(config)` | 返回 worker 节点。 |
| `is_local_node(node)` | 判断节点命令应本机执行还是 SSH 执行。 |
| `build_ssh_command(node, command)` | 构造 SSH 远程执行命令。 |
| `find_node_by_value(config, value)` | 按节点名、显示名、IP 等查找节点。 |
| `resolve_test_nodes(config, test, request_nodes)` | 解析功能测试的目标节点集合。 |
| `resolve_test_sudo(config, test, request_sudo)` | 解析功能测试是否启用 sudo。 |

实现原理：节点抽象屏蔽了本地与远程差异。上层只关心“在哪个节点执行”，底层由 `run_on_node()` 决定是否包一层 SSH。

#### 4.4.4 命令执行函数

| 函数 | 作用 |
| --- | --- |
| `sanitize_output(text)` | 清洗命令输出中的不可显示控制字符。 |
| `truncate_text(text, limit)` | 截断超长输出，避免响应过大。 |
| `run_command(command, cwd=None, timeout=None, env=None)` | 执行本地命令，记录退出码、输出、耗时、超时状态。 |
| `run_on_node(node, command, timeout=None)` | 在本地或远程节点执行命令。 |
| `maybe_sudo(command, enabled)` | 根据配置给命令加 sudo 前缀。 |
| `resolve_sudo(config, params)` | 合并全局 sudo 和请求参数。 |

实现原理：所有命令都统一返回结构化结果，前端和历史库不需要理解每个注入器的输出格式。

#### 4.4.5 模板与上下文函数

| 函数/类 | 作用 |
| --- | --- |
| `_SafeFormat` | 模板缺失字段时保留 `{key}`，便于调试。 |
| `render_template(template, context)` | 渲染命令模板。 |
| `normalize_cmds(cmds, context)` | 将字符串、列表、函数形式的命令统一转换为命令列表。 |
| `build_context(config, params)` | 合并配置、节点、路径、默认值和用户参数。 |

实现原理：静态命令用模板，复杂命令用构造函数。两者通过 `normalize_cmds()` 统一为执行列表。

#### 4.4.6 参数校验函数

| 函数 | 作用 |
| --- | --- |
| `validate_ip(value)` | 校验 IP/host 参数，减少命令注入风险。 |
| `validate_target(value)` | 校验进程名、节点名、VM 名等目标参数。 |
| `validate_hex(value)` | 校验十六进制地址或模式。 |
| `validate_kvm_target(config, params)` | 检查 KVM 目标 VM 是否存在且运行。 |
| `_ensure_vm_running(config, vm_name)` | 执行 KVM 故障前确认 VM 运行。 |

实现原理：Web 参数最终会进入 shell 命令，因此必须在进入模板前限制字符集、枚举范围和数值范围。

#### 4.4.7 QEMU/KVM 识别函数

| 函数 | 作用 |
| --- | --- |
| `_ps_aux()` | 获取进程列表。 |
| `_normalize_qemu_vm_name(name)` | 规范化 VM 名，去掉镜像前缀/后缀。 |
| `_is_qemu_args(args)` | 判断命令行是否属于 QEMU。 |
| `_extract_qemu_vm_name(args)` | 从 `-name` 或 qcow2 路径提取 VM 名。 |
| `_qemu_args_match_node(args, node)` | 判断 QEMU 参数是否匹配配置节点。 |
| `_iter_qemu_args()` | 遍历所有 QEMU 进程参数。 |
| `_is_vm_running(node_or_name)` | 判断 VM 是否运行。 |

实现原理：QEMU 没有统一的“项目级 VM registry”，所以后端通过 `/proc`/`ps` 命令行反推 VM 身份。

#### 4.4.8 Hadoop 与恢复计划函数

| 函数 | 作用 |
| --- | --- |
| `_build_hadoop_daemon_cmd(component, action, config)` | 构造 Hadoop 守护进程启动/停止命令。 |
| `_hadoop_daemon_step(node, component, action, config)` | 封装单个 daemon 操作步骤。 |
| `_build_hadoop_start_plan(config, params)` | 构造 Hadoop 启动计划。 |
| `_build_hadoop_stop_plan(config, params)` | 构造 Hadoop 停止计划。 |
| `_build_hadoop_restart_plan(config, params)` | 构造 Hadoop 重启计划。 |
| `_build_process_restart_plan(config, params)` | 构造进程恢复计划。 |
| `_build_process_restart_cmds(config, params)` | 将恢复计划转为命令列表。 |

实现原理：恢复操作不是单条命令，而是多节点、多组件的顺序计划。计划函数把“恢复语义”拆成可执行步骤。

#### 4.4.9 VM/KVM 命令构造函数

| 函数 | 作用 |
| --- | --- |
| `_build_vm_cpu_start_cmd(config, params)` | 构造 CPU 压力注入命令。 |
| `_build_vm_cpu_clear_cmd(config, params)` | 构造 CPU 压力清理命令。 |
| `_build_vm_mem_leak_start_cmd(config, params)` | 构造内存泄漏注入命令。 |
| `_build_vm_mem_leak_clear_cmd(config, params)` | 构造内存泄漏清理命令。 |
| `_build_vm_mem_inject_cmd(config, params)` | 构造 ptrace 内存注入命令。 |
| `_build_vm_reg_inject_cmd(config, params)` | 构造 ptrace 寄存器注入命令。 |
| `_build_kvm_soft_cmd(config, params)` | 构造 KVM/QEMU 软故障命令。 |

实现原理：这些函数把 Web 参数转换为底层 C 程序 argv，并集中处理路径、sudo、PID/VM 名等差异。

#### 4.4.10 功能测试与历史函数

| 函数 | 作用 |
| --- | --- |
| `collect_tests()` | 汇总 `test_scenarios.py` 中的 `FUNC_TESTS`。 |
| `_run_check_cmds(node, checks, timeout)` | 执行 before/after/cleanup 检查命令。 |
| `persist_history_safely(payload)` | 尝试写入历史库，失败不阻断主流程。 |

#### 4.4.11 HTTP 路由

| 路由函数 | URL | 作用 |
| --- | --- | --- |
| `index()` | `GET /` | 返回主页面。 |
| `history_page()` | `GET /history` | 返回历史页面。 |
| `api_config()` | `GET /api/config` | 返回配置、分组、动作、节点。 |
| `api_testcases()` | `GET /api/testcases` | 返回功能测试场景。 |
| `api_functest()` | `POST /api/functest` | 执行完整功能测试。 |
| `api_functest_cleanup()` | `POST /api/functest/cleanup` | 执行测试清理命令。 |
| `api_action()` | `POST /api/action` | 执行单次动作。 |
| `api_test()` | `POST /api/test` | 快速测试接口。 |
| `api_health()` | `GET /api/health` | 服务健康检查。 |
| `api_history()` | `GET /api/history` | 历史列表。 |
| `api_history_detail(run_id)` | `GET /api/history/{run_id}` | 历史详情。 |
| `api_history_clear()` | `DELETE /api/history` | 清空历史。 |
| `auto_start_vms()` | VM 启动相关接口 | 尝试启动 VM。 |

维护注意：前端 `runRecoveryAll()` 当前请求 `/api/recover/all`，但后端未见对应路由；若按钮需要可用，应补路由或修改前端调用。

### 4.5 `web_controller/db.py`

SQLite 历史库访问层。

| 函数 | 作用 |
| --- | --- |
| `get_db_path()` | 返回 SQLite 文件路径。 |
| `utc_now()` | 返回 UTC 时间戳。 |
| `ts_to_iso(ts)` | 时间戳转 ISO 字符串。 |
| `to_json(value)` | Python 对象转 JSON 字符串。 |
| `from_json(text, default)` | JSON 字符串转对象，失败时返回默认值。 |
| `connect()` | 创建 SQLite 连接并设置 row factory。 |
| `init_db()` | 创建 `fault_runs`、`fault_results` 和索引。 |
| `_insert_results(conn, run_id, results)` | 批量写入阶段结果。 |
| `record_run(run)` | 写入一次运行主记录和结果明细。 |
| `_run_from_row(row, include_results=False)` | 将数据库行转换为 API dict。 |
| `list_runs(limit, offset, run_type=None, group=None)` | 分页读取历史。 |
| `get_run(run_id)` | 读取单条历史详情。 |
| `clear_runs()` | 清空历史表。 |

实现原理：一次实验由一条 `fault_runs` 主记录和多条 `fault_results` 阶段记录组成，适合展示 before/inject/after/cleanup 的完整链路。

### 4.6 `web_controller/k8s_chaos.py`

Kubernetes/Chaos Mesh 命令构造器。

| 函数 | 作用 |
| --- | --- |
| `_kubectl(config)` | 返回 kubectl 命令路径。 |
| `_shell_cmd(cmd)` | 规范化 shell 命令文本。 |
| `_duration(params)` | 解析 Chaos 持续时间。 |
| `_selector(params)` | 构造 Chaos Mesh selector。 |
| `_network_probe_target(params)` | 构造网络探测目标。 |
| `_metadata(kind, name, namespace)` | 构造 Kubernetes metadata。 |
| `_manifest_apply_cmd(config, manifest)` | 生成 `kubectl apply -f -` 命令。 |
| `k8s_status_cmds(config, params)` | 构造状态检查命令。 |
| `k8s_demo_deploy_cmds(config, params)` | 构造示例应用部署命令。 |
| `k8s_demo_delete_cmds(config, params)` | 构造示例应用删除命令。 |
| `pod_kill_cmds(config, params)` | 构造 Pod kill chaos。 |
| `container_kill_cmds(config, params)` | 构造容器 kill chaos。 |
| `network_delay_cmds(config, params)` | 构造网络延迟 chaos。 |
| `network_loss_cmds(config, params)` | 构造网络丢包 chaos。 |
| `cpu_stress_cmds(config, params)` | 构造 CPU 压力 chaos。 |
| `memory_stress_cmds(config, params)` | 构造内存压力 chaos。 |
| `chaos_status_cmds(config, params)` | 构造 Chaos 对象状态查询。 |
| `chaos_clear_cmds(config, params)` | 构造 Chaos 清理命令。 |

实现原理：所有 Chaos 都被转换为 Kubernetes YAML，再通过 stdin 传给 `kubectl apply -f -`。这避免引入 Kubernetes Python SDK，也让手工调试命令与后端执行命令一致。

### 4.7 `web_controller/test_scenarios.py`

功能测试场景定义。

| 名称 | 作用 |
| --- | --- |
| `HDFS_CMD` | HDFS 命令前缀。 |
| `YARN_CMD` | YARN 命令前缀。 |
| `CLOUDSTACK_STATUS_CMD` | CloudStack 状态检查命令。 |
| `KVM_QEMU_STATUS_CMD` | QEMU/KVM 状态检查命令。 |
| K8s 命令常量 | kubectl、curl、资源检查、Pod selector 等片段。 |
| `k8s_wait_chaos_applied_cmd(kind)` | 生成等待 Chaos 生效的 shell 命令。 |
| `LEGACY_FUNC_TESTS` | 保存旧版 Hadoop/CloudStack/VM/KVM 场景。 |
| `FUNC_TESTS` | 当前导出的功能测试列表。 |

实现原理：场景定义是声明式 dict。每个测试包含参数、前置检查、注入命令、等待、后置检查、清理和预期现象。`app.py` 不需要知道每个场景的业务细节，只按统一流程执行。

维护注意：文件先定义旧版 `FUNC_TESTS`，再保存到 `LEGACY_FUNC_TESTS`，之后重写为 K8s 场景并追加旧版 VM/KVM 场景。因此旧版 Hadoop/CloudStack 功能测试未进入最终导出列表。

### 4.8 `web_controller/static/index.html`

主控制台 HTML 结构。

| 区域 | 作用 |
| --- | --- |
| 顶部状态区 | 展示系统标题、健康状态、快捷入口。 |
| 节点区 | 由 JS 渲染节点卡片。 |
| 场景区 | 由 JS 根据后端 `GROUPS`/`FUNC_TESTS` 渲染。 |
| 参数表单容器 | 由 `renderField()` 动态生成控件。 |
| 运行历史区 | 展示当前会话的执行结果。 |
| datalist | 提供节点名、namespace 等输入提示。 |

实现原理：HTML 保持轻结构，动态内容都由 `app.js` 根据 API 返回值生成。

### 4.9 `web_controller/static/history.html`

历史页 HTML 结构。

| 区域 | 作用 |
| --- | --- |
| 工具栏 | 刷新、清空、返回控制台。 |
| 过滤器 | 按类型和分组筛选。 |
| 列表容器 | 展示历史运行记录。 |
| 详情容器 | 展示阶段命令和输出。 |

### 4.10 `web_controller/static/app.js`

主控制台前端逻辑。

#### 状态与工具函数

| 函数/对象 | 作用 |
| --- | --- |
| `state` | 保存配置、测试、节点、分组、运行状态。 |
| `GROUP_ICONS` | 分组图标映射。 |
| `UTILITY_ACTIONS` | 前端固定工具动作。 |
| `elc(tag, className, text)` | 创建 DOM 元素。 |
| `escapeHtml(value)` | HTML 转义。 |
| `escapeRegExp(value)` | 正则转义。 |
| `fetchJson(url, options)` | fetch + JSON + 错误处理。 |
| `appendHistory(entry)` | 添加前端运行历史。 |
| `renderHistoryMessage(message)` | 渲染历史提示。 |

#### 输出解析函数

| 函数 | 作用 |
| --- | --- |
| `parsePingStats(text)` | 解析 ping 丢包和 RTT。 |
| `parseLoadavg(text)` | 解析 loadavg。 |
| `parseFreeUsedMb(text)` | 解析 `free -m` 已用内存。 |
| `parseFreeMemStats(text)` | 解析内存 total/used/free/available。 |
| `parseMaxCpuPercent(text)` | 解析最大 CPU 百分比。 |
| `parseDdThroughputMBps(text)` | 解析 dd 吞吐。 |
| `getCheckOutput(result, name)` | 取指定检查输出。 |
| `getCheckMergedOutput(result, phase)` | 合并阶段输出。 |
| `mergeResultOutputs(results)` | 合并结果输出。 |
| `countReadyPods(text)` | 统计 Ready Pod。 |
| `maxRestartCount(text)` | 提取最大重启次数。 |
| `parseHttpProbe(text)` | 解析 HTTP 探测。 |
| `parseK8sCpuMilli(text)` | 解析 K8s CPU millicores。 |
| `parseK8sMemoryMi(text)` | 解析 K8s 内存 Mi。 |
| `parseK8sResourceMetrics(text)` | 综合解析 K8s 资源指标。 |

#### 分析与渲染函数

| 函数 | 作用 |
| --- | --- |
| `evaluateK8sStressEffect(before, after)` | 判断 K8s 压力效果。 |
| `evaluateK8sContainerKillEffect(before, after)` | 判断容器 kill 是否触发重启。 |
| `detectActionKeywordStats(output)` | 统计输出关键词。 |
| `summarizeActionSignals(result)` | 生成动作摘要。 |
| `isHangLikeScenario(test)` | 判断挂起类场景。 |
| `renderActionFocusCard(result)` | 渲染动作重点卡。 |
| `renderK8sChaosFocusCard(result)` | 渲染 K8s Chaos 重点卡。 |
| `renderResourceFocusCard(result)` | 渲染资源故障卡。 |
| `renderNetworkFocusCard(result)` | 渲染网络故障卡。 |
| `renderHdfsFocusCard(result)` | 渲染 HDFS 结果卡。 |
| `renderMapReduceFocusCard(result)` | 渲染 MapReduce 结果卡。 |
| `renderNodes(nodes)` | 渲染节点列表。 |
| `renderMainPanel()` | 渲染当前分组场景。 |
| `buildScenarioCard(test)` | 构建功能测试卡片。 |
| `buildUtilityCard(action)` | 构建工具动作卡片。 |
| `renderField(field, value)` | 渲染参数控件。 |
| `renderComparison(before, after, test)` | 渲染前后对比。 |
| `renderCheckResult(result)` | 渲染检查命令结果。 |
| `renderNodeResult(result)` | 渲染节点命令结果。 |

#### 交互函数

| 函数 | 作用 |
| --- | --- |
| `collectParams(card)` | 收集表单参数。 |
| `executeScenario(test, card)` | 调用 `/api/functest`。 |
| `executeCleanup(test, card)` | 调用 `/api/functest/cleanup`。 |
| `runSimpleAction(action, params)` | 调用 `/api/action`。 |
| `runRecoveryAll()` | 调用全量恢复接口；当前需后端补 `/api/recover/all`。 |
| `buildEnhancedHistoryEntry(result)` | 构建功能测试历史条目。 |
| `buildSimpleHistoryEntry(result)` | 构建动作历史条目。 |
| `buildCleanupHistoryEntry(result)` | 构建清理历史条目。 |
| `buildErrorEntry(error)` | 构建错误历史条目。 |
| `healthCheck()` | 调用健康检查。 |
| `initLoad()` | 页面初始化。 |

实现原理：前端把后端返回的结构化结果二次解析成指标卡，而不是只显示原始日志。这让故障效果更容易被观察。

### 4.11 `web_controller/static/history.js`

历史页前端逻辑。

| 函数 | 作用 |
| --- | --- |
| `elc(tag, className, text)` | 创建元素。 |
| `escapeHtml(value)` | 转义输出。 |
| `fetchJson(url, options)` | 请求历史 API。 |
| `formatDate(value)` | 格式化时间。 |
| `runTypeText(type)` | 类型转中文。 |
| `setMessage(text, kind)` | 设置页面提示。 |
| `resultsByPhase(results)` | 按阶段分组。 |
| `phaseText(phase)` | 阶段名转中文。 |
| `renderResult(result)` | 渲染一条阶段结果。 |
| `renderRun(run)` | 渲染一条历史运行。 |
| `loadHistory()` | 加载历史列表。 |
| `clearDbHistory()` | 清空历史库。 |

### 4.12 `web_controller/static/styles.css`

前端样式文件。它定义：

| 模块 | 作用 |
| --- | --- |
| `:root` | 颜色、阴影、边框、字体变量。 |
| 全局样式 | body、按钮、链接、表单基础样式。 |
| 顶部状态区 | 控制台头部布局。 |
| 节点卡片 | 集群节点展示。 |
| 分组导航 | 场景组切换按钮。 |
| 场景卡片 | 功能测试与动作卡片布局。 |
| 表单控件 | input/select/checkbox/number 样式。 |
| 结果区 | stdout/stderr、退出码、阶段结果展示。 |
| 指标卡 | 网络、资源、K8s、HDFS、MapReduce 结果卡。 |
| 响应式规则 | 小屏布局调整。 |

### 4.13 `web_controller/tests/conftest.py`

pytest 夹具配置。

| 对象 | 作用 |
| --- | --- |
| `MOCK_CONFIG` | 模拟节点与路径配置。 |
| `mock_config_path` | 创建临时配置文件。 |
| `mock_config` | 返回模拟配置对象。 |
| `patch_config` | monkeypatch `CONFIG_PATH`。 |
| `patch_history_db` | 将历史库改到临时目录。 |
| `client` | FastAPI TestClient。 |

### 4.14 `web_controller/tests/test_app.py`

后端测试文件。

| 测试范围 | 覆盖内容 |
| --- | --- |
| 配置读取 | `load_config()`、节点函数。 |
| 命令执行 | 输出清洗、截断、超时、退出码。 |
| 参数校验 | IP、target、hex、枚举、数值范围。 |
| QEMU 识别 | VM 名提取、QEMU 命令行匹配。 |
| 动作表 | `GROUPS`、`ACTIONS` 结构和命令构造。 |
| API | health、config、action、history、functest cleanup。 |
| 历史库 | record/list/detail/clear。 |
| Pydantic | 请求模型默认值和字段解析。 |

维护注意：部分测试可能仍假设旧分组数量，若 `GROUPS` 已扩展，需要同步更新断言。

### 4.15 `web_controller/tests/__init__.py`

测试包标记文件，无业务逻辑。

## 5. `vm_injection/` 用户态注入器代码

本节不包含 `vm_injection/靶子程序/`。

### 5.1 `vm_injection/Makefile`

编译配置文件。

| 目标 | 作用 |
| --- | --- |
| `all` | 编译注入器和相关工具。 |
| `basic` | 编译基础用户态注入器。 |
| `kvm` | 编译 `kvm_injector`。 |
| `clean` | 删除编译产物。 |
| `help` | 输出帮助。 |

实现原理：Makefile 将每个 C 注入器编译为独立二进制，Web 后端通过配置路径调用这些二进制。

### 5.2 `vm_injection/run_cluster.sh`

QEMU/KVM 虚拟机集群启动脚本。

| 逻辑 | 作用 |
| --- | --- |
| 参数解析 | 根据 `master/slave1/slave2` 决定启动节点。 |
| 镜像选择 | 为每个节点选择 qcow2 镜像。 |
| QEMU `-name` | 设置 VM 名称，供后端和注入器识别。 |
| 端口转发 | 配置 SSH 和服务端口转发。 |
| KVM 加速 | 使用 QEMU/KVM 参数启动虚拟机。 |

实现原理：脚本把 VM 名称、镜像、端口绑定成稳定拓扑。后续 KVM/VM 注入器通过 QEMU 命令行识别目标。

### 5.3 `vm_injection/start_kvm.sh`

辅助 KVM 启动脚本，用于单 VM 或调试启动。它封装 QEMU 参数，便于快速验证 KVM 环境。

### 5.4 `vm_injection/process_injector.c`

进程故障注入器。

| 函数 | 作用 |
| --- | --- |
| `get_vm_pid(const char *vm_name)` | 根据 VM 名查找 QEMU 进程 PID。 |
| `inject_process(pid_t pid, const char *fault_type)` | 根据故障类型发送进程信号。 |
| `main(int argc, char **argv)` | 解析参数，定位目标 PID，执行 crash/hang/resume。 |

实现原理：Linux 信号直接改变进程状态。`SIGSTOP` 模拟挂起，`SIGCONT` 恢复，`SIGKILL`/`SIGTERM` 模拟崩溃。

### 5.5 `vm_injection/network_injector.c`

网络故障注入器。

| 函数 | 作用 |
| --- | --- |
| `get_interface_name()` | 从默认路由中解析网卡名。 |
| `inject_network(...)` | 根据故障类型拼接并执行 `tc qdisc netem` 命令。 |
| `main(int argc, char **argv)` | 解析 delay/loss/clear 等参数并执行。 |

实现原理：Linux `tc netem` 在队列层模拟网络异常，不需要修改应用程序。

### 5.6 `vm_injection/cpu_injector.c`

CPU 压力注入器。

| 函数/变量 | 作用 |
| --- | --- |
| `keep_running` | 全局运行标志。 |
| `stress_worker(void *arg)` | 忙循环线程函数。 |
| `simple_stress(int workers, int duration)` | 创建 worker，运行指定时间后停止。 |
| `main(int argc, char **argv)` | 解析 worker 和时长，启动压力。 |

实现原理：通过计算密集型忙循环占用 CPU，使目标系统产生调度延迟、负载升高和服务超时。

### 5.7 `vm_injection/memleak_injector.c`

内存泄漏/内存占用注入器。

| 函数 | 作用 |
| --- | --- |
| `main(int argc, char **argv)` | 循环分配内存、触摸页面并保持占用。 |

实现原理：触摸分配区域会触发缺页并提交物理页，比单纯 `malloc()` 更能制造真实内存压力。

### 5.8 `vm_injection/mem_injector.c`

目标进程内存篡改注入器。

| 函数/结构 | 作用 |
| --- | --- |
| 故障类型枚举 | 描述 bit flip、置零、置一、随机、固定值写入等方式。 |
| `InjectorContext` | 保存 PID、地址、模式、次数、间隔、随机种子等上下文。 |
| `die(const char *msg)` | 错误退出。 |
| `ptrace_attach(pid_t pid)` | 附加目标进程并等待停止。 |
| `ptrace_detach(pid_t pid)` | 恢复目标进程执行。 |
| `ptrace_read(pid_t pid, unsigned long addr)` | 读取目标地址 word。 |
| `ptrace_write(pid_t pid, unsigned long addr, unsigned long value)` | 写回目标地址 word。 |
| `find_region_address_blind(pid_t pid, size_t size)` | 从 `/proc/<pid>/maps` 选择可写内存区域。 |
| `scan_memory_for_pattern(pid_t pid, pattern)` | 扫描目标内存中的特征模式。 |
| `apply_fault_logic(old_value, fault_type, mask)` | 按故障类型修改 word。 |
| `print_help()` | 输出帮助。 |
| `main(int argc, char **argv)` | 参数解析、attach、读改写、detach。 |

实现原理：`ptrace` 让注入器像调试器一样读写目标进程地址空间，从而模拟内存软错误或数据破坏。

### 5.9 `vm_injection/reg_injector.c`

ARM64 寄存器故障注入器。

| 函数/结构 | 作用 |
| --- | --- |
| `struct user_pt_regs` | ARM64 用户态寄存器快照。 |
| `FaultType` | 寄存器故障类型。 |
| `die(const char *msg)` | 错误退出。 |
| `sigint_handler(int sig)` | Ctrl-C 时停止并确保 detach。 |
| `rand_bit()` | 随机选择 bit。 |
| `my_rand()` | 伪随机数生成。 |
| `bit_mask(int bit)` | 构造 bit 掩码。 |
| `pick_bit_with_value(value, expected)` | 选择值为 0 或 1 的 bit。 |
| `alarm_handler(int sig)` | 定时触发注入。 |
| `ptrace_attach(pid_t pid)` | 附加目标。 |
| `ptrace_detach(pid_t pid)` | 脱离目标。 |
| `apply_fault(struct user_pt_regs *regs)` | 修改寄存器值。 |
| `main(int argc, char **argv)` | 参数解析和注入循环。 |

实现原理：通过 ptrace 获取目标进程寄存器快照，修改指定寄存器或 bit 后写回，模拟 CPU 寄存器瞬态错误。

### 5.10 `vm_injection/fault_controller.c`

交互式用户态故障控制器。

| 函数 | 作用 |
| --- | --- |
| `get_vm_pid(const char *vm_name)` | 查找 VM/QEMU PID。 |
| `inject_process_wrapper()` | 进程故障交互封装。 |
| `inject_network_wrapper()` | 网络故障交互封装。 |
| `inject_memory_wrapper()` | 内存故障交互封装。 |
| `inject_register_wrapper()` | 寄存器故障交互封装。 |
| `inject_cpu_wrapper()` | CPU 压力交互封装。 |
| `inject_mem_leak_wrapper()` | 内存泄漏交互封装。 |
| `show_menu()` | 显示菜单。 |
| `main()` | 菜单循环和命令分发。 |

实现原理：该文件不重新实现故障逻辑，而是把多个独立注入器包装成菜单式 CLI。

### 5.11 `vm_injection/kvm_injector.c`

面向 QEMU/KVM 目标的用户态综合注入器。

#### QEMU 识别函数

| 函数 | 作用 |
| --- | --- |
| `normalize_status(const char *status)` | 标准化状态文本。 |
| `init_tool_dir()` | 初始化辅助工具目录。 |
| `build_tool_path(name)` | 拼接辅助工具路径。 |
| `ensure_helper_tool(name)` | 检查工具是否存在。 |
| `is_numeric_arg(text)` | 判断参数是否为 PID。 |
| `normalize_vm_name(name)` | 规范化 VM 名。 |
| `extract_name_after_marker(args, marker)` | 从参数中提取 `-name` 后的值。 |
| `extract_vm_name_from_args(args)` | 从 QEMU 参数提取 VM 名。 |
| `is_qemu_args(args)` | 判断是否 QEMU 命令行。 |
| `read_proc_cmdline(pid)` | 读取 `/proc/<pid>/cmdline`。 |
| `get_qemu_args(pid)` | 获取 QEMU 参数。 |
| `get_qemu_name(pid)` | 获取 QEMU VM 名。 |
| `vm_name_matches(actual, expected)` | 判断 VM 名是否匹配。 |
| `find_qemu_pids()` | 查找所有 QEMU PID。 |
| `find_qemu_pid_by_name(name)` | 按 VM 名查找 PID。 |
| `resolve_qemu_target_pid(arg)` | 将 PID 或 VM 名解析为 PID。 |
| `list_kvm_vms()` | 列出运行中的 VM。 |

#### 注入函数

| 函数 | 作用 |
| --- | --- |
| `inject_soft_error(pid, type)` | 注入软错误。 |
| `hold_guest_observation_window(seconds)` | 注入后保留观察窗口。 |
| `inject_guest_behavior_fault(pid, kind)` | 注入 guest 行为故障。 |
| `inject_performance_fault(pid, kind)` | 注入性能类故障。 |
| `inject_cpu_stress(workers, seconds)` | 制造 CPU 压力。 |
| `inject_cpu_hotplug_fault(pid, action)` | 模拟 CPU 热插拔故障。 |
| `clear_all_faults()` | 清理残留故障。 |
| `print_usage()` | 输出帮助。 |
| `main(int argc, char **argv)` | 子命令分发。 |

实现原理：通过识别 QEMU 进程，把 VM 名映射到宿主机 PID，再组合信号、资源压力、辅助工具和清理逻辑模拟虚拟化层可观测故障。

## 6. `kvm_injection/` KVM/集群代码与配置

### 6.1 `kvm_injection/cluster.conf`

集群节点配置。

| 字段 | 作用 |
| --- | --- |
| `name` | 节点名。 |
| `ip` | 节点 IP。 |
| `port` | SSH 端口。 |
| `role` | 节点角色。 |

实现原理：C 控制器按逗号解析固定字段。若 role 中包含额外逗号，可能只读取第一个 role 片段，应保持字段简单。

### 6.2 `kvm_injection/Makefile`

顶层编译配置。

| 目标 | 作用 |
| --- | --- |
| `all` | 编译集群控制器并递归构建子模块。 |
| `cluster_controller` | 编译 `cluster_controller.c`。 |
| 子目录目标 | 进入各注入模块目录执行对应 Makefile。 |
| `clean` | 清理构建产物。 |

实现原理：内核模块必须通过 Linux kernel build system 编译，顶层 Makefile 只是统一调度入口。

### 6.3 `kvm_injection/cluster_controller.c`

交互式集群故障控制器。

| 函数 | 作用 |
| --- | --- |
| `init_hadoop_cluster()` | 初始化 Hadoop 集群上下文。 |
| `load_cluster_config(const char *path)` | 读取 `cluster.conf`。 |
| `show_cluster_status()` | 展示节点和服务状态。 |
| `remote_exec(node, command)` | SSH 远程执行。 |
| `local_exec(command)` | 本地执行。 |
| `get_process_pid(node, process_name)` | 查询进程 PID。 |
| `inject_vm_fault()` | 分发 VM/KVM 故障。 |
| `inject_hadoop_fault()` | 分发 Hadoop 故障。 |
| `inject_cloudstack_fault()` | 分发 CloudStack 故障。 |
| `run_fault_scenario()` | 执行组合场景。 |
| `clear_all_faults()` | 清理故障残留。 |
| `show_main_menu()` | 主菜单。 |
| `show_vm_menu()` | VM 故障菜单。 |
| `show_hadoop_menu()` | Hadoop 故障菜单。 |
| `show_cloudstack_menu()` | CloudStack 故障菜单。 |
| `show_scenario_menu()` | 组合场景菜单。 |
| `main()` | 初始化并进入菜单循环。 |

实现原理：控制器把“用户选择”转换为本地命令或 SSH 命令。它是 CLI 版控制面，与 Web 控制面在职责上相似。

### 6.4 `kvm_injection/cluster_manage.sh`

Shell 版集群管理脚本。

| 函数 | 作用 |
| --- | --- |
| `print_info()` / `print_success()` / `print_warning()` / `print_error()` | 统一日志输出。 |
| `ssh_exec(node, command)` | SSH 执行命令。 |
| `check_node_connectivity(node)` | 检查节点连通。 |
| `check_ssh(node)` | 检查 SSH。 |
| `check_hadoop_process(node, process)` | 检查 Hadoop 进程。 |
| `show_status()` | 展示集群状态。 |
| `start_cluster()` | 启动 Hadoop 集群。 |
| `stop_cluster()` | 停止 Hadoop 集群。 |
| `restart_cluster()` | 重启 Hadoop 集群。 |
| `inject_network_partition(a, b)` | 注入网络分区。 |
| `clear_network_partition()` | 清理网络分区。 |
| `inject_process_crash(node, process)` | 注入进程崩溃。 |
| `inject_process_hang(node, process)` | 注入进程挂起。 |
| `resume_process(node, process)` | 恢复进程。 |
| `inject_network_delay(node, ms)` | 注入网络延迟。 |
| `clear_network_delay(node)` | 清理网络延迟。 |
| `run_hdfs_test()` | 执行 HDFS 验证。 |
| `show_help()` | 输出帮助。 |

实现原理：用 shell 命令直接操作集群，适合手工验证或 Web/C 控制器不可用时兜底。

## 7. Hadoop 与 CloudStack 专项注入器

### 7.1 `kvm_injection/hadoop-fi/Makefile`

编译 `hadoop_injector.c`。通常生成 `hadoop_injector` 用户态二进制。

### 7.2 `kvm_injection/hadoop-fi/hadoop_injector.c`

Hadoop 故障注入器。

#### 组件识别

| 函数 | 作用 |
| --- | --- |
| `get_proc_state(pid_t pid)` | 获取进程状态。 |
| `get_component_name(component)` | 组件枚举转名称。 |
| `is_slave_component(component)` | 判断是否 worker 组件。 |
| `find_hadoop_pid(component)` | 查找 Hadoop 组件 PID。 |
| `find_mapreduce_pids(kind)` | 查找 MapReduce 进程。 |
| `get_default_nic()` | 获取默认网卡。 |

#### 本地/远程执行

| 函数 | 作用 |
| --- | --- |
| `exec_remote_injector(node, args)` | SSH 执行远程注入器。 |
| `list_local_processes()` | 列出本机 Hadoop 进程。 |
| `list_cluster_processes()` | 列出集群 Hadoop 进程。 |
| `exec_local_process_fault(component, fault)` | 本机进程故障。 |
| `inject_process_fault_distributed(component, fault, node)` | 分布式进程故障。 |

#### 网络与资源

| 函数 | 作用 |
| --- | --- |
| `inject_network_fault(type, target)` | 网络故障总入口。 |
| `inject_network_delay(iface, ms)` | `tc netem delay`。 |
| `inject_network_loss(iface, percent)` | `tc netem loss`。 |
| `inject_network_reorder(iface, percent)` | `tc netem reorder`。 |
| `cpu_stress_worker(void *arg)` | CPU 压力线程。 |
| `inject_cpu_stress(workers, seconds)` | CPU 压力。 |
| `inject_memory_stress(mb, seconds)` | 内存压力。 |

#### Hadoop 语义故障

| 函数 | 作用 |
| --- | --- |
| `inject_hdfs_fault(type)` | HDFS 安全模式、磁盘等故障。 |
| `inject_yarn_fault(type)` | YARN unhealthy 等故障。 |
| `inject_io_delay(path, ms)` | IO 延迟。 |
| `inject_heartbeat_timeout(component)` | 心跳超时。 |
| `inject_mapreduce_fault(type)` | MapReduce task 故障。 |

#### 入口

| 函数 | 作用 |
| --- | --- |
| `print_usage()` | 输出帮助。 |
| `parse_component(text)` | 解析组件名。 |
| `main(argc, argv)` | 子命令分发。 |

实现原理：Hadoop 注入器通过进程信号、tc 网络队列、资源压力、HDFS/YARN 命令和远程 SSH 组合模拟分布式系统常见故障。

### 7.3 `kvm_injection/cloudstack-fi/Makefile`

编译 `cloudstack_injector.c`，生成 CloudStack 注入器。

### 7.4 `kvm_injection/cloudstack-fi/cloudstack_injector.c`

CloudStack 故障注入器。

| 函数 | 作用 |
| --- | --- |
| `get_cs_component_name(component)` | 组件枚举转名称。 |
| `get_cs_component_desc(component)` | 组件描述。 |
| `get_default_nic()` | 获取默认网卡。 |
| `find_cs_pid(component)` | 查找组件 PID。 |
| `list_cloudstack_processes()` | 列出 CloudStack 进程。 |
| `inject_cs_process_fault(component, fault)` | 进程 crash/hang/resume。 |
| `inject_api_fault(type)` | API 延迟/恢复。 |
| `inject_cs_network_fault(type)` | 网络故障。 |
| `inject_db_fault(type)` | 数据库限制、锁、恢复。 |
| `inject_storage_fault(type)` | 存储卸载、只读、填满、清理。 |
| `inject_agent_fault(type)` | Agent 断连/重连。 |
| `inject_sysvm_fault(type)` | SystemVM 故障。 |
| `cs_cpu_stress_worker(void *arg)` | CPU 压力线程。 |
| `inject_cs_cpu_stress(workers, seconds)` | CPU 压力。 |
| `inject_cs_memory_stress(mb, seconds)` | 内存压力。 |
| `inject_vm_operation_fault(type)` | VM 创建/迁移失败。 |
| `print_cs_usage()` | 输出帮助。 |
| `parse_cs_component(text)` | 解析组件。 |
| `parse_sysvm_type(text)` | 解析 SystemVM 类型。 |
| `main(argc, argv)` | 子命令分发。 |

实现原理：CloudStack 注入器用 Linux 进程控制、网络规则、数据库命令、存储挂载状态和资源压力模拟管理平台故障。

## 8. KVM 内核模块与控制程序

### 8.1 通用 Makefile 原理

各内核模块目录中的 Makefile 通常包含：

| 配置 | 作用 |
| --- | --- |
| `obj-m := xxx.o` | 声明要构建的内核模块对象。 |
| `KDIR := /lib/modules/$(uname -r)/build` | 指向当前内核构建目录。 |
| `make -C $(KDIR) M=$(PWD) modules` | 使用内核构建系统编译 `.ko`。 |
| 用户态 main 编译 | 用 gcc 编译写 procfs 的控制程序。 |
| `clean` | 删除 `.ko`、`.o`、`.mod*`、控制程序等产物。 |

实现原理：内核模块不能像普通 C 程序一样直接 gcc 链接，必须使用当前内核的 Kbuild 环境。

### 8.2 `kvm_injection/cpu-fi/Makefile`

编译 `cpu-reg.ko` 和 `cpu-reg-main`。

### 8.3 `kvm_injection/cpu-fi/cpu-reg.c`

CPU/寄存器路径内核模块。

| 函数/宏 | 作用 |
| --- | --- |
| `inject_fault_arm64(...)` | ARM64 值/寄存器故障注入。 |
| `handler_pre(struct kprobe *p, struct pt_regs *regs)` | kprobe pre-handler，挂钩 `kernel_clone`。 |
| proc 写函数宏 | 生成控制项 write handler。 |
| `my_cpu_init()` | 创建 procfs，注册 kprobe。 |
| `my_cpu_exit()` | 注销 kprobe，删除 procfs。 |

实现原理：模块在 `kernel_clone` 执行前触发，读取 `/proc/cpu-general-fi/` 控制参数后对 `pt_regs` 中的寄存器上下文执行位级故障注入。

### 8.4 `kvm_injection/cpu-fi/cpu-reg-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs 控制项。 |
| `main(argc, argv)` | 将命令行参数转换为 proc 写入。 |

### 8.5 `kvm_injection/access-control-fi/Makefile`

编译访问控制故障模块和子目录内存压力工具。

### 8.6 `kvm_injection/access-control-fi/resource.c`

KVM VM ioctl 路径故障模块。

| 函数 | 作用 |
| --- | --- |
| `getrando()` | 伪随机数。 |
| `change_arm64(...)` | ARM64 值修改。 |
| `handler_pre(struct kprobe *p, struct pt_regs *regs)` | kprobe pre-handler，挂钩 `kvm_vm_ioctl`。 |
| `proc_write_common(...)` | proc 写入公共解析。 |
| `write_time()` | 写触发次数。 |
| `write_pos()` | 写注入位置。 |
| `write_type()` | 写故障类型。 |
| `write_sig()` | 写启停信号。 |
| `write_style()` | 写注入风格。 |
| `my_res_init()` | 注册 kprobe 和 procfs。 |
| `my_res_exit()` | 清理模块。 |

实现原理：KVM 的大量 VM 管理操作会经过 `kvm_vm_ioctl`。在该函数入口用 kprobe 拦截，可以模拟 ioctl 参数异常或访问控制路径异常。

### 8.7 `kvm_injection/access-control-fi/resource-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置 resource 模块。 |

### 8.8 `kvm_injection/access-control-fi/memory-load/Makefile`

编译内存压力工具。

### 8.9 `kvm_injection/access-control-fi/memory-load/memory.c`

| 函数 | 作用 |
| --- | --- |
| `main(argc, argv)` | 分配并触摸内存，制造内存压力。 |

实现原理：通过实际访问分配页面触发物理内存占用，用于观察内存压力对 KVM 或上层服务的影响。

### 8.10 `kvm_injection/file-fi/Makefile`

递归编译文件读写故障模块。

### 8.11 `kvm_injection/file-fi/file-read-fi/Makefile`

编译 `file-read-fi.ko` 和读路径控制程序。

### 8.12 `kvm_injection/file-fi/file-read-fi/file-read-fi.c`

| 函数 | 作用 |
| --- | --- |
| `handler_pre(struct kprobe *p, struct pt_regs *regs)` | kprobe pre-handler，挂钩 `vfs_read`。 |
| `write_signal()` | 写启停信号。 |
| `write_times()` | 写触发次数。 |
| `write_type()` | 写故障类型。 |
| `my_detect_init()` | 注册 kprobe 和 procfs。 |
| `my_detect_exit()` | 清理模块。 |

实现原理：`vfs_read` 是 Linux 文件读取路径的核心函数之一。用 kprobe 拦截它可以模拟读失败、读延迟或读路径异常。

### 8.13 `kvm_injection/file-fi/file-read-fi/file-read-fi-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置读路径故障模块。 |

### 8.14 `kvm_injection/file-fi/file-write-fi/Makefile`

编译写路径故障模块。维护注意：当前 Makefile 中存在疑似 `obj-m` 命名不一致痕迹，应确认是否应指向 `file-write-fi.o`。

### 8.15 `kvm_injection/file-fi/file-write-fi/file-write-fi.c`

| 函数 | 作用 |
| --- | --- |
| `handler_pre(struct kprobe *p, struct pt_regs *regs)` | kprobe pre-handler，挂钩 `vfs_write`。 |
| `write_signal()` | 写启停信号。 |
| `write_times()` | 写触发次数。 |
| `write_type()` | 写故障类型。 |
| `my_detect_init()` | 注册写路径 kprobe。 |
| `my_detect_exit()` | 清理模块。 |

实现原理：`vfs_write` 是文件写入路径核心函数。拦截它可以模拟写失败、写异常或写路径扰动。

### 8.16 `kvm_injection/file-fi/file-write-fi/file-write-fi-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置写路径故障模块。 |

### 8.17 `kvm_injection/memory-fi/Makefile`

递归编译页表加载和页表更新故障模块。

### 8.18 `kvm_injection/memory-fi/pt-load-fi/Makefile`

编译 `pt-load-fi.ko` 和控制程序。

### 8.19 `kvm_injection/memory-fi/pt-load-fi/pt-load-fi.c`

| 函数 | 作用 |
| --- | --- |
| `entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)` | 进入 `handle_mm_fault` 时记录上下文。 |
| `ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)` | 返回时触发故障。 |
| `write_signal()` | 写启停信号。 |
| `write_times()` | 写触发次数。 |
| `write_type()` | 写故障类型。 |
| `my_mem_init()` | 注册 kretprobe 和 procfs。 |
| `my_mem_exit()` | 清理模块。 |

实现原理：缺页处理通过 `handle_mm_fault`。kretprobe 可以在缺页处理结束时修改返回值或记录异常，模拟页表加载失败。

### 8.20 `kvm_injection/memory-fi/pt-load-fi/pt-load-fi-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置 pt-load 模块。 |

### 8.21 `kvm_injection/memory-fi/pt-update-fi/Makefile`

编译页表更新故障模块。维护注意：其中疑似存在 `dmesg -w$(MAKE)` 拼写/粘连问题，若构建失败应优先检查。

### 8.22 `kvm_injection/memory-fi/pt-update-fi/pt-update-fi.c`

| 函数 | 作用 |
| --- | --- |
| `handler_pre(struct kprobe *p, struct pt_regs *regs)` | kprobe pre-handler，挂钩 `handle_mm_fault`。 |
| `show_int()` | proc read 显示整数状态。 |
| `open_signal()` | 打开 signal proc。 |
| `open_times()` | 打开 times proc。 |
| `write_common()` | 通用写入解析。 |
| `write_signal()` | 写启停信号。 |
| `write_times()` | 写触发次数。 |
| `my_pt_init()` | 注册 kprobe 和 procfs。 |
| `my_pt_exit()` | 清理模块。 |

实现原理：与 pt-load 不同，该模块在 `handle_mm_fault` 入口用 kprobe 触发，偏向观测或扰动页表更新前状态。

### 8.23 `kvm_injection/memory-fi/pt-update-fi/pt-update-fi-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置 pt-update 模块。 |

### 8.24 `kvm_injection/memory-manage-fi/Makefile`

编译 KVM 内存管理故障模块和 kswapd 子模块。

### 8.25 `kvm_injection/memory-manage-fi/memory.c`

| 函数 | 作用 |
| --- | --- |
| `getrando()` | 伪随机数。 |
| `change_arm64(...)` | ARM64 值修改。 |
| `handler_common(struct pt_regs *regs)` | 共用故障逻辑。 |
| `handler_pre1(struct kprobe *p, struct pt_regs *regs)` | kprobe `kvm_set_memory_region`。 |
| `handler_pre2(struct kprobe *p, struct pt_regs *regs)` | kprobe `gfn_to_hva_many`。 |
| `proc_write_common()` | proc 写入解析。 |
| `write_class()` | 写故障类别。 |
| `write_time()` | 写触发次数。 |
| `write_pos()` | 写注入位置。 |
| `write_type()` | 写类型。 |
| `write_sig()` | 写启停信号。 |
| `write_style()` | 写风格。 |
| `my_mm_init()` | 注册 kprobe 和 procfs。 |
| `my_mm_exit()` | 清理模块。 |

实现原理：`kvm_set_memory_region` 管理 guest 内存区域，`gfn_to_hva_many` 处理 guest frame number 到 host virtual address 的转换。拦截这两处可以模拟 KVM 内存映射和地址转换异常。

### 8.26 `kvm_injection/memory-manage-fi/memory-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置 memory-manage 模块。 |

### 8.27 `kvm_injection/memory-manage-fi/kswapd-fi/Makefile`

编译 kswapd 故障模块。

### 8.28 `kvm_injection/memory-manage-fi/kswapd-fi/kswapd-fi.c`

| 函数 | 作用 |
| --- | --- |
| `handler_pre(struct kprobe *p, struct pt_regs *regs)` | kprobe pre-handler，挂钩 `shrink_node`。 |
| `write_sig()` | 写启停信号。 |
| `write_time()` | 写触发次数。 |
| `my_swp_init()` | 注册 kprobe 和 procfs。 |
| `my_swp_exit()` | 清理模块。 |

实现原理：`shrink_node` 属于内存回收路径。拦截它可以模拟内存回收异常、swap 压力或回收路径抖动。

### 8.29 `kvm_injection/memory-manage-fi/kswapd-fi-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置 kswapd 模块。 |

### 8.30 `kvm_injection/state-query-fi/Makefile`

递归编译 KVM 状态查询和版本查询故障模块。

### 8.31 `kvm_injection/state-query-fi/kvm-state-fi/Makefile`

编译 KVM vCPU 状态查询故障模块。

### 8.32 `kvm_injection/state-query-fi/kvm-state-fi/kvm-state-fi.c`

| 函数 | 作用 |
| --- | --- |
| `ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)` | kretprobe `kvm_vcpu_ioctl` 返回路径。 |
| `write_signal()` | 写启停信号。 |
| `write_times()` | 写触发次数。 |
| `my_state_init()` | 注册 kretprobe 和 procfs。 |
| `my_state_exit()` | 清理模块。 |

实现原理：KVM vCPU 状态查询经过 `kvm_vcpu_ioctl`。在返回路径修改结果，可模拟状态查询错误或 vCPU 状态不可用。

### 8.33 `kvm_injection/state-query-fi/kvm-state-fi/kvm-state-fi-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置 state 模块。 |

### 8.34 `kvm_injection/state-query-fi/kvm-version-fi/Makefile`

编译 KVM 版本/能力查询故障模块。

### 8.35 `kvm_injection/state-query-fi/kvm-version-fi/kvm-version-fi.c`

| 函数 | 作用 |
| --- | --- |
| `ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)` | kretprobe `kvm_dev_ioctl` 返回路径。 |
| `write_signal()` | 写启停信号。 |
| `write_times()` | 写触发次数。 |
| `my_ver_init()` | 注册 kretprobe 和 procfs。 |
| `my_ver_exit()` | 清理模块。 |

实现原理：KVM 版本和能力查询经过 `/dev/kvm` ioctl。修改 `kvm_dev_ioctl` 返回路径可以模拟版本不兼容或能力查询失败。

### 8.36 `kvm_injection/state-query-fi/kvm-version-fi/kvm-version-fi-main.c`

| 函数 | 作用 |
| --- | --- |
| `write_proc(path, value)` | 写 procfs。 |
| `main(argc, argv)` | 配置 version 模块。 |

### 8.37 `kvm_injection/vm-migration-fi/Makefile`

编译 VM 迁移故障模块。

### 8.38 `kvm_injection/vm-migration-fi/vm-migration-fi.c`

| 函数 | 作用 |
| --- | --- |
| `ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)` | kretprobe `kvm_vm_ioctl` 返回路径。 |
| `write_sig()` | 写启停信号。 |
| `my_mig_init()` | 注册 kretprobe 和 procfs。 |
| `my_mig_exit()` | 清理模块。 |

实现原理：VM 迁移相关操作会通过 VM ioctl 路径。kretprobe 返回处理可模拟迁移 ioctl 失败或异常返回。

### 8.39 `kvm_injection/vm-migration-fi/vm-migration-fi-main.c`

| 函数 | 作用 |
| --- | --- |
| `main(argc, argv)` | 向 migration 模块 procfs 写入启停控制。 |

## 9. 覆盖索引

### 顶层

- `.active_faults.json`
- `start_frontend.sh`

### Web 控制面

- `web_controller/__init__.py`
- `web_controller/app.py`
- `web_controller/config.json`
- `web_controller/db.py`
- `web_controller/k8s_chaos.py`
- `web_controller/requirements.txt`
- `web_controller/test_scenarios.py`
- `web_controller/static/index.html`
- `web_controller/static/history.html`
- `web_controller/static/app.js`
- `web_controller/static/history.js`
- `web_controller/static/styles.css`
- `web_controller/tests/__init__.py`
- `web_controller/tests/conftest.py`
- `web_controller/tests/test_app.py`

### VM 用户态注入器

- `vm_injection/Makefile`
- `vm_injection/run_cluster.sh`
- `vm_injection/start_kvm.sh`
- `vm_injection/cpu_injector.c`
- `vm_injection/fault_controller.c`
- `vm_injection/kvm_injector.c`
- `vm_injection/mem_injector.c`
- `vm_injection/memleak_injector.c`
- `vm_injection/network_injector.c`
- `vm_injection/process_injector.c`
- `vm_injection/reg_injector.c`

### KVM 与集群注入器

- `kvm_injection/Makefile`
- `kvm_injection/cluster.conf`
- `kvm_injection/cluster_controller.c`
- `kvm_injection/cluster_manage.sh`
- `kvm_injection/hadoop-fi/Makefile`
- `kvm_injection/hadoop-fi/hadoop_injector.c`
- `kvm_injection/cloudstack-fi/Makefile`
- `kvm_injection/cloudstack-fi/cloudstack_injector.c`
- `kvm_injection/cpu-fi/Makefile`
- `kvm_injection/cpu-fi/cpu-reg.c`
- `kvm_injection/cpu-fi/cpu-reg-main.c`
- `kvm_injection/access-control-fi/Makefile`
- `kvm_injection/access-control-fi/resource.c`
- `kvm_injection/access-control-fi/resource-main.c`
- `kvm_injection/access-control-fi/memory-load/Makefile`
- `kvm_injection/access-control-fi/memory-load/memory.c`
- `kvm_injection/file-fi/Makefile`
- `kvm_injection/file-fi/file-read-fi/Makefile`
- `kvm_injection/file-fi/file-read-fi/file-read-fi.c`
- `kvm_injection/file-fi/file-read-fi/file-read-fi-main.c`
- `kvm_injection/file-fi/file-write-fi/Makefile`
- `kvm_injection/file-fi/file-write-fi/file-write-fi.c`
- `kvm_injection/file-fi/file-write-fi/file-write-fi-main.c`
- `kvm_injection/memory-fi/Makefile`
- `kvm_injection/memory-fi/pt-load-fi/Makefile`
- `kvm_injection/memory-fi/pt-load-fi/pt-load-fi.c`
- `kvm_injection/memory-fi/pt-load-fi/pt-load-fi-main.c`
- `kvm_injection/memory-fi/pt-update-fi/Makefile`
- `kvm_injection/memory-fi/pt-update-fi/pt-update-fi.c`
- `kvm_injection/memory-fi/pt-update-fi/pt-update-fi-main.c`
- `kvm_injection/memory-manage-fi/Makefile`
- `kvm_injection/memory-manage-fi/memory.c`
- `kvm_injection/memory-manage-fi/memory-main.c`
- `kvm_injection/memory-manage-fi/kswapd-fi/Makefile`
- `kvm_injection/memory-manage-fi/kswapd-fi/kswapd-fi.c`
- `kvm_injection/memory-manage-fi/kswapd-fi-main.c`
- `kvm_injection/state-query-fi/Makefile`
- `kvm_injection/state-query-fi/kvm-state-fi/Makefile`
- `kvm_injection/state-query-fi/kvm-state-fi/kvm-state-fi.c`
- `kvm_injection/state-query-fi/kvm-state-fi/kvm-state-fi-main.c`
- `kvm_injection/state-query-fi/kvm-version-fi/Makefile`
- `kvm_injection/state-query-fi/kvm-version-fi/kvm-version-fi.c`
- `kvm_injection/state-query-fi/kvm-version-fi/kvm-version-fi-main.c`
- `kvm_injection/vm-migration-fi/Makefile`
- `kvm_injection/vm-migration-fi/vm-migration-fi.c`
- `kvm_injection/vm-migration-fi/vm-migration-fi-main.c`

## 10. 关键函数代码摘录

本节配合前面的函数表阅读。前面的表格说明“函数负责什么”，这里补充“重要函数具体怎么写”。为避免把整份源码复制进文档，只摘录每个模块中最能体现实现方式的核心代码。

### 10.1 顶层启动：`start_frontend.sh`

`is_vm_running()` 和 `start_vm()` 是启动脚本的关键。它们用 QEMU 命令行特征判断 VM 是否已运行，并在缺失时后台启动。

```bash
is_vm_running() {
  local node="$1"
  local name_pattern="(-name(=| )[^ ]*(guest=)?(ubuntu_|alpine_|kvm_|vm_)?${node}([, ]|$)|(node_|ubuntu_|alpine_|kvm_|vm_)${node}[.]qcow2)"
  ps -ww -eo args= | grep -E '[q]emu-system|[q]emu-kvm' | grep -E -- "$name_pattern" >/dev/null 2>&1
}

start_vm() {
  local node="$1"
  local log_file="$LOG_DIR/${node}.log"

  if is_vm_running "$node"; then
    echo "[skip] $node already running"
    return 0
  fi

  (
    cd "$VM_DIR"
    nohup ./run_cluster.sh "$node" >"$log_file" 2>&1 &
  )

  echo "[ok] started $node (log: $log_file)"
}
```

关键点：

- `grep -E '[q]emu-system|[q]emu-kvm'` 避免匹配 grep 自己。
- `name_pattern` 同时兼容 `-name master` 和 `master.qcow2` 这两类命名。
- `nohup + &` 让 VM 后台运行，Web 服务可以继续启动。

### 10.2 后端统一执行器：`web_controller/app.py`

`run_command()` 是后端命令执行的统一出口。所有底层注入器、kubectl、SSH 命令最终都会变成这里的 `subprocess.run()`。

```python
def run_command(cmd: List[str], timeout: int, output_cfg: Dict[str, Any]) -> Dict[str, Any]:
    started = time.time()
    max_lines = int(output_cfg.get("max_lines", 200))
    max_chars = int(output_cfg.get("max_chars", 8000))
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        stdout_info = truncate_text(result.stdout or "", max_lines, max_chars)
        stderr_info = truncate_text(result.stderr or "", max_lines, max_chars)
        cmd_info = truncate_text(shlex.join(cmd), 1, 320)

        return {
            "ok": result.returncode == 0,
            "exit_code": result.returncode,
            "stdout": stdout_info["text"].strip(),
            "stderr": stderr_info["text"].strip(),
            "stdout_meta": stdout_info,
            "stderr_meta": stderr_info,
            "truncated": stdout_info["truncated"] or stderr_info["truncated"],
            "elapsed": round(time.time() - started, 3),
            "cmd": cmd_info["text"],
        }
    except subprocess.TimeoutExpired as exc:
        stdout = exc.stdout or ""
        stderr = exc.stderr or ""
        if isinstance(stdout, bytes):
            stdout = stdout.decode(errors="ignore")
        if isinstance(stderr, bytes):
            stderr = stderr.decode(errors="ignore")
        stderr = f"{stderr}\nTimeout after {timeout}s" if stderr else f"Timeout after {timeout}s"
        stdout_info = truncate_text(stdout or "", max_lines, max_chars)
        stderr_info = truncate_text(stderr or "", max_lines, max_chars)
        return {
            "ok": False,
            "exit_code": 124,
            "stdout": stdout_info["text"].strip(),
            "stderr": stderr_info["text"].strip(),
            "stdout_meta": stdout_info,
            "stderr_meta": stderr_info,
            "truncated": stdout_info["truncated"] or stderr_info["truncated"],
            "elapsed": round(time.time() - started, 3),
            "cmd": shlex.join(cmd),
        }
```

`run_on_node()` 把“本机执行”和“远程 SSH 执行”统一成同一个返回结构。

```python
def run_on_node(
    cfg: Dict[str, Any],
    node: Dict[str, Any],
    cmd: List[str],
    timeout_override: Optional[int] = None,
) -> Dict[str, Any]:
    timeout = int(cfg.get("controller", {}).get("command_timeout", 20))
    if timeout_override is not None:
        timeout = int(timeout_override)
    output_cfg = cfg.get("output", {})
    if is_local_node(node):
        return run_command(cmd, timeout, output_cfg)
    ssh_cmd = build_ssh_command(cfg, node, cmd)
    return run_command(ssh_cmd, timeout, output_cfg)
```

`normalize_cmds()` 是动作表能同时支持字符串模板和 argv 列表的原因。

```python
def normalize_cmds(cmds_spec: Any, ctx: Dict[str, Any]) -> List[List[str]]:
    if cmds_spec is None:
        return []

    if not isinstance(cmds_spec, list):
        cmds_spec = [cmds_spec]

    cmds: List[List[str]] = []
    for item in cmds_spec:
        if isinstance(item, str):
            rendered = render_template(item, ctx)
            cmds.append(["/bin/sh", "-lc", rendered])
        elif isinstance(item, list):
            rendered_parts = [render_template(str(part), ctx) for part in item]
            cmds.append(rendered_parts)
        else:
            continue
    return cmds
```

`api_action()` 中最重要的不是调用命令，而是在调用前完成参数约束。下面这段展示了必填参数、枚举、目标、十六进制和数值范围校验。

```python
if action not in ACTIONS:
    raise HTTPException(status_code=400, detail="未知操作")

spec = ACTIONS[action]
param_defs = spec.get("params", [])

for p in param_defs:
    name = p.get("name")
    required = bool(p.get("required", True))
    if required and (name not in params or params[name] in (None, "")):
        raise HTTPException(status_code=400, detail=f"缺少参数: {name}")

for name, allowed in PARAM_ENUMS.items():
    if name in params and params[name] not in allowed:
        raise HTTPException(status_code=400, detail=f"参数 {name} 非法")

target_def = next((p for p in param_defs if p.get("name") == "target"), {})
if "target" in params:
    target_ok = validate_kvm_target(cfg, params["target"]) if target_def.get("kvm_target") else validate_target(cfg, params["target"])
    if not target_ok:
        raise HTTPException(status_code=400, detail="目标节点无效")

if "addr" in params and params.get("addr"):
    if not validate_hex(params["addr"]):
        raise HTTPException(status_code=400, detail="地址必须为十六进制")

for name, (min_v, max_v) in NUM_RANGES.items():
    if name in params:
        value = int(params[name])
        if value < min_v or value > max_v:
            raise HTTPException(status_code=400, detail=f"参数 {name} 超出范围")
        params[name] = value
```

### 10.3 历史库：`web_controller/db.py`

`init_db()` 的关键代码是两张表：一次运行的主表 `fault_runs`，以及每个阶段命令结果的明细表 `fault_results`。

```python
conn.executescript(
    """
    CREATE TABLE IF NOT EXISTS fault_runs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        run_type TEXT NOT NULL,
        action_key TEXT,
        scenario_key TEXT,
        title TEXT,
        params_json TEXT NOT NULL DEFAULT '{}',
        ok INTEGER NOT NULL DEFAULT 0,
        started_at TEXT NOT NULL,
        finished_at TEXT NOT NULL,
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    );

    CREATE TABLE IF NOT EXISTS fault_results (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        run_id INTEGER NOT NULL,
        phase TEXT NOT NULL,
        check_title TEXT,
        node TEXT,
        host TEXT,
        cmd TEXT,
        stdout TEXT,
        stderr TEXT,
        exit_code INTEGER,
        elapsed REAL,
        ok INTEGER NOT NULL DEFAULT 0,
        truncated INTEGER NOT NULL DEFAULT 0,
        stdout_meta_json TEXT NOT NULL DEFAULT '{}',
        stderr_meta_json TEXT NOT NULL DEFAULT '{}',
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
        FOREIGN KEY (run_id) REFERENCES fault_runs(id) ON DELETE CASCADE
    );
    """
)
```

`record_run()` 展示了一次实验如何先写主记录，再按阶段写入明细。

```python
cur = conn.execute(
    """
    INSERT INTO fault_runs (
        run_type, action_key, scenario_key, title, params_json,
        ok, started_at, finished_at
    )
    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    """,
    (
        run_type,
        action_key,
        scenario_key,
        title,
        to_json(params or {}),
        1 if ok else 0,
        ts_to_iso(started_at),
        ts_to_iso(finished_at),
    ),
)
run_id = int(cur.lastrowid)
for phase in phases or []:
    _insert_results(
        conn,
        run_id,
        str(phase.get("phase") or "action"),
        phase.get("results", []),
        phase.get("check_title"),
    )
```

### 10.4 Chaos Mesh 命令构造：`web_controller/k8s_chaos.py`

`_manifest_apply_cmd()` 将 Python 字典压缩为 JSON，再通过 stdin 交给 `kubectl apply -f -`。

```python
def _manifest_apply_cmd(ctx: Dict[str, Any], manifest: Dict[str, Any]) -> List[str]:
    manifest_json = json.dumps(manifest, ensure_ascii=False, separators=(",", ":"))
    kubectl = _kubectl(ctx)
    script = f"printf %s {shlex.quote(manifest_json)} | {kubectl} apply -f -"
    return _shell_cmd(script)
```

Pod kill 的关键代码是生成 `PodChaos`。

```python
def pod_kill_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    manifest = {
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "PodChaos",
        "metadata": _metadata(params, "fi-pod-kill"),
        "spec": {
            "action": "pod-kill",
            "mode": str(params.get("chaos_mode") or "one"),
            "selector": _selector(params),
        },
    }
    return [_manifest_apply_cmd(ctx, manifest)]
```

网络延迟的关键代码是生成 `NetworkChaos`，其中 `delay` 字段决定延迟、抖动和相关性。

```python
def network_delay_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    latency = f"{int(params.get('ms', 200))}ms"
    jitter = f"{int(params.get('jitter', 50))}ms"
    correlation = str(int(params.get("correlation", 25)))
    manifest = {
        "apiVersion": "chaos-mesh.org/v1alpha1",
        "kind": "NetworkChaos",
        "metadata": _metadata(params, "fi-network-delay"),
        "spec": {
            "action": "delay",
            "mode": str(params.get("chaos_mode") or "all"),
            "selector": _selector(params),
            "delay": {
                "latency": latency,
                "jitter": jitter,
                "correlation": correlation,
            },
            "direction": "both",
            "target": _network_probe_target(params),
            "duration": _duration(params),
        },
    }
    return [_manifest_apply_cmd(ctx, manifest)]
```

### 10.5 前端参数渲染与执行：`web_controller/static/app.js`

`fetchJson()` 是前端所有 API 请求的统一包装，负责把 HTTP 错误转换为异常。

```javascript
async function fetchJson(url, options = {}) {
  const res = await fetch(url, options);
  if (!res.ok) {
    const data = await res.json().catch(() => ({}));
    throw new Error(data.detail || `请求失败: ${res.status}`);
  }
  return res.json();
}
```

`renderField()` 根据后端参数 schema 生成不同控件。这里体现了“后端定义参数，前端动态渲染表单”的模式。

```javascript
function renderField(param) {
  const wrapper = elc("label", "field");
  const label = elc("span", "field-label");
  label.textContent = param.label + (param.required ? "" : " (可选)");

  let input;
  if (param.type === "select") {
    input = document.createElement("select");
    (param.options || []).forEach(opt => {
      const option = document.createElement("option");
      option.value = opt.value; option.textContent = opt.label;
      input.appendChild(option);
    });
    if (param.default !== undefined) input.value = param.default;
  } else if (param.type === "node") {
    input = document.createElement("select");
    ((configCache && configCache.nodes) || []).forEach(n => {
      const option = document.createElement("option");
      option.value = n.name; option.textContent = `${n.name} (${n.host})`;
      input.appendChild(option);
    });
    if (param.default !== undefined) input.value = param.default;
  } else if (param.type === "number") {
    input = document.createElement("input");
    input.type = "number";
  } else {
    input = document.createElement("input");
    input.type = "text";
  }

  input.dataset.param = param.name;
  wrapper.appendChild(label);
  wrapper.appendChild(input);
  return wrapper;
}
```

`executeScenario()` 展示完整功能测试如何从按钮点击变成 `/api/functest` 请求。

```javascript
async function executeScenario(scenario, params, btn) {
  const startedAt = new Date();
  const btnText = btn.textContent;
  btn.textContent = "⏳ 执行中...";
  btn.disabled = true;

  try {
    const data = await fetchJson("/api/functest", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ key: scenario.key, params }),
    });
    lastRunParamsByKey[scenario.key] = { ...params };
    const entry = buildEnhancedHistoryEntry(scenario.title, data, startedAt);
    appendHistory(entry);
  } catch (err) {
    const entry = buildErrorEntry(scenario.title, scenario.key, err, startedAt);
    appendHistory(entry);
  } finally {
    btn.textContent = btnText;
    btn.disabled = false;
  }
}
```

### 10.6 VM 进程与网络故障：`process_injector.c`、`network_injector.c`

进程故障的核心就是查 PID 后发信号。

```c
void inject_process(const char *target, int action_type)
{
    int pid = get_vm_pid(target);
    if (pid == -1)
    {
        printf(" [错误] 未找到进程: %s\n", target);
        exit(1);
    }

    if (action_type == 1)
    { // Crash
        kill(pid, SIGKILL);
        printf(" [Crash] 已杀死进程 (PID: %d)\n", pid);
    }
    else if (action_type == 2)
    { // Hang
        kill(pid, SIGSTOP);
        printf("  [Hang] 已暂停进程 (PID: %d)\n", pid);
    }
    else if (action_type == 3)
    { // Resume
        kill(pid, SIGCONT);
        printf("  [Resume] 已恢复进程 (PID: %d)\n", pid);
    }
}
```

网络故障先清理旧规则，再按类型添加 `tc` 或 iptables 规则。

```c
int inject_network(int type, const char *param)
{
    char nic[32];
    get_interface_name(nic, sizeof(nic));
    char cmd[512];

    sprintf(cmd, "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);
    sprintf(cmd, "iptables -F OUTPUT 2>/dev/null");
    system(cmd);

    if (type == 0)
    {
        printf(" 网络故障已清理，网卡 %s 恢复正常\n", nic);
        return 0;
    }
    if (type == 1)
    {
        sprintf(cmd, "tc qdisc add dev %s root netem delay %s", nic, param);
    }
    else if (type == 2)
    {
        sprintf(cmd, "tc qdisc add dev %s root netem loss %s", nic, param);
    }
    else if (type == 3)
    {
        sprintf(cmd, "iptables -A OUTPUT -p tcp --dport %s -j DROP", param);
    }
    return system(cmd);
}
```

### 10.7 VM 资源故障：`cpu_injector.c`、`memleak_injector.c`

CPU 压力的关键在 worker：绑定核心、提高调度优先级、执行混合计算和内存访问，尽量避免被编译器优化为空循环。

```c
void *stress_worker(void *arg)
{
    int core_id = *(int *)arg;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    volatile double *arr = malloc(sizeof(double) * 10000);
    volatile double x = 1.0;
    volatile long long counter = 0;

    while (keep_running)
    {
        for (int i = 0; i < 1000; i++)
        {
            x = sqrt(x + 1.0) * sin(x) + cos(x * 0.1);
            counter += i * (i + 1);
            counter ^= (counter >> 3);
            if (arr)
            {
                arr[i % 100] = x;
                x += arr[(i + 50) % 100];
            }
        }
    }

    if (arr)
        free((void *)arr);
    return NULL;
}
```

内存泄漏注入器的关键是“分配后写入”，否则只保留虚拟地址空间，不一定占用物理页。下面是 `memleak_injector.c` 的实际核心循环。

```c
while (current_bytes < total_bytes)
{
    // 1. 申请虚拟内存
    char *ptr = (char *)malloc(chunk_size);
    if (ptr == NULL)
    {
        printf("\n malloc 失败！系统内存可能已耗尽。\n");
        break;
    }

    // 2. 写入数据 (Page Fault) 强制分配物理内存
    memset(ptr, 0xAA, chunk_size);

    current_bytes += chunk_size;

    printf("\r[Eat] 已占用: %4lld MB / %4d MB",
           current_bytes / 1024 / 1024, size_mb);
    fflush(stdout);

    usleep(50000);
}
```

### 10.8 VM 内存与寄存器故障：`mem_injector.c`、`reg_injector.c`

内存注入器使用 ptrace 读写目标进程地址空间。

```c
void ptrace_attach(pid_t pid)
{
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0)
        die("Attach failed");
    waitpid(pid, NULL, 0);
}

long ptrace_read(pid_t pid, unsigned long addr)
{
    errno = 0;
    long data = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, NULL);
    return data;
}

void ptrace_write(pid_t pid, unsigned long addr, long data)
{
    if (ptrace(PTRACE_POKEDATA, pid, (void *)addr, (void *)data) < 0)
        die("Write memory failed");
}
```

故障值的计算集中在 `apply_fault_logic()`。

```c
long apply_fault_logic(long original, InjectorContext *ctx)
{
    long corrupted = original;
    unsigned long mask = 1UL << ctx->target_bit;

    switch (ctx->type)
    {
    case FAULT_BIT_FLIP:
        corrupted = original ^ mask;
        break;
    case FAULT_STUCK_0:
        corrupted = original & (~mask);
        break;
    case FAULT_STUCK_1:
        corrupted = original | mask;
        break;
    case FAULT_BYTE_JUNK:
        corrupted = (original & ~0xFF) | (rand() % 0xFF);
        break;
    default:
        exit(1);
    }
    return corrupted;
}
```

寄存器故障同样先 attach，再修改寄存器快照。下面是寄存器值故障逻辑的核心。

```c
uint64_t apply_fault(uint64_t original, FaultType type, int user_specified_bit)
{
    uint64_t corrupted = original;
    int bit1 = (user_specified_bit >= 0) ? user_specified_bit : rand_bit();
    int bit2 = rand_bit();
    uint64_t mask_low_8 = 0xFFFFFFFFFFFFFF00;

    switch (type)
    {
    case FAULT_1_BIT_FLIP:
        corrupted ^= bit_mask(bit1);
        break;
    case FAULT_2_BIT_FLIP:
        if (bit2 == bit1)
            bit2 = (bit1 + 1) % 64;
        corrupted ^= bit_mask(bit1);
        corrupted ^= bit_mask(bit2);
        break;
    case FAULT_1_BIT_0:
        if (user_specified_bit < 0)
            bit1 = pick_bit_with_value(original, 1);
        if (bit1 >= 0)
            corrupted &= ~bit_mask(bit1);
        break;
    case FAULT_1_BIT_1:
        if (user_specified_bit < 0)
            bit1 = pick_bit_with_value(original, 0);
        if (bit1 >= 0)
            corrupted |= bit_mask(bit1);
        break;
    case FAULT_8_LOW_0:
        corrupted &= mask_low_8;
        break;
    case FAULT_8_LOW_1:
        corrupted |= 0xFF;
        break;
    }
    return corrupted;
}
```

### 10.9 QEMU/KVM 用户态入口：`vm_injection/kvm_injector.c`

`main()` 通过子命令分发不同故障。关键点是先把用户给的 VM 名或 PID 解析为 QEMU PID，再调用对应注入函数。

```c
if (strcmp(command, "list") == 0) {
    list_kvm_vms();
    return 0;
}
else if (strcmp(command, "soft-flip") == 0) {
    if (argc < 4) {
        printf(" 用法: %s soft-flip <目标> <寄存器> [位]\n", argv[0]);
        return 1;
    }
    int pid = resolve_qemu_target_pid(argv[2]);
    if (pid <= 0)
        return 1;
    int bit = (argc >= 5) ? atoi(argv[4]) : -1;
    return normalize_status(inject_soft_error(pid, SOFT_ERROR_BIT_FLIP, argv[3], bit));
}
else if (strcmp(command, "guest-data") == 0) {
    if (argc < 3) {
        printf(" 用法: %s guest-data <目标> [观察秒数]\n", argv[0]);
        return 1;
    }
    int pid = resolve_qemu_target_pid(argv[2]);
    if (pid <= 0)
        return 1;
    int duration = (argc >= 4) ? atoi(argv[3]) : 15;
    return normalize_status(inject_guest_behavior_fault(pid, 1, duration));
}
```

### 10.10 集群控制器：`kvm_injection/cluster_controller.c`

`load_cluster_config()` 展示了 `cluster.conf` 的解析方式。

```c
int load_cluster_config(const char *config_file) {
    FILE *fp = fopen(config_file, "r");
    if (!fp) {
        printf("  无法打开配置文件: %s，使用默认配置\n", config_file);
        init_hadoop_cluster();
        return -1;
    }

    char line[256];
    g_node_count = 0;

    while (fgets(line, sizeof(line), fp) && g_node_count < MAX_NODES) {
        if (line[0] == '#' || line[0] == '\n') continue;

        char *token = strtok(line, ",");
        if (token) {
            strcpy(g_cluster[g_node_count].name, token);
            token = strtok(NULL, ",");
            if (token) strcpy(g_cluster[g_node_count].ip, token);
            token = strtok(NULL, ",");
            if (token) g_cluster[g_node_count].ssh_port = atoi(token);
            token = strtok(NULL, ",\n");
            if (token) strcpy(g_cluster[g_node_count].role, token);
            g_cluster[g_node_count].is_active = 1;
            g_node_count++;
        }
    }
    fclose(fp);
    return 0;
}
```

`remote_exec()` 是跨节点执行的核心。

```c
int remote_exec(const char *node_name, const char *cmd) {
    int node_idx = -1;

    for (int i = 0; i < g_node_count; i++) {
        if (strcmp(g_cluster[i].name, node_name) == 0) {
            node_idx = i;
            break;
        }
    }

    if (node_idx < 0) {
        printf(" 未找到节点: %s\n", node_name);
        return -1;
    }

    char ssh_cmd[MAX_CMD_LEN];
    snprintf(ssh_cmd, sizeof(ssh_cmd),
             "ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "
             "-p %d root@%s '%s' 2>&1",
             g_cluster[node_idx].ssh_port,
             g_cluster[node_idx].ip,
             cmd);

    printf("[远程执行] %s -> %s\n", node_name, cmd);
    return system(ssh_cmd);
}
```

### 10.11 Hadoop 注入器：`hadoop_injector.c`

Hadoop 网络延迟注入支持“全局延迟”和“只对目标 IP 延迟”。目标 IP 模式通过 prio qdisc 和 u32 filter 实现。

```c
int inject_network_delay(const char *target_ip, int delay_ms, int jitter_ms)
{
    char cmd[512];
    char nic[32];

    get_default_nic(nic, sizeof(nic));
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);

    if (delay_ms <= 0)
    {
        printf(" [Network] 已清理网络延迟\n");
        return 0;
    }

    if (target_ip && strlen(target_ip) > 0)
    {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root handle 1: prio; "
                 "tc qdisc add dev %s parent 1:3 handle 30: netem delay %dms %dms; "
                 "tc filter add dev %s parent 1:0 protocol ip prio 3 u32 "
                 "match ip dst %s flowid 1:3",
                 nic, nic, delay_ms, jitter_ms, nic, target_ip);
    }
    else
    {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem delay %dms %dms",
                 nic, delay_ms, jitter_ms);
    }

    return system(cmd);
}
```

### 10.12 CloudStack 注入器：`cloudstack_injector.c`

CloudStack 进程类故障和 VM 进程故障一样，核心是组件名到 PID 的映射，以及 crash/hang/resume 信号。下面保留了源码中的错误检查和信号分发结构。

```c
int inject_cs_process_fault(CloudStackComponent component, CloudStackFaultType fault_type)
{
    const char *proc_name = get_cs_component_name(component);
    if (!proc_name)
    {
        printf(" 无效的组件类型\n");
        return -1;
    }

    int pid = find_cs_pid(proc_name);
    if (pid == -1)
    {
        printf(" 未找到进程: %s\n", proc_name);
        return -1;
    }

    printf("[CloudStack注入] 目标: %s (PID: %d)\n", proc_name, pid);

    switch (fault_type)
    {
    case CS_FAULT_CRASH:
        if (kill(pid, SIGKILL) == 0)
            printf(" [Crash] 已终止进程 %s\n", proc_name);
        else
            return -1;
        break;
    case CS_FAULT_HANG:
        if (kill(pid, SIGSTOP) == 0)
            printf("  [Hang] 已暂停进程 %s\n", proc_name);
        else
            return -1;
        break;
    case CS_FAULT_RESUME:
        if (kill(pid, SIGCONT) == 0)
            printf("  [Resume] 已恢复进程 %s\n", proc_name);
        else
            return -1;
        break;
    default:
        return -1;
    }

    return 0;
}
```

### 10.13 KVM 内核 kprobe：`cpu-reg.c`、`resource.c`

`cpu-reg.c` 的关键点是 pre-handler 判断开关和次数，触发后调用 `inject_fault_arm64(regs)` 修改寄存器上下文。

```c
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (inject_signal == 1 && fault_times > 0)
    {
        inject_fault_arm64(regs);
        fault_times--;
        if (fault_times <= 0)
        {
            inject_signal = 0;
            printk(KERN_INFO "[CPU-Fi] Injection Finished.\n");
        }
    }
    return 0;
}
```

procfs 写函数用宏生成，减少重复代码。

```c
#define DEFINE_PROC_WRITE(name, var)                                                                   \
    static ssize_t write_##name(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
    {                                                                                                  \
        char buffer[16];                                                                               \
        int val;                                                                                       \
        if (count > sizeof(buffer) - 1)                                                                \
            count = sizeof(buffer) - 1;                                                                \
        if (copy_from_user(buffer, buf, count))                                                        \
            return -EFAULT;                                                                            \
        buffer[count] = '\0';                                                                          \
        if (kstrtoint(buffer, 10, &val) == 0)                                                          \
            var = val;                                                                                 \
        return count;                                                                                  \
    }
```

`resource.c` 展示了 KVM ioctl 路径模块的通用结构：handler 读取控制变量，`module_init` 注册 kprobe 并创建 procfs。

```c
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (signal == 0) return 0;

    if (time_cnt != -1) {
        if (time_cnt > 0) {
            time_cnt--;
        } else {
            signal = 0;
            return 0;
        }
    }

    change_arm64(regs, position, type);
    return 0;
}

static int __init my_res_init(void)
{
    kp.symbol_name = TARGET_FUNC;
    kp.pre_handler = handler_pre;

    if (register_kprobe(&kp) < 0) {
        return -1;
    }

    dir = proc_mkdir("resource", NULL);
    if(dir) {
        proc_create("time", PERMISSION, dir, &time_fops);
        proc_create("position", PERMISSION, dir, &pos_fops);
        proc_create("type", PERMISSION, dir, &type_fops);
        proc_create("signal", PERMISSION, dir, &sig_fops);
        proc_create("style", PERMISSION, dir, &style_fops);
    }
    return 0;
}
```

### 10.14 KVM 内核 kretprobe：`pt-load-fi.c`

`pt-load-fi.c` 在 `handle_mm_fault` 返回后修改 ARM64 返回寄存器 `x0`，让缺页处理看起来返回 OOM 或 SIGBUS。

```c
static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0) {

        if (fault_type == 0) {
            regs->regs[0] = VM_FAULT_OOM;
            printk(KERN_INFO "[ARM-Mem-Fi] handle_mm_fault FORCE Return: VM_FAULT_OOM\n");
        } else {
            regs->regs[0] = VM_FAULT_SIGBUS;
            printk(KERN_INFO "[ARM-Mem-Fi] handle_mm_fault FORCE Return: VM_FAULT_SIGBUS\n");
        }

        fault_times--;
        if (fault_times <= 0) {
            inject_signal = 0;
            printk(KERN_INFO "[ARM-Mem-Fi] Injection finished.\n");
        }
    }
    return 0;
}
```

注册 kretprobe 时设置目标函数、entry handler、return handler 和并发实例数。

```c
static int __init my_mem_init(void)
{
    rp.kp.symbol_name = TARGET_FUNC;
    rp.entry_handler = entry_handler;
    rp.handler = ret_handler;
    rp.maxactive = 20;

    if (register_kretprobe(&rp) < 0) {
        printk(KERN_ERR "[ARM-Mem-Fi] register_kretprobe failed on %s\n", TARGET_FUNC);
        return -1;
    }

    pdir = proc_mkdir(PROC_DIR, NULL);
    if(pdir) {
        proc_create("signal", 0666, pdir, &signal_fops);
        proc_create("times", 0666, pdir, &times_fops);
        proc_create("type", 0666, pdir, &type_fops);
    }
    return 0;
}
```

### 10.15 文件路径内核故障：`file-read-fi.c`

文件读取模块挂钩 `vfs_read`，通过修改 ARM64 参数寄存器模拟空读或坏 buffer。

```c
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0)
    {
        if (fault_type == 0)
        {
            regs->regs[2] = 0;
            printk(KERN_INFO "[ARM-Fi-Read] vfs_read: Force count=0\n");
        }
        else if (fault_type == 1)
        {
            regs->regs[1] = 0;
            printk(KERN_INFO "[ARM-Fi-Read] vfs_read: Force buf=NULL\n");
        }

        fault_times--;
        if (fault_times <= 0)
        {
            inject_signal = 0;
            printk(KERN_INFO "[ARM-Fi-Read] Injection finished.\n");
        }
    }
    return 0;
}
```

### 10.16 用户态控制内核模块：`*-main.c`

控制程序的共同模式是把用户输入写入 `/proc/<module>/...`。以内存管理模块为例：

```c
#define PROC_BASE "/proc/memory-manage-fi"

void write_proc(const char *file, int val) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo %d > %s/%s", val, PROC_BASE, file);
    system(cmd);
}

int main(int argc, char **argv)
{
    int input;
    if(geteuid()!=0){printf("Need root\n");return 1;}

    printf("目标函数:\n 1. kvm_set_memory_region\n 2. gfn_to_hva_many\nChoice: ");
    scanf("%d", &input);
    write_proc("class", input);

    printf("故障参数位置 (1-8对应X0-X7):\nChoice: ");
    scanf("%d", &input);
    write_proc("position", input);

    printf("故障类型:\n 1. Flip\n 2. Set1\n 3. Set0\nChoice: ");
    scanf("%d", &input);
    write_proc("type", input);

    printf("故障次数: ");
    scanf("%d", &input);
    write_proc("time", input);

    write_proc("signal", 1);
    return 0;
}
```

这段代码体现了 KVM 内核模块的控制链路：用户态 main 不直接修改内核，只写 procfs；真正的修改发生在内核模块 handler 中。

## 11. 维护建议

| 问题 | 建议 |
| --- | --- |
| 前端调用 `/api/recover/all`，后端未见对应路由 | 补充 FastAPI 路由或修改前端为已有 action。 |
| `test_scenarios.py` 中旧版 Hadoop/CloudStack 场景未进入最终 `FUNC_TESTS` | 如果需要前端展示完整 Hadoop/CloudStack 功能测试，应重新追加这些场景。 |
| 内核模块依赖内核符号名 | 固定实验内核版本，构建前确认目标函数仍存在。 |
| kprobe/kretprobe 修改内核路径风险高 | 每次实验前准备恢复手段，避免在生产机器加载。 |
| procfs 控制参数缺少强类型 | 用户态 main 应尽量增加范围校验，避免向内核写入非法参数。 |
| 网络故障依赖 `tc` | 清理命令必须可靠，否则会残留 qdisc 影响后续实验。 |
