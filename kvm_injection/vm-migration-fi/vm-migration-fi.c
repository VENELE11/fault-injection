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

#define PROC_DIR "vm-migration-fi"
/* 修改目标为 kvm_vm_ioctl，因为它必定存在 */
#define TARGET_FUNC "kvm_vm_ioctl"

static int inject_signal = 0;
static struct kretprobe rp;
static struct proc_dir_entry *pdir;

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (inject_signal)
    {
        /* 返回 -EIO，这将导致用户态 QEMU 收到错误并在迁移相关操作中失败 */
        regs->regs[0] = -EIO;
        printk(KERN_INFO "[ARM-Mig-Fi] Intercepted kvm_vm_ioctl. Forcing -EIO to block migration.\n");
        // 为了防止误杀其他操作，触发一次后立即重置
        inject_signal = 0;
    }
    return 0;
}

static ssize_t write_sig(struct file *f, const char __user *b, size_t c, loff_t *p)
{
    char kbuf[2];
    if (c > 0 && !copy_from_user(kbuf, b, 1))
    {
        if (kbuf[0] == '1')
        {
            inject_signal = 1;
            printk(KERN_INFO "[ARM-Mig-Fi] Fault injection ARMED.\n");
        }
    }
    return c;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops sig_fops = {.proc_write = write_sig};
#else
static const struct file_operations sig_fops = {.write = write_sig};
#endif

static int __init my_mig_init(void)
{
    int ret;
    rp.kp.symbol_name = TARGET_FUNC;
    rp.handler = ret_handler;

    ret = register_kretprobe(&rp);
    if (ret < 0)
    {
        printk(KERN_ERR "[ARM-Mig-Fi] Failed to register kretprobe on %s, error: %d\n", TARGET_FUNC, ret);
        return ret; // 返回真实的错误码
    }

    pdir = proc_mkdir(PROC_DIR, NULL);
    if (pdir)
        proc_create("signal", 0666, pdir, &sig_fops);
    printk(KERN_INFO "[ARM-Mig-Fi] Module loaded. Target: %s\n", TARGET_FUNC);
    return 0;
}

static void __exit my_mig_exit(void)
{
    unregister_kretprobe(&rp);
    if (pdir)
    {
        remove_proc_entry("signal", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[ARM-Mig-Fi] Module unloaded.\n");
}

module_init(my_mig_init);
module_exit(my_mig_exit);