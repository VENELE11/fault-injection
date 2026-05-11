# CloudStack Injector 测试说明

本文档与 `kvm_injection/cloudstack-fi/cloudstack_injector.c` 当前命令表对齐，保留 CloudStack 管理面、Agent、SystemVM、网络、存储、数据库和资源故障说明。

## 1. 工具位置

```bash
cd /Users/venele/Downloads/fault-injection/kvm_injection/cloudstack-fi
make
sudo ./cloudstack_injector --help
```

大部分功能需要 root 权限，因为会操作进程信号、iptables、tc、存储挂载、cgroup 或数据库命令。

## 2. 支持命令

| 类别 | 命令 |
| --- | --- |
| 进程 | `list`、`crash <组件>`、`hang <组件>`、`resume <组件>` |
| SystemVM | `sysvm-crash <ssvm|cpvm|vr>`、`sysvm-hang <ssvm|cpvm|vr>`、`sysvm-resume <ssvm|cpvm|vr>` |
| 网络 | `api-delay <ms>`、`api-delay-clear`、`network <IP> [port]`、`network-clear <IP>`、`agent-disconnect [IP]`、`agent-reconnect [IP]` |
| 存储 | `storage-umount <path>`、`storage-ro <path>`、`storage-rw <path>`、`storage-fill <path>`、`storage-clean <path>` |
| 数据库 | `db-limit`、`db-restore`、`db-lock`、`db-unlock` |
| 资源 | `cpu-stress <seconds> [threads]`、`mem-stress <MB>`、`mem-stress-clear` |
| VM 操作 | `vm-create-fail`、`vm-migrate-fail`、`vm-op-clear` |

组件代号：

| 代号 | 组件 |
| --- | --- |
| `ms` | Management Server |
| `agent` | CloudStack Agent |
| `usage` | Usage Server |
| `mysql` | MySQL |
| `nfs` | NFS |
| `libvirt` | Libvirt |
| `ssvm` | Secondary Storage VM |
| `cpvm` | Console Proxy VM |
| `vr` | Virtual Router |

## 3. 模拟环境

如果没有真实 CloudStack，可用模拟进程验证注入器逻辑：

```bash
exec -a cloudstack-management sleep 10000 &
exec -a cloudstack-agent sleep 10000 &
exec -a cloudstack-usage sleep 10000 &
exec -a mysqld sleep 10000 &

exec -a "qemu-system-x86_64 -name guest=s-1-VM systemvm" sleep 10000 &
exec -a "qemu-system-x86_64 -name guest=v-2-VM consoleproxy" sleep 10000 &
exec -a "qemu-system-x86_64 -name guest=r-3-VM router" sleep 10000 &
```

API 延迟测试可启动本地 HTTP 服务：

```bash
python3 -m http.server 8080 --bind 0.0.0.0 &
```

## 4. 核心测试用例

### 4.1 服务状态

```bash
sudo ./cloudstack_injector list
```

预期：列出 Management、Agent、Usage、MySQL、NFS、Libvirt、SystemVM 等可识别组件状态。

### 4.2 组件挂起、恢复、崩溃

```bash
sudo ./cloudstack_injector hang agent
ps -o pid,stat,comm,args -p "$(pgrep -f cloudstack-agent | head -n 1)"
sudo ./cloudstack_injector resume agent
sudo ./cloudstack_injector crash ms
```

预期：`hang` 后进程状态包含 `T`，`resume` 后恢复，`crash` 终止目标进程。

### 4.3 API 延迟

```bash
sudo ./cloudstack_injector api-delay 1000
time curl http://127.0.0.1:8080/
sudo ./cloudstack_injector api-delay-clear
```

实现使用 `tc` 对 API 端口响应流量注入延迟。默认 API 端口为 8080。

### 4.4 网络隔离与 Agent 断连

```bash
sudo ./cloudstack_injector network 192.168.1.11 8250
sudo ./cloudstack_injector network-clear 192.168.1.11

sudo ./cloudstack_injector agent-disconnect 192.168.1.11
sudo ./cloudstack_injector agent-reconnect 192.168.1.11
```

预期：iptables 规则阻断指定 IP 或 Agent 端口通信。

### 4.5 存储故障

```bash
mkdir -p /tmp/cs_secondary
sudo ./cloudstack_injector storage-ro /tmp/cs_secondary
sudo ./cloudstack_injector storage-rw /tmp/cs_secondary
sudo ./cloudstack_injector storage-fill /tmp/cs_secondary
sudo ./cloudstack_injector storage-clean /tmp/cs_secondary
```

预期：只读、磁盘填充和清理动作按命令生效。真实挂载点上操作前请确认可恢复。

### 4.6 数据库故障

```bash
sudo ./cloudstack_injector db-limit
sudo ./cloudstack_injector db-restore
sudo ./cloudstack_injector db-lock
sudo ./cloudstack_injector db-unlock
```

无真实数据库时可能返回连接错误，但可验证命令分支和错误处理。

### 4.7 资源与 VM 操作故障

```bash
sudo ./cloudstack_injector cpu-stress 10 2
sudo ./cloudstack_injector mem-stress 200
sudo ./cloudstack_injector mem-stress-clear

sudo ./cloudstack_injector vm-create-fail
sudo ./cloudstack_injector vm-migrate-fail
sudo ./cloudstack_injector vm-op-clear
```

## 5. Web 控制器中的 CloudStack

`web_controller/app.py` 保留 CloudStack 单次动作：服务状态、组件进程控制、API 延迟、网络隔离和清理。当前 CLI 注入器使用 `network` / `network-clear` 作为网络命令名；如果 Web 动作使用的命令名与本地二进制不一致，请以 `cloudstack_injector --help` 为准并同步更新 `web_controller/app.py`。

## 6. 诊断命令

```bash
pgrep -af 'cloudstack|mysqld|systemvm|consoleproxy|router'
ss -tlnp | grep -E '8080|8250|3306'
ip route get 8.8.8.8
tc qdisc show
sudo iptables -L -n -v
```

## 7. 清理

```bash
sudo ./cloudstack_injector api-delay-clear
sudo ./cloudstack_injector agent-reconnect
sudo ./cloudstack_injector mem-stress-clear
sudo ./cloudstack_injector vm-op-clear
sudo iptables -F
```
