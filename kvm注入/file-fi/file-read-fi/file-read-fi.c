#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/version.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM-FI-Team");

#define PROC_DIR "file-read-fi"
#define TARGET_FUNC "vfs_read"

static int inject_signal = 0; // 0:Off, 1:On
static int fault_times = 0;   // 注入次数
static int fault_type = 0;    // 0:截断读取, 1:无效缓冲区

static struct kprobe kp;
static struct proc_dir_entry *pdir;

/*
 * ARM64 ABI:
 * vfs_read(file, buf, count, pos)
 * X0 = file, X1 = buf, X2 = count, X3 = pos
 */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0)
    {

        if (fault_type == 0)
        {
            // 模式0: 将 count (X2) 设为 0，模拟 EOF 或空读
            regs->regs[2] = 0;
            printk(KERN_INFO "[ARM-Fi-Read] vfs_read: Force count=0\n");
        }
        else if (fault_type == 1)
        {
            // 模式1: 将 buf (X1) 设为 NULL (0)，触发 EFAULT
            regs->regs[1] = 0;
            printk(KERN_INFO "[ARM-Fi-Read] vfs_read: Force buf=NULL\n");
        }

        fault_times--;
        if (fault_times <= 0)
        {
            inject_signal = 0;
            printk(KERN_INFO "[ARM-Fi-Read] Injection finished.\n");
        }
    }
    return 0;
}

// === Proc 文件操作 ===
static ssize_t write_signal(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[2];
    if (count > 0 && !copy_from_user(kbuf, buf, 1))
    {
        inject_signal = (kbuf[0] == '1');
    }
    return count;
}
static ssize_t write_times(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[16];
    if (count < 16 && !copy_from_user(kbuf, buf, count))
    {
        kbuf[count] = 0;
        sscanf(kbuf, "%d", &fault_times);
    }
    return count;
}
static ssize_t write_type(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[16];
    if (count < 16 && !copy_from_user(kbuf, buf, count))
    {
        kbuf[count] = 0;
        sscanf(kbuf, "%d", &fault_type);
    }
    return count;
}

// 兼容不同内核版本的 proc_ops 定义
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops signal_fops = {.proc_write = write_signal};
static const struct proc_ops times_fops = {.proc_write = write_times};
static const struct proc_ops type_fops = {.proc_write = write_type};
#else
static const struct file_operations signal_fops = {.write = write_signal};
static const struct file_operations times_fops = {.write = write_times};
static const struct file_operations type_fops = {.write = write_type};
#endif

static int __init my_detect_init(void)
{
    // 1. 注册 Kprobe
    kp.symbol_name = TARGET_FUNC;
    kp.pre_handler = handler_pre;

    if (register_kprobe(&kp) < 0)
    {
        printk(KERN_ERR "[ARM-Fi-Read] register_kprobe failed\n");
        return -1;
    }
    printk(KERN_INFO "[ARM-Fi-Read] Planted kprobe at '%s'\n", TARGET_FUNC);

    // 2. 创建 Proc 接口
    pdir = proc_mkdir(PROC_DIR, NULL);
    if (pdir)
    {
        proc_create("signal", 0666, pdir, &signal_fops);
        proc_create("times", 0666, pdir, &times_fops);
        proc_create("type", 0666, pdir, &type_fops);
    }
    return 0;
}

static void __exit my_detect_exit(void)
{
    unregister_kprobe(&kp);
    if (pdir)
    {
        remove_proc_entry("signal", pdir);
        remove_proc_entry("times", pdir);
        remove_proc_entry("type", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[ARM-Fi-Read] Module Unloaded\n");
}

module_init(my_detect_init);
module_exit(my_detect_exit);