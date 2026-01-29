#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void code_function()
{
    printf("代码段正在执行...\n");
}

int main()
{
    // 1. 堆上的数据
    long *heap_val = malloc(sizeof(long));
    *heap_val = 0xAAAAAAAAAAAAAAAA;

    // 2. 栈上的数据
    long stack_val = 0x5555555555555555;

    printf("PID: %d\n", getpid());
    printf("[DEBUG] 堆变量地址: %p\n", heap_val);
    printf("[DEBUG] 栈变量地址: %p\n", &stack_val);

    while (1)
    {
        printf("---------------------------\n");
        printf("Heap Value : 0x%lx\n", *heap_val);
        printf("Stack Value: 0x%lx\n", stack_val);
        sleep(2);
    }
    return 0;
}