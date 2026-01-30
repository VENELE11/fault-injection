/*
 * hadoop_injector.c - Hadoop集群故障注入工具
 * 
 * 功能：针对Hadoop生态系统（HDFS/YARN/MapReduce）进行多层次故障注入
 * 支持：
 *   - 核心进程故障：NameNode, DataNode, ResourceManager, NodeManager
 *   - 任务进程故障：Map进程, Reduce进程
 *   - 网络通信故障：延迟、丢包、乱序、分区
 *   - 资源占用故障：CPU、内存耗尽
 *   - 心跳超时故障：模拟心跳检测失败
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

// === 故障模型5元组 ===
/*typedef struct {
    char layer[32];          // 故障层次 (Hadoop/Spark)
    char tool[32];           // 故障工具名
    char ip[32];             // 故障位置 (IP地址)
    char timestamp[32];      // 故障发生时间
    char params[128];        // 故障参数
} FaultModel;
*/
// === 全局变量：资源压力控制 ===
static volatile int g_stress_running = 0;
static pthread_t *g_stress_threads = NULL;
static int g_stress_thread_count = 0;

// === 辅助函数：获取进程名 ===
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

// === 辅助函数：获取组件中文名 ===
const char* get_component_display_name(HadoopComponent component) {
    switch (component) {
        case COMPONENT_NAMENODE: return "NameNode (HDFS主节点)";
        case COMPONENT_DATANODE: return "DataNode (HDFS数据节点)";
        case COMPONENT_RESOURCE_MGR: return "ResourceManager (YARN主节点)";
        case COMPONENT_NODE_MGR: return "NodeManager (YARN计算节点)";
        case COMPONENT_SECONDARY_NN: return "SecondaryNameNode";
        case COMPONENT_HISTORY_SERVER: return "JobHistoryServer";
        case COMPONENT_MAP: return "Map任务进程";
        case COMPONENT_REDUCE: return "Reduce任务进程";
        case COMPONENT_APP_MASTER: return "ApplicationMaster";
        case COMPONENT_JOBTRACKER: return "JobTracker (Hadoop 1.x)";
        case COMPONENT_TASKTRACKER: return "TaskTracker (Hadoop 1.x)";
        default: return "未知组件";
    }
}

