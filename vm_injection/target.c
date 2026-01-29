/*
 * target.c - 全能故障注入演练靶场 (All-in-One Target)
 * =======================================================
 * 功能：集成 CPU、内存、网络、IO 等多种业务场景，
 *       专门用于配合 fault_controller 进行全方位故障测试。
 *
 * 包含线程：
 * 1. [CPU_Worker]  : 密集浮点运算，计算算力 (M/ops)，敏感于 CPU 争抢/限制。
 * 2. [Mem_Watcher] : 维护特征值 (0xDEADBEEF...)，敏感于内存篡改/寄存器错误。
 * 3. [Net_Server]  : TCP :8088 回显服务，敏感于网络延迟/丢包/中断。
 * 4. [Res_Monitor] : 资源分配监控，敏感于系统内存耗尽 (MemLeak 注入)。
 *
 * 编译：gcc -o target target.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>

#define CANARY_VAL 0xDEADBEEF // 内存/寄存器注入的目标特征值 (兼容 targetv2)
volatile int keep_running = 1;

// === 模块1: CPU 性能敏感线程 (测试 perf-delay, cpu-stress) ===
void *cpu_monitor(void *arg)
{
    long long iterations = 500000000; // 5亿次计算 (与 targetv3 一致)
    struct timeval start, end;

    printf(" [CPU线程] 已启动，基线计算量: %lld ops/cycle\n", iterations);

    while (keep_running)
    {
        gettimeofday(&start, NULL);

        // 模拟高负载计算 (避免被编译器过度优化)
        volatile long long count = 0;
        for (long long i = 0; i < iterations; i++)
        {
            count += (i % 3); // 与 targetv3 算法保持一致
        }

        gettimeofday(&end, NULL);

        double time_spent = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
        if (time_spent < 0.0001)
            time_spent = 0.0001;

        double score = iterations / time_spent / 1000000.0;

        // 只有当算力严重下降时才高亮显示
        printf("[CPU] 算力: %6.2f M/ops (耗时: %.3fs)\n", score, time_spent);

        // 移除 sleep 以模拟真实的高负载争抢 (targetv3 行为)
    }
    return NULL;
}

// === 模块2: 内存与寄存器敏感线程 (测试 mem-fi, reg-fi) ===
void *mem_reg_watcher(void *arg)
{
    // 1. 堆内存诱饵
    // 我们故意申请一个指针，让 mem_injector 有机会扫描到它
    uint64_t *heap_val = (uint64_t *)malloc(sizeof(uint64_t));
    *heap_val = CANARY_VAL;

    // 2. 栈内存诱饵 (volatile防止优化)
    volatile uint64_t stack_val = CANARY_VAL;

    // 3. 模拟寄存器敏感变量 (Counter)
    volatile uint64_t counter = 0;

    printf(" [MEM线程] 内存诱饵已部署:\n");
    printf("   > Heap Addr : %p (Value: 0x%lx)\n", heap_val, *heap_val);
    printf("   > Stack Addr: %p (Value: 0x%lx)\n", &stack_val, stack_val);

    while (keep_running)
    {
        // 简单计数
        counter++;

        // 检查堆
        if (*heap_val != CANARY_VAL)
        {
            printf("\n\033[31m[!!!] 严重告警: 堆内存(Heap)被篡改! 当前值: 0x%lx\033[0m\n", *heap_val);
            *heap_val = CANARY_VAL; // 自动修复
        }

        // 检查栈
        if (stack_val != CANARY_VAL)
        {
            printf("\n\033[31m[!!!] 严重告警: 栈内存(Stack)被篡改! 当前值: 0x%lx\033[0m\n", stack_val);
            stack_val = CANARY_VAL; // 自动修复
        }

        sleep(1);
    }
    free(heap_val);
    return NULL;
}

// === 模块3: 网络服务线程 (测试 network-fi) ===
void *net_server(void *arg)
{
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int port = 8088;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror(" [NET] Socket创建失败");
        return NULL;
    }

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror(" [NET] Bind失败(端口占用?)");
        return NULL;
    }

    if (listen(server_fd, 3) < 0)
    {
        perror(" [NET] Listen失败");
        return NULL;
    }

    printf(" [NET线程] TCP监控服务已启动，端口: %d\n", port);

    // 设置 Accept 超时，避免线程无法退出
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

    while (keep_running)
    {
        socklen_t addrlen = sizeof(address);
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) >= 0)
        {
            char buffer[1024] = {0};
            // 尝试读取，如果有网络延迟/丢包，这里可能会慢
            read(new_socket, buffer, 1024);

            const char *msg = "Target Alive.\n";
            send(new_socket, msg, strlen(msg), 0);

            printf(" [NET] 收到连接来自 %s (通信正常)\n", inet_ntoa(address.sin_addr));
            close(new_socket);
        }
    }
    close(server_fd);
    return NULL;
}

// === 模块4: 资源压力感知线程 (测试 memleak-fi) ===
void *res_monitor(void *arg)
{
    printf(" [RES线程] 资源监控已启动 (检测 OOM/Swap)\n");

    while (keep_running)
    {
        // 尝试申请 100MB 内存 (与 targetv4 保持一致)，如果系统内存被耗尽，这里会极慢或者失败
        size_t test_size = 100 * 1024 * 1024;
        struct timeval t1, t2;

        gettimeofday(&t1, NULL);
        char *ptr = (char *)malloc(test_size);
        gettimeofday(&t2, NULL);

        double cost_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;

        if (ptr == NULL)
        {
            printf("\n\033[31m[!!!] 内存分配失败! 系统可能已 OOM。\033[0m\n");
        }
        else
        {
            // 只有当分配显著变慢时才打印 (比如 > 5ms)
            // 正常 malloc 应该微秒级
            if (cost_ms > 5.0)
            {
                printf(" [RES] 内存分配迟滞! 耗时: %.2f ms (系统可能在 Swap)\n", cost_ms);
            }
            // 必须写入才能逼迫系统分配物理页
            memset(ptr, 0, test_size);
            free(ptr);
        }

        sleep(1); // 频率可以稍微高一点，与 targetv4 保持一致
    }
    return NULL;
}

// === 信号处理 ===
void sig_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM)
    {
        printf("\n [Main] 收到退出信号，正在停止所有线程...\n");
        keep_running = 0;
    }
}

int main()
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("################################################\n");
    printf("##           全能故障注入演练靶场             ##\n");
    printf("##               (Target v3.0)                ##\n");
    printf("################################################\n");
    printf("PID: %d\n\n", getpid());

    pthread_t t1, t2, t3, t4;

    // 启动各业务线程
    if (pthread_create(&t1, NULL, cpu_monitor, NULL) != 0)
        perror("Thread 1 err");
    if (pthread_create(&t2, NULL, mem_reg_watcher, NULL) != 0)
        perror("Thread 2 err");
    if (pthread_create(&t3, NULL, net_server, NULL) != 0)
        perror("Thread 3 err");
    if (pthread_create(&t4, NULL, res_monitor, NULL) != 0)
        perror("Thread 4 err");

    // 主线程仅仅等待
    while (keep_running)
    {
        sleep(1);
    }

    printf(" [Main] 等待线程回收...\n");
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);

    printf(" [Main] 靶场安全关闭。\n");
    return 0;
}
