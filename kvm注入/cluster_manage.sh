#!/bin/bash
# cluster_manage.sh - Hadoop集群管理脚本
# 用途：管理UTM/QEMU虚拟机上的3节点Hadoop集群
# 支持：启动、停止、状态检查、故障注入

# === 配置区 ===
# 节点配置（根据实际环境修改）
MASTER_IP="192.168.64.10"
SLAVE1_IP="192.168.64.11"
SLAVE2_IP="192.168.64.12"

# SSH配置
SSH_USER="root"
SSH_KEY=""  # 如果使用密钥，设置路径如 ~/.ssh/id_rsa
SSH_TIMEOUT=5

# Hadoop路径（根据实际安装路径修改）
HADOOP_HOME="/opt/hadoop"
HADOOP_SBIN="$HADOOP_HOME/sbin"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# === 函数定义 ===

# 打印带颜色的消息
print_info() { echo -e "${BLUE}[INFO]${NC} $1"; }
print_success() { echo -e "${GREEN}[OK]${NC} $1"; }
print_warning() { echo -e "${YELLOW}[WARN]${NC} $1"; }
print_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# SSH命令封装
ssh_exec() {
    local host=$1
    local cmd=$2
    local ssh_opts="-o StrictHostKeyChecking=no -o ConnectTimeout=$SSH_TIMEOUT"
    
    if [ -n "$SSH_KEY" ]; then
        ssh_opts="$ssh_opts -i $SSH_KEY"
    fi
    
    ssh $ssh_opts $SSH_USER@$host "$cmd" 2>/dev/null
}

# 检查节点连通性
check_node_connectivity() {
    local host=$1
    local name=$2
    
    if ping -c 1 -W 2 $host > /dev/null 2>&1; then
        print_success "$name ($host) 网络可达"
        return 0
    else
        print_error "$name ($host) 网络不可达"
        return 1
    fi
}

# 检查节点SSH连接
check_ssh() {
    local host=$1
    local name=$2
    
    if ssh_exec $host "echo ok" > /dev/null 2>&1; then
        print_success "$name SSH连接正常"
        return 0
    else
        print_error "$name SSH连接失败"
        return 1
    fi
}

# 检查Hadoop进程
check_hadoop_process() {
    local host=$1
    local process=$2
    
    local pid=$(ssh_exec $host "jps 2>/dev/null | grep $process | awk '{print \$1}'")
    
    if [ -n "$pid" ]; then
        echo "$pid"
        return 0
    else
        echo ""
        return 1
    fi
}

# 显示集群状态
show_status() {
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║                 Hadoop 集群状态检查                        ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""
    
    # 检查网络连通性
    echo "=== 网络连通性 ==="
    check_node_connectivity $MASTER_IP "Master"
    check_node_connectivity $SLAVE1_IP "Slave1"
    check_node_connectivity $SLAVE2_IP "Slave2"
    echo ""
    
    # 检查SSH
    echo "=== SSH连接状态 ==="
    check_ssh $MASTER_IP "Master"
    check_ssh $SLAVE1_IP "Slave1"
    check_ssh $SLAVE2_IP "Slave2"
    echo ""
    
    # 检查Hadoop进程
    echo "=== Hadoop进程状态 ==="
    
    # Master节点
    echo "--- Master ($MASTER_IP) ---"
    for proc in NameNode SecondaryNameNode ResourceManager; do
        pid=$(check_hadoop_process $MASTER_IP $proc)
        if [ -n "$pid" ]; then
            print_success "$proc (PID: $pid)"
        else
            print_error "$proc 未运行"
        fi
    done
    
    # Slave1节点
    echo "--- Slave1 ($SLAVE1_IP) ---"
    for proc in DataNode NodeManager; do
        pid=$(check_hadoop_process $SLAVE1_IP $proc)
        if [ -n "$pid" ]; then
            print_success "$proc (PID: $pid)"
        else
            print_error "$proc 未运行"
        fi
    done
    
    # Slave2节点
    echo "--- Slave2 ($SLAVE2_IP) ---"
    for proc in DataNode NodeManager; do
        pid=$(check_hadoop_process $SLAVE2_IP $proc)
        if [ -n "$pid" ]; then
            print_success "$proc (PID: $pid)"
        else
            print_error "$proc 未运行"
        fi
    done
    
    echo ""
}

