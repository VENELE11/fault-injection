/*
 * network_injector.c - 网络故障注入工具
 * 功能：Delay, Loss, Partition, Corrupt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// 获取当前系统的默认网卡名称
void get_interface_name(char *buffer, size_t size)
{
    // 使用 ip route 命令自动提取网卡名
    FILE *fp = popen("ip route get 8.8.8.8 | awk '{print $5; exit}'", "r");
    if (fp == NULL)
    {
        perror("获取网卡失败");
        exit(1);
    }
    fgets(buffer, size, fp);
    buffer[strcspn(buffer, "\n")] = 0; // 去除换行符
    pclose(fp);
}

void inject_network(int type, const char *param)
{
    char nic[32];
    get_interface_name(nic, sizeof(nic));
    char cmd[512];

    // 清理旧规则
    sprintf(cmd, "tc qdisc del dev %s root 2>/dev/null", nic);
    system(cmd);
    // 清理所有封锁端口的规则
    sprintf(cmd, "iptables -F OUTPUT 2>/dev/null");
    system(cmd);

    if (type == 0)
    {
        printf(" 网络故障已清理，网卡 %s 恢复正常\n", nic);
        return;
    }
    // 注入新故障
    if (type == 1)
    { // Delay
        sprintf(cmd, "tc qdisc add dev %s root netem delay %s", nic, param);
        printf(" [Delay] 已注入延迟: %s (设备: %s)\n", param, nic);
    }
    else if (type == 2)
    { // Loss
        sprintf(cmd, "tc qdisc add dev %s root netem loss %s", nic, param);
        printf(" [Loss] 已注入丢包率: %s (设备: %s)\n", param, nic);
    }
    else if (type == 3)
    { // Partition
        sprintf(cmd, "iptables -A OUTPUT -p tcp --dport %s -j DROP", param);
        printf(" [Partition] 已封锁端口: %s (模拟断网)\n", param);
    }
    else if (type == 4)
    { // Corrupt
        sprintf(cmd, "tc qdisc add dev %s root netem corrupt %s", nic, param);
        printf(" [Corrupt] 已注入报文损坏率: %s (设备: %s)\n", param, nic);
    }

    // 执行命令
    int ret = system(cmd);
    if (ret != 0)
        printf("  警告: 网络命令执行返回异常 (Code: %d)\n", ret);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        printf("Usage: %s <type> [param]\n", argv[0]);
        return 1;
    }
    int type = atoi(argv[1]);
    const char *param = (argc >= 3) ? argv[2] : NULL;
    inject_network(type, param);
    return 0;
}
