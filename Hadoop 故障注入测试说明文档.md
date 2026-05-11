# Hadoop 故障注入测试说明文档

本文档与 `kvm_injection/hadoop-fi/hadoop_injector.c` 当前实现对齐，保留 Hadoop/HDFS/YARN/MapReduce 相关测试内容。

## 1. 环境假设

| 项 | 当前默认 |
| --- | --- |
| master | `192.168.1.10` 或 Web 配置中的 `127.0.0.1:2220` |
| slave1 | `192.168.1.11` 或 Web 配置中的 `127.0.0.1:2221` |
| slave2 | `192.168.1.12` 或 Web 配置中的 `127.0.0.1:2222` |
| 注入器路径 | `/root/hadoop-fi/hadoop_injector` |
| 代码位置 | `kvm_injection/hadoop-fi/hadoop_injector.c` |
| 默认 slave 地址 | 源码常量 `SLAVE_HOSTS[] = {"192.168.1.11", "192.168.1.12"}` |

如果实际环境不同，需要调整源码中的 `SLAVE_HOSTS`、`REMOTE_TOOL_PATH`，或保证所有节点都部署相同路径的 `hadoop_injector`。

## 2. 编译与部署

```bash
cd /Users/venele/Downloads/fault-injection/kvm_injection/hadoop-fi
make

# 将二进制部署到 master/slave 的 /root/hadoop-fi/
mkdir -p /root/hadoop-fi
cp hadoop_injector /root/hadoop-fi/
```

## 3. 支持命令

| 类别 | 命令 | 示例 |
| --- | --- | --- |
| 状态 | `list`、`list-local` | `./hadoop_injector list` |
| 进程 | `crash <comp>`、`hang <comp>`、`resume <comp>` | `sudo ./hadoop_injector crash dn` |
| 网络延迟 | `delay <target> <ms> [jitter]`、`delay-clear`、`delay-show` | `sudo ./hadoop_injector delay slave1 200 50` |
| 丢包 | `loss <target> <percent>`、`loss-clear` | `sudo ./hadoop_injector loss slave1 10` |
| 乱序 | `reorder <target> <percent> [correlation]`、`reorder-clear` | `sudo ./hadoop_injector reorder slave1 30 25` |
| 隔离 | `isolate <target> [port]`、`isolate-clear` | `sudo ./hadoop_injector isolate slave1` |
| CPU | `cpu-stress <target> <duration> [threads]` | `sudo ./hadoop_injector cpu-stress slave1 10 2` |
| 内存 | `mem-stress <target> <MB>`、`mem-stress-clear` | `sudo ./hadoop_injector mem-stress slave1 512` |
| 磁盘 | `disk-fill <target> <MB>`、`disk-fill-clear` | `sudo ./hadoop_injector disk-fill slave1 512` |
| HDFS | `hdfs-safe <enter|leave>`、`hdfs-disk <target> <MB>` | `./hadoop_injector hdfs-safe enter` |
| YARN | `yarn-unhealthy <target> <on|off>` | `sudo ./hadoop_injector yarn-unhealthy slave1 on` |
| MapReduce | `crash-map <target>`、`crash-reduce <target>` | `sudo ./hadoop_injector crash-map slave1` |
| I/O | `io-slow <target> <on|off>` | `sudo ./hadoop_injector io-slow slave1 on` |
| 心跳 | `heartbeat <target> <ms>` | `sudo ./hadoop_injector heartbeat slave1 5000` |

组件代号：

| 代号 | 组件 |
| --- | --- |
| `nn` | NameNode |
| `dn` | DataNode |
| `rm` | ResourceManager |
| `nm` | NodeManager |
| `snn` | SecondaryNameNode |
| `jhs` | JobHistoryServer |
| `map` | YarnChild Map 任务 |
| `reduce` | YarnChild Reduce 任务 |
| `am` | MRAppMaster |

## 4. 推荐测试流程

