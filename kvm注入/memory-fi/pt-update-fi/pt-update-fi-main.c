#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROC_BASE "/proc/pt-update-fi"

void write_proc(const char *file, const char *val) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo %s > %s/%s", val, PROC_BASE, file);
    system(cmd);
}

int main(int argc, char *argv[]) {
    if (geteuid() != 0) {
        printf("Need root.\n");
        return 1;
    }

    printf("======================================\n");
    printf("   ARM64 PageTable Update Injector\n");
    printf("======================================\n");
    printf("Target: flush_tlb_mm (Simulating Stale TLB)\n");
    
    int times;
    printf("Fault Times: ");
    scanf("%d", &times);
    
    char nbuf[16];
    sprintf(nbuf, "%d", times);
    write_proc("times", nbuf);
    
    write_proc("signal", "1");
    
    printf("Injector Armed. Check dmesg.\n");
    return 0;
}