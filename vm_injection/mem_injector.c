/*
 * mem_injector.c
 * 支持多种故障模式、位级精确操控、以及【内存特征扫描】
 * 编译：gcc -o mem_injector mem_injector.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>

// === 定义故障类型 ===
typedef enum
{
    FAULT_BIT_FLIP = 0, // 指定位翻转
    FAULT_STUCK_0,      // 指定位强制置 0
    FAULT_STUCK_1,      // 指定位强制置 1
    FAULT_BYTE_JUNK,    // 字节随机化
    FAULT_TYPE_MAX
} FaultType;

// === 定义目标内存区域 ===
typedef enum
{
    REGION_HEAP,  // 堆区
    REGION_STACK, // 栈区
    REGION_CODE,  // 代码段
    REGION_MANUAL // 手动指定地址
} TargetRegion;

// === 上下文结构体 ===
typedef struct
{
    pid_t pid;
    unsigned long addr;      // 注入地址
    FaultType type;          // 故障类型
    int target_bit;          // 针对第几位 (0-63)
    TargetRegion region;     // 目标区域
    unsigned long signature; // 要搜索的特征值
    int use_scanner;         // 是否启用扫描模式
} InjectorContext;

// ==========================================
// 模块 1: 底层 Ptrace 封装
// ==========================================

void die(const char *msg)
{
    perror(msg);
    exit(1);
}
/*发出锁请求 (PTRACE_ATTACH)。
  等待锁就绪(waitpid)。 */
void ptrace_attach(pid_t pid)
{
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) < 0)
        die("Attach failed");
    waitpid(pid, NULL, 0);
}

void ptrace_detach(pid_t pid)
{
    if (ptrace(PTRACE_DETACH, pid, NULL, NULL) < 0)
        die("Detach failed");
}
// PTRACE_PEEKDATA 读取目标进程的数据段内容
long ptrace_read(pid_t pid, unsigned long addr)
{
    errno = 0;
    long data = ptrace(PTRACE_PEEKDATA, pid, (void *)addr, NULL);
    // 在扫描过程中，可能会读到无效地址，此处不直接退出，而是交给调用者处理
    // 但为了保持原逻辑兼容，如果 errno 被设置则报错
    // 在扫描模式下，如果读失败，通常意味着页面不可读，我们可以忽略并继续
    return data;
}

void ptrace_write(pid_t pid, unsigned long addr, long data)
{
    if (ptrace(PTRACE_POKEDATA, pid, (void *)addr, (void *)data) < 0)
        die("Write memory failed");
}

// ==========================================
// 模块 2: 内存映射解析与扫描
// ==========================================

// 盲猜地址
unsigned long find_region_address_blind(pid_t pid, TargetRegion region)
{
    char map_path[64];
    char line[256];
    sprintf(map_path, "/proc/%d/maps", pid);
    FILE *fp = fopen(map_path, "r");
    if (!fp)
        die("Cannot open maps file");

    unsigned long start, end;
    char perms[5];
    char path[128];
    unsigned long found_addr = 0;

    while (fgets(line, sizeof(line), fp))
    {
        // 清空 path 避免残留
        memset(path, 0, sizeof(path));
        sscanf(line, "%lx-%lx %s %*s %*s %*s %s", &start, &end, perms, path);

        if (region == REGION_HEAP && strstr(line, "[heap]"))
        {
            found_addr = start + 0x100; // 盲猜：堆头 + 偏移
            break;
        }
        else if (region == REGION_STACK && strstr(line, "[stack]"))
        {
            found_addr = end - 0x200; // 盲猜：栈底 - 偏移
            break;
        }
        else if (region == REGION_CODE && perms[2] == 'x')
        {
            found_addr = start + 0x100;
            break;
        }
    }
    fclose(fp);

    if (found_addr == 0)
    {
        fprintf(stderr, "[-] 未找到指定区域，请确保进程正在运行。\n");
        exit(1);
    }
    return found_addr;
}

