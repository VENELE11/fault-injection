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
 * 64  (0x40)  : X29 (FP)
 * 128 (0x80)  : X30 (LR/Link Register)
 * 256 (0x100) : SP  (Stack Pointer)
 * 512 (0x200) : PC  (Program Counter / ELR)
 */
static void inject_fault_arm64(struct pt_regs *regs)
{
    unsigned long *reg_ptr = NULL;
    char *reg_name = "Unknown";

    if (fault_aim & 1)        { reg_ptr = &regs->regs[0]; reg_name="X0"; }
    else if (fault_aim & 2)   { reg_ptr = &regs->regs[1]; reg_name="X1"; }
    else if (fault_aim & 4)   { reg_ptr = &regs->regs[2]; reg_name="X2"; }
    else if (fault_aim & 8)   { reg_ptr = &regs->regs[3]; reg_name="X3"; }
    else if (fault_aim & 16)  { reg_ptr = &regs->regs[4]; reg_name="X4"; }
    else if (fault_aim & 32)  { reg_ptr = &regs->regs[5]; reg_name="X5"; }
    else if (fault_aim & 64)  { reg_ptr = &regs->regs[29]; reg_name="FP(X29)"; }
    else if (fault_aim & 128) { reg_ptr = &regs->regs[30]; reg_name="LR(X30)"; }
    else if (fault_aim & 256) { reg_ptr = &regs->sp;       reg_name="SP"; }
    else if (fault_aim & 512) { reg_ptr = &regs->pc;       reg_name="PC"; }

    if (reg_ptr) {
        if (fault_lasting == 0) {
            // 瞬时故障: Bit Flip (翻转低位)
            *reg_ptr ^= 0x1;
            printk(KERN_INFO "[ARM-CPU-Fi] BitFlip on %s. New Val: 0x%lx\n", reg_name, *reg_ptr);
        } else {
            // 持久故障: Stuck-at-0 (清零)
            *reg_ptr = 0;
            printk(KERN_INFO "[ARM-CPU-Fi] Stuck-at-0 on %s. New Val: 0x%lx\n", reg_name, *reg_ptr);
        }
    }
}

static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0) {
        inject_fault_arm64(regs);
        
        fault_times--;
        if (fault_times <= 0) {
            inject_signal = 0;
            printk(KERN_INFO "[ARM-CPU-Fi] Injection finished.\n");
        }
    }
    return 0;
}

// === Proc 接口 ===
static ssize_t write_signal(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[2]; 
    if(count>0 && !copy_from_user(kbuf, buf, 1)) inject_signal = (kbuf[0]=='1');
    return count;
}
static ssize_t write_times(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[16];
    if(count<16 && !copy_from_user(kbuf, buf, count)) { kbuf[count]=0; sscanf(kbuf, "%d", &fault_times); }
    return count;
}
static ssize_t write_aim(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[16];
    if(count<16 && !copy_from_user(kbuf, buf, count)) { kbuf[count]=0; sscanf(kbuf, "%d", &fault_aim); }
    return count;
}
static ssize_t write_lasting(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[16];
    if(count<16 && !copy_from_user(kbuf, buf, count)) { kbuf[count]=0; sscanf(kbuf, "%d", &fault_lasting); }
    return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops signal_fops = { .proc_write = write_signal };
static const struct proc_ops times_fops = { .proc_write = write_times };
static const struct proc_ops aim_fops   = { .proc_write = write_aim };
static const struct proc_ops last_fops  = { .proc_write = write_lasting };
#else
static const struct file_operations signal_fops = { .write = write_signal };
static const struct file_operations times_fops = { .write = write_times };
static const struct file_operations aim_fops   = { .write = write_aim };
static const struct file_operations last_fops  = { .write = write_lasting };
#endif

static int __init my_cpu_init(void)
{
    kp.symbol_name = TARGET_FUNC;
    kp.pre_handler = handler_pre;

    if (register_kprobe(&kp) < 0) {
        printk(KERN_ERR "[ARM-CPU-Fi] register_kprobe failed. Try changing TARGET_FUNC.\n");
        return -1;
    }
    printk(KERN_INFO "[ARM-CPU-Fi] Hooked '%s' for register injection.\n", TARGET_FUNC);

    pdir = proc_mkdir(PROC_DIR, NULL);
    if(pdir) {
        proc_create("signal", 0666, pdir, &signal_fops);
        proc_create("times", 0666, pdir, &times_fops);
        proc_create("aim", 0666, pdir, &aim_fops);
        proc_create("lasting", 0666, pdir, &last_fops);
    }
    return 0;
}

static void __exit my_cpu_exit(void)
{
    unregister_kprobe(&kp);
    if (pdir) {
        remove_proc_entry("signal", pdir);
        remove_proc_entry("times", pdir);
        remove_proc_entry("aim", pdir);
        remove_proc_entry("lasting", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[ARM-CPU-Fi] Unloaded.\n");
}

module_init(my_cpu_init);
module_exit(my_cpu_exit);