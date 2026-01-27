#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#define PROC_BASE "/proc/cpu-general-fi"

void write_proc(const char *file, const char *val) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo %s > %s/%s", val, PROC_BASE, file);
    if(system(cmd) != 0) {
        printf("Error writing %s\n", file);
    }
}

int main(int argc, char **argv)
{
    if(geteuid() != 0) {
        printf("Please run as root.\n");
        return 1;
    }

    printf("=========================================\n");
    printf("   ARM64 CPU Register Fault Injector\n");
    printf("=========================================\n");
    printf("Target Register:\n");
    printf("  1   : X0 (Arg 0)\n");
    printf("  2   : X1 (Arg 1)\n");
    printf("  4   : X2 (Arg 2)\n");
    printf("  8   : X3 (Arg 3)\n");
    printf("  16  : X4 (Arg 4)\n");
    printf("  32  : X5 (Arg 5)\n");
    printf("  64  : FP (Frame Pointer / X29)\n");
    printf("  128 : LR (Link Register / X30)\n");
    printf("  256 : SP (Stack Pointer)\n");
    printf("  512 : PC (Program Counter)\n");
    
    int aim;
    printf("Enter Bitmask (e.g., 1 for X0): ");
    scanf("%d", &aim);

    int times;
    printf("Fault Times: ");
    scanf("%d", &times);

    int lasting;
    printf("Mode (0:Flip, 1:Zero): ");
    scanf("%d", &lasting);

    char buf[32];
    
    sprintf(buf, "%d", aim);
    write_proc("aim", buf);

    sprintf(buf, "%d", times);
    write_proc("times", buf);

    sprintf(buf, "%d", lasting);
    write_proc("lasting", buf);

    // Trigger
    write_proc("signal", "1");

    printf("[+] CPU Injection Armed! Trigger logic is 'kernel_clone'.\n");
    printf("[+] Try running a command (e.g., 'ls') to trigger it.\n");
    return 0;
}