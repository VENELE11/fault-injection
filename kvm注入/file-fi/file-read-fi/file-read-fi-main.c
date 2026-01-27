#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROC_BASE "/proc/file-read-fi"

void write_proc(const char *file, const char *val)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo %s > %s/%s", val, PROC_BASE, file);
    if (system(cmd) != 0)
    {
        printf("Error: Failed to write to %s/%s (Module loaded?)\n", PROC_BASE, file);
    }
}

int main(int argc, char *argv[])
{
    if (geteuid() != 0)
    {
        printf("Error: Please run as root (sudo).\n");
        return 1;
    }

    printf("======================================\n");
    printf("   ARM64 File-Read Fault Injector\n");
    printf("======================================\n");

    int type, times;

    printf("Fault Type:\n");
    printf("  0: Truncate Read (Force count=0, return EOF)\n");
    printf("  1: Bad Buffer (Force buf=NULL, return -EFAULT)\n");
    printf("Select Type (0/1): ");
    scanf("%d", &type);

    printf("Fault Times (How many reads to fail): ");
    scanf("%d", &times);

    char nbuf[16];

    // 1. 设置类型
    sprintf(nbuf, "%d", type);
    write_proc("type", nbuf);

    // 2. 设置次数
    sprintf(nbuf, "%d", times);
    write_proc("times", nbuf);

    // 3. 开启信号
    write_proc("signal", "1");

    printf("\n[+] Injection ARMED! Waiting for 'vfs_read' calls...\n");
    printf("[+] Check 'dmesg' for kernel logs.\n");

    return 0;
}