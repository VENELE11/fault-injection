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
/* 目标: kvm_vm_ioctl_get_dirty_log */
#define TARGET_FUNC "kvm_vm_ioctl_get_dirty_log"

static int inject_signal = 0;
static struct kretprobe rp;
static struct proc_dir_entry *pdir;

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (inject_signal) {
        // 强制返回 -EIO，中断迁移
        regs->regs[0] = -EIO;
        printk(KERN_INFO "[ARM-Mig-Fi] Blocked Dirty Log Query. Migration failed.\n");
        inject_signal = 0; // 单次触发
    }
    return 0;
}

static ssize_t write_sig(struct file *f, const char __user *b, size_t c, loff_t *p) {
    char kbuf[2]; if(c>0 && !copy_from_user(kbuf,b,1)) inject_signal=(kbuf[0]=='1'); return c;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops sig_fops = { .proc_write = write_sig };
#else
static const struct file_operations sig_fops = { .write = write_sig };
#endif

static int __init my_mig_init(void)
{
    rp.kp.symbol_name = TARGET_FUNC;
    rp.handler = ret_handler;
    if (register_kretprobe(&rp) < 0) return -1;
    pdir = proc_mkdir(PROC_DIR, NULL);
    if(pdir) proc_create("signal", 0666, pdir, &sig_fops);
    return 0;
}

static void __exit my_mig_exit(void)
{
    unregister_kretprobe(&rp);
    if(pdir) {
        remove_proc_entry("signal", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
}
module_init(my_mig_init);
module_exit(my_mig_exit);