#!/bin/bash

# 检查输入参数
if [ -z "$1" ]; then
    echo "用法: ./run_cluster.sh <master|slave1|slave2>"
    exit 1
fi

NODE=$1

# === 配置: 根据节点名分配 SSH 端口和 MAC 后缀 ===
# Master: SSH 2220, IP 192.168.1.10 (需在内部配置)
if [ "$NODE" == "master" ]; then
    SUFFIX="10"
    HOST_PORT="2220"
# Slave1: SSH 2221, IP 192.168.1.11
elif [ "$NODE" == "slave1" ]; then
    SUFFIX="11"
    HOST_PORT="2221"
# Slave2: SSH 2222, IP 192.168.1.12
elif [ "$NODE" == "slave2" ]; then
    SUFFIX="12"
    HOST_PORT="2222"
else
    echo " 错误: 未知节点名 '$NODE'"
    exit 1
fi

echo " 启动节点: $NODE (SSH端口: $HOST_PORT, 内部MAC后缀: $SUFFIX) ..."

# === 启动命令 (双网卡模式) ===
# net0 (User Mode): 负责连接外网 + 宿主机 SSH 端口转发
# net1 (Socket Mode): 负责集群内部多播通信 (模拟内网交换机)
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