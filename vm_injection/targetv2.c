/*
 * target_v2.c - 全能故障注入测试靶子
 * 功能：暴露 Stack, Heap, 寄存器状态，用于验证注入器
 * 编译：gcc -o target_v2 target_v2.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

// 定义一个特征值，用于测试内存扫描注入
#define SIGNATURE 0xDEADBEEF

int main()
{
    // 1. 堆内存 (Heap)
    uint64_t *heap_val = (uint64_t *)malloc(sizeof(uint64_t));
    *heap_val = SIGNATURE;

    // 2. 栈内存 (Stack)
    uint64_t stack_val = SIGNATURE;

    // 3. 模拟寄存器敏感变量 (Counter)
    // 使用 volatile 防止编译器优化，确保它频繁加载到寄存器
    volatile uint64_t counter = 0;

    printf("=== 故障注入全能靶子 (PID: %d) ===\n", getpid());
    printf("[地址信息]\n");
    printf("  Heap 地址: %p (值: 0x%lx)\n", heap_val, *heap_val);
    printf("  Stack地址: %p (值: 0x%lx)\n", &stack_val, stack_val);
    printf("----------------------------------------\n");
    printf("正在运行... (请在另一个终端运行注入器)\n");

    while (1)
    {
        // 打印状态
        printf("PID:%d | Count:%lu | Heap:0x%lx | Stack:0x%lx\n",getpid(), counter, *heap_val, stack_val);

        // 简单计数
        counter++;

        // 检查特征值是否被篡改
        if (*heap_val != SIGNATURE)
        {
            printf("\n[!!!] 警告：堆内存被修改！当前值: 0x%lx\n", *heap_val);
            // 恢复以便继续测试
            *heap_val = SIGNATURE;
        }
        if (stack_val != SIGNATURE)
        {
            printf("\n[!!!] 警告：栈内存被修改！当前值: 0x%lx\n", stack_val);
            stack_val = SIGNATURE;
        }

        // 模拟计算负载
        for (int i = 0; i < 5000000; i++)
            ;
        usleep(500000); // 0.5秒打印一次
    }
    return 0;
}