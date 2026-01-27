/*
 * reg_injector.c - 最终统一版 ARM64 寄存器注入器
 * 功能：支持全故障模型 + 立即/延时触发
 * 编译：gcc -o reg_injector reg_injector.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <unistd.h>
#include <elf.h>
#include <stdint.h>
#include <strings.h>
#include <time.h>
#include <signal.h>

// === 1. ARM64 寄存器结构定义 (防止头文件缺失) ===
struct user_pt_regs
{
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

// === 2. 故障类型定义 ===
typedef enum
{
    FAULT_1_BIT_FLIP,
    FAULT_2_BIT_FLIP,
    FAULT_1_BIT_0,
    FAULT_2_BIT_0,
    FAULT_1_BIT_1,
    FAULT_2_BIT_1,
    FAULT_8_LOW_0,
    FAULT_8_LOW_1,
    FAULT_8_LOW_ERROR,
    FAULT_PLUS_1,
    FAULT_PLUS_2,
    FAULT_PLUS_3,
    FAULT_PLUS_4,
    FAULT_PLUS_5
} FaultType;

// 全局变量 (用于信号处理)
pid_t global_target_pid = -1;

void die(const char *msg)
{
    perror(msg);
    exit(1);
}

// 辅助函数
int rand_bit() { return rand() % 64; }
uint64_t my_rand() { return (uint64_t)rand(); }

// 信号处理：定时器到期后暂停目标进程
void alarm_handler(int sig)
{
    if (global_target_pid > 0)
    {
        // 发送 SIGSTOP 暂停目标，以便 ptrace 接管
        kill(global_target_pid, SIGSTOP);
    }
}

void ptrace_attach(pid_t pid)
{
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0)
        die("Attach failed");
    waitpid(pid, NULL, 0);
}

void ptrace_detach(pid_t pid)
{
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
}

// === 3. 核心故障逻辑 ===
uint64_t apply_fault(uint64_t original, FaultType type, int user_specified_bit)
{
    uint64_t corrupted = original;
    int bit1 = (user_specified_bit >= 0) ? user_specified_bit : rand_bit();
    int bit2 = rand_bit();
    uint64_t mask_low_8 = 0xFFFFFFFFFFFFFF00;

    switch (type)
    {
    case FAULT_1_BIT_FLIP:
        corrupted ^= (1UL << bit1);
        break;
    case FAULT_2_BIT_FLIP:
        corrupted ^= (1UL << bit1);
        corrupted ^= (1UL << bit2);
        break;
    case FAULT_1_BIT_0:
        corrupted &= ~(1UL << bit1);
        break;
    case FAULT_2_BIT_0:
        corrupted &= ~(1UL << bit1);
        corrupted &= ~(1UL << bit2);
        break;
    case FAULT_1_BIT_1:
        corrupted |= (1UL << bit1);
        break;
    case FAULT_2_BIT_1:
        corrupted |= (1UL << bit1);
        corrupted |= (1UL << bit2);
        break;
    case FAULT_8_LOW_0:
        corrupted &= mask_low_8;
        break;
    case FAULT_8_LOW_1:
        corrupted |= 0xFF;
        break;
    case FAULT_8_LOW_ERROR:
        corrupted ^= (my_rand() & 0xFF);
        break;
    case FAULT_PLUS_1:
        corrupted += 1;
        break;
    case FAULT_PLUS_2:
        corrupted += 2;
        break;
    case FAULT_PLUS_3:
        corrupted += 3;
        break;
    case FAULT_PLUS_4:
        corrupted += 4;
        break;
    case FAULT_PLUS_5:
        corrupted += 5;
        break;
    default:
        break;
    }
    return corrupted;
}

int main(int argc, char *argv[])
{
    // 参数解析：支持 -w 选项
    if (argc < 4)
    {
        printf("用法: %s <PID> <Register> <Type> [Bit] [-w <microseconds>]\n", argv[0]);
        return 1;
    }

    srand(time(NULL));

    pid_t pid = atoi(argv[1]);
    char *reg_name = argv[2];
    char *type_str = argv[3];
    int bit = -1;
    int wait_usec = 0;

    // 解析可选参数
    for (int i = 4; i < argc; i++)
    {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc)
        {
            wait_usec = atoi(argv[i + 1]);
            i++;
        }
        else if (bit == -1)
        {
            bit = atoi(argv[i]);
        }
    }

    // 解析故障类型
    FaultType type = FAULT_1_BIT_FLIP;
    if (strcmp(type_str, "flip2") == 0)
        type = FAULT_2_BIT_FLIP;
    else if (strcmp(type_str, "zero1") == 0)
        type = FAULT_1_BIT_0;
    else if (strcmp(type_str, "zero2") == 0)
        type = FAULT_2_BIT_0;
    else if (strcmp(type_str, "set1") == 0)
        type = FAULT_1_BIT_1;
    else if (strcmp(type_str, "set2") == 0)
        type = FAULT_2_BIT_1;
    else if (strcmp(type_str, "low0") == 0)
        type = FAULT_8_LOW_0;
    else if (strcmp(type_str, "low1") == 0)
        type = FAULT_8_LOW_1;
    else if (strcmp(type_str, "lowerr") == 0)
        type = FAULT_8_LOW_ERROR;
    else if (strcmp(type_str, "add1") == 0)
        type = FAULT_PLUS_1;
    else if (strcmp(type_str, "add2") == 0)
        type = FAULT_PLUS_2;
    else if (strcmp(type_str, "add3") == 0)
        type = FAULT_PLUS_3;
    else if (strcmp(type_str, "add4") == 0)
        type = FAULT_PLUS_4;
    else if (strcmp(type_str, "add5") == 0)
        type = FAULT_PLUS_5;

    printf("=== ARM64 寄存器注入器 (PID: %d) ===\n", pid);

    // 1. 初次 Attach
    ptrace_attach(pid);
    global_target_pid = pid;

    // 2. 处理延时逻辑 (如果有 -w 参数)
    if (wait_usec > 0)
    {
        printf("⏳ 延时模式: 目标将继续运行 %.2f 秒...\n", wait_usec / 1000000.0);

        signal(SIGALRM, alarm_handler);
        ualarm(wait_usec, 0); // 设置微秒定时器

        // 放行
        if (ptrace(PTRACE_CONT, pid, NULL, NULL) < 0)
            die("PTRACE_CONT failed");

        // 等待被信号中断 (当 alarm_handler 发送 SIGSTOP 时，这里会返回)
        int status;
        waitpid(pid, &status, 0);

        if (WIFSTOPPED(status))
        {
            printf("⏰ 时间触发: 捕获目标，准备注入...\n");
        }
        else
        {
            printf("⚠️ 警告: 目标在等待期间异常退出。\n");
            return 1;
        }
    }
    else
    {
        printf("⚡ 立即模式: 直接注入...\n");
    }

    // 3. 获取寄存器
    struct user_pt_regs regs;
    struct iovec iov;
    iov.iov_base = &regs;
    iov.iov_len = sizeof(regs);
    if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) < 0)
        die("GETREGSET failed");

    // 4. 定位寄存器指针
    uint64_t *target_ptr = NULL;
    if (strcasecmp(reg_name, "PC") == 0)
        target_ptr = &regs.pc;
    else if (strcasecmp(reg_name, "SP") == 0)
        target_ptr = &regs.sp;
    else if (reg_name[0] == 'X' || reg_name[0] == 'x')
    {
        int idx = atoi(reg_name + 1);
        if (idx >= 0 && idx <= 30)
            target_ptr = &regs.regs[idx];
    }

    if (!target_ptr)
    {
        printf("❌ 无效寄存器\n");
        ptrace_detach(pid);
        return 1;
    }

    // 5. 应用故障
    uint64_t old_val = *target_ptr;
    uint64_t new_val = apply_fault(old_val, type, bit);
    *target_ptr = new_val;

    printf("[注入] %s: 0x%lx -> 0x%lx\n", reg_name, old_val, new_val);

    // 6. 写回并恢复
    if (ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov) < 0)
        die("SETREGSET failed");
    ptrace_detach(pid);
    printf("✅ 完成\n");

    return 0;
}