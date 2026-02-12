"""Comprehensive tests for web_controller/app.py

Every public function and API endpoint is covered. Tests are grouped
by category so the frontend dashboard can display them in sections.

Run with:
    cd web_controller && python -m pytest tests/ -v
"""

from __future__ import annotations

import json
import os
import sys
import textwrap
from pathlib import Path
from typing import Any, Dict, List
from unittest.mock import MagicMock, patch

import pytest

# Ensure the web_controller package is importable
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from web_controller.app import (
    ACTIONS,
    GROUPS,
    NUM_RANGES,
    PARAM_ENUMS,
    ActionRequest,
    _SafeFormat,
    _build_kvm_soft_cmd,
    _build_vm_mem_inject_cmd,
    _build_vm_reg_inject_cmd,
    build_context,
    build_ssh_command,
    find_node_by_value,
    get_master_node,
    get_nodes,
    is_local_node,
    maybe_sudo,
    normalize_cmds,
    render_template,
    resolve_sudo,
    resolve_test_nodes,
    resolve_test_sudo,
    sanitize_output,
    truncate_text,
    validate_hex,
    validate_ip,
    validate_target,
)


# ====================================================================
# 1. 验证工具函数 — validate_ip / validate_hex / validate_target
# ====================================================================


class TestValidateIp:
    """validate_ip: 合法/非法 IPv4 地址判断。"""

    def test_valid_ipv4(self):
        assert validate_ip("192.168.1.1") is True

    def test_valid_ipv4_zeros(self):
        assert validate_ip("0.0.0.0") is True

    def test_valid_ipv4_max(self):
        assert validate_ip("255.255.255.255") is True

    def test_invalid_ipv4_out_of_range(self):
        assert validate_ip("256.0.0.1") is False

    def test_invalid_ipv4_too_few_octets(self):
        assert validate_ip("192.168.1") is False

    def test_invalid_ipv4_letters(self):
        assert validate_ip("abc.def.ghi.jkl") is False

    def test_empty_string(self):
        assert validate_ip("") is False

    def test_localhost(self):
        assert validate_ip("127.0.0.1") is True


class TestValidateHex:
    """validate_hex: 十六进制字符串合法性。"""

    def test_plain_hex(self):
        assert validate_hex("deadbeef") is True

    def test_hex_with_prefix(self):
        assert validate_hex("0xCAFE") is True

    def test_invalid_hex(self):
        assert validate_hex("xyz123") is False

    def test_empty_string(self):
        assert validate_hex("") is False

    def test_single_digit(self):
        assert validate_hex("0") is True

    def test_mixed_case(self):
        assert validate_hex("AbCd01") is True


class TestValidateTarget:
    """validate_target: 节点名或 IP。"""

    def test_known_node_name(self, mock_config):
        assert validate_target(mock_config, "master") is True

    def test_known_slave(self, mock_config):
        assert validate_target(mock_config, "slave1") is True

    def test_valid_ip_not_in_config(self, mock_config):
        assert validate_target(mock_config, "10.0.0.1") is True

    def test_unknown_name(self, mock_config):
        assert validate_target(mock_config, "nonexistent") is False

    def test_empty_value(self, mock_config):
        assert validate_target(mock_config, "") is False


# ====================================================================
# 2. 文本处理 — sanitize_output / truncate_text
# ====================================================================


class TestSanitizeOutput:
    """sanitize_output: 清理控制台输出中的 ANSI 转义序列。"""

    def test_strips_ansi_codes(self):
        raw = "\x1b[31mERROR\x1b[0m: something"
        assert sanitize_output(raw) == "ERROR: something"

    def test_normalizes_crlf(self):
        assert sanitize_output("a\r\nb") == "a\nb"

    def test_normalizes_cr(self):
        assert sanitize_output("a\rb") == "a\nb"

    def test_empty_returns_empty(self):
        assert sanitize_output("") == ""

    def test_none_returns_empty(self):
        assert sanitize_output(None) == ""


