/*
 * hadoop_injector.c - Hadoop集群故障注入工具
 * 功能：针对Hadoop生态系统（HDFS/YARN/MapReduce）进行故障注入
 * 支持：NameNode, DataNode, ResourceManager, NodeManager故障模拟
 * 编译：gcc -o hadoop_injector hadoop_injector.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

// === Hadoop组件进程名定义 ===
#define NAMENODE_PROC "NameNode"
#define DATANODE_PROC "DataNode"
#define RESOURCE_MGR_PROC "ResourceManager"
#define NODE_MGR_PROC "NodeManager"
#define SECONDARY_NN_PROC "SecondaryNameNode"
#define HISTORY_SERVER_PROC "JobHistoryServer"

// === 故障类型枚举 ===
typedef enum {
    HADOOP_FAULT_CRASH = 1,      // 进程崩溃
    HADOOP_FAULT_HANG = 2,       // 进程挂起
    HADOOP_FAULT_RESUME = 3,     // 恢复进程
    HADOOP_FAULT_NETWORK = 4,    // 网络故障（节点间通信中断）
    HADOOP_FAULT_DISK_SLOW = 5,  // 磁盘IO慢
    HADOOP_FAULT_DISK_FULL = 6,  // 磁盘空间耗尽模拟
    HADOOP_FAULT_CPU_STRESS = 7, // CPU资源耗尽
    HADOOP_FAULT_MEM_STRESS = 8  // 内存资源耗尽
} HadoopFaultType;

// === 组件类型枚举 ===
typedef enum {
    COMPONENT_ALL = 0,
    COMPONENT_NAMENODE = 1,
    COMPONENT_DATANODE = 2,
    COMPONENT_RESOURCE_MGR = 3,
    COMPONENT_NODE_MGR = 4,
    COMPONENT_SECONDARY_NN = 5,
    COMPONENT_HISTORY_SERVER = 6
} HadoopComponent;

// === 辅助函数：获取进程名 ===
const char* get_component_name(HadoopComponent component) {
    switch (component) {
        case COMPONENT_NAMENODE: return NAMENODE_PROC;
        case COMPONENT_DATANODE: return DATANODE_PROC;
        case COMPONENT_RESOURCE_MGR: return RESOURCE_MGR_PROC;
        case COMPONENT_NODE_MGR: return NODE_MGR_PROC;
        case COMPONENT_SECONDARY_NN: return SECONDARY_NN_PROC;
        case COMPONENT_HISTORY_SERVER: return HISTORY_SERVER_PROC;
        default: return NULL;
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

// === 辅助函数：列出所有Hadoop进程 ===
void list_hadoop_processes() {
    printf("\n=== 当前Hadoop进程状态 ===\n");
    
    const char* components[] = {
        NAMENODE_PROC, DATANODE_PROC, RESOURCE_MGR_PROC,
        NODE_MGR_PROC, SECONDARY_NN_PROC, HISTORY_SERVER_PROC
    };
    
    for (int i = 0; i < 6; i++) {
        int pid = find_hadoop_pid(components[i]);
        if (pid > 0) {
            printf("  ✅ %s (PID: %d) - 运行中\n", components[i], pid);
        } else {
            printf("  ❌ %s - 未运行\n", components[i]);
        }
    }
    printf("\n");
}

// === 模块1：进程故障注入 ===
int inject_process_fault(HadoopComponent component, HadoopFaultType fault_type) {
    const char *proc_name = get_component_name(component);
    if (!proc_name) {
        printf("❌ 无效的组件类型\n");
        return -1;
    }
    
    int pid = find_hadoop_pid(proc_name);
    if (pid == -1) {
        printf("❌ 未找到进程: %s\n", proc_name);
        return -1;
    }
    
    printf("[Hadoop注入] 目标: %s (PID: %d)\n", proc_name, pid);
    
    switch (fault_type) {
        case HADOOP_FAULT_CRASH:
            if (kill(pid, SIGKILL) == 0) {
                printf("💥 [Crash] 已终止进程 %s\n", proc_name);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        case HADOOP_FAULT_HANG:
            if (kill(pid, SIGSTOP) == 0) {
                printf("❄️  [Hang] 已暂停进程 %s\n", proc_name);
            } else {
                perror("kill failed");
                return -1;
            }
            break;
            
        case HADOOP_FAULT_RESUME:
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
        printf("✅ 已清理与 %s 的网络隔离\n", target_ip);
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

// === 模块3：HDFS相关故障 ===
int inject_hdfs_fault(int fault_type, const char *param) {
    char cmd[1024];
    
    switch (fault_type) {
        case 1: // 强制进入安全模式
            snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -safemode enter");
            printf("🔒 [HDFS] 强制进入安全模式\n");
            break;
            
        case 2: // 退出安全模式
            snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -safemode leave");
            printf("🔓 [HDFS] 退出安全模式\n");
            break;
            
        case 3: // 模拟磁盘满（创建大文件占用空间）
            if (param) {
                snprintf(cmd, sizeof(cmd), 
                         "dd if=/dev/zero of=/tmp/hdfs_disk_fill bs=1M count=%s",
                         param);
                printf("💾 [HDFS] 模拟磁盘空间占用 %sMB\n", param);
            } else {
                printf("❌ 需要指定大小参数\n");
                return -1;
            }
            break;
            
        case 4: // 清理磁盘占用文件
            snprintf(cmd, sizeof(cmd), "rm -f /tmp/hdfs_disk_fill");
            printf("🧹 [HDFS] 清理模拟磁盘占用\n");
            break;
            
        case 5: // 强制刷新节点
            snprintf(cmd, sizeof(cmd), "hdfs dfsadmin -refreshNodes");
            printf("🔄 [HDFS] 刷新DataNode列表\n");
            break;
            
        default:
            printf("❌ 未知的HDFS故障类型\n");
            return -1;
    }
    
    int ret = system(cmd);
    if (ret != 0) {
        printf("⚠️  命令执行返回异常 (Code: %d)\n", ret);
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
                printf("🏥 [YARN] 标记节点健康检查失败\n");
            }
            break;
            
        case 2: // 恢复节点健康
            snprintf(cmd, sizeof(cmd), "rm -f /tmp/yarn_node_health_check");
            printf("💚 [YARN] 恢复节点健康状态\n");
            break;
            
        case 3: // 刷新节点
            snprintf(cmd, sizeof(cmd), "yarn rmadmin -refreshNodes");
            printf("🔄 [YARN] 刷新ResourceManager节点列表\n");
            break;
            
        case 4: // 刷新队列
            snprintf(cmd, sizeof(cmd), "yarn rmadmin -refreshQueues");
            printf("📋 [YARN] 刷新调度队列配置\n");
            break;
            
        default:
            printf("❌ 未知的YARN故障类型\n");
            return -1;
    }
    
    int ret = system(cmd);
    if (ret != 0) {
        printf("⚠️  命令执行返回异常 (Code: %d)\n", ret);
    }
    
    return ret;
}

// === 模块5：IO延迟注入 ===
int inject_io_delay(const char *mount_point, int delay_ms) {
    char cmd[512];
    
    if (delay_ms > 0) {
        // 使用tc对块设备模拟延迟（简化实现，实际可能需要更复杂的配置）
        printf("⏱️  [IO] 在 %s 注入 %dms 延迟\n", mount_point, delay_ms);
        printf("   注: 真实IO延迟注入建议使用dm-delay或fio工具\n");
        
        // 这里提供一个基于cgroups的简化方案
        snprintf(cmd, sizeof(cmd),
                 "echo '8:0 rbps=1048576 wbps=1048576' > "
                 "/sys/fs/cgroup/blkio/blkio.throttle.read_bps_device 2>/dev/null");
    } else {
        // 清理限速
        snprintf(cmd, sizeof(cmd),
                 "echo '' > /sys/fs/cgroup/blkio/blkio.throttle.read_bps_device 2>/dev/null");
        printf("✅ [IO] 清理IO限速\n");
    }
    
    system(cmd);
    return 0;
}

// === 打印使用帮助 ===
void print_usage(const char *prog) {
    printf("\n===========================================\n");
    printf("   Hadoop集群故障注入工具 v1.0\n");
    printf("===========================================\n\n");
    printf("用法: %s <命令> [参数]\n\n", prog);
    printf("命令:\n");
    printf("  list                       列出所有Hadoop进程状态\n");
    printf("  crash <组件>               终止指定组件进程\n");
    printf("  hang <组件>                暂停指定组件进程\n");
    printf("  resume <组件>              恢复指定组件进程\n");
    printf("  network <IP> [端口]        隔离指定IP的网络通信\n");
    printf("  network-clear <IP>         清理指定IP的网络隔离\n");
    printf("  hdfs-safe enter|leave      控制HDFS安全模式\n");
    printf("  hdfs-disk <MB>             模拟磁盘空间占用\n");
    printf("  hdfs-disk-clear            清理磁盘占用模拟\n");
    printf("  yarn-health fail|ok        设置YARN节点健康状态\n");
    printf("  yarn-refresh               刷新YARN节点和队列\n\n");
    printf("组件代号:\n");
    printf("  nn   - NameNode\n");
    printf("  dn   - DataNode\n");
    printf("  rm   - ResourceManager\n");
    printf("  nm   - NodeManager\n");
    printf("  snn  - SecondaryNameNode\n");
    printf("  jhs  - JobHistoryServer\n\n");
    printf("示例:\n");
    printf("  %s list                    # 查看所有Hadoop进程\n", prog);
    printf("  %s crash nn                # 终止NameNode\n", prog);
    printf("  %s network 192.168.1.11    # 隔离DataNode节点\n", prog);
    printf("  %s hdfs-safe enter         # 进入HDFS安全模式\n", prog);
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
    return COMPONENT_ALL;
}

// === 主函数 ===
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    // 检查root权限
    if (geteuid() != 0) {
        printf("⚠️  警告: 部分功能需要root权限运行\n");
    }
    
    const char *command = argv[1];
    
    // === 命令解析 ===
    if (strcmp(command, "list") == 0) {
        list_hadoop_processes();
    }
    else if (strcmp(command, "crash") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s crash <组件>\n", argv[0]);
            return 1;
        }
        HadoopComponent comp = parse_component(argv[2]);
        if (comp == COMPONENT_ALL) {
            printf("❌ 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_process_fault(comp, HADOOP_FAULT_CRASH);
    }
    else if (strcmp(command, "hang") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s hang <组件>\n", argv[0]);
            return 1;
        }
        HadoopComponent comp = parse_component(argv[2]);
        if (comp == COMPONENT_ALL) {
            printf("❌ 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_process_fault(comp, HADOOP_FAULT_HANG);
    }
    else if (strcmp(command, "resume") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s resume <组件>\n", argv[0]);
            return 1;
        }
        HadoopComponent comp = parse_component(argv[2]);
        if (comp == COMPONENT_ALL) {
            printf("❌ 无效的组件: %s\n", argv[2]);
            return 1;
        }
        inject_process_fault(comp, HADOOP_FAULT_RESUME);
    }
    else if (strcmp(command, "network") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s network <IP> [端口]\n", argv[0]);
            return 1;
        }
        int port = (argc >= 4) ? atoi(argv[3]) : 0;
        inject_network_fault(argv[2], port, 1);
    }
    else if (strcmp(command, "network-clear") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s network-clear <IP>\n", argv[0]);
            return 1;
        }
        inject_network_fault(argv[2], 0, 0);
    }
    else if (strcmp(command, "hdfs-safe") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s hdfs-safe enter|leave\n", argv[0]);
            return 1;
        }
        if (strcmp(argv[2], "enter") == 0) {
            inject_hdfs_fault(1, NULL);
        } else if (strcmp(argv[2], "leave") == 0) {
            inject_hdfs_fault(2, NULL);
        } else {
            printf("❌ 参数必须是 enter 或 leave\n");
            return 1;
        }
    }
    else if (strcmp(command, "hdfs-disk") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s hdfs-disk <MB>\n", argv[0]);
            return 1;
        }
        inject_hdfs_fault(3, argv[2]);
    }
    else if (strcmp(command, "hdfs-disk-clear") == 0) {
        inject_hdfs_fault(4, NULL);
    }
    else if (strcmp(command, "yarn-health") == 0) {
        if (argc < 3) {
            printf("❌ 用法: %s yarn-health fail|ok\n", argv[0]);
            return 1;
        }
        if (strcmp(argv[2], "fail") == 0) {
            inject_yarn_fault(1, NULL);
        } else if (strcmp(argv[2], "ok") == 0) {
            inject_yarn_fault(2, NULL);
        } else {
            printf("❌ 参数必须是 fail 或 ok\n");
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
        printf("❌ 未知命令: %s\n", command);
        print_usage(argv[0]);
        return 1;
    }
    
    return 0;
}