# 启动Hadoop集群
start_cluster() {
    print_info "启动Hadoop集群..."
    
    # 检查Master连通性
    if ! check_node_connectivity $MASTER_IP "Master" > /dev/null 2>&1; then
        print_error "Master节点不可达，无法启动集群"
        return 1
    fi
    
    # 启动HDFS
    print_info "启动HDFS..."
    ssh_exec $MASTER_IP "$HADOOP_SBIN/start-dfs.sh"
    
    sleep 3
    
    # 启动YARN
    print_info "启动YARN..."
    ssh_exec $MASTER_IP "$HADOOP_SBIN/start-yarn.sh"
    
    sleep 3
    
    print_success "集群启动命令已发送"
    show_status
}

# 停止Hadoop集群
stop_cluster() {
    print_info "停止Hadoop集群..."
    
    # 停止YARN
    print_info "停止YARN..."
    ssh_exec $MASTER_IP "$HADOOP_SBIN/stop-yarn.sh"
    
    sleep 2
    
    # 停止HDFS
    print_info "停止HDFS..."
    ssh_exec $MASTER_IP "$HADOOP_SBIN/stop-dfs.sh"
    
    sleep 2
    
    print_success "集群停止命令已发送"
}

# 重启Hadoop集群
restart_cluster() {
    stop_cluster
    sleep 5
    start_cluster
}

# 注入网络分区故障
inject_network_partition() {
    local target=$1
    
    if [ -z "$target" ]; then
        echo "用法: $0 inject-network <target_ip>"
        echo "示例: $0 inject-network $SLAVE1_IP"
        return 1
    fi
    
    print_warning "即将隔离节点: $target"
    
    # 使用iptables阻断与目标节点的通信
    sudo iptables -A INPUT -s $target -j DROP
    sudo iptables -A OUTPUT -d $target -j DROP
    
    print_success "已注入网络分区故障，节点 $target 被隔离"
}

# 清理网络分区故障
clear_network_partition() {
    local target=$1
    
    if [ -z "$target" ]; then
        # 清理所有规则
        print_info "清理所有网络隔离规则..."
        sudo iptables -F INPUT
        sudo iptables -F OUTPUT
    else
        # 清理特定节点
        sudo iptables -D INPUT -s $target -j DROP 2>/dev/null
        sudo iptables -D OUTPUT -d $target -j DROP 2>/dev/null
    fi
    
    print_success "网络隔离已清理"
}

# 注入进程崩溃故障
inject_process_crash() {
    local host=$1
    local process=$2
    
    if [ -z "$host" ] || [ -z "$process" ]; then
        echo "用法: $0 inject-crash <host_ip> <process_name>"
        echo "示例: $0 inject-crash $MASTER_IP NameNode"
        return 1
    fi
    
    print_warning "即将终止进程: $process on $host"
    
    local pid=$(check_hadoop_process $host $process)
    if [ -n "$pid" ]; then
        ssh_exec $host "kill -9 $pid"
        print_success "进程 $process (PID: $pid) 已被终止"
    else
        print_error "未找到进程 $process"
    fi
}

# 注入进程挂起故障
inject_process_hang() {
    local host=$1
    local process=$2
    
    if [ -z "$host" ] || [ -z "$process" ]; then
        echo "用法: $0 inject-hang <host_ip> <process_name>"
        echo "示例: $0 inject-hang $MASTER_IP NameNode"
        return 1
    fi
    
    print_warning "即将挂起进程: $process on $host"
    
    local pid=$(check_hadoop_process $host $process)
    if [ -n "$pid" ]; then
        ssh_exec $host "kill -STOP $pid"
        print_success "进程 $process (PID: $pid) 已被挂起"
    else
        print_error "未找到进程 $process"
    fi
}

