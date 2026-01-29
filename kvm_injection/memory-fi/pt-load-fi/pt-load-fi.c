#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/mm.h> /* for VM_FAULT_* definitions */
#include <linux/version.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM-FI-Team");

#define PROC_DIR "pt-load-fi"
/* 
 * 目标函数: handle_mm_fault
 * 它是 Linux 内存管理处理缺页异常的核心入口。
 * 拦截它并返回错误，可以模拟物理内存映射失败。
 */
#define TARGET_FUNC "handle_mm_fault"

static int inject_signal = 0; // 开关
static int fault_times = 0;   // 次数
static int fault_type = 0;    // 0: OOM (内存耗尽), 1: SIGBUS (总线错误)

/* 使用 kretprobe 来拦截函数返回 */
static struct kretprobe rp;
static struct proc_dir_entry *pdir;

// 函数入口处理 (可选，用于过滤 PID，此处略过以保持简单)
static int entry_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    return 0;
}

// 函数返回处理 (修改返回值 X0)
static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0) {
        
        // 修改 X0 寄存器 (ARM64 返回值)
        if (fault_type == 0) {
            regs->regs[0] = VM_FAULT_OOM;
            printk(KERN_INFO "[ARM-Mem-Fi] handle_mm_fault FORCE Return: VM_FAULT_OOM\n");
        } else {
            regs->regs[0] = VM_FAULT_SIGBUS;
            printk(KERN_INFO "[ARM-Mem-Fi] handle_mm_fault FORCE Return: VM_FAULT_SIGBUS\n");
        }

        fault_times--;
        if (fault_times <= 0) {
            inject_signal = 0;
            printk(KERN_INFO "[ARM-Mem-Fi] Injection finished.\n");
        }
    }
    return 0;
}

// === Proc 接口 ===
static ssize_t write_signal(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[2];
    if (count > 0 && !copy_from_user(kbuf, buf, 1)) inject_signal = (kbuf[0] == '1');
    return count;
}
static ssize_t write_times(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[16];
    if (count < 16 && !copy_from_user(kbuf, buf, count)) { kbuf[count] = 0; sscanf(kbuf, "%d", &fault_times); }
    return count;
}
static ssize_t write_type(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[16];
    if (count < 16 && !copy_from_user(kbuf, buf, count)) { kbuf[count] = 0; sscanf(kbuf, "%d", &fault_type); }
    return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops signal_fops = { .proc_write = write_signal };
static const struct proc_ops times_fops = { .proc_write = write_times };
static const struct proc_ops type_fops  = { .proc_write = write_type };
#else
static const struct file_operations signal_fops = { .write = write_signal };
static const struct file_operations times_fops = { .write = write_times };
static const struct file_operations type_fops  = { .write = write_type };
#endif

static int __init my_mem_init(void)
{
    rp.kp.symbol_name = TARGET_FUNC;
    rp.entry_handler = entry_handler;
    rp.handler = ret_handler;
    // maxactive 决定了多少个并发探测实例，设为 20 足够测试
    rp.maxactive = 20;

    if (register_kretprobe(&rp) < 0) {
        printk(KERN_ERR "[ARM-Mem-Fi] register_kretprobe failed on %s\n", TARGET_FUNC);
        return -1;
    }
    printk(KERN_INFO "[ARM-Mem-Fi] Planted kretprobe at '%s'\n", TARGET_FUNC);

    pdir = proc_mkdir(PROC_DIR, NULL);
    if(pdir) {
        proc_create("signal", 0666, pdir, &signal_fops);
        proc_create("times", 0666, pdir, &times_fops);
        proc_create("type", 0666, pdir, &type_fops);
    }
    return 0;
}

static void __exit my_mem_exit(void)
{
    unregister_kretprobe(&rp);
    if (pdir) {
        remove_proc_entry("signal", pdir);
        remove_proc_entry("times", pdir);
        remove_proc_entry("type", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[ARM-Mem-Fi] Unloaded.\n");
}

module_init(my_mem_init);
module_exit(my_mem_exit);