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
#include <ctype.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

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

static const char *CLUSTER_VM_NAMES[] = {"master", "slave1", "slave2"};
#define CLUSTER_VM_COUNT (sizeof(CLUSTER_VM_NAMES) / sizeof(CLUSTER_VM_NAMES[0]))

static char g_tool_dir[PATH_MAX] = ".";

int normalize_status(int status)
{
    if (status == 0)
        return 0;
    if (status < 0)
        return 1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
}

void init_tool_dir(const char *argv0)
{
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0)
    {
        path[n] = '\0';
    }
    else if (argv0 && strchr(argv0, '/'))
    {
        if (argv0[0] == '/')
        {
            snprintf(path, sizeof(path), "%s", argv0);
        }
        else
        {
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd)))
                snprintf(path, sizeof(path), "%s/%s", cwd, argv0);
            else
                snprintf(path, sizeof(path), "%s", argv0);
        }
    }
    else
    {
        snprintf(g_tool_dir, sizeof(g_tool_dir), ".");
        return;
    }

    char *slash = strrchr(path, '/');
    if (!slash)
    {
        snprintf(g_tool_dir, sizeof(g_tool_dir), ".");
        return;
    }
    *slash = '\0';
    snprintf(g_tool_dir, sizeof(g_tool_dir), "%s", path[0] ? path : "/");
}

void build_tool_path(char *buf, size_t size, const char *tool)
{
    snprintf(buf, size, "%s/%s", g_tool_dir, tool);
}

int ensure_helper_tool(const char *tool, const char *source, const char *libs)
{
    char tool_path[PATH_MAX];
    char source_path[PATH_MAX];
    char cmd[PATH_MAX * 2 + 128];

    build_tool_path(tool_path, sizeof(tool_path), tool);
    if (access(tool_path, X_OK) == 0)
        return 0;

    build_tool_path(source_path, sizeof(source_path), source);
    if (access(source_path, F_OK) != 0)
    {
        printf("  [错误] 未找到 %s，且源码不存在: %s\n", tool, source_path);
        return -1;
    }

    printf("  未找到 %s，尝试在工具目录编译...\n", tool);
    snprintf(cmd, sizeof(cmd), "gcc -o '%s' '%s' %s 2>/dev/null", tool_path, source_path, libs ? libs : "");
    int ret = system(cmd);
    if (ret != 0 || access(tool_path, X_OK) != 0)
    {
        printf("  [错误] 编译 %s 失败，请在 %s 执行 make。\n", tool, g_tool_dir);
        return -1;
    }
    return 0;
}

int is_numeric_arg(const char *s)
{
    if (!s || !*s)
        return 0;
    for (const char *p = s; *p; ++p)
    {
        if (!isdigit((unsigned char)*p))
            return 0;
    }
    return 1;
}

void normalize_vm_name(char *name)
{
    if (!name || !*name)
        return;

    while (isspace((unsigned char)name[0]))
    {
        memmove(name, name + 1, strlen(name));
    }

    if (strncmp(name, "guest=", 6) == 0)
    {
        memmove(name, name + 6, strlen(name + 6) + 1);
    }

    size_t len = strlen(name);
    while (len > 0 && (isspace((unsigned char)name[len - 1]) || name[len - 1] == '"' || name[len - 1] == '\'' || name[len - 1] == ','))
    {
        name[--len] = '\0';
    }

    if (strncmp(name, "alpine_", 7) == 0 && strlen(name) > 7)
    {
        memmove(name, name + 7, strlen(name + 7) + 1);
    }
}

int extract_vm_name_from_args(const char *args, char *name, size_t size)
{
    if (!args || !name || size == 0)
        return -1;

    name[0] = '\0';

    const char *p = strstr(args, "-name");
    if (p)
    {
        p += 5;
        while (*p == ' ' || *p == '=')
            p++;

        char quote = 0;
        if (*p == '"' || *p == '\'')
        {
            quote = *p;
            p++;
        }

        size_t i = 0;
        while (*p && i + 1 < size)
        {
            if ((quote && *p == quote) || (!quote && isspace((unsigned char)*p)))
                break;
            name[i++] = *p++;
        }
        name[i] = '\0';
    }

    if (!name[0])
    {
        const char *drive = strstr(args, "images/node_");
        if (drive)
        {
            drive += strlen("images/node_");
            size_t i = 0;
            while (drive[i] && drive[i] != '.' && drive[i] != ' ' && i + 1 < size)
            {
                name[i] = drive[i];
                i++;
            }
            name[i] = '\0';
        }
    }

    normalize_vm_name(name);
    return name[0] ? 0 : -1;
}

