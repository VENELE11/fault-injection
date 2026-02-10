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
#define MAP_PROC "YarnChild"        // Map任务JVM进程
#define REDUCE_PROC "YarnChild"     // Reduce任务JVM进程
#define MR_APP_MASTER "MRAppMaster" // MapReduce ApplicationMaster

// === Hadoop默认端口 ===
#define NAMENODE_RPC_PORT 8020
#define NAMENODE_HTTP_PORT 9870
#define DATANODE_DATA_PORT 9866
#define RESOURCEMANAGER_PORT 8088
#define NODEMANAGER_PORT 8042

// === 故障类型枚举 (扩展版) ===
typedef enum
{
    HADOOP_FAULT_CRASH = 1,           // 进程崩溃 (SIGKILL)
    HADOOP_FAULT_HANG = 2,            // 进程挂起 (SIGSTOP)
    HADOOP_FAULT_RESUME = 3,          // 恢复进程 (SIGCONT)
    HADOOP_FAULT_NETWORK_DELAY = 4,   // 网络延迟 (tc netem)
    HADOOP_FAULT_NETWORK_LOSS = 5,    // 网络丢包 (tc netem)
    HADOOP_FAULT_NETWORK_PART = 6,    // 网络分区 (iptables)
    HADOOP_FAULT_NETWORK_REORDER = 7, // 网络乱序 (tc netem)
    HADOOP_FAULT_DISK_SLOW = 8,       // 磁盘IO慢 (cgroups限速)
    HADOOP_FAULT_DISK_FULL = 9,       // 磁盘空间耗尽
    HADOOP_FAULT_CPU_STRESS = 10,     // CPU资源耗尽
    HADOOP_FAULT_MEM_STRESS = 11,     // 内存资源耗尽
    HADOOP_FAULT_HEARTBEAT = 12,      // 心跳超时模拟
    HADOOP_FAULT_CORRUPT = 13         // 数据损坏模拟
} HadoopFaultType;