class TestTruncateText:
    """truncate_text: 按行数/字符数截断长文本。"""

    def test_no_truncation_needed(self):
        result = truncate_text("hello\nworld", 10, 1000)
        assert result["truncated"] is False
        assert "hello" in result["text"]

    def test_line_truncation(self):
        text = "\n".join(f"line{i}" for i in range(100))
        result = truncate_text(text, 5, 0)
        assert result["truncated"] is True
        assert result["total_lines"] == 100

    def test_char_truncation(self):
        text = "x" * 200
        result = truncate_text(text, 0, 50)
        assert result["truncated"] is True
        assert len(result["text"]) == 50

    def test_preserves_total_chars(self):
        text = "abc\ndef"
        result = truncate_text(text, 0, 0)
        assert result["total_chars"] == 7

    def test_empty_text(self):
        result = truncate_text("", 10, 100)
        assert result["truncated"] is False
        assert result["text"] == ""


# ====================================================================
# 3. 配置读取 — load_config / get_nodes / get_master_node / is_local_node
# ====================================================================


class TestGetNodes:
    """get_nodes: 从配置提取节点列表。"""

    def test_returns_all_nodes(self, mock_config):
        nodes = get_nodes(mock_config)
        assert len(nodes) == 3

    def test_empty_config_returns_empty(self):
        assert get_nodes({}) == []


class TestGetMasterNode:
    """get_master_node: 定位 master 节点。"""

    def test_finds_master_by_role(self, mock_config):
        master = get_master_node(mock_config)
        assert master["name"] == "master"
        assert master["role"] == "master"

    def test_raises_when_no_master(self):
        cfg = {"nodes": [{"name": "slave1", "host": "10.0.0.1", "role": "slave"}]}
        with pytest.raises(RuntimeError, match="Master node not found"):
            get_master_node(cfg)


class TestIsLocalNode:
    """is_local_node: 判断节点是否为本地执行。"""

    def test_explicit_local_true(self):
        assert is_local_node({"local": True, "host": "10.0.0.1"}) is True

    def test_explicit_local_false(self):
        assert is_local_node({"local": False, "host": "127.0.0.1"}) is False

    def test_localhost_without_port(self):
        assert is_local_node({"host": "127.0.0.1"}) is True

    def test_localhost_with_port(self):
        assert is_local_node({"host": "127.0.0.1", "port": 2220}) is False

    def test_remote_host(self):
        assert is_local_node({"host": "192.168.1.100"}) is False

    def test_ipv6_localhost(self):
        assert is_local_node({"host": "::1"}) is True

    def test_localhost_string(self):
        assert is_local_node({"host": "localhost"}) is True


class TestLoadConfig:
    """load_config: 读取 JSON 配置文件。"""

    def test_loads_from_env(self, mock_config_path, monkeypatch):
        monkeypatch.setenv("FI_CONTROLLER_CONFIG", str(mock_config_path))
        from web_controller.app import load_config
        cfg = load_config()
        assert "nodes" in cfg
        assert cfg["hadoop"]["home"] == "/opt/hadoop"

    def test_missing_file_raises(self, monkeypatch, tmp_path):
        monkeypatch.setenv("FI_CONTROLLER_CONFIG", str(tmp_path / "nope.json"))
        from web_controller.app import load_config
        with pytest.raises(FileNotFoundError):
            load_config()


# ====================================================================
# 4. 命令构建 — build_ssh_command / normalize_cmds / render_template / maybe_sudo / resolve_sudo
# ====================================================================


class TestBuildSshCommand:
    """build_ssh_command: 构造 SSH 远程执行命令。"""

    def test_basic_ssh(self, mock_config):
        node = {"host": "10.0.0.1", "name": "slave1"}
        cmd = build_ssh_command(mock_config, node, ["jps"])
        assert cmd[0] == "ssh"
        assert "root@10.0.0.1" in cmd
        assert "jps" in cmd[-1]

    def test_with_port(self, mock_config):
        node = {"host": "127.0.0.1", "port": 2221, "name": "slave1"}
        cmd = build_ssh_command(mock_config, node, ["ls"])
        assert "-p" in cmd
        assert "2221" in cmd

    def test_with_identity_file(self):
        cfg = {
            "ssh": {"user": "hadoop", "identity_file": "/path/to/key", "connect_timeout": 3},
        }
        node = {"host": "10.0.0.1", "name": "test"}
        cmd = build_ssh_command(cfg, node, ["echo", "hi"])
        assert "-i" in cmd
        assert "/path/to/key" in cmd

    def test_strict_host_disabled(self, mock_config):
        node = {"host": "10.0.0.1", "name": "test"}
        cmd = build_ssh_command(mock_config, node, ["pwd"])
        assert "StrictHostKeyChecking=no" in cmd


