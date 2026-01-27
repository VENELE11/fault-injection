/*
 * fault_controller.c - 云平台故障注入控制器 (最终集成版)
 * 功能：集成 Process, Network, Memory, Register 故障注入
 * 现阶段注入目标默认为 QEMU 进程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

// 查找目标进程 PID
int get_vm_pid(const char *proc_name)
{
    char cmd[128];
    char output[32];
    snprintf(cmd, sizeof(cmd), "pgrep -f %s | head -n 1", proc_name);

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
        printf("❌ [错误] 未找到进程: %s (需先启动目标程序)\n", target);
        return;
    }

    printf("\n--- 内存故障配置 (PID: %d) ---\n", pid);
    printf("1. 盲注 (Blind Injection - Heap/Stack)\n");
    printf("2. 扫描特征值注入 (Scan & Inject)\n");
    printf("👉 选择模式: ");

    char input[10];
    if (!fgets(input, sizeof(input), stdin))
        return;
    int mode = atoi(input);

    char region[10] = "heap";
    char fault_type[10] = "flip";
    char signature[32] = "";
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
        printf("输入16进制特征值 (不带0x, 如 deadbeef): ");
        if (fgets(signature, sizeof(signature), stdin))
            signature[strcspn(signature, "\n")] = 0;

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

// === 模块 4: 寄存器故障注入 (Wrapper) [新增] ===
void inject_register_wrapper(const char *target)
{
    int pid = get_vm_pid(target);
    if (pid == -1)
    {
        printf("❌ [错误] 未找到进程: %s\n", target);
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
// === 模块 5: CPU 资源耗尽注入 (新增) ===
void inject_cpu_wrapper(const char *target)
{
    int pid = get_vm_pid(target);
    if (pid == -1)
    {
        printf("❌ 未找到进程: %s\n", target);
        return;
    }

    printf("\n--- CPU 高负载注入 (PID: %d) ---\n", pid);
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

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "./cpu_injector %d %d %d", pid, duration, threads);

    printf("执行: %s\n", cmd);
    system(cmd);
}

// === 模块 6: 内存泄漏注入 (新增) ===
void inject_mem_leak_wrapper(const char *target)
{
    printf("\n--- 内存泄漏注入 (Resource Exhaustion) ---\n");
    printf("原理: 注入器大量占用宿主机 RAM，迫使目标进程 OOM 或 Swap\n");

    // 这里 PID 实际上没用，但为了流程统一我们还是传进去
    int pid = get_vm_pid(target);

    int size_mb = 512;
    printf("输入要吞噬的内存大小 (MB): ");
    scanf("%d", &size_mb);
    getchar(); // 吃掉换行符

    char cmd[512];
    // PID 传 0 即可，因为这是系统级故障
    snprintf(cmd, sizeof(cmd), "./mem_leak %d %d", pid, size_mb);

    printf("执行: %s\n", cmd);
    system(cmd);
}

// === 主菜单 ===
void show_menu()
{
    printf("\n========================================\n");
    printf("   云平台故障注入系统 (最终完整版)\n");
    printf("========================================\n");
    printf("[进程类故障]\n");
    printf(" 1. 虚拟机宕机 (Crash)\n");
    printf(" 2. 虚拟机死锁 (Hang)\n");
    printf(" 3. 虚拟机恢复 (Resume)\n");
    printf("[网络类故障]\n");
    printf(" 4. 网络延迟 (Delay)\n");
    printf(" 5. 网络丢包 (Loss)\n");
    printf(" 6. 网络分区 (Partition)\n");
    printf(" 7. 报文损坏 (Corrupt)\n");
    printf("[资源类故障]\n");
    printf(" 8. 内存错误注入 (Mem Injector)\n");
    printf(" 9. 寄存器注入 (Reg Injector)\n");
    printf(" 10. CPU 资源耗尽注入 (CPU Injector)\n");
    printf(" 11. 内存泄漏 (Mem Leak) \n");
    printf("----------------------------------------\n");
    printf(" c. 一键复原 (Clear All)\n");
    printf(" q. 退出 (Quit)\n");
    printf("========================================\n");
    printf("👉 请输入选项: ");
}

int main()
{
    char input[10];
    char val[32];
    char target[32] = "qemu";

    if (geteuid() != 0)
    {
        printf("🔴 严重错误: 请使用 sudo 运行此程序！\n");
        return 1;
    }

    // 检查依赖文件
    if (access("./process_injector", F_OK) != 0 ||
        access("./network_injector", F_OK) != 0 ||
        access("./mem_injector", F_OK) != 0 ||
        access("./reg_injector", F_OK) != 0)
    {
        printf("⚠️  警告: 未找到部分子模块 (process/network/mem/reg_injector)\n");
        printf("   请确保所有模块均已编译: gcc xxx.c -o xxx\n");
    }

    while (1)
    {
        show_menu();
        if (fgets(input, sizeof(input), stdin) == NULL)
            break;
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "q") == 0)
            break;
        else if (strcmp(input, "c") == 0)
        {
            inject_network_wrapper(0, NULL);
            inject_process_wrapper(target, 3);
        }
        else if (strcmp(input, "1") == 0)
            inject_process_wrapper(target, 1);
        else if (strcmp(input, "2") == 0)
            inject_process_wrapper(target, 2);
        else if (strcmp(input, "3") == 0)
            inject_process_wrapper(target, 3);
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
            printf("输入端口 (如 80): ");
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
            inject_memory_wrapper(target);
        }
        else if (strcmp(input, "9") == 0)
        {
            // 注意：如果你要测试 target.c，请临时将上面的 target 变量改为 "target"
            // 或者在这里手动写死为 "target" 用于演示
            // inject_register_wrapper("target");
            inject_register_wrapper(target);
        }
        // 在 main 的 while 循环中添加：
        else if (strcmp(input, "10") == 0)
        {
            inject_cpu_wrapper(target);
        }
        else if (strcmp(input, "11") == 0)
        {
            inject_mem_leak_wrapper(target);
        }
        else
            printf("❌ 无效输入\n");
    }
    return 0;
}