- # **CloudStack Injector 模拟环境故障注入验证**

  ## **一、概览**

  **目的**：在无法启动真实 CloudStack 的情况下，使用模拟进程与虚拟机环境，系统性验证 cloudstack_injector 的各类故障注入能力，覆盖管理节点、Agent、系统虚拟机、网络、存储及数据库等关键组件。

  **已验证功能总览**：

  - 进程类：list / hang / resume / crash
  - 资源类：cpu-stress / mem-stress
  - 网络类：api-delay / network / network-clear / agent-disconnect
  - 虚拟机类：sysvm-crash / vm-migrate-fail
  - 存储类：storage-ro / storage-fill
  - 数据库类：db-lock

  ## **二、模拟环境与启动方式**

  ### **1. 管理组件进程模拟（Mac / Linux 通用）**

  ```
  # 模拟 Management Server / Agent / MySQL
  exec -a cloudstack-management sleep 10000 &
  exec -a cloudstack-agent sleep 10000 &
  exec -a mysqld sleep 10000 &
  ```

  ### **2. 系统虚拟机模拟（用于 VM / SysVM 故障测试）**

  ```
  # 二级存储虚拟机（SSVM）
  exec -a "qemu-system-x86_64 -name guest=s-1-VM" sleep 10000 &
  
  # 控制台代理虚拟机（CPVM）
  exec -a "qemu-system-x86_64 -name guest=v-2-VM" sleep 10000 &
  
  # 虚拟路由器（VR）
  exec -a "qemu-system-x86_64 -name guest=r-3-VM" sleep 10000 &
  ```

  ### **3. API 服务模拟（Ubuntu VM）**

  ```
  pkill -f "python3 -m http.server 8080"
  python3 -m http.server 8080 --bind 0.0.0.0 &
  ```

  ## **三、常用诊断与排查命令**

  ```
  # 进程
  pgrep -f cloudstack-management
  pgrep -f cloudstack-agent
  
  # 端口
  ss -tlnp | grep 8080
  
  # 路由 / 默认网卡
  ip route get 8.8.8.8
  
  # tc 规则
  tc qdisc show dev enp0s1
  tc filter show dev enp0s1
  sudo tc qdisc del dev enp0s1 root 2>/dev/null
  
  # iptables
  sudo iptables -L -n -v
  
  # 连通性测试
  ping -c 2 8.8.8.8
  time curl http://192.168.64.4:8080/
  ```

  ## **四、核心测试用例与结果**

  ### **1）组件列表检测（list）**

  ```
  sudo ./cloudstack_injector list
  ```

  **结果**：可正确识别 Management / Agent / MySQL 及 API 监听端口。

  ### **2）Agent 挂起与恢复（hang / resume）**

  ```
  sudo ./cloudstack_injector hang agent
  sudo ./cloudstack_injector resume agent
  ```

  **结果**：Agent 进程状态 S ↔ T 正常切换。

  ### **3）Management Server 崩溃（crash ms）**

  ```
  sudo ./cloudstack_injector crash ms
  ```

  **结果**：Management 进程被 SIGKILL 成功终止。

  ### **4）CPU 压力注入（cpu-stress）**

  ```
  sudo ./cloudstack_injector cpu-stress 5 2
  ```

  **结果**：CPU 使用率显著上升，结束后自动恢复。

  ### **5）API 延迟注入（api-delay）**

  ```
  sudo ./cloudstack_injector api-delay 5000
  time curl http://192.168.64.4:8080/
  ```

  **实现要点**：

  - 延迟作用于 **响应流量**
  - tc filter 使用 match ip sport 8080

  **结果**：API 响应延迟约 5–6 秒，注入生效。

  ### **6）Agent 通信中断（agent-disconnect）**

  ```
  sudo ./cloudstack_injector agent-disconnect
  sudo ./cloudstack_injector agent-reconnect
  ```

  **结果**：成功阻断 / 恢复 Agent → Management 的 8250 端口通信。

  ### **7）内存压力测试（mem-stress）**

  ```
  sudo ./cloudstack_injector mem-stress 200
  sudo ./cloudstack_injector mem-stress-clear
  ```

  **结果**：系统可用内存下降约 200MB，清理后恢复。

  ### **8）虚拟机迁移失败模拟（vm-migrate-fail）**

  ```
  sudo ./cloudstack_injector vm-migrate-fail
  sudo ./cloudstack_injector vm-op-clear
  ```

  **结果**：通过高延迟 + 丢包网络条件阻断迁移流程。

  ### **9）存储只读故障（storage-ro）**

  ```
  sudo ./cloudstack_injector storage-ro /tmp/cs_secondary
  sudo ./cloudstack_injector storage-rw /tmp/cs_secondary
  ```

  **结果**：挂载点变为只读，写入失败，恢复正常。

  ### **10）存储空间耗尽（storage-fill）**

  ```
  sudo ./cloudstack_injector storage-fill /tmp/cs_secondary
  sudo ./cloudstack_injector storage-clean /tmp/cs_secondary
  ```

  **结果**：成功制造磁盘满场景并清理。

  ### **11）数据库锁故障（db-lock）**

  ```
  sudo ./cloudstack_injector db-lock
  sudo ./cloudstack_injector db-unlock
  ```

  **结果**：工具正确尝试下发 LOCK TABLES，无真实 DB 环境下返回连接错误，逻辑验证通过。

  ### **12）系统虚拟机崩溃（sysvm-crash）**

  ```
  sudo ./cloudstack_injector crash ssvm
  ```

  **结果**：模拟的 SSVM 进程被成功终止，验证 SysVM 识别与故障注入能力。

  ## **五、结论**

  在无真实 CloudStack 环境下，通过进程级、网络级、存储级与资源级模拟，cloudstack_injector 已验证具备覆盖核心故障场景的能力，可作为 Chaos / 故障演练的基础工具。