// === 辅助函数：查找Hadoop进程PID ===
int find_hadoop_pid(const char *proc_name) {
    char cmd[256];
    char output[32];
    
    // 使用jps命令查找Java进程（Hadoop组件都是Java进程）
    snprintf(cmd, sizeof(cmd), 
             "jps -l 2>/dev/null | grep %s | awk '{print $1}' | head -n 1", 
             proc_name);
    
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
    
    // 根据任务类型查找：map或reduce
    // YarnChild进程参数中包含task类型信息
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

// === 辅助函数：列出所有Hadoop进程 ===
void list_hadoop_processes() {
    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║              当前Hadoop进程状态                              ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    // HDFS组件
    printf("║ [HDFS 分布式文件系统]                                        ║\n");
    const char* hdfs_components[] = {NAMENODE_PROC, SECONDARY_NN_PROC, DATANODE_PROC};
    const char* hdfs_names[] = {"NameNode (主节点)", "SecondaryNameNode", "DataNode (数据节点)"};
    
    for (int i = 0; i < 3; i++) {
        int pid = find_hadoop_pid(hdfs_components[i]);
        if (pid > 0) {
            printf("║    %-25s PID: %-6d 运行中         ║\n", hdfs_names[i], pid);
        } else {
            printf("║    %-25s 未运行                       ║\n", hdfs_names[i]);
        }
    }
    
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║ [YARN 资源管理]                                              ║\n");
    
    const char* yarn_components[] = {RESOURCE_MGR_PROC, NODE_MGR_PROC, HISTORY_SERVER_PROC};
    const char* yarn_names[] = {"ResourceManager", "NodeManager", "JobHistoryServer"};
    
    for (int i = 0; i < 3; i++) {
        int pid = find_hadoop_pid(yarn_components[i]);
        if (pid > 0) {
            printf("║    %-25s PID: %-6d 运行中         ║\n", yarn_names[i], pid);
        } else {
            printf("║    %-25s 未运行                       ║\n", yarn_names[i]);
        }
    }
    
    // 检查运行中的MapReduce任务
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    printf("║ [MapReduce 任务进程]                                         ║\n");
    
    int count = 0;
    find_mapreduce_pids("map", &count);
    if (count > 0) {
        printf("║    YarnChild任务进程数量: %-3d                            ║\n", count);
    } else {
        printf("║     当前无运行中的MapReduce任务                            ║\n");
    }
    
    int am_pid = find_hadoop_pid(MR_APP_MASTER);
    if (am_pid > 0) {
        printf("║    MRAppMaster              PID: %-6d 运行中         ║\n", am_pid);
    }
    
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");
}

// === 模块1：进程故障注入 ===
int inject_process_fault(HadoopComponent component, HadoopFaultType fault_type) {
    const char *proc_name = get_component_name(component);
    if (!proc_name) {
        printf(" 无效的组件类型\n");
        return -1;
    }
    
    int pid = find_hadoop_pid(proc_name);
    if (pid == -1) {
        printf(" 未找到进程: %s\n", proc_name);
        return -1;
    }
    
    printf("[Hadoop注入] 目标: %s (PID: %d)\n", proc_name, pid);
    
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

// === 模块2：网络故障注入（节点间通信） ===
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
    
    // 使用tc netem注入延迟
    if (target_ip && strlen(target_ip) > 0) {
        // 针对特定IP的延迟
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root handle 1: prio; "
                 "tc qdisc add dev %s parent 1:3 handle 30: netem delay %dms %dms; "
                 "tc filter add dev %s parent 1:0 protocol ip prio 3 u32 "
                 "match ip dst %s flowid 1:3",
                 nic, nic, delay_ms, jitter_ms, nic, target_ip);
        printf(" [Network Delay] 对 %s 注入 %dms%dms 延迟\n", target_ip, delay_ms, jitter_ms);
    } else {
        // 全局延迟
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
        case 1: // 强制进入安全模式
            snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -safemode enter");
            printf(" [HDFS] 强制进入安全模式\n");
            break;
            
        case 2: // 退出安全模式
            snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -safemode leave");
            printf(" [HDFS] 退出安全模式\n");
            break;
            
        case 3: // 模拟磁盘满（创建大文件占用空间）
            if (param) {
                snprintf(cmd, sizeof(cmd), 
                         "dd if=/dev/zero of=/tmp/hdfs_disk_fill bs=1M count=%s",
                         param);
                printf(" [HDFS] 模拟磁盘空间占用 %sMB\n", param);
            } else {
                printf(" 需要指定大小参数\n");
                return -1;
            }
            break;
            
        case 4: // 清理磁盘占用文件
            snprintf(cmd, sizeof(cmd), "rm -f /tmp/hdfs_disk_fill");
            printf(" [HDFS] 清理模拟磁盘占用\n");
            break;
            
        case 5: // 强制刷新节点
            snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -refreshNodes");
            printf(" [HDFS] 刷新DataNode列表\n");
            break;
            
        default:
            printf(" 未知的HDFS故障类型\n");
            return -1;
    }
    
    int ret = system(cmd);
    if (ret != 0) {
        printf("  命令执行返回异常 (Code: %d)\n", ret);
    }
    
    return ret;
}

// === 模块4：YARN资源故障 ===
int inject_yarn_fault(int fault_type, const char *node_ip) {
    char cmd[1024];
    
    switch (fault_type) {
        case 1: // 标记节点为不健康
            if (node_ip) {
                // 创建不健康检查脚本
                snprintf(cmd, sizeof(cmd),
                         "echo 'ERROR' > /tmp/yarn_node_health_check");
                printf(" [YARN] 标记节点健康检查失败\n");
            }
            break;
            
        case 2: // 恢复节点健康
            snprintf(cmd, sizeof(cmd), "rm -f /tmp/yarn_node_health_check");
            printf(" [YARN] 恢复节点健康状态\n");
            break;
            
        case 3: // 刷新节点
            snprintf(cmd, sizeof(cmd), "yarn rmadmin -refreshNodes");
            printf(" [YARN] 刷新ResourceManager节点列表\n");
            break;
            
        case 4: // 刷新队列
            snprintf(cmd, sizeof(cmd), "yarn rmadmin -refreshQueues");
            printf(" [YARN] 刷新调度队列配置\n");
            break;
            
        default:
            printf(" 未知的YARN故障类型\n");
            return -1;
    }
    
    int ret = system(cmd);
    if (ret != 0) {
        printf("  命令执行返回异常 (Code: %d)\n", ret);
    }
    
    return ret;
}

