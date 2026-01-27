#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void write_proc(const char *file, const char *val) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "echo %s > /proc/kvm-state-fi/%s", val, file);
    system(cmd);
}

int main() {
    if(geteuid()!=0) { printf("Need root\n"); return 1; }
    
    printf("ARM64 KVM GetRegs Fault Injector\n");
    int t;
    printf("Times: ");
    scanf("%d", &t);
    
    char buf[16];
    sprintf(buf, "%d", t);
    write_proc("times", buf);
    write_proc("signal", "1");
    
    printf("Armed. Next KVM_GET_REGS call will fail (-EIO).\n");
    return 0;
}