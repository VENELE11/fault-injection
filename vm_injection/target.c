/*
 * target.c - 全能故障注入演练靶场 (综合版)
 * =======================================================
 * 功能：集成 CPU、内存、网络、寄存器等多种业务场景
 *
 * 注意：如需单独测试某种故障，请使用对应的独立靶场：
 *   target_cpu - CPU 注入测试
 *   target_mem - 内存注入测试
 *   target_reg - 寄存器注入测试
 *   target_net - 网络注入测试
 *   target_res - 资源耗尽测试
 *
 * 编译：gcc -o target target.c -lpthread -lm
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
#include <math.h>
#include <netdb.h>
#include <fcntl.h>

#define CANARY_VAL 0xDEADBEEF
volatile int keep_running = 1;

// 基线算力，用于对比
double baseline_score = 0.0;
int baseline_set = 0;

// 全局指针，方便 mem_injector 扫描到
volatile uint64_t *g_heap_canary = NULL;
volatile uint64_t g_stack_canary = 0;

// 网络探测目标 (可配置)
#define NET_PROBE_HOST "127.0.0.1"
#define NET_PROBE_PORT 8088

// === 模块1: CPU 性能敏感线程 (测试 cpu-stress) ===
void *cpu_monitor(void *arg)
{
    long long iterations = 50000000; // 5000万次
    struct timeval start, end;
    int warmup_rounds = 3;
    int sample_count = 0;
    double score_sum = 0.0;

    printf(" [CPU线程] 已启动，正在进行基线测定...\n");

    while (keep_running)
    {
        gettimeofday(&start, NULL);

        volatile double result = 0.0;
        for (long long i = 0; i < iterations; i++)
        {
            result += sqrt((double)(i % 1000 + 1)) * sin((double)(i % 360));
        }

        gettimeofday(&end, NULL);

        double time_spent = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000000.0;
        if (time_spent < 0.0001)
            time_spent = 0.0001;

        double score = iterations / time_spent / 1000000.0;

        if (!baseline_set)
        {
            sample_count++;
            score_sum += score;
            if (sample_count >= warmup_rounds)
            {
                baseline_score = score_sum / warmup_rounds;
                baseline_set = 1;
                printf("\n\033[32m[CPU] ✓ 基线测定完成: %.2f M/ops\033[0m\n\n", baseline_score);
            }
            else
            {
                printf(" [CPU 基线] 第 %d/%d 轮: %.2f M/ops\n", sample_count, warmup_rounds, score);
            }
        }
        else
        {
            double degradation = ((baseline_score - score) / baseline_score) * 100.0;

            if (degradation > 50.0)
            {
                printf("\033[31m[CPU] ████ 严重降级! %.2f M/ops (↓%.1f%%)\033[0m\n", score, degradation);
            }
            else if (degradation > 20.0)
            {
                printf("\033[33m[CPU] ██   性能下降  %.2f M/ops (↓%.1f%%)\033[0m\n", score, degradation);
            }
            else if (degradation > 5.0)
            {
                printf("\033[36m[CPU] █    轻微波动  %.2f M/ops (↓%.1f%%)\033[0m\n", score, degradation);
            }
            else
            {
                printf("[CPU]      正常 %.2f M/ops (基线: %.2f)\n", score, baseline_score);
            }
        }
        usleep(800000); // 800ms
    }
    return NULL;
}

// === 模块2: 内存敏感线程 (测试 mem-fi) ===
void *mem_watcher(void *arg)
{
    g_heap_canary = (volatile uint64_t *)malloc(sizeof(uint64_t) * 16);
    for (int i = 0; i < 16; i++)
    {
        g_heap_canary[i] = CANARY_VAL;
    }

    g_stack_canary = CANARY_VAL;
    volatile uint64_t local_canary = CANARY_VAL;

    printf(" [MEM线程] 内存诱饵已部署:\n");
    printf("   > Heap : %p (16个 0x%X)\n", (void *)g_heap_canary, CANARY_VAL);
    printf("   > Stack: %p\n", (void *)&local_canary);

    int check_count = 0;
    while (keep_running)
    {
        check_count++;
        int corrupted = 0;

        // 检查堆
        for (int i = 0; i < 16; i++)
        {
            if (g_heap_canary[i] != CANARY_VAL)
            {
                printf("\n\033[31m[MEM] ████ 堆内存[%d]被篡改! 0x%lx -> 期望 0x%X\033[0m\n",
                       i, (unsigned long)g_heap_canary[i], CANARY_VAL);
                g_heap_canary[i] = CANARY_VAL;
                corrupted = 1;
            }
        }

        // 检查全局栈
        if (g_stack_canary != CANARY_VAL)
        {
            printf("\n\033[31m[MEM] ████ 全局变量被篡改! 0x%lx\033[0m\n", (unsigned long)g_stack_canary);
            g_stack_canary = CANARY_VAL;
            corrupted = 1;
        }

        // 检查局部栈
        if (local_canary != CANARY_VAL)
        {
            printf("\n\033[31m[MEM] ████ 栈内存被篡改! 0x%lx\033[0m\n", (unsigned long)local_canary);
            local_canary = CANARY_VAL;
            corrupted = 1;
        }

        if (check_count % 10 == 0 && !corrupted)
        {
            printf("[MEM] 检查 #%d: ✓ 正常\n", check_count);
        }

        sleep(1);
    }
    free((void *)g_heap_canary);
    return NULL;
}

// === 模块3: 寄存器敏感线程 (测试 reg-fi) ===
// 这个线程使用紧密循环，让 reg_injector 有机会注入时影响计算结果
void *reg_watcher(void *arg)
{
    printf(" [REG线程] 寄存器敏感计算已启动\n");
    printf("   > 使用累加器检测计算错误\n\n");

    // 使用简单的数学关系来验证计算正确性
    volatile uint64_t accumulator = 0;
    volatile uint64_t iteration = 0;
    uint64_t last_report_iter = 0;
    int error_count = 0;

    while (keep_running)
    {
        // 每轮执行 100万次 简单加法运算
        // 这些运算会使用通用寄存器，reg_injector 可以篡改
        uint64_t local_sum = 0;
        uint64_t expected_sum = 0;

        for (uint64_t i = 0; i < 1000000; i++)
        {
            local_sum += i;
            expected_sum += i;

            // 额外的寄存器操作，增加被注入的概率
            volatile uint64_t temp = local_sum * 2;
            temp = temp / 2;
            if (temp != local_sum)
            {
                printf("\033[35m[REG] !!!! 计算异常! temp=%lu, local_sum=%lu\033[0m\n",
                       (unsigned long)temp, (unsigned long)local_sum);
                error_count++;
            }
        }

        iteration++;
        accumulator += local_sum;

        // 校验计算结果
        // 0+1+2+...+999999 = 999999*1000000/2 = 499999500000
        uint64_t correct_sum = 499999500000ULL;

        if (local_sum != correct_sum)
        {
            printf("\033[35m[REG] ████ 累加结果异常! 得到: %lu, 期望: %lu (差值: %ld)\033[0m\n",
                   (unsigned long)local_sum, (unsigned long)correct_sum,
                   (long)(local_sum - correct_sum));
            error_count++;
        }

        // 每5轮报告一次状态
        if (iteration - last_report_iter >= 5)
        {
            if (error_count > 0)
            {
                printf("\033[35m[REG] 迭代 #%lu: 检测到 %d 次计算错误!\033[0m\n",
                       (unsigned long)iteration, error_count);
            }
            else
            {
                printf("[REG] 迭代 #%lu: ✓ 计算正常\n", (unsigned long)iteration);
            }
            last_report_iter = iteration;
            error_count = 0;
        }

        usleep(200000); // 200ms，让注入有机会发生
    }
    return NULL;
}

// === 模块4: 网络服务 + 主动探测线程 (测试 network-fi) ===
void *net_server(void *arg)
{
    int server_fd;
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
        perror(" [NET] Bind失败");
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 3) < 0)
    {
        perror(" [NET] Listen失败");
        close(server_fd);
        return NULL;
    }

    printf(" [NET线程] TCP服务已启动，端口: %d\n", port);

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);

    while (keep_running)
    {
        socklen_t addrlen = sizeof(address);
        int new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen);
        if (new_socket >= 0)
        {
            char buffer[1024] = {0};
            read(new_socket, buffer, 1024);
            const char *msg = "Target Alive.\n";
            send(new_socket, msg, strlen(msg), 0);
            printf("[NET] 收到连接: %s\n", inet_ntoa(address.sin_addr));
            close(new_socket);
        }
    }
    close(server_fd);
    return NULL;
}

// 网络主动探测线程
void *net_prober(void *arg)
{
    printf(" [NET探测] 主动网络探测已启动\n");
    printf("   > 目标: %s:%d\n", NET_PROBE_HOST, NET_PROBE_PORT);
    printf("   > 用于检测: 延迟、丢包、端口封锁\n\n");

    double baseline_latency = -1.0;
    int probe_count = 0;
    int fail_count = 0;
    int consecutive_fails = 0;

    sleep(2); // 等待服务器启动

    while (keep_running)
    {
        probe_count++;
        struct timeval t1, t2;

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            printf("\033[31m[NET探测] Socket创建失败\033[0m\n");
            sleep(2);
            continue;
        }

        // 设置连接超时
        struct timeval timeout;
        timeout.tv_sec = 3;
        timeout.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(NET_PROBE_PORT);
        inet_pton(AF_INET, NET_PROBE_HOST, &serv_addr.sin_addr);

        gettimeofday(&t1, NULL);
        int connected = connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));

        if (connected < 0)
        {
            gettimeofday(&t2, NULL);
            double elapsed = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;

            fail_count++;
            consecutive_fails++;

            if (consecutive_fails >= 3)
            {
                printf("\033[31m[NET探测] ████ 连续 %d 次连接失败! (端口可能被封锁)\033[0m\n", consecutive_fails);
            }
            else if (elapsed > 2000)
            {
                printf("\033[33m[NET探测] ██   连接超时 (%.0fms) - 可能丢包/延迟\033[0m\n", elapsed);
            }
            else
            {
                printf("\033[33m[NET探测] 连接失败 #%d: %s\033[0m\n", fail_count, strerror(errno));
            }
            close(sock);
            sleep(2);
            continue;
        }

        // 发送测试数据
        const char *probe_msg = "PROBE";
        send(sock, probe_msg, strlen(probe_msg), 0);

        char buf[256] = {0};
        recv(sock, buf, sizeof(buf) - 1, 0);

        gettimeofday(&t2, NULL);
        double latency = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;

        close(sock);
        consecutive_fails = 0;

        // 基线测定
        if (baseline_latency < 0 && probe_count <= 3)
        {
            baseline_latency = latency;
            printf(" [NET探测] 基线延迟: %.2f ms\n", baseline_latency);
        }
        else if (baseline_latency > 0)
        {
            double ratio = latency / baseline_latency;

            if (ratio > 10.0)
            {
                printf("\033[31m[NET探测] ████ 严重延迟! %.2f ms (%.1fx 基线)\033[0m\n", latency, ratio);
            }
            else if (ratio > 3.0)
            {
                printf("\033[33m[NET探测] ██   延迟升高  %.2f ms (%.1fx 基线)\033[0m\n", latency, ratio);
            }
            else if (ratio > 1.5)
            {
                printf("\033[36m[NET探测] █    轻微延迟  %.2f ms\033[0m\n", latency);
            }
            else if (probe_count % 10 == 0)
            {
                printf("[NET探测] #%d: ✓ 正常 (%.2f ms)\n", probe_count, latency);
            }
        }

        sleep(2);
    }
    return NULL;
}

// === 模块5: 资源压力感知线程 (测试 memleak-fi) ===
void *res_monitor(void *arg)
{
    printf(" [RES线程] 资源监控已启动\n\n");

    double baseline_time = -1.0;
    int sample_count = 0;

    while (keep_running)
    {
        size_t test_size = 50 * 1024 * 1024;
        struct timeval t1, t2;

        gettimeofday(&t1, NULL);
        char *ptr = (char *)malloc(test_size);
        if (ptr)
        {
            memset(ptr, 0xAA, test_size);
        }
        gettimeofday(&t2, NULL);

        double cost_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;

        if (ptr == NULL)
        {
            printf("\033[31m[RES] ████ 内存分配失败! 系统 OOM!\033[0m\n");
        }
        else
        {
            if (baseline_time < 0 && sample_count < 3)
            {
                sample_count++;
                if (sample_count == 3)
                {
                    baseline_time = cost_ms;
                    printf(" [RES] 内存分配基线: %.2f ms\n", baseline_time);
                }
            }
            else if (baseline_time > 0)
            {
                double slowdown = cost_ms / (baseline_time > 0.1 ? baseline_time : 0.1);
                if (slowdown > 10.0)
                {
                    printf("\033[31m[RES] ████ 分配严重变慢! %.2f ms (%.1fx)\033[0m\n", cost_ms, slowdown);
                }
                else if (slowdown > 3.0)
                {
                    printf("\033[33m[RES] ██   分配变慢 %.2f ms (%.1fx)\033[0m\n", cost_ms, slowdown);
                }
            }
            free(ptr);
        }
        sleep(3);
    }
    return NULL;
}

// === 信号处理 ===
void sig_handler(int signo)
{
    if (signo == SIGINT || signo == SIGTERM)
    {
        printf("\n [Main] 收到退出信号...\n");
        keep_running = 0;
    }
}

int main()
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║       全能故障注入演练靶场 v5.0                  ║\n");
    printf("╠══════════════════════════════════════════════════╣\n");
    printf("║  测试项:                                          ║\n");
    printf("║   [CPU]  - cpu_injector (资源争抢)               ║\n");
    printf("║   [MEM]  - mem_injector (内存篡改)               ║\n");
    printf("║   [REG]  - reg_injector (寄存器注入)             ║\n");
    printf("║   [NET]  - network_injector (延迟/丢包/封锁)     ║\n");
    printf("║   [RES]  - memleak_injector (OOM)                ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");
    printf("  PID: %d\n", getpid());
    printf("  内存特征值: 0x%X\n", CANARY_VAL);
    printf("  网络端口: %d\n\n", NET_PROBE_PORT);

    pthread_t t_cpu, t_mem, t_reg, t_net_srv, t_net_probe, t_res;

    pthread_create(&t_cpu, NULL, cpu_monitor, NULL);
    pthread_create(&t_mem, NULL, mem_watcher, NULL);
    pthread_create(&t_reg, NULL, reg_watcher, NULL);
    pthread_create(&t_net_srv, NULL, net_server, NULL);
    pthread_create(&t_net_probe, NULL, net_prober, NULL);
    pthread_create(&t_res, NULL, res_monitor, NULL);

    while (keep_running)
    {
        sleep(1);
    }

    printf(" [Main] 等待线程回收...\n");
    pthread_join(t_cpu, NULL);
    pthread_join(t_mem, NULL);
    pthread_join(t_reg, NULL);
    pthread_join(t_net_srv, NULL);
    pthread_join(t_net_probe, NULL);
    pthread_join(t_res, NULL);

    printf(" [Main] 靶场关闭。\n");
    return 0;
}