// === 组件类型枚举 ===
typedef enum
{
    COMPONENT_ALL = 0,
    COMPONENT_NAMENODE = 1,
    COMPONENT_DATANODE = 2,
    COMPONENT_RESOURCE_MGR = 3,
    COMPONENT_NODE_MGR = 4,
    COMPONENT_SECONDARY_NN = 5,
    COMPONENT_HISTORY_SERVER = 6,
    // 新增：任务进程
    COMPONENT_MAP = 7,        // Map任务进程
    COMPONENT_REDUCE = 8,     // Reduce任务进程
    COMPONENT_APP_MASTER = 9, // ApplicationMaster
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
const char *get_component_name(HadoopComponent component)
{
    switch (component)
    {
    case COMPONENT_NAMENODE:
        return NAMENODE_PROC;
    case COMPONENT_DATANODE:
        return DATANODE_PROC;
    case COMPONENT_RESOURCE_MGR:
        return RESOURCE_MGR_PROC;
    case COMPONENT_NODE_MGR:
        return NODE_MGR_PROC;
    case COMPONENT_SECONDARY_NN:
        return SECONDARY_NN_PROC;
    case COMPONENT_HISTORY_SERVER:
        return HISTORY_SERVER_PROC;
    case COMPONENT_MAP:
        return MAP_PROC;
    case COMPONENT_REDUCE:
        return REDUCE_PROC;
    case COMPONENT_APP_MASTER:
        return MR_APP_MASTER;
    case COMPONENT_JOBTRACKER:
        return JOBTRACKER_PROC;
    case COMPONENT_TASKTRACKER:
        return TASKTRACKER_PROC;
    default:
        return NULL;
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
int find_hadoop_pid(const char *proc_name)
{
    char cmd[256];
    char output[32];
    const char *full_class = NULL;

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
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL)
    {
        pclose(fp);
        return atoi(output);
    }
    if (fp)
        pclose(fp);

    // 备用方案：使用 ps + awk 精确匹配 Java 主类，避免 pgrep 误匹配自身
    if (strcmp(proc_name, "NameNode") == 0)
        full_class = "org.apache.hadoop.hdfs.server.namenode.NameNode";
    else if (strcmp(proc_name, "SecondaryNameNode") == 0)
        full_class = "org.apache.hadoop.hdfs.server.namenode.SecondaryNameNode";
    else if (strcmp(proc_name, "DataNode") == 0)
        full_class = "org.apache.hadoop.hdfs.server.datanode.DataNode";
    else if (strcmp(proc_name, "ResourceManager") == 0)
        full_class = "org.apache.hadoop.yarn.server.resourcemanager.ResourceManager";
    else if (strcmp(proc_name, "NodeManager") == 0)
        full_class = "org.apache.hadoop.yarn.server.nodemanager.NodeManager";
    else if (strcmp(proc_name, "JobHistoryServer") == 0)
        full_class = "org.apache.hadoop.mapreduce.v2.hs.JobHistoryServer";
    else if (strcmp(proc_name, "MRAppMaster") == 0)
        full_class = "org.apache.hadoop.mapreduce.v2.app.MRAppMaster";
    else if (strcmp(proc_name, "JobTracker") == 0)
        full_class = "org.apache.hadoop.mapred.JobTracker";
    else if (strcmp(proc_name, "TaskTracker") == 0)
        full_class = "org.apache.hadoop.mapred.TaskTracker";
    else
        full_class = proc_name;

    snprintf(cmd, sizeof(cmd),
             "ps -eo pid,args | awk -v pat='%s' '$0 ~ /java/ { for (i=2;i<=NF;i++) { if ($i == \"-cp\" || $i == \"-classpath\") { i++; continue; } if ($i ~ /^-/) { continue; } if ($i == pat) {print $1; exit;} break; } }'",
             full_class);

    fp = popen(cmd, "r");
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL)
    {
        pclose(fp);
        return atoi(output);
    }
    if (fp)
        pclose(fp);

    return -1;
}

// === 辅助函数：查找所有Map/Reduce任务进程 ===
int *find_mapreduce_pids(const char *task_type, int *count)
{
    char cmd[512];
    char output[1024];
    static int pids[100];
    *count = 0;

    // 使用 pgrep -f 搜索完整命令行，更可靠
    // Map 任务: attempt_..._m_
    // Reduce 任务: attempt_..._r_
    if (strcmp(task_type, "map") == 0)
    {
        // 搜索包含 YarnChild 且 attempt 参数中有 _m_ 的进程
        snprintf(cmd, sizeof(cmd),
                 "pgrep -f 'YarnChild.*attempt_.*_m_' 2>/dev/null");
    }
    else
    {
        // 搜索包含 YarnChild 且 attempt 参数中有 _r_ 的进程
        snprintf(cmd, sizeof(cmd),
                 "pgrep -f 'YarnChild.*attempt_.*_r_' 2>/dev/null");
    }

    FILE *fp = popen(cmd, "r");
    if (fp)
    {
        while (fgets(output, sizeof(output), fp) != NULL && *count < 100)
        {
            int pid = atoi(output);
            if (pid > 0)
            {
                pids[(*count)++] = pid;
            }
        }
        pclose(fp);
    }

    // 备用方案：如果 pgrep 没找到，尝试用 ps + grep 搜索完整命令行
    if (*count == 0)
    {
        if (strcmp(task_type, "map") == 0)
        {
            snprintf(cmd, sizeof(cmd),
                     "ps -eo pid,args | grep 'YarnChild' | grep 'attempt_.*_m_' | grep -v grep | awk '{print $1}'");
        }
        else
        {
            snprintf(cmd, sizeof(cmd),
                     "ps -eo pid,args | grep 'YarnChild' | grep 'attempt_.*_r_' | grep -v grep | awk '{print $1}'");
        }

        fp = popen(cmd, "r");
        if (fp)
        {
            while (fgets(output, sizeof(output), fp) != NULL && *count < 100)
            {
                int pid = atoi(output);
                if (pid > 0)
                {
                    pids[(*count)++] = pid;
                }
            }
            pclose(fp);
        }
    }

    return pids;
}

// === 辅助函数：获取默认网卡名 ===
/*void get_default_nic(char *nic, size_t size) {
    FILE *fp = popen("ip route get 8.8.8.8 2>/dev/null | awk '{print $5; exit}'", "r");
    if (fp == NULL || fgets(nic, size, fp) == NULL) {
        strcpy(nic, "eth0");
    } else {
        nic[strcspn(nic, "\n")] = 0;
    }
    if (fp) pclose(fp);
}*/
// === 辅助函数：获取默认网卡名 ===
void get_default_nic(char *nic, size_t size)
{
    // 强制使用集群通信网卡 eth1
    strncpy(nic, "eth1", size);
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
void list_local_processes(const char *hostname_prefix)
{
    char hostname[64];
    if (hostname_prefix)
    {
        strcpy(hostname, hostname_prefix);
    }
    else
    {
        gethostname(hostname, sizeof(hostname));
    }
    printf("--- 节点: %s ---\n", hostname);

    // HDFS组件
    const char *hdfs_components[] = {NAMENODE_PROC, SECONDARY_NN_PROC, DATANODE_PROC};
    const char *hdfs_names[] = {"NameNode", "SecondaryNN", "DataNode"};

    for (int i = 0; i < 3; i++)
    {
        int pid = find_hadoop_pid(hdfs_components[i]);
        if (pid > 0)
        {
            // 获取真实状态字符
            char state = get_proc_state(pid);
            const char *status_str = "[RUNNING]";
            if (state == 'T' || state == 't')
                status_str = "[STOPPED]";
            else if (state == 'D')
                status_str = "[DISK WAIT]";
            else if (state == 'Z')
                status_str = "[ZOMBIE]";

            printf("    %-15s PID: %-6d %s\n", hdfs_names[i], pid, status_str);
        }
    }

    // YARN组件
    const char *yarn_components[] = {RESOURCE_MGR_PROC, NODE_MGR_PROC, HISTORY_SERVER_PROC};
    const char *yarn_names[] = {"ResManager", "NodeManager", "HistoryServer"};

    for (int i = 0; i < 3; i++)
    {
        int pid = find_hadoop_pid(yarn_components[i]);
        if (pid > 0)
        {
            char state = get_proc_state(pid);
            const char *status_str = "[RUNNING]";
            if (state == 'T' || state == 't')
                status_str = "[STOPPED]";
            else if (state == 'D')
                status_str = "[DISK WAIT]";
            else if (state == 'Z')
                status_str = "[ZOMBIE]";

            printf("    %-15s PID: %-6d %s\n", yarn_names[i], pid, status_str);
        }
    }

    // MapReduce任务
    int count = 0;
    find_mapreduce_pids("map", &count);
    if (count > 0)
    {
        printf("    YarnChild任务进程数量: %-3d\n", count);
    }

    int am_pid = find_hadoop_pid(MR_APP_MASTER);
    if (am_pid > 0)
    {
        char state = get_proc_state(am_pid);
        const char *status_str = "[RUNNING]";
        if (state == 'T' || state == 't')
            status_str = "[STOPPED]";
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
int exec_local_process_fault(HadoopComponent component, HadoopFaultType fault_type)
{
    const char *proc_name = get_component_name(component);
    if (!proc_name)
    {
        printf(" 无效的组件类型\n");
        return -1;
    }

    int pid = find_hadoop_pid(proc_name);
    if (pid <= 0)
    {
        printf(" [Local] 未找到进程: %s\n", proc_name);
        return 0; // 不报错，因为可能在该节点未运行
    }

    printf("[Local] 目标: %s (PID: %d)\n", proc_name, pid);

    switch (fault_type)
    {
    case HADOOP_FAULT_CRASH:
        if (kill(pid, SIGKILL) == 0)
        {
            printf(" [Crash] 已终止进程 %s\n", proc_name);
        }
        else
        {
            perror("kill failed");
            return -1;
        }
        break;

    case HADOOP_FAULT_HANG:
        if (kill(pid, SIGSTOP) == 0)
        {
            printf("  [Hang] 已暂停进程 %s\n", proc_name);
        }
        else
        {
            perror("kill failed");
            return -1;
        }
        break;

    case HADOOP_FAULT_RESUME:
        if (kill(pid, SIGCONT) == 0)
        {
            printf("  [Resume] 已恢复进程 %s\n", proc_name);
        }
        else
        {
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
int inject_network_fault(const char *target_ip, int port, int action)
{
    char cmd[512];

    if (action == 0)
    {
        // 清理规则
        snprintf(cmd, sizeof(cmd),
                 "iptables -D INPUT -s %s -j DROP 2>/dev/null; "
                 "iptables -D OUTPUT -d %s -j DROP 2>/dev/null",
                 target_ip, target_ip);
        system(cmd);
        printf(" 已清理与 %s 的网络隔离\n", target_ip);
    }
    else
    {
        // 注入网络分区
        if (port > 0)
        {
            snprintf(cmd, sizeof(cmd),
                     "iptables -A INPUT -s %s -p tcp --dport %d -j DROP; "
                     "iptables -A OUTPUT -d %s -p tcp --sport %d -j DROP",
                     target_ip, port, target_ip, port);
        }
        else
        {
            snprintf(cmd, sizeof(cmd),
                     "iptables -A INPUT -s %s -j DROP; "
                     "iptables -A OUTPUT -d %s -j DROP",
                     target_ip, target_ip);
        }

        if (system(cmd) == 0)
        {
            if (port > 0)
            {
                printf(" [Network Partition] 已隔离 %s 端口 %d\n", target_ip, port);
            }
            else
            {
                printf(" [Network Partition] 已完全隔离节点 %s\n", target_ip);
            }
        }
        else
        {
            printf("  网络隔离命令执行失败\n");
            return -1;
        }
    }

    return 0;
}

// === 模块2.1：网络延迟注入 ===
int inject_network_delay(const char *target_ip, int delay_ms, int jitter_ms)
{
    char cmd[512];
    char nic[32];

    get_default_nic(nic, sizeof(nic));

    // 清理旧规则
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);

    if (delay_ms <= 0)
    {
        printf(" [Network] 已清理网络延迟\n");
        return 0;
    }

    if (target_ip && strlen(target_ip) > 0)
    {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root handle 1: prio; "
                 "tc qdisc add dev %s parent 1:3 handle 30: netem delay %dms %dms; "
                 "tc filter add dev %s parent 1:0 protocol ip prio 3 u32 "
                 "match ip dst %s flowid 1:3",
                 nic, nic, delay_ms, jitter_ms, nic, target_ip);
        printf(" [Network Delay] 对 %s 注入 %dms%dms 延迟\n", target_ip, delay_ms, jitter_ms);
    }
    else
    {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem delay %dms %dms",
                 nic, delay_ms, jitter_ms);
        printf(" [Network Delay] 全局注入 %dms%dms 延迟\n", delay_ms, jitter_ms);
    }

    return system(cmd);
}

// === 模块2.2：网络丢包注入 ===
int inject_network_loss(const char *target_ip, int loss_percent)
{
    char cmd[512];
    char nic[32];

    get_default_nic(nic, sizeof(nic));

    // 清理旧规则
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);

    if (loss_percent <= 0)
    {
        printf(" [Network] 已清理网络丢包\n");
        return 0;
    }

    if (target_ip && strlen(target_ip) > 0)
    {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root handle 1: prio; "
                 "tc qdisc add dev %s parent 1:3 handle 30: netem loss %d%%; "
                 "tc filter add dev %s parent 1:0 protocol ip prio 3 u32 "
                 "match ip dst %s flowid 1:3",
                 nic, nic, loss_percent, nic, target_ip);
        printf(" [Network Loss] 对 %s 注入 %d%% 丢包率\n", target_ip, loss_percent);
    }
    else
    {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem loss %d%%",
                 nic, loss_percent);
        printf(" [Network Loss] 全局注入 %d%% 丢包率\n", loss_percent);
    }

    return system(cmd);
}

// === 模块2.3：网络乱序注入 ===
int inject_network_reorder(const char *target_ip, int reorder_percent, int correlation)
{
    char cmd[512];
    char nic[32];

    get_default_nic(nic, sizeof(nic));

    // 清理旧规则
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);

    if (reorder_percent <= 0)
    {
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
int inject_hdfs_fault(int fault_type, const char *param)
{
    char cmd[1024];

    switch (fault_type)
    {
    case 1:
        snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -safemode enter");
        break;
    case 2:
        snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -safemode leave");
        break;
    case 3:
        if (param)
            snprintf(cmd, sizeof(cmd), "dd if=/dev/zero of=/tmp/hdfs_disk_fill bs=1M count=%s", param);
        else
            return -1;
        break;
    case 4:
        snprintf(cmd, sizeof(cmd), "rm -f /tmp/hdfs_disk_fill");
        break;
    case 5:
        snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -refreshNodes");
        break;
    default:
        return -1;
    }

    return system(cmd);
}

// === 模块4：YARN资源故障 ===
int inject_yarn_fault(int fault_type, const char *node_ip)
{
    char cmd[1024];
    switch (fault_type)
    {
    case 1:
        snprintf(cmd, sizeof(cmd), "echo 'ERROR' > /tmp/yarn_node_health_check");
        break;
    case 2:
        snprintf(cmd, sizeof(cmd), "rm -f /tmp/yarn_node_health_check");
        break;
    case 3:
        snprintf(cmd, sizeof(cmd), "yarn rmadmin -refreshNodes");
        break;
    case 4:
        snprintf(cmd, sizeof(cmd), "yarn rmadmin -refreshQueues");
        break;
    default:
        return -1;
    }
    return system(cmd);
}

// === 模块5：IO延迟注入 (Cgroup v2 + 设备号 253:0) ===
int inject_io_delay(int enable)
{
    if (enable > 0)
    {
        printf(" [IO Limit] 使用 cgroup v2 限制磁盘读写速度为 1MB/s\n");

        // 使用单个 system() 调用，确保命令按顺序在同一个 shell 中执行
        int ret = system(
            "set -e; " // 遇到错误立即退出
            // 1) 启用 io 控制器
            "echo '+io' > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null || true; "
            // 2) 创建子 cgroup
            "mkdir -p /sys/fs/cgroup/io_limited; "
            // 3) 设置限速 - 设备号 253:0
            "echo '253:0 rbps=1048576 wbps=1048576' > /sys/fs/cgroup/io_limited/io.max; "
            // 4) 将所有 Java 进程加入限速 cgroup
            "for pid in $(pgrep -f java 2>/dev/null); do "
            "  echo $pid > /sys/fs/cgroup/io_limited/cgroup.procs 2>/dev/null || true; "
            "done; "
            "echo '[IO] 限速已启用 (253:0, 1MB/s)'");

        if (ret != 0)
        {
            printf(" [IO Limit] 警告：部分命令可能执行失败\n");
        }
    }
    else
    {
        printf(" [IO Limit] 解除磁盘限速\n");

        system(
            // 将进程移回根 cgroup
            "for pid in $(cat /sys/fs/cgroup/io_limited/cgroup.procs 2>/dev/null); do "
            "  echo $pid > /sys/fs/cgroup/cgroup.procs 2>/dev/null || true; "
            "done; "
            // 删除子 cgroup
            "rmdir /sys/fs/cgroup/io_limited 2>/dev/null || true; "
            "echo '[IO] 限速已解除'");
    }
    return 0;
}

// === 模块6：CPU资源耗尽注入 ===
void *cpu_stress_worker(void *arg)
{
    double x = 0.0;
    while (g_stress_running)
    {
        x = x + 0.1;
        if (x > 1000000)
            x = 0;
    }
    return NULL;
}

int inject_cpu_stress(int duration_sec, int num_threads)
{
    if (num_threads <= 0)
        num_threads = sysconf(_SC_NPROCESSORS_ONLN);

    printf(" [CPU Stress] 启动 %d 个线程, 持续 %d 秒\n", num_threads, duration_sec);

    g_stress_running = 1;
    g_stress_thread_count = num_threads;
    g_stress_threads = malloc(num_threads * sizeof(pthread_t));

    for (int i = 0; i < num_threads; i++)
    {
        pthread_create(&g_stress_threads[i], NULL, cpu_stress_worker, NULL);
    }

    sleep(duration_sec);

    g_stress_running = 0;
    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(g_stress_threads[i], NULL);
    }

    free(g_stress_threads);
    g_stress_threads = NULL;
    return 0;
}

// === 模块7：内存资源耗尽注入 ===
int inject_memory_stress(int size_mb)
{
    char cmd[256];
    if (size_mb <= 0)
    {
        snprintf(cmd, sizeof(cmd), "rm -f /tmp/hadoop_mem_stress 2>/dev/null");
        system(cmd);
        return 0;
    }

    snprintf(cmd, sizeof(cmd),
             "dd if=/dev/zero of=/tmp/hadoop_mem_stress bs=1M count=%d 2>/dev/null",
             size_mb);
    int ret = system(cmd);
    if (ret == 0)
    {
        snprintf(cmd, sizeof(cmd), "cat /tmp/hadoop_mem_stress > /dev/null &");
        system(cmd);
    }
    return ret;
}

// === 模块8：心跳超时模拟 ===
int inject_heartbeat_timeout(const char *node_ip, int timeout_ms)
{
    char cmd[512];
    char nic[32];
    get_default_nic(nic, sizeof(nic));

    if (timeout_ms <= 0)
    {
        snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
        system(cmd);
        return 0;
    }

    if (node_ip && strlen(node_ip) > 0)
    {
        inject_network_delay(node_ip, timeout_ms, 0);
    }
    else
    {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem delay %dms", nic, timeout_ms);
        system(cmd);
    }
    return 0;
}

void print_usage(const char *prog)
{
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

HadoopComponent parse_component(const char *arg)
{
    if (strcmp(arg, "nn") == 0)
        return COMPONENT_NAMENODE;
    if (strcmp(arg, "dn") == 0)
        return COMPONENT_DATANODE;
    if (strcmp(arg, "rm") == 0)
        return COMPONENT_RESOURCE_MGR;
    if (strcmp(arg, "nm") == 0)
        return COMPONENT_NODE_MGR;
    if (strcmp(arg, "snn") == 0)
        return COMPONENT_SECONDARY_NN;
    if (strcmp(arg, "jhs") == 0)
        return COMPONENT_HISTORY_SERVER;
    if (strcmp(arg, "map") == 0)
        return COMPONENT_MAP;
    if (strcmp(arg, "reduce") == 0)
        return COMPONENT_REDUCE;
    if (strcmp(arg, "am") == 0)
        return COMPONENT_APP_MASTER;
    return COMPONENT_ALL;
}
// === Map/Reduce任务进程故障注入 ===
int inject_mapreduce_fault(const char *task_type, HadoopFaultType fault_type)
{
    int count = 0;
    int *pids = find_mapreduce_pids(task_type, &count);

    if (count == 0)
    {
        printf(" 未找到运行中的 %s 任务进程\n", task_type);
        return -1;
    }

    srand(time(NULL));
    int target_idx = rand() % count;
    int target_pid = pids[target_idx];

    switch (fault_type)
    {
    case HADOOP_FAULT_CRASH:
        kill(target_pid, SIGKILL);
        break;
    case HADOOP_FAULT_HANG:
        kill(target_pid, SIGSTOP);
        break;
    default:
        break;
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
        if (argc < 3)
        {
            print_usage(argv[0]);
            return 1;
        }

        HadoopComponent comp = parse_component(argv[2]);
        HadoopFaultType type = 0;
        int local_mode = 0;

        if (strncmp(action, "crash", 5) == 0)
            type = HADOOP_FAULT_CRASH;
        else if (strncmp(action, "hang", 4) == 0)
            type = HADOOP_FAULT_HANG;
        else if (strncmp(action, "resume", 6) == 0)
            type = HADOOP_FAULT_RESUME;

        // 检查是否为内部 local 指令
        if (strstr(action, "-local"))
            local_mode = 1;

        inject_process_fault_distributed(argv[2], comp, type, local_mode);
        return 0;
    }

    // === 其他资源故障 (保持原有单机/主控执行方式) ===
    // 对于资源类故障，简单起见目前暂不分发，如果需要分发可以参考 inject_process_fault_distributed 改造

    else if (strcmp(action, "delay") == 0)
    {
        if (argc < 4)
        {
            printf("Usage: %s delay <target_ip_or_name> <ms> [jitter]\n", argv[0]);
            printf("Example: %s delay slave1 200\n", argv[0]);
            return 1;
        }

        const char *input_target = argv[2];
        int ms = atoi(argv[3]);
        int jitter = (argc >= 5) ? atoi(argv[4]) : 0;

        // --- 1. 定义主机名映射 (请确保顺序与 SLAVE_HOSTS 一致) ---
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        char target_ip[64];
        strcpy(target_ip, input_target); // 默认假设输入的是 IP

        // --- 2. 尝试解析主机名 (slave1 -> 192.168.1.11) ---
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
            {
                strcpy(target_ip, SLAVE_HOSTS[i]);
                printf("[解析] 将主机名 %s 解析为 IP: %s\n", input_target, target_ip);
                break;
            }
        }

        // --- 3. 检查目标 IP 是否属于 Slave (需要远程执行) ---
        int is_remote_slave = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote_slave = 1;

                // 构造远程命令: ssh root@192.168.1.11 '... delay-local global 200 ...'
                char remote_cmd[512];
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s delay-local global %d %d'",
                         SLAVE_HOSTS[i], REMOTE_TOOL_PATH, ms, jitter);

                printf("[Master] 正在向 %s (%s) 分发延迟指令...\n", input_target, target_ip);
                int ret = system(remote_cmd);
                if (ret == 0)
                    printf("[Success] 远程注入命令已发送。\n");
                else
                    printf("[Error] 远程注入失败，返回值: %d\n", ret);

                break;
            }
        }
        // --- 4. 如果不是 Slave IP，则在本地执行 ---
        if (!is_remote_slave)
        {
            printf("[Master] 目标 %s 不是集群 Slave，将在本机执行定向延迟...\n", target_ip);
            inject_network_delay(target_ip, ms, jitter);
        }
    }
    else if (strcmp(action, "delay-local") == 0)
    {
        if (argc < 4)
            return 1;
        const char *target_str = argv[2];
        int ms = atoi(argv[3]); // <--- 修正：从 argv[3] 读取时间
        int jitter = (argc >= 5) ? atoi(argv[4]) : 0;

        printf("[Slave] 收到指令: 目标=%s, 延迟=%dms\n", target_str, ms);

        // 如果是 "global"，传 NULL 给底层函数
        if (strcmp(target_str, "global") == 0)
        {
            inject_network_delay(NULL, ms, jitter);
        }
        else
        {
            // 支持定向注入 (为未来扩展保留)
            inject_network_delay(target_str, ms, jitter);
        }
    }
    else if (strcmp(action, "delay-clear") == 0)
    {
        printf("[Master] 正在清理全集群网络故障...\n");

        // 1. 先清理 Master 自己 (本地)
        inject_network_delay(NULL, 0, 0);

        // 2. 遍历所有 Slave，远程发送清理命令
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            char remote_cmd[512];

            // 构造远程命令: ssh root@slaveX '... delay-local global 0 0'
            // "global 0 0" 意味着延迟为 0，底层会自动执行删除规则操作
            snprintf(remote_cmd, sizeof(remote_cmd),
                     "ssh root@%s '%s delay-local global 0 0'",
                     SLAVE_HOSTS[i], REMOTE_TOOL_PATH);

            printf("  -> 正在清理节点 %s ...\n", SLAVE_HOSTS[i]);
            system(remote_cmd);
        }

        printf("[Success] 全集群网络规则已清除。\n");
    }
    // === 新增：查看当前规则 ===
    else if (strcmp(action, "delay-show") == 0)
    {
        printf("--- Current Network Rules (eth1) ---\n");
        system("tc qdisc show dev eth1");
    }
    // ============================================================
    // [修改] cpu-stress 命令：支持远程分发
    // 用法: ./hadoop_injector cpu-stress <target> <duration> [threads]
    // ============================================================
    else if (strcmp(action, "cpu-stress") == 0)
    {
        if (argc < 4)
        {
            printf("Usage: %s cpu-stress <target_ip_or_name> <duration_sec> [threads]\n", argv[0]);
            printf("Example: %s cpu-stress slave1 10 2\n", argv[0]);
            return 1;
        }

        const char *input_target = argv[2];
        int duration = atoi(argv[3]);
        int threads = (argc >= 5) ? atoi(argv[4]) : 0;

        // --- 1. 定义主机名映射 (确保与 SLAVE_HOSTS 一致) ---
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        char target_ip[64];
        strcpy(target_ip, input_target); // 默认假设是IP

        // --- 2. 解析主机名 ---
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
            {
                strcpy(target_ip, SLAVE_HOSTS[i]);
                printf("[解析] 将主机名 %s 解析为 IP: %s\n", input_target, target_ip);
                break;
            }
        }

        // --- 3. 判断是否为远程 Slave ---
        int is_remote = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote = 1;
                char remote_cmd[512];
                // 构造远程命令: ssh ... cpu-stress-local <duration> <threads>
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s cpu-stress-local %d %d'",
                         SLAVE_HOSTS[i], REMOTE_TOOL_PATH, duration, threads);

                printf("[Master] 正在向 %s 发送 CPU 压力指令 (持续%ds)...\n", input_target, duration);
                // 异步执行 (加上 &)，防止Master一直卡着等Slave跑完
                // 如果你想等Slave跑完再返回，就去掉后面的 " &"
                // 这里我们选择同步等待（去掉&），以便看到完成提示
                system(remote_cmd);
                printf("[Success] 远程 CPU 压力测试完成。\n");
                break;
            }
        }

        // --- 4. 本地执行 ---
        if (!is_remote)
        {
            printf("[Local] 在本机执行 CPU 压力测试...\n");
            inject_cpu_stress(duration, threads);
        }
    }

    // ============================================================
    // [新增] cpu-stress-local：Slave 真正执行的内部命令
    // ============================================================
    else if (strcmp(action, "cpu-stress-local") == 0)
    {
        // 参数: cpu-stress-local <duration> <threads>
        if (argc < 3)
            return 1;
        int duration = atoi(argv[2]);
        int threads = (argc >= 4) ? atoi(argv[3]) : 0;

        printf("[Slave] 收到 CPU 压力指令: %d秒, %d线程\n", duration, threads);
        inject_cpu_stress(duration, threads);
    }
    // ============================================================
    // [修改] mem-stress 命令：支持远程分发
    // 用法: ./hadoop_injector mem-stress <target> <size_mb>
    // ============================================================
    else if (strcmp(action, "mem-stress") == 0)
    {
        if (argc < 4)
        {
            printf("Usage: %s mem-stress <target_ip_or_name> <size_mb>\n", argv[0]);
            printf("Example: %s mem-stress slave1 512  (Consume 512MB)\n", argv[0]);
            return 1;
        }

        const char *input_target = argv[2];
        int size_mb = atoi(argv[3]);

        // --- 1. 主机名解析 ---
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        char target_ip[64];
        strcpy(target_ip, input_target);

        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
            {
                strcpy(target_ip, SLAVE_HOSTS[i]);
                break;
            }
        }

        // --- 2. 远程分发 ---
        int is_remote = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote = 1;
                char remote_cmd[512];
                // 发送 mem-stress-local 指令
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s mem-stress-local %d'",
                         SLAVE_HOSTS[i], REMOTE_TOOL_PATH, size_mb);

                printf("[Master] 正在向 %s 注入内存压力 (%d MB)...\n", input_target, size_mb);
                system(remote_cmd);
                break;
            }
        }

        // --- 3. 本地执行 ---
        if (!is_remote)
        {
            printf("[Local] 在本机执行内存压力 (%d MB)...\n", size_mb);
            inject_memory_stress(size_mb);
        }
    }

    // ============================================================
    // [新增] mem-stress-local：Slave 执行端
    // ============================================================
    else if (strcmp(action, "mem-stress-local") == 0)
    {
        if (argc < 3)
            return 1;
        int size_mb = atoi(argv[2]);
        printf("[Slave] 执行内存占用: %d MB\n", size_mb);
        inject_memory_stress(size_mb);
    }

    // ============================================================
    // [修改] mem-stress-clear：分布式清理
    // ============================================================
    else if (strcmp(action, "mem-stress-clear") == 0)
    {
        printf("[Master] 正在清理全集群内存压力...\n");

        // 1. 清理本机
        inject_memory_stress(0);

        // 2. 清理所有 Slave
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            char remote_cmd[512];
            snprintf(remote_cmd, sizeof(remote_cmd),
                     "ssh root@%s '%s mem-stress-local 0'",
                     SLAVE_HOSTS[i], REMOTE_TOOL_PATH);
            system(remote_cmd);
        }
        printf("[Success] 内存压力已释放。\n");
    }
    // ============================================================
    // [新增] loss 命令：网络丢包 (分布式)
    // 用法: ./hadoop_injector loss <target> <percent>
    // ============================================================
    else if (strcmp(action, "loss") == 0)
    {
        if (argc < 4)
        {
            printf("Usage: %s loss <target_ip_or_name> <percent>\n", argv[0]);
            printf("Example: %s loss slave1 10  (10%% packet loss)\n", argv[0]);
            return 1;
        }

        const char *input_target = argv[2];
        int percent = atoi(argv[3]);

        // --- 1. 主机名解析 ---
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        char target_ip[64];
        strcpy(target_ip, input_target);

        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
            {
                strcpy(target_ip, SLAVE_HOSTS[i]);
                break;
            }
        }

        // --- 2. 远程分发 ---
        int is_remote = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote = 1;
                char remote_cmd[512];
                // 发送 loss-local 指令
                // 格式: loss-local global <percent>
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s loss-local global %d'",
                         SLAVE_HOSTS[i], REMOTE_TOOL_PATH, percent);

                printf("[Master] 正在向 %s 注入 %d%% 丢包率...\n", input_target, percent);
                system(remote_cmd);
                break;
            }
        }

        // --- 3. 本地执行 ---
        if (!is_remote)
        {
            printf("[Local] 在本机注入定向丢包 (目标: %s, 丢包: %d%%)...\n", target_ip, percent);
            inject_network_loss(target_ip, percent);
        }
    }

    // ============================================================
    // [新增] loss-local：Slave 执行端
    // ============================================================
    else if (strcmp(action, "loss-local") == 0)
    {
        // 格式: loss-local <target|global> <percent>
        if (argc < 4)
            return 1;
        const char *target = argv[2];
        int percent = atoi(argv[3]);

        printf("[Slave] 执行丢包注入: %d%%\n", percent);

        if (strcmp(target, "global") == 0)
        {
            inject_network_loss(NULL, percent);
        }
        else
        {
            inject_network_loss(target, percent);
        }
    }

    // ============================================================
    // [新增] loss-clear：分布式清理
    // ============================================================
    else if (strcmp(action, "loss-clear") == 0)
    {
        printf("[Master] 正在清理全集群网络丢包...\n");
        // 1. 本地
        inject_network_loss(NULL, 0);
        // 2. 远程
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            char remote_cmd[512];
            snprintf(remote_cmd, sizeof(remote_cmd),
                     "ssh root@%s '%s loss-local global 0'",
                     SLAVE_HOSTS[i], REMOTE_TOOL_PATH);
            system(remote_cmd);
        }
        printf("[Success] 丢包规则已清除。\n");
    }
    else if (strcmp(action, "reorder") == 0)
    {
        if (argc < 4)
        {
            printf("Usage: %s reorder <target> <percent> [correlation]\n", argv[0]);
            return 1;
        }
        const char *input_target = argv[2];
        int percent = atoi(argv[3]);
        int correlation = (argc >= 5) ? atoi(argv[4]) : 25; // 默认相关性 25%

        // 解析主机名
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        char target_ip[64];
        strcpy(target_ip, input_target);
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
            {
                strcpy(target_ip, SLAVE_HOSTS[i]);
                break;
            }
        }

        // 远程分发
        int is_remote = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote = 1;
                char remote_cmd[512];
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s reorder-local global %d %d'",
                         SLAVE_HOSTS[i], REMOTE_TOOL_PATH, percent, correlation);
                printf("[Master] 向 %s 注入 %d%% 乱序 (相关性%d%%)...\n", input_target, percent, correlation);
                system(remote_cmd);
                break;
            }
        }
        // 本地执行
        if (!is_remote)
            inject_network_reorder(target_ip, percent, correlation);
    }
    // [内部] reorder-local
    else if (strcmp(action, "reorder-local") == 0)
    {
        if (argc < 4)
            return 1;
        int percent = atoi(argv[3]);
        int correlation = atoi(argv[4]);
        printf("[Slave] 执行乱序注入: %d%%\n", percent);
        inject_network_reorder(NULL, percent, correlation);
    }
    // [清理] reorder-clear
    else if (strcmp(action, "reorder-clear") == 0)
    {
        printf("[Master] 清理全集群网络乱序...\n");
        inject_network_reorder(NULL, 0, 0); // 清理本地
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "ssh root@%s '%s reorder-local global 0 0'", SLAVE_HOSTS[i], REMOTE_TOOL_PATH);
            system(cmd);
        }
        printf("[Success] 乱序规则已清除。\n");
    }

    // ============================================================
    // [新增] 2. isolate 命令：网络隔离/分区 (分布式)
    // 用法: isolate <target_ip> [port] (如果不指定端口，则完全断网)
    // 底层依赖: iptables (你的代码中已有 inject_network_fault)
    // ============================================================
    else if (strcmp(action, "isolate") == 0)
    {
        if (argc < 3)
        {
            printf("Usage: %s isolate <target_node> [port]\n", argv[0]);
            return 1;
        }
        const char *input_target = argv[2];
        int port = (argc >= 4) ? atoi(argv[3]) : 0;

        // 解析主机名
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        char target_ip[64];
        strcpy(target_ip, input_target);
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
            {
                strcpy(target_ip, SLAVE_HOSTS[i]);
                break;
            }
        }

        // 远程分发
        int is_remote = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote = 1;
                char remote_cmd[512];
                // 发送 isolate-local 指令
                // 注意：这里我们让 Slave 隔离掉 Master 或者其他 Slave 的 IP
                // 简单起见，这里实现的是“隔离该节点的所有入站/出站流量”
                // 传 "all" 给 local 命令表示针对所有 IP 隔离，或者指定 IP
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s isolate-local all %d'",
                         SLAVE_HOSTS[i], REMOTE_TOOL_PATH, port);
                printf("[Master] 正在隔离节点 %s (端口: %d)...\n", input_target, port);
                system(remote_cmd);
                break;
            }
        }
        if (!is_remote)
        {
            // 本地隔离通常指：本机禁止访问目标 IP
            inject_network_fault(target_ip, port, 1);
        }
    }
    // [内部] isolate-local
    else if (strcmp(action, "isolate-local") == 0)
    {
        if (argc < 3)
            return 1;
        int port = (argc >= 4) ? atoi(argv[3]) : 0;

        if (port > 0)
        {
            // 隔离指定端口
            printf("[Slave] 隔离端口 TCP %d...\n", port);
            char cmd[256];
            snprintf(cmd, sizeof(cmd),
                     "iptables -A INPUT -p tcp --dport %d -j DROP; "
                     "iptables -A OUTPUT -p tcp --sport %d -j DROP",
                     port, port);
            system(cmd);
        }
        else
        {
            // 隔离 Hadoop 相关端口，但保留 SSH (22)
            printf("[Slave] 执行 Hadoop 端口隔离 (保留SSH)...\n");

            // 分开执行，避免命令过长被截断
            // HDFS 入站
            system("iptables -A INPUT -s 192.168.1.0/24 -p tcp --dport 8020 -j DROP"); // NameNode RPC
            system("iptables -A INPUT -s 192.168.1.0/24 -p tcp --dport 9870 -j DROP"); // NameNode HTTP
            system("iptables -A INPUT -s 192.168.1.0/24 -p tcp --dport 9866 -j DROP"); // DataNode
            system("iptables -A INPUT -s 192.168.1.0/24 -p tcp --dport 9867 -j DROP"); // DataNode IPC
            system("iptables -A INPUT -s 192.168.1.0/24 -p tcp --dport 9864 -j DROP"); // DataNode HTTP
            // YARN 入站
            system("iptables -A INPUT -s 192.168.1.0/24 -p tcp --dport 8088 -j DROP");      // ResourceManager
            system("iptables -A INPUT -s 192.168.1.0/24 -p tcp --dport 8042 -j DROP");      // NodeManager
            system("iptables -A INPUT -s 192.168.1.0/24 -p tcp --dport 8030:8033 -j DROP"); // RM 内部端口
            // HDFS/YARN 出站
            system("iptables -A OUTPUT -d 192.168.1.0/24 -p tcp --dport 8020 -j DROP");
            system("iptables -A OUTPUT -d 192.168.1.0/24 -p tcp --dport 9870 -j DROP");
            system("iptables -A OUTPUT -d 192.168.1.0/24 -p tcp --dport 9866 -j DROP");
            system("iptables -A OUTPUT -d 192.168.1.0/24 -p tcp --dport 8088 -j DROP");
            system("iptables -A OUTPUT -d 192.168.1.0/24 -p tcp --dport 8042 -j DROP");

            printf("[Slave] Hadoop 端口隔离完成\n");
        }
    }

    // [清理] isolate-clear
    else if (strcmp(action, "isolate-clear") == 0)
    {
        printf("[Master] 清理网络隔离规则...\n");
        // 清理本地
        system("iptables -F"); // 简单粗暴清空
        // 清理远程
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "ssh root@%s 'iptables -F'", SLAVE_HOSTS[i]);
            system(cmd);
        }
        printf("[Success] 防火墙规则已重置。\n");
    }
    // ============================================================
    // [新增] 3. disk-fill 命令：磁盘空间填满 (分布式)
    // 用法: disk-fill <target> <size_mb>
    // ============================================================
    else if (strcmp(action, "disk-fill") == 0)
    {
        if (argc < 4)
            return 1;
        const char *input_target = argv[2];
        int size_mb = atoi(argv[3]);

        // 解析主机名与远程分发逻辑 (简化写法，复用上面的模式)
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        char target_ip[64];
        strcpy(target_ip, input_target);
        for (int i = 0; i < SLAVE_COUNT; i++)
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
                strcpy(target_ip, SLAVE_HOSTS[i]);

        int is_remote = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote = 1;
                char remote_cmd[512];
                snprintf(remote_cmd, sizeof(remote_cmd), "ssh root@%s '%s disk-fill-local %d'", SLAVE_HOSTS[i], REMOTE_TOOL_PATH, size_mb);
                printf("[Master] 令 %s 填充磁盘 %dMB...\n", input_target, size_mb);
                system(remote_cmd);
                break;
            }
        }
        if (!is_remote)
        {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "dd if=/dev/zero of=/tmp/disk_hog bs=1M count=%d", size_mb);
            system(cmd);
        }
    }
    // [内部] disk-fill-local
    else if (strcmp(action, "disk-fill-local") == 0)
    {
        int size_mb = atoi(argv[2]);
        char cmd[256];
        printf("[Slave] 填充垃圾文件 /tmp/disk_hog (%d MB)...\n", size_mb);
        snprintf(cmd, sizeof(cmd), "dd if=/dev/zero of=/tmp/disk_hog bs=1M count=%d", size_mb);
        system(cmd);
    }
    // [清理] disk-fill-clear
    else if (strcmp(action, "disk-fill-clear") == 0)
    {
        printf("[Master] 清理磁盘垃圾文件...\n");
        system("rm -f /tmp/disk_hog"); // 本地
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), "ssh root@%s 'rm -f /tmp/disk_hog'", SLAVE_HOSTS[i]);
            system(cmd);
        }
        printf("[Success] 磁盘空间已释放。\n");
    }
    else if (strcmp(action, "hdfs-safe") == 0)
    {
        if (argc < 3)
        {
            printf("Usage: %s hdfs-safe <enter|leave>\n", argv[0]);
            return 1;
        }
        const char *op = argv[2];
        int type = 0;
        if (strcmp(op, "enter") == 0)
            type = 1;
        else if (strcmp(op, "leave") == 0)
            type = 2;
        else
        {
            printf("Unknown operation: %s\n", op);
            return 1;
        }

        printf("[Master] 执行 HDFS 安全模式操作: %s\n", op);
        // 安全模式通常只需要在安装了 Hadoop Client 的机器(Master)上执行一次即可生效
        inject_hdfs_fault(type, NULL);
    }
    // ============================================================
    // [新增] 5. hdfs-disk 命令：模拟 HDFS 磁盘空间不足
    // 用法: hdfs-disk <target> <MB>
    // 说明: 其实就是生成大文件占满磁盘，测试 HDFS 写失败
    // ============================================================
    else if (strcmp(action, "hdfs-disk") == 0)
    {
        // 这其实是 disk-fill 的别名，为了语义清晰单独列出
        if (argc < 4)
        {
            printf("Usage: %s hdfs-disk <target> <MB>\n", argv[0]);
            return 1;
        }
        // 直接复用 disk-fill 的逻辑，重新构造参数调用自身
        // 或者直接复制 disk-fill 的代码。为了简单，这里直接复用 disk-fill-local 的分发逻辑
        char new_cmd[512];
        snprintf(new_cmd, sizeof(new_cmd), "%s disk-fill %s %s", argv[0], argv[2], argv[3]);
        system(new_cmd);
    }

    // ============================================================
    // [新增] 6. MapReduce 任务故障 (需要集群正在运行作业)
    // 用法: crash-map <target_slave>  或  crash-reduce <target_slave>
    // ============================================================
    else if (strcmp(action, "crash-map") == 0 || strcmp(action, "crash-reduce") == 0)
    {
        if (argc < 3)
        {
            printf("Usage: %s %s <target_slave>\n", argv[0], action);
            return 1;
        }
        const char *target_input = argv[2];
        const char *task_type = (strcmp(action, "crash-map") == 0) ? "map" : "reduce";

        // 解析主机名
        char target_ip[64];
        strcpy(target_ip, target_input);
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        for (int i = 0; i < SLAVE_COUNT; i++)
            if (strcmp(target_input, NODE_NAMES[i]) == 0)
                strcpy(target_ip, SLAVE_HOSTS[i]);

        // 远程分发
        int is_remote = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote = 1;
                char remote_cmd[512];
                // 发送 mr-fault-local 指令
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s mr-fault-local %s'",
                         SLAVE_HOSTS[i], REMOTE_TOOL_PATH, task_type);

                printf("[Master] 正在 %s 上寻找并杀死 %s 任务...\n", target_input, task_type);
                system(remote_cmd);
                break;
            }
        }
        if (!is_remote)
            printf("[Error] MapReduce 任务通常运行在 Slave 节点，请指定 slave1 或 slave2\n");
    }

    // [内部] mr-fault-local: Slave 执行端
    else if (strcmp(action, "mr-fault-local") == 0)
    {
        if (argc < 3)
            return 1;
        const char *task_type = argv[2]; // "map" or "reduce"

        printf("[Slave] 尝试注入 %s 任务故障...\n", task_type);
        // 调用底层 inject_mapreduce_fault (HADOOP_FAULT_CRASH = 1)
        inject_mapreduce_fault(task_type, HADOOP_FAULT_CRASH);
    }
    // ============================================================
    // [补全] 7. io-slow 命令：模拟磁盘 I/O 缓慢 (分布式)
    // 用法: io-slow <target> <on|off>
    // ============================================================
    else if (strcmp(action, "io-slow") == 0)
    {
        if (argc < 4)
        {
            printf("Usage: %s io-slow <target> <on|off>\n", argv[0]);
            return 1;
        }
        const char *input_target = argv[2];
        int is_on = (strcmp(argv[3], "on") == 0) ? 1 : 0;

        // 解析主机名
        char target_ip[64];
        strcpy(target_ip, input_target);
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        for (int i = 0; i < SLAVE_COUNT; i++)
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
                strcpy(target_ip, SLAVE_HOSTS[i]);

        // 分发
        int is_remote = 0;
        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                is_remote = 1;
                char remote_cmd[512];
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s io-slow-local %d'", SLAVE_HOSTS[i], REMOTE_TOOL_PATH, is_on);
                printf("[Master] %s 磁盘 I/O 限速...\n", is_on ? "开启" : "关闭");
                system(remote_cmd);
                break;
            }
        }
        if (!is_remote)
            inject_io_delay(is_on); // 本地
    }
    // [内部] io-slow-local
    else if (strcmp(action, "io-slow-local") == 0)
    {
        int is_on = atoi(argv[2]);
        printf("[Slave] 执行 IO 限速: %s\n", is_on ? "ON" : "OFF");
        inject_io_delay(is_on);
    }

    // ============================================================
    // [补全] 8. yarn-unhealthy 命令：模拟 NodeManager 不健康
    // 用法: yarn-unhealthy <target> <on|off>
    // ============================================================
    else if (strcmp(action, "yarn-unhealthy") == 0)
    {
        if (argc < 4)
        {
            printf("Usage: %s yarn-unhealthy <target> <on|off>\n", argv[0]);
            return 1;
        }
        const char *input_target = argv[2];
        int type = (strcmp(argv[3], "on") == 0) ? 1 : 2; // 1=Error, 2=Clear

        char target_ip[64];
        strcpy(target_ip, input_target);
        const char *NODE_NAMES[] = {"slave1", "slave2"};
        for (int i = 0; i < SLAVE_COUNT; i++)
            if (strcmp(input_target, NODE_NAMES[i]) == 0)
                strcpy(target_ip, SLAVE_HOSTS[i]);

        for (int i = 0; i < SLAVE_COUNT; i++)
        {
            if (strcmp(target_ip, SLAVE_HOSTS[i]) == 0)
            {
                char remote_cmd[512];
                snprintf(remote_cmd, sizeof(remote_cmd),
                         "ssh root@%s '%s yarn-unhealthy-local %d'", SLAVE_HOSTS[i], REMOTE_TOOL_PATH, type);
                printf("[Master] 设置 %s YARN 节点状态...\n", input_target);
                system(remote_cmd);
                break;
            }
        }
    }
    // [内部] yarn-unhealthy-local
    else if (strcmp(action, "yarn-unhealthy-local") == 0)
    {
        int type = atoi(argv[2]);
        printf("[Slave] 修改 YARN 健康检查文件...\n");
        inject_yarn_fault(type, NULL);
    }
    // ============================================================
    // [补全] 9. heartbeat 命令：心跳超时模拟 (语义化封装)
    // 用法: heartbeat <target> <ms>
    // ============================================================
    else if (strcmp(action, "heartbeat") == 0)
    {
        if (argc < 4)
        {
            printf("Usage: %s heartbeat <target> <ms>\n", argv[0]);
            return 1;
        }
        // 直接复用 delay 的逻辑，但语义上是“心跳超时”
        char new_cmd[512];
        snprintf(new_cmd, sizeof(new_cmd), "%s delay %s %s", argv[0], argv[2], argv[3]);
        system(new_cmd);
    }
    else
    {
        printf("Unknown command: %s\n", action);
        print_usage(argv[0]);
    }

    return 0;
}