// === 模块5：IO延迟注入 ===
int inject_io_delay(const char *mount_point, int delay_ms) {
    char cmd[512];
    
    if (delay_ms > 0) {
        // 使用tc对块设备模拟延迟（简化实现，实际可能需要更复杂的配置）
        printf("  [IO] 在 %s 注入 %dms 延迟\n", mount_point, delay_ms);
        printf("   注: 真实IO延迟注入建议使用dm-delay或fio工具\n");
        
        // 这里提供一个基于cgroups的简化方案
        snprintf(cmd, sizeof(cmd),
                 "echo '8:0 rbps=1048576 wbps=1048576' > "
                 "/sys/fs/cgroup/blkio/blkio.throttle.read_bps_device 2>/dev/null");
    } else {
        // 清理限速
        snprintf(cmd, sizeof(cmd),
                 "echo '' > /sys/fs/cgroup/blkio/blkio.throttle.read_bps_device 2>/dev/null");
        printf(" [IO] 清理IO限速\n");
    }
    
    system(cmd);
    return 0;
}

// === 模块6：CPU资源耗尽注入 ===
void* cpu_stress_worker(void *arg) {
    double x = 0.0;
    while (g_stress_running) {
        // 密集浮点运算消耗CPU
        x = x + 0.1;
        if (x > 1000000) x = 0;
    }
    return NULL;
}

int inject_cpu_stress(int duration_sec, int num_threads) {
    if (num_threads <= 0) {
        // 默认使用所有CPU核心
        num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    }
    
    printf(" [CPU Stress] 启动 %d 个线程进行CPU压力测试, 持续 %d 秒\n", 
           num_threads, duration_sec);
    
    g_stress_running = 1;
    g_stress_thread_count = num_threads;
    g_stress_threads = malloc(num_threads * sizeof(pthread_t));
    
    if (!g_stress_threads) {
        perror("malloc failed");
        return -1;
    }
    
    // 启动压力线程
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&g_stress_threads[i], NULL, cpu_stress_worker, NULL) != 0) {
            perror("创建线程失败");
        }
    }
    
    // 等待指定时间
    printf("    CPU压力持续中...\n");
    sleep(duration_sec);
    
    // 停止压力
    g_stress_running = 0;
    for (int i = 0; i < num_threads; i++) {
        pthread_join(g_stress_threads[i], NULL);
    }
    
    free(g_stress_threads);
    g_stress_threads = NULL;
    
    printf(" [CPU Stress] CPU压力测试完成\n");
    return 0;
}

// === 模块7：内存资源耗尽注入 ===
int inject_memory_stress(int size_mb) {
    char cmd[256];
    
    if (size_mb <= 0) {
        // 清理内存占用
        snprintf(cmd, sizeof(cmd), "rm -f /tmp/hadoop_mem_stress 2>/dev/null");
        system(cmd);
        printf(" [Memory] 清理内存压力\n");
        return 0;
    }
    
    // 使用sysinfo获取当前内存状态
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        unsigned long free_mb = si.freeram / (1024 * 1024);
        printf("  [Memory] 当前可用内存: %lu MB\n", free_mb);
        
        if ((unsigned long)size_mb > free_mb * 0.9) {
            printf("  警告: 请求的内存 %d MB 接近可用内存上限!\n", size_mb);
        }
    }
    
    printf(" [Memory Stress] 占用 %d MB 内存\n", size_mb);
    
    // 使用dd创建大文件占用内存（通过页缓存）
    snprintf(cmd, sizeof(cmd),
             "dd if=/dev/zero of=/tmp/hadoop_mem_stress bs=1M count=%d 2>/dev/null",
             size_mb);
    
    int ret = system(cmd);
    
    if (ret == 0) {
        // 将文件读入内存
        snprintf(cmd, sizeof(cmd), "cat /tmp/hadoop_mem_stress > /dev/null &");
        system(cmd);
        printf(" [Memory Stress] 内存压力已注入\n");
    }
    
    return ret;
}

