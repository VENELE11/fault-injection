/*
 * target_mem.c - 内存故障注入测试靶场
 * 测试: mem_injector (内存篡改)
 * 编译: gcc -o target_mem target_mem.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

// 特征值 - 与 mem_injector 默认扫描值匹配
// mem_injector 默认搜索 "deadbeefcafebabe" (16进制)
#define CANARY_64 0xDEADBEEFCAFEBABEULL
#define CANARY_32 0xDEADBEEFU

volatile int keep_running = 1;

// ==================== 全局诱饵区 ====================
// 使用对齐确保 mem_injector 可以正确扫描
volatile uint64_t g_canary_1 __attribute__((aligned(8))) = CANARY_64;
volatile uint64_t g_canary_2 __attribute__((aligned(8))) = CANARY_64;
volatile uint64_t g_canary_3 __attribute__((aligned(8))) = CANARY_64;
volatile uint64_t g_canary_4 __attribute__((aligned(8))) = CANARY_64;

// 诱饵数组
volatile uint64_t g_canary_array[16] __attribute__((aligned(8)));

// 堆上诱饵
volatile uint64_t *g_heap_canary = NULL;

void sig_handler(int sig)
{
    printf("\n[退出]\n");
    keep_running = 0;
}

void print_hex_diff(uint64_t expected, uint64_t actual)
{
    printf("  期望值: 0x%016llX\n", (unsigned long long)expected);
    printf("  实际值: 0x%016llX\n", (unsigned long long)actual);
    printf("  差异位: ");
    uint64_t diff = expected ^ actual;
    for (int i = 63; i >= 0; i--)
    {
        if (diff & (1ULL << i))
        {
            printf("\033[31m%d\033[0m ", i);
        }
    }
    printf("\n");
}

void *mem_watcher(void *arg)
{
    // 初始化全局数组
    for (int i = 0; i < 16; i++)
    {
        g_canary_array[i] = CANARY_64;
    }

    // 分配堆内存诱饵
    g_heap_canary = (volatile uint64_t *)aligned_alloc(8, sizeof(uint64_t) * 32);
    if (!g_heap_canary)
    {
        perror("aligned_alloc");
        return NULL;
    }
    for (int i = 0; i < 32; i++)
    {
        g_heap_canary[i] = CANARY_64;
    }

    // 栈上诱饵
    volatile uint64_t stack_canary_1 __attribute__((aligned(8))) = CANARY_64;
    volatile uint64_t stack_canary_2 __attribute__((aligned(8))) = CANARY_64;

    printf("\n[MEM] 诱饵部署完成:\n");
    printf("----------------------------------------\n");
    printf("  全局变量:\n");
    printf("    g_canary_1: %p\n", (void *)&g_canary_1);
    printf("    g_canary_2: %p\n", (void *)&g_canary_2);
    printf("    g_canary_3: %p\n", (void *)&g_canary_3);
    printf("    g_canary_4: %p\n", (void *)&g_canary_4);
    printf("  堆区 (32个): %p - %p\n",
           (void *)g_heap_canary,
           (void *)(g_heap_canary + 31));
    printf("  栈区:\n");
    printf("    stack_1: %p\n", (void *)&stack_canary_1);
    printf("    stack_2: %p\n", (void *)&stack_canary_2);
    printf("----------------------------------------\n");
    printf("  特征值: 0x%016llX\n", (unsigned long long)CANARY_64);
    printf("  (mem_injector 用 'deadbeefcafebabe' 扫描)\n");
    printf("----------------------------------------\n\n");

    int check_count = 0;
    int total_corruptions = 0;

    while (keep_running)
    {
        check_count++;
        int found_this_round = 0;

        // 检查全局变量
        struct
        {
            volatile uint64_t *ptr;
            const char *name;
        } globals[] = {
            {&g_canary_1, "g_canary_1"},
            {&g_canary_2, "g_canary_2"},
            {&g_canary_3, "g_canary_3"},
            {&g_canary_4, "g_canary_4"},
        };

        for (int i = 0; i < 4; i++)
        {
            if (*globals[i].ptr != CANARY_64)
            {
                printf("\n\033[31m+========================================+\033[0m\n");
                printf("\033[31m| [!!!] %s 被篡改!              |\033[0m\n", globals[i].name);
                printf("\033[31m+========================================+\033[0m\n");
                print_hex_diff(CANARY_64, *globals[i].ptr);
                *globals[i].ptr = CANARY_64;
                found_this_round++;
                total_corruptions++;
            }
        }

        // 检查堆
        for (int i = 0; i < 32; i++)
        {
            if (g_heap_canary[i] != CANARY_64)
            {
                printf("\n\033[31m[MEM] #### heap[%d] 被篡改!\033[0m\n", i);
                print_hex_diff(CANARY_64, g_heap_canary[i]);
                g_heap_canary[i] = CANARY_64;
                found_this_round++;
                total_corruptions++;
            }
        }

        // 检查数组
        for (int i = 0; i < 16; i++)
        {
            if (g_canary_array[i] != CANARY_64)
            {
                printf("\n\033[31m[MEM] #### array[%d] 被篡改!\033[0m\n", i);
                print_hex_diff(CANARY_64, g_canary_array[i]);
                g_canary_array[i] = CANARY_64;
                found_this_round++;
                total_corruptions++;
            }
        }

        // 检查栈
        if (stack_canary_1 != CANARY_64)
        {
            printf("\n\033[31m[MEM] #### stack_1 被篡改!\033[0m\n");
            print_hex_diff(CANARY_64, stack_canary_1);
            stack_canary_1 = CANARY_64;
            found_this_round++;
            total_corruptions++;
        }
        if (stack_canary_2 != CANARY_64)
        {
            printf("\n\033[31m[MEM] #### stack_2 被篡改!\033[0m\n");
            print_hex_diff(CANARY_64, stack_canary_2);
            stack_canary_2 = CANARY_64;
            found_this_round++;
            total_corruptions++;
        }

        // 定期状态报告
        if (check_count % 5 == 0)
        {
            if (found_this_round > 0)
            {
                printf("\033[33m[MEM] 检查 #%d: 发现 %d 处篡改 (累计: %d)\033[0m\n",
                       check_count, found_this_round, total_corruptions);
            }
            else
            {
                printf("[MEM] 检查 #%d: [OK] 全部正常 (累计篡改: %d)\n",
                       check_count, total_corruptions);
            }
        }

        sleep(1);
    }

    free((void *)g_heap_canary);
    return NULL;
}

int main()
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("\n");
    printf("+===============================================+\n");
    printf("|       内存故障注入测试靶场                    |\n");
    printf("+===============================================+\n");
    printf("|  PID: %-6d                                  |\n", getpid());
    printf("+===============================================+\n");
    printf("|  测试方法 (扫描模式):                         |\n");
    printf("|  ./mem_injector -p %d -r heap \\            |\n", getpid());
    printf("|     -s deadbeefcafebabe -t flip -b 0          |\n");
    printf("|                                               |\n");
    printf("|  测试方法 (盲注模式):                         |\n");
    printf("|  ./mem_injector -p %d -r heap -t flip -b 0  |\n", getpid());
    printf("|                                               |\n");
    printf("|  预期效果:                                    |\n");
    printf("|  * 检测到内存被篡改                           |\n");
    printf("|  * 显示期望值与实际值对比                     |\n");
    printf("|  * 显示被篡改的位                             |\n");
    printf("+===============================================+\n");

    pthread_t th;
    pthread_create(&th, NULL, mem_watcher, NULL);

    while (keep_running)
        sleep(1);

    pthread_join(th, NULL);
    printf("[Main] 结束\n");
    return 0;
}
