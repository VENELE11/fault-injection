#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/ptrace.h>
#include <linux/random.h>
#include <linux/kvm_host.h>
#include <linux/version.h>
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ARM-FI-Team");

#define PERMISSION 0666
#define MAX_LINE 256
#define TARGET_FUNC "kvm_vm_ioctl" // 或 "kvm_vcpu_ioctl"

static u64 rando;
static struct proc_dir_entry *dir = NULL;
// 各个 proc 入口
static int position = 0;   // 1=CMD, 2=ARG
static int type = 0;       // 1=Flip, 2=Set1, 3=Set0
static int time_cnt = 0;
static int signal = 0;
static int persistence = 0; // 0=Transient, 1=Persistent

static struct kprobe kp;
static char temp[MAX_LINE];

static void getrando(int n)
{
    get_random_bytes(&rando, sizeof(u64));
    rando = (((int)rando) % n + n) % n;
}

static void change_arm64(struct pt_regs *regs, int pos, int tp)
{
    unsigned long *p;
    
    // ARM64 参数: X0=fd, X1=cmd, X2=arg
    if (pos == 1) {
        // 修改 IOCTL CMD
        printk(KERN_INFO "[ARM-Res-Fi] Mutating CMD (X1) from 0x%llx", regs->regs[1]);
        p = (unsigned long *)&regs->regs[1];
    } else if (pos == 2) {
        // 修改 IOCTL ARG
        printk(KERN_INFO "[ARM-Res-Fi] Mutating ARG (X2) from 0x%llx", regs->regs[2]);
        p = (unsigned long *)&regs->regs[2];
    } else {
        return;
    }
    
    getrando(32); // 随机选一位 (0-31)
    
    if (tp == 1) {
        *p ^= (1UL << rando);   // Flip
        printk(KERN_CONT " (BitFlip bit %lld) -> 0x%llx\n", rando, *p);
    } else if (tp == 2) {
        *p |= (1UL << rando);   // Set 1
        printk(KERN_CONT " (Set1 bit %lld) -> 0x%llx\n", rando, *p);
    } else if (tp == 3) {
        *p &= ~(1UL << rando);  // Set 0
        printk(KERN_CONT " (Set0 bit %lld) -> 0x%llx\n", rando, *p);
    }
}

static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    if (signal == 0) return 0;
    
    if (time_cnt != -1) {
        if (time_cnt > 0) {
            time_cnt--;
        } else {
            signal = 0;
            printk(KERN_INFO "[ARM-Res-Fi] Injection Finished.\n");
            return 0;
        }
    }
    
    // 执行故障注入
    change_arm64(regs, position, type);
    
    return 0;
}

// === Proc Write Functions  ===
static ssize_t proc_write_common(struct file *file, const char __user *buf, size_t count, int *val)
{
    if(count <= 0 || count >= MAX_LINE) return -EFAULT;
    if(copy_from_user(temp, buf, count)) return -EFAULT;
    temp[count] = '\0';
    sscanf(temp, "%d", val);
    return count;
}

static ssize_t write_time(struct file *f, const char __user *b, size_t c, loff_t *p) { return proc_write_common(f, b, c, &time_cnt); }
static ssize_t write_pos(struct file *f, const char __user *b, size_t c, loff_t *p)  { return proc_write_common(f, b, c, &position); }
static ssize_t write_type(struct file *f, const char __user *b, size_t c, loff_t *p) { return proc_write_common(f, b, c, &type); }
static ssize_t write_sig(struct file *f, const char __user *b, size_t c, loff_t *p)  { return proc_write_common(f, b, c, &signal); }
static ssize_t write_style(struct file *f, const char __user *b, size_t c, loff_t *p){ return proc_write_common(f, b, c, &persistence); }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops time_fops = { .proc_write = write_time };
static const struct proc_ops pos_fops  = { .proc_write = write_pos };
static const struct proc_ops type_fops = { .proc_write = write_type };
static const struct proc_ops sig_fops  = { .proc_write = write_sig };
static const struct proc_ops style_fops= { .proc_write = write_style };
#else
static const struct file_operations time_fops = { .write = write_time };
static const struct file_operations pos_fops  = { .write = write_pos };
static const struct file_operations type_fops = { .write = write_type };
static const struct file_operations sig_fops  = { .write = write_sig };
static const struct file_operations style_fops= { .write = write_style };
#endif

static int __init my_res_init(void)
{
    kp.symbol_name = TARGET_FUNC;
    kp.pre_handler = handler_pre;

    if (register_kprobe(&kp) < 0) {
        printk(KERN_ERR "Register kprobe failed on %s\n", TARGET_FUNC);
        return -1;
    }
    
    dir = proc_mkdir("resource", NULL);
    if(dir) {
        proc_create("time", PERMISSION, dir, &time_fops);
        proc_create("position", PERMISSION, dir, &pos_fops);
        proc_create("type", PERMISSION, dir, &type_fops);
        proc_create("signal", PERMISSION, dir, &sig_fops);
        proc_create("style", PERMISSION, dir, &style_fops);
    }
    printk("Access-Control FI Loaded.\n");
    return 0;
}

static void __exit my_res_exit(void)
{
    unregister_kprobe(&kp);
    if(dir) {
        remove_proc_entry("time", dir);
        remove_proc_entry("position", dir);
        remove_proc_entry("type", dir);
        remove_proc_entry("signal", dir);
        remove_proc_entry("style", dir);
        remove_proc_entry("resource", NULL);
    }
    printk("Access-Control FI Unloaded.\n");
}

module_init(my_res_init);
module_exit(my_res_exit);