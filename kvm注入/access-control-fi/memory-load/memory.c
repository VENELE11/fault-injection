/*
 * memory.c - 简单的内存负载生成器
 * 用法: ./memory-usage <Size_MB>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <Size_MB>\n", argv[0]);
        return 1;
    }

    int size_mb = atoi(argv[1]);
    if (size_mb <= 0) size_mb = 128; // 默认 128MB
    
    long long bytes = (long long)size_mb * 1024 * 1024;
    char *ptr = (char *)malloc(bytes);
    
    if (!ptr) {
        perror("malloc failed");
        return 1;
    }

    printf("Allocated %d MB. Starting stress loop (Ctrl+C to stop)...\n", size_mb);
    
    // 持续读写内存，强制触发缺页和 TLB 刷新
    while (1) {
        // memset 触发写故障
        memset(ptr, 0xAA, bytes);
        
        // 简单的读检查，防止被优化
        if (ptr[bytes-1] != 0xAA) {
            printf("Memory corruption detected!\n");
        }
        
        usleep(100000); // 休眠 100ms，避免 CPU 占用过高 (只测内存)
    }

    free(ptr);
    return 0;
}