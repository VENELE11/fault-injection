#!/bin/bash

# 检查输入参数
if [ -z "$1" ]; then
    echo "用法: ./run_cluster.sh <master|slave1|slave2>"
    exit 1
fi

NODE=$1
# 为不同节点分配不同的 MAC 地址后缀，防止网络冲突
if [ "$NODE" == "master" ]; then SUFFIX="10";
elif [ "$NODE" == "slave1" ]; then SUFFIX="11";
elif [ "$NODE" == "slave2" ]; then SUFFIX="12";
else SUFFIX="99"; fi

echo "🚀 正在以图形模式启动节点: $NODE ..."

qemu-system-aarch64 \
  -name "alpine_$NODE" \
  -M virt,accel=kvm \
  -cpu host \
  -m 1024 \
  -bios ./uefi.fd \
  -drive file=images/node_$NODE.qcow2,format=qcow2,if=virtio \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:$SUFFIX \
  -device virtio-gpu-pci \
  -device qemu-xhci \
  -device usb-kbd \
  -device usb-tablet \
  -boot menu=on
