/*
 * cpu_injector.c - CPU 高负载故障注入器
 * 功能：创建高强度计算线程，争抢 CPU 资源 (Resource Exhaustion)
 * 编译：gcc -o cpu_injector cpu_injector.c -lpthread -lm
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

// 全局标志位，控制线程运行
volatile int keep_running = 1;

// 压力测试线程函数
// 执行密集的浮点运算以消耗 CPU
void *stress_worker(void *arg)
{
    double x = 0.0;
    while (keep_running)
    {
        // 进行无意义的高强度计算
        x = sqrt(rand() % 100000) * tan(rand() % 100000);
        // 防止编译器优化掉这段代码
        if (x > 10000000)
            x = 0;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("用法: %s <PID> <Duration_Sec> [Threads]\n", argv[0]);
        printf("说明: PID 参数仅用于日志记录（CPU 压力通常是系统级的）\n");
        printf("示例: %s 1234 10 4 (模拟10秒高负载，使用4个线程)\n", argv[0]);
        return 1;
    }

    int target_pid = atoi(argv[1]);
    int duration = atoi(argv[2]);
    int num_threads = 1;

    // 默认线程数 = CPU 核心数 (如果没有指定)
    if (argc >= 4)
    {
        num_threads = atoi(argv[3]);
    }
    else
    {
        num_threads = sysconf(_SC_NPROCESSORS_ONLN);
    }

    printf("=== CPU 高负载注入器 (Target PID: %d) ===\n", target_pid);
    printf("[配置] 持续时间: %d 秒\n", duration);
    printf("[配置] 压力线程: %d 个 (模拟多核满载)\n", num_threads);
    printf("🚀 开始注入 CPU 压力...\n");

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    if (!threads)
    {
        perror("malloc failed");
        return 1;
    }

    // 1. 启动压力线程
    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_create(&threads[i], NULL, stress_worker, NULL) != 0)
        {
            perror("创建线程失败");
        }
    }

    // 2. 倒计时等待
    // 这里我们简单使用 sleep，期间线程在后台疯狂计算
    for (int i = 0; i < duration; i++)
    {
        if (i % 1 == 0)
        {
            printf("\r⏳ 正在施压... %d/%d 秒", i + 1, duration);
            fflush(stdout);
        }
        sleep(1);
    }
    printf("\n");

    // 3. 停止并清理
    keep_running = 0;
    printf("🛑 时间到，停止施压...\n");

    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    printf("✅ CPU 注入结束，资源已释放。\n");

    return 0;
}