// [新功能] 扫描内存寻找特征值
// 改进版：扫描所有可读写的内存区域（包括匿名 mmap）
unsigned long scan_memory_for_pattern(pid_t pid, TargetRegion region, unsigned long signature)
{
    char map_path[64];
    char line[256];
    sprintf(map_path, "/proc/%d/maps", pid);
    FILE *fp = fopen(map_path, "r");
    if (!fp)
        die("Cannot open maps file");

    unsigned long start, end;
    char perms[5];
    char path[128];

    printf("[扫描器] 搜索特征值: 0x%lx\n", signature);
    printf("[扫描器] 扫描所有可读写内存区域...\n");

    // 遍历所有内存区域
    while (fgets(line, sizeof(line), fp))
    {
        memset(path, 0, sizeof(path));
        int fields = sscanf(line, "%lx-%lx %4s %*s %*s %*s %127s", &start, &end, perms, path);

        // 检查是否可读写 (rw)
        if (perms[0] != 'r' || perms[1] != 'w')
            continue;

        // 根据区域类型过滤
        int should_scan = 0;
        if (region == REGION_HEAP)
        {
            // 扫描: [heap] 或 匿名内存 (无路径名) 或 [anon:*]
            if (strstr(line, "[heap]") ||
                (fields < 4) || // 无路径名 = 匿名内存
                (path[0] == '\0') ||
                strstr(line, "[anon"))
            {
                should_scan = 1;
            }
        }
        else if (region == REGION_STACK)
        {
            if (strstr(line, "[stack]"))
            {
                should_scan = 1;
            }
        }

        if (!should_scan)
            continue;

        printf("[扫描] 区域: 0x%lx - 0x%lx (%s)\n", start, end,
               path[0] ? path : "anonymous");

        // 扫描这个区域
        for (unsigned long curr = start; curr < end; curr += 8)
        {
            errno = 0;
            long data = ptrace(PTRACE_PEEKDATA, pid, (void *)curr, NULL);
            if (errno != 0)
                continue;

            if ((unsigned long)data == signature)
            {
                printf("[+] 命中目标! 地址: 0x%lx (值: 0x%lx)\n", curr, (unsigned long)data);
                fclose(fp);
                return curr;
            }
        }
    }
    fclose(fp);

    fprintf(stderr, "[-] 扫描结束，未找到特征值 0x%lx\n", signature);
    fprintf(stderr, "    提示: 确认目标进程中确实存在该特征值\n");
    // 没找到则退出
    ptrace_detach(pid);
    exit(1);
}

// ==========================================
// 模块 3: 故障逻辑引擎
// ==========================================

long apply_fault_logic(long original, InjectorContext *ctx)
{
    long corrupted = original;
    unsigned long mask = 1UL << ctx->target_bit;

    printf("[逻辑层] 正在计算故障数据...\n");

    switch (ctx->type)
    {
    case FAULT_BIT_FLIP:
        corrupted = original ^ mask;
        printf("  -> 模式: Bit Flip (翻转第 %d 位)\n", ctx->target_bit);
        break;
    case FAULT_STUCK_0:
        corrupted = original & (~mask);
        printf("  -> 模式: Stuck-at-0 (第 %d 位强置为0)\n", ctx->target_bit);
        break;
    case FAULT_STUCK_1:
        corrupted = original | mask;
        printf("  -> 模式: Stuck-at-1 (第 %d 位强置为1)\n", ctx->target_bit);
        break;
    case FAULT_BYTE_JUNK:
        corrupted = (original & ~0xFF) | (rand() % 0xFF);
        printf("  -> 模式: Byte Junk (低8位随机化)\n");
        break;
    default:
        fprintf(stderr, "未知故障类型\n");
        exit(1);
    }
    return corrupted;
}

// ==========================================
// 主控制逻辑
// ==========================================
void print_help(char *prog)
{
    printf("用法: %s -p <PID> [选项]\n", prog);
    printf("选项:\n");
    printf("  -r <region>  注入区域: heap, stack (默认: heap)\n");
    printf("  -a <addr>    手动指定16进制地址 (优先级最高)\n");
    printf("  -s <sig>     [扫描模式] 指定特征值 (Hex) 自动搜索地址\n");
    printf("  -t <type>    故障类型: flip, set0, set1, byte (默认: flip)\n");
    printf("  -b <bit>     目标位数 0-63 (默认: 0)\n");
    printf("示例:\n");
    printf("  %s -p 1234 -r stack -s 0x1111111111111111 -t set0 -b 4\n", prog);
    exit(0);
}