// === 模块8：心跳超时模拟 ===
int inject_heartbeat_timeout(const char *node_ip, int timeout_ms) {
    char cmd[512];
    char nic[32];
    
    get_default_nic(nic, sizeof(nic));
    
    if (timeout_ms <= 0) {
        // 清理
        snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
        system(cmd);
        printf(" [Heartbeat] 清理心跳超时模拟\n");
        return 0;
    }
    
    printf(" [Heartbeat Timeout] 模拟节点 %s 心跳超时 (%dms延迟)\n", 
           node_ip ? node_ip : "全局", timeout_ms);
    
    // 通过注入极大延迟来模拟心跳超时
    // Hadoop默认心跳间隔为3秒，超时为10分钟
    if (node_ip && strlen(node_ip) > 0) {
        inject_network_delay(node_ip, timeout_ms, 0);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem delay %dms", nic, timeout_ms);
        system(cmd);
    }
    
    printf("   注: Hadoop默认心跳超时为10分钟 (dfs.namenode.heartbeat.recheck-interval)\n");
    return 0;
}

// === 打印使用帮助 ===
void print_usage(const char *prog) {
    printf("\n╔═══════════════════════════════════════════════════════════════════╗\n");
    printf("║        Hadoop集群故障注入工具 v2.0 (增强版)                       ║\n");
    printf("║                     ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════╝\n\n");
    printf("用法: %s <命令> [参数]\n\n", prog);
    
    printf("【进程故障注入】\n");
    printf("  list                       列出所有Hadoop进程状态\n");
    printf("  crash <组件>               终止指定组件进程 (SIGKILL)\n");
    printf("  hang <组件>                暂停指定组件进程 (SIGSTOP)\n");
    printf("  resume <组件>              恢复指定组件进程 (SIGCONT)\n");
    printf("  crash-map                  随机终止一个Map任务\n");
    printf("  crash-reduce               随机终止一个Reduce任务\n\n");
    
    printf("【网络故障注入】\n");
    printf("  network <IP> [端口]        隔离指定IP的网络通信 (iptables)\n");
    printf("  network-clear <IP>         清理指定IP的网络隔离\n");
    printf("  delay <IP> <毫秒> [抖动]   对指定IP注入网络延迟 (tc netem)\n");
    printf("  delay-clear                清理网络延迟\n");
    printf("  loss <IP> <百分比>         对指定IP注入丢包 (tc netem)\n");
    printf("  loss-clear                 清理网络丢包\n");
    printf("  reorder <百分比> [相关性]  注入数据包乱序\n");
    printf("  heartbeat <IP> <超时ms>    模拟心跳超时\n\n");
    
    printf("【资源占用故障】\n");
    printf("  cpu-stress <秒> [线程数]   CPU资源耗尽注入\n");
    printf("  mem-stress <MB>            内存资源耗尽注入\n");
    printf("  mem-stress-clear           清理内存占用\n\n");
    
    printf("【HDFS故障注入】\n");
    printf("  hdfs-safe enter|leave      控制HDFS安全模式\n");
    printf("  hdfs-disk <MB>             模拟磁盘空间占用\n");
    printf("  hdfs-disk-clear            清理磁盘占用模拟\n");
    printf("  hdfs-refresh               刷新DataNode列表\n\n");
    
    printf("【YARN故障注入】\n");
    printf("  yarn-health fail|ok        设置YARN节点健康状态\n");
    printf("  yarn-refresh               刷新YARN节点和队列\n\n");
    
    printf("【组件代号】\n");
    printf("  nn   - NameNode            dn   - DataNode\n");
    printf("  rm   - ResourceManager     nm   - NodeManager\n");
    printf("  snn  - SecondaryNameNode   jhs  - JobHistoryServer\n");
    printf("  map  - Map任务进程         reduce - Reduce任务进程\n");
    printf("  am   - ApplicationMaster\n\n");
    
    printf("【示例】\n");
    printf("  %s list                         # 查看所有Hadoop进程\n", prog);
    printf("  %s crash nn                     # 终止NameNode\n", prog);
    printf("  %s delay 192.168.1.11 100 20    # 对节点注入100ms20ms延迟\n", prog);
    printf("  %s loss 192.168.1.11 10         # 对节点注入10%%丢包率\n", prog);
    printf("  %s cpu-stress 30 4              # 30秒CPU压力测试(4线程)\n", prog);
    printf("  %s mem-stress 512               # 占用512MB内存\n", prog);
    printf("  %s heartbeat 192.168.1.11 60000 # 模拟60秒心跳超时\n", prog);
    printf("\n");
}

