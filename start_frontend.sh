#!/bin/bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VM_DIR="$ROOT_DIR/vm_injection"
#VM_DIR="$ROOT_DIR/vm_injection"
LOG_DIR="$ROOT_DIR/.vm_logs"

if [ ! -x "$VM_DIR/run_cluster.sh" ]; then
  echo "[error] $VM_DIR/run_cluster.sh not found or not executable"
  exit 1
fi

mkdir -p "$LOG_DIR"

is_vm_running() {
  local node="$1"
  local pattern="qemu-system-aarch64.*alpine_${node}"
  if command -v pgrep >/dev/null 2>&1; then
    pgrep -f "$pattern" >/dev/null 2>&1
    return $?
  fi
  ps aux | grep -v grep | grep -E "$pattern" >/dev/null 2>&1
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

start_vm master
start_vm slave1
start_vm slave2

UVICORN_BIN="uvicorn"
if [ -x "$ROOT_DIR/.venv/bin/uvicorn" ]; then
  UVICORN_BIN="$ROOT_DIR/.venv/bin/uvicorn"
elif ! command -v uvicorn >/dev/null 2>&1; then
  echo "[error] uvicorn not found. Activate .venv or install requirements."
  exit 1
fi

echo "[info] starting web controller..."
exec "$UVICORN_BIN" web_controller.app:app --host 0.0.0.0 --port 8080
