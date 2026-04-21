/*
 * target_net.c - 网络故障注入测试靶场
 * 测试: network_injector (延迟/丢包/损坏/封锁)
 * 编译: gcc -o target_net target_net.c -lpthread
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define TCP_PORT 8088
#define UDP_PORT 9999

volatile int keep_running = 1;

// 统计数据
typedef struct
{
    int tcp_sent;
    int tcp_recv;
    int tcp_timeout;
    int udp_sent;
    int udp_recv;
    int udp_timeout;
    int udp_corrupt;
    double tcp_rtt_sum;
    double tcp_rtt_max;
    double udp_rtt_sum;
} NetStats;

NetStats stats = {0};
pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
double tcp_baseline = -1;
double udp_baseline = -1;

void sig_handler(int sig)
{
    printf("\n[退出]\n");
    keep_running = 0;
}

// ==================== TCP 服务器 ====================
void *tcp_server(void *arg)
{
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0)
    {
        perror("tcp socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(TCP_PORT)};

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("tcp bind");
        close(sfd);
        return NULL;
    }
    listen(sfd, 10);

    struct timeval tv = {1, 0};
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("[TCP服务] 启动 :%d\n", TCP_PORT);

    while (keep_running)
    {
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        int cfd = accept(sfd, (struct sockaddr *)&cli, &len);
        if (cfd >= 0)
        {
            char buf[256] = {0};
            recv(cfd, buf, sizeof(buf) - 1, 0);
            // 回显时间戳
            struct timeval now;
            gettimeofday(&now, NULL);
            char resp[64];
            snprintf(resp, sizeof(resp), "OK:%ld.%06ld\n", now.tv_sec, now.tv_usec);
            send(cfd, resp, strlen(resp), 0);
            close(cfd);
        }
    }
    close(sfd);
    return NULL;
}

// ==================== UDP 服务器 ====================
void *udp_server(void *arg)
{
    int sfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sfd < 0)
    {
        perror("udp socket");
        return NULL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(UDP_PORT)};

    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("udp bind");
        close(sfd);
        return NULL;
    }

    struct timeval tv = {1, 0};
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    printf("[UDP服务] 启动 :%d\n", UDP_PORT);

    while (keep_running)
    {
        char buf[512];
        struct sockaddr_in cli;
        socklen_t len = sizeof(cli);
        ssize_t n = recvfrom(sfd, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &len);
        if (n > 0)
        {
            // 原样回显
            sendto(sfd, buf, n, 0, (struct sockaddr *)&cli, len);
        }
    }
    close(sfd);
    return NULL;
}

// ==================== TCP 探测 (测延迟) ====================
void *tcp_prober(void *arg)
{
    sleep(1);
    printf("[TCP探测] 启动 -> 127.0.0.1:%d\n", TCP_PORT);

    while (keep_running)
    {
        struct timeval t1, t2;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            sleep(1);
            continue;
        }

        struct timeval to = {3, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));

        struct sockaddr_in sa = {
            .sin_family = AF_INET,
            .sin_port = htons(TCP_PORT)};
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

        gettimeofday(&t1, NULL);

        pthread_mutex_lock(&stats_lock);
        stats.tcp_sent++;
        pthread_mutex_unlock(&stats_lock);

        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0)
        {
            send(sock, "PING", 4, 0);
            char buf[64];
            ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
            gettimeofday(&t2, NULL);

            if (n > 0)
            {
                double rtt = (t2.tv_sec - t1.tv_sec) * 1000.0 +
                             (t2.tv_usec - t1.tv_usec) / 1000.0;

                pthread_mutex_lock(&stats_lock);
                stats.tcp_recv++;
                stats.tcp_rtt_sum += rtt;
                if (rtt > stats.tcp_rtt_max)
                    stats.tcp_rtt_max = rtt;
                pthread_mutex_unlock(&stats_lock);

                // 基线
                if (tcp_baseline < 0)
                {
                    tcp_baseline = rtt;
                    printf("\033[32m[TCP] 基线 RTT: %.2f ms\033[0m\n", tcp_baseline);
                }
                else
                {
                    double ratio = rtt / tcp_baseline;
                    if (ratio > 100)
                    {
                        printf("\033[31m[TCP] #### RTT=%.0fms (%.0fx) 极端延迟!\033[0m\n", rtt, ratio);
                    }
                    else if (ratio > 10)
                    {
                        printf("\033[31m[TCP] ###  RTT=%.0fms (%.0fx) 严重延迟\033[0m\n", rtt, ratio);
                    }
                    else if (ratio > 3)
                    {
                        printf("\033[33m[TCP] ##   RTT=%.1fms (%.1fx) 延迟升高\033[0m\n", rtt, ratio);
                    }
                    else if (ratio > 1.5)
                    {
                        printf("\033[36m[TCP] #    RTT=%.1fms\033[0m\n", rtt);
                    }
                    // 正常不打印
                }
            }
            else
            {
                pthread_mutex_lock(&stats_lock);
                stats.tcp_timeout++;
                pthread_mutex_unlock(&stats_lock);
                printf("\033[33m[TCP] 接收超时\033[0m\n");
            }
        }
        else
        {
            gettimeofday(&t2, NULL);
            double elapsed = (t2.tv_sec - t1.tv_sec) * 1000.0 +
                             (t2.tv_usec - t1.tv_usec) / 1000.0;

            pthread_mutex_lock(&stats_lock);
            stats.tcp_timeout++;
            pthread_mutex_unlock(&stats_lock);

            if (elapsed > 2000)
            {
                printf("\033[31m[TCP] #### 连接超时 (%.0fms) - 端口可能被封锁\033[0m\n", elapsed);
            }
            else
            {
                printf("\033[33m[TCP] 连接失败: %s\033[0m\n", strerror(errno));
            }
        }
        close(sock);
        sleep(1);
    }
    return NULL;
}

// ==================== UDP 探测 (测丢包+损坏) ====================
void *udp_prober(void *arg)
{
    sleep(1);
    printf("[UDP探测] 启动 -> 127.0.0.1:%d\n", UDP_PORT);
    printf("----------------------------------------\n");
    printf("  * 延迟注入 -> RTT 升高\n");
    printf("  * 丢包注入 -> 响应超时\n");
    printf("  * 损坏注入 -> 校验失败\n");
    printf("----------------------------------------\n\n");

    uint32_t seq = 0;

    while (keep_running)
    {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0)
        {
            sleep(1);
            continue;
        }

        struct timeval to = {2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));

        struct sockaddr_in sa = {
            .sin_family = AF_INET,
            .sin_port = htons(UDP_PORT)};
        inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

        // 构造带校验的数据包
        // [SEQ:4][DATA:16][CHECKSUM:4] = 24字节
        seq++;
        uint8_t packet[24];
        memcpy(packet, &seq, 4);
        snprintf((char *)packet + 4, 16, "UDP_PROBE_%05u", seq);

        // 计算校验和
        uint32_t cksum = 0;
        for (int i = 0; i < 20; i++)
        {
            cksum += packet[i];
        }
        memcpy(packet + 20, &cksum, 4);

        struct timeval t1, t2;
        gettimeofday(&t1, NULL);

        sendto(sock, packet, 24, 0, (struct sockaddr *)&sa, sizeof(sa));

        pthread_mutex_lock(&stats_lock);
        stats.udp_sent++;
        pthread_mutex_unlock(&stats_lock);

        uint8_t rbuf[64];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(sock, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&from, &flen);

        gettimeofday(&t2, NULL);
        double rtt = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;

        if (n == 24)
        {
            pthread_mutex_lock(&stats_lock);
            stats.udp_recv++;
            stats.udp_rtt_sum += rtt;
            pthread_mutex_unlock(&stats_lock);

            // 验证校验和
            uint32_t recv_cksum = 0;
            for (int i = 0; i < 20; i++)
            {
                recv_cksum += rbuf[i];
            }
            uint32_t stored_cksum;
            memcpy(&stored_cksum, rbuf + 20, 4);

            if (recv_cksum != stored_cksum)
            {
                pthread_mutex_lock(&stats_lock);
                stats.udp_corrupt++;
                pthread_mutex_unlock(&stats_lock);

                printf("\033[35m[UDP] #### 报文损坏! seq=%u\033[0m\n", seq);
                printf("      校验期望: %u, 实际: %u\n", stored_cksum, recv_cksum);
            }
            else
            {
                // 基线
                if (udp_baseline < 0)
                {
                    udp_baseline = rtt;
                    printf("\033[32m[UDP] 基线 RTT: %.2f ms\033[0m\n", udp_baseline);
                }
                else
                {
                    double ratio = rtt / udp_baseline;
                    if (ratio > 10)
                    {
                        printf("\033[33m[UDP] ##   RTT=%.1fms (%.1fx)\033[0m\n", rtt, ratio);
                    }
                }
            }
        }
        else if (n < 0)
        {
            pthread_mutex_lock(&stats_lock);
            stats.udp_timeout++;
            pthread_mutex_unlock(&stats_lock);

            double loss_rate = stats.udp_sent > 0 ? (double)(stats.udp_sent - stats.udp_recv) / stats.udp_sent * 100 : 0;

            if (loss_rate > 30)
            {
                printf("\033[31m[UDP] #### 超时! 丢包率: %.1f%%\033[0m\n", loss_rate);
            }
            else if (loss_rate > 10)
            {
                printf("\033[33m[UDP] ###  超时 (丢包率: %.1f%%)\033[0m\n", loss_rate);
            }
            else
            {
                printf("\033[33m[UDP] 响应超时 seq=%u\033[0m\n", seq);
            }
        }

        close(sock);
        usleep(500000); // 500ms
    }
    return NULL;
}

// ==================== 统计显示 ====================
void *stats_display(void *arg)
{
    sleep(5);

    while (keep_running)
    {
        sleep(10);

        pthread_mutex_lock(&stats_lock);
        double tcp_loss = stats.tcp_sent > 0 ? (double)stats.tcp_timeout / stats.tcp_sent * 100 : 0;
        double tcp_avg_rtt = stats.tcp_recv > 0 ? stats.tcp_rtt_sum / stats.tcp_recv : 0;
        double udp_loss = stats.udp_sent > 0 ? (double)(stats.udp_sent - stats.udp_recv) / stats.udp_sent * 100 : 0;
        double udp_corrupt_rate = stats.udp_recv > 0 ? (double)stats.udp_corrupt / stats.udp_recv * 100 : 0;

        printf("\n");
        printf("+================================================+\n");
        printf("|              网络状态统计                      |\n");
        printf("+================================================+\n");
        printf("| TCP: 发送=%d 成功=%d 超时=%d              \n",
               stats.tcp_sent, stats.tcp_recv, stats.tcp_timeout);
        printf("|      平均RTT=%.1fms 最大RTT=%.1fms 丢包=%.1f%%\n",
               tcp_avg_rtt, stats.tcp_rtt_max, tcp_loss);
        printf("+------------------------------------------------+\n");
        printf("| UDP: 发送=%d 接收=%d 超时=%d 损坏=%d      \n",
               stats.udp_sent, stats.udp_recv, stats.udp_timeout, stats.udp_corrupt);
        printf("|      丢包率=%.1f%% 损坏率=%.1f%%                \n",
               udp_loss, udp_corrupt_rate);
        printf("+================================================+\n\n");
        pthread_mutex_unlock(&stats_lock);
    }
    return NULL;
}

int main()
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("\n");
    printf("+===================================================+\n");
    printf("|        网络故障注入测试靶场                       |\n");
    printf("+===================================================+\n");
    printf("|  PID: %-6d                                      |\n", getpid());
    printf("|  TCP端口: %d | UDP端口: %d                      |\n", TCP_PORT, UDP_PORT);
    printf("+===================================================+\n");
    printf("|  测试方法:                                        |\n");
    printf("|  1. 延迟: ./network_injector 1 500ms              |\n");
    printf("|     效果: TCP/UDP RTT 升高                        |\n");
    printf("|                                                   |\n");
    printf("|  2. 丢包: ./network_injector 2 30%%                |\n");
    printf("|     效果: UDP 超时增多，丢包率上升                |\n");
    printf("|                                                   |\n");
    printf("|  3. 封锁: ./network_injector 3 8088               |\n");
    printf("|     效果: TCP 连接失败                            |\n");
    printf("|                                                   |\n");
    printf("|  4. 损坏: ./network_injector 4 20%%                |\n");
    printf("|     效果: UDP 校验失败，损坏率上升                |\n");
    printf("|                                                   |\n");
    printf("|  清除: ./network_injector 0                       |\n");
    printf("+===================================================+\n\n");

    pthread_t th_tcp_srv, th_udp_srv, th_tcp_probe, th_udp_probe, th_stats;

    pthread_create(&th_tcp_srv, NULL, tcp_server, NULL);
    pthread_create(&th_udp_srv, NULL, udp_server, NULL);
    pthread_create(&th_tcp_probe, NULL, tcp_prober, NULL);
    pthread_create(&th_udp_probe, NULL, udp_prober, NULL);
    pthread_create(&th_stats, NULL, stats_display, NULL);

    while (keep_running)
        sleep(1);

    pthread_join(th_tcp_srv, NULL);
    pthread_join(th_udp_srv, NULL);
    pthread_join(th_tcp_probe, NULL);
    pthread_join(th_udp_probe, NULL);
    pthread_join(th_stats, NULL);

    printf("[Main] 结束\n");
    return 0;
}