class TestRenderTemplate:
    """render_template: 安全字符串模板替换。"""

    def test_basic_substitution(self):
        assert render_template("{name} is {state}", {"name": "NN", "state": "OK"}) == "NN is OK"

    def test_missing_key_kept(self):
        result = render_template("{known} and {unknown}", {"known": "yes"})
        assert result == "yes and {unknown}"

    def test_no_placeholders(self):
        assert render_template("plain text", {}) == "plain text"


class TestSafeFormat:
    """_SafeFormat: 缺失键保留占位符。"""

    def test_missing_returns_placeholder(self):
        sf = _SafeFormat({"a": "1"})
        assert sf["a"] == "1"
        assert sf["missing"] == "{missing}"


class TestNormalizeCmds:
    """normalize_cmds: 将命令规格统一为 List[List[str]]。"""

    def test_string_becomes_shell_cmd(self):
        cmds = normalize_cmds("echo hello", {})
        assert len(cmds) == 1
        assert cmds[0][0] == "/bin/sh"
        assert cmds[0][-1] == "echo hello"

    def test_list_of_strings(self):
        cmds = normalize_cmds([["jps"]], {})
        assert cmds == [["jps"]]

    def test_none_returns_empty(self):
        assert normalize_cmds(None, {}) == []

    def test_template_rendered(self):
        cmds = normalize_cmds("{injector} list", {"injector": "/usr/bin/inj"})
        assert "/usr/bin/inj list" in cmds[0][-1]

    def test_mixed_types(self):
        spec = ["echo hello", ["ls", "-la"]]
        cmds = normalize_cmds(spec, {})
        assert len(cmds) == 2


class TestMaybeSudo:
    """maybe_sudo: 条件性添加 sudo 前缀。"""

    def test_with_sudo(self):
        assert maybe_sudo(["jps"], True) == ["sudo", "-n", "jps"]

    def test_without_sudo(self):
        assert maybe_sudo(["jps"], False) == ["jps"]


class TestResolveSudo:
    """resolve_sudo: 根据配置决定是否使用 sudo。"""

    def test_hadoop_sudo_false(self, mock_config):
        assert resolve_sudo(mock_config, {"sudo": "hadoop"}) is False

    def test_vm_sudo_true(self, mock_config):
        assert resolve_sudo(mock_config, {"sudo": "vm"}) is True

    def test_kvm_sudo_true(self, mock_config):
        assert resolve_sudo(mock_config, {"sudo": "kvm"}) is True

    def test_explicit_true(self, mock_config):
        assert resolve_sudo(mock_config, {"sudo": True}) is True

    def test_explicit_false(self, mock_config):
        assert resolve_sudo(mock_config, {"sudo": False}) is False


# ====================================================================
# 5. 节点解析 — find_node_by_value / resolve_test_nodes / build_context
# ====================================================================


class TestFindNodeByValue:
    """find_node_by_value: 按名称或 IP 查找节点。"""

    def test_find_by_name(self, mock_config):
        node = find_node_by_value(mock_config, "master")
        assert node is not None
        assert node["name"] == "master"

    def test_find_by_host(self, mock_config):
        node = find_node_by_value(mock_config, "127.0.0.1")
        assert node is not None

    def test_find_unknown_ip_creates_adhoc(self, mock_config):
        node = find_node_by_value(mock_config, "10.0.0.99")
        assert node is not None
        assert node["host"] == "10.0.0.99"

    def test_empty_returns_none(self, mock_config):
        assert find_node_by_value(mock_config, "") is None

    def test_invalid_name_returns_none(self, mock_config):
        assert find_node_by_value(mock_config, "nonexistent") is None


