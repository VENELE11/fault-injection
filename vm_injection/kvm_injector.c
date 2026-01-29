/*
 * kvm_injector.c - KVM虚拟化层故障注入工具 (增强版)
 * 
 * 功能：针对KVM虚拟化层进行多种故障注入
 * 支持：
 *   - 软错误注入：寄存器位翻转、交换、覆盖
 *   - 客户OS错误行为：随机修改进程状态
 *   - 性能故障：qemu-kvm ioctl延迟
 *   - 维护故障：CPU热插拔
 * 
 * 编译：gcc -o kvm_injector kvm_injector.c -lpthread
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

// === 故障类型枚举 ===
typedef enum {
    KVM_FAULT_SOFT_ERROR = 1,      // 软错误
    KVM_FAULT_GUEST_BEHAVIOR = 2,  // 客户OS错误行为
    KVM_FAULT_PERFORMANCE = 3,     // 性能故障
    KVM_FAULT_MAINTENANCE = 4      // 维护故障
} KVMFaultType;

// === 软错误类型 ===
typedef enum {
    SOFT_ERROR_BIT_FLIP = 1,       // 一位或多位翻转
    SOFT_ERROR_SWAP = 2,           // 两位交换
    SOFT_ERROR_OVERWRITE = 3,      // 覆盖特定值
    SOFT_ERROR_NOP = 4             // NOP指令注入
} SoftErrorType;

// === 查找QEMU-KVM进程 ===
int* find_qemu_pids(int *count) {
    static int pids[100];
    *count = 0;
    
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "pgrep -f 'qemu.*-enable-kvm' 2>/dev/null");
    
    FILE *fp = popen(cmd, "r");
    if (fp) {
        char line[32];
        while (fgets(line, sizeof(line), fp) && *count < 100) {
            int pid = atoi(line);
            if (pid > 0) {
                pids[(*count)++] = pid;
            }
        }
        pclose(fp);
    }
    
    return pids;
}

// === 列出所有QEMU-KVM虚拟机 ===
void list_kvm_vms() {
    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║              当前KVM虚拟机进程状态                           ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    int count = 0;
    int *pids = find_qemu_pids(&count);
    
    if (count == 0) {
        printf("║     未发现运行中的KVM虚拟机                               ║\n");
    } else {
        for (int i = 0; i < count; i++) {
            char cmd[256];
            char name[128] = "unknown";
            
            // 获取虚拟机名称
            snprintf(cmd, sizeof(cmd), 
                     "ps -p %d -o args= 2>/dev/null | grep -oP '(?<=-name )[^ ]+' | head -1",
                     pids[i]);
            
            FILE *fp = popen(cmd, "r");
            if (fp && fgets(name, sizeof(name), fp)) {
                name[strcspn(name, "\n")] = 0;
            }
            if (fp) pclose(fp);
            
            printf("║    VM: %-20s  PID: %-6d               ║\n", name, pids[i]);
        }
        printf("║   总计: %d 个虚拟机正在运行                                 ║\n", count);
    }
    
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    // 显示CPU热插拔状态
    printf("║ [CPU热插拔状态]                                              ║\n");
    
    // 检查CPU在线状态
    int online_cpus = 0;
    int total_cpus = sysconf(_SC_NPROCESSORS_CONF);
    
    for (int i = 0; i < total_cpus; i++) {
        char path[128];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", i);
        
        FILE *f = fopen(path, "r");
        if (f) {
            int status;
            if (fscanf(f, "%d", &status) == 1 && status == 1) {
                online_cpus++;
            }
            fclose(f);
        } else if (i == 0) {
            // CPU0通常不可下线
            online_cpus++;
        }
    }
    
    printf("║   在线CPU: %d / %d                                            ║\n", 
           online_cpus, total_cpus);
    
    printf("╚══════════════════════════════════════════════════════════════╝\n\n");
}

// === 模块1：软错误注入 ===
// 通过外部调用reg_injector实现
int inject_soft_error(int pid, SoftErrorType error_type, const char *target_reg, int bit) {
    char cmd[512];
    const char *type_str;
    
    switch (error_type) {
        case SOFT_ERROR_BIT_FLIP:
            type_str = "flip1";
            break;
        case SOFT_ERROR_SWAP:
            type_str = "flip2";  // 两位翻转模拟交换效果
            break;
        case SOFT_ERROR_OVERWRITE:
            type_str = "zero1";  // 覆盖为0
            break;
        case SOFT_ERROR_NOP:
            printf("  NOP注入需要内存注入器支持\n");
            return -1;
        default:
            printf(" 未知的软错误类型\n");
            return -1;
    }
    
    printf(" [软错误注入]\n");
    printf("   目标PID: %d, 寄存器: %s, 类型: %s\n", pid, target_reg, type_str);
    
    if (access("./reg_injector", F_OK) != 0) {
        printf("  未找到reg_injector，尝试编译...\n");
        system("gcc -o reg_injector reg_injector.c 2>/dev/null");
    }
    
    if (bit >= 0) {
        snprintf(cmd, sizeof(cmd), "./reg_injector %d %s %s %d", pid, target_reg, type_str, bit);
    } else {
        snprintf(cmd, sizeof(cmd), "./reg_injector %d %s %s", pid, target_reg, type_str);
    }
    
    return system(cmd);
}

// === 模块2：客户OS错误行为注入 ===
int inject_guest_behavior_fault(int pid, int behavior_type) {
    char cmd[512];
    
    printf(" [客户OS错误行为注入]\n");
    
    switch (behavior_type) {
        case 1: // 随机修改数据段
            printf("   类型: 随机修改进程数据段\n");
            if (access("./mem_injector", F_OK) == 0) {
                snprintf(cmd, sizeof(cmd), 
                         "./mem_injector -p %d -r heap -t byte -b 0", pid);
                return system(cmd);
            }
            break;
            
        case 2: // 触发除零异常
            printf("   类型: 模拟除零异常 (通过修改寄存器)\n");
            snprintf(cmd, sizeof(cmd), "./reg_injector %d X0 zero1 0", pid);
            return system(cmd);
            
        case 3: // 触发无效指令
            printf("   类型: 模拟无效操作异常\n");
            // 通过修改PC寄存器导致执行无效指令
            printf("     警告: 这可能导致客户OS崩溃!\n");
            snprintf(cmd, sizeof(cmd), "./reg_injector %d PC add1", pid);
            return system(cmd);
            
        default:
            printf(" 未知的错误行为类型\n");
            return -1;
    }
    
    printf("  需要相应的注入器工具\n");
    return -1;
}

// === 模块3：性能故障注入 ===
// 通过cgroups限制CPU来间接实现延迟效果
int inject_performance_fault(int pid, int delay_ms) {
    char cmd[512];
    
    printf("  [性能故障注入]\n");
    printf("   目标PID: %d, 延迟: %dms\n", pid, delay_ms);
    
    if (delay_ms <= 0) {
        // 清理：移除CPU限制
        snprintf(cmd, sizeof(cmd),
                 "echo %d > /sys/fs/cgroup/cpu/tasks 2>/dev/null", pid);
        system(cmd);
        printf(" 已清理性能限制\n");
        return 0;
    }
    
    // 方法1：通过cgroups限制CPU配额
    // 创建cgroup
    system("mkdir -p /sys/fs/cgroup/cpu/qemu_throttle 2>/dev/null");
    
    // 计算CPU配额 (延迟越大，配额越少)
    // 默认周期为100ms，配额设为实际执行时间
    int quota = 100000 - (delay_ms * 1000);  // 微秒
    if (quota < 10000) quota = 10000;  // 最少10%
    
    snprintf(cmd, sizeof(cmd),
             "echo 100000 > /sys/fs/cgroup/cpu/qemu_throttle/cpu.cfs_period_us 2>/dev/null && "
             "echo %d > /sys/fs/cgroup/cpu/qemu_throttle/cpu.cfs_quota_us 2>/dev/null && "
             "echo %d > /sys/fs/cgroup/cpu/qemu_throttle/tasks 2>/dev/null",
             quota, pid);
    
    int ret = system(cmd);
    
    if (ret == 0) {
        printf("   通过cgroups注入CPU限制 (配额: %d%%)\n", quota / 1000);
        printf("   效果: qemu-kvm执行速度下降，模拟ioctl延迟\n");
    } else {
        // 方法2：使用cpulimit工具
        printf("   cgroups方法失败，尝试cpulimit...\n");
        int cpu_percent = 100 - (delay_ms / 10);
        if (cpu_percent < 10) cpu_percent = 10;
        
        snprintf(cmd, sizeof(cmd),
                 "cpulimit -p %d -l %d -b 2>/dev/null &", pid, cpu_percent);
        system(cmd);
        printf("   通过cpulimit限制CPU使用率为 %d%%\n", cpu_percent);
    }
    
    return 0;
}
// === 模块：CPU 高负载注入 (基于资源争抢) ===
int inject_cpu_stress(int pid, int duration, int threads)
{
    char cmd[512];

    printf(" [CPU高负载注入]\n");
    // 如果 threads 为 0，说明用户没指定，交给 cpu_injector 自动判断
    if (threads <= 0)
        printf("   目标PID: %d (伴随压力), 持续: %d秒, 线程: 自动(全核)\n", pid, duration);
    else
        printf("   目标PID: %d (伴随压力), 持续: %d秒, 线程: %d\n", pid, duration, threads);

    // 1. 自动检查并编译 cpu_injector
    if (access("./cpu_injector", F_OK) != 0)
    {
        printf("  未找到 cpu_injector，尝试自动编译...\n");
        int ret = system("gcc -o cpu_injector cpu_injector.c -lpthread -lm 2>/dev/null");
        if (ret != 0)
        {
            printf("  [错误] 编译失败！请确认 cpu_injector.c 存在且已安装 gcc。\n");
            return -1;
        }
    }

    // 2. 构造调用命令
    // 对应 cpu_injector 的参数: <PID> <Duration> [Threads]
    if (threads > 0)
    {
        snprintf(cmd, sizeof(cmd), "./cpu_injector %d %d %d", pid, duration, threads);
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "./cpu_injector %d %d", pid, duration);
    }

    // 3. 执行
    return system(cmd);
}
// === 模块4：CPU热插拔维护故障 ===
int inject_cpu_hotplug_fault(int cpu_id, int online) {
    char path[128];
    char cmd[256];
    
    printf(" [CPU热插拔故障]\n");
    
    // CPU0通常不能下线
    if (cpu_id == 0 && !online) {
        printf("  CPU0通常不能下线，尝试CPU1\n");
        cpu_id = 1;
    }
    
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online", cpu_id);
    
    // 检查文件是否存在
    if (access(path, F_OK) != 0) {
        printf(" CPU%d 不支持热插拔或不存在\n", cpu_id);
        return -1;
    }
    
    if (online) {
        snprintf(cmd, sizeof(cmd), "echo 1 > %s", path);
        printf("   操作: 上线 CPU%d\n", cpu_id);
    } else {
        snprintf(cmd, sizeof(cmd), "echo 0 > %s", path);
        printf("   操作: 下线 CPU%d\n", cpu_id);
        printf("   预期: 该CPU上的虚拟机vCPU线程将迁移\n");
    }
    
    int ret = system(cmd);
    
    if (ret == 0) {
        printf(" CPU%d 已%s\n", cpu_id, online ? "上线" : "下线");
    } else {
        printf(" 操作失败 (可能需要root权限或内核不支持)\n");
    }
    
    return ret;
}

// === 清理所有注入的故障 ===
void clear_all_faults() {
    printf("\n [清理所有KVM故障]\n");
    
    // 清理cgroups限制
    system("rmdir /sys/fs/cgroup/cpu/qemu_throttle 2>/dev/null");
    
    // 恢复所有CPU
    int total_cpus = sysconf(_SC_NPROCESSORS_CONF);
    for (int i = 1; i < total_cpus; i++) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), 
                 "echo 1 > /sys/devices/system/cpu/cpu%d/online 2>/dev/null", i);
        system(cmd);
    }
    
    // 停止cpulimit
    system("pkill cpulimit 2>/dev/null");
    
    printf(" 故障清理完成\n");
}

// === 打印帮助 ===
void print_usage(const char *prog) {
    printf("\n╔═══════════════════════════════════════════════════════════════════╗\n");
    printf("║         KVM虚拟化层故障注入工具 v2.0                              ║\n");
    printf("║                     ║\n");
    printf("╚═══════════════════════════════════════════════════════════════════╝\n\n");
    printf("用法: %s <命令> [参数]\n\n", prog);
    
    printf("【虚拟机管理】\n");
    printf("  list                          列出所有KVM虚拟机状态\n\n");
    
    printf("【软错误注入】\n");
    printf("  soft-flip <PID> <寄存器> [位]  位翻转故障\n");
    printf("  soft-swap <PID> <寄存器>       两位交换故障\n");
    printf("  soft-zero <PID> <寄存器> [位]  位置零覆盖\n\n");
    
    printf("【客户OS错误行为】\n");
    printf("  guest-data <PID>               随机修改数据段\n");
    printf("  guest-divzero <PID>            模拟除零异常\n");
    printf("  guest-invalid <PID>            模拟无效指令\n\n");
    
    printf("【性能故障】\n");
    printf("  perf-delay <PID> <毫秒>        注入执行延迟\n");
    printf("  perf-stress <PID> <秒> [线程]  注入CPU高负载 (资源争抢)\n");
    printf("  perf-clear <PID>               清理性能限制\n\n");
    
    printf("【维护故障】\n");
    printf("  cpu-offline <CPU号>            下线指定CPU\n");
    printf("  cpu-online <CPU号>             上线指定CPU\n\n");
    
    printf("【其他】\n");
    printf("  clear                          清理所有故障\n\n");
    
    printf("【寄存器】\n");
    printf("  ARM64: PC, SP, X0-X30\n");
    printf("  x86_64: RIP, RSP, RAX, RBX, RCX, RDX, etc.\n\n");
    
    printf("【示例】\n");
    printf("  %s list                        # 查看虚拟机\n", prog);
    printf("  %s soft-flip 1234 PC 10        # 翻转PC第10位\n", prog);
    printf("  %s perf-delay 1234 50          # 注入50ms延迟\n", prog);
    printf("  %s cpu-offline 2               # 下线CPU2\n", prog);
    printf("\n");
}

// === 主函数 ===
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    // 检查root权限
    if (geteuid() != 0) {
        printf("  警告: 大部分功能需要root权限\n");
    }
    
    const char *command = argv[1];
    
    // 命令解析
    if (strcmp(command, "list") == 0) {
        list_kvm_vms();
    }
    // 软错误
    else if (strcmp(command, "soft-flip") == 0) {
        if (argc < 4) {
            printf(" 用法: %s soft-flip <PID> <寄存器> [位]\n", argv[0]);
            return 1;
        }
        int pid = atoi(argv[2]);
        int bit = (argc >= 5) ? atoi(argv[4]) : -1;
        inject_soft_error(pid, SOFT_ERROR_BIT_FLIP, argv[3], bit);
    }
    else if (strcmp(command, "soft-swap") == 0) {
        if (argc < 4) {
            printf(" 用法: %s soft-swap <PID> <寄存器>\n", argv[0]);
            return 1;
        }
        int pid = atoi(argv[2]);
        inject_soft_error(pid, SOFT_ERROR_SWAP, argv[3], -1);
    }
    else if (strcmp(command, "soft-zero") == 0) {
        if (argc < 4) {
            printf(" 用法: %s soft-zero <PID> <寄存器> [位]\n", argv[0]);
            return 1;
        }
        int pid = atoi(argv[2]);
        int bit = (argc >= 5) ? atoi(argv[4]) : -1;
        inject_soft_error(pid, SOFT_ERROR_OVERWRITE, argv[3], bit);
    }
    // 客户OS错误行为
    else if (strcmp(command, "guest-data") == 0) {
        if (argc < 3) {
            printf(" 用法: %s guest-data <PID>\n", argv[0]);
            return 1;
        }
        inject_guest_behavior_fault(atoi(argv[2]), 1);
    }
    else if (strcmp(command, "guest-divzero") == 0) {
        if (argc < 3) {
            printf(" 用法: %s guest-divzero <PID>\n", argv[0]);
            return 1;
        }
        inject_guest_behavior_fault(atoi(argv[2]), 2);
    }
    else if (strcmp(command, "guest-invalid") == 0) {
        if (argc < 3) {
            printf(" 用法: %s guest-invalid <PID>\n", argv[0]);
            return 1;
        }
        inject_guest_behavior_fault(atoi(argv[2]), 3);
    }
    // 性能故障
    else if (strcmp(command, "perf-delay") == 0) {
        if (argc < 4) {
            printf(" 用法: %s perf-delay <PID> <毫秒>\n", argv[0]);
            return 1;
        }
        inject_performance_fault(atoi(argv[2]), atoi(argv[3]));
    }
    else if (strcmp(command, "perf-clear") == 0) {
        if (argc < 3) {
            printf(" 用法: %s perf-clear <PID>\n", argv[0]);
            return 1;
        }
        inject_performance_fault(atoi(argv[2]), 0);
    }
    else if (strcmp(command, "perf-stress") == 0)
    {
        if (argc < 4)
        {
            printf(" 用法: %s perf-stress <PID> <持续秒数> [线程数]\n", argv[0]);
            return 1;
        }
        int pid = atoi(argv[2]);
        int duration = atoi(argv[3]);
        int threads = (argc >= 5) ? atoi(argv[4]) : 0; // 0 表示默认
        inject_cpu_stress(pid, duration, threads);
    }
    // CPU热插拔
    else if (strcmp(command, "cpu-offline") == 0) {
        if (argc < 3) {
            printf(" 用法: %s cpu-offline <CPU号>\n", argv[0]);
            return 1;
        }
        inject_cpu_hotplug_fault(atoi(argv[2]), 0);
    }
    else if (strcmp(command, "cpu-online") == 0) {
        if (argc < 3) {
            printf(" 用法: %s cpu-online <CPU号>\n", argv[0]);
            return 1;
        }
        inject_cpu_hotplug_fault(atoi(argv[2]), 1);
    }
    // 清理
    else if (strcmp(command, "clear") == 0) {
        clear_all_faults();
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
