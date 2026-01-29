/*
 * fault_controller.c - 虚拟机故障注入控制器
 * 功能：集成 Process, Network, Memory, Register, CPU, MemLeak 故障注入
 * 目标：动态可配置的目标进程 (默认: target)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

// 全局目标变量
char target_process[64] = "target";

// 查找目标进程 PID
int get_vm_pid(const char *proc_name)
{
    char cmd[128];
    char output[32];
    // 使用 pgreg -f 查找包含名字的进程，排除 grep 自身和 controller 自身
    // head -n 1 取第一个匹配的
    // 使用 ^[^ ]* 来匹配开头不为空格的，或者直接简单匹配
    snprintf(cmd, sizeof(cmd), "pgrep -f '^[^ ]*%s' | head -n 1", proc_name);

    FILE *fp = popen(cmd, "r");
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL)
    {
        pclose(fp);
        return atoi(output);
    }
    if (fp)
        pclose(fp);
    return -1;
}

// === 模块 1: 进程故障注入 (Wrapper) ===
void inject_process_wrapper(const char *target, int action_type)
{
    char cmd[256];
    printf(" [Process] 对 %s 执行动作 %d\n", target, action_type);
    snprintf(cmd, sizeof(cmd), "./process_injector %s %d", target, action_type);
    system(cmd);
}

// === 模块 2: 网络故障注入 (Wrapper) ===
void inject_network_wrapper(int type, const char *param)
{
    char cmd[256];
    if (param)
    {
        snprintf(cmd, sizeof(cmd), "./network_injector %d %s", type, param);
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "./network_injector %d", type);
    }
    system(cmd);
}

// === 模块 3: 内存故障注入 (Wrapper) ===
void inject_memory_wrapper(const char *target)
{
    int pid = get_vm_pid(target);
    if (pid == -1)
    {
        printf(" [错误] 未找到进程: %s (需先启动目标程序)\n", target);
        return;
    }

    printf("\n--- 内存故障配置 (PID: %d) ---\n", pid);
    printf("1. 盲注 (Blind Injection - Heap/Stack)\n");
    printf("2. 扫描特征值注入 (Scan & Inject - 自动定位 0xDEADBEEF...)\n");
    printf(" 选择模式: ");

    char input[10];
    if (!fgets(input, sizeof(input), stdin))
        return;
    int mode = atoi(input);

    char region[10] = "heap";
    char fault_type[10] = "flip";
    char signature[32] = "deadbeefcafebabe"; // 默认匹配 target.c の CANARY
    int bit = 0;

    // 选择区域
    if (mode == 1)
    {
        printf("区域 [heap/stack]: ");
        if (fgets(region, sizeof(region), stdin))
            region[strcspn(region, "\n")] = 0;
        if (strlen(region) == 0)
            strcpy(region, "heap");
    }
    // 特征值
    else if (mode == 2)
    {
        printf("输入16进制特征值 (默认 deadbeefcafebabe): ");
        char sig_input[64];
        if (fgets(sig_input, sizeof(sig_input), stdin) && sig_input[0] != '\n')
        {
            sig_input[strcspn(sig_input, "\n")] = 0;
            strncpy(signature, sig_input, sizeof(signature) - 1);
        }

        printf("搜索区域 [heap/stack]: ");
        if (fgets(region, sizeof(region), stdin))
            region[strcspn(region, "\n")] = 0;
        if (strlen(region) == 0)
            strcpy(region, "heap");
    }
    else
    {
        printf("无效模式\n");
        return;
    }

    printf("故障类型 [flip/set0/set1/byte]: ");
    if (fgets(fault_type, sizeof(fault_type), stdin))
        fault_type[strcspn(fault_type, "\n")] = 0;
    if (strlen(fault_type) == 0)
        strcpy(fault_type, "flip");

    printf("目标位 (0-63): ");
    char bit_mk[10];
    if (fgets(bit_mk, sizeof(bit_mk), stdin))
        bit = atoi(bit_mk);

    char cmd[512];
    if (mode == 1)
        snprintf(cmd, sizeof(cmd), "./mem_injector -p %d -r %s -t %s -b %d", pid, region, fault_type, bit);
    else
        snprintf(cmd, sizeof(cmd), "./mem_injector -p %d -r %s -s %s -t %s -b %d", pid, region, signature, fault_type, bit);

    printf("Executing: %s\n", cmd);
    system(cmd);
}

// === 模块 4: 寄存器故障注入 (Wrapper) ===
void inject_register_wrapper(const char *target)
{
    int pid = get_vm_pid(target);
    if (pid == -1)
    {
        printf(" [错误] 未找到进程: %s\n", target);
        return;
    }

    printf("\n--- ARM64 寄存器注入 (PID: %d) ---\n", pid);
    printf("常用寄存器: PC (崩溃), SP (栈错), X0-X30 (数据)\n");
    printf("输入目标寄存器 [PC/SP/X0]: ");

    char reg[10];
    scanf("%s", reg);
    getchar(); // 吃掉换行

    printf("故障类型 [flip1/flip2/zero1/add1...]: ");
    char type[10];
    scanf("%s", type);
    getchar();

    printf("目标位 (输入 -1 为随机): ");
    int bit = -1;
    scanf("%d", &bit);
    getchar();

    printf("是否启用时间延迟? (y/n): ");
    char use_time[10];
    scanf("%s", use_time);
    getchar();

    char cmd[512];
    if (use_time[0] == 'y')
    {
        int delay_us;
        printf("输入延迟 (微秒, 1秒=1000000): ");
        scanf("%d", &delay_us);
        getchar();
        snprintf(cmd, sizeof(cmd), "./reg_injector %d %s %s %d -w %d", pid, reg, type, bit, delay_us);
    }
    else
    {
        snprintf(cmd, sizeof(cmd), "./reg_injector %d %s %s %d", pid, reg, type, bit);
    }

    printf("执行: %s\n", cmd);
    system(cmd);
}

// === 模块 5: CPU 资源耗尽注入 (Wrapper) ===
void inject_cpu_wrapper(const char *target)
{
    int pid = get_vm_pid(target);
    if (pid == -1)
    {
        printf(" [WARN] 未找到目标进程 %s (将进行无目标全系统施压)\n", target);
        pid = 0;
    }

    printf("\n--- CPU 高负载注入 ---\n");
    printf("原理: 创建竞争线程，争抢宿主机 CPU 时间片\n");

    int duration = 10;
    int threads = 4;

    printf("持续时间 (秒): ");
    scanf("%d", &duration);
    getchar();

    printf("压力线程数 (建议 = 宿主机核心数, 默认4): ");
    char t_str[10];
    fgets(t_str, sizeof(t_str), stdin);
    if (t_str[0] != '\n')
        threads = atoi(t_str);
    if (threads <= 0)
        threads = 4;

    char cmd[512];
    // 检查 cpu_injector 是否存在
    if (access("./cpu_injector", F_OK) != 0)
    {
        printf(" [Info] 自动编译 cpu_injector...\n");
        system("gcc -o cpu_injector cpu_injector.c -lpthread -lm");
    }

    snprintf(cmd, sizeof(cmd), "./cpu_injector %d %d %d", pid, duration, threads);
    printf("执行: %s\n", cmd);
    system(cmd);
}

// === 模块 6: 内存泄漏注入 (Wrapper) ===
void inject_mem_leak_wrapper(const char *target)
{
    printf("\n--- 内存泄漏注入 (系统级 OOM 测试) ---\n");
    printf("原理: 注入器大量占用宿主机 RAM，迫使系统进入 Swap 或 OOM\n");

    int pid = get_vm_pid(target); // 仅作记录用

    int size_mb = 512;
    printf("输入要吞噬的内存大小 (MB): ");
    scanf("%d", &size_mb);
    getchar();

    char cmd[512];
    // 检查 mem_leak 是否存在 (源文件 MemLeak_injector.c or memleak_injector.c -> mem_leak)
    if (access("./mem_leak", F_OK) != 0)
    {
        printf(" [Info] 自动编译 mem_leak...\n");
        system("gcc -o mem_leak memleak_injector.c");
    }

    snprintf(cmd, sizeof(cmd), "./mem_leak %d %d", pid, size_mb);
    printf("执行: %s\n", cmd);
    system(cmd);
}

// === 主菜单 ===
void show_menu()
{
    int pid = get_vm_pid(target_process);

    printf("\n========================================\n");
    printf("   云平台故障注入系统 v2.2 (集成版)\n");
    printf("========================================\n");
    if (pid > 0)
        printf(" 当前目标: \033[32m%s\033[0m (PID: %d)\n", target_process, pid);
    else
        printf(" 当前目标: \033[31m%s\033[0m (未运行!)\n", target_process);
    printf("========================================\n");
    printf(" t. [设置] 切换攻击目标 (Switch Target)\n");
    printf("----------------------------------------\n");
    printf("[进程类故障]\n");
    printf(" 1. 进程宕机 (Crash/Kill)\n");
    printf(" 2. 进程死锁 (Hang/Stop)\n");
    printf(" 3. 进程恢复 (Resume/Cont)\n");
    printf("[网络类故障]\n");
    printf(" 4. 网络延迟 (Delay)\n");
    printf(" 5. 网络丢包 (Loss)\n");
    printf(" 6. 端口封锁 (Partition/Drop)\n");
    printf(" 7. 报文损坏 (Corrupt)\n");
    printf("[资源类故障]\n");
    printf(" 8. 内存错误注入 (Mem Injector)\n");
    printf(" 9. 寄存器注入 (Reg Injector)\n");
    printf(" 10. CPU 资源耗尽注入 (CPU Stress)\n");
    printf(" 11. 内存泄漏注入 (Mem Leak) \n");
    printf("----------------------------------------\n");
    printf(" c. 一键复原 (Clear All)\n");
    printf(" q. 退出 (Quit)\n");
    printf("========================================\n");
    printf(" 请输入选项: ");
}

int main(int argc, char *argv[])
{
    char input[10];
    char val[32];

    if (geteuid() != 0)
    {
        printf(" 严重错误: 请使用 sudo 运行此程序！\n");
        return 1;
    }

    // 允许通过命令行参数指定目标
    if (argc > 1)
    {
        strncpy(target_process, argv[1], sizeof(target_process) - 1);
    }

    // 预检查及编译
    if (access("./process_injector", F_OK) != 0)
        system("gcc -o process_injector process_injector.c");
    if (access("./network_injector", F_OK) != 0)
        system("gcc -o network_injector network_injector.c");

    while (1)
    {
        show_menu();
        if (fgets(input, sizeof(input), stdin) == NULL)
            break;
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "q") == 0)
            break;
        else if (strcmp(input, "t") == 0)
        {
            printf("\n请输入新的目标进程名 (例如 qemu-kvm, nginx, target): ");
            if (fgets(target_process, sizeof(target_process), stdin))
            {
                target_process[strcspn(target_process, "\n")] = 0;
            }
            printf("目标已切换为: %s\n", target_process);
        }
        else if (strcmp(input, "c") == 0)
        {
            inject_network_wrapper(0, NULL);
            inject_process_wrapper(target_process, 3);
        }
        else if (strcmp(input, "1") == 0)
            inject_process_wrapper(target_process, 1);
        else if (strcmp(input, "2") == 0)
            inject_process_wrapper(target_process, 2);
        else if (strcmp(input, "3") == 0)
            inject_process_wrapper(target_process, 3);
        else if (strcmp(input, "4") == 0)
        {
            printf("输入延迟 (如 500ms): ");
            scanf("%s", val);
            getchar();
            inject_network_wrapper(1, val);
        }
        else if (strcmp(input, "5") == 0)
        {
            printf("输入丢包率 (如 20%%): ");
            scanf("%s", val);
            getchar();
            inject_network_wrapper(2, val);
        }
        else if (strcmp(input, "6") == 0)
        {
            printf("输入端口 (如 8088): ");
            scanf("%s", val);
            getchar();
            inject_network_wrapper(3, val);
        }
        else if (strcmp(input, "7") == 0)
        {
            printf("输入损坏率 (如 10%%): ");
            scanf("%s", val);
            getchar();
            inject_network_wrapper(4, val);
        }
        else if (strcmp(input, "8") == 0)
        {
            inject_memory_wrapper(target_process);
        }
        else if (strcmp(input, "9") == 0)
        {
            inject_register_wrapper(target_process);
        }
        else if (strcmp(input, "10") == 0)
        {
            inject_cpu_wrapper(target_process);
        }
        else if (strcmp(input, "11") == 0)
        {
            inject_mem_leak_wrapper(target_process);
        }
        else
            printf(" 无效输入\n");
    }
    return 0;
}
