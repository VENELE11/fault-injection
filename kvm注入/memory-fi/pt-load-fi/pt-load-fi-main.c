#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PROC_BASE "/proc/pt-load-fi"

void write_proc(const char *file, const char *val) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "echo %s > %s/%s", val, PROC_BASE, file);
    if(system(cmd) != 0) {
        printf("Error: Failed to write %s\n", file);
    }
}

int main(int argc, char *argv[]) {
    if (geteuid() != 0) {
        printf("Error: Run as root.\n");
        return 1;
    }

    printf("========================================\n");
    printf("   ARM64 Memory (PageTable) Fault Injector\n");
    printf("========================================\n");
    printf("Target: handle_mm_fault\n");
    
    int type, times;
    
    printf("Fault Type:\n");
    printf("  0: VM_FAULT_OOM (Simulate Out Of Memory)\n");
    printf("  1: VM_FAULT_SIGBUS (Simulate Bus Error/Invalid Map)\n");
    printf("Select (0/1): ");
    scanf("%d", &type);
    
    printf("Fault Times: ");
    scanf("%d", &times);
    
    char nbuf[16];
    sprintf(nbuf, "%d", type);
    write_proc("type", nbuf);
    
    sprintf(nbuf, "%d", times);
    write_proc("times", nbuf);
    
    // Enable
    write_proc("signal", "1");
    
    printf("\n[+] Injection Armed!\n");
    printf("CAUTION: This affects the whole system if not careful.\n");
    printf("Running programs will fail to allocate memory page.\n");
    
    return 0;
}