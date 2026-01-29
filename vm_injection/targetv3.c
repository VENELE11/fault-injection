/*
 * target_cpu_real.c - 真实时间敏感型靶子
 * 功能：使用 Wall Clock Time 计算性能，能体现 CPU 争抢的影响
 * 编译：gcc -o target_cpu target_cpu.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h> // 引入真实时间库
#include <sys/types.h>

int main()
{
    pid_t pid = getpid();
    printf("=== CPU 真实性能靶子 (PID: %d) ===\n", pid);
    printf("使用 Wall Clock Time 计时，注入故障后数值必跌！\n");
    printf("----------------------------------------\n");

    struct timeval start, end;

    // 每次测试的计算量 (根据机器性能调整，推荐 5亿次)
    long long iterations = 500000000;

    while (1)
    {
        // 1. 获取真实世界的开始时间
        gettimeofday(&start, NULL);

        // 2. 疯狂计算 (CPU 密集型任务)
        long long count = 0;
        for (long long i = 0; i < iterations; i++)
        {
            count += (i % 3); // 稍微复杂一点的运算
        }

        // 3. 获取真实世界的结束时间
        gettimeofday(&end, NULL);

        // 4. 计算耗时 (秒)
        double time_spent = (end.tv_sec - start.tv_sec) +
                            (end.tv_usec - start.tv_usec) / 1000000.0;

        // 5. 计算得分 (M/ops)
        if (time_spent < 0.0001)
            time_spent = 0.0001;
        double score = iterations / time_spent / 1000000.0;

        printf("[PID:%d] 真实性能: %6.2f M/ops (真实耗时: %5.3fs)\n",
               pid, score, time_spent);
    }
    return 0;
}