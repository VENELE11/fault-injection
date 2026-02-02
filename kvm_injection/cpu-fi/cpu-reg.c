#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <asm/ptrace.h>
#include <linux/version.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM-FI-Team");

/* /proc/cpu-general-fi */
#define PROC_DIR "cpu-general-fi"
/*
 * 目标函数: kernel_clone (旧版内核为 _do_fork)
 * 这是进程创建的核心函数，参数丰富，适合测试寄存器修改效果
 */
#define TARGET_FUNC "kernel_clone"

static int inject_signal = 0; // 注入开关
static int fault_times = 0;   // 注入次数
static int fault_aim = 0;     // 目标寄存器掩码
static int fault_lasting = 0; // 0:瞬时(BitFlip), 1:持久(SetZero) - 简化逻辑

static struct kprobe kp;
static struct proc_dir_entry *pdir;

/*
 * Bitmask 映射 (ARM64):
 * 1   (0x1)   : X0
 * 2   (0x2)   : X1
 * 4   (0x4)   : X2
 * 8   (0x8)   : X3
 * 16  (0x10)  : X4
 * 32  (0x20)  : X5
 * 64  (0x40)  : FP (X29)
 * 128 (0x80)  : LR (X30)
 * 256 (0x100) : SP
 * 512 (0x200) : PC
 */

static void inject_fault_arm64(struct pt_regs *regs)
{
    // 修改类型为 u64* 以匹配 ARM64 的 struct pt_regs 定义
    u64 *reg_ptr = NULL;
    char *reg_name = "Unknown";
    u64 rando;

    // 根据掩码选择目标寄存器
    if (fault_aim & 1)
    {
        reg_ptr = (u64 *)&regs->regs[0];
        reg_name = "X0";
    }
    else if (fault_aim & 2)
    {
        reg_ptr = (u64 *)&regs->regs[1];
        reg_name = "X1";
    }
    else if (fault_aim & 4)
    {
        reg_ptr = (u64 *)&regs->regs[2];
        reg_name = "X2";
    }
    else if (fault_aim & 8)
    {
        reg_ptr = (u64 *)&regs->regs[3];
        reg_name = "X3";
    }
    else if (fault_aim & 16)
    {
        reg_ptr = (u64 *)&regs->regs[4];
        reg_name = "X4";
    }
    else if (fault_aim & 32)
    {
        reg_ptr = (u64 *)&regs->regs[5];
        reg_name = "X5";
    }
    else if (fault_aim & 64)
    {
        reg_ptr = (u64 *)&regs->regs[29];
        reg_name = "FP(X29)";
    }
    else if (fault_aim & 128)
    {
        reg_ptr = (u64 *)&regs->regs[30];
        reg_name = "LR(X30)";
    }
    else if (fault_aim & 256)
    {
        reg_ptr = (u64 *)&regs->sp;
        reg_name = "SP";
    }
    else if (fault_aim & 512)
    {
        reg_ptr = (u64 *)&regs->pc;
        reg_name = "PC";
    }

    if (reg_ptr)
    {
        if (fault_lasting == 0)
        { // Bit Flip
            get_random_bytes(&rando, sizeof(u64));
            rando %= 64; // 0-63
            *reg_ptr ^= (1ULL << rando);
            printk(KERN_INFO "[CPU-Fi] Injected BitFlip at %s, bit %lld, val now: 0x%llx\n",
                   reg_name, rando, *reg_ptr);
        }
        else
        { // Set Zero (Persistent-like)
            *reg_ptr = 0;
            printk(KERN_INFO "[CPU-Fi] Injected SetZero at %s\n", reg_name);
        }
    }
}

static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (inject_signal == 1 && fault_times > 0)
    {
        inject_fault_arm64(regs);
        fault_times--;
        if (fault_times <= 0)
        {
            inject_signal = 0;
            printk(KERN_INFO "[CPU-Fi] Injection Finished.\n");
        }
    }
    return 0;
}

// === Proc 文件操作 ===
#define DEFINE_PROC_WRITE(name, var)                                                                   \
    static ssize_t write_##name(struct file *file, const char __user *buf, size_t count, loff_t *ppos) \
    {                                                                                                  \
        char buffer[16];                                                                               \
        int val;                                                                                       \
        if (count > sizeof(buffer) - 1)                                                                \
            count = sizeof(buffer) - 1;                                                                \
        if (copy_from_user(buffer, buf, count))                                                        \
            return -EFAULT;                                                                            \
        buffer[count] = '\0';                                                                          \
        if (kstrtoint(buffer, 10, &val) == 0)                                                          \
            var = val;                                                                                 \
        return count;                                                                                  \
    }

DEFINE_PROC_WRITE(signal, inject_signal)
DEFINE_PROC_WRITE(times, fault_times)
DEFINE_PROC_WRITE(aim, fault_aim)
DEFINE_PROC_WRITE(lasting, fault_lasting)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops sig_fops = {.proc_write = write_signal};
static const struct proc_ops times_fops = {.proc_write = write_times};
static const struct proc_ops aim_fops = {.proc_write = write_aim};
static const struct proc_ops last_fops = {.proc_write = write_lasting};
#else
static const struct file_operations sig_fops = {.write = write_signal};
static const struct file_operations times_fops = {.write = write_times};
static const struct file_operations aim_fops = {.write = write_aim};
static const struct file_operations last_fops = {.write = write_lasting};
#endif

static int __init my_cpu_init(void)
{
    // 注册 kprobe
    kp.symbol_name = TARGET_FUNC;
    kp.pre_handler = handler_pre;
    if (register_kprobe(&kp) < 0)
    {
        printk(KERN_ERR "[CPU-Fi] Failed to register kprobe on %s\n", TARGET_FUNC);
        return -1;
    }

    // 创建 proc 文件
    pdir = proc_mkdir(PROC_DIR, NULL);
    if (pdir)
    {
        proc_create("signal", 0666, pdir, &sig_fops);
        proc_create("times", 0666, pdir, &times_fops);
        proc_create("aim", 0666, pdir, &aim_fops);
        proc_create("lasting", 0666, pdir, &last_fops);
    }

    printk(KERN_INFO "[CPU-Fi] Module Loaded.\n");
    return 0;
}

static void __exit my_cpu_exit(void)
{
    unregister_kprobe(&kp);
    if (pdir)
    {
        remove_proc_entry("signal", pdir);
        remove_proc_entry("times", pdir);
        remove_proc_entry("aim", pdir);
        remove_proc_entry("lasting", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[CPU-Fi] Module Unloaded.\n");
}

module_init(my_cpu_init);
module_exit(my_cpu_exit);