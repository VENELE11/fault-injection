# Web 控制器说明

`web_controller` 是当前项目的统一控制面，后端使用 FastAPI，前端是 `static/index.html`、`static/app.js`、`static/styles.css`。它负责读取 `config.json`，把 Web 操作转换成本地命令、SSH 命令或 Chaos Mesh CRD，并把执行结果、验证结果和历史记录返回给页面。

## 当前功能

| 分组 | 对应代码 | 支持能力 |
| --- | --- | --- |
| K8s 状态概览 | `k8s_status` | 节点、Pod、Chaos Mesh 实验、事件查看 |
| Pod 混沌实验 | `k8s_pod_kill`、`k8s_container_kill` | Pod Kill、Container Kill |
| 网络混沌实验 | `k8s_network_delay`、`k8s_network_loss` | NetworkChaos 延迟、抖动、丢包 |
| 资源混沌实验 | `k8s_cpu_stress`、`k8s_memory_stress` | StressChaos CPU/内存压力 |
| Hadoop 集群 | `cluster_*`、`process_fault`、`hdfs_*` 等 | jps、Hadoop 启停、进程、网络、资源、HDFS/YARN、MapReduce 动作 |
| VM 注入 | `vm_*` | 进程、网络、CPU、内存泄漏、内存篡改、寄存器注入 |
| KVM 注入 | `kvm_*` | 虚拟机列表、软错误、客户机异常、性能故障、CPU 热插拔、清理 |
| CloudStack | `cloudstack_*` | 服务状态、组件进程、API 延迟、网络隔离动作 |

当前 `/api/functest` 的测试用例来自 `test_scenarios.py`，包含 Chaos Mesh、VM、KVM 场景。Hadoop 与 CloudStack 的按钮仍存在于 `/api/action` 动作集中，适合单步执行和人工观察。

## 安装与启动

```bash
cd /Users/venele/Downloads/fault-injection
python3 -m venv .venv
. .venv/bin/activate
pip install -r web_controller/requirements.txt
./start_frontend.sh
```

访问：`http://<宿主机IP>:8080`

只启动 Web 服务：

```bash
uvicorn web_controller.app:app --host 0.0.0.0 --port 8080
```

`start_frontend.sh` 会自动尝试启动 `vm_injection/run_cluster.sh master|slave1|slave2`，日志写到 `.vm_logs/`。

## 配置文件

默认读取 `web_controller/config.json`，也可以通过环境变量覆盖：

```bash
FI_CONTROLLER_CONFIG=/path/to/config.json uvicorn web_controller.app:app --host 0.0.0.0 --port 8080
```

关键字段：

| 字段 | 说明 |
| --- | --- |
| `ssh` | 远程节点登录用户、私钥、连接超时 |
| `nodes` | master/slave 节点名、host、端口、角色、是否本地 |
| `hadoop.home` | Hadoop 安装路径，当前默认 `/root/hadoop` |
| `hadoop.injector` | master 上可执行的 `hadoop_injector` 路径 |
| `cloudstack.injector` | CloudStack 注入器路径 |
| `kubernetes.kubectl` | kubectl 命令，可以是本地命令或 SSH 包装命令 |
| `kubernetes.default_namespace` | Chaos Mesh 实验默认业务命名空间 |
| `vm.*` | VM 注入器二进制路径 |
| `kvm.injector` | `vm_injection/kvm_injector` 的路径 |
| `output.max_lines/max_chars` | 命令输出裁剪限制，0 表示不裁剪 |
| `tests.enabled` | 单次动作后是否自动附加默认检查 |

当前示例配置通过 `127.0.0.1:2220/2221/2222` 连接三台虚拟机，`kubernetes.kubectl` 通过 SSH 到 master 节点执行 `sudo k3s kubectl`。

## Chaos Mesh 工作流

1. 确认 K8s 集群已安装 Chaos Mesh CRD。
2. 在 Web 页面点击“K8s / Chaos 状态查看”，确认 `podchaos,networkchaos,stresschaos` 能被 kubectl 查询。
3. 点击“部署演示应用”，创建默认 `nginx-demo` Deployment。
4. 执行 Pod Kill、Container Kill、网络延迟、网络丢包、CPU 压力或内存压力。
5. 使用“查看混沌实验”观察 CRD 和事件。
6. 使用“清理混沌实验”删除 `podchaos`、`networkchaos`、`stresschaos`。

默认参数：

```text
namespace=default
label_key=app
label_value=nginx-demo
chaos_name=fi-pod-kill / fi-container-kill / fi-network-delay / fi-network-loss / fi-cpu-stress / fi-memory-stress
```

## API 摘要

| 接口 | 方法 | 用途 |
| --- | --- | --- |
| `/` | GET | 返回前端页面 |
| `/api/config` | GET | 返回分组、动作、节点、输出配置 |
| `/api/action` | POST | 执行单个动作 |
| `/api/testcases` | GET | 列出功能测试 |
| `/api/functest` | POST | 执行功能测试流程 |
| `/api/functest/cleanup` | POST | 执行测试清理动作 |
| `/api/history` | GET | 历史记录列表 |
| `/api/history/{run_id}` | GET | 单次运行详情 |
| `/api/health` | GET | 服务健康状态 |

`/api/action` 请求示例：

```json
{
  "action": "k8s_network_delay",
  "params": {
    "namespace": "default",
    "label_key": "app",
    "label_value": "nginx-demo",
    "chaos_name": "fi-network-delay",
    "chaos_mode": "all",
    "ms": 800,
    "jitter": 100,
    "correlation": 25,
    "duration": 60
  }
}
```

## 注意事项

- `sudo` 通过 `sudo -n` 执行，必须提前配置免密 sudo，否则动作会失败。
- Hadoop 注入默认通过 SSH 到 master，再由 `hadoop_injector` 分发到 slave。
- VM/KVM 动作默认在控制器所在宿主机本地执行。
- Chaos Mesh 动作依赖 `kubectl apply -f -` 和已安装的 CRD。
- CloudStack CLI 当前使用 `network` / `network-clear` 命令；如通过 Web 调 CloudStack 网络动作，请确认 `web_controller/app.py` 中的动作名与实际 `cloudstack_injector` 版本一致。