// === 解析组件参数 ===
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
    if (strcmp(arg, "jt") == 0) return COMPONENT_JOBTRACKER;
    if (strcmp(arg, "tt") == 0) return COMPONENT_TASKTRACKER;
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
    
    printf("[MapReduce注入] 找到 %d 个 %s 任务进程\n", count, task_type);
    
    // 随机选择一个任务进程
    srand(time(NULL));
    int target_idx = rand() % count;
    int target_pid = pids[target_idx];
    
    printf("[MapReduce注入] 随机选择进程 PID: %d\n", target_pid);
    
    switch (fault_type) {
        case HADOOP_FAULT_CRASH:
            if (kill(target_pid, SIGKILL) == 0) {
                printf(" [Crash] 已终止 %s 任务 (PID: %d)\n", task_type, target_pid);
                printf("   预期: Hadoop会重新调度该任务到其他节点执行\n");
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        case HADOOP_FAULT_HANG:
            if (kill(target_pid, SIGSTOP) == 0) {
                printf("  [Hang] 已暂停 %s 任务 (PID: %d)\n", task_type, target_pid);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        default:
            printf(" 不支持的故障类型\n");
            return -1;
    }
    
    return 0;
}

// === 主函数 ===
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    // 检查root权限
    if (geteuid() != 0) {
        printf("  警告: 部分功能需要root权限运行\n");
    }
    
    const char *command = argv[1];
    
    // === 命令解析 ===
    if (strcmp(command, "list") == 0) {
        list_hadoop_processes();
    }
    // === 进程故障 ===
    else if (strcmp(command, "crash") == 0) {
        if (argc < 3) {
            printf(" 用法: %s crash <组件>\n", argv[0]);
            return 1;
        }
        HadoopComponent comp = parse_component(argv[2]);
        if (comp == COMPONENT_ALL) {
            printf(" 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_process_fault(comp, HADOOP_FAULT_CRASH);
    }
    else if (strcmp(command, "hang") == 0) {
        if (argc < 3) {
            printf(" 用法: %s hang <组件>\n", argv[0]);
            return 1;
        }
        HadoopComponent comp = parse_component(argv[2]);
        if (comp == COMPONENT_ALL) {
            printf(" 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_process_fault(comp, HADOOP_FAULT_HANG);
    }
    else if (strcmp(command, "resume") == 0) {
        if (argc < 3) {
            printf(" 用法: %s resume <组件>\n", argv[0]);
            return 1;
        }
        HadoopComponent comp = parse_component(argv[2]);
        if (comp == COMPONENT_ALL) {
            printf(" 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_process_fault(comp, HADOOP_FAULT_RESUME);
    }
    // === MapReduce任务进程故障 ===
    else if (strcmp(command, "crash-map") == 0) {
        inject_mapreduce_fault("map", HADOOP_FAULT_CRASH);
    }
    else if (strcmp(command, "crash-reduce") == 0) {
        inject_mapreduce_fault("reduce", HADOOP_FAULT_CRASH);
    }
    // === 网络故障 ===
    else if (strcmp(command, "network") == 0) {
        if (argc < 3) {
            printf(" 用法: %s network <IP> [端口]\n", argv[0]);
            return 1;
        }
        int port = (argc >= 4) ? atoi(argv[3]) : 0;
        inject_network_fault(argv[2], port, 1);
    }
    else if (strcmp(command, "network-clear") == 0) {
        if (argc < 3) {
            printf(" 用法: %s network-clear <IP>\n", argv[0]);
            return 1;
        }
        inject_network_fault(argv[2], 0, 0);
    }
    else if (strcmp(command, "delay") == 0) {
        if (argc < 4) {
            printf(" 用法: %s delay <IP> <毫秒> [抖动]\n", argv[0]);
            return 1;
        }
        int delay_ms = atoi(argv[3]);
        int jitter_ms = (argc >= 5) ? atoi(argv[4]) : 0;
        inject_network_delay(argv[2], delay_ms, jitter_ms);
    }
    else if (strcmp(command, "delay-clear") == 0) {
        inject_network_delay(NULL, 0, 0);
    }
    else if (strcmp(command, "loss") == 0) {
        if (argc < 4) {
            printf(" 用法: %s loss <IP> <百分比>\n", argv[0]);
            return 1;
        }
        int loss_percent = atoi(argv[3]);
        inject_network_loss(argv[2], loss_percent);
    }
    else if (strcmp(command, "loss-clear") == 0) {
        inject_network_loss(NULL, 0);
    }
    else if (strcmp(command, "reorder") == 0) {
        if (argc < 3) {
            printf(" 用法: %s reorder <百分比> [相关性]\n", argv[0]);
            return 1;
        }
        int reorder_percent = atoi(argv[2]);
        int correlation = (argc >= 4) ? atoi(argv[3]) : 25;
        inject_network_reorder(NULL, reorder_percent, correlation);
    }
    else if (strcmp(command, "heartbeat") == 0) {
        if (argc < 4) {
            printf(" 用法: %s heartbeat <IP> <超时毫秒>\n", argv[0]);
            return 1;
        }
        int timeout_ms = atoi(argv[3]);
        inject_heartbeat_timeout(argv[2], timeout_ms);
    }
    // === 资源占用故障 ===
    else if (strcmp(command, "cpu-stress") == 0) {
        if (argc < 3) {
            printf(" 用法: %s cpu-stress <秒> [线程数]\n", argv[0]);
            return 1;
        }
        int duration = atoi(argv[2]);
        int threads = (argc >= 4) ? atoi(argv[3]) : 0;
        inject_cpu_stress(duration, threads);
    }
    else if (strcmp(command, "mem-stress") == 0) {
        if (argc < 3) {
            printf(" 用法: %s mem-stress <MB>\n", argv[0]);
            return 1;
        }
        int size_mb = atoi(argv[2]);
        inject_memory_stress(size_mb);
    }
    else if (strcmp(command, "mem-stress-clear") == 0) {
        inject_memory_stress(0);
    }
    // === HDFS故障 ===
    else if (strcmp(command, "hdfs-safe") == 0) {
        if (argc < 3) {
            printf(" 用法: %s hdfs-safe enter|leave\n", argv[0]);
            return 1;
        }
        if (strcmp(argv[2], "enter") == 0) {
            inject_hdfs_fault(1, NULL);
        } else if (strcmp(argv[2], "leave") == 0) {
            inject_hdfs_fault(2, NULL);
        } else {
            printf(" 参数必须是 enter 或 leave\n");
            return 1;
        }
    }
    else if (strcmp(command, "hdfs-disk") == 0) {
        if (argc < 3) {
            printf(" 用法: %s hdfs-disk <MB>\n", argv[0]);
            return 1;
        }
        inject_hdfs_fault(3, argv[2]);
    }
    else if (strcmp(command, "hdfs-disk-clear") == 0) {
        inject_hdfs_fault(4, NULL);
    }
    else if (strcmp(command, "hdfs-refresh") == 0) {
        inject_hdfs_fault(5, NULL);
    }
    // === YARN故障 ===
    else if (strcmp(command, "yarn-health") == 0) {
        if (argc < 3) {
            printf(" 用法: %s yarn-health fail|ok\n", argv[0]);
            return 1;
        }
        if (strcmp(argv[2], "fail") == 0) {
            inject_yarn_fault(1, NULL);
        } else if (strcmp(argv[2], "ok") == 0) {
            inject_yarn_fault(2, NULL);
        } else {
            printf(" 参数必须是 fail 或 ok\n");
            return 1;
        }
    }
    else if (strcmp(command, "yarn-refresh") == 0) {
        inject_yarn_fault(3, NULL);
        inject_yarn_fault(4, NULL);
    }
    else if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        print_usage(argv[0]);
    }
    else {
        printf(" 未知命令: %s\n", command);
        print_usage(argv[0]);
        return 1;
    }
    
    return 0;
}
