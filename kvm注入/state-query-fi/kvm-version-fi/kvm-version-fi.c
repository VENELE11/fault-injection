#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/kvm.h>
#include <linux/version.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM-FI-Team");

#define PROC_DIR "kvm-version-fi"
/* 
 * 拦截函数: kvm_dev_ioctl
 * 这是处理 KVM 全局命令（如获取版本、创建 VM）的入口。
 */
#define TARGET_FUNC "kvm_dev_ioctl"

static int inject_signal = 0;
static int fault_times = 0;

static struct kretprobe rp;
static struct proc_dir_entry *pdir;

/*
 * Ret-Handler:
 * 检查返回值。如果是版本号 (通常是 12)，我们将其篡改。
 * 由于 kretprobe 拿不到 entry 时的参数，如果不配合 entry_handler 
 * 很难区分具体的 ioctl command。
 * 
 * 简化策略：只要返回值等于 KVM_API_VERSION (12)，我们就将其改为 0。
 */
#define KVM_API_VER 12

static int ret_handler(struct kretprobe_instance *ri, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0) {
        
        long ret_val = regs->regs[0]; // X0 是返回值
        
        if (ret_val == KVM_API_VER) {
            // 劫持版本号
            regs->regs[0] = 0; // Invalid Version
            printk(KERN_INFO "[ARM-Ver-Fi] Intercepted KVM Version Query. Mutated 12 -> 0\n");

            fault_times--;
            if (fault_times <= 0) {
                inject_signal = 0;
                printk(KERN_INFO "[ARM-Ver-Fi] Injection finished.\n");
            }
        }
    }
    return 0;
}

// === Proc 接口 ===
static ssize_t write_signal(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[2]; 
    if(count>0 && !copy_from_user(kbuf,buf,1)) inject_signal=(kbuf[0]=='1'); 
    return count;
}
static ssize_t write_times(struct file *file, const char __user *buf, size_t count, loff_t *ppos) {
    char kbuf[16]; 
    if(count<16 && !copy_from_user(kbuf,buf,count)) { kbuf[count]=0; sscanf(kbuf,"%d",&fault_times); } 
    return count;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops signal_fops = { .proc_write = write_signal };
static const struct proc_ops times_fops = { .proc_write = write_times };
#else
static const struct file_operations signal_fops = { .write = write_signal };
static const struct file_operations times_fops = { .write = write_times };
#endif

static int __init my_ver_init(void)
{
    rp.kp.symbol_name = TARGET_FUNC;
    rp.handler = ret_handler;
    rp.maxactive = 10;

    if (register_kretprobe(&rp) < 0) {
        printk(KERN_ERR "[ARM-Ver-Fi] Failed to hook %s\n", TARGET_FUNC);
        return -1;
    }
    printk(KERN_INFO "[ARM-Ver-Fi] Hooked '%s'\n", TARGET_FUNC);

    pdir = proc_mkdir(PROC_DIR, NULL);
    if(pdir) {
        proc_create("signal", 0666, pdir, &signal_fops);
        proc_create("times", 0666, pdir, &times_fops);
    }
    return 0;
}

static void __exit my_ver_exit(void)
{
    unregister_kretprobe(&rp);
    if(pdir) {
        remove_proc_entry("signal", pdir);
        remove_proc_entry("times", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[ARM-Ver-Fi] Unloaded\n");
}

module_init(my_ver_init);
module_exit(my_ver_exit);