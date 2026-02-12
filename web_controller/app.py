from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = BASE_DIR / "config.json"
CONFIG_ENV = "FI_CONTROLLER_CONFIG"
REPO_ROOT = BASE_DIR.parent
VM_DIR = REPO_ROOT / "vm_injection"
VM_LOG_DIR = REPO_ROOT / ".vm_logs"

app = FastAPI(title="Fault Injection Controller", version="0.4")

static_dir = BASE_DIR / "static"
app.mount("/static", StaticFiles(directory=static_dir), name="static")


class ActionRequest(BaseModel):
    action: str
    params: Optional[Dict[str, Any]] = None
    tests: Optional[Dict[str, Any]] = None


GROUPS = [
    {
        "key": "cluster",
        "title": "集群管理",
        "desc": "Hadoop 服务管理与进程状态检查。",
    },
    {
        "key": "process",
        "title": "Hadoop 进程故障",
        "desc": "对 NameNode/DataNode 等核心组件进行崩溃/挂起/恢复。",
    },
    {
        "key": "network",
        "title": "集群网络故障",
        "desc": "延迟、丢包、乱序、隔离与心跳超时模拟。",
    },
    {
        "key": "resource",
        "title": "集群资源故障",
        "desc": "CPU/内存/磁盘/I/O 相关压力与限制。",
    },
    {
        "key": "hdfs",
        "title": "HDFS / YARN",
        "desc": "安全模式、磁盘不足与节点健康状态控制。",
    },
    {
        "key": "mapreduce",
        "title": "MapReduce 任务",
        "desc": "对 Map/Reduce 任务进程进行故障注入。",
    },
    {
        "key": "vm",
        "title": "VM 注入",
        "desc": "虚拟机侧进程、网络、CPU、内存、寄存器注入功能。",
    },
    {
        "key": "kvm",
        "title": "KVM 注入",
        "desc": "KVM 虚拟化层软错误、性能故障与 CPU 热插拔。",
    },
]


def load_config() -> Dict[str, Any]:
    cfg_path = Path(os.environ.get(CONFIG_ENV, str(DEFAULT_CONFIG_PATH)))
    if not cfg_path.exists():
        raise FileNotFoundError(f"Config not found: {cfg_path}")
    with cfg_path.open("r", encoding="utf-8") as f:
        return json.load(f)


