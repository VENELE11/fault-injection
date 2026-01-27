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

// 查找目标进程 PID
int get_vm_pid(const char *proc_name)
{
    char cmd[128];
    char output[32];
    snprintf(cmd, sizeof(cmd), "pgrep -f %s | head -n 1", proc_name);

    FILE *fp = popen(cmd, "r");
    if (fp != NULL && fgets(output, sizeof(output), fp) != NULL)
    {
        pclose(fp);
        return atoi(output);
    }
    if (fp)
        pclose(fp);
    return -1;
}

void inject_process(const char *target, int action_type)
{
    int pid = get_vm_pid(target);
    if (pid == -1)
    {
        printf("❌ [错误] 未找到进程: %s\n", target);
        exit(1);
    }

    if (action_type == 1)
    { // Crash
        kill(pid, SIGKILL);
        printf("💥 [Crash] 已杀死进程 (PID: %d)\n", pid);
    }
    else if (action_type == 2)
    { // Hang
        kill(pid, SIGSTOP);
        printf("❄️  [Hang] 已暂停进程 (PID: %d)\n", pid);
    }
    else if (action_type == 3)
    { // Resume
        kill(pid, SIGCONT);
        printf("▶️  [Resume] 已恢复进程 (PID: %d)\n", pid);
    }
    else
    {
        printf("❌ 未知操作类型\n");
    }
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        printf("Usage: %s <process_name> <action_type 1|2|3>\n", argv[0]);
        return 1;
    }
    inject_process(argv[1], atoi(argv[2]));
    return 0;
}