class TestResolveTestNodes:
    """resolve_test_nodes: 按 scope 解析测试执行节点列表。"""

    def test_scope_all(self, mock_config):
        nodes = resolve_test_nodes(mock_config, "all", [], {})
        assert len(nodes) == 3

    def test_scope_master(self, mock_config):
        nodes = resolve_test_nodes(mock_config, "master", [], {})
        assert len(nodes) == 1
        assert nodes[0]["name"] == "master"

    def test_scope_local(self, mock_config):
        nodes = resolve_test_nodes(mock_config, "local", [], {})
        assert len(nodes) == 1
        assert nodes[0]["name"] == "local"

    def test_scope_target_with_valid(self, mock_config):
        nodes = resolve_test_nodes(mock_config, "target", [], {"target": "slave1"})
        assert len(nodes) == 1

    def test_scope_target_empty(self, mock_config):
        nodes = resolve_test_nodes(mock_config, "target", [], {})
        assert len(nodes) == 0

    def test_scope_action_returns_action_nodes(self, mock_config):
        action_nodes = [{"name": "n1"}]
        nodes = resolve_test_nodes(mock_config, "action", action_nodes, {})
        assert nodes == action_nodes


class TestResolveTestSudo:
    """resolve_test_sudo: 测试级别 sudo 解析。"""

    def test_bool_true(self, mock_config):
        assert resolve_test_sudo(mock_config, {"sudo": True}, {}) is True

    def test_bool_false(self, mock_config):
        assert resolve_test_sudo(mock_config, {"sudo": False}, {}) is False

    def test_vm_key(self, mock_config):
        assert resolve_test_sudo(mock_config, {"sudo": "vm"}, {}) is True

    def test_action_key(self, mock_config):
        assert resolve_test_sudo(mock_config, {"sudo": "action"}, {"sudo": False}) is False

    def test_no_sudo_key(self, mock_config):
        assert resolve_test_sudo(mock_config, {}, {}) is False


class TestBuildContext:
    """build_context: 从配置构建模板上下文。"""

    def test_has_hadoop_home(self, mock_config):
        ctx = build_context(mock_config)
        assert ctx["hadoop_home"] == "/opt/hadoop"

    def test_has_injector(self, mock_config):
        ctx = build_context(mock_config)
        assert ctx["injector"] == "/usr/local/bin/hadoop_injector"

    def test_has_vm_tools(self, mock_config):
        ctx = build_context(mock_config)
        assert "vm_cpu_injector" in ctx
        assert "vm_mem_injector" in ctx
        assert "vm_reg_injector" in ctx

    def test_has_kvm_injector(self, mock_config):
        ctx = build_context(mock_config)
        assert ctx["kvm_injector"] == "/tmp/kvm_injection/kvm_injector"


# ====================================================================
# 6. ACTIONS 定义完整性
# ====================================================================


class TestActionsDefinition:
    """验证 ACTIONS dict 中每个 action 的结构完整性。"""

    def test_all_actions_have_required_keys(self):
        required = {"title", "group", "cmds"}
        for key, spec in ACTIONS.items():
            for rk in required:
                assert rk in spec, f"Action '{key}' missing key '{rk}'"

    def test_all_groups_referenced_exist(self):
        group_keys = {g["key"] for g in GROUPS}
        for key, spec in ACTIONS.items():
            assert spec["group"] in group_keys, f"Action '{key}': group '{spec['group']}' undefined"

    def test_all_actions_have_callable_cmds(self):
        for key, spec in ACTIONS.items():
            assert callable(spec["cmds"]), f"Action '{key}': cmds is not callable"

    def test_param_names_are_strings(self):
        for key, spec in ACTIONS.items():
            for p in spec.get("params", []):
                assert isinstance(p.get("name"), str), f"Action '{key}' has param without name"

    def test_action_count_is_reasonable(self):
        assert len(ACTIONS) >= 25, f"Expected >=25 actions, got {len(ACTIONS)}"

    def test_groups_count(self):
        assert len(GROUPS) == 8


