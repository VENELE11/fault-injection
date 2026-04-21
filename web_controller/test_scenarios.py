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

HDFS_CMD = (
    ". /etc/profile >/dev/null 2>&1 || true; "
    "if [ -z \"${JAVA_HOME:-}\" ] || [ ! -d \"${JAVA_HOME:-/nonexistent}\" ]; then "
    "if command -v java >/dev/null 2>&1; then _fi_java_bin=$(readlink -f \"$(command -v java)\" 2>/dev/null || command -v java); _fi_java_home=$(dirname \"$(dirname \"$_fi_java_bin\")\"); "
    "if [ -x \"$_fi_java_home/bin/java\" ]; then export JAVA_HOME=\"$_fi_java_home\"; fi; fi; fi; "
    "if [ -z \"${JAVA_HOME:-}\" ] || [ ! -x \"${JAVA_HOME:-/nonexistent}/bin/java\" ]; then "
    "for _fi_java in /usr/lib/jvm/default-jvm /usr/lib/jvm/default-java /usr/lib/jvm/java-21-openjdk /usr/lib/jvm/java-21-openjdk-* /usr/lib/jvm/java-17-openjdk /usr/lib/jvm/java-17-openjdk-* /usr/lib/jvm/java-11-openjdk /usr/lib/jvm/java-11-openjdk-* /usr/lib/jvm/java-1.*-openjdk* /usr/lib/jvm/*; do "
    "if [ -x \"$_fi_java/bin/java\" ]; then export JAVA_HOME=\"$_fi_java\"; break; fi; done; fi; "
    "HDFS='{hadoop_bin}/hdfs'; "
    "if [ ! -x \"$HDFS\" ]; then HDFS=$(command -v hdfs 2>/dev/null || true); fi; "
    "if [ -z \"$HDFS\" ] || [ ! -x \"$HDFS\" ]; then "
    "echo 'hdfs_not_found: 请检查 hadoop.home 配置'; exit 127; fi; "
)

YARN_CMD = (
    ". /etc/profile >/dev/null 2>&1 || true; "
    "if [ -z \"${JAVA_HOME:-}\" ] || [ ! -d \"${JAVA_HOME:-/nonexistent}\" ]; then "
    "if command -v java >/dev/null 2>&1; then _fi_java_bin=$(readlink -f \"$(command -v java)\" 2>/dev/null || command -v java); _fi_java_home=$(dirname \"$(dirname \"$_fi_java_bin\")\"); "
    "if [ -x \"$_fi_java_home/bin/java\" ]; then export JAVA_HOME=\"$_fi_java_home\"; fi; fi; fi; "
    "if [ -z \"${JAVA_HOME:-}\" ] || [ ! -x \"${JAVA_HOME:-/nonexistent}/bin/java\" ]; then "
    "for _fi_java in /usr/lib/jvm/default-jvm /usr/lib/jvm/default-java /usr/lib/jvm/java-21-openjdk /usr/lib/jvm/java-21-openjdk-* /usr/lib/jvm/java-17-openjdk /usr/lib/jvm/java-17-openjdk-* /usr/lib/jvm/java-11-openjdk /usr/lib/jvm/java-11-openjdk-* /usr/lib/jvm/java-1.*-openjdk* /usr/lib/jvm/*; do "
    "if [ -x \"$_fi_java/bin/java\" ]; then export JAVA_HOME=\"$_fi_java\"; break; fi; done; fi; "
    "YARN='{hadoop_bin}/yarn'; "
    "if [ ! -x \"$YARN\" ]; then YARN=$(command -v yarn 2>/dev/null || true); fi; "
    "if [ -z \"$YARN\" ] || [ ! -x \"$YARN\" ]; then "
    "echo 'yarn_not_found: 请检查 hadoop.home 配置'; exit 127; fi; "
)

CLOUDSTACK_STATUS_CMD = (
    "for name in cloudstack-management cloudstack-agent cloudstack-usage mysqld; do "
    "pidfile=/tmp/${name}.pid; log=/tmp/${name}.log; "
    "marker=\"fi_cloudstack_sim:${name}\"; "
    "pid=''; "
    "if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then "
    "pid=$(cat \"$pidfile\"); "
    "else "
    "pid=$(pgrep -f \"$marker\" 2>/dev/null | head -n 1 || true); "
    "if [ -n \"$pid\" ]; then echo \"$pid\" >\"$pidfile\"; "
    "else nohup /bin/sh -c 'while :; do sleep 3600; done' \"$marker\" >\"$log\" 2>&1 & echo $! >\"$pidfile\"; sleep 0.2; fi; "
    "fi; "
    "done; "
    "echo 'CloudStack component status'; "
    "for name in cloudstack-management cloudstack-agent cloudstack-usage mysqld; do "
    "pidfile=/tmp/${name}.pid; "
    "if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then "
    "pid=$(cat \"$pidfile\"); stat=$(ps -o stat= -p \"$pid\" 2>/dev/null | tr -d ' '); echo \"$name PID=$pid STATE=${stat:-unknown}\"; "
    "else echo \"$name STOPPED\"; fi; "
    "done; "
    "if [ -s /tmp/cloudstack_api_delay_ms ]; then echo \"cloudstack-api-delay $(cat /tmp/cloudstack_api_delay_ms)ms\"; fi; "
    "if [ -s /tmp/cloudstack_network_isolate ]; then echo \"cloudstack-network-isolate $(cat /tmp/cloudstack_network_isolate)\"; fi"
)