def get_nodes(cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
    return cfg.get("nodes", [])


def get_master_node(cfg: Dict[str, Any]) -> Dict[str, Any]:
    for node in get_nodes(cfg):
        if node.get("role") == "master" or node.get("name") == "master":
            return node
    raise RuntimeError("Master node not found in config")


def is_local_node(node: Dict[str, Any]) -> bool:
    # Honor explicit override first; `local: false` should force SSH even on 127.0.0.1.
    if node.get("local") is True:
        return True
    if node.get("local") is False:
        return False
    host = (node.get("host") or "").lower()
    # Treat localhost as local only when there's no SSH port override.
    if host in {"localhost", "127.0.0.1", "::1"} and not node.get("port"):
        return True
    return False


def build_ssh_command(cfg: Dict[str, Any], node: Dict[str, Any], remote_cmd: List[str]) -> List[str]:
    ssh_cfg = cfg.get("ssh", {})
    user = ssh_cfg.get("user", "root")
    identity_file = ssh_cfg.get("identity_file") or ""
    timeout = int(ssh_cfg.get("connect_timeout", 5))

    cmd = ["ssh", "-o", "StrictHostKeyChecking=no", "-o", f"ConnectTimeout={timeout}"]
    if identity_file:
        cmd += ["-i", identity_file]
    if node.get("port"):
        cmd += ["-p", str(node["port"])]
    cmd.append(f"{user}@{node['host']}")
    cmd.append(shlex.join(remote_cmd))
    return cmd


def sanitize_output(text: str) -> str:
    if not text:
        return ""
    text = text.replace("\r\n", "\n").replace("\r", "\n")
    text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", text)
    return text


def truncate_text(text: str, max_lines: int, max_chars: int) -> Dict[str, Any]:
    raw = sanitize_output(text)
    total_chars = len(raw)
    lines = raw.splitlines()
    total_lines = len(lines)
    truncated = False

    if max_lines > 0 and total_lines > max_lines:
        lines = lines[-max_lines:]
        truncated = True

    clipped = "\n".join(lines)
    if max_chars > 0 and len(clipped) > max_chars:
        clipped = clipped[-max_chars:]
        truncated = True

    return {
        "text": clipped,
        "truncated": truncated,
        "total_chars": total_chars,
        "total_lines": total_lines,
    }


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
        if stderr:
            stderr = f"{stderr}\nTimeout after {timeout}s"
        else:
            stderr = f"Timeout after {timeout}s"

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


class _SafeFormat(dict):
    def __missing__(self, key: str) -> str:
        return "{" + key + "}"


def render_template(value: str, ctx: Dict[str, Any]) -> str:
    try:
        return value.format_map(_SafeFormat(ctx))
    except Exception:
        return value


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


def find_node_by_value(cfg: Dict[str, Any], value: str) -> Optional[Dict[str, Any]]:
    if not value:
        return None
    for node in get_nodes(cfg):
        if node.get("name") == value or node.get("host") == value:
            return node
    if validate_ip(value):
        return {"name": value, "host": value, "local": False}
    return None


def resolve_test_nodes(
    cfg: Dict[str, Any],
    scope: str,
    action_nodes: List[Dict[str, Any]],
    params: Dict[str, Any],
) -> List[Dict[str, Any]]:
    if scope == "all":
        return get_nodes(cfg)
    if scope == "master":
        return [get_master_node(cfg)]
    if scope == "local":
        return [{"name": "local", "host": "127.0.0.1", "local": True}]
    if scope == "target":
        target = params.get("target")
        node = find_node_by_value(cfg, str(target)) if target else None
        if node:
            return [node]
        return []
    return action_nodes


def resolve_test_sudo(cfg: Dict[str, Any], test_spec: Dict[str, Any], action_spec: Dict[str, Any]) -> bool:
    if "sudo" not in test_spec:
        return False
    sudo_val = test_spec.get("sudo")
    if isinstance(sudo_val, bool):
        return sudo_val
    if sudo_val in {"vm", "kvm", "hadoop"}:
        return resolve_sudo(cfg, {"sudo": sudo_val})
    if sudo_val == "action":
        return resolve_sudo(cfg, action_spec)
    return False


def collect_tests(
    cfg: Dict[str, Any],
    action: str,
    action_spec: Dict[str, Any],
    params: Dict[str, Any],
    action_nodes: List[Dict[str, Any]],
    action_ok: bool,
    test_flags: Optional[Dict[str, Any]] = None,
) -> List[Dict[str, Any]]:
    tests_cfg = cfg.get("tests", {}) if isinstance(cfg.get("tests", {}), dict) else {}
    if not tests_cfg or not tests_cfg.get("enabled", False):
        return []

    test_flags = test_flags or {}
    allow_kvm_tests = bool(test_flags.get("kvm"))

    tests: List[Dict[str, Any]] = []

    defaults = tests_cfg.get("defaults", {})
    skip_defaults = tests_cfg.get("skip_defaults", []) if isinstance(tests_cfg.get("skip_defaults", []), list) else []
    tool_key = action_spec.get("tool")
    if action not in skip_defaults and tool_key == "injector":
        tests += defaults.get("hadoop", []) if isinstance(defaults.get("hadoop", []), list) else []
    elif action not in skip_defaults and tool_key and tool_key.startswith("vm_"):
        tests += defaults.get("vm", []) if isinstance(defaults.get("vm", []), list) else []
    elif action not in skip_defaults and tool_key == "kvm_injector" and allow_kvm_tests:
        tests += defaults.get("kvm", []) if isinstance(defaults.get("kvm", []), list) else []

    after = tests_cfg.get("after", {})
    if isinstance(after, dict):
        tests += after.get(action, []) if isinstance(after.get(action, []), list) else []

    after_group = tests_cfg.get("after_group", {})
    group_key = action_spec.get("group", "")
    if isinstance(after_group, dict) and group_key:
        if group_key == "kvm" and not allow_kvm_tests:
            pass
        else:
            tests += after_group.get(group_key, []) if isinstance(after_group.get(group_key, []), list) else []

    if not tests:
        return []

    ctx = build_context(cfg)
    ctx.update(params)

    results_payload: List[Dict[str, Any]] = []
    for test in tests:
        if not isinstance(test, dict):
            continue
        if test.get("enabled") is False:
            continue
        when = test.get("when", "success")
        if when == "success" and not action_ok:
            continue

        scope = test.get("scope", "action")
        nodes = resolve_test_nodes(cfg, scope, action_nodes, params)
        if not nodes:
            continue

        test_title = test.get("title", "自动测试")
        test_timeout = test.get("timeout")
        test_cmds_spec = test.get("cmds", test.get("cmd"))
        test_ok = True
        test_results: List[Dict[str, Any]] = []
        for node in nodes:
            node_ctx = dict(ctx)
            node_ctx.update(
                {
                    "node": node.get("name", ""),
                    "host": node.get("host", ""),
                    "port": node.get("port", ""),
                }
            )
            cmds = normalize_cmds(test_cmds_spec, node_ctx)
            for cmd in cmds:
                cmd = maybe_sudo(cmd, resolve_test_sudo(cfg, test, action_spec))
                res = run_on_node(cfg, node, cmd, timeout_override=test_timeout)
                res.update({"node": node.get("name"), "host": node.get("host")})
                test_results.append(res)
                res_ok = bool(res.get("ok"))
                if not res_ok:
                    ok_codes = test.get("ok_exit_codes")
                    allow_timeout = bool(test.get("allow_timeout"))
                    exit_code = res.get("exit_code")
                    if isinstance(ok_codes, list) and exit_code in ok_codes:
                        res["ok"] = True
                        res_ok = True
                    elif allow_timeout and exit_code == 124:
                        res["ok"] = True
                        res_ok = True
                if not res_ok:
                    test_ok = False

        results_payload.append(
            {
                "title": test_title,
                "ok": test_ok,
                "results": test_results,
            }
        )

    return results_payload


def _ps_aux() -> str:
    try:
        result = subprocess.run(
            ["/bin/ps", "aux"],
            capture_output=True,
            text=True,
            timeout=3,
            check=False,
        )
        return result.stdout or ""
    except Exception:
        return ""


def _is_vm_running(node: str) -> bool:
    pattern = re.compile(rf"qemu-system-aarch64.*alpine_{re.escape(node)}")
    return bool(pattern.search(_ps_aux()))


def _ensure_vm_running(node: str) -> None:
    if _is_vm_running(node):
        print(f"[vm-auto-start] skip {node}: already running")
        return

    run_script = VM_DIR / "run_cluster.sh"
    if not run_script.exists():
        print(f"[vm-auto-start] skip {node}: {run_script} not found")
        return
    if not os.access(run_script, os.X_OK):
        print(f"[vm-auto-start] skip {node}: {run_script} not executable")
        return

    VM_LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_file = VM_LOG_DIR / f"{node}.log"

    try:
        with log_file.open("a", encoding="utf-8") as log_fh:
            subprocess.Popen(
                [str(run_script), node],
                cwd=str(VM_DIR),
                stdout=log_fh,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        print(f"[vm-auto-start] started {node} (log: {log_file})")
    except Exception as exc:
        print(f"[vm-auto-start] failed {node}: {exc}")


def maybe_sudo(cmd: List[str], use_sudo: bool) -> List[str]:
    if use_sudo:
        return ["sudo", "-n"] + cmd
    return cmd


def validate_ip(value: str) -> bool:
    if not value:
        return False
    if not re.match(r"^\d{1,3}(?:\.\d{1,3}){3}$", value):
        return False
    parts = value.split(".")
    return all(0 <= int(p) <= 255 for p in parts)


def validate_target(cfg: Dict[str, Any], value: str) -> bool:
    if not value:
        return False
    node_names = {n.get("name") for n in get_nodes(cfg)}
    if value in node_names:
        return True
    return validate_ip(value)


def validate_hex(value: str) -> bool:
    if not value:
        return False
    value = value.lower().strip()
    if value.startswith("0x"):
        value = value[2:]
    return re.fullmatch(r"[0-9a-f]+", value) is not None


def build_context(cfg: Dict[str, Any]) -> Dict[str, Any]:
    hadoop_home = cfg.get("hadoop", {}).get("home", "/opt/hadoop")
    injector = cfg.get("hadoop", {}).get("injector", "")
    vm_cfg = cfg.get("vm", {})
    kvm_cfg = cfg.get("kvm", {})
    vm_base_dir = vm_cfg.get("base_dir", "")
    kvm_base_dir = kvm_cfg.get("base_dir", "")

    return {
        "hadoop_home": hadoop_home,
        "hadoop_sbin": f"{hadoop_home}/sbin",
        "injector": injector,
        "vm_base_dir": vm_base_dir,
        "kvm_base_dir": kvm_base_dir,
        "vm_process_injector": vm_cfg.get("process_injector", ""),
        "vm_network_injector": vm_cfg.get("network_injector", ""),
        "vm_cpu_injector": vm_cfg.get("cpu_injector", ""),
        "vm_mem_leak": vm_cfg.get("mem_leak", ""),
        "vm_mem_injector": vm_cfg.get("mem_injector", ""),
        "vm_reg_injector": vm_cfg.get("reg_injector", ""),
        "kvm_injector": kvm_cfg.get("injector", ""),
    }


def resolve_sudo(cfg: Dict[str, Any], spec: Dict[str, Any]) -> bool:
    sudo_key = spec.get("sudo", "hadoop")
    if sudo_key is True:
        return True
    if sudo_key is False:
        return False
    if sudo_key == "vm":
        return bool(cfg.get("vm", {}).get("use_sudo", False))
    if sudo_key == "kvm":
        return bool(cfg.get("kvm", {}).get("use_sudo", False))
    return bool(cfg.get("hadoop", {}).get("use_sudo", False))


PARAM_ENUMS = {
    "op": {"crash", "hang", "resume"},
    "component": {"nn", "dn", "rm", "nm", "snn", "jhs", "map", "reduce", "am"},
    "mode": {"enter", "leave"},
    "state": {"on", "off"},
    "task": {"map", "reduce"},
    "proc_action": {"crash", "hang", "resume"},
    "net_type": {"delay", "loss", "partition", "corrupt", "clear"},
    "mem_region": {"heap", "stack"},
    "mem_type": {"flip", "set0", "set1", "byte"},
    "reg_type": {
        "flip1",
        "flip2",
        "zero1",
        "zero2",
        "set1",
        "set2",
        "low0",
        "low1",
        "lowerr",
        "add1",
        "add2",
        "add3",
        "add4",
        "add5",
    },
    "soft_type": {"flip", "swap", "zero"},
    "guest_type": {"data", "divzero", "invalid"},
    "cpu_state": {"online", "offline"},
}

NUM_RANGES = {
    "ms": (1, 60000),
    "jitter": (0, 60000),
    "percent": (0, 100),
    "correlation": (0, 100),
    "duration": (1, 86400),
    "threads": (0, 256),
    "size_mb": (1, 1048576),
    "port": (1, 65535),
    "pid": (0, 1000000000),
    "cpu_mode": (1, 2),
    "mem_bit": (0, 63),
    "soft_bit": (-1, 63),
    "reg_bit": (-1, 63),
    "reg_delay": (0, 60000000),
    "reg_loop": (0, 1000000),
    "reg_interval": (1, 600000),
    "cpu_id": (0, 1024),
}


ACTIONS: Dict[str, Dict[str, Any]] = {
    "cluster_status": {
        "title": "节点进程状态 (jps)",
        "desc": "在所有节点上执行 jps，查看 Hadoop 进程状态。",
        "group": "cluster",
        "scope": "all",
        "params": [],
        "cmds": lambda ctx, params: [["jps"]],
        "sudo": False,
    },
    "hadoop_start": {
        "title": "启动 Hadoop",
        "desc": "依次启动 HDFS 与 YARN 服务。",
        "group": "cluster",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [
            ["/bin/sh", "-lc", "source /etc/profile; hdfs --daemon start namenode"],
            ["/bin/sh", "-lc", "source /etc/profile; hdfs --daemon start secondarynamenode"],
            ["ssh", "slave1", "source /etc/profile; hdfs --daemon start datanode"],
            ["ssh", "slave2", "source /etc/profile; hdfs --daemon start datanode"],
            ["/bin/sh", "-lc", "source /etc/profile; yarn --daemon start resourcemanager"],
            ["ssh", "slave1", "source /etc/profile; yarn --daemon start nodemanager"],
            ["ssh", "slave2", "source /etc/profile; yarn --daemon start nodemanager"],
        ],
    },
    "hadoop_stop": {
        "title": "停止 Hadoop",
        "desc": "依次停止 YARN 与 HDFS 服务。",
        "group": "cluster",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [
            [f"{ctx['hadoop_sbin']}/stop-yarn.sh"],
            [f"{ctx['hadoop_sbin']}/stop-dfs.sh"],
        ],
    },
    "hadoop_restart": {
        "title": "重启 Hadoop",
        "desc": "停止后等待 3 秒，再启动 HDFS/YARN。",
        "group": "cluster",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [
            [f"{ctx['hadoop_sbin']}/stop-yarn.sh"],
            [f"{ctx['hadoop_sbin']}/stop-dfs.sh"],
            ["/bin/sleep", "3"],
            ["/bin/sh", "-lc", "source /etc/profile; hdfs --daemon start namenode"],
            ["/bin/sh", "-lc", "source /etc/profile; hdfs --daemon start secondarynamenode"],
            ["ssh", "slave1", "source /etc/profile; hdfs --daemon start datanode"],
            ["ssh", "slave2", "source /etc/profile; hdfs --daemon start datanode"],
            ["/bin/sh", "-lc", "source /etc/profile; yarn --daemon start resourcemanager"],
            ["ssh", "slave1", "source /etc/profile; yarn --daemon start nodemanager"],
            ["ssh", "slave2", "source /etc/profile; yarn --daemon start nodemanager"],
        ],
    },
    "inject_list": {
        "title": "进程清单 (injector)",
        "desc": "通过 hadoop_injector 查看集群进程清单。",
        "group": "cluster",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["injector"], "list"]],
        "tool": "injector",
    },
    "process_fault": {
        "title": "进程故障 (崩溃/挂起/恢复)",
        "desc": "选择组件与操作，注入崩溃/挂起/恢复故障。",
        "group": "process",
        "scope": "master",
        "params": [
            {
                "name": "op",
                "label": "操作",
                "type": "select",
                "options": [
                    {"value": "crash", "label": "崩溃 (crash)"},
                    {"value": "hang", "label": "挂起 (hang)"},
                    {"value": "resume", "label": "恢复 (resume)"},
                ],
                "default": "crash",
                "required": True,
            },
            {
                "name": "component",
                "label": "组件",
                "type": "select",
                "options": [
                    {"value": "nn", "label": "NameNode (nn)"},
                    {"value": "dn", "label": "DataNode (dn)"},
                    {"value": "rm", "label": "ResourceManager (rm)"},
                    {"value": "nm", "label": "NodeManager (nm)"},
                    {"value": "snn", "label": "SecondaryNameNode (snn)"},
                    {"value": "jhs", "label": "JobHistoryServer (jhs)"},
                    {"value": "map", "label": "Map 任务 (map)"},
                    {"value": "reduce", "label": "Reduce 任务 (reduce)"},
                    {"value": "am", "label": "AppMaster (am)"},
                ],
                "default": "nn",
                "required": True,
            },
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], params["op"], params["component"]]],
        "tool": "injector",
        "danger": True,
    },
    "delay": {
        "title": "网络延迟",
        "desc": "为目标节点注入延迟与抖动 (jitter)。",
        "group": "network",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "ms", "label": "延迟 (ms)", "type": "number", "default": 200, "required": True},
            {"name": "jitter", "label": "抖动 (ms, 可选)", "type": "number", "required": False},
        ],
        "cmds": lambda ctx, params: (
            [[ctx["injector"], "delay", params["target"], str(params["ms"]), str(params["jitter"])]]
            if params.get("jitter") is not None
            else [[ctx["injector"], "delay", params["target"], str(params["ms"])]]
        ),
        "tool": "injector",
    },
    "delay_clear": {
        "title": "清理延迟",
        "desc": "清理集群的延迟规则。",
        "group": "network",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["injector"], "delay-clear"]],
        "tool": "injector",
    },
    "delay_show": {
        "title": "查看延迟规则",
        "desc": "显示 master 节点当前 tc 规则。",
        "group": "network",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["injector"], "delay-show"]],
        "tool": "injector",
    },
    "loss": {
        "title": "网络丢包",
        "desc": "为目标节点注入丢包率。",
        "group": "network",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "percent", "label": "丢包率 (%)", "type": "number", "default": 10, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], "loss", params["target"], str(params["percent"])]],
        "tool": "injector",
    },
    "loss_clear": {
        "title": "清理丢包",
        "desc": "清理集群的丢包规则。",
        "group": "network",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["injector"], "loss-clear"]],
        "tool": "injector",
    },
    "reorder": {
        "title": "网络乱序",
        "desc": "为目标节点注入乱序与相关性。",
        "group": "network",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "percent", "label": "乱序率 (%)", "type": "number", "default": 10, "required": True},
            {"name": "correlation", "label": "相关性 (%)", "type": "number", "default": 25, "required": False},
        ],
        "cmds": lambda ctx, params: (
            [[ctx["injector"], "reorder", params["target"], str(params["percent"]), str(params["correlation"])]]
            if params.get("correlation") is not None
            else [[ctx["injector"], "reorder", params["target"], str(params["percent"])]]
        ),
        "tool": "injector",
    },
    "reorder_clear": {
        "title": "清理乱序",
        "desc": "清理集群的乱序规则。",
        "group": "network",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["injector"], "reorder-clear"]],
        "tool": "injector",
    },
    "isolate": {
        "title": "网络隔离/分区",
        "desc": "隔离节点或端口。端口为空则隔离 Hadoop 端口。",
        "group": "network",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "port", "label": "端口 (可选)", "type": "number", "required": False},
        ],
        "cmds": lambda ctx, params: (
            [[ctx["injector"], "isolate", params["target"], str(params["port"])]]
            if params.get("port") is not None
            else [[ctx["injector"], "isolate", params["target"]]]
        ),
        "tool": "injector",
        "danger": True,
    },
    "isolate_clear": {
        "title": "清理隔离",
        "desc": "清理集群的隔离防火墙规则。",
        "group": "network",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["injector"], "isolate-clear"]],
        "tool": "injector",
    },
    "heartbeat": {
        "title": "心跳超时",
        "desc": "模拟心跳超时（底层复用延迟）。",
        "group": "network",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "ms", "label": "超时 (ms)", "type": "number", "default": 3000, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], "heartbeat", params["target"], str(params["ms"]) ]],
        "tool": "injector",
    },
    "cpu_stress": {
        "title": "CPU 压力",
        "desc": "在目标节点上运行 CPU 压力测试。",
        "group": "resource",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 10, "required": True},
            {"name": "threads", "label": "线程数 (可选)", "type": "number", "required": False},
        ],
        "cmds": lambda ctx, params: (
            [[ctx["injector"], "cpu-stress", params["target"], str(params["duration"]), str(params["threads"])]]
            if params.get("threads") is not None
            else [[ctx["injector"], "cpu-stress", params["target"], str(params["duration"])]]
        ),
        "tool": "injector",
        "danger": True,
    },
    "mem_stress": {
        "title": "内存压力",
        "desc": "在目标节点上消耗指定内存。",
        "group": "resource",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "size_mb", "label": "内存 (MB)", "type": "number", "default": 512, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], "mem-stress", params["target"], str(params["size_mb"])]],
        "tool": "injector",
        "danger": True,
    },
    "mem_stress_clear": {
        "title": "清理内存压力",
        "desc": "释放全集群内存压力。",
        "group": "resource",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["injector"], "mem-stress-clear"]],
        "tool": "injector",
    },
    "disk_fill": {
        "title": "磁盘填满",
        "desc": "在目标节点生成大文件占满磁盘空间。",
        "group": "resource",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "size_mb", "label": "大小 (MB)", "type": "number", "default": 512, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], "disk-fill", params["target"], str(params["size_mb"])]],
        "tool": "injector",
        "danger": True,
    },
    "disk_fill_clear": {
        "title": "清理磁盘填充",
        "desc": "删除 /tmp/disk_hog 释放磁盘空间。",
        "group": "resource",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["injector"], "disk-fill-clear"]],
        "tool": "injector",
    },
    "io_slow": {
        "title": "磁盘 I/O 限速",
        "desc": "开启或关闭磁盘 I/O 限速 (cgroup v2)。",
        "group": "resource",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {
                "name": "state",
                "label": "开关",
                "type": "select",
                "options": [
                    {"value": "on", "label": "开启"},
                    {"value": "off", "label": "关闭"},
                ],
                "default": "on",
                "required": True,
            },
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], "io-slow", params["target"], params["state"]]],
        "tool": "injector",
        "danger": True,
    },
    "hdfs_safe": {
        "title": "HDFS 安全模式",
        "desc": "进入或退出 HDFS 安全模式。",
        "group": "hdfs",
        "scope": "master",
        "params": [
            {
                "name": "mode",
                "label": "模式",
                "type": "select",
                "options": [
                    {"value": "enter", "label": "进入"},
                    {"value": "leave", "label": "退出"},
                ],
                "default": "enter",
                "required": True,
            }
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], "hdfs-safe", params["mode"]]],
        "tool": "injector",
    },
    "hdfs_disk": {
        "title": "HDFS 磁盘不足",
        "desc": "填充磁盘模拟 HDFS 空间不足。",
        "group": "hdfs",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {"name": "size_mb", "label": "大小 (MB)", "type": "number", "default": 512, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], "hdfs-disk", params["target"], str(params["size_mb"])]],
        "tool": "injector",
        "danger": True,
    },
    "yarn_unhealthy": {
        "title": "YARN 节点不健康",
        "desc": "模拟 NodeManager 不健康状态。",
        "group": "hdfs",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点", "type": "node", "required": True},
            {
                "name": "state",
                "label": "状态",
                "type": "select",
                "options": [
                    {"value": "on", "label": "标记不健康"},
                    {"value": "off", "label": "恢复健康"},
                ],
                "default": "on",
                "required": True,
            },
        ],
        "cmds": lambda ctx, params: [[ctx["injector"], "yarn-unhealthy", params["target"], params["state"]]],
        "tool": "injector",
        "danger": True,
    },
    "mapreduce_fault": {
        "title": "MapReduce 任务故障",
        "desc": "杀死指定节点上的 Map 或 Reduce 任务。",
        "group": "mapreduce",
        "scope": "master",
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
        "cmds": lambda ctx, params: (
            [[ctx["injector"], "crash-map", params["target"]]]
            if params["task"] == "map"
            else [[ctx["injector"], "crash-reduce", params["target"]]]
        ),
        "tool": "injector",
        "danger": True,
    },
    "vm_process": {
        "title": "VM 进程控制",
        "desc": "对目标进程执行崩溃/挂起/恢复。",
        "group": "vm",
        "scope": "local",
        "params": [
            {
                "name": "process",
                "label": "进程名",
                "type": "text",
                "required": True,
                "placeholder": "qemu-system-aarch64 / target",
            },
            {
                "name": "proc_action",
                "label": "操作",
                "type": "select",
                "options": [
                    {"value": "crash", "label": "崩溃 (crash)"},
                    {"value": "hang", "label": "挂起 (hang)"},
                    {"value": "resume", "label": "恢复 (resume)"},
                ],
                "default": "crash",
                "required": True,
            },
        ],
        "cmds": lambda ctx, params: [
            [
                ctx["vm_process_injector"],
                params["process"],
                {"crash": "1", "hang": "2", "resume": "3"}[params["proc_action"]],
            ]
        ],
        "tool": "vm_process_injector",
        "sudo": "vm",
        "danger": True,
    },
    "vm_network": {
        "title": "VM 网络故障",
        "desc": "延迟/丢包/端口隔离/报文损坏/清理。",
        "group": "vm",
        "scope": "local",
        "params": [
            {
                "name": "net_type",
                "label": "故障类型",
                "type": "select",
                "options": [
                    {"value": "delay", "label": "延迟 (delay)"},
                    {"value": "loss", "label": "丢包 (loss)"},
                    {"value": "partition", "label": "端口隔离 (partition)"},
                    {"value": "corrupt", "label": "报文损坏 (corrupt)"},
                    {"value": "clear", "label": "清理 (clear)"},
                ],
                "default": "delay",
                "required": True,
            },
            {
                "name": "net_param",
                "label": "参数",
                "type": "text",
                "required": False,
                "placeholder": "100ms / 10% / 8080",
                "help": "根据故障类型填写参数，清理模式无需参数。",
            },
        ],
        "cmds": lambda ctx, params: [
            [
                ctx["vm_network_injector"],
                {
                    "delay": "1",
                    "loss": "2",
                    "partition": "3",
                    "corrupt": "4",
                    "clear": "0",
                }[params["net_type"]],
            ]
            + ([params["net_param"]] if params.get("net_param") else [])
        ],
        "tool": "vm_network_injector",
        "sudo": "vm",
        "danger": True,
    },
    "vm_cpu": {
        "title": "VM CPU 压力",
        "desc": "使用 cpu_injector 施加高负载。",
        "group": "vm",
        "scope": "local",
        "params": [
            {"name": "pid", "label": "目标 PID (可选)", "type": "number", "required": False, "placeholder": "0"},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 10, "required": True},
            {"name": "threads", "label": "线程数 (可选)", "type": "number", "required": False},
            {
                "name": "cpu_mode",
                "label": "模式",
                "type": "select",
                "options": [
                    {"value": "2", "label": "激进 (2)"},
                    {"value": "1", "label": "普通 (1)"},
                ],
                "default": "2",
                "required": False,
            },
        ],
        "cmds": lambda ctx, params: [
            [ctx["vm_cpu_injector"], str(params.get("pid", 0)), str(params["duration"])]
            + ([str(params.get("threads", 0))] if params.get("threads") is not None or params.get("cpu_mode") is not None else [])
            + ([str(params["cpu_mode"])] if params.get("cpu_mode") is not None else [])
        ],
        "tool": "vm_cpu_injector",
        "sudo": "vm",
        "danger": True,
    },
    "vm_mem_leak": {
        "title": "VM 内存泄漏",
        "desc": "使用 mem_leak 大量占用内存，模拟 OOM。",
        "group": "vm",
        "scope": "local",
        "timeout": 300,
        "params": [
            {"name": "size_mb", "label": "占用内存 (MB)", "type": "number", "default": 512, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["vm_mem_leak"], "0", str(params["size_mb"]) ]],
        "tool": "vm_mem_leak",
        "sudo": "vm",
        "danger": True,
    },
    "vm_mem_inject": {
        "title": "VM 内存注入",
        "desc": "ptrace 内存位翻转/置 0/置 1/字节随机化。",
        "group": "vm",
        "scope": "local",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {
                "name": "mem_region",
                "label": "区域",
                "type": "select",
                "options": [
                    {"value": "heap", "label": "Heap"},
                    {"value": "stack", "label": "Stack"},
                ],
                "default": "heap",
                "required": True,
            },
            {
                "name": "mem_type",
                "label": "故障类型",
                "type": "select",
                "options": [
                    {"value": "flip", "label": "flip (位翻转)"},
                    {"value": "set0", "label": "set0"},
                    {"value": "set1", "label": "set1"},
                    {"value": "byte", "label": "byte (随机字节)"},
                ],
                "default": "flip",
                "required": True,
            },
            {"name": "mem_bit", "label": "目标位 (0-63)", "type": "number", "default": 0, "required": True},
            {"name": "addr", "label": "手动地址 (Hex 可选)", "type": "text", "required": False, "placeholder": "0x7ff..."},
            {
                "name": "signature",
                "label": "扫描特征值 (Hex 可选)",
                "type": "text",
                "required": False,
                "placeholder": "deadbeefcafebabe",
                "help": "填写特征值将启用扫描模式。",
            },
        ],
        "cmds": lambda ctx, params: _build_vm_mem_inject_cmd(ctx, params),
        "tool": "vm_mem_injector",
        "sudo": "vm",
        "danger": True,
    },
    "vm_reg_inject": {
        "title": "VM 寄存器注入",
        "desc": "ARM64 寄存器故障注入，支持延时与循环。",
        "group": "vm",
        "scope": "local",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {"name": "reg", "label": "寄存器", "type": "text", "required": True, "placeholder": "X0 / SP / PC"},
            {
                "name": "reg_type",
                "label": "故障类型",
                "type": "select",
                "options": [
                    {"value": "flip1", "label": "flip1"},
                    {"value": "flip2", "label": "flip2"},
                    {"value": "zero1", "label": "zero1"},
                    {"value": "zero2", "label": "zero2"},
                    {"value": "set1", "label": "set1"},
                    {"value": "set2", "label": "set2"},
                    {"value": "low0", "label": "low0"},
                    {"value": "low1", "label": "low1"},
                    {"value": "lowerr", "label": "lowerr"},
                    {"value": "add1", "label": "add1"},
                    {"value": "add2", "label": "add2"},
                    {"value": "add3", "label": "add3"},
                    {"value": "add4", "label": "add4"},
                    {"value": "add5", "label": "add5"},
                ],
                "default": "flip1",
                "required": True,
            },
            {"name": "reg_bit", "label": "目标位 (-1 随机)", "type": "number", "default": -1, "required": True},
            {"name": "reg_delay", "label": "延迟 (微秒, 可选)", "type": "number", "required": False},
            {"name": "reg_loop", "label": "循环次数 (0=无限, 可选)", "type": "number", "required": False},
            {"name": "reg_interval", "label": "循环间隔(ms, 可选)", "type": "number", "required": False},
        ],
        "cmds": lambda ctx, params: _build_vm_reg_inject_cmd(ctx, params),
        "tool": "vm_reg_injector",
        "sudo": "vm",
        "danger": True,
    },
    "kvm_list": {
        "title": "KVM 虚拟机列表",
        "desc": "列出当前运行的 KVM 虚拟机进程。",
        "group": "kvm",
        "scope": "local",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["kvm_injector"], "list"]],
        "tool": "kvm_injector",
        "sudo": "kvm",
    },
    "kvm_soft": {
        "title": "KVM 软错误注入",
        "desc": "对虚拟机寄存器执行位翻转/交换/置零。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {"name": "reg", "label": "寄存器", "type": "text", "required": True, "placeholder": "PC / SP / X0"},
            {
                "name": "soft_type",
                "label": "故障类型",
                "type": "select",
                "options": [
                    {"value": "flip", "label": "位翻转 (soft-flip)"},
                    {"value": "swap", "label": "位交换 (soft-swap)"},
                    {"value": "zero", "label": "置零覆盖 (soft-zero)"},
                ],
                "default": "flip",
                "required": True,
            },
            {
                "name": "soft_bit",
                "label": "位索引 (-1 随机, 可选)",
                "type": "number",
                "required": False,
                "placeholder": "-1",
                "help": "仅 flip/zero 可用，留空则随机。",
            },
        ],
        "cmds": lambda ctx, params: _build_kvm_soft_cmd(ctx, params),
        "tool": "kvm_injector",
        "sudo": "kvm",
        "danger": True,
    },
    "kvm_guest": {
        "title": "KVM 客户OS错误行为",
        "desc": "模拟客户机异常行为（数据段异常/除零/非法指令）。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {
                "name": "guest_type",
                "label": "类型",
                "type": "select",
                "options": [
                    {"value": "data", "label": "数据段异常"},
                    {"value": "divzero", "label": "除零异常"},
                    {"value": "invalid", "label": "非法指令"},
                ],
                "default": "data",
                "required": True,
            },
        ],
        "cmds": lambda ctx, params: [
            [
                ctx["kvm_injector"],
                {"data": "guest-data", "divzero": "guest-divzero", "invalid": "guest-invalid"}[params["guest_type"]],
                str(params["pid"]),
            ]
        ],
        "tool": "kvm_injector",
        "sudo": "kvm",
        "danger": True,
    },
    "kvm_perf_delay": {
        "title": "KVM 性能故障 - 延迟",
        "desc": "为指定虚拟机注入执行延迟。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {"name": "ms", "label": "延迟 (毫秒)", "type": "number", "default": 100, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["kvm_injector"], "perf-delay", str(params["pid"]), str(params["ms"]) ]],
        "tool": "kvm_injector",
        "sudo": "kvm",
        "danger": True,
    },
    "kvm_perf_stress": {
        "title": "KVM 性能故障 - CPU 压力",
        "desc": "注入 CPU 高负载，模拟资源争抢。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 10, "required": True},
            {"name": "threads", "label": "线程数 (可选)", "type": "number", "required": False},
        ],
        "cmds": lambda ctx, params: (
            [[ctx["kvm_injector"], "perf-stress", str(params["pid"]), str(params["duration"]), str(params["threads"])]]
            if params.get("threads") is not None
            else [[ctx["kvm_injector"], "perf-stress", str(params["pid"]), str(params["duration"])]]
        ),
        "tool": "kvm_injector",
        "sudo": "kvm",
        "danger": True,
    },
    "kvm_perf_clear": {
        "title": "KVM 性能故障 - 清理",
        "desc": "清理性能限制。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {"name": "pid", "label": "目标 PID", "type": "number", "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["kvm_injector"], "perf-clear", str(params["pid"]) ]],
        "tool": "kvm_injector",
        "sudo": "kvm",
    },
    "kvm_cpu_hotplug": {
        "title": "KVM CPU 热插拔",
        "desc": "上线或下线指定 CPU 核心。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {"name": "cpu_id", "label": "CPU 号", "type": "number", "default": 0, "required": True},
            {
                "name": "cpu_state",
                "label": "动作",
                "type": "select",
                "options": [
                    {"value": "offline", "label": "下线"},
                    {"value": "online", "label": "上线"},
                ],
                "default": "offline",
                "required": True,
            },
        ],
        "cmds": lambda ctx, params: [
            [
                ctx["kvm_injector"],
                "cpu-offline" if params["cpu_state"] == "offline" else "cpu-online",
                str(params["cpu_id"]),
            ]
        ],
        "tool": "kvm_injector",
        "sudo": "kvm",
        "danger": True,
    },
    "kvm_clear": {
        "title": "KVM 一键清理",
        "desc": "清理所有 KVM 注入故障。",
        "group": "kvm",
        "scope": "local",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["kvm_injector"], "clear"]],
        "tool": "kvm_injector",
        "sudo": "kvm",
    },
}


