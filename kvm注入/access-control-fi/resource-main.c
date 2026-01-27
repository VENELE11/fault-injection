#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_LINE 256
#define PROC_BASE "/proc/resource"

void write_proc(const char *file, int val) {
    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "echo %d > %s/%s", val, PROC_BASE, file);
    if(system(cmd) != 0) {
        printf("Error writing %s\n", file);
    }
}

int main(int argc, char **argv)
{
    int input;

    if (geteuid() != 0) {
        printf("Please run as root.\n");
        return 1;
    }

    /* 原版参数较少，此处进入向导模式 */
    printf("ARM64 Access Control (Resource) FI\n");
    printf("----------------------------------\n");
    
    // 1. 选择故障位置
    printf("故障位置:\n 1. IOCTL CMD (x1)\n 2. IOCTL ARG (x2)\nChoice: ");
    scanf("%d", &input);
    write_proc("position", input);

    // 2. 选择故障类型
    printf("故障类型:\n 1. 随机一位翻转 (Flip)\n 2. 随机一位置1 (Set1)\n 3. 随机一位置0 (Set0)\nChoice: ");
    scanf("%d", &input);
    write_proc("type", input);

    // 3. 次数
    printf("故障次数: ");
    scanf("%d", &input);
    write_proc("time", input);
    
    // 4. 模式 (原版有 style)
    // printf("模式 (0:Transient, 2:Persistent): ");
    // scanf("%d", &input);
    // write_proc("style", input);

    // 5. 触发
    write_proc("signal", 1);
    
    printf("Injection Armed! Waiting for 'kvm_vm_ioctl' calls...\n");
    return 0;
}