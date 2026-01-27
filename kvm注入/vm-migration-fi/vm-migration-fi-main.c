#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main() {
    if(geteuid()!=0) return 1;
    printf("ARM64 VM Migration Failure Injector\n");
    // 写入 1 激活
    system("echo 1 > /proc/vm-migration-fi/signal");
    printf("Armed. Next live migration attempting to sync memory will fail.\n");
    return 0;
}