int main(int argc, char *argv[])
{
    InjectorContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.target_bit = 0;
    ctx.region = REGION_HEAP;
    ctx.type = FAULT_BIT_FLIP;
    ctx.use_scanner = 0;
    ctx.signature = 0;

    int opt;
    int manual_addr_set = 0;

    // 解析参数
    while ((opt = getopt(argc, argv, "p:r:a:t:b:s:")) != -1)
    {
        switch (opt)
        {
        case 'p':
            ctx.pid = atoi(optarg);
            break;
        case 'b':
            ctx.target_bit = atoi(optarg);
            break;
        case 'a':
            ctx.addr = strtoul(optarg, NULL, 16);
            manual_addr_set = 1;
            ctx.region = REGION_MANUAL;
            break;
        case 's': // 新增：特征值扫描
            ctx.signature = strtoul(optarg, NULL, 16);
            ctx.use_scanner = 1;
            break;
        case 'r':
            if (strcmp(optarg, "heap") == 0)
                ctx.region = REGION_HEAP;
            else if (strcmp(optarg, "stack") == 0)
                ctx.region = REGION_STACK;
            else
            {
                fprintf(stderr, "当前仅支持 heap 或 stack 区域扫描/盲注\n");
                return 1;
            }
            break;
        case 't':
            if (strcmp(optarg, "flip") == 0)
                ctx.type = FAULT_BIT_FLIP;
            else if (strcmp(optarg, "set0") == 0)
                ctx.type = FAULT_STUCK_0;
            else if (strcmp(optarg, "set1") == 0)
                ctx.type = FAULT_STUCK_1;
            else if (strcmp(optarg, "byte") == 0)
                ctx.type = FAULT_BYTE_JUNK;
            else
            {
                fprintf(stderr, "非法类型\n");
                return 1;
            }
            break;
        default:
            print_help(argv[0]);
        }
    }

    if (ctx.pid == 0)
        print_help(argv[0]);

    srand(time(NULL));

    printf("=== 高级内存故障注入器 (Scanner Enabled) ===\n");
    printf("[*] 目标 PID: %d\n", ctx.pid);

    // ==========================================
    // 关键修改：Attach 必须移到地址计算之前！
    // ==========================================
    // 无论是读取地址内容进行校验，还是扫描内存寻找特征值，
    // 都需要 PTRACE_PEEKDATA，这要求必须先 Attach 目标进程。
    printf("[*] 正在挂起目标进程 (Attach)...\n");
    ptrace_attach(ctx.pid);

    // 1. 确定注入地址
    if (manual_addr_set)
    {
        printf("[*] 使用手动指定地址: 0x%lx\n", ctx.addr);
    }
    else if (ctx.use_scanner)
    {
        // 扫描模式
        ctx.addr = scan_memory_for_pattern(ctx.pid, ctx.region, ctx.signature);
    }
    else
    {
        // 盲猜模式 (旧逻辑)
        printf("[!] 警告：使用盲猜模式 (建议使用 -s 特征扫描)\n");
        printf("[*] 正在解析内存布局盲猜注入点...\n");
        ctx.addr = find_region_address_blind(ctx.pid, ctx.region);
    }

    printf("[*] 锁定注入地址: 0x%lx\n", ctx.addr);

    // 2. Read (读取原始值)
    long orig_data = ptrace_read(ctx.pid, ctx.addr);
    printf("[R] 读取原始数据: 0x%lx\n", orig_data);

    // 3. 计算故障值
    long bad_data = apply_fault_logic(orig_data, &ctx);

    // 4. Write (写入故障值)
    printf("[W] 写入故障数据: 0x%lx\n", bad_data);
    ptrace_write(ctx.pid, ctx.addr, bad_data);

    // 5. Detach
    ptrace_detach(ctx.pid);
    printf("[+] 注入完成，进程已恢复运行。\n");

    return 0;
}