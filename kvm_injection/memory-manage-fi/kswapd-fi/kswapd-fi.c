#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/version.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM-FI-Team");

#define PROC_DIR "kswapd-fi"
/* 
 * 目标: shrink_node
 * 這是内存回收的核心函数，kswapd 调用它来释放页面。
 * 拦截它并跳过执行（直接返回），可导致物理内存无法回收。
 */
#define TARGET_FUNC "shrink_node"

static int inject_signal = 0;
static int fault_times = 0;

static struct kprobe kp;
static struct proc_dir_entry *pdir;

/*
 * 模拟函数直接返回 (Skip Execution)
 * 警告：直接让 shrink_node 返回可能会导致返回值未初始化。
 * 大多数情况下 shrink_node 返回 void 或 unsigned long (scanned pages)。
 * 如果是 unsigned long，我们可以设返回值 X0 = 0 (表示没有回收任何页)。
 */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (inject_signal && fault_times > 0) {
        
        // 简单策略：修改参数 pgdat (X0) 为 NULL
        // 这通常会导致函数内部检查参数时直接返回，或者 Crash (取决于内核健壮性)。
        // 更安全的做法是利用 override_function，但那是高版本特性。
        
        // 我们尝试通过修改 X0 = 0 (Node Ptr) 来让它失效
        regs->regs[0] = 0; 
        printk(KERN_INFO "[ARM-Kswapd-Fi] Force shrink_node(NULL). Preventing reclaim.\n");

        fault_times--;
        if (fault_times <= 0) {
            inject_signal = 0;
            printk(KERN_INFO "[ARM-Kswapd-Fi] Injection finished.\n");
        }
    }
    return 0;
}

// === Proc 接口 ===
static ssize_t write_sig(struct file *f, const char __user *b, size_t c, loff_t *p) {
    char kbuf[2]; if(c>0 && !copy_from_user(kbuf,b,1)) inject_signal=(kbuf[0]=='1'); return c;
}
static ssize_t write_time(struct file *f, const char __user *b, size_t c, loff_t *p) {
    char kbuf[16]; if(c<16 && !copy_from_user(kbuf,b,c)) { kbuf[c]=0; sscanf(kbuf,"%d",&fault_times); } return c;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops sig_fops = { .proc_write = write_sig };
static const struct proc_ops time_fops = { .proc_write = write_time };
#else
static const struct file_operations sig_fops = { .write = write_sig };
static const struct file_operations time_fops = { .write = write_time };
#endif

static int __init my_swp_init(void)
{
    kp.symbol_name = TARGET_FUNC;
    kp.pre_handler = handler_pre;
    if (register_kprobe(&kp) < 0) {
        printk(KERN_ERR "Register KPI failed on %s\n", TARGET_FUNC);
        return -1;
    }
    pdir = proc_mkdir(PROC_DIR, NULL);
    if(pdir) {
        proc_create("signal", 0666, pdir, &sig_fops);
        proc_create("times", 0666, pdir, &time_fops);
    }
    return 0;
}

static void __exit my_swp_exit(void)
{
    unregister_kprobe(&kp);
    if(pdir) {
        remove_proc_entry("signal", pdir);
        remove_proc_entry("times", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
}
module_init(my_swp_init);
module_exit(my_swp_exit);