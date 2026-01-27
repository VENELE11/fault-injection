#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROC_BASE "/proc/memory-manage-fi"

void write_proc(const char *file, int val) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo %d > %s/%s", val, PROC_BASE, file);
    system(cmd);
}

int main(int argc, char **argv)
{
    int input;
    if(geteuid()!=0){printf("Need root\n");return 1;}

    printf("ARM64 Memory Management Fault Injector\n");
    printf("--------------------------------------\n");

    // 1. 选择目标函数 (Class)
    printf("目标函数:\n 1. kvm_set_memory_region\n 2. gfn_to_hva_many\nChoice: ");
    scanf("%d", &input);
    write_proc("class", input);

    // 2. 选择参数位置
    printf("故障参数位置 (1-8对应X0-X7):\nChoice: ");
    scanf("%d", &input);
    write_proc("position", input);

    // 3. 故障类型
    printf("故障类型:\n 1. Flip\n 2. Set1\n 3. Set0\nChoice: ");
    scanf("%d", &input);
    write_proc("type", input);

    // 4. 次数
    printf("故障次数: ");
    scanf("%d", &input);
    write_proc("time", input);

    // 5. 触发
    write_proc("signal", 1);

    printf("Armed.\n");
    return 0;
}