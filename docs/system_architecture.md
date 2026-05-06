# 系统架构图

本文档基于当前仓库实现梳理整个故障注入平台的系统架构，重点覆盖 `web_controller`、`vm_injection`、`kvm_injection`、QEMU/KVM 虚拟化环境以及 Hadoop/CloudStack 被测对象之间的关系。

## 1. 总体逻辑架构图

```mermaid
flowchart TB
    User["实验人员 / 运维人员"]

    subgraph CP["控制面（宿主机）"]
        UI["Web 前端<br/>index.html + app.js + styles.css"]
        API["FastAPI 控制器<br/>web_controller/app.py"]
        CFG["配置中心<br/>config.json"]
        CASE["功能测试编排<br/>test_scenarios.py<br/>baseline -> action -> verify -> cleanup"]
        EXEC["执行器<br/>run_on_node / run_command / SSH"]
        AUTO["启动辅助<br/>startup -> auto_start_vms()"]
    end

    subgraph CLI["命令行控制入口（可选）"]
        CLUSTER_CTRL["kvm_injection/cluster_controller"]
        VM_CTRL["vm_injection/fault_controller"]
        MANAGE["kvm_injection/cluster_manage.sh"]
    end

    subgraph INJ["故障注入执行层"]
        HDFSI["Hadoop 注入器<br/>kvm_injection/hadoop-fi/hadoop_injector"]
        CSI["CloudStack 注入器<br/>kvm_injection/cloudstack-fi/cloudstack_injector"]
        VMI["VM 用户态注入器<br/>process/network/cpu/mem/reg/mem_leak"]
        KVMI["KVM 用户态入口<br/>vm_injection/kvm_injector"]
        LKMS["KVM/内核模块集<br/>cpu-fi / memory-fi / file-fi / state-query-fi / vm-migration-fi / access-control-fi"]
    end

    subgraph TARGET["被测环境"]
        HOST["宿主机 OS<br/>Linux / Ubuntu"]
        HYP["虚拟化层<br/>QEMU + KVM + Linux Kernel"]
        subgraph GUESTS["3 节点 Guest 集群"]
            MASTER["master<br/>NameNode / ResourceManager / SecondaryNameNode"]
            SLAVE1["slave1<br/>DataNode / NodeManager"]
            SLAVE2["slave2<br/>DataNode / NodeManager"]
        end
        CLOUD["CloudStack 管理面 / 组件（可选）"]
    end

    User --> UI
    UI -->|/api/config /api/action /api/functest| API
    API --> CFG
    API --> CASE
    API --> EXEC
    API --> AUTO

    CLUSTER_CTRL --> HDFSI
    CLUSTER_CTRL --> CSI
    CLUSTER_CTRL --> VMI
    VM_CTRL --> VMI
    MANAGE --> HDFSI

    EXEC -->|SSH 到 master/slave| HDFSI
    EXEC -->|SSH 到管理节点| CSI
    EXEC -->|本地执行| VMI
    EXEC -->|本地执行| KVMI
    KVMI --> LKMS

    AUTO --> HYP
    VMI --> HOST
    KVMI --> HYP
    LKMS --> HYP
    HDFSI --> MASTER
    HDFSI --> SLAVE1
    HDFSI --> SLAVE2
    CSI --> CLOUD
    HOST --> HYP
    HYP --> MASTER
    HYP --> SLAVE1
    HYP --> SLAVE2
```

## 2. 部署与执行路径图

