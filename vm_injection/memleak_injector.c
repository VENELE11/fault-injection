/*
 * mem_leak.c - 内存资源耗尽注入器
 * 功能：持续吞噬宿主机物理内存，模拟 OOM (Out Of Memory) 场景
 * 编译：gcc -o mem_leak mem_leak.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char *argv[])
{
    // 参数兼容设计：保留 PID 位置，保持与 fault_controller 格式一致
    if (argc < 3)
    {
        printf("用法: %s <PID_ignored> <Size_MB>\n", argv[0]);
        printf("示例: %s 0 1024 (尝试占用 1GB 内存)\n", argv[0]);
        return 1;
    }

    int size_mb = atoi(argv[2]);
    long long total_bytes = (long long)size_mb * 1024 * 1024;
    long long current_bytes = 0;

    // 每次分配的块大小 (10MB)
    int chunk_size = 10 * 1024 * 1024;

    printf("=== 内存资源耗尽注入器 ===\n");
    printf("目标占用: %d MB\n", size_mb);
    printf("注意：这会触发系统级压力，可能导致 Swap 交换或进程被杀。\n");
    printf(" 开始吞噬内存...\n");


    while (current_bytes < total_bytes)
    {
        // 1. 申请虚拟内存
        char *ptr = (char *)malloc(chunk_size);
        if (ptr == NULL)
        {
            printf("\n malloc 失败！系统内存可能已耗尽。\n");
            break;
        }

        // 2. 关键步骤：写入数据 (Page Fault) 强制分配物理内存
        memset(ptr, 0xAA, chunk_size);

        current_bytes += chunk_size;

        // 打印进度条
        printf("\r[Eat] 已占用: %4lld MB / %4d MB", current_bytes / 1024 / 1024, size_mb);
        fflush(stdout);

        // 稍微休眠一下，防止瞬间卡死系统无法响应中断
        usleep(50000); // 50ms
    }

    printf("\n 分配完成。正在保持占用状态 60 秒...\n");
    printf("此时请观察靶子程序的反应 (或使用 'free -h' 查看)\n");

    // 保持占用 60 秒，期间这部分内存无法被其他进程使用
    for (int i = 0; i < 60; i++)
    {
        sleep(1);
    }

    // 程序退出后，OS 会自动回收这些内存
    printf("释放内存，退出。\n");
    return 0;
}