#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void write_proc(const char *file, const char *val) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "echo %s > /proc/kvm-version-fi/%s", val, file);
    system(cmd);
}

int main() {
    if(geteuid()!=0) { printf("Need root\n"); return 1; }
    
    printf("ARM64 KVM Version Spoofing Tool\n");
    int t;
    printf("Times: ");
    scanf("%d", &t);
    
    char buf[16];
    sprintf(buf, "%d", t);
    write_proc("times", buf);
    write_proc("signal", "1");
    
    printf("Armed. KVM API Version will be reported as 0.\n");
    return 0;
}