class TestParamEnumsAndRanges:
    """验证 PARAM_ENUMS 与 NUM_RANGES 的结构。"""

    def test_enum_values_are_sets(self):
        for name, values in PARAM_ENUMS.items():
            assert isinstance(values, set), f"PARAM_ENUMS['{name}'] is not a set"

    def test_ranges_are_tuples(self):
        for name, rng in NUM_RANGES.items():
            assert isinstance(rng, tuple) and len(rng) == 2, f"NUM_RANGES['{name}'] invalid"
            assert rng[0] <= rng[1], f"NUM_RANGES['{name}']: min > max"


# ====================================================================
# 7. 命令构建器 — _build_vm_mem_inject_cmd / _build_vm_reg_inject_cmd / _build_kvm_soft_cmd
# ====================================================================


class TestBuildVmMemInjectCmd:
    """_build_vm_mem_inject_cmd: VM 内存注入命令构建。"""

    def test_basic_heap_flip(self):
        ctx = {"vm_mem_injector": "/tmp/mem_inj"}
        params = {"pid": 1234, "mem_type": "flip", "mem_bit": 5, "mem_region": "heap"}
        cmds = _build_vm_mem_inject_cmd(ctx, params)
        assert len(cmds) == 1
        cmd = cmds[0]
        assert cmd[0] == "/tmp/mem_inj"
        assert "-p" in cmd and "1234" in cmd
        assert "-r" in cmd and "heap" in cmd

    def test_with_address(self):
        ctx = {"vm_mem_injector": "/tmp/mem_inj"}
        params = {"pid": 1, "mem_type": "set0", "mem_bit": 0, "mem_region": "heap", "addr": "0x7fff"}
        cmds = _build_vm_mem_inject_cmd(ctx, params)
        assert "-a" in cmds[0]
        assert "0x7fff" in cmds[0]
        assert "-r" not in cmds[0]

    def test_with_signature(self):
        ctx = {"vm_mem_injector": "/tmp/mem_inj"}
        params = {"pid": 1, "mem_type": "byte", "mem_bit": 0, "mem_region": "stack", "signature": "deadbeef"}
        cmds = _build_vm_mem_inject_cmd(ctx, params)
        assert "-s" in cmds[0]
        assert "deadbeef" in cmds[0]


class TestBuildVmRegInjectCmd:
    """_build_vm_reg_inject_cmd: VM 寄存器注入命令构建。"""

    def test_basic_command(self):
        ctx = {"vm_reg_injector": "/tmp/reg_inj"}
        params = {"pid": 100, "reg": "X0", "reg_type": "flip1", "reg_bit": 3}
        cmds = _build_vm_reg_inject_cmd(ctx, params)
        assert cmds[0][0] == "/tmp/reg_inj"
        assert "100" in cmds[0]
        assert "X0" in cmds[0]
        assert "flip1" in cmds[0]

    def test_with_delay_and_loop(self):
        ctx = {"vm_reg_injector": "/tmp/reg_inj"}
        params = {
            "pid": 100, "reg": "SP", "reg_type": "zero1", "reg_bit": -1,
            "reg_delay": 500, "reg_loop": 10, "reg_interval": 100,
        }
        cmds = _build_vm_reg_inject_cmd(ctx, params)
        cmd = cmds[0]
        assert "-w" in cmd and "500" in cmd
        assert "-l" in cmd and "10" in cmd
        assert "-i" in cmd and "100" in cmd


class TestBuildKvmSoftCmd:
    """_build_kvm_soft_cmd: KVM 软错误命令构建。"""

    def test_soft_flip(self):
        ctx = {"kvm_injector": "/tmp/kvm_inj"}
        params = {"soft_type": "flip", "pid": 42, "reg": "PC", "soft_bit": 7}
        cmds = _build_kvm_soft_cmd(ctx, params)
        assert "soft-flip" in cmds[0]
        assert "7" in cmds[0]

    def test_soft_swap_no_bit(self):
        ctx = {"kvm_injector": "/tmp/kvm_inj"}
        params = {"soft_type": "swap", "pid": 42, "reg": "X1"}
        cmds = _build_kvm_soft_cmd(ctx, params)
        assert "soft-swap" in cmds[0]
        # swap does not take bit arg
        assert len(cmds[0]) == 4

    def test_soft_zero_with_bit(self):
        ctx = {"kvm_injector": "/tmp/kvm_inj"}
        params = {"soft_type": "zero", "pid": 42, "reg": "SP", "soft_bit": 0}
        cmds = _build_kvm_soft_cmd(ctx, params)
        assert "soft-zero" in cmds[0]
        assert "0" in cmds[0]


