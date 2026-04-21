#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void write_proc(const char *file, const char *val) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "echo %s > /proc/kswapd-fi/%s", val, file);
    system(cmd);
}

int main() {
    if(geteuid()!=0) return 1;
    printf("ARM64 Kswapd (Memory Reclaim) Injector\n");
    int t;
    printf("Fault Times: ");
    scanf("%d", &t);
    char buf[16];
    sprintf(buf, "%d", t);
    write_proc("times", buf);
    write_proc("signal", "1");
    return 0;
}