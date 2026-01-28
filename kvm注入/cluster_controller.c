/*
 * cluster_controller.c - 集群故障注入统一控制器
 * 功能：集成虚拟机、Hadoop、CloudStack故障注入的统一控制界面
 * 支持：多节点协调注入、批量操作、故障场景预设
 * 编译：gcc -o cluster_controller cluster_controller.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <time.h>

// === 集群节点配置 ===
#define MAX_NODES 10
#define MAX_CMD_LEN 1024

typedef struct {
    char name[32];      // 节点名称
    char ip[32];        // 节点IP
    int ssh_port;       // SSH端口
    char role[64];      // 角色：master/slave/agent等
    int is_active;      // 是否活跃
} ClusterNode;

// 全局集群配置
static ClusterNode g_cluster[MAX_NODES];
static int g_node_count = 0;

// === 预设的3节点Hadoop集群配置 ===
void init_hadoop_cluster() {
    g_node_count = 3;
    
    // Master节点
    strcpy(g_cluster[0].name, "master");
    strcpy(g_cluster[0].ip, "192.168.64.10");
    g_cluster[0].ssh_port = 22;
    strcpy(g_cluster[0].role, "NameNode,ResourceManager");
    g_cluster[0].is_active = 1;
    
    // Slave1节点
    strcpy(g_cluster[1].name, "slave1");
    strcpy(g_cluster[1].ip, "192.168.64.11");
    g_cluster[1].ssh_port = 22;
    strcpy(g_cluster[1].role, "DataNode,NodeManager");
    g_cluster[1].is_active = 1;
    
    // Slave2节点
    strcpy(g_cluster[2].name, "slave2");
    strcpy(g_cluster[2].ip, "192.168.64.12");
    g_cluster[2].ssh_port = 22;
    strcpy(g_cluster[2].role, "DataNode,NodeManager");
    g_cluster[2].is_active = 1;
    
    printf("✅ 已加载默认Hadoop集群配置 (3节点)\n");
}

// === 从配置文件加载集群 ===
int load_cluster_config(const char *config_file) {
    FILE *fp = fopen(config_file, "r");
    if (!fp) {
        printf("⚠️  无法打开配置文件: %s，使用默认配置\n", config_file);
        init_hadoop_cluster();
        return -1;
    }
    
    char line[256];
    g_node_count = 0;
    
    while (fgets(line, sizeof(line), fp) && g_node_count < MAX_NODES) {
        // 跳过注释和空行
        if (line[0] == '#' || line[0] == '\n') continue;
        
        // 格式：name,ip,port,role
        char *token = strtok(line, ",");
        if (token) {
            strcpy(g_cluster[g_node_count].name, token);
            token = strtok(NULL, ",");
            if (token) strcpy(g_cluster[g_node_count].ip, token);
            token = strtok(NULL, ",");
            if (token) g_cluster[g_node_count].ssh_port = atoi(token);
            token = strtok(NULL, ",\n");
            if (token) strcpy(g_cluster[g_node_count].role, token);
            g_cluster[g_node_count].is_active = 1;
            g_node_count++;
        }
    }
    
    fclose(fp);
    printf("✅ 已从 %s 加载 %d 个节点配置\n", config_file, g_node_count);
    return 0;
}

// === 显示集群状态 ===
void show_cluster_status() {
    printf("\n╔════════════════════════════════════════════════════════════╗\n");
    printf("║                    集群节点状态                            ║\n");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    printf("║ %-8s │ %-15s │ %-6s │ %-22s ║\n", "节点", "IP地址", "端口", "角色");
    printf("╠════════════════════════════════════════════════════════════╣\n");
    
    for (int i = 0; i < g_node_count; i++) {
        const char *status = g_cluster[i].is_active ? "[+]" : "[-]";
        printf("║ %-3s %-4s │ %-15s │ %-6d │ %-22s ║\n",
               status,
               g_cluster[i].name,
               g_cluster[i].ip,
               g_cluster[i].ssh_port,
               g_cluster[i].role);
    }
    printf("╚════════════════════════════════════════════════════════════╝\n\n");
}

// === 远程执行命令 ===
int remote_exec(const char *node_name, const char *cmd) {
    int node_idx = -1;
    
    // 查找节点
    for (int i = 0; i < g_node_count; i++) {
        if (strcmp(g_cluster[i].name, node_name) == 0) {
            node_idx = i;
            break;
        }
    }
    
    if (node_idx < 0) {
        printf("❌ 未找到节点: %s\n", node_name);
        return -1;
    }
    
    char ssh_cmd[MAX_CMD_LEN];
    snprintf(ssh_cmd, sizeof(ssh_cmd),
             "ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 "
             "-p %d root@%s '%s' 2>&1",
             g_cluster[node_idx].ssh_port,
             g_cluster[node_idx].ip,
             cmd);
    
    printf("[远程执行] %s -> %s\n", node_name, cmd);
    int ret = system(ssh_cmd);
    
    return ret;
}

// === 本地执行命令（封装） ===
int local_exec(const char *cmd) {
    printf("[本地执行] %s\n", cmd);
    return system(cmd);
}

// === 查找进程PID（通用） ===
int get_process_pid(const char *proc_name) {
    char cmd[256];
    char output[32];
    snprintf(cmd, sizeof(cmd), "pgrep -f '%s' | head -n 1", proc_name);
    
    FILE *fp = popen(cmd, "r");
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL) {
        pclose(fp);
        return atoi(output);
    }
    if (fp) pclose(fp);
    return -1;
}

// ===========================================
// 故障注入模块封装
// ===========================================

// === VM故障注入 ===
void inject_vm_fault(int fault_type) {
    char cmd[MAX_CMD_LEN];
    const char *target = "qemu";
    
    printf("\n=== 虚拟机故障注入 ===\n");
    
    switch (fault_type) {
        case 1: // 虚拟机崩溃
            snprintf(cmd, sizeof(cmd), "./process_injector %s 1", target);
            break;
        case 2: // 虚拟机挂起
            snprintf(cmd, sizeof(cmd), "./process_injector %s 2", target);
            break;
        case 3: // 虚拟机恢复
            snprintf(cmd, sizeof(cmd), "./process_injector %s 3", target);
            break;
        case 4: // 网络延迟
            printf("输入延迟值 (如 100ms): ");
            char delay[16];
            if (fgets(delay, sizeof(delay), stdin)) {
                delay[strcspn(delay, "\n")] = 0;
                snprintf(cmd, sizeof(cmd), "./network_injector 1 %s", delay);
            }
            break;
        case 5: // 网络丢包
            printf("输入丢包率 (如 10%%): ");
            char loss[16];
            if (fgets(loss, sizeof(loss), stdin)) {
                loss[strcspn(loss, "\n")] = 0;
                snprintf(cmd, sizeof(cmd), "./network_injector 2 %s", loss);
            }
            break;
        case 6: // 清理网络故障
            snprintf(cmd, sizeof(cmd), "./network_injector 0");
            break;
        default:
            printf("❌ 未知的故障类型\n");
            return;
    }
    
    local_exec(cmd);
}

// === Hadoop故障注入 ===
void inject_hadoop_fault(int fault_type) {
    char cmd[MAX_CMD_LEN];
    
    printf("\n=== Hadoop故障注入 ===\n");
    
    switch (fault_type) {
        case 1: // NameNode崩溃
            snprintf(cmd, sizeof(cmd), "./hadoop_injector crash nn");
            break;
        case 2: // NameNode挂起
            snprintf(cmd, sizeof(cmd), "./hadoop_injector hang nn");
            break;
        case 3: // NameNode恢复
            snprintf(cmd, sizeof(cmd), "./hadoop_injector resume nn");
            break;
        case 4: // DataNode崩溃
            snprintf(cmd, sizeof(cmd), "./hadoop_injector crash dn");
            break;
        case 5: // DataNode挂起
            snprintf(cmd, sizeof(cmd), "./hadoop_injector hang dn");
            break;
        case 6: // DataNode恢复
            snprintf(cmd, sizeof(cmd), "./hadoop_injector resume dn");
            break;
        case 7: // 进入安全模式
            snprintf(cmd, sizeof(cmd), "./hadoop_injector hdfs-safe enter");
            break;
        case 8: // 退出安全模式
            snprintf(cmd, sizeof(cmd), "./hadoop_injector hdfs-safe leave");
            break;
        case 9: // 节点网络隔离
            printf("输入要隔离的节点IP: ");
            char ip[32];
            if (fgets(ip, sizeof(ip), stdin)) {
                ip[strcspn(ip, "\n")] = 0;
                snprintf(cmd, sizeof(cmd), "./hadoop_injector network %s", ip);
            }
            break;
        case 10: // 清理网络隔离
            printf("输入要恢复的节点IP: ");
            char clear_ip[32];
            if (fgets(clear_ip, sizeof(clear_ip), stdin)) {
                clear_ip[strcspn(clear_ip, "\n")] = 0;
                snprintf(cmd, sizeof(cmd), "./hadoop_injector network-clear %s", clear_ip);
            }
            break;
        case 11: // 查看Hadoop进程状态
            snprintf(cmd, sizeof(cmd), "./hadoop_injector list");
            break;
        default:
            printf("❌ 未知的故障类型\n");
            return;
    }
    
    local_exec(cmd);
}

// === CloudStack故障注入 ===
void inject_cloudstack_fault(int fault_type) {
    char cmd[MAX_CMD_LEN];
    
    printf("\n=== CloudStack故障注入 ===\n");
    
    switch (fault_type) {
        case 1: // Management Server崩溃
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector crash ms");
            break;
        case 2: // Management Server挂起
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector hang ms");
            break;
        case 3: // Management Server恢复
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector resume ms");
            break;
        case 4: // Agent崩溃
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector crash agent");
            break;
        case 5: // Agent挂起
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector hang agent");
            break;
        case 6: // Agent恢复
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector resume agent");
            break;
        case 7: // API延迟
            printf("输入延迟值 (毫秒): ");
            char delay_ms[16];
            if (fgets(delay_ms, sizeof(delay_ms), stdin)) {
                delay_ms[strcspn(delay_ms, "\n")] = 0;
                snprintf(cmd, sizeof(cmd), "./cloudstack_injector api-delay %s", delay_ms);
            }
            break;
        case 8: // 清理API延迟
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector api-delay-clear");
            break;
        case 9: // 数据库连接限制
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector db-limit");
            break;
        case 10: // 恢复数据库连接
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector db-restore");
            break;
        case 11: // 查看CloudStack服务状态
            snprintf(cmd, sizeof(cmd), "./cloudstack_injector list");
            break;
        default:
            printf("❌ 未知的故障类型\n");
            return;
    }
    
    local_exec(cmd);
}

// === 预设故障场景 ===
void run_fault_scenario(int scenario) {
    printf("\n=== 执行预设故障场景 ===\n");
    
    switch (scenario) {
        case 1: // 场景1：单节点故障（DataNode宕机）
            printf("📋 场景: 单个DataNode节点宕机\n");
            printf("   预期: HDFS副本机制自动恢复\n");
            local_exec("./hadoop_injector crash dn");
            printf("\n⏳ 等待30秒后检查集群状态...\n");
            sleep(3);  // 演示用，实际可能需要更长时间
            local_exec("./hadoop_injector list");
            break;
            
        case 2: // 场景2：网络分区（节点间通信中断）
            printf("📋 场景: 网络分区 - 隔离一个Slave节点\n");
            printf("   预期: 被隔离节点被标记为不可用\n");
            if (g_node_count > 1) {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "./hadoop_injector network %s", g_cluster[1].ip);
                local_exec(cmd);
            }
            break;
            
        case 3: // 场景3：Master故障（NameNode宕机）
            printf("📋 场景: NameNode宕机\n");
            printf("   ⚠️  警告: 这将导致HDFS不可用!\n");
            printf("   按Enter继续或Ctrl+C取消...");
            getchar();
            local_exec("./hadoop_injector crash nn");
            break;
            
        case 4: // 场景4：级联故障（网络+进程）
            printf("📋 场景: 级联故障 - 先注入网络延迟，再注入进程挂起\n");
            local_exec("./network_injector 1 200ms");
            sleep(2);
            local_exec("./hadoop_injector hang dn");
            printf("\n🔄 3秒后自动恢复...\n");
            sleep(3);
            local_exec("./hadoop_injector resume dn");
            local_exec("./network_injector 0");
            break;
            
        case 5: // 场景5：资源耗尽
            printf("📋 场景: CPU资源耗尽\n");
            printf("输入持续时间 (秒): ");
            char duration[16];
            if (fgets(duration, sizeof(duration), stdin)) {
                duration[strcspn(duration, "\n")] = 0;
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "./cpu_injector 0 %s 4", duration);
                local_exec(cmd);
            }
            break;
            
        default:
            printf("❌ 未知的场景\n");
    }
}

// === 一键恢复所有故障 ===
void clear_all_faults() {
    printf("\n=== 一键恢复所有故障 ===\n");
    
    // 清理网络故障
    local_exec("./network_injector 0 2>/dev/null");
    
    // 清理iptables规则
    system("iptables -F INPUT 2>/dev/null");
    system("iptables -F OUTPUT 2>/dev/null");
    
    // 恢复挂起的Hadoop进程
    local_exec("./hadoop_injector resume nn 2>/dev/null");
    local_exec("./hadoop_injector resume dn 2>/dev/null");
    local_exec("./hadoop_injector resume rm 2>/dev/null");
    local_exec("./hadoop_injector resume nm 2>/dev/null");
    
    // 恢复CloudStack进程
    local_exec("./cloudstack_injector resume ms 2>/dev/null");
    local_exec("./cloudstack_injector resume agent 2>/dev/null");
    local_exec("./cloudstack_injector api-delay-clear 2>/dev/null");
    local_exec("./cloudstack_injector db-restore 2>/dev/null");
    
    // 清理磁盘填充文件
    system("rm -f /tmp/hdfs_disk_fill 2>/dev/null");
    
    printf("\n✅ 所有故障已尝试恢复\n");
}

// === 主菜单 ===
void show_main_menu() {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║          集群故障注入统一控制器 v1.0                          ║\n");
    printf("║          (VM / Hadoop / CloudStack)                           ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║  [1] 虚拟机故障注入      [2] Hadoop故障注入                   ║\n");
    printf("║  [3] CloudStack故障注入  [4] 预设故障场景                     ║\n");
    printf("║  [5] 查看集群状态        [6] 一键恢复所有                     ║\n");
    printf("║  [7] 加载集群配置        [q] 退出                             ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n");
    printf("👉 请选择: ");
}

void show_vm_menu() {
    printf("\n--- 虚拟机故障注入 ---\n");
    printf("[1] 虚拟机崩溃 (Crash)\n");
    printf("[2] 虚拟机挂起 (Hang)\n");
    printf("[3] 虚拟机恢复 (Resume)\n");
    printf("[4] 网络延迟 (Delay)\n");
    printf("[5] 网络丢包 (Loss)\n");
    printf("[6] 清理网络故障\n");
    printf("[0] 返回主菜单\n");
    printf("👉 请选择: ");
}

void show_hadoop_menu() {
    printf("\n--- Hadoop故障注入 ---\n");
    printf("[1] NameNode崩溃    [2] NameNode挂起    [3] NameNode恢复\n");
    printf("[4] DataNode崩溃    [5] DataNode挂起    [6] DataNode恢复\n");
    printf("[7] 进入安全模式    [8] 退出安全模式\n");
    printf("[9] 节点网络隔离    [10] 清理网络隔离\n");
    printf("[11] 查看Hadoop进程状态\n");
    printf("[0] 返回主菜单\n");
    printf("👉 请选择: ");
}

void show_cloudstack_menu() {
    printf("\n--- CloudStack故障注入 ---\n");
    printf("[1] MS崩溃    [2] MS挂起    [3] MS恢复\n");
    printf("[4] Agent崩溃 [5] Agent挂起 [6] Agent恢复\n");
    printf("[7] API延迟   [8] 清理API延迟\n");
    printf("[9] 数据库限制 [10] 恢复数据库\n");
    printf("[11] 查看CloudStack服务状态\n");
    printf("[0] 返回主菜单\n");
    printf("👉 请选择: ");
}

void show_scenario_menu() {
    printf("\n--- 预设故障场景 ---\n");
    printf("[1] 单节点故障 (DataNode宕机)\n");
    printf("[2] 网络分区 (隔离Slave节点)\n");
    printf("[3] Master故障 (NameNode宕机) ⚠️危险\n");
    printf("[4] 级联故障 (网络+进程)\n");
    printf("[5] 资源耗尽 (CPU)\n");
    printf("[0] 返回主菜单\n");
    printf("👉 请选择: ");
}

// === 主函数 ===
int main(int argc, char *argv[]) {
    char input[16];
    int choice;
    
    // 检查root权限
    if (geteuid() != 0) {
        printf("🔴 警告: 请使用 sudo 运行此程序以获得完整功能!\n");
    }
    
    // 初始化默认集群配置
    init_hadoop_cluster();
    
    // 检查依赖文件
    if (access("./process_injector", F_OK) != 0 ||
        access("./network_injector", F_OK) != 0) {
        printf("⚠️  警告: 未找到部分基础注入器，请先编译:\n");
        printf("   gcc -o process_injector process_injector.c\n");
        printf("   gcc -o network_injector network_injector.c\n");
    }
    
    if (access("./hadoop_injector", F_OK) != 0) {
        printf("⚠️  警告: 未找到hadoop_injector，请编译:\n");
        printf("   gcc -o hadoop_injector hadoop_injector.c\n");
    }
    
    if (access("./cloudstack_injector", F_OK) != 0) {
        printf("⚠️  警告: 未找到cloudstack_injector，请编译:\n");
        printf("   gcc -o cloudstack_injector cloudstack_injector.c\n");
    }
    
    while (1) {
        show_main_menu();
        if (fgets(input, sizeof(input), stdin) == NULL) break;
        input[strcspn(input, "\n")] = 0;
        
        if (strcmp(input, "q") == 0 || strcmp(input, "Q") == 0) {
            printf("👋 再见！\n");
            break;
        }
        
        choice = atoi(input);
        
        switch (choice) {
            case 1: // 虚拟机故障
                show_vm_menu();
                if (fgets(input, sizeof(input), stdin)) {
                    int vm_choice = atoi(input);
                    if (vm_choice > 0) inject_vm_fault(vm_choice);
                }
                break;
                
            case 2: // Hadoop故障
                show_hadoop_menu();
                if (fgets(input, sizeof(input), stdin)) {
                    int hadoop_choice = atoi(input);
                    if (hadoop_choice > 0) inject_hadoop_fault(hadoop_choice);
                }
                break;
                
            case 3: // CloudStack故障
                show_cloudstack_menu();
                if (fgets(input, sizeof(input), stdin)) {
                    int cs_choice = atoi(input);
                    if (cs_choice > 0) inject_cloudstack_fault(cs_choice);
                }
                break;
                
            case 4: // 预设场景
                show_scenario_menu();
                if (fgets(input, sizeof(input), stdin)) {
                    int scenario = atoi(input);
                    if (scenario > 0) run_fault_scenario(scenario);
                }
                break;
                
            case 5: // 查看集群状态
                show_cluster_status();
                // 同时显示各组件状态
                printf("检查Hadoop进程...\n");
                local_exec("./hadoop_injector list 2>/dev/null");
                printf("\n检查CloudStack服务...\n");
                local_exec("./cloudstack_injector list 2>/dev/null");
                break;
                
            case 6: // 一键恢复
                clear_all_faults();
                break;
                
            case 7: // 加载配置
                printf("输入配置文件路径 (默认: cluster.conf): ");
                char config_path[256] = "cluster.conf";
                if (fgets(input, sizeof(input), stdin)) {
                    input[strcspn(input, "\n")] = 0;
                    if (strlen(input) > 0) strcpy(config_path, input);
                }
                load_cluster_config(config_path);
                break;
                
            default:
                printf("❌ 无效的选项\n");
        }
    }
    
    return 0;
}
