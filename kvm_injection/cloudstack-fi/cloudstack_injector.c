/*
 * cloudstack_injector.c - CloudStack云平台故障注入工具 (增强版)
 *
 * 功能：针对CloudStack云计算平台进行多层次故障注入
 * 支持：
 *   - 存储故障：主存储、二级存储读写故障
 *   - 系统虚拟机故障：SSVM、CPVM、VR故障
 *   - 网络故障：管理节点与各组件间通信故障
 *   - 管理节点资源故障：CPU/内存占用
 *   - 虚拟机操作故障：创建、迁移、资源分配故障
 *
 * 编译：gcc -o cloudstack_injector cloudstack_injector.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <fcntl.h>
#include <dirent.h>

// === CloudStack组件进程名定义 ===
#define CS_MANAGEMENT "cloudstack-management"
#define CS_AGENT "cloudstack-agent"
#define CS_USAGE "cloudstack-usage"
#define MYSQL_PROC "mysqld"
#define NFS_PROC "nfsd"
#define LIBVIRTD_PROC "libvirtd"
// 系统虚拟机相关
#define SSVM_PROC "systemvm"     // 二级存储虚拟机
#define CPVM_PROC "consoleproxy" // 控制台代理虚拟机
#define VR_PROC "router"         // 虚拟路由器

// === CloudStack默认端口 ===
#define CS_API_PORT 8080
#define CS_AGENT_PORT 8250
#define CS_CONSOLE_PORT 8443
#define CS_CLUSTER_PORT 9090
#define MYSQL_PORT 3306
#define NFS_PORT 2049

// === 故障类型枚举 (扩展版) ===
typedef enum
{
    CS_FAULT_CRASH = 1,            // 进程崩溃
    CS_FAULT_HANG = 2,             // 进程挂起
    CS_FAULT_RESUME = 3,           // 恢复进程
    CS_FAULT_API_DELAY = 4,        // API响应延迟
    CS_FAULT_NETWORK = 5,          // 网络故障
    CS_FAULT_DB_SLOW = 6,          // 数据库慢查询
    CS_FAULT_STORAGE_READ = 7,     // 存储读故障
    CS_FAULT_STORAGE_WRITE = 8,    // 存储写故障
    CS_FAULT_AGENT_DISCONNECT = 9, // Agent断连
    CS_FAULT_SYSVM = 10,           // 系统虚拟机故障
    CS_FAULT_VM_CREATE = 11,       // 虚拟机创建故障
    CS_FAULT_VM_MIGRATE = 12,      // 虚拟机迁移故障
    CS_FAULT_CPU_STRESS = 13,      // CPU资源耗尽
    CS_FAULT_MEM_STRESS = 14       // 内存资源耗尽
} CloudStackFaultType;

// === 组件类型枚举 (扩展版) ===
typedef enum
{
    CS_COMPONENT_ALL = 0,
    CS_COMPONENT_MANAGEMENT = 1,
    CS_COMPONENT_AGENT = 2,
    CS_COMPONENT_USAGE = 3,
    CS_COMPONENT_MYSQL = 4,
    CS_COMPONENT_NFS = 5,
    CS_COMPONENT_LIBVIRT = 6,
    // 系统虚拟机
    CS_COMPONENT_SSVM = 7, // 二级存储虚拟机
    CS_COMPONENT_CPVM = 8, // 控制台代理虚拟机
    CS_COMPONENT_VR = 9    // 虚拟路由器
} CloudStackComponent;

// === 故障模型5元组 ===
typedef struct
{
    char layer[32];     // 故障层次 (CloudStack)
    char tool[64];      // 故障工具名
    char ip[32];        // 故障位置 (IP地址)
    char timestamp[32]; // 故障发生时间
    char params[128];   // 故障参数
} CSFaultModel;

// === 全局变量 ===
static volatile int g_stress_running = 0;
static pthread_t *g_stress_threads = NULL;
static int g_stress_thread_count = 0;

// === 辅助函数：获取进程名 ===
const char *get_cs_component_name(CloudStackComponent component)
{
    switch (component)
    {
    case CS_COMPONENT_MANAGEMENT:
        return CS_MANAGEMENT;
    case CS_COMPONENT_AGENT:
        return CS_AGENT;
    case CS_COMPONENT_USAGE:
        return CS_USAGE;
    case CS_COMPONENT_MYSQL:
        return MYSQL_PROC;
    case CS_COMPONENT_NFS:
        return NFS_PROC;
    case CS_COMPONENT_LIBVIRT:
        return LIBVIRTD_PROC;
    case CS_COMPONENT_SSVM:
        return "s-1-VM"; 
    case CS_COMPONENT_CPVM:
        return "v-2-VM"; 
    case CS_COMPONENT_VR:
        return "r-3-VM"; 
    default:
        return NULL;
    }
}

// === 辅助函数：获取组件中文描述 ===
const char *get_cs_component_desc(CloudStackComponent component)
{
    switch (component)
    {
    case CS_COMPONENT_MANAGEMENT:
        return "Management Server (管理节点)";
    case CS_COMPONENT_AGENT:
        return "CloudStack Agent (计算节点代理)";
    case CS_COMPONENT_USAGE:
        return "Usage Server (用量统计)";
    case CS_COMPONENT_MYSQL:
        return "MySQL Database (数据库)";
    case CS_COMPONENT_NFS:
        return "NFS Server (网络存储)";
    case CS_COMPONENT_LIBVIRT:
        return "Libvirtd (虚拟化服务)";
    case CS_COMPONENT_SSVM:
        return "Secondary Storage VM (二级存储虚拟机)";
    case CS_COMPONENT_CPVM:
        return "Console Proxy VM (控制台代理虚拟机)";
    case CS_COMPONENT_VR:
        return "Virtual Router (虚拟路由器)";
    default:
        return "未知组件";
    }
}

// === 辅助函数：获取默认网卡 ===
// 修复后的 get_default_nic 函数
void get_default_nic(char *nic, size_t size)
{
    FILE *fp = popen("ip route get 8.8.8.8 2>/dev/null | awk '{print $5; exit}'", "r");
    strcpy(nic, "eth0"); // 先给个默认值
    if (fp)
    {
        if (fgets(nic, size, fp) != NULL)
        {
            nic[strcspn(nic, "\n")] = 0;
        }
        pclose(fp); // 统一关闭
    }
}

// === 辅助函数：查找CloudStack进程PID ===
int find_cs_pid(const char *proc_name)
{
    char cmd[256];
    char output[32];
    int pid = -1;

    // 统一使用 pgrep 这种更通用的方式，避免 systemctl property 导致的复杂解析
    snprintf(cmd, sizeof(cmd), "pgrep -f '%s' | head -n 1", proc_name);

    FILE *fp = popen(cmd, "r");
    if (fp)
    {
        if (fgets(output, sizeof(output), fp) != NULL)
        {
            pid = atoi(output);
        }
        pclose(fp); // 确保在这里统一关闭，且只关闭一次
    }

    return pid;
}

// === 辅助函数：列出所有CloudStack相关进程 ===
void list_cloudstack_processes()
{
    printf("\n=== CloudStack服务状态 ===\n");

    const char *components[] = {
        CS_MANAGEMENT, CS_AGENT, CS_USAGE,
        MYSQL_PROC, NFS_PROC, LIBVIRTD_PROC};
    const char *names[] = {
        "Management Server", "Agent", "Usage Server",
        "MySQL", "NFS Server", "Libvirtd"};

    for (int i = 0; i < 6; i++)
    {
        int pid = find_cs_pid(components[i]);
        if (pid > 0)
        {
            printf("   %-20s (PID: %d) - 运行中\n", names[i], pid);
        }
        else
        {
            printf("   %-20s - 未运行\n", names[i]);
        }
    }

    // 检查关键端口
    printf("\n=== 关键端口状态 ===\n");
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "ss -tlnp 2>/dev/null | grep -E ':%d|:%d|:%d' | head -5",
             CS_API_PORT, CS_AGENT_PORT, CS_CONSOLE_PORT);
    printf("  API端口 (%d), Agent端口 (%d), Console端口 (%d)\n",
           CS_API_PORT, CS_AGENT_PORT, CS_CONSOLE_PORT);
    system(cmd);
    printf("\n");
}

// === 模块1：进程故障注入 ===
int inject_cs_process_fault(CloudStackComponent component, CloudStackFaultType fault_type)
{
    const char *proc_name = get_cs_component_name(component);
    if (!proc_name)
    {
        printf(" 无效的组件类型\n");
        return -1;
    }

    int pid = find_cs_pid(proc_name);
    if (pid == -1)
    {
        printf(" 未找到进程: %s\n", proc_name);
        return -1;
    }

    printf("[CloudStack注入] 目标: %s (PID: %d)\n", proc_name, pid);

    switch (fault_type)
    {
    case CS_FAULT_CRASH:
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

    case CS_FAULT_HANG:
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

    case CS_FAULT_RESUME:
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

// === 模块2：API故障注入（使用tc延迟） ===
int inject_api_fault(int delay_ms, int action)
{
    char cmd[512];
    char nic[32];

    // 获取默认网卡
    FILE *fp = popen("ip route get 8.8.8.8 2>/dev/null | awk '{print $5; exit}'", "r");
    if (fp == NULL || fgets(nic, sizeof(nic), fp) == NULL)
    {
        strcpy(nic, "eth0");
    }
    else
    {
        nic[strcspn(nic, "\n")] = 0;
    }
    if (fp)
        pclose(fp);

    // 清理旧规则（包括 filter）
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null; tc filter del dev %s parent 1:0 protocol ip prio 3 2>/dev/null", nic, nic);
    system(cmd);

    if (action == 0)
    {
        printf(" API延迟已清理\n");
        return 0;
    }

    // 注入延迟，匹配源端口（sport）8080
    snprintf(cmd, sizeof(cmd),
             "tc qdisc add dev %s root handle 1: prio; "
             "tc qdisc add dev %s parent 1:3 handle 30: netem delay %dms; "
             "tc filter add dev %s parent 1:0 protocol ip prio 3 u32 "
             "match ip sport %d 0xffff flowid 1:3",
             nic, nic, delay_ms, nic, CS_API_PORT);

    if (system(cmd) == 0)
    {
        printf(" [API Delay] 已注入 %dms 延迟到端口 %d（响应流量）\n", delay_ms, CS_API_PORT);
    }
    else
    {
        // 简化方案
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem delay %dms", nic, delay_ms);
        system(cmd);
        printf(" [Network Delay] 已注入全局 %dms 延迟\n", delay_ms);
    }

    return 0;
}

// === 模块3：网络故障注入 ===
int inject_cs_network_fault(const char *target_ip, int port, int action)
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
                     "iptables -A OUTPUT -d %s -p tcp --dport %d -j DROP",
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

// === 模块4：数据库故障注入 ===
int inject_db_fault(int fault_type, const char *param)
{
    char cmd[1024];

    switch (fault_type)
    {
    case 1: // 数据库连接限制
        snprintf(cmd, sizeof(cmd),
                 "mysql -e \"SET GLOBAL max_connections = 5;\" 2>/dev/null");
        printf(" [MySQL] 限制最大连接数为5\n");
        break;

    case 2: // 恢复数据库连接
        snprintf(cmd, sizeof(cmd),
                 "mysql -e \"SET GLOBAL max_connections = 151;\" 2>/dev/null");
        printf(" [MySQL] 恢复最大连接数为151\n");
        break;

    case 3: // 模拟慢查询（设置全局延迟）
        if (param)
        {
            snprintf(cmd, sizeof(cmd),
                     "mysql -e \"SET GLOBAL long_query_time = %s;\" 2>/dev/null",
                     param);
            printf(" [MySQL] 设置慢查询阈值为 %s 秒\n", param);
        }
        break;

    case 4: // 锁定表（模拟写阻塞）
        snprintf(cmd, sizeof(cmd),
                 "mysql cloud -e \"LOCK TABLES vm_instance WRITE;\" 2>/dev/null &");
        printf(" [MySQL] 锁定vm_instance表\n");
        break;

    case 5: // 解锁表
        snprintf(cmd, sizeof(cmd),
                 "mysql cloud -e \"UNLOCK TABLES;\" 2>/dev/null");
        printf(" [MySQL] 解锁所有表\n");
        break;

    default:
        printf(" 未知的数据库故障类型\n");
        return -1;
    }

    int ret = system(cmd);
    if (ret != 0)
    {
        printf("  命令执行返回异常 (Code: %d)\n", ret);
    }

    return ret;
}

// === 模块5：存储故障注入 ===
int inject_storage_fault(int fault_type, const char *mount_point)
{
    char cmd[512];

    switch (fault_type)
    {
    case 1: // 模拟NFS挂载断开
        if (mount_point)
        {
            snprintf(cmd, sizeof(cmd), "umount -l %s 2>/dev/null", mount_point);
            printf(" [Storage] 卸载存储: %s\n", mount_point);
            printf("   预期: CloudStack将检测到存储不可用\n");
        }
        break;

    case 2: // 设置存储为只读
        if (mount_point)
        {
            snprintf(cmd, sizeof(cmd),
                     "mount -o remount,ro %s 2>/dev/null", mount_point);
            printf(" [Storage] 设置 %s 为只读 (模拟写失效)\n", mount_point);
            printf("   预期: 虚拟机创建/快照等写操作将失败\n");
        }
        break;

    case 3: // 恢复存储为读写
        if (mount_point)
        {
            snprintf(cmd, sizeof(cmd),
                     "mount -o remount,rw %s 2>/dev/null", mount_point);
            printf(" [Storage] 恢复 %s 为读写\n", mount_point);
        }
        break;

    case 4: // 模拟存储满
        if (mount_point)
        {
            snprintf(cmd, sizeof(cmd),
                     "dd if=/dev/zero of=%s/cs_storage_fill bs=1M count=1024 2>/dev/null",
                     mount_point);
            printf(" [Storage] 在 %s 填充1GB空间\n", mount_point);
        }
        break;

    case 5: // 清理存储填充
        if (mount_point)
        {
            snprintf(cmd, sizeof(cmd), "rm -f %s/cs_storage_fill", mount_point);
            printf(" [Storage] 清理存储填充文件\n");
        }
        break;

    default:
        printf(" 未知的存储故障类型\n");
        return -1;
    }

    int ret = system(cmd);
    return ret;
}

// === 模块6：Agent故障注入 ===
int inject_agent_fault(int fault_type, const char *agent_ip)
{
    char cmd[512];

    switch (fault_type)
    {
    case 1: // 断开Agent连接（通过端口阻断）
        if (agent_ip)
        {
            snprintf(cmd, sizeof(cmd),
                     "iptables -A OUTPUT -d %s -p tcp --dport %d -j DROP",
                     agent_ip, CS_AGENT_PORT);
            printf(" [Agent] 断开与 %s 的Agent连接\n", agent_ip);
        }
        else
        {
            // 本地Agent
            snprintf(cmd, sizeof(cmd),
                     "iptables -A OUTPUT -p tcp --dport %d -j DROP",
                     CS_AGENT_PORT);
            printf(" [Agent] 阻断Agent端口 %d\n", CS_AGENT_PORT);
        }
        break;

    case 2: // 恢复Agent连接
        if (agent_ip)
        {
            snprintf(cmd, sizeof(cmd),
                     "iptables -D OUTPUT -d %s -p tcp --dport %d -j DROP 2>/dev/null",
                     agent_ip, CS_AGENT_PORT);
        }
        else
        {
            snprintf(cmd, sizeof(cmd),
                     "iptables -D OUTPUT -p tcp --dport %d -j DROP 2>/dev/null",
                     CS_AGENT_PORT);
        }
        printf(" [Agent] 恢复Agent连接\n");
        break;

    case 3: // 模拟Agent心跳超时（通过限制带宽）
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev eth0 root tbf rate 1kbit burst 1kb latency 500ms 2>/dev/null");
        printf(" [Agent] 模拟心跳超时（极低带宽）\n");
        break;

    case 4: // 清理带宽限制
        snprintf(cmd, sizeof(cmd), "tc qdisc del dev eth0 root 2>/dev/null");
        printf(" [Agent] 清理带宽限制\n");
        break;

    default:
        printf(" 未知的Agent故障类型\n");
        return -1;
    }

    system(cmd);
    return 0;
}

// === 模块7：系统虚拟机故障注入 ===
int inject_sysvm_fault(int vm_type, int fault_type)
{
    char cmd[1024];
    char vm_name[64] = "";
    char vm_type_name[64] = "";

    // 确定系统虚拟机类型
    switch (vm_type)
    {
    case 1: // 二级存储虚拟机
        strcpy(vm_name, "s-*-VM");
        strcpy(vm_type_name, "Secondary Storage VM");
        break;
    case 2: // 控制台代理虚拟机
        strcpy(vm_name, "v-*-VM");
        strcpy(vm_type_name, "Console Proxy VM");
        break;
    case 3: // 虚拟路由器
        strcpy(vm_name, "r-*-VM");
        strcpy(vm_type_name, "Virtual Router");
        break;
    default:
        printf(" 未知的系统虚拟机类型\n");
        return -1;
    }

    printf("  [SystemVM] 目标: %s\n", vm_type_name);

    // 查找系统虚拟机
    snprintf(cmd, sizeof(cmd),
             "virsh list --name 2>/dev/null | grep -E '^[svr]-[0-9]+-VM$' | head -n 1");

    FILE *fp = popen(cmd, "r");
    char vm_domain[128] = "";
    if (fp && fgets(vm_domain, sizeof(vm_domain), fp))
    {
        vm_domain[strcspn(vm_domain, "\n")] = 0;
        pclose(fp);
    }
    else
    {
        if (fp)
            pclose(fp);
        printf(" 未找到系统虚拟机 (请确保CloudStack正在运行)\n");
        return -1;
    }

    switch (fault_type)
    {
    case CS_FAULT_CRASH:
        snprintf(cmd, sizeof(cmd), "virsh destroy %s 2>/dev/null", vm_domain);
        printf(" [SystemVM] 强制关闭 %s (%s)\n", vm_type_name, vm_domain);
        printf("   预期: CloudStack会检测到系统虚拟机异常并尝试重启\n");
        break;

    case CS_FAULT_HANG:
        snprintf(cmd, sizeof(cmd), "virsh suspend %s 2>/dev/null", vm_domain);
        printf("  [SystemVM] 挂起 %s (%s)\n", vm_type_name, vm_domain);
        break;

    case CS_FAULT_RESUME:
        snprintf(cmd, sizeof(cmd), "virsh resume %s 2>/dev/null", vm_domain);
        printf("  [SystemVM] 恢复 %s (%s)\n", vm_type_name, vm_domain);
        break;

    default:
        printf(" 不支持的故障类型\n");
        return -1;
    }

    return system(cmd);
}

// === 模块8：CPU资源耗尽注入 ===
void *cs_cpu_stress_worker(void *arg)
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

int inject_cs_cpu_stress(int duration_sec, int num_threads)
{
    if (num_threads <= 0)
    {
        num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    }

    printf(" [CPU Stress] 管理节点CPU压力测试: %d线程, %d秒\n",
           num_threads, duration_sec);
    printf("   预期: 管理节点响应变慢，部分控制命令可能无法执行\n");

    g_stress_running = 1;
    g_stress_thread_count = num_threads;
    g_stress_threads = malloc(num_threads * sizeof(pthread_t));

    if (!g_stress_threads)
    {
        perror("malloc failed");
        return -1;
    }

    for (int i = 0; i < num_threads; i++)
    {
        pthread_create(&g_stress_threads[i], NULL, cs_cpu_stress_worker, NULL);
    }

    sleep(duration_sec);

    g_stress_running = 0;
    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(g_stress_threads[i], NULL);
    }

    free(g_stress_threads);
    g_stress_threads = NULL;

    printf(" [CPU Stress] 压力测试完成\n");
    return 0;
}

// === 模块9：内存资源耗尽注入 ===
int inject_cs_memory_stress(int size_mb)
{
    char cmd[256];

    if (size_mb <= 0)
    {
        snprintf(cmd, sizeof(cmd), "rm -f /tmp/cs_mem_stress 2>/dev/null");
        system(cmd);
        printf(" [Memory] 清理内存压力\n");
        return 0;
    }

    printf(" [Memory Stress] 管理节点内存压力: 占用 %d MB\n", size_mb);
    printf("   预期: 管理节点内存不足，可能导致OOM或服务降级\n");

    snprintf(cmd, sizeof(cmd),
             "dd if=/dev/zero of=/tmp/cs_mem_stress bs=1M count=%d 2>/dev/null && "
             "cat /tmp/cs_mem_stress > /dev/null &",
             size_mb);

    return system(cmd);
}

// === 模块10：虚拟机操作故障模拟 ===
int inject_vm_operation_fault(int op_type, const char *target)
{
    char cmd[512];
    char nic[32];

    get_default_nic(nic, sizeof(nic));

    switch (op_type)
    {
    case 1: // 模拟VM创建失败 (通过阻断存储访问)
        printf(" [VM Operation] 模拟虚拟机创建故障\n");
        printf("   方法: 临时阻断存储访问，导致磁盘创建失败\n");
        // 通过注入存储延迟来模拟
        if (target)
        {
            snprintf(cmd, sizeof(cmd),
                     "tc qdisc add dev %s root netem delay 5000ms", nic);
            system(cmd);
        }
        break;

    case 2: // 模拟VM迁移失败
        printf(" [VM Operation] 模拟虚拟机迁移故障\n");
        printf("   方法: 注入网络延迟，导致迁移超时\n");
        snprintf(cmd, sizeof(cmd),
                 "tc qdisc add dev %s root netem delay 3000ms loss 30%%", nic);
        system(cmd);
        break;

    case 3: // 清理操作故障
        snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
        system(cmd);
        printf(" [VM Operation] 清理操作故障模拟\n");
        break;

    default:
        printf(" 未知的操作故障类型\n");
        return -1;
    }

    return 0;
}

// === 打印使用帮助 ===
void print_cs_usage(const char *prog)
{
    printf("\n╔═══════════════════════════════════════════════════════════════════╗\n");
    printf("║        CloudStack故障注入工具 v2.0 (增强版)                       ║\n");
    printf("║                     ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════╝\n\n");
    printf("用法: %s <命令> [参数]\n\n", prog);

    printf("【进程故障注入】\n");
    printf("  list                        列出CloudStack服务状态\n");
    printf("  crash <组件>                终止指定组件进程\n");
    printf("  hang <组件>                 暂停指定组件进程\n");
    printf("  resume <组件>               恢复指定组件进程\n\n");

    printf("【系统虚拟机故障】\n");
    printf("  sysvm-crash <类型>          强制关闭系统虚拟机\n");
    printf("  sysvm-hang <类型>           挂起系统虚拟机\n");
    printf("  sysvm-resume <类型>         恢复系统虚拟机\n");
    printf("  类型: ssvm(二级存储), cpvm(控制台), vr(虚拟路由器)\n\n");

    printf("【网络故障注入】\n");
    printf("  api-delay <毫秒>            注入API响应延迟\n");
    printf("  api-delay-clear             清理API延迟\n");
    printf("  network <IP> [端口]         隔离指定IP的网络\n");
    printf("  network-clear <IP>          清理网络隔离\n");
    printf("  agent-disconnect [IP]       断开Agent连接\n");
    printf("  agent-reconnect [IP]        恢复Agent连接\n\n");

    printf("【存储故障注入】\n");
    printf("  storage-umount <挂载点>     卸载存储\n");
    printf("  storage-ro <挂载点>         设置存储只读 (写失效)\n");
    printf("  storage-rw <挂载点>         恢复存储读写\n");
    printf("  storage-fill <挂载点>       模拟存储满\n");
    printf("  storage-clean <挂载点>      清理存储填充\n\n");

    printf("【数据库故障注入】\n");
    printf("  db-limit                    限制数据库连接数\n");
    printf("  db-restore                  恢复数据库连接数\n");
    printf("  db-lock                     锁定关键表\n");
    printf("  db-unlock                   解锁表\n\n");

    printf("【资源占用故障】\n");
    printf("  cpu-stress <秒> [线程数]    CPU资源耗尽\n");
    printf("  mem-stress <MB>             内存资源耗尽\n");
    printf("  mem-stress-clear            清理内存占用\n\n");

    printf("【虚拟机操作故障】\n");
    printf("  vm-create-fail              模拟VM创建失败\n");
    printf("  vm-migrate-fail             模拟VM迁移失败\n");
    printf("  vm-op-clear                 清理操作故障\n\n");

    printf("【组件代号】\n");
    printf("  ms      - Management Server    agent   - CloudStack Agent\n");
    printf("  usage   - Usage Server         mysql   - MySQL数据库\n");
    printf("  nfs     - NFS存储服务          libvirt - Libvirt服务\n");
    printf("  ssvm    - 二级存储虚拟机       cpvm    - 控制台代理虚拟机\n");
    printf("  vr      - 虚拟路由器\n\n");

    printf("【示例】\n");
    printf("  %s list                      # 查看服务状态\n", prog);
    printf("  %s crash ms                  # 终止Management Server\n", prog);
    printf("  %s sysvm-crash ssvm          # 关闭二级存储虚拟机\n", prog);
    printf("  %s cpu-stress 30 4           # 30秒CPU压力(4线程)\n", prog);
    printf("  %s storage-ro /mnt/secondary # 设置二级存储只读\n", prog);
    printf("\n");
}

// === 解析组件参数 ===
CloudStackComponent parse_cs_component(const char *arg)
{
    if (strcmp(arg, "ms") == 0)
        return CS_COMPONENT_MANAGEMENT;
    if (strcmp(arg, "agent") == 0)
        return CS_COMPONENT_AGENT;
    if (strcmp(arg, "usage") == 0)
        return CS_COMPONENT_USAGE;
    if (strcmp(arg, "mysql") == 0)
        return CS_COMPONENT_MYSQL;
    if (strcmp(arg, "nfs") == 0)
        return CS_COMPONENT_NFS;
    if (strcmp(arg, "libvirt") == 0)
        return CS_COMPONENT_LIBVIRT;
    if (strcmp(arg, "ssvm") == 0)
        return CS_COMPONENT_SSVM;
    if (strcmp(arg, "cpvm") == 0)
        return CS_COMPONENT_CPVM;
    if (strcmp(arg, "vr") == 0)
        return CS_COMPONENT_VR;
    return CS_COMPONENT_ALL;
}

// === 解析系统虚拟机类型 ===
int parse_sysvm_type(const char *arg)
{
    if (strcmp(arg, "ssvm") == 0)
        return 1; // Secondary Storage VM
    if (strcmp(arg, "cpvm") == 0)
        return 2; // Console Proxy VM
    if (strcmp(arg, "vr") == 0)
        return 3; // Virtual Router
    return 0;
}

// === 主函数 ===
int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        print_cs_usage(argv[0]);
        return 1;
    }

    // 检查root权限
    if (geteuid() != 0)
    {
        printf("  警告: 大部分功能需要root权限运行\n");
    }

    const char *command = argv[1];

    // === 命令解析 ===
    if (strcmp(command, "list") == 0)
    {
        list_cloudstack_processes();
    }
    // === 进程故障 ===
    else if (strcmp(command, "crash") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s crash <组件>\n", argv[0]);
            return 1;
        }
        CloudStackComponent comp = parse_cs_component(argv[2]);
        if (comp == CS_COMPONENT_ALL)
        {
            printf(" 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_cs_process_fault(comp, CS_FAULT_CRASH);
    }
    else if (strcmp(command, "hang") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s hang <组件>\n", argv[0]);
            return 1;
        }
        CloudStackComponent comp = parse_cs_component(argv[2]);
        if (comp == CS_COMPONENT_ALL)
        {
            printf(" 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_cs_process_fault(comp, CS_FAULT_HANG);
    }
    else if (strcmp(command, "resume") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s resume <组件>\n", argv[0]);
            return 1;
        }
        CloudStackComponent comp = parse_cs_component(argv[2]);
        if (comp == CS_COMPONENT_ALL)
        {
            printf(" 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_cs_process_fault(comp, CS_FAULT_RESUME);
    }
    // === 系统虚拟机故障 ===
    else if (strcmp(command, "sysvm-crash") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s sysvm-crash <ssvm|cpvm|vr>\n", argv[0]);
            return 1;
        }
        int vm_type = parse_sysvm_type(argv[2]);
        if (vm_type == 0)
        {
            printf(" 无效的系统虚拟机类型: %s\n", argv[2]);
            return 1;
        }
        inject_sysvm_fault(vm_type, CS_FAULT_CRASH);
    }
    else if (strcmp(command, "sysvm-hang") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s sysvm-hang <ssvm|cpvm|vr>\n", argv[0]);
            return 1;
        }
        int vm_type = parse_sysvm_type(argv[2]);
        if (vm_type == 0)
        {
            printf(" 无效的系统虚拟机类型: %s\n", argv[2]);
            return 1;
        }
        inject_sysvm_fault(vm_type, CS_FAULT_HANG);
    }
    else if (strcmp(command, "sysvm-resume") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s sysvm-resume <ssvm|cpvm|vr>\n", argv[0]);
            return 1;
        }
        int vm_type = parse_sysvm_type(argv[2]);
        if (vm_type == 0)
        {
            printf(" 无效的系统虚拟机类型: %s\n", argv[2]);
            return 1;
        }
        inject_sysvm_fault(vm_type, CS_FAULT_RESUME);
    }
    // === 网络故障 ===
    else if (strcmp(command, "api-delay") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s api-delay <毫秒>\n", argv[0]);
            return 1;
        }
        inject_api_fault(atoi(argv[2]), 1);
    }
    else if (strcmp(command, "api-delay-clear") == 0)
    {
        inject_api_fault(0, 0);
    }
    else if (strcmp(command, "network") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s network <IP> [端口]\n", argv[0]);
            return 1;
        }
        int port = (argc >= 4) ? atoi(argv[3]) : 0;
        inject_cs_network_fault(argv[2], port, 1);
    }
    else if (strcmp(command, "network-clear") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s network-clear <IP>\n", argv[0]);
            return 1;
        }
        inject_cs_network_fault(argv[2], 0, 0);
    }
    // === 数据库故障 ===
    else if (strcmp(command, "db-limit") == 0)
    {
        inject_db_fault(1, NULL);
    }
    else if (strcmp(command, "db-restore") == 0)
    {
        inject_db_fault(2, NULL);
    }
    else if (strcmp(command, "db-lock") == 0)
    {
        inject_db_fault(4, NULL);
    }
    else if (strcmp(command, "db-unlock") == 0)
    {
        inject_db_fault(5, NULL);
    }
    // === 存储故障 ===
    else if (strcmp(command, "storage-umount") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s storage-umount <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(1, argv[2]);
    }
    else if (strcmp(command, "storage-ro") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s storage-ro <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(2, argv[2]);
    }
    else if (strcmp(command, "storage-rw") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s storage-rw <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(3, argv[2]);
    }
    else if (strcmp(command, "storage-fill") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s storage-fill <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(4, argv[2]);
    }
    else if (strcmp(command, "storage-clean") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s storage-clean <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(5, argv[2]);
    }
    // === Agent故障 ===
    else if (strcmp(command, "agent-disconnect") == 0)
    {
        const char *ip = (argc >= 3) ? argv[2] : NULL;
        inject_agent_fault(1, ip);
    }
    else if (strcmp(command, "agent-reconnect") == 0)
    {
        const char *ip = (argc >= 3) ? argv[2] : NULL;
        inject_agent_fault(2, ip);
    }
    // === 资源占用故障 ===
    else if (strcmp(command, "cpu-stress") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s cpu-stress <秒> [线程数]\n", argv[0]);
            return 1;
        }
        int duration = atoi(argv[2]);
        int threads = (argc >= 4) ? atoi(argv[3]) : 0;
        inject_cs_cpu_stress(duration, threads);
    }
    else if (strcmp(command, "mem-stress") == 0)
    {
        if (argc < 3)
        {
            printf(" 用法: %s mem-stress <MB>\n", argv[0]);
            return 1;
        }
        inject_cs_memory_stress(atoi(argv[2]));
    }
    else if (strcmp(command, "mem-stress-clear") == 0)
    {
        inject_cs_memory_stress(0);
    }
    // === 虚拟机操作故障 ===
    else if (strcmp(command, "vm-create-fail") == 0)
    {
        inject_vm_operation_fault(1, NULL);
    }
    else if (strcmp(command, "vm-migrate-fail") == 0)
    {
        inject_vm_operation_fault(2, NULL);
    }
    else if (strcmp(command, "vm-op-clear") == 0)
    {
        inject_vm_operation_fault(3, NULL);
    }
    else if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0)
    {
        print_cs_usage(argv[0]);
    }
    else
    {
        printf(" 未知命令: %s\n", command);
        print_cs_usage(argv[0]);
        return 1;
    }

    return 0;
}
