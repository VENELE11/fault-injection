#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/version.h>
#include <linux/seq_file.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM-FI-Team");

#define PROC_DIR "pt-update-fi"
/* 换成更稳的靶子 */
#define TARGET_FUNC "handle_mm_fault"

/*
 * VM_FAULT_SIGSEGV 通常定义在 mm_types.h 或类似头文件中
 * 我们可以手动定义一个危险值来模拟错误
 */
#define FAKE_ERROR_RET 0x4 // VM_FAULT_SIGSEGV usually

static int inject_signal = 0;
static int fault_times = 0;

static struct kprobe kp;
static struct proc_dir_entry *pdir;

/*
 * 我们利用 Kprobe 的 override 机制 (如果内核支持)
 * 或者简单地使用 kretprobe 修改返回值。
 * 为了兼容性，这里使用修改入参的方法：
 * handle_mm_fault(vma, addr, flags, regs)
 * X0=vma, X1=addr
 *
 * 如果我们将 X0 (vma) 置为 0 (NULL)，函数内部肯定会崩或者直接返回错误。
 * 但为了不让内核崩，我们最好不要给 NULL vma。
 *
 * 更安全的方式：只打日志，观察注入频率。
 * 如果要真正让应用崩，我们需要修改 X0 寄存器为错误值，并直接返回。
 * 在 ARM64 上，这需要修改 PC 到 LR。
 */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0)
    {
        /*
         * 模拟故障：
         * 我们无法安全地在 pre_handler 里直接 "return"，
         * 除非修改 PC = LR。
         *
         * 既然之前的测试没反应，我们先只打印一条强烈的日志，
         * 证明我们确实拦截到了这个函数。
         * 如果能看到日志刷屏，说明注入点选对了。
         */

        // 简单计数
        fault_times--;
        if (fault_times % 10 == 0)
        { // 防止日志太多
            printk(KERN_INFO "[PT-Fi] Intercepted handle_mm_fault! Times left: %d\n", fault_times);
        }

        if (fault_times <= 0)
        {
            inject_signal = 0;
            printk(KERN_INFO "[PT-Fi] Injection finished.\n");
        }
    }
    return 0;
}

// === Proc 操作 (读写支持) ===
static int show_int(struct seq_file *m, void *v)
{
    int *val = (int *)m->private;
    seq_printf(m, "%d\n", *val);
    return 0;
}
static int open_signal(struct inode *inode, struct file *file) { return single_open(file, show_int, &inject_signal); }
static int open_times(struct inode *inode, struct file *file) { return single_open(file, show_int, &fault_times); }

static ssize_t write_common(struct file *file, const char __user *buf, size_t count, loff_t *ppos, int *var)
{
    char kbuf[16];
    if (count > 15)
        count = 15;
    if (copy_from_user(kbuf, buf, count))
        return -EFAULT;
    kbuf[count] = 0;
    sscanf(kbuf, "%d", var);
    return count;
}
static ssize_t write_signal(struct file *file, const char __user *buf, size_t count, loff_t *ppos) { return write_common(file, buf, count, ppos, &inject_signal); }
static ssize_t write_times(struct file *file, const char __user *buf, size_t count, loff_t *ppos) { return write_common(file, buf, count, ppos, &fault_times); }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops signal_fops = {.proc_open = open_signal, .proc_read = seq_read, .proc_write = write_signal, .proc_release = single_release};
static const struct proc_ops times_fops = {.proc_open = open_times, .proc_read = seq_read, .proc_write = write_times, .proc_release = single_release};
#else
static const struct file_operations signal_fops = {.open = open_signal, .read = seq_read, .write = write_signal, .release = single_release};
static const struct file_operations times_fops = {.open = open_times, .read = seq_read, .write = write_times, .release = single_release};
#endif

static int __init my_pt_init(void)
{
    kp.symbol_name = TARGET_FUNC;
    kp.pre_handler = handler_pre;
    if (register_kprobe(&kp) < 0)
    {
        printk(KERN_ERR "[PT-Fi] Failed to register kprobe on %s\n", TARGET_FUNC);
        return -1;
    }
    pdir = proc_mkdir(PROC_DIR, NULL);
    if (pdir)
    {
        proc_create("signal", 0666, pdir, &signal_fops);
        proc_create("times", 0666, pdir, &times_fops);
    }
    printk(KERN_INFO "[PT-Fi] Loaded. Target: %s\n", TARGET_FUNC);
    return 0;
}

static void __exit my_pt_exit(void)
{
    unregister_kprobe(&kp);
    if (pdir)
    {
        remove_proc_entry("signal", pdir);
        remove_proc_entry("times", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[PT-Fi] Unloaded.\n");
}

module_init(my_pt_init);
module_exit(my_pt_exit);