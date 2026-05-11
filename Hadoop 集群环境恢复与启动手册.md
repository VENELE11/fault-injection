# Hadoop 集群环境恢复与启动手册

本文档保留 Hadoop 集群恢复步骤，基础依赖和 Web 配置已合并到 `使用须知.md`。当前代码默认三节点名为 `master`、`slave1`、`slave2`，Web 配置通过 `127.0.0.1:2220/2221/2222` 访问。

## 1. 启动三台 QEMU 虚拟机

在宿主机执行：

```bash
cd /Users/venele/Downloads/fault-injection/vm_injection
./run_cluster.sh master
./run_cluster.sh slave1
./run_cluster.sh slave2
```

`start_frontend.sh` 也会自动尝试执行这三条命令，并把日志写到 `.vm_logs/`。

## 2. 检查 SSH

```bash
ssh -p 2220 root@127.0.0.1 hostname
ssh -p 2221 root@127.0.0.1 hostname
ssh -p 2222 root@127.0.0.1 hostname
```

如果 Hadoop 脚本内部使用主机名互访，还需要在 master 内确认：

```bash
ssh slave1 hostname
ssh slave2 hostname
```

## 3. 配置内网 IP

如果使用双网卡集群内网，建议固定：

| 节点 | 内网 IP | Hadoop 角色 |
| --- | --- | --- |
| master | `192.168.1.10` | NameNode、SecondaryNameNode、ResourceManager |
| slave1 | `192.168.1.11` | DataNode、NodeManager |
| slave2 | `192.168.1.12` | DataNode、NodeManager |

Alpine 示例：

```text
auto eth1
iface eth1 inet static
    address 192.168.1.11
    netmask 255.255.255.0
```

生效：

```bash
rc-service networking restart
```

`kvm_injection/hadoop-fi/hadoop_injector.c` 当前内置 slave 地址为 `192.168.1.11` 和 `192.168.1.12`，远程工具路径为 `/root/hadoop-fi/hadoop_injector`。如果实际 IP 或路径不同，需要同步修改源码或部署路径。

## 4. 启动 HDFS

在 master 节点执行：

```bash
source /etc/profile
hdfs --daemon start namenode
hdfs --daemon start secondarynamenode
ssh slave1 "source /etc/profile; hdfs --daemon start datanode"
ssh slave2 "source /etc/profile; hdfs --daemon start datanode"
hdfs dfsadmin -report
```

首次初始化才需要：

```bash
hdfs namenode -format
```

## 5. 启动 YARN

```bash
source /etc/profile
yarn --daemon start resourcemanager
ssh slave1 "source /etc/profile; yarn --daemon start nodemanager"
ssh slave2 "source /etc/profile; yarn --daemon start nodemanager"
```

验证：

```bash
jps
ssh slave1 jps
ssh slave2 jps
yarn node -list -all
```

## 6. 常用恢复命令

```bash
# 退出 HDFS 安全模式
hdfs dfsadmin -safemode leave

# 重启 DataNode
ssh slave1 "source /etc/profile; hdfs --daemon start datanode"
ssh slave2 "source /etc/profile; hdfs --daemon start datanode"

# 重启 NodeManager
ssh slave1 "source /etc/profile; yarn --daemon start nodemanager"
ssh slave2 "source /etc/profile; yarn --daemon start nodemanager"

# 清理 Hadoop 注入器制造的网络/资源故障
sudo /root/hadoop-fi/hadoop_injector delay-clear
sudo /root/hadoop-fi/hadoop_injector loss-clear
sudo /root/hadoop-fi/hadoop_injector reorder-clear
sudo /root/hadoop-fi/hadoop_injector isolate-clear
sudo /root/hadoop-fi/hadoop_injector mem-stress-clear
sudo /root/hadoop-fi/hadoop_injector disk-fill-clear
sudo /root/hadoop-fi/hadoop_injector io-slow slave1 off
sudo /root/hadoop-fi/hadoop_injector io-slow slave2 off
sudo /root/hadoop-fi/hadoop_injector yarn-unhealthy slave1 off
sudo /root/hadoop-fi/hadoop_injector yarn-unhealthy slave2 off
```

## 7. 健康检查

```bash
hdfs dfsadmin -report
hdfs dfs -ls /
yarn node -list -all
for n in master slave1 slave2; do echo "== $n =="; ssh "$n" jps; done
```

期望状态：

- master：`NameNode`、`SecondaryNameNode`、`ResourceManager`
- slave：`DataNode`、`NodeManager`

## 8. 相关文档

- `Hadoop 故障注入测试说明文档.md`：注入命令、测试项和验证方式。
- `web_controller/操作指南.md`：Web 中如何触发 Hadoop 单次动作。
