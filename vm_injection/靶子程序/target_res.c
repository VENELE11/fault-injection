/*
 * target_res.c - 资源耗尽故障注入测试靶场
 * 测试: memleak_injector (内存泄漏/OOM)
 * 编译: gcc -o target_res target_res.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/sysinfo.h>

volatile int keep_running = 1;

// 基线数据
double baseline_alloc_time = -1;
double baseline_free_mem = -1;

void sig_handler(int sig)
{
    printf("\n[退出]\n");
    keep_running = 0;
}

// 获取系统内存信息
void get_mem_info(long *total_mb, long *free_mb, long *avail_mb)
{
    struct sysinfo si;
    if (sysinfo(&si) == 0)
    {
        *total_mb = si.totalram * si.mem_unit / 1024 / 1024;
        *free_mb = si.freeram * si.mem_unit / 1024 / 1024;
        // available ≈ free + buffers + cached (简化)
        *avail_mb = (si.freeram + si.bufferram) * si.mem_unit / 1024 / 1024;
    }
    else
    {
        *total_mb = *free_mb = *avail_mb = 0;
    }
}

// 绘制进度条
void draw_bar(double ratio, int width, char *buf)
{
    int filled = (int)(ratio * width);
    if (filled > width)
        filled = width;
    if (filled < 0)
        filled = 0;

    for (int i = 0; i < width; i++)
    {
        if (i < filled)
        {
            if (ratio > 0.9)
                buf[i] = '#'; // 危险
            else if (ratio > 0.7)
                buf[i] = '='; // 警告
            else
                buf[i] = '-'; // 正常
        }
        else
        {
            buf[i] = ' ';
        }
    }
    buf[width] = '\0';
}

void *memory_tester(void *arg)
{
    printf("[RES] 内存分配测试启动\n");
    printf("----------------------------------------\n");
    printf("  测试: 分配 50MB -> 写入 -> 释放\n");
    printf("  检测: 分配时间、系统可用内存\n");
    printf("----------------------------------------\n\n");

    int round = 0;
    int warmup = 3;

    while (keep_running)
    {
        round++;
        size_t alloc_size = 50 * 1024 * 1024; // 50MB
        struct timeval t1, t2, t3;

        // 获取分配前内存状态
        long total_mb, free_before, avail_before;
        get_mem_info(&total_mb, &free_before, &avail_before);

        // 分配
        gettimeofday(&t1, NULL);
        char *ptr = (char *)malloc(alloc_size);
        gettimeofday(&t2, NULL);

        double alloc_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 +
                          (t2.tv_usec - t1.tv_usec) / 1000.0;

        if (ptr == NULL)
        {
            printf("\n\033[31m+========================================+\033[0m\n");
            printf("\033[31m| [!!!] malloc 失败! 系统内存耗尽!       |\033[0m\n");
            printf("\033[31m+========================================+\033[0m\n\n");
            sleep(2);
            continue;
        }

        // 写入 (触发实际物理内存分配)
        memset(ptr, 0xAA, alloc_size);
        gettimeofday(&t3, NULL);

        double write_ms = (t3.tv_sec - t2.tv_sec) * 1000.0 +
                          (t3.tv_usec - t2.tv_usec) / 1000.0;

        // 获取分配后内存状态
        long free_after, avail_after;
        get_mem_info(&total_mb, &free_after, &avail_after);

        // 释放
        free(ptr);

        // 基线测定
        if (round <= warmup)
        {
            if (round == warmup)
            {
                baseline_alloc_time = alloc_ms > 0.1 ? alloc_ms : 0.1;
                baseline_free_mem = avail_before;
                printf("\033[32m[RES] [OK] 基线测定完成\033[0m\n");
                printf("      分配时间: %.2f ms\n", baseline_alloc_time);
                printf("      可用内存: %ld MB\n\n", (long)baseline_free_mem);
            }
            continue;
        }

        // 计算指标
        double alloc_ratio = alloc_ms / baseline_alloc_time;
        double mem_ratio = (double)avail_before / baseline_free_mem;
        double used_ratio = 1.0 - (double)avail_before / total_mb;

        // 绘制内存使用条
        char bar[32];
        draw_bar(used_ratio, 20, bar);

        // 显示结果
        int alert_level = 0; // 0=正常, 1=注意, 2=警告, 3=严重

        if (alloc_ratio > 10 || mem_ratio < 0.3)
        {
            alert_level = 3;
        }
        else if (alloc_ratio > 3 || mem_ratio < 0.5)
        {
            alert_level = 2;
        }
        else if (alloc_ratio > 1.5 || mem_ratio < 0.7)
        {
            alert_level = 1;
        }

        if (alert_level >= 3)
        {
            printf("\033[31m[RES] #### 严重!\033[0m\n");
            printf("      分配: %.1fms (%.1fx基线)\n", alloc_ms, alloc_ratio);
            printf("      写入: %.1fms\n", write_ms);
            printf("      内存: [%s] %ld/%ld MB (%.0f%%)\n",
                   bar, avail_before, total_mb, used_ratio * 100);
            if (mem_ratio < 0.3)
            {
                printf("      \033[31m警告: 可用内存不足基线的30%%!\033[0m\n");
            }
        }
        else if (alert_level >= 2)
        {
            printf("\033[33m[RES] ###  警告\033[0m\n");
            printf("      分配: %.1fms (%.1fx) | 内存: %ld MB\n",
                   alloc_ms, alloc_ratio, avail_before);
        }
        else if (alert_level >= 1)
        {
            printf("\033[36m[RES] ##   注意: 分配%.1fms (%.1fx)\033[0m\n",
                   alloc_ms, alloc_ratio);
        }
        else if (round % 10 == 0)
        {
            // 正常情况定期报告
            printf("[RES] #%d [OK] 分配:%.1fms 可用:%ldMB\n",
                   round, alloc_ms, avail_before);
        }

        sleep(2);
    }
    return NULL;
}

void *system_monitor(void *arg)
{
    sleep(3);
    printf("[SYS] 系统资源监控启动\n\n");

    while (keep_running)
    {
        long total_mb, free_mb, avail_mb;
        get_mem_info(&total_mb, &free_mb, &avail_mb);

        double used_ratio = 1.0 - (double)avail_mb / total_mb;
        char bar[32];
        draw_bar(used_ratio, 30, bar);

        // 只在内存压力大时显示
        if (used_ratio > 0.8)
        {
            printf("\033[31m[SYS] 内存压力: [%s] %.0f%%\033[0m\n", bar, used_ratio * 100);
        }
        else if (used_ratio > 0.6)
        {
            printf("\033[33m[SYS] 内存使用: [%s] %.0f%%\033[0m\n", bar, used_ratio * 100);
        }

        sleep(5);
    }
    return NULL;
}

int main()
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // 获取初始内存信息
    long total_mb, free_mb, avail_mb;
    get_mem_info(&total_mb, &free_mb, &avail_mb);

    printf("\n");
    printf("+===================================================+\n");
    printf("|       资源耗尽故障注入测试靶场                    |\n");
    printf("+===================================================+\n");
    printf("|  PID: %-6d                                      |\n", getpid());
    printf("|  系统内存: %ld MB (可用: %ld MB)                \n", total_mb, avail_mb);
    printf("+===================================================+\n");
    printf("|  测试方法:                                        |\n");
    printf("|  ./mem_leak %d <MB数>                           |\n", getpid());
    printf("|  例如: ./mem_leak %d 1024  (占用1GB)            |\n", getpid());
    printf("|                                                   |\n");
    printf("|  预期效果:                                        |\n");
    printf("|  * 内存分配时间显著增加                           |\n");
    printf("|  * 系统可用内存下降                               |\n");
    printf("|  * 可能触发 Swap 交换                             |\n");
    printf("|  * 严重时 malloc 失败                             |\n");
    printf("+===================================================+\n\n");

    pthread_t th_mem, th_sys;
    pthread_create(&th_mem, NULL, memory_tester, NULL);
    pthread_create(&th_sys, NULL, system_monitor, NULL);

    while (keep_running)
        sleep(1);

    pthread_join(th_mem, NULL);
    pthread_join(th_sys, NULL);

    printf("[Main] 结束\n");
    return 0;
}
