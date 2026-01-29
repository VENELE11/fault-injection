/*
 * target_mem.c - 内存敏感型靶子
 * 功能：尝试申请内存，验证系统剩余内存是否充足
 * 编译：gcc -o target_mem target_mem.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main()
{
    printf("=== 内存可用性监测靶子 ===\n");
    printf("正常情况：申请成功，速度快。\n");
    printf("注入后：申请失败，或因 Swap 导致速度极慢。\n");

    while (1)
    {
        // 尝试申请 100MB
        size_t size = 100 * 1024 * 1024;

        // 记录开始时间 (粗略计时即可)
        // 这里只是为了看卡顿，不需要高精度
        printf("[尝试] 申请 100MB 内存... ");
        fflush(stdout);

        char *ptr = (char *)malloc(size);
        if (ptr == NULL)
        {
            printf(" 失败! (OOM)\n");
        }
        else
        {
            // 尝试写入，测试是否会触发 Swap (变慢)
            memset(ptr, 0, size);
            printf(" 成功 (已释放)\n");
            free(ptr);
        }

        sleep(1);
    }
    return 0;
}