# 恢复挂起的进程
resume_process() {
    local host=$1
    local process=$2
    
    if [ -z "$host" ] || [ -z "$process" ]; then
        echo "用法: $0 resume <host_ip> <process_name>"
        echo "示例: $0 resume $MASTER_IP NameNode"
        return 1
    fi
    
    local pid=$(ssh_exec $host "ps aux | grep $process | grep -v grep | awk '{print \$2}' | head -1")
    if [ -n "$pid" ]; then
        ssh_exec $host "kill -CONT $pid"
        print_success "进程 $process (PID: $pid) 已恢复"
    else
        print_error "未找到进程 $process"
    fi
}

# 注入网络延迟
inject_network_delay() {
    local delay=$1
    
    if [ -z "$delay" ]; then
        delay="100ms"
    fi
    
    local nic=$(ip route get 8.8.8.8 2>/dev/null | awk '{print $5; exit}')
    if [ -z "$nic" ]; then
        nic="eth0"
    fi
    
    print_info "在网卡 $nic 上注入 $delay 延迟..."
    
    sudo tc qdisc del dev $nic root 2>/dev/null
    sudo tc qdisc add dev $nic root netem delay $delay
    
    print_success "网络延迟 $delay 已注入"
}

# 清理网络延迟
clear_network_delay() {
    local nic=$(ip route get 8.8.8.8 2>/dev/null | awk '{print $5; exit}')
    if [ -z "$nic" ]; then
        nic="eth0"
    fi
    
    sudo tc qdisc del dev $nic root 2>/dev/null
    print_success "网络延迟已清理"
}

# 运行HDFS测试
run_hdfs_test() {
    print_info "执行HDFS基础测试..."
    
    # 创建测试目录
    ssh_exec $MASTER_IP "hdfs dfs -mkdir -p /test"
    
    # 上传测试文件
    ssh_exec $MASTER_IP "echo 'Hello Hadoop' > /tmp/test.txt && hdfs dfs -put -f /tmp/test.txt /test/"
    
    # 读取测试文件
    local content=$(ssh_exec $MASTER_IP "hdfs dfs -cat /test/test.txt")
    
    if [ "$content" == "Hello Hadoop" ]; then
        print_success "HDFS读写测试通过"
    else
        print_error "HDFS读写测试失败"
    fi
    
    # 清理
    ssh_exec $MASTER_IP "hdfs dfs -rm -r /test"
}

# 显示帮助
show_help() {
    echo ""
    echo "Hadoop集群管理脚本 - 支持故障注入"
    echo ""
    echo "用法: $0 <command> [options]"
    echo ""
    echo "集群管理命令:"
    echo "  status              显示集群状态"
    echo "  start               启动Hadoop集群"
    echo "  stop                停止Hadoop集群"
    echo "  restart             重启Hadoop集群"
    echo "  test                运行HDFS基础测试"
    echo ""
    echo "故障注入命令:"
    echo "  inject-network <ip>         网络分区 (隔离节点)"
    echo "  inject-crash <ip> <process> 进程崩溃"
    echo "  inject-hang <ip> <process>  进程挂起"
    echo "  inject-delay [ms]           网络延迟 (默认100ms)"
    echo ""
    echo "恢复命令:"
    echo "  clear-network [ip]          清理网络隔离"
    echo "  clear-delay                 清理网络延迟"
    echo "  resume <ip> <process>       恢复挂起的进程"
    echo ""
    echo "示例:"
    echo "  $0 status"
    echo "  $0 inject-network 192.168.64.11"
    echo "  $0 inject-crash 192.168.64.10 NameNode"
    echo "  $0 inject-delay 200ms"
    echo ""
}

# === 主逻辑 ===
case "$1" in
    status)
        show_status
        ;;
    start)
        start_cluster
        ;;
    stop)
        stop_cluster
        ;;
    restart)
        restart_cluster
        ;;
    test)
        run_hdfs_test
        ;;
    inject-network)
        inject_network_partition $2
        ;;
    inject-crash)
        inject_process_crash $2 $3
        ;;
    inject-hang)
        inject_process_hang $2 $3
        ;;
    inject-delay)
        inject_network_delay $2
        ;;
    clear-network)
        clear_network_partition $2
        ;;
    clear-delay)
        clear_network_delay
        ;;
    resume)
        resume_process $2 $3
        ;;
    help|-h|--help)
        show_help
        ;;
    *)
        show_help
        ;;
esac
