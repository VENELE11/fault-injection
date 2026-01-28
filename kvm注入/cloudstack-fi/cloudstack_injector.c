/*
 * cloudstack_injector.c - CloudStack云平台故障注入工具
 * 功能：针对CloudStack云计算平台进行故障注入
 * 支持：Management Server, Agent, 虚拟机, 存储等故障模拟
 * 编译：gcc -o cloudstack_injector cloudstack_injector.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

// === CloudStack组件进程名定义 ===
#define CS_MANAGEMENT "cloudstack-management"
#define CS_AGENT "cloudstack-agent"
#define CS_USAGE "cloudstack-usage"
#define MYSQL_PROC "mysqld"
#define NFS_PROC "nfsd"
#define LIBVIRTD_PROC "libvirtd"

// === CloudStack默认端口 ===
#define CS_API_PORT 8080
#define CS_AGENT_PORT 8250
#define CS_CONSOLE_PORT 8443

// === 故障类型枚举 ===
typedef enum {
    CS_FAULT_CRASH = 1,          // 进程崩溃
    CS_FAULT_HANG = 2,           // 进程挂起
    CS_FAULT_RESUME = 3,         // 恢复进程
    CS_FAULT_API_DELAY = 4,      // API响应延迟
    CS_FAULT_NETWORK = 5,        // 网络故障
    CS_FAULT_DB_SLOW = 6,        // 数据库慢查询
    CS_FAULT_STORAGE = 7,        // 存储故障
    CS_FAULT_AGENT_DISCONNECT = 8 // Agent断连
} CloudStackFaultType;

// === 组件类型枚举 ===
typedef enum {
    CS_COMPONENT_ALL = 0,
    CS_COMPONENT_MANAGEMENT = 1,
    CS_COMPONENT_AGENT = 2,
    CS_COMPONENT_USAGE = 3,
    CS_COMPONENT_MYSQL = 4,
    CS_COMPONENT_NFS = 5,
    CS_COMPONENT_LIBVIRT = 6
} CloudStackComponent;

// === 辅助函数：获取进程名 ===
const char* get_cs_component_name(CloudStackComponent component) {
    switch (component) {
        case CS_COMPONENT_MANAGEMENT: return CS_MANAGEMENT;
        case CS_COMPONENT_AGENT: return CS_AGENT;
        case CS_COMPONENT_USAGE: return CS_USAGE;
        case CS_COMPONENT_MYSQL: return MYSQL_PROC;
        case CS_COMPONENT_NFS: return NFS_PROC;
        case CS_COMPONENT_LIBVIRT: return LIBVIRTD_PROC;
        default: return NULL;
    }
}

// === 辅助函数：查找CloudStack进程PID ===
int find_cs_pid(const char *proc_name) {
    char cmd[256];
    char output[32];
    
    // 首先尝试systemctl检查服务状态
    snprintf(cmd, sizeof(cmd), 
             "systemctl show %s --property=MainPID 2>/dev/null | cut -d= -f2", 
             proc_name);
    
    FILE *fp = popen(cmd, "r");
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL) {
        int pid = atoi(output);
        pclose(fp);
        if (pid > 0) return pid;
    }
    if (fp) pclose(fp);
    
    // 备用方案：使用pgrep
    snprintf(cmd, sizeof(cmd), "pgrep -f '%s' | head -n 1", proc_name);
    
    fp = popen(cmd, "r");
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL) {
        pclose(fp);
        return atoi(output);
    }
    if (fp) pclose(fp);
    
    return -1;
}

// === 辅助函数：列出所有CloudStack相关进程 ===
void list_cloudstack_processes() {
    printf("\n=== CloudStack服务状态 ===\n");
    
    const char* components[] = {
        CS_MANAGEMENT, CS_AGENT, CS_USAGE,
        MYSQL_PROC, NFS_PROC, LIBVIRTD_PROC
    };
    const char* names[] = {
        "Management Server", "Agent", "Usage Server",
        "MySQL", "NFS Server", "Libvirtd"
    };
    
    for (int i = 0; i < 6; i++) {
        int pid = find_cs_pid(components[i]);
        if (pid > 0) {
            printf("  ✅ %-20s (PID: %d) - 运行中\n", names[i], pid);
        } else {
            printf("  ❌ %-20s - 未运行\n", names[i]);
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
int inject_cs_process_fault(CloudStackComponent component, CloudStackFaultType fault_type) {
    const char *proc_name = get_cs_component_name(component);
    if (!proc_name) {
        printf("❌ 无效的组件类型\n");
        return -1;
    }
    
    int pid = find_cs_pid(proc_name);
    if (pid == -1) {
        printf("❌ 未找到进程: %s\n", proc_name);
        return -1;
    }
    
    printf("[CloudStack注入] 目标: %s (PID: %d)\n", proc_name, pid);
    
    switch (fault_type) {
        case CS_FAULT_CRASH:
            if (kill(pid, SIGKILL) == 0) {
                printf("💥 [Crash] 已终止进程 %s\n", proc_name);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        case CS_FAULT_HANG:
            if (kill(pid, SIGSTOP) == 0) {
                printf("❄️  [Hang] 已暂停进程 %s\n", proc_name);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        case CS_FAULT_RESUME:
            if (kill(pid, SIGCONT) == 0) {
                printf("▶️  [Resume] 已恢复进程 %s\n", proc_name);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        default:
            printf("❌ 此故障类型不支持进程操作\n");
            return -1;
    }
    
    return 0;
}

// === 模块2：API故障注入（使用tc延迟） ===
int inject_api_fault(int delay_ms, int action) {
    char cmd[512];
    char nic[32];
    
    // 获取默认网卡
    FILE *fp = popen("ip route get 8.8.8.8 2>/dev/null | awk '{print $5; exit}'", "r");
    if (fp == NULL || fgets(nic, sizeof(nic), fp) == NULL) {
        strcpy(nic, "eth0");
    } else {
        nic[strcspn(nic, "\n")] = 0;
    }
    if (fp) pclose(fp);
    
    // 清理旧规则
    snprintf(cmd, sizeof(cmd), "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);
    
    if (action == 0) {
        printf("✅ API延迟已清理\n");
        return 0;
    }
    
    // 针对CloudStack API端口注入延迟
    snprintf(cmd, sizeof(cmd),
             "tc qdisc add dev %s root handle 1: prio; "
             "tc qdisc add dev %s parent 1:3 handle 30: netem delay %dms; "
             "tc filter add dev %s parent 1:0 protocol ip prio 3 u32 "
             "match ip dport %d 0xffff flowid 1:3",
             nic, nic, delay_ms, nic, CS_API_PORT);
    
    if (system(cmd) == 0) {
        printf("🐢 [API Delay] 已注入 %dms 延迟到端口 %d\n", delay_ms, CS_API_PORT);
    } else {
        // 简化方案
        snprintf(cmd, sizeof(cmd), 
                 "tc qdisc add dev %s root netem delay %dms", nic, delay_ms);
        system(cmd);
        printf("🐢 [Network Delay] 已注入全局 %dms 延迟\n", delay_ms);
    }
    
    return 0;
}

// === 模块3：网络故障注入 ===
int inject_cs_network_fault(const char *target_ip, int port, int action) {
    char cmd[512];
    
    if (action == 0) {
        // 清理规则
        snprintf(cmd, sizeof(cmd),
                 "iptables -D INPUT -s %s -j DROP 2>/dev/null; "
                 "iptables -D OUTPUT -d %s -j DROP 2>/dev/null",
                 target_ip, target_ip);
        system(cmd);
        printf("✅ 已清理与 %s 的网络隔离\n", target_ip);
    } else {
        // 注入网络分区
        if (port > 0) {
            snprintf(cmd, sizeof(cmd),
                     "iptables -A INPUT -s %s -p tcp --dport %d -j DROP; "
                     "iptables -A OUTPUT -d %s -p tcp --dport %d -j DROP",
                     target_ip, port, target_ip, port);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "iptables -A INPUT -s %s -j DROP; "
                     "iptables -A OUTPUT -d %s -j DROP",
                     target_ip, target_ip);
        }
        
        if (system(cmd) == 0) {
            if (port > 0) {
                printf("🚧 [Network Partition] 已隔离 %s 端口 %d\n", target_ip, port);
            } else {
                printf("🚧 [Network Partition] 已完全隔离节点 %s\n", target_ip);
            }
        } else {
            printf("⚠️  网络隔离命令执行失败\n");
            return -1;
        }
    }
    
    return 0;
}

// === 模块4：数据库故障注入 ===
int inject_db_fault(int fault_type, const char *param) {
    char cmd[1024];
    
    switch (fault_type) {
        case 1: // 数据库连接限制
            snprintf(cmd, sizeof(cmd),
                     "mysql -e \"SET GLOBAL max_connections = 5;\" 2>/dev/null");
            printf("🔒 [MySQL] 限制最大连接数为5\n");
            break;
            
        case 2: // 恢复数据库连接
            snprintf(cmd, sizeof(cmd),
                     "mysql -e \"SET GLOBAL max_connections = 151;\" 2>/dev/null");
            printf("🔓 [MySQL] 恢复最大连接数为151\n");
            break;
            
        case 3: // 模拟慢查询（设置全局延迟）
            if (param) {
                snprintf(cmd, sizeof(cmd),
                         "mysql -e \"SET GLOBAL long_query_time = %s;\" 2>/dev/null",
                         param);
                printf("🐢 [MySQL] 设置慢查询阈值为 %s 秒\n", param);
            }
            break;
            
        case 4: // 锁定表（模拟写阻塞）
            snprintf(cmd, sizeof(cmd),
                     "mysql cloud -e \"LOCK TABLES vm_instance WRITE;\" 2>/dev/null &");
            printf("🔐 [MySQL] 锁定vm_instance表\n");
            break;
            
        case 5: // 解锁表
            snprintf(cmd, sizeof(cmd),
                     "mysql cloud -e \"UNLOCK TABLES;\" 2>/dev/null");
            printf("🔓 [MySQL] 解锁所有表\n");
            break;
            
        default:
            printf("❌ 未知的数据库故障类型\n");
            return -1;
    }
    
    int ret = system(cmd);
    if (ret != 0) {
        printf("⚠️  命令执行返回异常 (Code: %d)\n", ret);
    }
    
    return ret;
}

// === 模块5：存储故障注入 ===
int inject_storage_fault(int fault_type, const char *mount_point) {
    char cmd[512];
    
    switch (fault_type) {
        case 1: // 模拟NFS挂载断开
            if (mount_point) {
                snprintf(cmd, sizeof(cmd), "umount -l %s 2>/dev/null", mount_point);
                printf("💾 [Storage] 卸载存储: %s\n", mount_point);
            }
            break;
            
        case 2: // 设置存储为只读
            if (mount_point) {
                snprintf(cmd, sizeof(cmd), 
                         "mount -o remount,ro %s 2>/dev/null", mount_point);
                printf("📁 [Storage] 设置 %s 为只读\n", mount_point);
            }
            break;
            
        case 3: // 恢复存储为读写
            if (mount_point) {
                snprintf(cmd, sizeof(cmd), 
                         "mount -o remount,rw %s 2>/dev/null", mount_point);
                printf("📂 [Storage] 恢复 %s 为读写\n", mount_point);
            }
            break;
            
        case 4: // 模拟存储满
            if (mount_point) {
                snprintf(cmd, sizeof(cmd),
                         "dd if=/dev/zero of=%s/cs_storage_fill bs=1M count=1024 2>/dev/null",
                         mount_point);
                printf("💿 [Storage] 在 %s 填充1GB空间\n", mount_point);
            }
            break;
            
        case 5: // 清理存储填充
            if (mount_point) {
                snprintf(cmd, sizeof(cmd), "rm -f %s/cs_storage_fill", mount_point);
                printf("🧹 [Storage] 清理存储填充文件\n");
            }
            break;
            
        default:
            printf("❌ 未知的存储故障类型\n");
            return -1;
    }
    
    int ret = system(cmd);
    return ret;
}

// === 模块6：Agent故障注入 ===
int inject_agent_fault(int fault_type, const char *agent_ip) {
    char cmd[512];
    
    switch (fault_type) {
        case 1: // 断开Agent连接（通过端口阻断）
            if (agent_ip) {
                snprintf(cmd, sizeof(cmd),
                         "iptables -A OUTPUT -d %s -p tcp --dport %d -j DROP",
                         agent_ip, CS_AGENT_PORT);
                printf("🔌 [Agent] 断开与 %s 的Agent连接\n", agent_ip);
            } else {
                // 本地Agent
                snprintf(cmd, sizeof(cmd),
                         "iptables -A OUTPUT -p tcp --dport %d -j DROP",
                         CS_AGENT_PORT);
                printf("🔌 [Agent] 阻断Agent端口 %d\n", CS_AGENT_PORT);
            }
            break;
            
        case 2: // 恢复Agent连接
            if (agent_ip) {
                snprintf(cmd, sizeof(cmd),
                         "iptables -D OUTPUT -d %s -p tcp --dport %d -j DROP 2>/dev/null",
                         agent_ip, CS_AGENT_PORT);
            } else {
                snprintf(cmd, sizeof(cmd),
                         "iptables -D OUTPUT -p tcp --dport %d -j DROP 2>/dev/null",
                         CS_AGENT_PORT);
            }
            printf("🔗 [Agent] 恢复Agent连接\n");
            break;
            
        case 3: // 模拟Agent心跳超时（通过限制带宽）
            snprintf(cmd, sizeof(cmd),
                     "tc qdisc add dev eth0 root tbf rate 1kbit burst 1kb latency 500ms 2>/dev/null");
            printf("💓 [Agent] 模拟心跳超时（极低带宽）\n");
            break;
            
        case 4: // 清理带宽限制
            snprintf(cmd, sizeof(cmd), "tc qdisc del dev eth0 root 2>/dev/null");
            printf("✅ [Agent] 清理带宽限制\n");
            break;
            
        default:
            printf("❌ 未知的Agent故障类型\n");
            return -1;
    }
    
    system(cmd);
    return 0;
}

// === 打印使用帮助 ===
void print_cs_usage(const char *prog) {
    printf("\n===========================================\n");
    printf("   CloudStack故障注入工具 v1.0\n");
    printf("===========================================\n\n");
    printf("用法: %s <命令> [参数]\n\n", prog);
    printf("命令:\n");
    printf("  list                        列出CloudStack服务状态\n");
    printf("  crash <组件>                终止指定组件进程\n");
    printf("  hang <组件>                 暂停指定组件进程\n");
    printf("  resume <组件>               恢复指定组件进程\n");
    printf("  api-delay <毫秒>            注入API响应延迟\n");
    printf("  api-delay-clear             清理API延迟\n");
    printf("  network <IP> [端口]         隔离指定IP的网络\n");
    printf("  network-clear <IP>          清理网络隔离\n");
    printf("  db-limit                    限制数据库连接\n");
    printf("  db-restore                  恢复数据库连接\n");
    printf("  db-lock                     锁定关键表\n");
    printf("  db-unlock                   解锁表\n");
    printf("  storage-ro <挂载点>         设置存储只读\n");
    printf("  storage-rw <挂载点>         恢复存储读写\n");
    printf("  storage-fill <挂载点>       模拟存储满\n");
    printf("  storage-clean <挂载点>      清理存储填充\n");
    printf("  agent-disconnect [IP]       断开Agent连接\n");
    printf("  agent-reconnect [IP]        恢复Agent连接\n\n");
    printf("组件代号:\n");
    printf("  ms      - Management Server\n");
    printf("  agent   - CloudStack Agent\n");
    printf("  usage   - Usage Server\n");
    printf("  mysql   - MySQL数据库\n");
    printf("  nfs     - NFS存储服务\n");
    printf("  libvirt - Libvirt服务\n\n");
    printf("示例:\n");
    printf("  %s list                     # 查看服务状态\n", prog);
    printf("  %s crash ms                 # 终止Management Server\n", prog);
    printf("  %s api-delay 500            # 注入500ms API延迟\n", prog);
    printf("  %s network 192.168.1.20     # 隔离计算节点\n", prog);
    printf("\n");
}

// === 解析组件参数 ===
CloudStackComponent parse_cs_component(const char *arg) {
    if (strcmp(arg, "ms") == 0) return CS_COMPONENT_MANAGEMENT;
    if (strcmp(arg, "agent") == 0) return CS_COMPONENT_AGENT;
    if (strcmp(arg, "usage") == 0) return CS_COMPONENT_USAGE;
    if (strcmp(arg, "mysql") == 0) return CS_COMPONENT_MYSQL;
    if (strcmp(arg, "nfs") == 0) return CS_COMPONENT_NFS;
    if (strcmp(arg, "libvirt") == 0) return CS_COMPONENT_LIBVIRT;
    return CS_COMPONENT_ALL;
}

// === 主函数 ===
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_cs_usage(argv[0]);
        return 1;
    }
    
    // 检查root权限
    if (geteuid() != 0) {
        printf("⚠️  警告: 大部分功能需要root权限运行\n");
    }
    
    const char *command = argv[1];
    
    // === 命令解析 ===
    if (strcmp(command, "list") == 0) {
        list_cloudstack_processes();
    }
    else if (strcmp(command, "crash") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s crash <组件>\n", argv[0]);
            return 1;
        }
        CloudStackComponent comp = parse_cs_component(argv[2]);
        if (comp == CS_COMPONENT_ALL) {
            printf("❌ 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_cs_process_fault(comp, CS_FAULT_CRASH);
    }
    else if (strcmp(command, "hang") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s hang <组件>\n", argv[0]);
            return 1;
        }
        CloudStackComponent comp = parse_cs_component(argv[2]);
        if (comp == CS_COMPONENT_ALL) {
            printf("❌ 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_cs_process_fault(comp, CS_FAULT_HANG);
    }
    else if (strcmp(command, "resume") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s resume <组件>\n", argv[0]);
            return 1;
        }
        CloudStackComponent comp = parse_cs_component(argv[2]);
        if (comp == CS_COMPONENT_ALL) {
            printf("❌ 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_cs_process_fault(comp, CS_FAULT_RESUME);
    }
    else if (strcmp(command, "api-delay") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s api-delay <毫秒>\n", argv[0]);
            return 1;
        }
        inject_api_fault(atoi(argv[2]), 1);
    }
    else if (strcmp(command, "api-delay-clear") == 0) {
        inject_api_fault(0, 0);
    }
    else if (strcmp(command, "network") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s network <IP> [端口]\n", argv[0]);
            return 1;
        }
        int port = (argc >= 4) ? atoi(argv[3]) : 0;
        inject_cs_network_fault(argv[2], port, 1);
    }
    else if (strcmp(command, "network-clear") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s network-clear <IP>\n", argv[0]);
            return 1;
        }
        inject_cs_network_fault(argv[2], 0, 0);
    }
    else if (strcmp(command, "db-limit") == 0) {
        inject_db_fault(1, NULL);
    }
    else if (strcmp(command, "db-restore") == 0) {
        inject_db_fault(2, NULL);
    }
    else if (strcmp(command, "db-lock") == 0) {
        inject_db_fault(4, NULL);
    }
    else if (strcmp(command, "db-unlock") == 0) {
        inject_db_fault(5, NULL);
    }
    else if (strcmp(command, "storage-ro") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s storage-ro <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(2, argv[2]);
    }
    else if (strcmp(command, "storage-rw") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s storage-rw <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(3, argv[2]);
    }
    else if (strcmp(command, "storage-fill") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s storage-fill <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(4, argv[2]);
    }
    else if (strcmp(command, "storage-clean") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s storage-clean <挂载点>\n", argv[0]);
            return 1;
        }
        inject_storage_fault(5, argv[2]);
    }
    else if (strcmp(command, "agent-disconnect") == 0) {
        const char *ip = (argc >= 3) ? argv[2] : NULL;
        inject_agent_fault(1, ip);
    }
    else if (strcmp(command, "agent-reconnect") == 0) {
        const char *ip = (argc >= 3) ? argv[2] : NULL;
        inject_agent_fault(2, ip);
    }
    else if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        print_cs_usage(argv[0]);
    }
    else {
        printf("❌ 未知命令: %s\n", command);
        print_cs_usage(argv[0]);
        return 1;
    }
    
    return 0;
}
