/*
 * cpu_injector.c - CPU 高负载故障注入器 (增强版)
 * 功能：创建高强度计算线程，争抢 CPU 资源 (Resource Exhaustion)
 * 增强：支持 CPU 亲和性绑定、高优先级、多种压力模式
 * 编译：gcc -o cpu_injector cpu_injector.c -lpthread -lm
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>

// 全局标志位，控制线程运行
volatile int keep_running = 1;

// 压力测试线程函数 - 增强版
// 混合整数、浮点、内存访问，最大化 CPU 占用
void *stress_worker(void *arg)
{
    int core_id = *(int *)arg;

    // 尝试绑定到指定 CPU 核心 (Linux)
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id % sysconf(_SC_NPROCESSORS_ONLN), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif

    // 尝试提高线程优先级
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    // 分配一些内存用于缓存压力
    volatile double *arr = malloc(sizeof(double) * 10000);
    if (!arr)
        arr = malloc(sizeof(double) * 100);

    volatile double x = 1.0;
    volatile long long counter = 0;

    while (keep_running)
    {
        // 混合多种计算，避免 CPU 流水线优化
        for (int i = 0; i < 1000; i++)
        {
            // 浮点运算
            x = sqrt(x + 1.0) * sin(x) + cos(x * 0.1);
            x = (x > 1e10 || x < -1e10) ? 1.0 : x;

            // 整数运算
            counter += i * (i + 1);
            counter ^= (counter >> 3);

            // 内存访问 (如果有数组)
            if (arr)
            {
                arr[i % 100] = x;
                x += arr[(i + 50) % 100];
            }
        }
    }

    if (arr)
        free((void *)arr);
    return NULL;
}

// 简单压力线程 (备用)
void *simple_stress(void *arg)
{
    volatile double x = 0.0;
    while (keep_running)
    {
        x = sqrt(rand() % 100000) * tan(rand() % 100000);
        if (x > 10000000)
            x = 0;
    }
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("用法: %s <PID> <Duration_Sec> [Threads] [Mode]\n", argv[0]);
        printf("参数:\n");
        printf("  PID      - 目标进程 (用于日志)\n");
        printf("  Duration - 持续秒数\n");
        printf("  Threads  - 线程数 (默认=CPU核心数x2)\n");
        printf("  Mode     - 模式: 1=普通 2=激进 (默认2)\n");
        printf("\n示例: %s 1234 30 8 2\n", argv[0]);
        return 1;
    }

    int target_pid = atoi(argv[1]);
    int duration = atoi(argv[2]);
    int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int num_threads = num_cpus * 2; // 默认 2 倍核心数
    int mode = 2;                   // 默认激进模式

    if (argc >= 4)
        num_threads = atoi(argv[3]);
    if (argc >= 5)
        mode = atoi(argv[4]);

    if (num_threads <= 0)
        num_threads = num_cpus * 2;
    if (num_threads > 256)
        num_threads = 256;

    printf("╔═══════════════════════════════════════════════╗\n");
    printf("║     CPU 高负载注入器 (增强版)                 ║\n");
    printf("╠═══════════════════════════════════════════════╣\n");
    printf("║ 目标 PID: %-6d                              ║\n", target_pid);
    printf("║ 持续时间: %-3d 秒                              ║\n", duration);
    printf("║ 压力线程: %-3d 个 (CPU核心: %d)                ║\n", num_threads, num_cpus);
    printf("║ 压力模式: %s                            ║\n", mode == 2 ? "激进" : "普通");
    printf("╚═══════════════════════════════════════════════╝\n\n");

    // 尝试提高进程优先级
    if (setpriority(PRIO_PROCESS, 0, -20) < 0)
    {
        printf("[提示] 无法提高优先级 (需要 root)\n");
    }
    else
    {
        printf("[✓] 已提高进程优先级\n");
    }

    pthread_t *threads = malloc(num_threads * sizeof(pthread_t));
    int *core_ids = malloc(num_threads * sizeof(int));
    if (!threads || !core_ids)
    {
        perror("malloc failed");
        return 1;
    }

    printf("[*] 启动 %d 个压力线程...\n", num_threads);

    // 启动压力线程
    for (int i = 0; i < num_threads; i++)
    {
        core_ids[i] = i;
        void *(*worker)(void *) = (mode == 2) ? stress_worker : simple_stress;
        if (pthread_create(&threads[i], NULL, worker, &core_ids[i]) != 0)
        {
            perror("创建线程失败");
        }
    }

    printf("[*] 开始施压!\n\n");

    // 倒计时
    for (int i = 0; i < duration; i++)
    {
        int bar_len = 30;
        int filled = (i + 1) * bar_len / duration;

        printf("\r[");
        for (int j = 0; j < bar_len; j++)
        {
            printf(j < filled ? "█" : "░");
        }
        printf("] %d/%d 秒 ", i + 1, duration);
        fflush(stdout);
        sleep(1);
    }
    printf("\n\n");

    // 停止
    keep_running = 0;
    printf("[*] 停止施压...\n");

    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    free(core_ids);
    printf("[✓] CPU 注入结束\n");

    return 0;
}