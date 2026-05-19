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

from fastapi import FastAPI, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from web_controller.db import clear_runs, get_run, init_db, list_runs, record_run
from web_controller.k8s_chaos import (
    chaos_clear_cmds,
    chaos_status_cmds,
    container_kill_cmds,
    cpu_stress_cmds,
    k8s_demo_delete_cmds,
    k8s_demo_deploy_cmds,
    k8s_status_cmds,
    memory_stress_cmds,
    network_delay_cmds,
    network_loss_cmds,
    pod_kill_cmds,
)

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_CONFIG_PATH = BASE_DIR / "config.json"
CONFIG_ENV = "FI_CONTROLLER_CONFIG"
REPO_ROOT = BASE_DIR.parent
VM_DIR = REPO_ROOT / "vm_injection"
VM_LOG_DIR = REPO_ROOT / ".vm_logs"
VM_NAME_PREFIXES = ("alpine_", "ubuntu_", "kvm_", "vm_")

app = FastAPI(title="云平台故障注入工具", version="0.4")

static_dir = BASE_DIR / "static"
app.mount("/static", StaticFiles(directory=static_dir), name="static")


def persist_history_safely(**kwargs: Any) -> Optional[int]:
    try:
        return record_run(**kwargs)
    except Exception as exc:
        print(f"[history-db] persist failed: {exc}")
        return None


class ActionRequest(BaseModel):
    action: str
    params: Optional[Dict[str, Any]] = None
    tests: Optional[Dict[str, Any]] = None


