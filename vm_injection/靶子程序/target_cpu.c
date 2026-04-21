/*
 * target_cpu.c - CPU 故障注入测试靶场
 * 测试: cpu_injector (CPU 资源争抢)
 * 编译: gcc -o target_cpu target_cpu.c -lpthread -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <math.h>

volatile int keep_running = 1;
double baseline_score = 0.0;
int baseline_set = 0;

void sig_handler(int sig)
{
    printf("\n[退出]\n");
    keep_running = 0;
}

void *cpu_worker(void *arg)
{
    int id = *(int *)arg;
    long long iterations = 30000000;
    struct timeval start, end;
    int warmup = 3, cnt = 0;
    double sum = 0.0;

    printf("[Worker %d] 启动\n", id);

    while (keep_running)
    {
        gettimeofday(&start, NULL);

        volatile double r = 0.0;
        for (long long i = 0; i < iterations; i++)
        {
            r += sqrt((double)(i % 1000 + 1)) * sin((double)(i % 360));
        }

        gettimeofday(&end, NULL);
        double t = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1e6;
        if (t < 0.0001)
            t = 0.0001;
        double score = iterations / t / 1e6;

        if (!baseline_set)
        {
            cnt++;
            sum += score;
            if (cnt >= warmup)
            {
                baseline_score = sum / warmup;
                baseline_set = 1;
                printf("\n\033[32m[CPU] [OK] 基线测定完成: %.2f M/ops\033[0m\n", baseline_score);
                printf("----------------------------------------\n");
                printf("  现在可以运行 cpu_injector 进行测试\n");
                printf("  示例: ./cpu_injector %d 10 4\n", getpid());
                printf("----------------------------------------\n\n");
            }
            else
            {
                printf("[基线] 第 %d/%d 轮: %.2f M/ops\n", cnt, warmup, score);
            }
        }
        else
        {
            double deg = (baseline_score - score) / baseline_score * 100.0;

            // 构建进度条
            int bar_len = 20;
            int filled = (int)(score / baseline_score * bar_len);
            if (filled > bar_len)
                filled = bar_len;
            if (filled < 0)
                filled = 0;

            char bar[32];
            for (int i = 0; i < bar_len; i++)
            {
                bar[i] = (i < filled) ? '#' : '-';
            }
            bar[bar_len] = '\0';

            if (deg > 50)
            {
                printf("\033[31m[CPU] %s %.1f M/ops (v%.0f%%) CRITICAL!\033[0m\n", bar, score, deg);
            }
            else if (deg > 20)
            {
                printf("\033[33m[CPU] %s %.1f M/ops (v%.0f%%) WARNING\033[0m\n", bar, score, deg);
            }
            else if (deg > 5)
            {
                printf("\033[36m[CPU] %s %.1f M/ops (v%.0f%%)\033[0m\n", bar, score, deg);
            }
            else
            {
                printf("[CPU] %s %.1f M/ops [OK]\n", bar, score);
            }
        }
        usleep(500000);
    }
    return NULL;
}

int main()
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("\n");
    printf("+===============================================+\n");
    printf("|        CPU 故障注入测试靶场                   |\n");
    printf("+===============================================+\n");
    printf("|  PID: %-6d                                  |\n", getpid());
    printf("+===============================================+\n");
    printf("|  测试方法:                                    |\n");
    printf("|  ./cpu_injector <PID> <秒数> <线程数>         |\n");
    printf("|                                               |\n");
    printf("|  预期效果:                                    |\n");
    printf("|  * 算力(M/ops)明显下降                        |\n");
    printf("|  * 进度条变短，显示红色/黄色警告              |\n");
    printf("+===============================================+\n\n");

    pthread_t th;
    int id = 0;
    pthread_create(&th, NULL, cpu_worker, &id);

    while (keep_running)
        sleep(1);

    pthread_join(th, NULL);
    printf("[Main] 结束\n");
    return 0;
}