```mermaid
flowchart LR
    subgraph HOST["宿主机 / 控制节点"]
        START["start_frontend.sh"]
        UVI["uvicorn + FastAPI"]
        WEB["浏览器访问 8080"]
        VMTOOLS["vm_injection<br/>用户态注入工具"]
        KVMUSER["vm_injection/kvm_injector"]
        KVMMOD["kvm_injection/*<br/>内核模块源码 / CLI 控制器"]
        QEMU["run_cluster.sh<br/>qemu-system-aarch64 x3"]
        KERNEL["Linux Kernel / KVM / /dev/kvm"]
        LOCALPROC["宿主机本地目标<br/>QEMU 进程 / 测试靶子 / 网络栈"]
    end

    subgraph MASTER["VM: master"]
        SSHM["SSH 127.0.0.1:2220"]
        HADOOPM["NameNode<br/>ResourceManager<br/>SecondaryNameNode"]
        HINJ["hadoop_injector"]
    end

    subgraph SLAVE1["VM: slave1"]
        SSH1["SSH 127.0.0.1:2221"]
        HADOOP1["DataNode<br/>NodeManager"]
    end

    subgraph SLAVE2["VM: slave2"]
        SSH2["SSH 127.0.0.1:2222"]
        HADOOP2["DataNode<br/>NodeManager"]
    end

    subgraph OPTIONAL["可选 CloudStack 节点/组件"]
        CSSVC["Management Server / Agent / MySQL / NFS"]
    end

    WEB --> UVI
    START --> QEMU
    START --> UVI
    UVI -->|读取 config.json| SSHM
    UVI -->|读取 config.json| SSH1
    UVI -->|读取 config.json| SSH2
    UVI -->|本地动作| VMTOOLS
    UVI -->|本地动作| KVMUSER

    QEMU --> KERNEL
    KERNEL --> MASTER
    KERNEL --> SLAVE1
    KERNEL --> SLAVE2

    SSHM --> HINJ
    HINJ --> HADOOPM
    HINJ --> HADOOP1
    HINJ --> HADOOP2

    SSH1 --> HADOOP1
    SSH2 --> HADOOP2

    VMTOOLS -->|ptrace / tc / iptables / signal| LOCALPROC
    KVMUSER -->|软错误 / 性能故障 / 热插拔入口| KERNEL
    KVMMOD -->|insmod / kprobe / hook| KERNEL
    LOCALPROC --> QEMU

    UVI -->|可选 SSH| CSSVC
```

## 3. 模块职责对应

- `web_controller`
  - 提供统一 Web 控制台、动作接口、功能测试接口、健康检查接口。
  - 通过 `config.json` 决定节点拓扑、SSH 连接方式和工具二进制路径。
  - 将请求分发为两类执行路径：本地执行、远程 SSH 执行。

- `kvm_injection`
  - 提供面向 Hadoop/CloudStack 的用户态注入器。
  - 提供面向 KVM/内核的模块源码与统一 CLI 控制器。
  - 适合宿主机级、虚拟化层级、管理平面级故障注入。

- `vm_injection`
  - 提供进程、网络、CPU、内存、寄存器等 Guest/用户态注入器。
  - 既可作用于虚拟机内部业务进程，也可在宿主机上针对 QEMU 相关进程做干预。

- `run_cluster.sh` 与 `start_frontend.sh`
  - 负责拉起 3 节点 QEMU 集群与 Web 控制器。
  - 当前默认通过宿主机端口转发 `2220/2221/2222` 访问三台 VM。

## 4. 当前实现下的关键控制流

1. 用户在浏览器中发起注入或功能测试。
2. 前端调用 FastAPI 的 `/api/action` 或 `/api/functest`。
3. 后端按 `ACTIONS` 或 `FUNC_TESTS` 解析参数、选择目标节点、决定是否加 `sudo`。
4. Hadoop/CloudStack 动作通常通过 SSH 落到远端节点执行。
5. VM/KVM 动作通常在宿主机本地执行注入器。
6. 执行结果被裁剪、结构化后返回前端展示。

## 5. 建图说明

- 图中同时保留了 Web 控制路径和 CLI 控制路径，因为这两条路径在仓库中都是真实存在的入口。
- CloudStack 在当前仓库中属于可选被测对象，因此在部署图中单独作为可选区域表示。
- KVM 故障注入实际分为“用户态入口工具”和“内核模块 Hook 点”两层，图中已拆开表示。
