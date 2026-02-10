# Hadoop 集群环境恢复与启动手册

## 1. 宿主机：启动虚拟机脚本

在 **Ubuntu 宿主机**上，使用支持“双网卡”的脚本启动虚拟机。网卡 1 用于 SSH 连接，网卡 2 用于集群内网通信。

Bash

```
#!/bin/bash
# 保存为 run_cluster.sh

if [ -z "$1" ]; then
    echo "用法: ./run_cluster.sh <master|slave1|slave2>"
    exit 1
fi

NODE=$1

# 配置：根据节点名分配 SSH 端口转发和内网 MAC 后缀
if [ "$NODE" == "master" ]; then
    SUFFIX="10"
    HOST_PORT="2220"
elif [ "$NODE" == "slave1" ]; then
    SUFFIX="11"
    HOST_PORT="2221"
elif [ "$NODE" == "slave2" ]; then
    SUFFIX="12"
    HOST_PORT="2222"
else
    echo "❌ 错误: 未知节点名 '$NODE'"
    exit 1
fi

echo "🚀 正在启动节点: $NODE (SSH 映射端口: $HOST_PORT) ..."

qemu-system-aarch64 \
  -name "alpine_$NODE" \
  -M virt,accel=kvm \
  -cpu host \
  -m 2048 \
  -bios ./uefi.fd \
  -drive file=images/node_$NODE.qcow2,format=qcow2,if=virtio \
  -netdev user,id=net0,hostfwd=tcp::${HOST_PORT}-:22 \
  -device virtio-net-pci,netdev=net0 \
  -netdev socket,id=net1,mcast=230.0.0.1:1234 \
  -device virtio-net-pci,netdev=net1,mac=52:54:00:12:34:$SUFFIX \
  -device virtio-gpu-pci \
  -device qemu-xhci \
  -device usb-kbd \
  -device usb-tablet \
  -boot menu=on
```

 [chmod +x *]

./run_cluster master

## 2. 虚拟机：配置内网 IP 永久生效

在 **Alpine 虚拟机内部**修改网络配置文件，确保重启后 `192.168.1.x` 仍然可用。

### 在 Master 节点上：

修改 `/etc/network/interfaces`：

Plaintext

```
auto eth1
iface eth1 inet static
    address 192.168.1.10
    netmask 255.255.255.0
```

### 在 Slave 节点上：

分别修改 Slave1 (`.11`) 和 Slave2 (`.12`) 的 `/etc/network/interfaces`：

Plaintext

```
# 以 Slave1 为例
auto eth1
iface eth1 inet static
    address 192.168.1.11
    netmask 255.255.255.0
```

**生效命令**：执行 `rc-service networking restart` 或 `reboot`。

## 3. 启动 Hadoop 服务 (HDFS)

在 **Master 节点**上，通过守护进程方式手动启动。

Bash

```
# 加载环境变量
source /etc/profile

# 启动 NameNode 和 SecondaryNameNode
hdfs --daemon start namenode
hdfs --daemon start secondarynamenode

# 远程启动 Slave 节点的 DataNode
ssh slave1 "source /etc/profile; hdfs --daemon start datanode"
ssh slave2 "source /etc/profile; hdfs --daemon start datanode"

# 检查 HDFS 状态
hdfs dfsadmin -report
```

## 4. 启动 YARN 服务

在 **Master 节点**上启动资源调度系统。

Bash

```
# 启动 ResourceManager
yarn --daemon start resourcemanager

# 远程启动 Slave 节点的 NodeManager
ssh slave1 "source /etc/profile; yarn --daemon start nodemanager"
ssh slave2 "source /etc/profile; yarn --daemon start nodemanager"

# 检查进程
jps
# 应看到: NameNode, SecondaryNameNode, ResourceManager, Jps
```

## 5. 验证集群状态

在 **Master** 上验证全线进程是否正常。

- **Master 节点应有**：`NameNode`, `SecondaryNameNode`, `ResourceManager`

- **Slave 节点应有**：`DataNode`, `NodeManager`

- **测试 HDFS 写入**：

  Bash

  ```
  hdfs dfs -mkdir /success_test
  hdfs dfs -ls /
  ```

