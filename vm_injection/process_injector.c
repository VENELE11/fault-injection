/*
 * process_injector.c - 进程故障注入工具
 * 功能：Crash, Hang, Resume
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <ctype.h>
#include <dirent.h>

static int is_numeric(const char *value)
{
    if (value == NULL || *value == '\0')
        return 0;
    for (const char *p = value; *p; p++)
    {
        if (!isdigit((unsigned char)*p))
            return 0;
    }
    return 1;
}

static int read_cmdline(pid_t pid, char *buf, size_t size)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return 0;

    size_t n = fread(buf, 1, size - 1, fp);
    fclose(fp);
    if (n == 0)
        return 0;

    buf[n] = '\0';
    for (size_t i = 0; i < n; i++)
    {
        if (buf[i] == '\0')
            buf[i] = ' ';
    }
    return 1;
}

// 查找目标进程 PID。支持直接传 PID，并避免匹配到注入器自己的命令行。
int get_vm_pid(const char *proc_name)
{
    if (is_numeric(proc_name))
        return atoi(proc_name);

    DIR *dir = opendir("/proc");
    if (dir == NULL)
        return -1;

    pid_t self = getpid();
    pid_t parent = getppid();
    struct dirent *entry;
    char cmdline[4096];

    while ((entry = readdir(dir)) != NULL)
    {
        if (!is_numeric(entry->d_name))
            continue;

        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0 || pid == self || pid == parent)
            continue;

        if (!read_cmdline(pid, cmdline, sizeof(cmdline)))
            continue;

        if (strstr(cmdline, "process_injector") != NULL)
            continue;

        if (strstr(cmdline, proc_name) != NULL)
        {
            closedir(dir);
            return (int)pid;
        }
    }

    closedir(dir);
    return -1;
}

int inject_process(const char *target, int action_type)
{
    int pid = get_vm_pid(target);
    if (pid == -1)
    {
        printf(" [错误] 未找到进程: %s\n", target);
        return 1;
    }

    if (action_type == 1)
    { // Crash
        if (kill(pid, SIGKILL) != 0)
        {
            perror("kill(SIGKILL)");
            return 1;
        }
        printf(" [Crash] 已杀死进程 (PID: %d)\n", pid);
    }
    else if (action_type == 2)
    { // Hang
        if (kill(pid, SIGSTOP) != 0)
        {
            perror("kill(SIGSTOP)");
            return 1;
        }
        printf("  [Hang] 已暂停进程 (PID: %d)\n", pid);
    }
    else if (action_type == 3)
    { // Resume
        if (kill(pid, SIGCONT) != 0)
        {
            perror("kill(SIGCONT)");
            return 1;
        }
        printf("  [Resume] 已恢复进程 (PID: %d)\n", pid);
    }
    else
    {
        printf(" 未知操作类型\n");
        return 1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("Usage: %s <process_name> <action_type 1|2|3>\n", argv[0]);
        return 1;
    }
    return inject_process(argv[1], atoi(argv[2]));
}