GROUPS = [
    {
        "key": "k8s",
        "title": "K8s 状态概览",
        "desc": "k3s 节点、Pod 与 Chaos Mesh 实验状态查看。",
    },
    {
        "key": "chaos_pod",
        "title": "Pod 混沌实验",
        "desc": "通过 Chaos Mesh 注入 Pod Kill、容器 Kill 等故障。",
    },
    {
        "key": "chaos_network",
        "title": "网络混沌实验",
        "desc": "通过 NetworkChaos 注入延迟、丢包等网络异常。",
    },
    {
        "key": "chaos_resource",
        "title": "资源混沌实验",
        "desc": "通过 StressChaos 注入 CPU 与内存压力。",
    },
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
    {
        "key": "cloudstack",
        "title": "CloudStack 注入",
        "desc": "CloudStack 服务管理与故障注入。",
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


def get_worker_nodes(cfg: Dict[str, Any]) -> List[Dict[str, Any]]:
    return [
        node
        for node in get_nodes(cfg)
        if node.get("role") != "master" and node.get("name") != "master"
    ]


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


def _normalize_qemu_vm_name(name: str) -> str:
    value = str(name or "").strip().strip("\"'")
    if value.startswith("guest="):
        value = value[6:]
    value = value.split(",", 1)[0].strip().strip("\"'")
    for prefix in VM_NAME_PREFIXES:
        if value.startswith(prefix) and len(value) > len(prefix):
            value = value[len(prefix):]
            break
    return value


def _is_qemu_args(args: str) -> bool:
    return "qemu-system" in args or "qemu-kvm" in args


def _extract_qemu_vm_name(args: str) -> str:
    raw = str(args or "")
    match = re.search(r"(?:^|\s)-name(?:\s+|=)(\"[^\"]+\"|'[^']+'|\S+)", raw)
    if match:
        return _normalize_qemu_vm_name(match.group(1))

    for prefix in ("node_", "ubuntu_", "alpine_", "kvm_", "vm_"):
        match = re.search(rf"(?:^|[/\s=]){re.escape(prefix)}([A-Za-z0-9_-]+)(?:\.qcow2|[,/\s]|$)", raw)
        if match:
            value = match.group(1) if prefix == "node_" else prefix + match.group(1)
            return _normalize_qemu_vm_name(value)

    return ""


def _qemu_args_match_node(args: str, node: str) -> bool:
    if not _is_qemu_args(args):
        return False
    return _extract_qemu_vm_name(args) == _normalize_qemu_vm_name(node)


def _iter_qemu_args() -> List[str]:
    proc_dir = Path("/proc")
    results: List[str] = []
    if proc_dir.exists():
        for entry in proc_dir.iterdir():
            if not entry.name.isdigit():
                continue
            try:
                raw = (entry / "cmdline").read_bytes()
            except (OSError, PermissionError):
                continue
            if not raw:
                continue
            args = raw.replace(b"\0", b" ").decode(errors="ignore").strip()
            if _is_qemu_args(args):
                results.append(args)
        if results:
            return results

    return [line for line in _ps_aux().splitlines() if _is_qemu_args(line)]


def _is_vm_running(node: str) -> bool:
    return any(_qemu_args_match_node(args, node) for args in _iter_qemu_args())


def validate_kvm_target(cfg: Dict[str, Any], value: str) -> bool:
    target = str(value or "").strip()
    if not target:
        return False
    if target.isdigit():
        return True
    if validate_target(cfg, target):
        return True
    normalized = _normalize_qemu_vm_name(target)
    return normalized in {str(n.get("name", "")) for n in get_nodes(cfg)}



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
    cs_cfg = cfg.get("cloudstack", {})
    k8s_cfg = cfg.get("kubernetes", {})
    vm_base_dir = vm_cfg.get("base_dir", "")
    kvm_base_dir = kvm_cfg.get("base_dir", "")
    kubectl_cmd = str(k8s_cfg.get("kubectl", "kubectl"))
    kubeconfig = str(k8s_cfg.get("kubeconfig", ""))
    kubectl = f"KUBECONFIG={shlex.quote(kubeconfig)} {kubectl_cmd}" if kubeconfig else kubectl_cmd

    return {
        "hadoop_home": hadoop_home,
        "hadoop_sbin": f"{hadoop_home}/sbin",
        "hadoop_bin": f"{hadoop_home}/bin",
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
        "cloudstack_injector": cs_cfg.get("injector", ""),
        "kubectl_cmd": kubectl_cmd,
        "kubeconfig": kubeconfig,
        "kubectl": kubectl,
        "k8s_default_namespace": k8s_cfg.get("default_namespace", "default"),
        "chaos_namespace": k8s_cfg.get("chaos_namespace", "chaos-mesh"),
        "k8s_demo_image": k8s_cfg.get("demo_image", "nginx"),
        "k8s_demo_replicas": k8s_cfg.get("demo_replicas", 2),
        "k8s_probe_image": k8s_cfg.get("probe_image", "busybox:1.36"),
    }


def _build_hadoop_daemon_cmd(
    ctx: Dict[str, Any],
    tool: str,
    daemon: str,
    action: str,
    log_name: str,
    process_name: str = "",
) -> List[str]:
    tool_path = f"{ctx.get('hadoop_bin', '/opt/hadoop/bin')}/{tool}"
    log_path = f"/tmp/fi_{log_name}"
    shell_cmd = (
        ". /etc/profile >/dev/null 2>&1; "
        f"{tool_path} --daemon {action} {daemon} >{shlex.quote(log_path)} 2>&1"
    )
    if action == "start" and process_name:
        shell_cmd += f"; jps 2>/dev/null | grep -q {shlex.quote(process_name)}"
    return ["/bin/sh", "-lc", shell_cmd]


def _hadoop_daemon_step(
    node: Dict[str, Any],
    ctx: Dict[str, Any],
    tool: str,
    daemon: str,
    action: str,
    log_name: str,
    process_name: str,
) -> Dict[str, Any]:
    return {
        "node": node,
        "cmd": _build_hadoop_daemon_cmd(ctx, tool, daemon, action, log_name, process_name),
    }


def _build_hadoop_start_plan(cfg: Dict[str, Any], ctx: Dict[str, Any]) -> List[Dict[str, Any]]:
    master = get_master_node(cfg)
    workers = get_worker_nodes(cfg)
    plan: List[Dict[str, Any]] = [
        _hadoop_daemon_step(master, ctx, "hdfs", "namenode", "start", "nn.log", "NameNode"),
        _hadoop_daemon_step(master, ctx, "hdfs", "secondarynamenode", "start", "snn.log", "SecondaryNameNode"),
    ]
    plan.extend(
        _hadoop_daemon_step(node, ctx, "hdfs", "datanode", "start", f"{node.get('name', 'worker')}_dn.log", "DataNode")
        for node in workers
    )
    plan.append(_hadoop_daemon_step(master, ctx, "yarn", "resourcemanager", "start", "rm.log", "ResourceManager"))
    plan.extend(
        _hadoop_daemon_step(node, ctx, "yarn", "nodemanager", "start", f"{node.get('name', 'worker')}_nm.log", "NodeManager")
        for node in workers
    )
    return plan


def _build_hadoop_stop_plan(cfg: Dict[str, Any], ctx: Dict[str, Any]) -> List[Dict[str, Any]]:
    master = get_master_node(cfg)
    workers = get_worker_nodes(cfg)
    plan: List[Dict[str, Any]] = []
    plan.extend(
        _hadoop_daemon_step(node, ctx, "yarn", "nodemanager", "stop", f"{node.get('name', 'worker')}_nm_stop.log", "NodeManager")
        for node in workers
    )
    plan.append(_hadoop_daemon_step(master, ctx, "yarn", "resourcemanager", "stop", "rm_stop.log", "ResourceManager"))
    plan.extend(
        _hadoop_daemon_step(node, ctx, "hdfs", "datanode", "stop", f"{node.get('name', 'worker')}_dn_stop.log", "DataNode")
        for node in workers
    )
    plan.append(_hadoop_daemon_step(master, ctx, "hdfs", "secondarynamenode", "stop", "snn_stop.log", "SecondaryNameNode"))
    plan.append(_hadoop_daemon_step(master, ctx, "hdfs", "namenode", "stop", "nn_stop.log", "NameNode"))
    return plan


def _build_hadoop_restart_plan(cfg: Dict[str, Any], ctx: Dict[str, Any]) -> List[Dict[str, Any]]:
    master = get_master_node(cfg)
    return _build_hadoop_stop_plan(cfg, ctx) + [{"node": master, "cmd": ["/bin/sleep", "3"]}] + _build_hadoop_start_plan(cfg, ctx)


def _build_process_restart_plan(cfg: Dict[str, Any], ctx: Dict[str, Any], component: str) -> List[Dict[str, Any]]:
    master = get_master_node(cfg)
    workers = get_worker_nodes(cfg)
    specs = {
        "nn": ([master], "hdfs", "namenode", "NameNode"),
        "dn": (workers, "hdfs", "datanode", "DataNode"),
        "rm": ([master], "yarn", "resourcemanager", "ResourceManager"),
        "nm": (workers, "yarn", "nodemanager", "NodeManager"),
        "snn": ([master], "hdfs", "secondarynamenode", "SecondaryNameNode"),
    }
    nodes, tool, daemon, process_name = specs.get(component, ([], "", "", ""))
    return [
        _hadoop_daemon_step(node, ctx, tool, daemon, "start", f"{node.get('name', 'node')}_{component}_restart.log", process_name)
        for node in nodes
    ]


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
    if sudo_key == "cloudstack":
        return bool(cfg.get("cloudstack", {}).get("use_sudo", False))
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
    "cs_component": {"ms", "agent", "usage", "mysql"},
    "chaos_mode": {"one", "all"},
    "chaos_kind": {"all", "podchaos", "networkchaos", "stresschaos"},
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
    "replicas": (1, 100),
    "workers": (1, 64),
    "load": (1, 100),
    "memory_mb": (1, 1048576),
}

K8S_SAFE_PARAM_NAMES = {
    "namespace",
    "deployment",
    "label_key",
    "label_value",
    "chaos_name",
    "container_name",
}

K8S_SELECTOR_PARAMS = [
    {"name": "namespace", "label": "命名空间", "type": "text", "default": "default", "required": True},
    {"name": "label_key", "label": "标签键", "type": "text", "default": "app", "required": True},
    {"name": "label_value", "label": "标签值", "type": "text", "default": "nginx-demo", "required": True},
]

K8S_CHAOS_COMMON_PARAMS = [
    {"name": "chaos_name", "label": "实验名称", "type": "text", "required": True},
    {
        "name": "chaos_mode",
        "label": "选择模式",
        "type": "select",
        "options": [
            {"value": "one", "label": "单个 Pod"},
            {"value": "all", "label": "全部匹配 Pod"},
        ],
        "default": "one",
        "required": True,
    },
    *K8S_SELECTOR_PARAMS,
]


ACTIONS: Dict[str, Dict[str, Any]] = {
    "k8s_status": {
        "title": "K8s / Chaos 状态查看",
        "desc": "查看节点、Pod、Chaos Mesh 实验和最近事件。",
        "group": "k8s",
        "scope": "local",
        "params": [
            {"name": "namespace", "label": "命名空间", "type": "text", "default": "default", "required": False},
        ],
        "cmds": k8s_status_cmds,
        "timeout": 30,
    },
    "k8s_demo_deploy": {
        "title": "部署演示应用",
        "desc": "创建或更新 nginx-demo Deployment，用于混沌实验验证。",
        "group": "k8s",
        "scope": "local",
        "params": [
            {"name": "namespace", "label": "命名空间", "type": "text", "default": "default", "required": True},
            {"name": "deployment", "label": "Deployment", "type": "text", "default": "nginx-demo", "required": True},
            {"name": "image", "label": "镜像", "type": "text", "default": "nginx", "required": True},
            {"name": "replicas", "label": "副本数", "type": "number", "default": 2, "required": True},
        ],
        "cmds": k8s_demo_deploy_cmds,
        "timeout": 240,
    },
    "k8s_demo_delete": {
        "title": "删除演示应用",
        "desc": "删除 nginx-demo Deployment。",
        "group": "k8s",
        "scope": "local",
        "params": [
            {"name": "namespace", "label": "命名空间", "type": "text", "default": "default", "required": True},
            {"name": "deployment", "label": "Deployment", "type": "text", "default": "nginx-demo", "required": True},
        ],
        "cmds": k8s_demo_delete_cmds,
        "timeout": 60,
    },
    "k8s_chaos_status": {
        "title": "查看混沌实验",
        "desc": "查看 Chaos Mesh 实验详情和最近事件。",
        "group": "k8s",
        "scope": "local",
        "params": [
            {"name": "namespace", "label": "命名空间", "type": "text", "default": "default", "required": False},
            {"name": "chaos_name", "label": "实验名称", "type": "text", "required": False},
        ],
        "cmds": chaos_status_cmds,
        "timeout": 30,
    },
    "k8s_chaos_clear": {
        "title": "清理混沌实验",
        "desc": "删除指定或全部 Chaos Mesh 实验资源。",
        "group": "k8s",
        "scope": "local",
        "params": [
            {"name": "namespace", "label": "命名空间", "type": "text", "default": "default", "required": True},
            {
                "name": "chaos_kind",
                "label": "实验类型",
                "type": "select",
                "options": [
                    {"value": "all", "label": "全部"},
                    {"value": "podchaos", "label": "PodChaos"},
                    {"value": "networkchaos", "label": "NetworkChaos"},
                    {"value": "stresschaos", "label": "StressChaos"},
                ],
                "default": "all",
                "required": True,
            },
            {"name": "chaos_name", "label": "实验名称", "type": "text", "required": False},
        ],
        "cmds": chaos_clear_cmds,
        "timeout": 60,
    },
    "k8s_pod_kill": {
        "title": "Pod Kill",
        "desc": "通过 Chaos Mesh 杀死一个或多个匹配 Pod。",
        "group": "chaos_pod",
        "scope": "local",
        "params": [
            {"name": "chaos_name", "label": "实验名称", "type": "text", "default": "fi-pod-kill", "required": True},
            *K8S_CHAOS_COMMON_PARAMS[1:],
        ],
        "cmds": pod_kill_cmds,
        "timeout": 60,
        "danger": True,
    },
    "k8s_container_kill": {
        "title": "Container Kill",
        "desc": "杀死匹配 Pod 中的容器进程，观察容器重启。",
        "group": "chaos_pod",
        "scope": "local",
        "params": [
            {"name": "chaos_name", "label": "实验名称", "type": "text", "default": "fi-container-kill", "required": True},
            *K8S_CHAOS_COMMON_PARAMS[1:],
            {"name": "container_name", "label": "容器名", "type": "text", "default": "nginx", "required": False},
        ],
        "cmds": container_kill_cmds,
        "timeout": 60,
        "danger": True,
    },
    "k8s_network_delay": {
        "title": "网络延迟",
        "desc": "通过 NetworkChaos 为匹配 Pod 注入延迟、抖动和相关性。",
        "group": "chaos_network",
        "scope": "local",
        "params": [
            {"name": "chaos_name", "label": "实验名称", "type": "text", "default": "fi-network-delay", "required": True},
            {
                "name": "chaos_mode",
                "label": "选择模式",
                "type": "select",
                "options": [
                    {"value": "all", "label": "全部匹配 Pod"},
                    {"value": "one", "label": "单个 Pod"},
                ],
                "default": "all",
                "required": True,
            },
            *K8S_SELECTOR_PARAMS,
            {"name": "ms", "label": "延迟 (ms)", "type": "number", "default": 800, "required": True},
            {"name": "jitter", "label": "抖动 (ms)", "type": "number", "default": 100, "required": True},
            {"name": "correlation", "label": "相关性 (%)", "type": "number", "default": 25, "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 60, "required": True},
        ],
        "cmds": network_delay_cmds,
        "timeout": 60,
        "danger": True,
    },
    "k8s_network_loss": {
        "title": "网络丢包",
        "desc": "通过 NetworkChaos 为匹配 Pod 注入丢包。",
        "group": "chaos_network",
        "scope": "local",
        "params": [
            {"name": "chaos_name", "label": "实验名称", "type": "text", "default": "fi-network-loss", "required": True},
            {
                "name": "chaos_mode",
                "label": "选择模式",
                "type": "select",
                "options": [
                    {"value": "all", "label": "全部匹配 Pod"},
                    {"value": "one", "label": "单个 Pod"},
                ],
                "default": "all",
                "required": True,
            },
            *K8S_SELECTOR_PARAMS,
            {"name": "percent", "label": "丢包率 (%)", "type": "number", "default": 50, "required": True},
            {"name": "correlation", "label": "相关性 (%)", "type": "number", "default": 25, "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 60, "required": True},
        ],
        "cmds": network_loss_cmds,
        "timeout": 60,
        "danger": True,
    },
    "k8s_cpu_stress": {
        "title": "CPU 压力",
        "desc": "通过 StressChaos 对匹配 Pod 注入 CPU 压力。",
        "group": "chaos_resource",
        "scope": "local",
        "params": [
            {"name": "chaos_name", "label": "实验名称", "type": "text", "default": "fi-cpu-stress", "required": True},
            *K8S_CHAOS_COMMON_PARAMS[1:],
            {"name": "workers", "label": "工作线程", "type": "number", "default": 2, "required": True},
            {"name": "load", "label": "CPU 负载 (%)", "type": "number", "default": 80, "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 30, "required": True},
        ],
        "cmds": cpu_stress_cmds,
        "timeout": 60,
        "danger": True,
    },
    "k8s_memory_stress": {
        "title": "内存压力",
        "desc": "通过 StressChaos 对匹配 Pod 注入内存压力。",
        "group": "chaos_resource",
        "scope": "local",
        "params": [
            {"name": "chaos_name", "label": "实验名称", "type": "text", "default": "fi-memory-stress", "required": True},
            *K8S_CHAOS_COMMON_PARAMS[1:],
            {"name": "workers", "label": "工作线程", "type": "number", "default": 1, "required": True},
            {"name": "memory_mb", "label": "内存 (MB)", "type": "number", "default": 256, "required": True},
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 30, "required": True},
        ],
        "cmds": memory_stress_cmds,
        "timeout": 60,
        "danger": True,
    },
    "cluster_status": {
        "title": "节点进程状态 (jps)",
        "desc": "在所有节点上执行 jps，查看 Hadoop 进程状态。",
        "group": "cluster",
        "scope": "all",
        "params": [],
        "cmds": lambda ctx, params: [["/bin/sh", "-lc", "jps 2>/dev/null || true"]],
        "sudo": False,
    },
    "hadoop_start": {
        "title": "启动 Hadoop",
        "desc": "依次启动 HDFS 与 YARN 服务。",
        "group": "cluster",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [
            ["/bin/sh", "-lc", f". /etc/profile >/dev/null 2>&1; test -x {ctx['hadoop_sbin']}/start-dfs.sh; test -x {ctx['hadoop_sbin']}/start-yarn.sh; {ctx['hadoop_sbin']}/start-dfs.sh >/tmp/fi_start_dfs.log 2>&1; {ctx['hadoop_sbin']}/start-yarn.sh >/tmp/fi_start_yarn.log 2>&1"],
            ["/bin/sh", "-lc", "jps 2>/dev/null | grep -q NameNode && jps 2>/dev/null | grep -q SecondaryNameNode && jps 2>/dev/null | grep -q ResourceManager && for n in slave1 slave2; do ssh -o StrictHostKeyChecking=no \"$n\" \"jps 2>/dev/null | grep -q DataNode\" || exit 1; done && for n in slave1 slave2; do ssh -o StrictHostKeyChecking=no \"$n\" \"jps 2>/dev/null | grep -q NodeManager\" || exit 1; done"],
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
            ["/bin/sh", "-lc", f"{ctx['hadoop_sbin']}/stop-yarn.sh >/tmp/fi_stop_yarn.log 2>&1 || true"],
            ["/bin/sh", "-lc", f"{ctx['hadoop_sbin']}/stop-dfs.sh >/tmp/fi_stop_dfs.log 2>&1 || true"],
            ["/bin/sleep", "3"],
            ["/bin/sh", "-lc", f". /etc/profile >/dev/null 2>&1; test -x {ctx['hadoop_sbin']}/start-dfs.sh; test -x {ctx['hadoop_sbin']}/start-yarn.sh; {ctx['hadoop_sbin']}/start-dfs.sh >/tmp/fi_start_dfs.log 2>&1; {ctx['hadoop_sbin']}/start-yarn.sh >/tmp/fi_start_yarn.log 2>&1"],
            ["/bin/sh", "-lc", "jps 2>/dev/null | grep -q NameNode && jps 2>/dev/null | grep -q SecondaryNameNode && jps 2>/dev/null | grep -q ResourceManager && for n in slave1 slave2; do ssh -o StrictHostKeyChecking=no \"$n\" \"jps 2>/dev/null | grep -q DataNode\" || exit 1; done && for n in slave1 slave2; do ssh -o StrictHostKeyChecking=no \"$n\" \"jps 2>/dev/null | grep -q NodeManager\" || exit 1; done"],
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
        "timeout": 180,
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
        "cmds": lambda ctx, params: _build_vm_process_cmd(ctx, params),
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
        "timeout": 20,
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
        "cmds": lambda ctx, params: _build_vm_cpu_start_cmd(ctx, params),
        "tool": "vm_cpu_injector",
        "sudo": "vm",
        "danger": True,
    },
    "vm_cpu_clear": {
        "title": "清理 VM CPU 压力",
        "desc": "停止后台 cpu_injector 压力进程。",
        "group": "vm",
        "scope": "local",
        "params": [],
        "cmds": lambda ctx, params: _build_vm_cpu_clear_cmd(ctx, params),
        "sudo": "vm",
    },
    "vm_mem_leak": {
        "title": "VM 内存泄漏",
        "desc": "使用 mem_leak 大量占用内存，模拟 OOM。",
        "group": "vm",
        "scope": "local",
        "timeout": 20,
        "params": [
            {"name": "size_mb", "label": "占用内存 (MB)", "type": "number", "default": 512, "required": True},
        ],
        "cmds": lambda ctx, params: _build_vm_mem_leak_start_cmd(ctx, params),
        "tool": "vm_mem_leak",
        "sudo": False,
        "danger": True,
    },
    "vm_mem_leak_clear": {
        "title": "清理 VM 内存泄漏",
        "desc": "停止后台 mem_leak 压力进程。",
        "group": "vm",
        "scope": "local",
        "params": [],
        "cmds": lambda ctx, params: _build_vm_mem_leak_clear_cmd(ctx, params),
        "sudo": False,
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
            {
                "name": "target",
                "label": "目标虚拟机/PID",
                "type": "text",
                "default": "master",
                "required": True,
                "placeholder": "master / slave1 / slave2 / PID",
                "kvm_target": True,
            },
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
            {
                "name": "target",
                "label": "目标虚拟机/PID",
                "type": "text",
                "default": "master",
                "required": True,
                "placeholder": "master / slave1 / slave2 / PID",
                "kvm_target": True,
            },
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
                str(params["target"]),
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
            {
                "name": "target",
                "label": "目标虚拟机/PID",
                "type": "text",
                "default": "master",
                "required": True,
                "placeholder": "master / slave1 / slave2 / PID",
                "kvm_target": True,
            },
            {"name": "ms", "label": "延迟 (毫秒)", "type": "number", "default": 100, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["kvm_injector"], "perf-delay", str(params["target"]), str(params["ms"]) ]],
        "tool": "kvm_injector",
        "sudo": "kvm",
        "danger": True,
    },
    "kvm_perf_stress": {
        "title": "KVM 性能故障 - CPU 压力",
        "desc": "在目标虚拟机内注入 CPU 高负载，模拟客户机资源争抢。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {
                "name": "target",
                "label": "目标虚拟机/PID",
                "type": "text",
                "default": "master",
                "required": True,
                "placeholder": "master / slave1 / slave2 / PID",
                "kvm_target": True,
            },
            {"name": "duration", "label": "持续时间 (秒)", "type": "number", "default": 10, "required": True},
            {"name": "threads", "label": "线程数 (0 按 1 处理)", "type": "number", "default": 1, "required": False},
            {"name": "kvm_guest_user", "label": "VM 用户名", "type": "text", "default": "ubuntu", "required": True},
            {"name": "kvm_guest_password", "label": "VM 密码", "type": "text", "default": "123456", "required": True},
        ],
        "cmds": lambda ctx, params: _build_kvm_guest_cpu_stress_cmd(params),
        "sudo": False,
        "danger": True,
    },
    "kvm_guest_cpu_clear": {
        "title": "清理 KVM VM 内 CPU 压力",
        "desc": "停止目标虚拟机内由测试启动的 CPU 压力进程。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {
                "name": "target",
                "label": "目标虚拟机",
                "type": "text",
                "default": "master",
                "required": True,
                "placeholder": "master / slave1 / slave2",
                "kvm_target": True,
            },
            {"name": "kvm_guest_user", "label": "VM 用户名", "type": "text", "default": "ubuntu", "required": True},
            {"name": "kvm_guest_password", "label": "VM 密码", "type": "text", "default": "123456", "required": True},
        ],
        "cmds": lambda ctx, params: _build_kvm_guest_cpu_clear_cmd(params),
        "sudo": False,
    },
    "kvm_perf_clear": {
        "title": "KVM 性能故障 - 清理",
        "desc": "清理性能限制。",
        "group": "kvm",
        "scope": "local",
        "params": [
            {
                "name": "target",
                "label": "目标虚拟机/PID",
                "type": "text",
                "default": "master",
                "required": True,
                "placeholder": "master / slave1 / slave2 / PID",
                "kvm_target": True,
            },
        ],
        "cmds": lambda ctx, params: [[ctx["kvm_injector"], "perf-clear", str(params["target"]) ]],
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
    "process_restart": {
        "title": "进程重启",
        "desc": "重启指定 Hadoop 组件守护进程。",
        "group": "process",
        "scope": "master",
        "params": [
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
                ],
                "default": "nn",
                "required": True,
            },
        ],
        "cmds": lambda ctx, params: _build_process_restart_cmds(ctx, params),
    },
    "cloudstack_list": {
        "title": "CloudStack 服务状态",
        "desc": "查看 CloudStack 服务与关键端口状态。",
        "group": "cloudstack",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["cloudstack_injector"], "list"]],
        "tool": "cloudstack_injector",
        "sudo": "cloudstack",
    },
    "cloudstack_process": {
        "title": "CloudStack 进程控制",
        "desc": "挂起/恢复 CloudStack 核心组件进程。",
        "group": "cloudstack",
        "scope": "master",
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
            {
                "name": "op",
                "label": "操作",
                "type": "select",
                "options": [
                    {"value": "hang", "label": "挂起 (hang)"},
                    {"value": "resume", "label": "恢复 (resume)"},
                    {"value": "crash", "label": "崩溃 (crash)"},
                ],
                "default": "hang",
                "required": True,
            },
        ],
        "cmds": lambda ctx, params: [[ctx["cloudstack_injector"], params["op"], params["cs_component"]]],
        "tool": "cloudstack_injector",
        "sudo": "cloudstack",
        "danger": True,
    },
    "cloudstack_api_delay": {
        "title": "CloudStack API 延迟",
        "desc": "注入 CloudStack API 延迟。",
        "group": "cloudstack",
        "scope": "master",
        "params": [
            {"name": "ms", "label": "延迟 (ms)", "type": "number", "default": 1000, "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["cloudstack_injector"], "api-delay", str(params["ms"])]],
        "tool": "cloudstack_injector",
        "sudo": "cloudstack",
        "danger": True,
    },
    "cloudstack_api_delay_clear": {
        "title": "清理 API 延迟",
        "desc": "清理 CloudStack API 延迟规则。",
        "group": "cloudstack",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["cloudstack_injector"], "api-delay-clear"]],
        "tool": "cloudstack_injector",
        "sudo": "cloudstack",
    },
    "cloudstack_network": {
        "title": "CloudStack 网络隔离",
        "desc": "隔离指定节点的网络连接。",
        "group": "cloudstack",
        "scope": "master",
        "params": [
            {"name": "target", "label": "目标节点/IP", "type": "node", "required": True},
        ],
        "cmds": lambda ctx, params: [[ctx["cloudstack_injector"], "isolate", params["target"]]],
        "tool": "cloudstack_injector",
        "sudo": "cloudstack",
        "danger": True,
    },
    "cloudstack_network_clear": {
        "title": "清理网络隔离",
        "desc": "清理 CloudStack 网络隔离规则。",
        "group": "cloudstack",
        "scope": "master",
        "params": [],
        "cmds": lambda ctx, params: [[ctx["cloudstack_injector"], "isolate-clear"]],
        "tool": "cloudstack_injector",
        "sudo": "cloudstack",
    },
}


