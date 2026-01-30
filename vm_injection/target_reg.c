/*
 * target_reg.c - 寄存器故障注入测试靶场 (v5 - 最终修正版)
 * 编译: gcc -O0 -o target_reg target_reg.c -lpthread
 *
 * 三层门槛：采样掩码、500ms间隔、连续3次才 ALERT
 *
 * 工作原理:
 * 1. 计数器线程在 `x19` 寄存器上进行高速累加，并将结果同步到
 *    一个全局共享变量 `g_shared_counter`。
 * 2. 报告线程从 `g_shared_counter` 读取计数值并计算差量（Delta），
 *    以展示注入效果。
 *
 * 如何测试:
 * 1. 编译: make target_reg
 * 2. 运行: ./target_reg
 * 3. 注入: sudo ./reg_injector <PID> X19 add1 -1 -l 0
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>

// 计数器仍在 x19 寄存器中操作
volatile register uint64_t counter asm("x19");

// 引入一个全局共享变量，用于线程间通信
volatile uint64_t g_shared_counter = 0;

// 影子计数器（内存中），用于和寄存器值做一致性校验
volatile uint64_t g_shadow_counter = 0;

// 线程控制标志
volatile int running = 1;

void sig_handler(int sig)
{
    if (running)
    {
        printf("\n[INFO] Signal received, shutting down gracefully...\n");
        running = 0;
    }
}

static inline uint64_t now_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main()
{
    // 设置信号处理
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // 关闭stdout缓冲，确保日志“瞬间”可见
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("[INFO] Starting register injection target (PID: %d).\n", getpid());
    printf("[INFO] Press Ctrl+C to exit.\n");

    counter = 0;
    g_shared_counter = 0;
    g_shadow_counter = 0;

    uint64_t last_counter_value = 0;
    int mismatch_count = 0;
    uint64_t last_report_ns = now_ns();

    printf("+--------------------------------------------------------------------------------------+\n");
    printf("| %-25s | %-20s | %-15s | %-18s | %-8s |\n",
           "Timestamp", "Counter Value", "Delta", "Shadow", "Status");
    printf("+--------------------------------------------------------------------------------------+\n");

    while (running)
    {
        counter++;
        g_shadow_counter++;
        g_shared_counter = counter;

        // 每约 100ms 输出一次，保证“瞬间可见”但不过度闪烁
        if ((g_shadow_counter & 0xFFFFF) == 0)
        {
            uint64_t now = now_ns();
            if (now - last_report_ns >= 500000000ull)
            {
                uint64_t current_counter_value = g_shared_counter;
                uint64_t shadow_value = g_shadow_counter;
                long long delta = (long long)(current_counter_value - last_counter_value);

                if (llabs((long long)(current_counter_value - shadow_value)) > 1000)
                {
                    mismatch_count++;
                    printf("| %-25ld | %-20llu | %-15lld | %-18llu | %-8s |\n",
                           (long)time(NULL),
                           (unsigned long long)current_counter_value,
                           delta,
                           (unsigned long long)shadow_value,
                           (mismatch_count >= 3) ? "ALERT" : "CHECK");
                }
                else
                {
                    mismatch_count = 0;
                    printf("| %-25ld | %-20llu | %-15lld | %-18llu | %-8s |\n",
                           (long)time(NULL),
                           (unsigned long long)current_counter_value,
                           delta,
                           (unsigned long long)shadow_value,
                           "OK");
                }

                last_counter_value = current_counter_value;
                last_report_ns = now;
            }
        }
    }

    printf("+--------------------------------------------------------------------------------------+\n");
    printf("[INFO] Program finished. Final counter value: %llu\n", (unsigned long long)g_shared_counter);

    return 0;
}
