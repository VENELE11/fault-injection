from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Any, Dict, List

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from web_controller.db import connect, init_db, record_run


def result(
    *,
    node: str,
    host: str,
    cmd: str,
    stdout: str,
    stderr: str = "",
    exit_code: int = 0,
    elapsed: float = 0.12,
) -> Dict[str, Any]:
    ok = exit_code == 0
    return {
        "ok": ok,
        "node": node,
        "host": host,
        "cmd": cmd,
        "stdout": stdout,
        "stderr": stderr,
        "exit_code": exit_code,
        "elapsed": elapsed,
        "truncated": False,
        "stdout_meta": {
            "text": stdout,
            "truncated": False,
            "total_chars": len(stdout),
            "total_lines": len(stdout.splitlines()),
        },
        "stderr_meta": {
            "text": stderr,
            "truncated": False,
            "total_chars": len(stderr),
            "total_lines": len(stderr.splitlines()),
        },
    }


def reset_history() -> None:
    init_db()
    with connect() as conn:
        conn.execute("DELETE FROM fault_results")
        conn.execute("DELETE FROM fault_runs")
        conn.execute("DELETE FROM sqlite_sequence WHERE name IN ('fault_results', 'fault_runs')")


def seed() -> List[int]:
    now = time.time()
    ids: List[int] = []

    ids.append(
        record_run(
            run_type="functest",
            action_key="process_fault",
            scenario_key="test_process_hang_resume",
            title="进程挂起/恢复测试",
            params={"component": "dn", "op": "hang"},
            ok=True,
            started_at=now - 7200,
            finished_at=now - 7196,
            phases=[
                {
                    "phase": "baseline",
                    "check_title": "挂起前进程列表",
                    "results": [
                        result(node="slave1", host="127.0.0.1", cmd="jps", stdout="132 DataNode\n221 NodeManager"),
                        result(node="slave2", host="127.0.0.1", cmd="jps", stdout="135 DataNode\n228 NodeManager"),
                    ],
                },
                {
                    "phase": "action",
                    "results": [
                        result(
                            node="master",
                            host="127.0.0.1",
                            cmd="/root/hadoop-fi/hadoop_injector hang dn",
                            stdout="[Slave] 目标: DataNode (PID: 132)\n[Hang] 已暂停进程 DataNode",
                            elapsed=0.41,
                        )
                    ],
                },
                {
                    "phase": "verify",
                    "check_title": "挂起后进程列表",
                    "results": [
                        result(node="slave1", host="127.0.0.1", cmd="jps", stdout="132 DataNode\n221 NodeManager"),
                        result(node="slave2", host="127.0.0.1", cmd="jps", stdout="135 DataNode\n228 NodeManager"),
                    ],
                },
            ],
        )
    )

    ids.append(
        record_run(
            run_type="cleanup",
            action_key="process_fault",
            scenario_key="test_process_hang_resume",
            title="进程挂起/恢复测试 - 清理",
            params={"component": "dn", "op": "resume"},
            ok=True,
            started_at=now - 7100,
            finished_at=now - 7099,
            phases=[
                {
                    "phase": "cleanup",
                    "results": [
                        result(
                            node="master",
                            host="127.0.0.1",
                            cmd="/root/hadoop-fi/hadoop_injector resume dn",
                            stdout="[Resume] 已恢复进程 DataNode",
                            elapsed=0.27,
                        )
                    ],
                }
            ],
        )
    )

    ids.append(
        record_run(
            run_type="functest",
            action_key="vm_network",
            scenario_key="test_vm_network",
            title="VM 网络故障测试",
            params={"net_type": "delay", "net_param": "200ms"},
            ok=True,
            started_at=now - 5400,
            finished_at=now - 5388,
            phases=[
                {
                    "phase": "baseline",
                    "check_title": "注入前 ping 测试",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="ping -c 4 -W 2 8.8.8.8",
                            stdout="4 packets transmitted, 4 received, 0% packet loss\nrtt min/avg/max = 18.2/21.4/25.0 ms",
                            elapsed=4.02,
                        )
                    ],
                },
                {
                    "phase": "action",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="sudo -n /home/venele/grad_project/vm_injection/network_injector 1 200ms",
                            stdout="[Delay] 已注入延迟: 200ms (设备: eth0)",
                            elapsed=0.19,
                        )
                    ],
                },
                {
                    "phase": "verify",
                    "check_title": "注入后 ping 测试",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="ping -c 4 -W 2 8.8.8.8",
                            stdout="4 packets transmitted, 4 received, 0% packet loss\nrtt min/avg/max = 218.5/224.1/232.6 ms",
                            elapsed=4.31,
                        ),
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="tc qdisc show",
                            stdout="qdisc netem 8001: dev eth0 root refcnt 2 limit 1000 delay 200ms",
                            elapsed=0.05,
                        ),
                    ],
                },
            ],
        )
    )

    ids.append(
        record_run(
            run_type="functest",
            action_key="cloudstack_api_delay",
            scenario_key="test_cloudstack_api_delay",
            title="CloudStack API 延迟注入测试",
            params={"ms": 1000},
            ok=True,
            started_at=now - 3600,
            finished_at=now - 3598,
            phases=[
                {
                    "phase": "baseline",
                    "check_title": "注入前 API 状态",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="cloudstack status",
                            stdout="cloudstack-management PID=410 STATE=S\ncloudstack-agent PID=512 STATE=S",
                        )
                    ],
                },
                {
                    "phase": "action",
                    "results": [
                        result(
                            node="master",
                            host="127.0.0.1",
                            cmd="sudo -n cloudstack_injector api-delay 1000",
                            stdout="[API Delay] 已注入 1000ms 延迟到端口 8080",
                            elapsed=0.33,
                        )
                    ],
                },
                {
                    "phase": "verify",
                    "check_title": "注入后 API 状态",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="cloudstack status",
                            stdout="cloudstack-management PID=410 STATE=S\ncloudstack-agent PID=512 STATE=S\ncloudstack-api-delay 1000ms",
                        )
                    ],
                },
            ],
        )
    )

    ids.append(
        record_run(
            run_type="functest",
            action_key="kvm_perf_delay",
            scenario_key="test_kvm_perf_delay",
            title="KVM 性能延迟测试",
            params={"target": "slave1", "ms": 100, "bench_mb": 64, "rounds": 1},
            ok=True,
            started_at=now - 2400,
            finished_at=now - 2310,
            phases=[
                {
                    "phase": "baseline",
                    "check_title": "延迟前任务速度",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="ssh guest dd/sha256 benchmark",
                            stdout="ROUND_1_CPU_HASH_START\n67.1 MB/s\nROUND_1_DISK_WRITE_START\n142 MB/s",
                            elapsed=16.8,
                        )
                    ],
                },
                {
                    "phase": "action",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="sudo -n kvm_injector perf-delay slave1 100",
                            stdout="[Cgroups v2] 注入CPU限制 (配额: 10%)",
                            elapsed=0.22,
                        )
                    ],
                },
                {
                    "phase": "verify",
                    "check_title": "延迟后任务速度",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="ssh guest dd/sha256 benchmark",
                            stdout="ROUND_1_CPU_HASH_START\n18.4 MB/s\nROUND_1_DISK_WRITE_START\n41 MB/s",
                            elapsed=42.5,
                        )
                    ],
                },
            ],
        )
    )

    ids.append(
        record_run(
            run_type="action",
            action_key="vm_mem_inject",
            scenario_key=None,
            title="VM 内存注入",
            params={"pid": 2401, "mem_region": "heap", "mem_type": "flip", "mem_bit": 3},
            ok=False,
            started_at=now - 1200,
            finished_at=now - 1199,
            phases=[
                {
                    "phase": "action",
                    "results": [
                        result(
                            node="local",
                            host="127.0.0.1",
                            cmd="sudo -n mem_injector -p 2401 -r heap -t flip -b 3",
                            stdout="=== 高级内存故障注入器 ===\n[*] 目标 PID: 2401",
                            stderr="Attach failed: Operation not permitted",
                            exit_code=1,
                            elapsed=0.08,
                        )
                    ],
                }
            ],
        )
    )

    return ids


def main() -> None:
    parser = argparse.ArgumentParser(description="Seed mock fault-injection history data.")
    parser.add_argument("--reset", action="store_true", help="clear existing history before inserting mock data")
    args = parser.parse_args()

    if args.reset:
        reset_history()
    ids = seed()
    print(f"inserted={len(ids)}")
    print("ids=" + ",".join(str(i) for i in ids))


if __name__ == "__main__":
    main()