def _build_vm_process_cmd(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    process = str(params["process"]).strip()
    action_code = {"crash": "1", "hang": "2", "resume": "3"}[params["proc_action"]]

    if process != "fi_vm_target_process":
        return [[ctx["vm_process_injector"], process, action_code]]

    tool = shlex.quote(ctx["vm_process_injector"])
    cmd = (
        "pidfile=/tmp/fi_vm_target_process.pid; "
        "log=/tmp/fi_vm_target_process.log; "
        "name=fi_vm_target_process; "
        "if [ ! -s \"$pidfile\" ] || ! kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then "
        "rm -f \"$pidfile\"; "
        "nohup /bin/bash -c 'exec -a \"$1\" sleep 300' _ \"$name\" > \"$log\" 2>&1 & "
        "pid=$!; echo \"$pid\" > \"$pidfile\"; sleep 0.2; "
        "fi; "
        "target=\"$name\"; "
        "if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then target=$(cat \"$pidfile\"); fi; "
        f"{tool} \"$target\" {action_code}; "
        "rc=$?; "
        f"if [ \"$rc\" -eq 0 ] && [ \"{action_code}\" = \"1\" ]; then rm -f \"$pidfile\"; fi; "
        "exit \"$rc\""
    )
    return [["/bin/sh", "-lc", cmd]]


def _build_kvm_guest_ssh_cmd(params: Dict[str, Any], remote_script: str) -> List[List[str]]:
    target = str(params.get("target", "master")).strip()
    user = str(params.get("kvm_guest_user", "ubuntu")).strip() or "ubuntu"
    password = str(params.get("kvm_guest_password", "123456"))
    cmd = (
        f"target={shlex.quote(target)}; "
        "case \"$target\" in "
        "master) port=2220;; "
        "slave1) port=2221;; "
        "slave2) port=2222;; "
        "*) echo \"unknown target: $target\"; exit 1;; "
        "esac; "
        f"user={shlex.quote(user)}; "
        f"password={shlex.quote(password)}; "
        "command -v sshpass >/dev/null || (echo 'sshpass_missing: 请先安装 sshpass'; exit 1); "
        "sshpass -p \"$password\" ssh "
        "-o StrictHostKeyChecking=no "
        "-o UserKnownHostsFile=/dev/null "
        "-o ConnectTimeout=5 "
        "-o LogLevel=ERROR "
        "-p \"$port\" \"$user@127.0.0.1\" "
        f"/bin/bash -lc {shlex.quote(remote_script)}"
    )
    return [["/bin/sh", "-lc", cmd]]


def _build_kvm_guest_cpu_stress_cmd(params: Dict[str, Any]) -> List[List[str]]:
    duration = int(params["duration"])
    threads = int(params.get("threads", 1) or 1)
    if threads <= 0:
        threads = 1
    remote_script = (
        "pidfile=/tmp/fi_kvm_guest_cpu_stress.pid; "
        "log=/tmp/fi_kvm_guest_cpu_stress.log; "
        "if [ -s \"$pidfile\" ]; then "
        "for pid in $(cat \"$pidfile\"); do kill \"$pid\" 2>/dev/null || true; done; "
        "rm -f \"$pidfile\"; "
        "fi; "
        ": > \"$log\"; "
        f"duration={duration}; threads={threads}; "
        "pids=''; i=1; "
        "while [ \"$i\" -le \"$threads\" ]; do "
        "nohup /bin/bash -c 'duration=\"$1\"; end=$((SECONDS + duration)); while [ \"$SECONDS\" -lt \"$end\" ]; do :; done' _ \"$duration\" >> \"$log\" 2>&1 & "
        "pids=\"$pids $!\"; i=$((i+1)); "
        "done; "
        "echo \"$pids\" > \"$pidfile\"; "
        f"echo '[guest_cpu_stress] started target={shlex.quote(str(params.get('target', 'master')))} duration={duration} threads={threads} pids:' \"$pids\""
    )
    return _build_kvm_guest_ssh_cmd(params, remote_script)


def _build_kvm_guest_cpu_clear_cmd(params: Dict[str, Any]) -> List[List[str]]:
    remote_script = (
        "pidfile=/tmp/fi_kvm_guest_cpu_stress.pid; "
        "log=/tmp/fi_kvm_guest_cpu_stress.log; "
        "if [ -s \"$pidfile\" ]; then "
        "pids=$(cat \"$pidfile\"); "
        "for pid in $pids; do kill \"$pid\" 2>/dev/null || true; done; "
        "sleep 0.5; "
        "for pid in $pids; do kill -9 \"$pid\" 2>/dev/null || true; done; "
        "rm -f \"$pidfile\"; "
        "echo '[guest_cpu_stress] stopped pids:' \"$pids\"; "
        "else "
        "echo '[guest_cpu_stress] no running process'; "
        "fi; "
        "tail -20 \"$log\" 2>/dev/null || true"
    )
    return _build_kvm_guest_ssh_cmd(params, remote_script)


def _build_vm_cpu_start_cmd(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    target_pid = int(params.get("pid", 0) or 0)
    duration = int(params["duration"])
    threads = int(params.get("threads", 0) or 0)
    mode = int(params.get("cpu_mode", 2) or 2)
    tool = shlex.quote(ctx["vm_cpu_injector"])
    cmd = (
        "pidfile=/tmp/fi_vm_cpu_stress.pid; "
        "log=/tmp/fi_vm_cpu_stress.log; "
        "if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then "
        "oldpid=$(cat \"$pidfile\"); kill \"$oldpid\" 2>/dev/null || true; sleep 1; "
        "fi; "
        "rm -f \"$pidfile\" \"$log\"; "
        f"nohup {tool} {target_pid} {duration} {threads} {mode} > \"$log\" 2>&1 & "
        "pid=$!; echo \"$pid\" > \"$pidfile\"; "
        "sleep 1; "
        "if kill -0 \"$pid\" 2>/dev/null; then "
        f"echo '[cpu_stress] started PID:' \"$pid\" 'duration:' {duration} 'threads:' {threads} 'mode:' {mode}; "
        "tail -30 \"$log\" 2>/dev/null || true; "
        "else "
        "echo '[cpu_stress] failed to start'; "
        "cat \"$log\" 2>/dev/null || true; "
        "rm -f \"$pidfile\"; "
        "exit 1; "
        "fi"
    )
    return [["/bin/sh", "-lc", cmd]]


def _build_vm_cpu_clear_cmd(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    cmd = (
        "pidfile=/tmp/fi_vm_cpu_stress.pid; "
        "log=/tmp/fi_vm_cpu_stress.log; "
        "if [ -s \"$pidfile\" ]; then "
        "pid=$(cat \"$pidfile\"); "
        "if kill -0 \"$pid\" 2>/dev/null; then "
        "kill \"$pid\" 2>/dev/null || true; sleep 1; "
        "if kill -0 \"$pid\" 2>/dev/null; then kill -9 \"$pid\" 2>/dev/null || true; fi; "
        "echo '[cpu_stress] stopped PID:' \"$pid\"; "
        "else "
        "echo '[cpu_stress] pidfile exists but process is not running:' \"$pid\"; "
        "fi; "
        "rm -f \"$pidfile\"; "
        "else "
        "pkill -f '[c]pu_injector ' 2>/dev/null && echo '[cpu_stress] stopped by pattern' || echo '[cpu_stress] no running process'; "
        "fi; "
        "tail -30 \"$log\" 2>/dev/null || true"
    )
    return [["/bin/sh", "-lc", cmd]]


def _build_vm_mem_leak_start_cmd(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    size_mb = int(params["size_mb"])
    tool = shlex.quote(ctx["vm_mem_leak"])
    cmd = (
        "pidfile=/tmp/fi_vm_mem_leak.pid; "
        "log=/tmp/fi_vm_mem_leak.log; "
        "if [ -s \"$pidfile\" ] && kill -0 \"$(cat \"$pidfile\")\" 2>/dev/null; then "
        "oldpid=$(cat \"$pidfile\"); kill \"$oldpid\" 2>/dev/null || true; sleep 1; "
        "fi; "
        "rm -f \"$pidfile\" \"$log\"; "
        f"nohup {tool} 0 {size_mb} > \"$log\" 2>&1 & "
        "pid=$!; echo \"$pid\" > \"$pidfile\"; "
        "sleep 2; "
        "if kill -0 \"$pid\" 2>/dev/null; then "
        f"echo '[mem_leak] started PID:' \"$pid\" 'size_mb:' {size_mb}; "
        "tail -20 \"$log\" 2>/dev/null || true; "
        "else "
        "echo '[mem_leak] failed to start'; "
        "cat \"$log\" 2>/dev/null || true; "
        "rm -f \"$pidfile\"; "
        "exit 1; "
        "fi"
    )
    return [["/bin/sh", "-lc", cmd]]


def _build_vm_mem_leak_clear_cmd(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    cmd = (
        "pidfile=/tmp/fi_vm_mem_leak.pid; "
        "log=/tmp/fi_vm_mem_leak.log; "
        "if [ -s \"$pidfile\" ]; then "
        "pid=$(cat \"$pidfile\"); "
        "if kill -0 \"$pid\" 2>/dev/null; then "
        "kill \"$pid\" 2>/dev/null || true; sleep 1; "
        "if kill -0 \"$pid\" 2>/dev/null; then kill -9 \"$pid\" 2>/dev/null || true; fi; "
        "echo '[mem_leak] stopped PID:' \"$pid\"; "
        "else "
        "echo '[mem_leak] pidfile exists but process is not running:' \"$pid\"; "
        "fi; "
        "rm -f \"$pidfile\"; "
        "else "
        "pkill -f '[m]em_leak 0 ' 2>/dev/null && echo '[mem_leak] stopped by pattern' || echo '[mem_leak] no running process'; "
        "fi; "
        "tail -20 \"$log\" 2>/dev/null || true"
    )
    return [["/bin/sh", "-lc", cmd]]


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

    cmd += [str(params["target"]), params["reg"]]

    if params.get("soft_bit") is not None and soft_type in {"flip", "zero"}:
        cmd.append(str(params["soft_bit"]))

    return [cmd]


def _build_process_restart_cmds(ctx: Dict[str, Any], params: Dict[str, Any]) -> List[List[str]]:
    component = params.get("component", "")
    cmd_map = {
        "nn": ["/bin/sh", "-lc", ". /etc/profile >/dev/null 2>&1; hdfs --daemon start namenode >/tmp/fi_restart_nn.log 2>&1; jps 2>/dev/null | grep -q NameNode"],
        # 对 slave 组件使用 Hadoop 分发脚本，避免依赖硬编码 slave1/slave2 主机名
        "dn": ["/bin/sh", "-lc", f". /etc/profile >/dev/null 2>&1; test -x {ctx['hadoop_sbin']}/start-dfs.sh; {ctx['hadoop_sbin']}/start-dfs.sh >/tmp/fi_restart_dn.log 2>&1; for n in slave1 slave2; do ssh -o StrictHostKeyChecking=no \"$n\" \"jps 2>/dev/null | grep -q DataNode\" || exit 1; done"],
        "rm": ["/bin/sh", "-lc", ". /etc/profile >/dev/null 2>&1; yarn --daemon start resourcemanager >/tmp/fi_restart_rm.log 2>&1; jps 2>/dev/null | grep -q ResourceManager"],
        "nm": ["/bin/sh", "-lc", f". /etc/profile >/dev/null 2>&1; test -x {ctx['hadoop_sbin']}/start-yarn.sh; {ctx['hadoop_sbin']}/start-yarn.sh >/tmp/fi_restart_nm.log 2>&1; for n in slave1 slave2; do ssh -o StrictHostKeyChecking=no \"$n\" \"jps 2>/dev/null | grep -q NodeManager\" || exit 1; done"],
        "snn": ["/bin/sh", "-lc", ". /etc/profile >/dev/null 2>&1; hdfs --daemon start secondarynamenode >/tmp/fi_restart_snn.log 2>&1; jps 2>/dev/null | grep -q SecondaryNameNode"],
    }
    cmd = cmd_map.get(component)
    if cmd:
        return [cmd]
    return [["/bin/echo", f"Unknown component: {component}"]]


@app.get("/")
def index() -> FileResponse:
    return FileResponse(static_dir / "index.html")


@app.get("/history")
def history_page() -> FileResponse:
    return FileResponse(static_dir / "history.html")


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
        check_timeout = check.get("timeout", 15)
        try:
            check_timeout = int(check_timeout)
        except (TypeError, ValueError):
            check_timeout = 15
        if check_timeout <= 0:
            check_timeout = 15

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
            res = run_on_node(cfg, node, cmd, timeout_override=check_timeout)
            res.update({"node": node.get("name"), "host": node.get("host")})
            node_results.append(res)

        results.append({
            "title": title,
            "cmd": rendered,
            "scope": scope,
            "timeout": check_timeout,
            "results": node_results,
            "ok": all(r.get("ok") for r in node_results) if node_results else True,
        })
    return results


@app.post("/api/functest")
def api_functest(req: FuncTestRequest) -> JSONResponse:
    """Run a single functional test scenario with baseline → action → verify."""
    started_at = time.time()
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
    baseline_ok = all(check.get("ok") for check in baseline_results) if baseline_results else True

    # 2. Execute the action
    action_key = scenario["action"]
    action_response = None
    action_ok = False
    skip_action = bool(scenario.get("require_baseline")) and not baseline_ok
    if skip_action:
        action_response = {
            "ok": False,
            "action": action_key,
            "results": [],
            "error": "基线检查未通过，已跳过故障注入动作。",
        }
    elif action_key and action_key in ACTIONS:
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

            # Scenario can override action scope to keep action/verify on the same target.
            scope = scenario.get("action_scope", spec.get("scope", "master"))
            if scope == "all":
                nodes = get_nodes(cfg)
            elif scope == "local":
                nodes = [{"name": "local", "host": "127.0.0.1", "local": True}]
            else:
                nodes = [get_master_node(cfg)]

            use_sudo = resolve_sudo(cfg, spec)
            action_results = []
            action_timeout = scenario.get("action_timeout", spec.get("timeout"))
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
    verify_results = [] if skip_action else _run_check_cmds(
        cfg, scenario.get("verify", []), action_params, ctx,
    )
    verify_ok = all(check.get("ok") for check in verify_results) if verify_results else True
    overall_ok = baseline_ok and action_ok and verify_ok

    # Build response
    payload = {
        "key": test_key,
        "title": scenario["title"],
        "ok": overall_ok,
        "baseline": baseline_results,
        "action": action_response,
        "verify": verify_results,
        "params": action_params,
        "has_cleanup": bool(scenario.get("cleanup")),
        "cleanup_action": scenario.get("cleanup"),
        "cleanup_params": scenario.get("cleanup_params", scenario.get("cleanup_params_override", {})),
    }

    phases: List[Dict[str, Any]] = []
    for check in baseline_results:
        phases.append({"phase": "baseline", "check_title": check.get("title"), "results": check.get("results", [])})
    if action_response:
        phases.append({"phase": "action", "results": action_response.get("results", [])})
    for check in verify_results:
        phases.append({"phase": "verify", "check_title": check.get("title"), "results": check.get("results", [])})

    run_id = persist_history_safely(
        run_type="functest",
        action_key=action_key,
        scenario_key=test_key,
        title=scenario.get("title"),
        params=action_params,
        ok=overall_ok,
        started_at=started_at,
        finished_at=time.time(),
        phases=phases,
    )
    if run_id is not None:
        payload["run_id"] = run_id
    return JSONResponse(payload)


@app.post("/api/functest/cleanup")
def api_functest_cleanup(req: FuncTestRequest) -> JSONResponse:
    """Run cleanup action for one functional test scenario."""
    started_at = time.time()
    test_key = req.key
    user_params = req.params or {}

    if test_key not in FUNC_TESTS_MAP:
        raise HTTPException(status_code=400, detail=f"未知测试: {test_key}")

    scenario = FUNC_TESTS_MAP[test_key]
    cleanup_action = scenario.get("cleanup")
    if not cleanup_action:
        return JSONResponse(
            {
                "ok": False,
                "key": test_key,
                "cleanup_action": None,
                "results": [],
                "error": "该测试没有清理动作",
            }
        )

    # Start from action params + user inputs, then force cleanup-specific params.
    action_params = dict(scenario.get("action_params", {}))
    action_params.update(user_params)
    cleanup_params = dict(action_params)
    cleanup_defaults = scenario.get("cleanup_params", scenario.get("cleanup_params_override", {})) or {}
    cleanup_params.update(cleanup_defaults)

    action_resp = api_action(
        ActionRequest(
            action=cleanup_action,
            params=cleanup_params,
            tests={"kvm": False, "__persist": False},
        )
    )
    payload = json.loads(action_resp.body.decode("utf-8"))

    response_payload = {
        "ok": bool(payload.get("ok")),
        "key": test_key,
        "cleanup_action": cleanup_action,
        "results": payload.get("results", []),
        "error": payload.get("error"),
    }
    run_id = persist_history_safely(
        run_type="cleanup",
        action_key=cleanup_action,
        scenario_key=test_key,
        title=f"{scenario.get('title', test_key)} - 清理",
        params=cleanup_params,
        ok=bool(payload.get("ok")),
        started_at=started_at,
        finished_at=time.time(),
        phases=[{"phase": "cleanup", "results": payload.get("results", [])}],
    )
    if run_id is not None:
        response_payload["run_id"] = run_id
    return JSONResponse(response_payload)


@app.on_event("startup")
def auto_start_vms() -> None:
    init_db()
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


@app.get("/api/history")
def api_history(
    limit: int = Query(50, ge=1, le=500),
    run_type: Optional[str] = Query(None),
) -> JSONResponse:
    """Return persisted fault-injection run history."""
    return JSONResponse({"runs": list_runs(limit=limit, run_type=run_type)})


@app.get("/api/history/{run_id}")
def api_history_detail(run_id: int) -> JSONResponse:
    """Return one persisted run with all command results."""
    run = get_run(run_id)
    if not run:
        raise HTTPException(status_code=404, detail="历史记录不存在")
    return JSONResponse(run)


@app.delete("/api/history")
def api_history_clear() -> JSONResponse:
    """Delete all persisted run history."""
    deleted = clear_runs()
    return JSONResponse({"ok": True, "deleted": deleted})


@app.post("/api/action")
def api_action(req: ActionRequest) -> JSONResponse:
    started_at = time.time()
    cfg = load_config()
    action = req.action
    params = req.params or {}
    test_flags = req.tests or {}
    persist_history = test_flags.get("__persist", True) is not False

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

    target_def = next((p for p in param_defs if p.get("name") == "target"), {})
    if "target" in params:
        target_ok = validate_kvm_target(cfg, params["target"]) if target_def.get("kvm_target") else validate_target(cfg, params["target"])
        if not target_ok:
            raise HTTPException(status_code=400, detail="目标节点无效")

    if "addr" in params and params.get("addr"):
        if not validate_hex(params["addr"]):
            raise HTTPException(status_code=400, detail="地址必须为十六进制")

    if "signature" in params and params.get("signature"):
        if not validate_hex(params["signature"]):
            raise HTTPException(status_code=400, detail="特征值必须为十六进制")

    for name in K8S_SAFE_PARAM_NAMES:
        if name in params and params.get(name):
            value = str(params[name])
            if not re.fullmatch(r"[A-Za-z0-9_.:/@-]{1,253}", value):
                raise HTTPException(status_code=400, detail=f"参数 {name} 包含非法字符")

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
    payload = {"ok": ok, "action": action, "results": results, "tests": tests}
    if persist_history:
        phases = [{"phase": "action", "results": results}]
        for test in tests:
            phases.append(
                {
                    "phase": "auto_test",
                    "check_title": test.get("title"),
                    "results": test.get("results", []),
                }
            )
        run_id = persist_history_safely(
            run_type="action",
            action_key=action,
            title=spec.get("title"),
            params=params,
            ok=ok,
            started_at=started_at,
            finished_at=time.time(),
            phases=phases,
        )
        if run_id is not None:
            payload["run_id"] = run_id
    return JSONResponse(payload)


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