### 4.1 进程故障

```bash
./hadoop_injector list
sudo ./hadoop_injector crash dn
./hadoop_injector list
ssh slave1 "source /etc/profile; hdfs --daemon start datanode"
```

验证：

```bash
jps
hdfs dfsadmin -report
yarn node -list -all
```

### 4.2 网络故障

```bash
sudo ./hadoop_injector delay slave1 200 50
sudo ./hadoop_injector loss slave1 10
sudo ./hadoop_injector reorder slave1 30
sudo ./hadoop_injector isolate slave1
```

验证：

```bash
ping -c 4 slave1
tc qdisc show
iptables -L -n
```

清理：

```bash
sudo ./hadoop_injector delay-clear
sudo ./hadoop_injector loss-clear
sudo ./hadoop_injector reorder-clear
sudo ./hadoop_injector isolate-clear
```

### 4.3 资源故障

```bash
sudo ./hadoop_injector cpu-stress slave1 10 2
sudo ./hadoop_injector mem-stress slave1 512
sudo ./hadoop_injector disk-fill slave1 512
sudo ./hadoop_injector io-slow slave1 on
```

验证：

```bash
ssh slave1 "top -bn1 | head -5"
ssh slave1 "free -m | head -2"
ssh slave1 "df -h /; ls -lh /tmp/disk_hog /tmp/hadoop_mem_stress 2>/dev/null || true"
ssh slave1 "cat /sys/fs/cgroup/io_limited/io.max 2>/dev/null || true"
```

清理：

```bash
sudo ./hadoop_injector mem-stress-clear
sudo ./hadoop_injector disk-fill-clear
sudo ./hadoop_injector io-slow slave1 off
```

### 4.4 HDFS / YARN

```bash
./hadoop_injector hdfs-safe enter
hdfs dfsadmin -safemode get
./hadoop_injector hdfs-safe leave

sudo ./hadoop_injector yarn-unhealthy slave1 on
yarn node -list -all
sudo ./hadoop_injector yarn-unhealthy slave1 off
```

### 4.5 MapReduce 任务故障

Map/Reduce 任务故障需要先有正在运行的作业：

```bash
hadoop jar $HADOOP_HOME/share/hadoop/mapreduce/hadoop-mapreduce-examples-*.jar \
  pi 50 10000 > /tmp/fi_mapreduce_job.log 2>&1 &

sleep 5
sudo ./hadoop_injector crash-map slave1
sudo ./hadoop_injector crash-reduce slave1
yarn application -list -appStates ALL
```

预期现象是 YarnChild 被杀死，YARN 日志中出现 container 失败和重试记录。

## 5. Web 控制器中的 Hadoop

`web_controller/app.py` 的 `ACTIONS` 保留 Hadoop 单次动作，包含集群状态、Hadoop 启停、进程故障、网络故障、资源故障、HDFS/YARN 和 MapReduce。当前 `test_scenarios.py` 的最终功能测试列表没有包含 Hadoop group，因此 Hadoop 主要通过 Web 单步按钮或 `/api/action` 执行。

## 6. 结论记录模板

| 测试项 | 注入命令 | 验证命令 | 预期现象 | 清理命令 | 结果 |
| --- | --- | --- | --- | --- | --- |
| DataNode 崩溃 | `crash dn` | `jps`、`hdfs dfsadmin -report` | DataNode 消失或被标记异常 | 重启 DataNode |  |
| 网络延迟 | `delay slave1 200` | `ping`、`tc qdisc show` | RTT 增加 | `delay-clear` |  |
| HDFS 安全模式 | `hdfs-safe enter` | `hdfs dfsadmin -safemode get` | HDFS 只读 | `hdfs-safe leave` |  |
| YARN 不健康 | `yarn-unhealthy slave1 on` | `yarn node -list -all` | 节点异常或服务停止 | `yarn-unhealthy slave1 off` |  |