# ---------------------------------------------------------------------------
# Test scenario structure
# ---------------------------------------------------------------------------
# {
#     "key": unique id,
#     "title": display name,
#     "desc": description,
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
            {"title": "重启前进程 (jps)", "cmd": "jps 2>/dev/null || true", "scope": "all"},
        ],
        "action": "hadoop_restart",
        "action_params": {},
        "verify": [
            {"title": "重启后进程 (jps)", "cmd": "jps 2>/dev/null || true", "scope": "all"},
        ],
        "cleanup": None,
    },

    # =================================================================
    #  Hadoop 进程故障  (VM 内 — 通过 SSH)
    # =================================================================
    {
        "key": "test_process_crash",
        "title": "进程崩溃测试",
        "desc": "崩溃指定组件进程，对比崩溃前后 jps 输出，并支持一键恢复。",
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
            {"title": "崩溃前进程列表", "cmd": "jps 2>/dev/null || true", "scope": "all"},
        ],
        "action": "process_fault",
        "action_params": {"op": "crash"},
        "verify": [
            {"title": "崩溃后进程列表", "cmd": "jps 2>/dev/null || true", "scope": "all"},
        ],
        "cleanup": "process_restart",
        "cleanup_params": {},
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
            {"title": "挂起前进程列表", "cmd": "jps 2>/dev/null || true", "scope": "all"},
        ],
        "action": "process_fault",
        "action_params": {"op": "hang"},
        "verify": [
            {"title": "挂起后进程列表", "cmd": "jps 2>/dev/null || true", "scope": "all"},
        ],
        "cleanup": "process_fault",
        "cleanup_params_override": {"op": "resume"},
    },

    # =================================================================
    #  网络故障  (本地虚拟机注入 — Ubuntu 宿主机本地执行)
    # =================================================================
    {
        "key": "test_delay",
        "title": "网络延迟注入测试",
        "desc": "在本地虚拟机网卡注入延迟，对比注入前后连通性与 tc 规则。",
        "group": "network",
        "params": [
            {
                "name": "net_param",
                "label": "延迟参数",
                "type": "text",
                "default": "200ms",
                "required": True,
                "placeholder": "200ms",
            },
        ],
        "baseline": [
            {"title": "注入前 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "注入前 tc 规则", "cmd": "tc qdisc show 2>/dev/null || echo 'no tc rules'", "scope": "local"},
        ],
        "action": "vm_network",
        "action_params": {"net_type": "delay", "net_param": "200ms"},
        "verify": [
            {"title": "注入后 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "注入后 tc 规则", "cmd": "tc qdisc show 2>/dev/null || echo 'no tc rules'", "scope": "local"},
        ],
        "cleanup": "vm_network",
        "cleanup_params": {"net_type": "clear"},
    },
    {
        "key": "test_loss",
        "title": "网络丢包注入测试",
        "desc": "在本地虚拟机网卡注入丢包，对比注入前后连通性与 tc 规则。",
        "group": "network",
        "params": [
            {
                "name": "net_param",
                "label": "丢包参数",
                "type": "text",
                "default": "30%",
                "required": True,
                "placeholder": "30%",
            },
        ],
        "baseline": [
            {"title": "注入前 ping 测试", "cmd": "ping -c 10 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "注入前 tc 规则", "cmd": "tc qdisc show 2>/dev/null || echo 'no tc rules'", "scope": "local"},
        ],
        "action": "vm_network",
        "action_params": {"net_type": "loss", "net_param": "30%"},
        "verify": [
            {"title": "注入后 ping 测试", "cmd": "ping -c 10 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "注入后 tc 规则", "cmd": "tc qdisc show 2>/dev/null || echo 'no tc rules'", "scope": "local"},
        ],
        "cleanup": "vm_network",
        "cleanup_params": {"net_type": "clear"},
    },
    {
        "key": "test_reorder",
        "title": "网络报文损坏注入测试",
        "desc": "在本地虚拟机网卡注入报文损坏，观察注入前后连通性变化。",
        "group": "network",
        "params": [
            {
                "name": "net_param",
                "label": "损坏参数",
                "type": "text",
                "default": "20%",
                "required": True,
                "placeholder": "20%",
            },
        ],
        "baseline": [
            {"title": "注入前 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "注入前 tc 规则", "cmd": "tc qdisc show 2>/dev/null || echo 'no tc rules'", "scope": "local"},
        ],
        "action": "vm_network",
        "action_params": {"net_type": "corrupt", "net_param": "20%"},
        "verify": [
            {"title": "注入后 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "注入后 tc 规则", "cmd": "tc qdisc show 2>/dev/null || echo 'no tc rules'", "scope": "local"},
        ],
        "cleanup": "vm_network",
        "cleanup_params": {"net_type": "clear"},
    },
    {
        "key": "test_isolate",
        "title": "网络隔离测试",
        "desc": "在本地虚拟机注入端口隔离，检查 TCP 端口连通性与隔离规则变化。",
        "group": "network",
        "params": [
            {
                "name": "net_param",
                "label": "隔离端口",
                "type": "text",
                "default": "8080",
                "required": True,
                "placeholder": "8080",
            },
        ],
        "baseline": [
            {"title": "隔离前 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "隔离前 TCP 端口连通性", "cmd": "python -c \"import socket,threading,sys; p=int(sys.argv[1]); srv=socket.socket(); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); srv.bind(('0.0.0.0',p)); srv.listen(1); threading.Thread(target=lambda: srv.accept()[0].close(),daemon=True).start(); u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); u.connect(('8.8.8.8',80)); ip=u.getsockname()[0]; u.close(); c=socket.socket(); c.settimeout(2); ok=True;\ntry:\n c.connect((ip,p));\nexcept Exception:\n ok=False;\nprint(f'TCP_CONNECT_{\'OK\' if ok else \'FAIL\'} {ip}:{p}'); c.close(); srv.close()\" {net_param}", "scope": "local"},
            {"title": "隔离前 OUTPUT 规则", "cmd": "(sudo -n iptables -S OUTPUT 2>/dev/null || iptables -S OUTPUT 2>/dev/null || true) | grep -- '--dport' || echo 'no partition rules'", "scope": "local"},
        ],
        "action": "vm_network",
        "action_params": {"net_type": "partition", "net_param": "8080"},
        "verify": [
            {"title": "隔离后 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "隔离后 TCP 端口连通性", "cmd": "python -c \"import socket,threading,sys; p=int(sys.argv[1]); srv=socket.socket(); srv.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1); srv.bind(('0.0.0.0',p)); srv.listen(1); threading.Thread(target=lambda: srv.accept()[0].close(),daemon=True).start(); u=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); u.connect(('8.8.8.8',80)); ip=u.getsockname()[0]; u.close(); c=socket.socket(); c.settimeout(2); ok=True;\ntry:\n c.connect((ip,p));\nexcept Exception:\n ok=False;\nprint(f'TCP_CONNECT_{\'OK\' if ok else \'FAIL\'} {ip}:{p}'); c.close(); srv.close()\" {net_param}", "scope": "local"},
            {"title": "隔离后 OUTPUT 规则", "cmd": "(sudo -n iptables -S OUTPUT 2>/dev/null || iptables -S OUTPUT 2>/dev/null || true) | grep -- '--dport' || echo 'no partition rules'", "scope": "local"},
        ],
        "cleanup": "vm_network",
        "cleanup_params": {"net_type": "clear"},
    },
    # =================================================================
    #  资源故障  (可观测版 / 虚拟机本机)
    # =================================================================
    {
        "key": "test_cpu_stress",
        "title": "CPU 压力测试",
        "desc": "使用 vm_injection/cpu_injector 在本机执行 CPU 压力注入，展示负载变化与动作日志。",
        "group": "resource",
        "params": [
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 12, "required": True},
            {"name": "threads", "label": "线程数 (可选)", "type": "number", "required": False},
        ],
        "baseline": [
            {"title": "注入前 loadavg", "cmd": "cat /proc/loadavg", "scope": "local"},
            {"title": "注入前 CPU 占用前 5", "cmd": "ps -eo pid,pcpu,comm --sort=-pcpu | head -5", "scope": "local"},
        ],
        "action": "vm_cpu",
        "action_params": {"pid": 0, "duration": 12, "cpu_mode": "2"},
        "verify": [
            {"title": "注入后 loadavg", "cmd": "cat /proc/loadavg", "scope": "local"},
            {"title": "注入后 CPU 占用前 5", "cmd": "ps -eo pid,pcpu,comm --sort=-pcpu | head -5", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_mem_stress",
        "title": "内存压力测试",
        "desc": "使用 vm_injection/mem_leak 在本机注入内存压力，展示注入前后内存变化。",
        "group": "resource",
        "params": [
            {"name": "size_mb", "label": "内存 (MB)", "type": "number", "default": 512, "required": True},
        ],
        "baseline": [
            {"title": "注入前内存概览", "cmd": "free -m | head -2", "scope": "local"},
            {"title": "注入前 mem_leak 进程", "cmd": "pgrep -af 'mem_leak' | head -n 3 || echo 'mem_leak_not_running'", "scope": "local"},
        ],
        "action": "vm_mem_leak",
        "action_params": {"size_mb": 512},
        "verify": [
            {"title": "注入后内存概览", "cmd": "free -m | head -2", "scope": "local"},
            {"title": "注入后 mem_leak 进程", "cmd": "pgrep -af 'mem_leak' | head -n 5 || echo 'mem_leak_not_running'", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_disk_fill",
        "title": "磁盘填充测试",
        "desc": "使用 hadoop_injector 对 slave1 注入磁盘填充，展示填充前后变化。",
        "group": "resource",
        "params": [
            {"name": "size_mb", "label": "填充大小 (MB)", "type": "number", "default": 512, "required": True},
        ],
        "baseline": [
            {"title": "填充前 slave1 根分区", "cmd": "ssh slave1 'df -h / | head -5' 2>&1 || true", "scope": "master"},
            {"title": "填充前磁盘文件", "cmd": "ssh slave1 '(ls -lh /tmp/disk_hog 2>/dev/null || echo disk_hog_absent)' 2>&1 || true", "scope": "master"},
        ],
        "action": "disk_fill",
        "action_timeout": 180,
        "action_params": {"target": "slave1", "size_mb": 512},
        "verify": [
            {"title": "填充后 slave1 根分区", "cmd": "ssh slave1 'df -h / | head -5' 2>&1 || true", "scope": "master"},
            {"title": "填充后磁盘文件", "cmd": "ssh slave1 '(ls -lh /tmp/disk_hog 2>/dev/null || echo disk_hog_absent)' 2>&1 || true", "scope": "master"},
            {"title": "填充后磁盘文件大小(MB)", "cmd": "ssh slave1 '(du -m /tmp/disk_hog 2>/dev/null || true)' 2>&1 || true", "scope": "master"},
        ],
        "cleanup": "disk_fill_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_io_slow",
        "title": "cgroup I/O 限速测试",
        "desc": "使用 hadoop_injector 对 slave1 开启/关闭 I/O 限速，展示 cgroup 状态变化。",
        "group": "resource",
        "params": [],
        "baseline": [
            {"title": "限速前 cgroup 状态", "cmd": "ssh slave1 '(test -d /sys/fs/cgroup/io_limited && cat /sys/fs/cgroup/io_limited/io.max 2>/dev/null) || echo io_limit_off' 2>&1 || true", "scope": "master"},
            {"title": "限速前写入输出", "cmd": "ssh slave1 '(dd if=/dev/zero of=/root/iotest bs=1M count=20 oflag=direct 2>&1 || dd if=/dev/zero of=/root/iotest bs=1M count=20 2>&1); sync; rm -f /root/iotest' 2>&1 || true", "scope": "master", "timeout": 45},
        ],
        "action": "io_slow",
        "action_params": {"target": "slave1", "state": "on"},
        "verify": [
            {"title": "限速后 cgroup 状态", "cmd": "ssh slave1 '(test -d /sys/fs/cgroup/io_limited && cat /sys/fs/cgroup/io_limited/io.max 2>/dev/null) || echo io_limit_off' 2>&1 || true", "scope": "master"},
            {"title": "限速后写入输出", "cmd": "ssh slave1 'if [ -d /sys/fs/cgroup/io_limited ]; then echo $$ > /sys/fs/cgroup/io_limited/cgroup.procs 2>/dev/null || true; fi; (dd if=/dev/zero of=/root/iotest bs=1M count=20 oflag=direct 2>&1 || dd if=/dev/zero of=/root/iotest bs=1M count=20 2>&1); sync; rm -f /root/iotest' 2>&1 || true", "scope": "master", "timeout": 45},
        ],
        "cleanup": "io_slow",
        "cleanup_params": {"target": "slave1", "state": "off"},
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
            {"title": "安全模式前 HDFS 状态", "cmd": HDFS_CMD + "\"$HDFS\" dfsadmin -safemode get 2>&1", "scope": "master"},
            {"title": "安全模式前目录列表", "cmd": HDFS_CMD + "\"$HDFS\" dfs -ls / 2>&1 | head -5", "scope": "master"},
        ],
        "action": "hdfs_safe",
        "action_params": {"mode": "enter"},
        "verify": [
            {"title": "进入安全模式后 HDFS 状态", "cmd": HDFS_CMD + "\"$HDFS\" dfsadmin -safemode get 2>&1", "scope": "master"},
            {
                "title": "安全模式下写入测试",
                "cmd": HDFS_CMD + "echo test | \"$HDFS\" dfs -put - /tmp/__safemode_test 2>&1; rc=$?; if [ \"$rc\" -eq 0 ]; then echo 'WRITE_UNEXPECTED_SUCCESS'; \"$HDFS\" dfs -rm -f /tmp/__safemode_test >/dev/null 2>&1 || true; exit 1; else echo \"WRITE_BLOCKED_EXPECTED rc=$rc\"; fi",
                "scope": "master",
            },
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
            {"name": "target", "label": "目标节点", "type": "node", "default": "slave1", "required": True},
            {"name": "size_mb", "label": "大小 (MB)", "type": "number", "default": 256, "required": True},
        ],
        "baseline": [
            {"title": "填充前目标磁盘", "cmd": "df -h / | head -5; ls -lh /root/fi_disk_hog /tmp/disk_hog 2>/dev/null || echo disk_hog_absent", "scope": "target"},
            {"title": "填充前 HDFS 报告", "cmd": HDFS_CMD + "\"$HDFS\" dfsadmin -report 2>&1 | head -15 || true", "scope": "master"},
        ],
        "action": "hdfs_disk",
        "action_params": {},
        "verify": [
            {"title": "填充后目标磁盘", "cmd": "df -h / | head -5; ls -lh /root/fi_disk_hog /tmp/disk_hog 2>/dev/null || echo disk_hog_absent; du -h /root/fi_disk_hog /tmp/disk_hog 2>/dev/null || true", "scope": "target"},
            {"title": "填充后 HDFS 报告", "cmd": HDFS_CMD + "\"$HDFS\" dfsadmin -report 2>&1 | head -15 || true", "scope": "master"},
        ],
        "cleanup": "hdfs_disk_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_yarn_unhealthy",
        "title": "YARN 节点不健康测试",
        "desc": "标记节点不健康前后，对比 YARN 节点列表。",
        "group": "hdfs",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "default": "slave1", "required": True},
        ],
        "baseline": [
            {"title": "标记前目标 NodeManager", "cmd": "jps 2>/dev/null | grep NodeManager || pgrep -af 'org.apache.hadoop.yarn.server.nodemanager.NodeManager' || echo NodeManager_not_running", "scope": "target"},
            {"title": "标记前 YARN 节点", "cmd": YARN_CMD + "\"$YARN\" node -list -all 2>&1 | head -20 || true", "scope": "master"},
        ],
        "action": "yarn_unhealthy",
        "action_params": {"state": "on"},
        "verify": [
            {"title": "标记后目标 NodeManager", "cmd": "jps 2>/dev/null | grep NodeManager || pgrep -af 'org.apache.hadoop.yarn.server.nodemanager.NodeManager' || echo NodeManager_stopped_unhealthy_simulated", "scope": "target"},
            {"title": "标记后 YARN 节点", "cmd": YARN_CMD + "\"$YARN\" node -list -all 2>&1 | head -20 || true", "scope": "master"},
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
        "desc": "自动提交后台 wordcount MapReduce 作业，再杀死 Map/Reduce 任务进程并对比状态。",
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
            {"name": "target", "label": "目标节点", "type": "node", "default": "slave1", "required": True},
        ],
        "baseline": [
            {"title": "故障前 YARN 应用", "cmd": YARN_CMD + "\"$YARN\" application -list 2>&1 | head -20 || true", "scope": "master"},
            {"title": "故障前进程列表", "cmd": "ps -eo pid,comm,args | grep -E 'NodeManager|YarnChild|MRAppMaster|DataNode|ResourceManager|NameNode' | grep -v grep | head -20 || true", "scope": "all", "timeout": 8},
        ],
        "action": "mapreduce_fault",
        "action_params": {},
        "verify": [
            {"title": "故障后 YARN 应用", "cmd": YARN_CMD + "\"$YARN\" application -list -appStates ALL 2>&1 | head -30 || true", "scope": "master"},
            {"title": "后台任务日志", "cmd": "tail -120 /tmp/fi_mapreduce_job.log 2>/dev/null || echo mapreduce_job_log_missing", "scope": "master"},
            {"title": "故障后进程列表", "cmd": "ps -eo pid,comm,args | grep -E 'YarnChild|MRAppMaster|NodeManager|DataNode|ResourceManager|NameNode' | grep -v grep | head -20 || true", "scope": "all", "timeout": 8},
        ],
        "cleanup": None,
    },

    # =================================================================
    #  CloudStack 注入  (宿主机本地执行)
    # =================================================================
    {
        "key": "test_cloudstack_process_hang_resume",
        "title": "CloudStack 进程挂起/恢复测试",
        "desc": "挂起指定 CloudStack 组件后检查状态，再恢复并复检。",
        "group": "cloudstack",
        "params": [
            {
                "name": "cs_component",
                "label": "组件",
                "type": "select",
                "options": [
                    {"value": "ms", "label": "Management Server"},
                    {"value": "agent", "label": "CloudStack Agent"},
                    {"value": "usage", "label": "Usage Server"},
                    {"value": "mysql", "label": "MySQL"},
                ],
                "default": "agent",
                "required": True,
            },
        ],
        "baseline": [
            {"title": "挂起前服务状态", "cmd": CLOUDSTACK_STATUS_CMD, "scope": "local"},
        ],
        "action": "cloudstack_process",
        "action_params": {"op": "hang"},
        "verify": [
            {"title": "挂起后服务状态", "cmd": CLOUDSTACK_STATUS_CMD, "scope": "local"},
        ],
        "cleanup": "cloudstack_process",
        "cleanup_params_override": {"op": "resume"},
    },
    {
        "key": "test_cloudstack_api_delay",
        "title": "CloudStack API 延迟注入测试",
        "desc": "注入 API 延迟后检查 tc 规则与接口响应时间。",
        "group": "cloudstack",
        "params": [
            {"name": "ms", "label": "延迟 (ms)", "type": "number", "default": 1000, "required": True},
        ],
        "baseline": [
            {"title": "注入前 API 状态", "cmd": CLOUDSTACK_STATUS_CMD, "scope": "local"},
        ],
        "action": "cloudstack_api_delay",
        "action_params": {},
        "verify": [
            {"title": "注入后 API 状态", "cmd": CLOUDSTACK_STATUS_CMD, "scope": "local"},
        ],
        "cleanup": "cloudstack_api_delay_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_cloudstack_network_isolate",
        "title": "CloudStack 网络隔离测试",
        "desc": "隔离目标节点/IP 前后对比连通性。",
        "group": "cloudstack",
        "params": [
            {"name": "target", "label": "目标节点/IP", "type": "node", "required": True},
        ],
        "baseline": [
            {"title": "隔离前网络状态", "cmd": CLOUDSTACK_STATUS_CMD, "scope": "local"},
        ],
        "action": "cloudstack_network",
        "action_params": {},
        "verify": [
            {"title": "隔离后网络状态", "cmd": CLOUDSTACK_STATUS_CMD, "scope": "local"},
        ],
        "cleanup": "cloudstack_network_clear",
        "cleanup_params": {},
    },

    # =================================================================
    #  VM 注入  (Ubuntu 宿主机 — 本地执行)
    # =================================================================
    {
        "key": "test_vm_process",
        "title": "VM 进程控制测试",
        "desc": "自动创建测试进程后执行崩溃/挂起操作，对比前后状态。",
        "group": "vm",
        "params": [
            {
                "name": "process",
                "label": "进程名",
                "type": "text",
                "default": "fi_vm_target_process",
                "required": False,
                "placeholder": "fi_vm_target_process",
            },
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
            {"title": "操作前自动靶进程", "cmd": "pidfile=/tmp/fi_vm_target_process.pid; if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then ps -o pid,stat,comm,args -p \"$(cat \"$pidfile\")\"; else echo '自动靶进程尚未创建'; fi", "scope": "local"},
        ],
        "action": "vm_process",
        "action_params": {"process": "fi_vm_target_process"},
        "verify": [
            {"title": "操作后自动靶进程", "cmd": "pidfile=/tmp/fi_vm_target_process.pid; if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then ps -o pid,stat,comm,args -p \"$(cat \"$pidfile\")\"; else echo '自动靶进程已退出'; fi", "scope": "local"},
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
            {"name": "net_param", "label": "参数", "type": "text", "default": "200ms", "required": False, "placeholder": "200ms / 20%"},
        ],
        "baseline": [
            {"title": "注入前 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
        ],
        "action": "vm_network",
        "action_params": {"net_type": "delay", "net_param": "200ms"},
        "verify": [
            {"title": "注入后 ping 测试", "cmd": "ping -c 4 -W 2 8.8.8.8 2>&1 || true", "scope": "local"},
            {"title": "注入后 tc 规则", "cmd": "tc qdisc show 2>/dev/null | grep -E 'netem|delay|loss|corrupt' || echo 'no netem rule'", "scope": "local"},
        ],
        "cleanup": "vm_network",
        "cleanup_params": {"net_type": "clear"},
    },
    {
        "key": "test_vm_cpu",
        "title": "VM CPU 压力测试",
        "desc": "后台启动 CPU 压力，并在压力运行期间连续采样。",
        "group": "vm",
        "params": [
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 20, "required": True},
            {"name": "threads", "label": "线程数 (0=全核)", "type": "number", "default": 0, "required": False},
        ],
        "baseline": [
            {"title": "压力前 CPU 前 5", "cmd": "ps -eo pid,pcpu,comm --sort=-pcpu | head -5", "scope": "local"},
            {"title": "压力前负载", "cmd": "cat /proc/loadavg 2>/dev/null || uptime", "scope": "local"},
        ],
        "action": "vm_cpu",
        "action_params": {"pid": 0, "duration": 20, "threads": 0, "cpu_mode": "2"},
        "verify": [
            {"title": "压力中 CPU 连续采样", "cmd": "for i in 1 2 3 4 5; do echo SAMPLE_$i; ps -eo pid,pcpu,comm,args --sort=-pcpu | head -8; sleep 1; done", "scope": "local", "timeout": 12},
            {"title": "压力中负载", "cmd": "cat /proc/loadavg 2>/dev/null || uptime", "scope": "local"},
            {"title": "CPU 压力日志", "cmd": "if [ -s /tmp/fi_vm_cpu_stress.log ]; then tail -80 /tmp/fi_vm_cpu_stress.log; else echo 'cpu_stress_log_missing_or_empty'; fi", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_vm_mem_leak",
        "title": "VM 内存泄漏测试",
        "desc": "启动内存泄漏后在前台连续观测内存增长，再保留后台进程。",
        "group": "vm",
        "params": [
            {"name": "size_mb", "label": "占用内存 (MB)", "type": "number", "default": 512, "required": True},
        ],
        "baseline": [
            {"title": "泄漏前内存", "cmd": "free -m | head -2", "scope": "local"},
        ],
        "action": "vm_mem_leak",
        "action_params": {},
        "verify": [
            {"title": "泄漏后内存", "cmd": "free -m | head -2", "scope": "local"},
            {"title": "内存泄漏进程", "cmd": "pidfile=/tmp/fi_vm_mem_leak.pid; if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then ps -o pid,stat,comm,args -p \"$(cat \"$pidfile\")\"; else echo 'mem_leak_not_running'; fi", "scope": "local"},
            {
                "title": "内存泄漏前台采样",
                "cmd": "pidfile=/tmp/fi_vm_mem_leak.pid; pid=$(cat \"$pidfile\" 2>/dev/null || true); for i in 1 2 3 4 5; do echo SAMPLE_$i; free -m | head -2; tail -10 /tmp/fi_vm_mem_leak.log 2>/dev/null || echo 'mem_leak_log_missing_or_empty'; if [ -n \"$pid\" ] && ! kill -0 \"$pid\" 2>/dev/null; then echo 'mem_leak_not_running'; break; fi; sleep 1; done",
                "scope": "local",
                "timeout": 12,
            },
        ],
        "cleanup": None,
    },
    {
        "key": "test_vm_mem_inject",
        "title": "VM 内存注入测试",
        "desc": "自动创建带特征值的测试进程，并对其内存注入位翻转等故障。",
        "group": "vm",
        "params": [
            {"name": "pid", "label": "目标 PID (0=自动创建)", "type": "number", "default": 0, "required": False},
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
            {"title": "注入前自动靶进程", "cmd": "pidfile=/tmp/fi_vm_target_mem.pid; if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then ps -o pid,stat,comm,args -p \"$(cat \"$pidfile\")\"; else echo '自动靶进程将在动作阶段创建'; fi", "scope": "local"},
        ],
        "action": "vm_mem_inject",
        "action_params": {"pid": 0},
        "verify": [
            {"title": "注入后自动靶进程", "cmd": "pidfile=/tmp/fi_vm_target_mem.pid; if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then ps -o pid,stat,comm,args -p \"$(cat \"$pidfile\")\"; else echo '自动靶进程已退出'; fi", "scope": "local"},
            {"title": "自动靶进程日志", "cmd": "tail -20 /tmp/fi_vm_target_mem.log 2>/dev/null || echo 'target_log_missing'", "scope": "local"},
        ],
        "cleanup": None,
    },
    {
        "key": "test_vm_reg_inject",
        "title": "VM 寄存器注入测试",
        "desc": "自动创建测试进程，并对其寄存器注入故障。",
        "group": "vm",
        "params": [
            {"name": "pid", "label": "目标 PID (0=自动创建)", "type": "number", "default": 0, "required": False},
            {"name": "reg", "label": "寄存器", "type": "text", "default": "X0", "required": True, "placeholder": "X0 / SP / PC"},
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
            {"title": "注入前自动靶进程", "cmd": "pidfile=/tmp/fi_vm_target_reg.pid; if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then ps -o pid,stat,comm,args -p \"$(cat \"$pidfile\")\"; else echo '自动靶进程将在动作阶段创建'; fi", "scope": "local"},
        ],
        "action": "vm_reg_inject",
        "action_params": {"pid": 0, "reg": "X0"},
        "verify": [
            {"title": "注入后自动靶进程", "cmd": "pidfile=/tmp/fi_vm_target_reg.pid; if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then ps -o pid,stat,comm,args -p \"$(cat \"$pidfile\")\"; else echo '自动靶进程已退出'; fi", "scope": "local"},
            {"title": "自动靶进程日志", "cmd": "tail -20 /tmp/fi_vm_target_reg.log 2>/dev/null || echo 'target_log_missing'", "scope": "local"},
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
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
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
            {"title": "注入前目标虚拟机", "cmd": "ps -ef | grep -E 'qemu-system|qemu-kvm' | grep 'alpine_{target}' | grep -v grep | head -3 || echo '虚拟机未运行'", "scope": "local"},
        ],
        "action": "kvm_soft",
        "action_params": {},
        "verify": [
            {
                "title": "注入后目标虚拟机仍在运行",
                "cmd": "ps -ef | grep -E 'qemu-system|qemu-kvm' | grep 'alpine_{target}' | grep -v grep | head -3 || echo '虚拟机未运行'",
                "scope": "local",
            },
        ],
        "cleanup": None,
    },
    {
        "key": "test_kvm_perf_delay",
        "title": "KVM 性能延迟测试",
        "desc": "为虚拟机注入执行延迟，并在 VM 内运行轻量 CPU 哈希 + dd 任务对比前后速度。",
        "group": "kvm",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "ms", "label": "延迟 (ms)", "type": "number", "default": 100, "required": True},
            {"name": "bench_mb", "label": "单轮大小 (MB)", "type": "number", "default": 64, "required": True},
            {"name": "rounds", "label": "轮数", "type": "number", "default": 1, "required": True},
        ],
        "baseline": [
            {"title": "延迟前目标虚拟机", "cmd": "ps -ef | grep -E 'qemu-system|qemu-kvm' | grep 'alpine_{target}' | grep -v grep | head -3 || echo '虚拟机未运行'", "scope": "local"},
            {
                "title": "延迟前任务速度 (VM 内 CPU+dd)",
                "cmd": "target='{target}'; case \"$target\" in master) port=2220;; slave1) port=2221;; slave2) port=2222;; *) echo \"unknown target: $target\"; exit 1;; esac; command -v sshpass >/dev/null || (echo 'sshpass_missing: 请先安装 sshpass'; exit 1); sshpass -p '{kvm_guest_password}' ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR -p \"$port\" {kvm_guest_user}@127.0.0.1 'set -e; rm -f /tmp/fi_kvm_bench; i=1; while [ \"$i\" -le {rounds} ]; do echo ROUND_${{i}}_CPU_HASH_START; dd if=/dev/zero bs=1M count={bench_mb} 2>/tmp/fi_kvm_cpu_dd.log | sha256sum; cat /tmp/fi_kvm_cpu_dd.log; rm -f /tmp/fi_kvm_cpu_dd.log; echo ROUND_${{i}}_DISK_WRITE_START; dd if=/dev/zero of=/tmp/fi_kvm_bench bs=1M count={bench_mb} 2>&1; sync; echo ROUND_${{i}}_DISK_READ_START; dd if=/tmp/fi_kvm_bench of=/dev/null bs=1M 2>&1; i=$((i+1)); done; rm -f /tmp/fi_kvm_bench'",
                "scope": "local",
                "timeout": 240,
            },
        ],
        "action": "kvm_perf_delay",
        "action_params": {},
        "verify": [
            {"title": "延迟后目标虚拟机", "cmd": "ps -ef | grep -E 'qemu-system|qemu-kvm' | grep 'alpine_{target}' | grep -v grep | head -3 || echo '虚拟机未运行'", "scope": "local"},
            {
                "title": "延迟后任务速度 (VM 内 CPU+dd)",
                "cmd": "target='{target}'; case \"$target\" in master) port=2220;; slave1) port=2221;; slave2) port=2222;; *) echo \"unknown target: $target\"; exit 1;; esac; command -v sshpass >/dev/null || (echo 'sshpass_missing: 请先安装 sshpass'; exit 1); sshpass -p '{kvm_guest_password}' ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR -p \"$port\" {kvm_guest_user}@127.0.0.1 'set -e; rm -f /tmp/fi_kvm_bench; i=1; while [ \"$i\" -le {rounds} ]; do echo ROUND_${{i}}_CPU_HASH_START; dd if=/dev/zero bs=1M count={bench_mb} 2>/tmp/fi_kvm_cpu_dd.log | sha256sum; cat /tmp/fi_kvm_cpu_dd.log; rm -f /tmp/fi_kvm_cpu_dd.log; echo ROUND_${{i}}_DISK_WRITE_START; dd if=/dev/zero of=/tmp/fi_kvm_bench bs=1M count={bench_mb} 2>&1; sync; echo ROUND_${{i}}_DISK_READ_START; dd if=/tmp/fi_kvm_bench of=/dev/null bs=1M 2>&1; i=$((i+1)); done; rm -f /tmp/fi_kvm_bench'",
                "scope": "local",
                "timeout": 240,
            },
        ],
        "cleanup": "kvm_perf_clear",
        "cleanup_params": {},
    },
    {
        "key": "test_kvm_perf_stress",
        "title": "KVM CPU 压力测试",
        "desc": "后台启动 KVM CPU 压力，并在压力运行期间连续采样宿主机 CPU。",
        "group": "kvm",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 20, "required": True},
            {"name": "threads", "label": "线程数 (0=全核)", "type": "number", "default": 0, "required": False},
        ],
        "baseline": [
            {"title": "压力前 CPU 使用率", "cmd": "ps -o pid,pcpu,comm --sort=-pcpu | head -5", "scope": "local"},
            {"title": "压力前 loadavg", "cmd": "cat /proc/loadavg 2>/dev/null || uptime", "scope": "local"},
        ],
        "action": "kvm_perf_stress",
        "action_params": {},
        "verify": [
            {
                "title": "压力进程检查",
                "cmd": "pgrep -af 'cpu_injector|kvm_injector.*perf-stress' || echo 'no_stress_process'",
                "scope": "local",
            },
            {
                "title": "压力中 CPU 连续采样",
                "cmd": "for i in 1 2 3 4 5; do echo SAMPLE_$i; ps -o pid,pcpu,comm --sort=-pcpu | head -8; sleep 1; done",
                "scope": "local",
                "timeout": 12,
            },
            {
                "title": "压力中 loadavg",
                "cmd": "cat /proc/loadavg 2>/dev/null || uptime",
                "scope": "local",
            },
            {
                "title": "压力注入日志",
                "cmd": "if [ -s /tmp/fi_kvm_perf_stress_{target}.log ]; then tail -80 /tmp/fi_kvm_perf_stress_{target}.log; else echo 'stress_log_missing_or_empty'; fi",
                "scope": "local",
            },
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
    {
        "key": "test_kvm_recover",
        "title": "KVM 虚拟机恢复/重启测试",
        "desc": "停止目标节点残留 QEMU 进程，并通过 run_cluster.sh 重新启动虚拟机。",
        "group": "kvm",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
        ],
        "baseline": [
            {
                "title": "恢复前目标虚拟机",
                "cmd": "ps -ef | grep -E 'qemu-system|qemu-kvm' | grep 'alpine_{target}' | grep -v grep | head -3 || echo '虚拟机未运行'",
                "scope": "local",
            },
        ],
        "action": "kvm_recover",
        "action_params": {},
        "verify": [
            {
                "title": "恢复后目标虚拟机",
                "cmd": "ps -ef | grep -E 'qemu-system|qemu-kvm' | grep 'alpine_{target}' | grep -v grep | head -3 || echo '虚拟机未运行'",
                "scope": "local",
            },
        ],
        "cleanup": None,
    },
]

# Quick lookup by key
FUNC_TESTS_MAP: Dict[str, Dict[str, Any]] = {t["key"]: t for t in FUNC_TESTS}
