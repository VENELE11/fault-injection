#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/version.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM-FI-Team");

#define PROC_DIR "kvm-state-fi"
/*
 * 更换靶子: kvm_vcpu_ioctl
 * 这是一个非常高频的函数，QEMU 与 KVM 交互 90% 都走这里。
 * 拦截它并返回错误，QEMU 必定感知到。
 */
#define TARGET_FUNC "kvm_vcpu_ioctl"

static int inject_signal = 0;
static int fault_times = 0;

static struct kretprobe rp;
static struct proc_dir_entry *pdir;

/*
 * Ret-Handler:
 * 强行修改 ioctl 的返回值为 -EIO
 */
static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0)
    {

        // 修改返回值 X0 = -EIO (Input/output error)
        regs->regs[0] = -EIO;

        // 限制日志打印频率，避免死机
        if (fault_times % 10 == 0 || fault_times == 1)
        {
            printk(KERN_INFO "[ARM-State-Fi] Intercepted %s -> Force Return -EIO (Left: %d)\n",
                   TARGET_FUNC, fault_times);
        }

        fault_times--;
        if (fault_times <= 0)
        {
            inject_signal = 0;
            printk(KERN_INFO "[ARM-State-Fi] Injection finished.\n");
        }
    }
    return 0;
}

// === Proc 接口 ===
static ssize_t write_signal(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
    char kbuf[2];
    if (count > 0 && !copy_from_user(kbuf, buf, 1))
        inject_signal = (kbuf[0] == '1');
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

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops signal_fops = {.proc_write = write_signal};
static const struct proc_ops times_fops = {.proc_write = write_times};
#else
static const struct file_operations signal_fops = {.write = write_signal};
static const struct file_operations times_fops = {.write = write_times};
#endif

static int __init my_state_init(void)
{
    rp.kp.symbol_name = TARGET_FUNC;
    rp.handler = ret_handler;
    rp.maxactive = 20; // 增加并发深度

    if (register_kretprobe(&rp) < 0)
    {
        printk(KERN_ERR "[ARM-State-Fi] Failed to hook %s\n", TARGET_FUNC);
        return -1;
    }
    printk(KERN_INFO "[ARM-State-Fi] Hooked '%s'\n", TARGET_FUNC);

    pdir = proc_mkdir(PROC_DIR, NULL);
    if (pdir)
    {
        proc_create("signal", 0666, pdir, &signal_fops);
        proc_create("times", 0666, pdir, &times_fops);
    }
    return 0;
}

static void __exit my_state_exit(void)
{
    unregister_kretprobe(&rp);
    if (pdir)
    {
        remove_proc_entry("signal", pdir);
        remove_proc_entry("times", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[ARM-State-Fi] Unloaded\n");
}

module_init(my_state_init);
module_exit(my_state_exit);