def _build_vm_mem_inject_cmd(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    cmd = [ctx["vm_mem_injector"], "-p", str(params["pid"]), "-t", params["mem_type"], "-b", str(params["mem_bit"])]
    addr = params.get("addr")
    signature = params.get("signature")

    if addr:
        cmd += ["-a", addr]
    else:
        cmd += ["-r", params["mem_region"]]

    if signature:
        cmd += ["-s", signature]

    return [cmd]


def _build_vm_reg_inject_cmd(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    cmd = [
        ctx["vm_reg_injector"],
        str(params["pid"]),
        params["reg"],
        params["reg_type"],
        str(params["reg_bit"]),
    ]
    if params.get("reg_delay") is not None:
        cmd += ["-w", str(params["reg_delay"])]
    if params.get("reg_loop") is not None:
        cmd += ["-l", str(params["reg_loop"])]
    if params.get("reg_interval") is not None:
        cmd += ["-i", str(params["reg_interval"])]
    return [cmd]


def _build_kvm_soft_cmd(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    soft_type = params["soft_type"]
    cmd = [ctx["kvm_injector"]]
    if soft_type == "flip":
        cmd.append("soft-flip")
    elif soft_type == "swap":
        cmd.append("soft-swap")
    else:
        cmd.append("soft-zero")

    cmd += [str(params["pid"]), params["reg"]]

    if params.get("soft_bit") is not None and soft_type in {"flip", "zero"}:
        cmd.append(str(params["soft_bit"]))

    return [cmd]


@app.get("/")
def index() -> FileResponse:
    return FileResponse(static_dir / "index.html")


@app.get("/test")
def test_page() -> FileResponse:
    return FileResponse(static_dir / "test.html")


# ---------------------------------------------------------------------------
#  Functional test API
# ---------------------------------------------------------------------------

from web_controller.test_scenarios import FUNC_TESTS, FUNC_TESTS_MAP  # noqa: E402


class FuncTestRequest(BaseModel):
    key: str
    params: Optional[Dict[str, Any]] = None


@app.get("/api/testcases")
def api_testcases() -> JSONResponse:
    """Return all functional test scenario definitions for the frontend."""
    cases = []
    for t in FUNC_TESTS:
        cases.append({
            "key": t["key"],
            "title": t["title"],
            "desc": t["desc"],
            "group": t["group"],
            "params": t.get("params", []),
            "has_baseline": bool(t.get("baseline")),
            "has_cleanup": bool(t.get("cleanup")),
        })
    return JSONResponse({"tests": cases, "groups": GROUPS})


def _run_check_cmds(
    cfg: Dict[str, Any],
    checks: List[Dict[str, Any]],
    params: Dict[str, Any],
    ctx: Dict[str, Any],
) -> List[Dict[str, Any]]:
    """Execute a list of check commands (baseline or verify) and return results."""
    results: List[Dict[str, Any]] = []
    for check in checks:
        title = check.get("title", "检查")
        cmd_tpl = check.get("cmd", "")
        scope = check.get("scope", "master")

        # Render template variables into the command
        check_ctx = dict(ctx)
        check_ctx.update(params)
        try:
            rendered = cmd_tpl.format_map(_SafeFormat(check_ctx))
        except Exception:
            rendered = cmd_tpl

        # Determine nodes based on scope
        if scope == "all":
            nodes = get_nodes(cfg)
        elif scope == "local":
            nodes = [{"name": "local", "host": "127.0.0.1", "local": True}]
        else:
            nodes = [get_master_node(cfg)]

        node_results = []
        for node in nodes:
            cmd = ["/bin/sh", "-lc", rendered]
            res = run_on_node(cfg, node, cmd, timeout_override=15)
            res.update({"node": node.get("name"), "host": node.get("host")})
            node_results.append(res)

        results.append({
            "title": title,
            "cmd": rendered,
            "scope": scope,
            "results": node_results,
            "ok": all(r.get("ok") for r in node_results) if node_results else True,
        })
    return results


@app.post("/api/functest")
def api_functest(req: FuncTestRequest) -> JSONResponse:
    """Run a single functional test scenario with baseline → action → verify."""
    test_key = req.key
    user_params = req.params or {}

    if test_key not in FUNC_TESTS_MAP:
        raise HTTPException(status_code=400, detail=f"未知测试: {test_key}")

    scenario = FUNC_TESTS_MAP[test_key]
    cfg = load_config()
    ctx = build_context(cfg)

    # Merge default action params with user params
    action_params = dict(scenario.get("action_params", {}))
    action_params.update(user_params)

    # 1. Baseline checks
    baseline_results = _run_check_cmds(
        cfg, scenario.get("baseline", []), action_params, ctx,
    )

    # 2. Execute the action
    action_key = scenario["action"]
    action_response = None
    action_ok = False
    if action_key and action_key in ACTIONS:
        try:
            # Call the existing action endpoint directly via internal logic
            spec = ACTIONS[action_key]
            param_defs = spec.get("params", [])

            # Apply same validation as api_action
            for p in param_defs:
                name = p.get("name")
                required = bool(p.get("required", True))
                if required and (name not in action_params or action_params[name] in (None, "")):
                    raise HTTPException(status_code=400, detail=f"缺少测试参数: {name}")

            scope = spec.get("scope", "master")
            if scope == "all":
                nodes = get_nodes(cfg)
            elif scope == "local":
                nodes = [{"name": "local", "host": "127.0.0.1", "local": True}]
            else:
                nodes = [get_master_node(cfg)]

            use_sudo = resolve_sudo(cfg, spec)
            action_results = []
            action_timeout = spec.get("timeout")
            for node in nodes:
                cmds = spec["cmds"](ctx, action_params)
                for cmd in cmds:
                    cmd = maybe_sudo(cmd, use_sudo)
                    res = run_on_node(cfg, node, cmd, timeout_override=action_timeout)
                    res.update({"node": node.get("name"), "host": node.get("host")})
                    action_results.append(res)

            action_ok = all(r.get("ok") for r in action_results) if action_results else False
            action_response = {
                "ok": action_ok,
                "action": action_key,
                "results": action_results,
            }
        except HTTPException:
            raise
        except Exception as exc:
            action_response = {
                "ok": False,
                "action": action_key,
                "results": [],
                "error": str(exc),
            }

    # 3. Verify checks
    verify_results = _run_check_cmds(
        cfg, scenario.get("verify", []), action_params, ctx,
    )

    # Build response
    return JSONResponse({
        "key": test_key,
        "title": scenario["title"],
        "ok": action_ok,
        "baseline": baseline_results,
        "action": action_response,
        "verify": verify_results,
        "has_cleanup": bool(scenario.get("cleanup")),
        "cleanup_action": scenario.get("cleanup"),
        "cleanup_params": scenario.get("cleanup_params", scenario.get("cleanup_params_override", {})),
    })


@app.on_event("startup")
def auto_start_vms() -> None:
    for node in ("master", "slave1", "slave2"):
        _ensure_vm_running(node)


@app.get("/api/config")
def api_config() -> JSONResponse:
    cfg = load_config()
    nodes = [
        {
            "name": n.get("name"),
            "host": n.get("host"),
            "port": n.get("port"),
            "role": n.get("role"),
            "local": bool(n.get("local", False)),
        }
        for n in get_nodes(cfg)
    ]

    actions = []
    for key, spec in ACTIONS.items():
        actions.append(
            {
                "key": key,
                "title": spec.get("title"),
                "desc": spec.get("desc"),
                "group": spec.get("group"),
                "params": spec.get("params", []),
                "danger": bool(spec.get("danger", False)),
            }
        )

    output_cfg = cfg.get("output", {})
    return JSONResponse({"nodes": nodes, "actions": actions, "groups": GROUPS, "output": output_cfg})


@app.post("/api/action")
def api_action(req: ActionRequest) -> JSONResponse:
    cfg = load_config()
    action = req.action
    params = req.params or {}
    test_flags = req.tests or {}

    if action not in ACTIONS:
        raise HTTPException(status_code=400, detail="未知操作")

    spec = ACTIONS[action]
    param_defs = spec.get("params", [])

    for p in param_defs:
        name = p.get("name")
        required = bool(p.get("required", True))
        if required and (name not in params or params[name] in (None, "")):
            raise HTTPException(status_code=400, detail=f"缺少参数: {name}")

    for p in param_defs:
        name = p.get("name")
        if params.get(name) in (None, ""):
            params.pop(name, None)

    for name, allowed in PARAM_ENUMS.items():
        if name in params and params[name] not in allowed:
            raise HTTPException(status_code=400, detail=f"参数 {name} 非法")

    if "target" in params and not validate_target(cfg, params["target"]):
        raise HTTPException(status_code=400, detail="目标节点无效")

    if "addr" in params and params.get("addr"):
        if not validate_hex(params["addr"]):
            raise HTTPException(status_code=400, detail="地址必须为十六进制")

    if "signature" in params and params.get("signature"):
        if not validate_hex(params["signature"]):
            raise HTTPException(status_code=400, detail="特征值必须为十六进制")

    for name, (min_v, max_v) in NUM_RANGES.items():
        if name in params:
            try:
                value = int(params[name])
            except (TypeError, ValueError):
                raise HTTPException(status_code=400, detail=f"参数 {name} 无效")
            if value < min_v or value > max_v:
                raise HTTPException(status_code=400, detail=f"参数 {name} 超出范围")
            params[name] = value

    if action == "vm_network":
        net_type = params.get("net_type")
        if net_type != "clear" and not params.get("net_param"):
            raise HTTPException(status_code=400, detail="该网络故障需要参数")

    ctx = build_context(cfg)

    scope = spec.get("scope", "master")
    if scope == "all":
        nodes = get_nodes(cfg)
    elif scope == "local":
        nodes = [{"name": "local", "host": "127.0.0.1", "local": True}]
    else:
        nodes = [get_master_node(cfg)]

    tool_key = spec.get("tool")
    if tool_key:
        tool_path = ctx.get(tool_key, "")
        if not tool_path:
            raise HTTPException(status_code=400, detail="未配置工具路径")
        # Only validate local path when the command will run locally.
        if any(is_local_node(n) for n in nodes) and not Path(tool_path).exists():
            raise HTTPException(status_code=400, detail="工具不存在或路径错误")

    use_sudo = resolve_sudo(cfg, spec)

    results = []
    action_timeout = spec.get("timeout")
    for node in nodes:
        cmds = spec["cmds"](ctx, params)
        for cmd in cmds:
            cmd = maybe_sudo(cmd, use_sudo)
            res = run_on_node(cfg, node, cmd, timeout_override=action_timeout)
            res.update({"node": node.get("name"), "host": node.get("host")})
            results.append(res)

    ok = all(r.get("ok") for r in results) if results else False
    tests = collect_tests(cfg, action, spec, params, nodes, ok, test_flags=test_flags)
    return JSONResponse({"ok": ok, "action": action, "results": results, "tests": tests})


@app.get("/api/test")
def api_test() -> JSONResponse:
    """Run the pytest suite and return structured JSON results."""
    import subprocess as _sp
    import json as _json
    import tempfile as _tf

    tests_dir = BASE_DIR / "tests"
    if not tests_dir.exists():
        return JSONResponse({"ok": False, "error": "tests/ directory not found", "tests": [], "summary": {}})

    with _tf.TemporaryDirectory() as tmpdir:
        report_path = Path(tmpdir) / "report.json"
        cmd = [
            sys.executable, "-m", "pytest",
            str(tests_dir),
            "--tb=short",
            "-q",
            f"--json-report-file={report_path}",
            "--json-report",
        ]
        try:
            result = _sp.run(cmd, capture_output=True, text=True, timeout=120, check=False,
                             cwd=str(REPO_ROOT))
        except _sp.TimeoutExpired:
            return JSONResponse({"ok": False, "error": "Test execution timed out", "tests": [], "summary": {}})

        if not report_path.exists():
            return JSONResponse({
                "ok": False,
                "error": f"pytest failed to produce report.\nstdout: {result.stdout[-2000:]}\nstderr: {result.stderr[-2000:]}",
                "tests": [],
                "summary": {},
            })

        report = _json.loads(report_path.read_text(encoding="utf-8"))

    tests_out: list = []
    for t in report.get("tests", []):
        node_id = t.get("nodeid", "")
        # Extract group from class name: "tests/test_app.py::TestValidateIp::test_xxx" -> "TestValidateIp"
        parts = node_id.split("::")
        group = parts[1] if len(parts) >= 3 else "Other"
        name = parts[-1] if parts else node_id

        outcome = t.get("outcome", "unknown")
        duration = round(t.get("duration", 0), 4)

        message = ""
        call_info = t.get("call", {})
        if outcome == "failed":
            crash = call_info.get("crash", {})
            longrepr = call_info.get("longrepr", "")
            message = crash.get("message", "") or (longrepr[:500] if isinstance(longrepr, str) else "")

        tests_out.append({
            "name": name,
            "group": group,
            "passed": outcome == "passed",
            "outcome": outcome,
            "duration": duration,
            "message": message,
        })

    summary_raw = report.get("summary", {})
    summary = {
        "total": summary_raw.get("total", 0),
        "passed": summary_raw.get("passed", 0),
        "failed": summary_raw.get("failed", 0),
        "error": summary_raw.get("error", 0),
        "skipped": summary_raw.get("skipped", 0),
        "duration": round(report.get("duration", 0), 3),
    }

    all_passed = summary.get("failed", 0) == 0 and summary.get("error", 0) == 0
    return JSONResponse({"ok": all_passed, "tests": tests_out, "summary": summary})


@app.get("/api/health")
def api_health() -> JSONResponse:
    return JSONResponse({"ok": True})
