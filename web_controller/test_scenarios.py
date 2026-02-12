"""Functional test scenario definitions.

Each scenario specifies:
- baseline commands (run before action)
- the action itself
- verify commands (run after action)
- optional cleanup action

Architecture note:
- Hadoop tests (cluster, process, network, resource, hdfs, mapreduce)
  execute inside VMs via SSH (scope: "master" / "all")
- VM / KVM injection tests execute on the Ubuntu host directly
  (scope: "local")
"""

from __future__ import annotations

from typing import Any, Dict, List

# ---------------------------------------------------------------------------
# Test scenario structure
# ---------------------------------------------------------------------------
# {
#     "key": unique id,
#     "title": display name,
#     "desc": long description,
#     "group": matches GROUPS in app.py,
#     "params": [{ name, label, type, ... }],   (user-fillable params)
#     "baseline": [{ "title", "cmd", "scope" }], (pre-action checks)
#     "action": action_key in ACTIONS,
#     "action_params": {},                        (default params for action)
#     "verify": [{ "title", "cmd", "scope" }],    (post-action checks)
#     "cleanup": action_key or None,
#     "cleanup_params": {},
# }
# ---------------------------------------------------------------------------

FUNC_TESTS: List[Dict[str, Any]] = [
    # =================================================================
    #  集群管理  (VM 内 — 通过 SSH)
    # =================================================================
    {
        "key": "test_cluster_status",
        "title": "集群进程状态检测",
        "desc": "在所有节点运行 jps，检测 Hadoop 各组件是否正常运行。",
        "group": "cluster",
        "params": [],
        "baseline": [],
        "action": "cluster_status",
        "action_params": {},
        "verify": [],
        "cleanup": None,
    },
    {
        "key": "test_inject_list",
        "title": "Injector 进程清单",
        "desc": "通过 hadoop_injector list 获取集群进程清单。",
        "group": "cluster",
        "params": [],
        "baseline": [],
        "action": "inject_list",
        "action_params": {},
        "verify": [],
        "cleanup": None,
    },
    {
        "key": "test_hadoop_restart",
        "title": "Hadoop 重启测试",
        "desc": "先检查进程状态，重启 Hadoop，再检查进程状态，对比重启前后。",
        "group": "cluster",
        "params": [],
        "baseline": [
            {"title": "重启前进程 (jps)", "cmd": "jps", "scope": "all"},
        ],
        "action": "hadoop_restart",
        "action_params": {},
        "verify": [
            {"title": "重启后进程 (jps)", "cmd": "jps", "scope": "all"},
        ],
        "cleanup": None,
    },

    # =================================================================
    #  Hadoop 进程故障  (VM 内 — 通过 SSH)
    # =================================================================
    {
        "key": "test_process_crash",
        "title": "进程崩溃测试",
        "desc": "崩溃指定组件进程，对比崩溃前后 jps 输出。",
        "group": "process",
        "params": [
            {
                "name": "component",
                "label": "组件",
                "type": "select",
                "options": [
                    {"value": "nn", "label": "NameNode"},
                    {"value": "dn", "label": "DataNode"},
                    {"value": "rm", "label": "ResourceManager"},
                    {"value": "nm", "label": "NodeManager"},
                ],
                "default": "dn",
                "required": True,
            },
        ],
        "baseline": [
            {"title": "崩溃前进程列表", "cmd": "jps", "scope": "all"},
        ],
        "action": "process_fault",
        "action_params": {"op": "crash"},
        "verify": [
            {"title": "崩溃后进程列表", "cmd": "jps", "scope": "all"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_process_hang_resume",
        "title": "进程挂起/恢复测试",
        "desc": "挂起进程后检查状态，再恢复并检查进程是否正常。",
        "group": "process",
        "params": [
            {
                "name": "component",
                "label": "组件",
                "type": "select",
                "options": [
                    {"value": "nn", "label": "NameNode"},
                    {"value": "dn", "label": "DataNode"},
                    {"value": "rm", "label": "ResourceManager"},
                    {"value": "nm", "label": "NodeManager"},
                ],
                "default": "dn",
                "required": True,
            },
        ],
        "baseline": [
            {"title": "挂起前进程列表", "cmd": "jps", "scope": "all"},
        ],
        "action": "process_fault",
        "action_params": {"op": "hang"},
        "verify": [
            {"title": "挂起后进程列表", "cmd": "jps", "scope": "all"},
        ],
        "cleanup": "process_fault",
        "cleanup_params_override": {"op": "resume"},
    },

    # =================================================================
    #  集群网络故障  (VM 内 — 通过 SSH)
    # =================================================================
    {
        "key": "test_delay",
        "title": "网络延迟注入测试",
        "desc": "注入延迟前后分别 ping 目标节点，对比延迟变化。",
        "group": "network",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "ms", "label": "延迟 (ms)", "type": "number", "default": 200, "required": True},
        ],
        "baseline": [
            {"title": "注入前 ping 测试", "cmd": "ping -c 4 -W 2 {target}", "scope": "master"},
            {"title": "注入前 tc 规则", "cmd": "tc qdisc show dev eth0 2>/dev/null || echo 'no tc rules'", "scope": "master"},
        ],
        "action": "delay",
        "action_params": {},
        "verify": [
            {"title": "注入后 ping 测试", "cmd": "ping -c 4 -W 2 {target}", "scope": "master"},
            {"title": "注入后 tc 规则", "cmd": "tc qdisc show dev eth0 2>/dev/null || echo 'no tc rules'", "scope": "master"},
        ],
        "cleanup": "delay_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_loss",
        "title": "网络丢包注入测试",
        "desc": "注入丢包前后分别 ping 目标节点，对比丢包率变化。",
        "group": "network",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "percent", "label": "丢包率 (%)", "type": "number", "default": 30, "required": True},
        ],
        "baseline": [
            {"title": "注入前 ping 测试", "cmd": "ping -c 10 -W 2 {target}", "scope": "master"},
        ],
        "action": "loss",
        "action_params": {},
        "verify": [
            {"title": "注入后 ping 测试", "cmd": "ping -c 10 -W 2 {target}", "scope": "master"},
        ],
        "cleanup": "loss_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_reorder",
        "title": "网络乱序注入测试",
        "desc": "注入乱序前后对比 tc 规则状态。",
        "group": "network",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "percent", "label": "乱序率 (%)", "type": "number", "default": 25, "required": True},
        ],
        "baseline": [
            {"title": "注入前 ping 测试", "cmd": "ping -c 4 -W 2 {target}", "scope": "master"},
        ],
        "action": "reorder",
        "action_params": {},
        "verify": [
            {"title": "注入后 ping 测试", "cmd": "ping -c 4 -W 2 {target}", "scope": "master"},
        ],
        "cleanup": "reorder_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_isolate",
        "title": "网络隔离测试",
        "desc": "隔离节点前后对比连通性。",
        "group": "network",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
        ],
        "baseline": [
            {"title": "隔离前 ping 测试", "cmd": "ping -c 3 -W 2 {target}", "scope": "master"},
        ],
        "action": "isolate",
        "action_params": {},
        "verify": [
            {"title": "隔离后 ping 测试", "cmd": "ping -c 3 -W 2 {target}", "scope": "master"},
        ],
        "cleanup": "isolate_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_heartbeat",
        "title": "心跳超时测试",
        "desc": "模拟心跳超时前后，检查 YARN 节点状态。",
        "group": "network",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "ms", "label": "超时 (ms)", "type": "number", "default": 5000, "required": True},
        ],
        "baseline": [
            {"title": "超时前 YARN 节点", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/yarn node -list 2>/dev/null | head -10", "scope": "master"},
        ],
        "action": "heartbeat",
        "action_params": {},
        "verify": [
            {"title": "超时后 YARN 节点", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/yarn node -list 2>/dev/null | head -10", "scope": "master"},
        ],
        "cleanup": "delay_clear",
        "cleanup_params": {},
    },

    # =================================================================
    #  集群资源故障  (VM 内 — 通过 SSH)
    # =================================================================
    {
        "key": "test_cpu_stress",
        "title": "CPU 压力测试",
        "desc": "施加 CPU 压力前后，对比目标节点负载。",
        "group": "resource",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 10, "required": True},
        ],
        "baseline": [
            {"title": "压力前负载", "cmd": "uptime", "scope": "master"},
            {"title": "压力前 top 5 进程", "cmd": "ps -eo pid,pcpu,comm --sort=-pcpu | head -5", "scope": "master"},
        ],
        "action": "cpu_stress",
        "action_params": {},
        "verify": [
            {"title": "压力后负载", "cmd": "uptime", "scope": "master"},
            {"title": "压力后 top 5 进程", "cmd": "ps -eo pid,pcpu,comm --sort=-pcpu | head -5", "scope": "master"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_mem_stress",
        "title": "内存压力测试",
        "desc": "消耗指定内存前后，对比 /proc/meminfo。",
        "group": "resource",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "size_mb", "label": "内存 (MB)", "type": "number", "default": 256, "required": True},
        ],
        "baseline": [
            {"title": "压力前内存", "cmd": "cat /proc/meminfo | head -5", "scope": "master"},
        ],
        "action": "mem_stress",
        "action_params": {},
        "verify": [
            {"title": "压力后内存", "cmd": "cat /proc/meminfo | head -5", "scope": "master"},
        ],
        "cleanup": "mem_stress_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_disk_fill",
        "title": "磁盘填充测试",
        "desc": "填充磁盘前后，对比 df 输出。",
        "group": "resource",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "size_mb", "label": "大小 (MB)", "type": "number", "default": 256, "required": True},
        ],
        "baseline": [
            {"title": "填充前磁盘", "cmd": "df -h / | head -5", "scope": "master"},
        ],
        "action": "disk_fill",
        "action_params": {},
        "verify": [
            {"title": "填充后磁盘", "cmd": "df -h / | head -5", "scope": "master"},
        ],
        "cleanup": "disk_fill_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_io_slow",
        "title": "磁盘 I/O 限速测试",
        "desc": "限速前后对比 I/O 写入速度。",
        "group": "resource",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
        ],
        "baseline": [
            {"title": "限速前写入速度", "cmd": "dd if=/dev/zero of=/tmp/iotest bs=1M count=10 2>&1; rm -f /tmp/iotest", "scope": "master"},
        ],
        "action": "io_slow",
        "action_params": {"state": "on"},
        "verify": [
            {"title": "限速后写入速度", "cmd": "dd if=/dev/zero of=/tmp/iotest bs=1M count=10 2>&1; rm -f /tmp/iotest", "scope": "master"},
        ],
        "cleanup": "io_slow",
        "cleanup_params": {"state": "off"},
    },

    # =================================================================
    #  HDFS / YARN  (VM 内 — 通过 SSH)
    # =================================================================
    {
        "key": "test_hdfs_safe",
        "title": "HDFS 安全模式测试",
        "desc": "进入安全模式后检查 HDFS 状态，再退出并验证。",
        "group": "hdfs",
        "params": [],
        "baseline": [
            {"title": "安全模式前 HDFS 状态", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/hdfs dfsadmin -safemode get 2>/dev/null", "scope": "master"},
            {"title": "安全模式前目录列表", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/hdfs dfs -ls / 2>/dev/null | head -5", "scope": "master"},
        ],
        "action": "hdfs_safe",
        "action_params": {"mode": "enter"},
        "verify": [
            {"title": "进入安全模式后 HDFS 状态", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/hdfs dfsadmin -safemode get 2>/dev/null", "scope": "master"},
            {"title": "安全模式下写入测试", "cmd": ". /etc/profile >/dev/null 2>&1; echo test | /opt/hadoop/bin/hdfs dfs -put - /tmp/__safemode_test 2>&1 || true", "scope": "master"},
        ],
        "cleanup": "hdfs_safe",
        "cleanup_params": {"mode": "leave"},
    },
    {
        "key": "test_hdfs_disk",
        "title": "HDFS 磁盘不足测试",
        "desc": "填充磁盘模拟 HDFS 空间不足，对比填充前后。",
        "group": "hdfs",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "size_mb", "label": "大小 (MB)", "type": "number", "default": 256, "required": True},
        ],
        "baseline": [
            {"title": "填充前磁盘", "cmd": "df -h / | head -5", "scope": "master"},
            {"title": "填充前 HDFS 报告", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/hdfs dfsadmin -report 2>/dev/null | head -15", "scope": "master"},
        ],
        "action": "hdfs_disk",
        "action_params": {},
        "verify": [
            {"title": "填充后磁盘", "cmd": "df -h / | head -5", "scope": "master"},
            {"title": "填充后 HDFS 报告", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/hdfs dfsadmin -report 2>/dev/null | head -15", "scope": "master"},
        ],
        "cleanup": "disk_fill_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_yarn_unhealthy",
        "title": "YARN 节点不健康测试",
        "desc": "标记节点不健康前后，对比 YARN 节点列表。",
        "group": "hdfs",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
        ],
        "baseline": [
            {"title": "标记前 YARN 节点", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/yarn node -list -all 2>/dev/null | head -10", "scope": "master"},
        ],
        "action": "yarn_unhealthy",
        "action_params": {"state": "on"},
        "verify": [
            {"title": "标记后 YARN 节点", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/yarn node -list -all 2>/dev/null | head -10", "scope": "master"},
        ],
        "cleanup": "yarn_unhealthy",
        "cleanup_params": {"state": "off"},
    },

    # =================================================================
    #  MapReduce 任务  (VM 内 — 通过 SSH)
    # =================================================================
    {
        "key": "test_mapreduce_fault",
        "title": "MapReduce 任务故障测试",
        "desc": "杀死 Map/Reduce 任务进程，对比前后状态。",
        "group": "mapreduce",
        "params": [
            {
                "name": "task",
                "label": "任务类型",
                "type": "select",
                "options": [
                    {"value": "map", "label": "Map"},
                    {"value": "reduce", "label": "Reduce"},
                ],
                "default": "map",
                "required": True,
            },
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
        ],
        "baseline": [
            {"title": "故障前 YARN 应用", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/yarn application -list 2>/dev/null | head -10", "scope": "master"},
            {"title": "故障前进程列表", "cmd": "jps", "scope": "all"},
        ],
        "action": "mapreduce_fault",
        "action_params": {},
        "verify": [
            {"title": "故障后 YARN 应用", "cmd": ". /etc/profile >/dev/null 2>&1; /opt/hadoop/bin/yarn application -list 2>/dev/null | head -10", "scope": "master"},
            {"title": "故障后进程列表", "cmd": "jps", "scope": "all"},
        ],
        "cleanup": None,
    },

    # =================================================================
    #  VM 注入  (Ubuntu 宿主机 — 本地执行)
    # =================================================================
    {
        "key": "test_vm_process",
        "title": "VM 进程控制测试",
        "desc": "对虚拟机内进程执行崩溃/挂起操作，对比前后进程列表。",
        "group": "vm",
        "params": [
            {"name": "process", "label": "进程名", "type": "text", "required": True, "placeholder": "target"},
            {
                "name": "proc_action",
                "label": "操作",
                "type": "select",
                "options": [
                    {"value": "crash", "label": "崩溃"},
                    {"value": "hang", "label": "挂起"},
                ],
                "default": "crash",
                "required": True,
            },
        ],
        "baseline": [
            {"title": "操作前进程列表", "cmd": "pgrep -af \"{process}\" | head -5 || echo '无匹配进程'", "scope": "local"},
        ],
        "action": "vm_process",
        "action_params": {},
        "verify": [
            {"title": "操作后进程列表", "cmd": "pgrep -af \"{process}\" | head -5 || echo '无匹配进程'", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_vm_network",
        "title": "VM 网络故障测试",
        "desc": "注入 VM 侧网络故障前后对比连通性。",
        "group": "vm",
        "params": [
            {
                "name": "net_type",
                "label": "故障类型",
                "type": "select",
                "options": [
                    {"value": "delay", "label": "延迟"},
                    {"value": "loss", "label": "丢包"},
                    {"value": "corrupt", "label": "报文损坏"},
                ],
                "default": "delay",
                "required": True,
            },
            {"name": "net_param", "label": "参数", "type": "text", "required": True, "placeholder": "100ms / 10%"},
        ],
        "baseline": [
            {"title": "注入前 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
        ],
        "action": "vm_network",
        "action_params": {},
        "verify": [
            {"title": "注入后 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
        ],
        "cleanup": "vm_network",
        "cleanup_params": {"net_type": "clear"},
    },
    {
        "key": "test_vm_cpu",
        "title": "VM CPU 压力测试",
        "desc": "施加 CPU 压力前后，对比 Ubuntu 宿主机 CPU 使用率。",
        "group": "vm",
        "params": [
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 10, "required": True},
        ],
        "baseline": [
            {"title": "压力前 CPU 前 5", "cmd": "ps -eo pid,pcpu,comm --sort=-pcpu | head -5", "scope": "local"},
            {"title": "压力前负载", "cmd": "uptime", "scope": "local"},
        ],
        "action": "vm_cpu",
        "action_params": {"pid": 0},
        "verify": [
            {"title": "压力后 CPU 前 5", "cmd": "ps -eo pid,pcpu,comm --sort=-pcpu | head -5", "scope": "local"},
            {"title": "压力后负载", "cmd": "uptime", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_vm_mem_leak",
        "title": "VM 内存泄漏测试",
        "desc": "执行内存泄漏前后，对比宿主机 /proc/meminfo。",
        "group": "vm",
        "params": [
            {"name": "size_mb", "label": "占用内存 (MB)", "type": "number", "default": 128, "required": True},
        ],
        "baseline": [
            {"title": "泄漏前内存", "cmd": "cat /proc/meminfo | head -5", "scope": "local"},
        ],
        "action": "vm_mem_leak",
        "action_params": {},
        "verify": [
            {"title": "泄漏后内存", "cmd": "cat /proc/meminfo | head -5", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_vm_mem_inject",
        "title": "VM 内存注入测试",
        "desc": "对目标进程内存注入位翻转等故障，检查进程状态。",
        "group": "vm",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {
                "name": "mem_region", "label": "区域", "type": "select",
                "options": [{"value": "heap", "label": "Heap"}, {"value": "stack", "label": "Stack"}],
                "default": "heap", "required": True,
            },
            {
                "name": "mem_type", "label": "故障类型", "type": "select",
                "options": [
                    {"value": "flip", "label": "位翻转"},
                    {"value": "set0", "label": "set0"},
                    {"value": "set1", "label": "set1"},
                    {"value": "byte", "label": "随机字节"},
                ],
                "default": "flip", "required": True,
            },
            {"name": "mem_bit", "label": "目标位 (0-63)", "type": "number", "default": 0, "required": True},
        ],
        "baseline": [
            {"title": "注入前进程状态", "cmd": "ps -o pid,stat,comm -p {pid} || echo '进程不存在'", "scope": "local"},
            {"title": "注入前内存映射", "cmd": "cat /proc/{pid}/maps 2>/dev/null | head -10 || echo 'N/A'", "scope": "local"},
        ],
        "action": "vm_mem_inject",
        "action_params": {},
        "verify": [
            {"title": "注入后进程状态", "cmd": "ps -o pid,stat,comm -p {pid} || echo '进程已退出'", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_vm_reg_inject",
        "title": "VM 寄存器注入测试",
        "desc": "对目标进程寄存器注入故障，检查进程状态。",
        "group": "vm",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {"name": "reg", "label": "寄存器", "type": "text", "required": True, "placeholder": "X0 / SP / PC"},
            {
                "name": "reg_type", "label": "故障类型", "type": "select",
                "options": [
                    {"value": "flip1", "label": "flip1"},
                    {"value": "zero1", "label": "zero1"},
                    {"value": "set1", "label": "set1"},
                ],
                "default": "flip1", "required": True,
            },
            {"name": "reg_bit", "label": "目标位 (-1 随机)", "type": "number", "default": -1, "required": True},
        ],
        "baseline": [
            {"title": "注入前进程状态", "cmd": "ps -o pid,stat,comm -p {pid} || echo '进程不存在'", "scope": "local"},
        ],
        "action": "vm_reg_inject",
        "action_params": {},
        "verify": [
            {"title": "注入后进程状态", "cmd": "ps -o pid,stat,comm -p {pid} || echo '进程已退出'", "scope": "local"},
        ],
        "cleanup": None,
    },

    # =================================================================
    #  KVM 注入  (Ubuntu 宿主机 — 本地执行)
    # =================================================================
    {
        "key": "test_kvm_list",
        "title": "KVM 虚拟机列表",
        "desc": "列出当前运行的 KVM/QEMU 虚拟机进程。",
        "group": "kvm",
        "params": [],
        "baseline": [],
        "action": "kvm_list",
        "action_params": {},
        "verify": [],
        "cleanup": None,
    },
    {
        "key": "test_kvm_soft",
        "title": "KVM 软错误注入测试",
        "desc": "对虚拟机寄存器注入软错误（位翻转/交换/置零），检查进程状态。",
        "group": "kvm",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {"name": "reg", "label": "寄存器", "type": "text", "required": True, "placeholder": "PC / SP / X0"},
            {
                "name": "soft_type", "label": "故障类型", "type": "select",
                "options": [
                    {"value": "flip", "label": "位翻转"},
                    {"value": "swap", "label": "位交换"},
                    {"value": "zero", "label": "置零"},
                ],
                "default": "flip", "required": True,
            },
        ],
        "baseline": [
            {"title": "注入前进程状态", "cmd": "ps -o pid,stat,comm -p {pid} || echo '进程不存在'", "scope": "local"},
        ],
        "action": "kvm_soft",
        "action_params": {},
        "verify": [
            {"title": "注入后进程状态", "cmd": "ps -o pid,stat,comm -p {pid} || echo '进程已退出'", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_kvm_guest",
        "title": "KVM 客户OS错误行为测试",
        "desc": "模拟客户机异常行为，检查 QEMU 进程状态。",
        "group": "kvm",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {
                "name": "guest_type", "label": "类型", "type": "select",
                "options": [
                    {"value": "data", "label": "数据段异常"},
                    {"value": "divzero", "label": "除零异常"},
                    {"value": "invalid", "label": "非法指令"},
                ],
                "default": "data", "required": True,
            },
        ],
        "baseline": [
            {"title": "注入前 QEMU 进程", "cmd": "ps -ef | grep -E 'qemu-system|qemu-kvm' | grep -v grep | head -5 || echo 'qemu not running'", "scope": "local"},
        ],
        "action": "kvm_guest",
        "action_params": {},
        "verify": [
            {"title": "注入后 QEMU 进程", "cmd": "ps -ef | grep -E 'qemu-system|qemu-kvm' | grep -v grep | head -5 || echo 'qemu not running'", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_kvm_perf_delay",
        "title": "KVM 性能延迟测试",
        "desc": "为虚拟机注入执行延迟前后对比。",
        "group": "kvm",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {"name": "ms", "label": "延迟 (ms)", "type": "number", "default": 100, "required": True},
        ],
        "baseline": [
            {"title": "延迟前进程状态", "cmd": "ps -o pid,stat,pcpu,comm -p {pid} || echo '进程不存在'", "scope": "local"},
        ],
        "action": "kvm_perf_delay",
        "action_params": {},
        "verify": [
            {"title": "延迟后进程状态", "cmd": "ps -o pid,stat,pcpu,comm -p {pid} || echo '进程不存在'", "scope": "local"},
        ],
        "cleanup": "kvm_perf_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_kvm_perf_stress",
        "title": "KVM CPU 压力测试",
        "desc": "对虚拟机注入 CPU 压力前后对比。",
        "group": "kvm",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 10, "required": True},
        ],
        "baseline": [
            {"title": "压力前 CPU 使用率", "cmd": "ps -o pid,pcpu,comm --sort=-pcpu | head -5", "scope": "local"},
        ],
        "action": "kvm_perf_stress",
        "action_params": {},
        "verify": [
            {"title": "压力后 CPU 使用率", "cmd": "ps -o pid,pcpu,comm --sort=-pcpu | head -5", "scope": "local"},
        ],
        "cleanup": "kvm_perf_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_kvm_cpu_hotplug",
        "title": "KVM CPU 热插拔测试",
        "desc": "下线 CPU 核心前后对比在线 CPU 列表。",
        "group": "kvm",
        "params": [
            {"name": "cpu_id", "label": "CPU 号", "type": "number", "default": 1, "required": True},
        ],
        "baseline": [
            {"title": "下线前 CPU 列表", "cmd": "lscpu | head -8 || cat /proc/cpuinfo | grep processor", "scope": "local"},
            {"title": "下线前 CPU 在线状态", "cmd": "cat /sys/devices/system/cpu/online 2>/dev/null || echo 'N/A'", "scope": "local"},
        ],
        "action": "kvm_cpu_hotplug",
        "action_params": {"cpu_state": "offline"},
        "verify": [
            {"title": "下线后 CPU 列表", "cmd": "lscpu | head -8 || cat /proc/cpuinfo | grep processor", "scope": "local"},
            {"title": "下线后 CPU 在线状态", "cmd": "cat /sys/devices/system/cpu/online 2>/dev/null || echo 'N/A'", "scope": "local"},
        ],
        "cleanup": "kvm_cpu_hotplug",
        "cleanup_params": {"cpu_state": "online"},
    },
]

# Quick lookup by key
FUNC_TESTS_MAP: Dict[str, Dict[str, Any]] = {t["key"]: t for t in FUNC_TESTS}