int get_qemu_args(int pid, char *args, size_t size)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ps -p %d -o args= 2>/dev/null", pid);

    FILE *fp = popen(cmd, "r");
    if (!fp)
        return -1;

    int ok = -1;
    if (fgets(args, (int)size, fp))
    {
        args[strcspn(args, "\n")] = '\0';
        ok = 0;
    }
    pclose(fp);
    return ok;
}

int get_qemu_name(int pid, char *name, size_t size)
{
    char args[4096];
    if (get_qemu_args(pid, args, sizeof(args)) != 0)
        return -1;
    return extract_vm_name_from_args(args, name, size);
}

int vm_name_matches(const char *actual, const char *target)
{
    if (!actual || !*actual || !target || !*target)
        return 0;

    if (strcmp(actual, target) == 0)
        return 1;

    char expected[128];
    snprintf(expected, sizeof(expected), "alpine_%s", target);
    return strcmp(actual, expected) == 0;
}

// === 查找QEMU-KVM进程 ===
int* find_qemu_pids(int *count) {
    static int pids[100];
    *count = 0;
    
    char cmd[512];
    snprintf(
        cmd,
        sizeof(cmd),
        "ps -eo pid=,args= | awk '(/qemu-system/ || /qemu-kvm/) && (/accel=kvm/ || /-enable-kvm/) {print $1}'"
    );
    
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

int find_qemu_pid_by_name(const char *target)
{
    int count = 0;
    int *pids = find_qemu_pids(&count);
    for (int i = 0; i < count; i++)
    {
        char name[128] = "";
        if (get_qemu_name(pids[i], name, sizeof(name)) == 0 && vm_name_matches(name, target))
        {
            return pids[i];
        }
    }
    return -1;
}

int resolve_qemu_target_pid(const char *target)
{
    if (is_numeric_arg(target))
        return atoi(target);

    int pid = find_qemu_pid_by_name(target);
    if (pid > 0)
        return pid;

    printf(" [Error] 未找到目标虚拟机: %s\n", target);
    return -1;
}

// === 列出所有QEMU-KVM虚拟机 ===
void list_kvm_vms() {
    printf("\n╔══════════════════════════════════════════════════════════════╗\n");
    printf("║              当前KVM虚拟机进程状态                           ║\n");
    printf("╠══════════════════════════════════════════════════════════════╣\n");
    
    int running_count = 0;
    for (size_t i = 0; i < CLUSTER_VM_COUNT; i++) {
        int pid = find_qemu_pid_by_name(CLUSTER_VM_NAMES[i]);
        if (pid > 0) {
            running_count++;
            printf("║    VM: %-20s  PID: %-6d [RUNNING]     ║\n", CLUSTER_VM_NAMES[i], pid);
        } else {
            printf("║    VM: %-20s  PID: %-6s [STOPPED]     ║\n", CLUSTER_VM_NAMES[i], "-");
        }
    }
    printf("║   集群运行中: %d / %zu 个虚拟机                             ║\n", running_count, CLUSTER_VM_COUNT);
    
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
    char reg_tool[PATH_MAX];
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

    if (ensure_helper_tool("reg_injector", "reg_injector.c", "") != 0)
        return -1;

    build_tool_path(reg_tool, sizeof(reg_tool), "reg_injector");
    
    if (bit >= 0) {
        snprintf(cmd, sizeof(cmd), "'%s' %d %s %s %d", reg_tool, pid, target_reg, type_str, bit);
    } else {
        snprintf(cmd, sizeof(cmd), "'%s' %d %s %s", reg_tool, pid, target_reg, type_str);
    }
    
    return system(cmd);
}

// === 模块2：客户OS错误行为注入 ===
int hold_guest_observation_window(int pid, int duration_sec)
{
    if (duration_sec <= 0)
        return 0;

    char cmd[256];
    printf(" [观察窗口] 暂停 QEMU 进程 %d，持续 %d 秒，便于页面观察状态变化\n", pid, duration_sec);
    if (kill(pid, SIGSTOP) != 0)
    {
        perror("SIGSTOP failed");
        return -1;
    }

    snprintf(cmd, sizeof(cmd), "(sleep %d; kill -CONT %d 2>/dev/null) >/dev/null 2>&1 &", duration_sec, pid);
    int ret = system(cmd);
    if (ret != 0)
    {
        printf(" [警告] 自动恢复后台任务启动失败，请手动执行: kill -CONT %d\n", pid);
        return ret;
    }
    printf(" [观察窗口] 已安排 %d 秒后自动恢复: kill -CONT %d\n", duration_sec, pid);
    return 0;
}

int inject_guest_behavior_fault(int pid, int behavior_type, int duration_sec) {
    char cmd[512];
    char mem_tool[PATH_MAX];
    char reg_tool[PATH_MAX];
    
    printf(" [客户OS错误行为注入]\n");
    
    switch (behavior_type) {
        case 1: // 随机修改数据段
            printf("   类型: 随机修改进程数据段\n");
            if (ensure_helper_tool("mem_injector", "mem_injector.c", "") == 0) {
                build_tool_path(mem_tool, sizeof(mem_tool), "mem_injector");
                snprintf(cmd, sizeof(cmd), 
                         "'%s' -p %d -r heap -t byte -b 0", mem_tool, pid);
                int ret = system(cmd);
                if (normalize_status(ret) != 0)
                    return ret;
                return hold_guest_observation_window(pid, duration_sec);
            }
            break;
            
        case 2: // 触发除零异常
            printf("   类型: 模拟除零异常 (将 X0 整体置零)\n");
            if (ensure_helper_tool("reg_injector", "reg_injector.c", "") != 0)
                return -1;
            build_tool_path(reg_tool, sizeof(reg_tool), "reg_injector");
            snprintf(cmd, sizeof(cmd), "'%s' %d X0 zeroall", reg_tool, pid);
            return system(cmd);
            
        case 3: // 触发无效指令
            printf("   类型: 模拟无效操作异常\n");
            printf("     警告: 将 PC 置为无效地址，可能导致虚拟机崩溃，请使用 KVM 恢复动作重启节点。\n");
            if (ensure_helper_tool("reg_injector", "reg_injector.c", "") != 0)
                return -1;
            build_tool_path(reg_tool, sizeof(reg_tool), "reg_injector");
            snprintf(cmd, sizeof(cmd), "'%s' %d PC invalidpc", reg_tool, pid);
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
int inject_performance_fault(int pid, int delay_ms)
{
    char cmd[512];

    printf("  [性能故障注入]\n");
    printf("   目标PID: %d, 延迟: %dms\n", pid, delay_ms);

    if (delay_ms <= 0)
    {
        // 清理：移除CPU限制
        // 尝试 v1 恢复 (移回 tasks)
        snprintf(cmd, sizeof(cmd), "echo %d > /sys/fs/cgroup/cpu/tasks 2>/dev/null", pid);
        system(cmd);
        // 尝试 v2 恢复 (移回 cgroup.procs)
        snprintf(cmd, sizeof(cmd), "echo %d > /sys/fs/cgroup/cgroup.procs 2>/dev/null", pid);
        system(cmd);

        printf(" 已尝试清理性能限制\n");
        return 0;
    }

    // 计算CPU配额 (延迟越大，配额越少)
    // 默认周期为100ms，配额设为实际执行时间
    int quota = 100000 - (delay_ms * 1000); // 微秒
    if (quota < 10000)
        quota = 10000; // 最少10%
    int ret = -1;

    // --- 尝试 Cgroups v1 ---
    // 检查是否存在v1的cpu控制器路径
    if (access("/sys/fs/cgroup/cpu", F_OK) == 0)
    {
        system("mkdir -p /sys/fs/cgroup/cpu/qemu_throttle 2>/dev/null");
        snprintf(cmd, sizeof(cmd),
                 "echo 100000 > /sys/fs/cgroup/cpu/qemu_throttle/cpu.cfs_period_us 2>/dev/null && "
                 "echo %d > /sys/fs/cgroup/cpu/qemu_throttle/cpu.cfs_quota_us 2>/dev/null && "
                 "echo %d > /sys/fs/cgroup/cpu/qemu_throttle/tasks 2>/dev/null",
                 quota, pid);
        ret = system(cmd);
        if (ret == 0)
        {
            printf("   [Cgroups v1] 注入CPU限制 (配额: %d%%)\n", quota / 1000);
            printf("   效果: qemu-kvm执行速度下降，模拟ioctl延迟\n");
        }
    }

    // --- 尝试 Cgroups v2 ---
    // 如果v1失败，且存在v2特征文件
    if (ret != 0 && access("/sys/fs/cgroup/cgroup.controllers", F_OK) == 0)
    {
        system("mkdir -p /sys/fs/cgroup/qemu_throttle 2>/dev/null");

        // 确保父层级开启了cpu控制器(部分系统需要显式开启)
        system("echo '+cpu' > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null");

        // v2 使用 cpu.max: "QUOTA PERIOD"
        snprintf(cmd, sizeof(cmd),
                 "echo '%d 100000' > /sys/fs/cgroup/qemu_throttle/cpu.max 2>/dev/null && "
                 "echo %d > /sys/fs/cgroup/qemu_throttle/cgroup.procs 2>/dev/null",
                 quota, pid);
        ret = system(cmd);
        if (ret == 0)
        {
            printf("   [Cgroups v2] 注入CPU限制 (配额: %d%%)\n", quota / 1000);
            printf("   效果: qemu-kvm执行速度下降，模拟ioctl延迟\n");
        }
    }

    // --- 备选方案：cpulimit ---
    if (ret != 0)
    {
        // 方法2：使用cpulimit工具
        printf("   cgroups方法失败，尝试cpulimit...\n");
        int cpu_percent = 100 - (delay_ms / 10);
        if (cpu_percent < 10)
            cpu_percent = 10;

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
    char cpu_tool[PATH_MAX];

    printf(" [CPU高负载注入]\n");
    // 如果 threads 为 0，说明用户没指定，交给 cpu_injector 自动判断
    if (threads <= 0)
        printf("   目标PID: %d (伴随压力), 持续: %d秒, 线程: 自动(全核)\n", pid, duration);
    else
        printf("   目标PID: %d (伴随压力), 持续: %d秒, 线程: %d\n", pid, duration, threads);

    if (ensure_helper_tool("cpu_injector", "cpu_injector.c", "-lpthread -lm") != 0)
        return -1;

    build_tool_path(cpu_tool, sizeof(cpu_tool), "cpu_injector");

    // 2. 构造调用命令
    // 对应 cpu_injector 的参数: <PID> <Duration> [Threads]
    if (threads > 0)
    {
        snprintf(cmd, sizeof(cmd), "'%s' %d %d %d", cpu_tool, pid, duration, threads);
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "'%s' %d %d", cpu_tool, pid, duration);
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
    system("rmdir /sys/fs/cgroup/cpu/qemu_throttle 2>/dev/null"); // v1
    system("rmdir /sys/fs/cgroup/qemu_throttle 2>/dev/null");     // v2

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
    printf("  soft-flip <目标> <寄存器> [位]  位翻转故障\n");
    printf("  soft-swap <目标> <寄存器>       两位交换故障\n");
    printf("  soft-zero <目标> <寄存器> [位]  位置零覆盖\n\n");
    
    printf("【客户OS错误行为】\n");
    printf("  guest-data <目标> [秒]          随机修改数据段，并暂停观察窗口\n");
    printf("  guest-divzero <目标>            将 X0 置零，模拟除零前置条件\n");
    printf("  guest-invalid <目标>            将 PC 置为无效地址\n\n");
    
    printf("【性能故障】\n");
    printf("  perf-delay <目标> <毫秒>        注入执行延迟\n");
    printf("  perf-stress <目标> <秒> [线程]  注入CPU高负载 (资源争抢)\n");
    printf("  perf-clear <目标>               清理性能限制\n\n");
    
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
    printf("  %s soft-flip master PC 10      # 对 master 翻转PC第10位\n", prog);
    printf("  %s perf-delay slave1 50        # 对 slave1 注入50ms延迟\n", prog);
    printf("  %s cpu-offline 2               # 下线CPU2\n", prog);
    printf("\n");
}

// === 主函数 ===
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    init_tool_dir(argv[0]);
    
    // 检查root权限
    if (geteuid() != 0) {
        printf("  警告: 大部分功能需要root权限\n");
    }
    
    const char *command = argv[1];
    
    // 命令解析
    if (strcmp(command, "list") == 0) {
        list_kvm_vms();
        return 0;
    }
    // 软错误
    else if (strcmp(command, "soft-flip") == 0) {
        if (argc < 4) {
            printf(" 用法: %s soft-flip <目标> <寄存器> [位]\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        int bit = (argc >= 5) ? atoi(argv[4]) : -1;
        return normalize_status(inject_soft_error(pid, SOFT_ERROR_BIT_FLIP, argv[3], bit));
    }
    else if (strcmp(command, "soft-swap") == 0) {
        if (argc < 4) {
            printf(" 用法: %s soft-swap <目标> <寄存器>\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        return normalize_status(inject_soft_error(pid, SOFT_ERROR_SWAP, argv[3], -1));
    }
    else if (strcmp(command, "soft-zero") == 0) {
        if (argc < 4) {
            printf(" 用法: %s soft-zero <目标> <寄存器> [位]\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        int bit = (argc >= 5) ? atoi(argv[4]) : -1;
        return normalize_status(inject_soft_error(pid, SOFT_ERROR_OVERWRITE, argv[3], bit));
    }
    // 客户OS错误行为
    else if (strcmp(command, "guest-data") == 0) {
        if (argc < 3) {
            printf(" 用法: %s guest-data <目标> [观察秒数]\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        int duration = (argc >= 4) ? atoi(argv[3]) : 15;
        return normalize_status(inject_guest_behavior_fault(pid, 1, duration));
    }
    else if (strcmp(command, "guest-divzero") == 0) {
        if (argc < 3) {
            printf(" 用法: %s guest-divzero <目标>\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        return normalize_status(inject_guest_behavior_fault(pid, 2, 0));
    }
    else if (strcmp(command, "guest-invalid") == 0) {
        if (argc < 3) {
            printf(" 用法: %s guest-invalid <目标>\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        return normalize_status(inject_guest_behavior_fault(pid, 3, 0));
    }
    // 性能故障
    else if (strcmp(command, "perf-delay") == 0) {
        if (argc < 4) {
            printf(" 用法: %s perf-delay <目标> <毫秒>\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        return normalize_status(inject_performance_fault(pid, atoi(argv[3])));
    }
    else if (strcmp(command, "perf-clear") == 0) {
        if (argc < 3) {
            printf(" 用法: %s perf-clear <目标>\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        return normalize_status(inject_performance_fault(pid, 0));
    }
    else if (strcmp(command, "perf-stress") == 0)
    {
        if (argc < 4)
        {
            printf(" 用法: %s perf-stress <目标> <持续秒数> [线程数]\n", argv[0]);
            return 1;
        }
        int pid = resolve_qemu_target_pid(argv[2]);
        if (pid <= 0)
            return 1;
        int duration = atoi(argv[3]);
        int threads = (argc >= 5) ? atoi(argv[4]) : 0; // 0 表示默认
        return normalize_status(inject_cpu_stress(pid, duration, threads));
    }
    // CPU热插拔
    else if (strcmp(command, "cpu-offline") == 0) {
        if (argc < 3) {
            printf(" 用法: %s cpu-offline <CPU号>\n", argv[0]);
            return 1;
        }
        return normalize_status(inject_cpu_hotplug_fault(atoi(argv[2]), 0));
    }
    else if (strcmp(command, "cpu-online") == 0) {
        if (argc < 3) {
            printf(" 用法: %s cpu-online <CPU号>\n", argv[0]);
            return 1;
        }
        return normalize_status(inject_cpu_hotplug_fault(atoi(argv[2]), 1));
    }
    // 清理
    else if (strcmp(command, "clear") == 0) {
        clear_all_faults();
        return 0;
    }
    else if (strcmp(command, "-h") == 0 || strcmp(command, "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    else {
        printf(" 未知命令: %s\n", command);
        print_usage(argv[0]);
        return 1;
    }
    
    return 0;
}
