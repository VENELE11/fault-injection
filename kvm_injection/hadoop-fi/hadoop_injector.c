/*
 * hadoop_injector.c - Hadoop集群故障注入工具 (分布式控制完整版)
 *
 * 功能：针对Hadoop生态系统（HDFS/YARN/MapReduce）进行多层次故障注入
 * 支持：
 *   - 核心进程故障：NameNode, DataNode, ResourceManager, NodeManager
 *   - 任务进程故障：Map进程, Reduce进程
 *   - 网络通信故障：延迟、丢包、乱序、分区
 *   - 资源占用故障：CPU、内存耗尽
 *   - 心跳超时故障：模拟心跳检测失败
 *   - 分布式控制：支持在Master节点统一控制所有Slave节点
 *
 * 编译：gcc -o hadoop_injector hadoop_injector.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/sysinfo.h>

// ==========================================
// ===  集群配置 (请根据您的环境修改)     ===
// ==========================================
const char *SLAVE_HOSTS[] = {
    "192.168.1.11", // Slave1 IP
    "192.168.1.12"  // Slave2 IP
};
#define SLAVE_COUNT (sizeof(SLAVE_HOSTS) / sizeof(SLAVE_HOSTS[0]))

// 定义工具在所有节点上的绝对路径
#define REMOTE_TOOL_PATH "/root/hadoop-fi/hadoop_injector"
// ==========================================

// === Hadoop组件进程名定义 ===
// Hadoop 1.x 进程名
#define JOBTRACKER_PROC "JobTracker"
#define TASKTRACKER_PROC "TaskTracker"
// Hadoop 2.x/3.x 进程名
#define NAMENODE_PROC "NameNode"
#define DATANODE_PROC "DataNode"
#define RESOURCE_MGR_PROC "ResourceManager"
#define NODE_MGR_PROC "NodeManager"
#define SECONDARY_NN_PROC "SecondaryNameNode"
#define HISTORY_SERVER_PROC "JobHistoryServer"
// MapReduce任务进程
#define MAP_PROC "YarnChild"           // Map任务JVM进程
#define REDUCE_PROC "YarnChild"        // Reduce任务JVM进程
#define MR_APP_MASTER "MRAppMaster"    // MapReduce ApplicationMaster

// === Hadoop默认端口 ===
#define NAMENODE_RPC_PORT 8020
#define NAMENODE_HTTP_PORT 9870
#define DATANODE_DATA_PORT 9866
#define RESOURCEMANAGER_PORT 8088
#define NODEMANAGER_PORT 8042

// === 故障类型枚举 (扩展版) ===
typedef enum {
    HADOOP_FAULT_CRASH = 1,          // 进程崩溃 (SIGKILL)
    HADOOP_FAULT_HANG = 2,           // 进程挂起 (SIGSTOP)
    HADOOP_FAULT_RESUME = 3,         // 恢复进程 (SIGCONT)
    HADOOP_FAULT_NETWORK_DELAY = 4,  // 网络延迟 (tc netem)
    HADOOP_FAULT_NETWORK_LOSS = 5,   // 网络丢包 (tc netem)
    HADOOP_FAULT_NETWORK_PART = 6,   // 网络分区 (iptables)
    HADOOP_FAULT_NETWORK_REORDER = 7,// 网络乱序 (tc netem)
    HADOOP_FAULT_DISK_SLOW = 8,      // 磁盘IO慢 (cgroups限速)
    HADOOP_FAULT_DISK_FULL = 9,      // 磁盘空间耗尽
    HADOOP_FAULT_CPU_STRESS = 10,    // CPU资源耗尽
    HADOOP_FAULT_MEM_STRESS = 11,    // 内存资源耗尽
    HADOOP_FAULT_HEARTBEAT = 12,     // 心跳超时模拟
    HADOOP_FAULT_CORRUPT = 13        // 数据损坏模拟
} HadoopFaultType;

// === 组件类型枚举 ===
typedef enum {
    COMPONENT_ALL = 0,
    COMPONENT_NAMENODE = 1,
    COMPONENT_DATANODE = 2,
    COMPONENT_RESOURCE_MGR = 3,
    COMPONENT_NODE_MGR = 4,
    COMPONENT_SECONDARY_NN = 5,
    COMPONENT_HISTORY_SERVER = 6,
    // 新增：任务进程
    COMPONENT_MAP = 7,               // Map任务进程
    COMPONENT_REDUCE = 8,            // Reduce任务进程
    COMPONENT_APP_MASTER = 9,        // ApplicationMaster
    // Hadoop 1.x 兼容
    COMPONENT_JOBTRACKER = 10,
    COMPONENT_TASKTRACKER = 11
} HadoopComponent;

// === 全局变量：资源压力控制 ===
static volatile int g_stress_running = 0;
static pthread_t *g_stress_threads = NULL;
static int g_stress_thread_count = 0;

// === 核心：获取进程真实状态 (新增) ===
// 必须定义在 print/list 函数之前
char get_proc_state(int pid)
{
    char path[128], buf[256];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return 0;

    // /proc/PID/stat 格式: pid (filename) state ...
    // 读取第三个字段 state
    if (fgets(buf, sizeof(buf), fp))
    {
        char *p = strrchr(buf, ')'); // 找到文件名结束的右括号
        if (p && strlen(p) >= 2)
        {
            fclose(fp);
            // ')' 后面是一个空格，然后是状态字符
            return p[2];
        }
    }
    fclose(fp);
    return 0;
}

// === 辅助函数：获取组件进程名 ===
const char* get_component_name(HadoopComponent component) {
    switch (component) {
        case COMPONENT_NAMENODE: return NAMENODE_PROC;
        case COMPONENT_DATANODE: return DATANODE_PROC;
        case COMPONENT_RESOURCE_MGR: return RESOURCE_MGR_PROC;
        case COMPONENT_NODE_MGR: return NODE_MGR_PROC;
        case COMPONENT_SECONDARY_NN: return SECONDARY_NN_PROC;
        case COMPONENT_HISTORY_SERVER: return HISTORY_SERVER_PROC;
        case COMPONENT_MAP: return MAP_PROC;
        case COMPONENT_REDUCE: return REDUCE_PROC;
        case COMPONENT_APP_MASTER: return MR_APP_MASTER;
        case COMPONENT_JOBTRACKER: return JOBTRACKER_PROC;
        case COMPONENT_TASKTRACKER: return TASKTRACKER_PROC;
        default: return NULL;
    }
}

// === 辅助函数：判断组件是否通常运行在 Slave 节点上 ===
int is_slave_component(HadoopComponent component)
{
    if (component == COMPONENT_DATANODE ||
        component == COMPONENT_NODE_MGR ||
        component == COMPONENT_MAP ||
        component == COMPONENT_REDUCE ||
        component == COMPONENT_TASKTRACKER)
    {
        return 1;
    }
    return 0;
}

// === 辅助函数：查找Hadoop进程PID ===
int find_hadoop_pid(const char *proc_name) {
    char cmd[256];
    char output[32];
    
    // 针对 NameNode 特殊处理，防止匹配到 SecondaryNameNode
    if (strcmp(proc_name, "NameNode") == 0)
    {
        snprintf(cmd, sizeof(cmd),
                 "jps -l 2>/dev/null | grep 'NameNode' | grep -v 'Secondary' | awk '{print $1}' | head -n 1");
    }
    else
    {
        // 使用jps命令查找Java进程
        snprintf(cmd, sizeof(cmd), 
                 "jps -l 2>/dev/null | grep %s | awk '{print $1}' | head -n 1", 
                 proc_name);
    }
    
    FILE *fp = popen(cmd, "r");
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL) {
        pclose(fp);
        return atoi(output);
    }
    if (fp) pclose(fp);
    
    // 备用方案：使用pgrep
    snprintf(cmd, sizeof(cmd), 
             "pgrep -f 'java.*%s' | head -n 1", 
             proc_name);
    
    fp = popen(cmd, "r");
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL) {
        pclose(fp);
        return atoi(output);
    }
    if (fp) pclose(fp);
    
    return -1;
}

// === 辅助函数：查找所有Map/Reduce任务进程 ===
int* find_mapreduce_pids(const char *task_type, int *count) {
    char cmd[512];
    char output[1024];
    static int pids[100];
    *count = 0;
    
    if (strcmp(task_type, "map") == 0) {
        snprintf(cmd, sizeof(cmd),
                 "ps aux | grep 'YarnChild' | grep -v grep | awk '{print $2}'");
    } else {
        snprintf(cmd, sizeof(cmd),
                 "ps aux | grep 'YarnChild' | grep -v grep | awk '{print $2}'");
    }
    
    FILE *fp = popen(cmd, "r");
    if (fp) {
        while (fgets(output, sizeof(output), fp) != NULL && *count < 100) {
            int pid = atoi(output);
            if (pid > 0) {
                pids[(*count)++] = pid;
            }
        }
        pclose(fp);
    }
    
    return pids;
}

// === 辅助函数：获取默认网卡名 ===
void get_default_nic(char *nic, size_t size) {
    FILE *fp = popen("ip route get 8.8.8.8 2>/dev/null | awk '{print $5; exit}'", "r");
    if (fp == NULL || fgets(nic, size, fp) == NULL) {
        strcpy(nic, "eth0");
    } else {
        nic[strcspn(nic, "\n")] = 0;
    }
    if (fp) pclose(fp);
}

// === 远程执行辅助函数 ===
int exec_remote_injector(const char *host, const char *args)
{
    char cmd[1024];
    printf(" [Remote] 连接到 %s 执行命令...\n", host);
    snprintf(cmd, sizeof(cmd), "ssh -o StrictHostKeyChecking=no root@%s '%s %s'",
             host, REMOTE_TOOL_PATH, args);
    return system(cmd);
}

// === 辅助函数：列出本机进程 (Renamed from list_hadoop_processes) ===
// 改名为 list_local_processes 并增加状态显示
void list_local_processes(const char *hostname_prefix) {
    char hostname[64];
    if (hostname_prefix) {
        strcpy(hostname, hostname_prefix);
    } else {
        gethostname(hostname, sizeof(hostname));
    }
    printf("--- 节点: %s ---\n", hostname);
    
    // HDFS组件
    const char* hdfs_components[] = {NAMENODE_PROC, SECONDARY_NN_PROC, DATANODE_PROC};
    const char* hdfs_names[] = {"NameNode", "SecondaryNN", "DataNode"};
    
    for (int i = 0; i < 3; i++) {
        int pid = find_hadoop_pid(hdfs_components[i]);
        if (pid > 0) {
             // 获取真实状态字符
            char state = get_proc_state(pid);
            const char *status_str = "[RUNNING]";
            if (state == 'T' || state == 't') status_str = "[STOPPED]";
            else if (state == 'D') status_str = "[DISK WAIT]";
            else if (state == 'Z') status_str = "[ZOMBIE]";

            printf("    %-15s PID: %-6d %s\n", hdfs_names[i], pid, status_str);
        }
    }
    
    // YARN组件
    const char* yarn_components[] = {RESOURCE_MGR_PROC, NODE_MGR_PROC, HISTORY_SERVER_PROC};
    const char* yarn_names[] = {"ResManager", "NodeManager", "HistoryServer"};
    
    for (int i = 0; i < 3; i++) {
        int pid = find_hadoop_pid(yarn_components[i]);
        if (pid > 0) {
            char state = get_proc_state(pid);
            const char *status_str = "[RUNNING]";
            if (state == 'T' || state == 't') status_str = "[STOPPED]";
            else if (state == 'D') status_str = "[DISK WAIT]";
            else if (state == 'Z') status_str = "[ZOMBIE]";

            printf("    %-15s PID: %-6d %s\n", yarn_names[i], pid, status_str);
        }
    }
    
    // MapReduce任务
    int count = 0;
    find_mapreduce_pids("map", &count);
    if (count > 0) {
        printf("    YarnChild任务进程数量: %-3d\n", count);
    }
    
    int am_pid = find_hadoop_pid(MR_APP_MASTER);
    if (am_pid > 0) {
        char state = get_proc_state(am_pid);
        const char *status_str = "[RUNNING]";
        if (state == 'T' || state == 't') status_str = "[STOPPED]";
        printf("    MRAppMaster      PID: %-6d %s\n", am_pid, status_str);
    }
    printf("\n");
}

// === 辅助函数：列出集群所有进程 ===
void list_cluster_processes()
{
    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║              全集群 Hadoop 进程状态一览                      ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");

    // 1. 本机 (Master)
    list_local_processes("Master (Local)");

    // 2. 远程 Slaves
    for (int i = 0; i < SLAVE_COUNT; i++)
    {
        char cmd[1024];
        // 远程调用 list-local 命令
        snprintf(cmd, sizeof(cmd), "ssh -o StrictHostKeyChecking=no root@%s '%s list-local'",
                 SLAVE_HOSTS[i], REMOTE_TOOL_PATH);

        printf("正在查询 Slave: %s ...\n", SLAVE_HOSTS[i]);
        system(cmd);
    }
    printf("╚══════════════════════════════════════════════════════════════╝\n");
}

// === 模块1：进程故障注入 (纯本地执行) ===
// 此函数对应原文件的 inject_process_fault，但我们将其重命名为本地执行版本
int exec_local_process_fault(HadoopComponent component, HadoopFaultType fault_type) {
    const char *proc_name = get_component_name(component);
    if (!proc_name) {
        printf(" 无效的组件类型\n");
        return -1;
    }
    
    int pid = find_hadoop_pid(proc_name);
    if (pid <= 0) {
        printf(" [Local] 未找到进程: %s\n", proc_name);
        return 0; // 不报错，因为可能在该节点未运行
    }
    
    printf("[Local] 目标: %s (PID: %d)\n", proc_name, pid);
    
    switch (fault_type) {
        case HADOOP_FAULT_CRASH:
            if (kill(pid, SIGKILL) == 0) {
                printf(" [Crash] 已终止进程 %s\n", proc_name);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        case HADOOP_FAULT_HANG:
            if (kill(pid, SIGSTOP) == 0) {
                printf("  [Hang] 已暂停进程 %s\n", proc_name);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        case HADOOP_FAULT_RESUME:
            if (kill(pid, SIGCONT) == 0) {
                printf("  [Resume] 已恢复进程 %s\n", proc_name);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        default:
            printf(" 此故障类型不支持进程操作\n");
            return -1;
    }
    
    return 0;
}

// === 故障注入路由 (含自动分发) ===
// 参数 use_local_only: 强制只在本地执行，不递归分发
int inject_process_fault_distributed(const char *comp_str, HadoopComponent comp, HadoopFaultType fault_type, int use_local_only)
{
    // 1. 如果指定了 use_local_only (带有 -local 后缀的指令)，则只执行本地操作
    if (use_local_only)
    {
        return exec_local_process_fault(comp, fault_type);
    }

    // 2. 否则，根据组件类型决定是本地执行还是远程分发
    if (is_slave_component(comp))
    {
        // Slave 组件：分发给所有 Slave 节点
        // 为了防止死循环，发送给 slave 的命令必须加上 "-local"
        char args[128];
        const char *action_str = "";
        switch (fault_type)
        {
        case HADOOP_FAULT_CRASH:
            action_str = "crash-local";
            break;
        case HADOOP_FAULT_HANG:
            action_str = "hang-local";
            break;
        case HADOOP_FAULT_RESUME:
            action_str = "resume-local";
            break;
        default:
            action_str = "unknown";
            break;
        }

        snprintf(args, sizeof(args), "%s %s", action_str, comp_str);

        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            exec_remote_injector(SLAVE_HOSTS[i], args);
        }
        return 0;
    }
    else
    {
        // Master 组件：直接在本地执行
        return exec_local_process_fault(comp, fault_type);
    }
}

// === 模块2：网络故障注入 ===
int inject_network_fault(const char *target_ip, int port, int action) {
    char cmd[512];
    
    if (action == 0) {
        // 清理规则
        snprintf(cmd, sizeof(cmd),
                 "iptables -D INPUT -s %s -j DROP 2>/dev/null; "
                 "iptables -D OUTPUT -d %s -j DROP 2>/dev/null",
                 target_ip, target_ip);
        system(cmd);
        printf(" 已清理与 %s 的网络隔离\n", target_ip);
    } else {
        // 注入网络分区
        if (port > 0) {
            snprintf(cmd, sizeof(cmd),
                     "iptables -A INPUT -s %s -p tcp --dport %d -j DROP; "
                     "iptables -A OUTPUT -d %s -p tcp --sport %d -j DROP",
                     target_ip, port, target_ip, port);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "iptables -A INPUT -s %s -j DROP; "
                     "iptables -A OUTPUT -d %s -j DROP",
                     target_ip, target_ip);
        }
        
        if (system(cmd) == 0) {
            if (port > 0) {
                printf(" [Network Partition] 已隔离 %s 端口 %d\n", target_ip, port);
            } else {
                printf(" [Network Partition] 已完全隔离节点 %s\n", target_ip);
            }
        } else {
            printf("  网络隔离命令执行失败\n");
            return -1;
        }
    }
    
    return 0;
}

// === 模块2.1：网络延迟注入 ===
int inject_network_delay(const char *target_ip, int delay_ms, int jitter_ms) {
    char cmd[512];
    char nic[32];
    
    get_default_nic(nic, sizeof(nic));
    
    // 清理旧规则
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);
    
    if (delay_ms <= 0) {
        printf(" [Network] 已清理网络延迟\n");
        return 0;
    }
    
    if (target_ip && strlen(target_ip) > 0) {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root handle 1: prio; "
                 "tc qdisc add dev %s parent 1:3 handle 30: netem delay %dms %dms; "
                 "tc filter add dev %s parent 1:0 protocol ip prio 3 u32 "
                 "match ip dst %s flowid 1:3",
                 nic, nic, delay_ms, jitter_ms, nic, target_ip);
        printf(" [Network Delay] 对 %s 注入 %dms%dms 延迟\n", target_ip, delay_ms, jitter_ms);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem delay %dms %dms",
                 nic, delay_ms, jitter_ms);
        printf(" [Network Delay] 全局注入 %dms%dms 延迟\n", delay_ms, jitter_ms);
    }
    
    return system(cmd);
}

// === 模块2.2：网络丢包注入 ===
int inject_network_loss(const char *target_ip, int loss_percent) {
    char cmd[512];
    char nic[32];
    
    get_default_nic(nic, sizeof(nic));
    
    // 清理旧规则
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);
    
    if (loss_percent <= 0) {
        printf(" [Network] 已清理网络丢包\n");
        return 0;
    }
    
    if (target_ip && strlen(target_ip) > 0) {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root handle 1: prio; "
                 "tc qdisc add dev %s parent 1:3 handle 30: netem loss %d%%; "
                 "tc filter add dev %s parent 1:0 protocol ip prio 3 u32 "
                 "match ip dst %s flowid 1:3",
                 nic, nic, loss_percent, nic, target_ip);
        printf(" [Network Loss] 对 %s 注入 %d%% 丢包率\n", target_ip, loss_percent);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem loss %d%%",
                 nic, loss_percent);
        printf(" [Network Loss] 全局注入 %d%% 丢包率\n", loss_percent);
    }
    
    return system(cmd);
}

// === 模块2.3：网络乱序注入 ===
int inject_network_reorder(const char *target_ip, int reorder_percent, int correlation) {
    char cmd[512];
    char nic[32];
    
    get_default_nic(nic, sizeof(nic));
    
    // 清理旧规则
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);
    
    if (reorder_percent <= 0) {
        printf(" [Network] 已清理网络乱序\n");
        return 0;
    }
    
    snprintf(cmd, sizeof(cmd),
             "tc qdisc add dev %s root netem delay 10ms reorder %d%% %d%%",
             nic, reorder_percent, correlation);
    printf(" [Network Reorder] 注入 %d%% 乱序率 (相关性 %d%%)\n", reorder_percent, correlation);
    
    return system(cmd);
}

// === 模块3：HDFS相关故障 ===
int inject_hdfs_fault(int fault_type, const char *param) {
    char cmd[1024];
    
    switch (fault_type) {
        case 1: snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -safemode enter"); break;
        case 2: snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -safemode leave"); break;
        case 3: 
            if (param) 
                snprintf(cmd, sizeof(cmd), "dd if=/dev/zero of=/tmp/hdfs_disk_fill bs=1M count=%s", param); 
            else return -1;
            break;
        case 4: snprintf(cmd, sizeof(cmd), "rm -f /tmp/hdfs_disk_fill"); break;
        case 5: snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -refreshNodes"); break;
        default: return -1;
    }
    
    return system(cmd);
}

// === 模块4：YARN资源故障 ===
int inject_yarn_fault(int fault_type, const char *node_ip) {
    char cmd[1024];
    switch (fault_type) {
        case 1: snprintf(cmd, sizeof(cmd), "echo 'ERROR' > /tmp/yarn_node_health_check"); break;
        case 2: snprintf(cmd, sizeof(cmd), "rm -f /tmp/yarn_node_health_check"); break;
        case 3: snprintf(cmd, sizeof(cmd), "yarn rmadmin -refreshNodes"); break;
        case 4: snprintf(cmd, sizeof(cmd), "yarn rmadmin -refreshQueues"); break;
        default: return -1;
    }
    return system(cmd);
}

// === 模块5：IO延迟注入 ===
int inject_io_delay(const char *mount_point, int delay_ms) {
    char cmd[512];
    if (delay_ms > 0) {
        snprintf(cmd, sizeof(cmd),
                 "echo '8:0 rbps=1048576 wbps=1048576' > "
                 "/sys/fs/cgroup/blkio/blkio.throttle.read_bps_device 2>/dev/null");
    } else {
        snprintf(cmd, sizeof(cmd),
                 "echo '' > /sys/fs/cgroup/blkio/blkio.throttle.read_bps_device 2>/dev/null");
    }
    system(cmd);
    return 0;
}

// === 模块6：CPU资源耗尽注入 ===
void* cpu_stress_worker(void *arg) {
    double x = 0.0;
    while (g_stress_running) {
        x = x + 0.1;
        if (x > 1000000) x = 0;
    }
    return NULL;
}

int inject_cpu_stress(int duration_sec, int num_threads) {
    if (num_threads <= 0) num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    
    printf(" [CPU Stress] 启动 %d 个线程, 持续 %d 秒\n", num_threads, duration_sec);
    
    g_stress_running = 1;
    g_stress_thread_count = num_threads;
    g_stress_threads = malloc(num_threads * sizeof(pthread_t));
    
    for (int i = 0; i < num_threads; i++) {
        pthread_create(&g_stress_threads[i], NULL, cpu_stress_worker, NULL);
    }
    
    sleep(duration_sec);
    
    g_stress_running = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(g_stress_threads[i], NULL);
    }
    
    free(g_stress_threads);
    g_stress_threads = NULL;
    return 0;
}

// === 模块7：内存资源耗尽注入 ===
int inject_memory_stress(int size_mb) {
    char cmd[256];
    if (size_mb <= 0) {
        snprintf(cmd, sizeof(cmd), "rm -f /tmp/hadoop_mem_stress 2>/dev/null");
        system(cmd);
        return 0;
    }
    
    snprintf(cmd, sizeof(cmd),
             "dd if=/dev/zero of=/tmp/hadoop_mem_stress bs=1M count=%d 2>/dev/null",
             size_mb);
    int ret = system(cmd);
    if (ret == 0) {
        snprintf(cmd, sizeof(cmd), "cat /tmp/hadoop_mem_stress > /dev/null &");
        system(cmd);
    }
    return ret;
}

// === 模块8：心跳超时模拟 ===
int inject_heartbeat_timeout(const char *node_ip, int timeout_ms) {
    char cmd[512];
    char nic[32];
    get_default_nic(nic, sizeof(nic));
    
    if (timeout_ms <= 0) {
        snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
        system(cmd);
        return 0;
    }
    
    if (node_ip && strlen(node_ip) > 0) {
        inject_network_delay(node_ip, timeout_ms, 0);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem delay %dms", nic, timeout_ms);
        system(cmd);
    }
    return 0;
}

void print_usage(const char *prog) {
    printf("Usage: %s <action> [component] [options]\n", prog);
    printf("Actions:\n");
    printf("  list                        List all Hadoop processes in cluster\n");
    printf("  list-local                  (Internal) List processes on current node only\n");
    printf("  crash <comp>                Kill component process\n");
    printf("  hang <comp>                 Pause component process (SIGSTOP)\n");
    printf("  resume <comp>               Resume component process (SIGCONT)\n");
    printf("  // Internal commands to prevent recursion:\n");
    printf("  crash-local <comp>\n");
    printf("  hang-local <comp>\n");
    printf("  resume-local <comp>\n");
    printf("\n  ... (Other resource faults supported: cpu-stress, mem-stress, network, etc.)\n");
}

HadoopComponent parse_component(const char *arg) {
    if (strcmp(arg, "nn") == 0) return COMPONENT_NAMENODE;
    if (strcmp(arg, "dn") == 0) return COMPONENT_DATANODE;
    if (strcmp(arg, "rm") == 0) return COMPONENT_RESOURCE_MGR;
    if (strcmp(arg, "nm") == 0) return COMPONENT_NODE_MGR;
    if (strcmp(arg, "snn") == 0) return COMPONENT_SECONDARY_NN;
    if (strcmp(arg, "jhs") == 0) return COMPONENT_HISTORY_SERVER;
    if (strcmp(arg, "map") == 0) return COMPONENT_MAP;
    if (strcmp(arg, "reduce") == 0) return COMPONENT_REDUCE;
    if (strcmp(arg, "am") == 0) return COMPONENT_APP_MASTER;
    return COMPONENT_ALL;
}
// === Map/Reduce任务进程故障注入 ===
int inject_mapreduce_fault(const char *task_type, HadoopFaultType fault_type) {
    int count = 0;
    int *pids = find_mapreduce_pids(task_type, &count);
    
    if (count == 0) {
        printf(" 未找到运行中的 %s 任务进程\n", task_type);
        return -1;
    }
    
    srand(time(NULL));
    int target_idx = rand() % count;
    int target_pid = pids[target_idx];
    
    switch (fault_type) {
        case HADOOP_FAULT_CRASH:
            kill(target_pid, SIGKILL); break;
        case HADOOP_FAULT_HANG:
            kill(target_pid, SIGSTOP); break;
        default: break;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_usage(argv[0]);
        return 1;
    }

    const char *action = argv[1];

    if (strcmp(action, "list") == 0)
    {
        list_cluster_processes();
        return 0;
    }
    else if (strcmp(action, "list-local") == 0)
    {
        // 仅列出本机
        list_local_processes(NULL);
        return 0;
    }

    // === 进程故障指令处理 ===
    if (strcmp(action, "crash") == 0 || strcmp(action, "hang") == 0 || strcmp(action, "resume") == 0 ||
        strcmp(action, "crash-local") == 0 || strcmp(action, "hang-local") == 0 || strcmp(action, "resume-local") == 0)
    {
        if (argc < 3) { print_usage(argv[0]); return 1; }
        
        HadoopComponent comp = parse_component(argv[2]);
        HadoopFaultType type = 0;
        int local_mode = 0;

        if (strncmp(action, "crash", 5) == 0) type = HADOOP_FAULT_CRASH;
        else if (strncmp(action, "hang", 4) == 0) type = HADOOP_FAULT_HANG;
        else if (strncmp(action, "resume", 6) == 0) type = HADOOP_FAULT_RESUME;

        // 检查是否为内部 local 指令
        if (strstr(action, "-local")) local_mode = 1;

        inject_process_fault_distributed(argv[2], comp, type, local_mode);
        return 0;
    }

    // === 其他资源故障 (保持原有单机/主控执行方式) ===
    // 对于资源类故障，简单起见目前暂不分发，如果需要分发可以参考 inject_process_fault_distributed 改造
    
    if (strcmp(action, "network") == 0) {
        if (argc < 3) return 1;
        int port = (argc >= 4) ? atoi(argv[3]) : 0;
        inject_network_fault(argv[2], port, 1);
    }
    else if (strcmp(action, "network-clear") == 0) {
        if (argc < 3) return 1;
        inject_network_fault(argv[2], 0, 0);
    }
    else if (strcmp(action, "delay") == 0) {
        if (argc < 4) return 1;
        inject_network_delay(argv[2], atoi(argv[3]), (argc >= 5) ? atoi(argv[4]) : 0);
    }
    else if (strcmp(action, "cpu-stress") == 0) {
        if (argc < 3) return 1;
        inject_cpu_stress(atoi(argv[2]), (argc >= 4) ? atoi(argv[3]) : 0);
    }
    else if (strcmp(action, "mem-stress") == 0) {
         if (argc < 3) return 1;
         inject_memory_stress(atoi(argv[2]));
    }
    else if (strcmp(action, "mem-stress-clear") == 0) {
         inject_memory_stress(0);
    }
    else {
        printf("Unknown command: %s\n", action);
        print_usage(argv[0]);
    }

    return 0;
}