# ====================================================================
# 8. API 端点测试 — /api/health, /api/config, /api/action
# ====================================================================


class TestApiHealth:
    """GET /api/health: 健康检查端点。"""

    def test_returns_ok(self, client):
        resp = client.get("/api/health")
        assert resp.status_code == 200
        assert resp.json()["ok"] is True


class TestApiConfig:
    """GET /api/config: 返回配置与 action 列表。"""

    def test_returns_nodes(self, client):
        resp = client.get("/api/config")
        data = resp.json()
        assert "nodes" in data
        assert len(data["nodes"]) == 3

    def test_returns_actions(self, client):
        resp = client.get("/api/config")
        data = resp.json()
        assert "actions" in data
        assert len(data["actions"]) >= 25

    def test_returns_groups(self, client):
        resp = client.get("/api/config")
        data = resp.json()
        assert "groups" in data
        assert len(data["groups"]) == 8

    def test_action_has_key_and_title(self, client):
        resp = client.get("/api/config")
        for a in resp.json()["actions"]:
            assert "key" in a
            assert "title" in a


class TestApiAction:
    """POST /api/action: 参数校验与错误处理。"""

    def test_unknown_action_returns_400(self, client):
        resp = client.post("/api/action", json={"action": "nonexistent"})
        assert resp.status_code == 400

    def test_missing_required_param(self, client):
        # process_fault requires op and component
        resp = client.post("/api/action", json={"action": "process_fault", "params": {}})
        assert resp.status_code == 400

    def test_invalid_enum_value(self, client):
        resp = client.post(
            "/api/action",
            json={"action": "process_fault", "params": {"op": "INVALID", "component": "nn"}},
        )
        assert resp.status_code == 400

    def test_invalid_target(self, client):
        resp = client.post(
            "/api/action",
            json={"action": "delay", "params": {"target": "!!!bad!!!", "ms": 100}},
        )
        assert resp.status_code == 400

    def test_number_out_of_range(self, client):
        resp = client.post(
            "/api/action",
            json={"action": "delay", "params": {"target": "slave1", "ms": 999999}},
        )
        assert resp.status_code == 400

    def test_hex_validation_for_addr(self, client):
        resp = client.post(
            "/api/action",
            json={
                "action": "vm_mem_inject",
                "params": {
                    "pid": 1,
                    "mem_region": "heap",
                    "mem_type": "flip",
                    "mem_bit": 0,
                    "addr": "NOT_HEX",
                },
            },
        )
        assert resp.status_code == 400

    def test_vm_network_needs_param(self, client):
        resp = client.post(
            "/api/action",
            json={"action": "vm_network", "params": {"net_type": "delay"}},
        )
        assert resp.status_code == 400


class TestApiActionExecution:
    """POST /api/action: 可本地执行的命令（cluster_status 使用 jps）。"""

    def test_cluster_status_returns_results(self, client):
        """cluster_status runs jps on all nodes via SSH.
        Even if SSH fails, the response should have correct structure."""
        resp = client.post("/api/action", json={"action": "cluster_status", "params": {}})
        data = resp.json()
        assert "ok" in data
        assert "results" in data
        assert isinstance(data["results"], list)
        assert "action" in data and data["action"] == "cluster_status"


# ====================================================================
# 9. ActionRequest Model
# ====================================================================


class TestActionRequestModel:
    """ActionRequest Pydantic 模型验证。"""

    def test_minimal(self):
        req = ActionRequest(action="test_action")
        assert req.action == "test_action"
        assert req.params is None
        assert req.tests is None

    def test_with_params(self):
        req = ActionRequest(action="a", params={"k": "v"}, tests={"kvm": True})
        assert req.params == {"k": "v"}
        assert req.tests == {"kvm": True}
