"""Shared fixtures for fault-injection controller tests."""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any, Dict

import pytest
from fastapi.testclient import TestClient


# ---------------------------------------------------------------------------
# Mock configuration – mirrors real config.json structure but uses localhost
# without SSH so tests can run anywhere.
# ---------------------------------------------------------------------------

MOCK_CONFIG: Dict[str, Any] = {
    "ssh": {
        "user": "root",
        "identity_file": "",
        "connect_timeout": 5,
    },
    "nodes": [
        {"name": "master", "host": "127.0.0.1", "port": 2220, "role": "master", "local": False},
        {"name": "slave1", "host": "127.0.0.1", "port": 2221, "role": "slave", "local": False},
        {"name": "slave2", "host": "127.0.0.1", "port": 2222, "role": "slave", "local": False},
    ],
    "hadoop": {
        "home": "/opt/hadoop",
        "injector": "/usr/local/bin/hadoop_injector",
        "use_sudo": False,
    },
    "vm": {
        "base_dir": "/tmp/vm_injection",
        "process_injector": "/tmp/vm_injection/process_injector",
        "network_injector": "/tmp/vm_injection/network_injector",
        "cpu_injector": "/tmp/vm_injection/cpu_injector",
        "mem_leak": "/tmp/vm_injection/mem_leak",
        "mem_injector": "/tmp/vm_injection/mem_injector",
        "reg_injector": "/tmp/vm_injection/reg_injector",
        "use_sudo": True,
    },
    "kvm": {
        "base_dir": "/tmp/kvm_injection",
        "injector": "/tmp/kvm_injection/kvm_injector",
        "use_sudo": True,
    },
    "controller": {"command_timeout": 20},
    "output": {"max_lines": 0, "max_chars": 0},
    "tests": {"enabled": False},
}


@pytest.fixture()
def mock_config_path(tmp_path: Path) -> Path:
    """Write MOCK_CONFIG to a temp file and return its path."""
    cfg_path = tmp_path / "config.json"
    cfg_path.write_text(json.dumps(MOCK_CONFIG), encoding="utf-8")
    return cfg_path


@pytest.fixture()
def mock_config() -> Dict[str, Any]:
    """Return a deep-copy-safe dict of the mock config."""
    return json.loads(json.dumps(MOCK_CONFIG))


@pytest.fixture()
def patch_config(mock_config_path: Path, monkeypatch: pytest.MonkeyPatch):
    """Patch the FI_CONTROLLER_CONFIG env var so load_config() uses our mock."""
    monkeypatch.setenv("FI_CONTROLLER_CONFIG", str(mock_config_path))


@pytest.fixture()
def client(patch_config) -> TestClient:
    """TestClient with mocked config; imports app lazily to pick up env."""
    # Re-import to ensure env var takes effect
    import importlib
    import web_controller.app as app_module

    importlib.reload(app_module)
    return TestClient(app_module.app, raise_server_exceptions=False)
