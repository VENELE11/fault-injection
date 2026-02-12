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

#define PROC_DIR "memory-manage-fi"

/* 目标函数 */
#define TARGET_1 "kvm_set_memory_region"
#define TARGET_2 "gfn_to_hva_many" 

/* 全局控制变量 */
static int target_class = 0; // 1: kvm_set_memory_region, 2: gfn_to_hva_many
static int position = 0;     // 1: Arg1, 2: Arg2 ...
static int type = 0;         // 1: Flip, 2: Set1, 3: Set0
static int time_cnt = 0;
static int signal = 0;
static int persistence = 0;

static struct kprobe kp1, kp2;
static struct proc_dir_entry *pdir;
static char temp[256];
static u64 rando;

static void getrando(int n)
{
    get_random_bytes(&rando, sizeof(u64));
    rando = (((int)rando) % n + n) % n;
}

/* 执行寄存器修改 */
static void change_arm64(struct pt_regs *regs, int pos, int tp)
{
    unsigned long *p;
    
    // ARM64 参数: X0, X1, X2...
    // pos=1 -> X0, pos=2 -> X1
    if (pos >= 1 && pos <= 8) {
        p = (unsigned long *)&regs->regs[pos-1];
        printk(KERN_INFO "[ARM-MM-Fi] Target Arg%d (X%d) = 0x%llx ", pos, pos-1, *p);
    } else {
        return;
    }
    
    getrando(32);
    
    if (tp == 1) {
        *p ^= (1UL << rando);   // Flip
        printk(KERN_CONT "^= (1<<%lld) -> 0x%llx\n", rando, *p);
    } else if (tp == 2) {
        *p |= (1UL << rando);   // Set 1
        printk(KERN_CONT "|= (1<<%lld) -> 0x%llx\n", rando, *p);
    } else if (tp == 3) {
        *p &= ~(1UL << rando);  // Set 0
        printk(KERN_CONT "&= ~(1<<%lld) -> 0x%llx\n", rando, *p);
    }
}

/* 通用处理逻辑 */
static int handler_common(struct kprobe *p, struct pt_regs *regs, int current_class)
{
    // 如果 signal 关闭 或 class 不匹配，则忽略
    if (signal == 0 || target_class != current_class) return 0;
    
    if (time_cnt != -1) {
        if (time_cnt > 0) {
            time_cnt--;
        } else {
            signal = 0;
            printk(KERN_INFO "[ARM-MM-Fi] Injection Finished.\n");
            return 0;
        }
    }
    
    change_arm64(regs, position, type);
    return 0;
}

static int handler_pre1(struct kprobe *p, struct pt_regs *regs) { return handler_common(p, regs, 1); }
static int handler_pre2(struct kprobe *p, struct pt_regs *regs) { return handler_common(p, regs, 2); }

/* Proc Write Helpers */
static ssize_t proc_write_common(struct file *file, const char __user *buf, size_t count, int *val)
{
    if(count <= 0 || count >= 256) return -EFAULT;
    if(copy_from_user(temp, buf, count)) return -EFAULT;
    temp[count] = '\0';
    sscanf(temp, "%d", val);
    return count;
}

static ssize_t write_class(struct file *f, const char __user *b, size_t c, loff_t *p){ return proc_write_common(f, b, c, &target_class); }
static ssize_t write_time(struct file *f, const char __user *b, size_t c, loff_t *p) { return proc_write_common(f, b, c, &time_cnt); }
static ssize_t write_pos(struct file *f, const char __user *b, size_t c, loff_t *p)  { return proc_write_common(f, b, c, &position); }
static ssize_t write_type(struct file *f, const char __user *b, size_t c, loff_t *p) { return proc_write_common(f, b, c, &type); }
static ssize_t write_sig(struct file *f, const char __user *b, size_t c, loff_t *p)  { return proc_write_common(f, b, c, &signal); }
static ssize_t write_style(struct file *f, const char __user *b, size_t c, loff_t *p){ return proc_write_common(f, b, c, &persistence); }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0)
static const struct proc_ops class_fops= { .proc_write = write_class };
static const struct proc_ops time_fops = { .proc_write = write_time };
static const struct proc_ops pos_fops  = { .proc_write = write_pos };
static const struct proc_ops type_fops = { .proc_write = write_type };
static const struct proc_ops sig_fops  = { .proc_write = write_sig };
static const struct proc_ops style_fops= { .proc_write = write_style };
#else
static const struct file_operations class_fops= { .write = write_class };
static const struct file_operations time_fops = { .write = write_time };
static const struct file_operations pos_fops  = { .write = write_pos };
static const struct file_operations type_fops = { .write = write_type };
static const struct file_operations sig_fops  = { .write = write_sig };
static const struct file_operations style_fops= { .write = write_style };
#endif

static int __init my_mm_init(void)
{
    // Register KP1
    kp1.symbol_name = TARGET_1;
    kp1.pre_handler = handler_pre1;
    if (register_kprobe(&kp1) < 0) {
        printk(KERN_ERR "[ARM-MM-Fi] Failed to hook %s. (Maybe symbol not exported?)\n", TARGET_1);
        // Continue trying KP2
    } else {
        printk(KERN_INFO "[ARM-MM-Fi] Hooked %s\n", TARGET_1);
    }

    // Register KP2
    kp2.symbol_name = TARGET_2;
    kp2.pre_handler = handler_pre2;
    if (register_kprobe(&kp2) < 0) {
         printk(KERN_ERR "[ARM-MM-Fi] Failed to hook %s.\n", TARGET_2);
    } else {
        printk(KERN_INFO "[ARM-MM-Fi] Hooked %s\n", TARGET_2);
    }

    pdir = proc_mkdir(PROC_DIR, NULL);
    if(pdir) {
        proc_create("class", 0666, pdir, &class_fops);
        proc_create("time", 0666, pdir, &time_fops);
        proc_create("position", 0666, pdir, &pos_fops);
        proc_create("type", 0666, pdir, &type_fops);
        proc_create("signal", 0666, pdir, &sig_fops);
        proc_create("style", 0666, pdir, &style_fops);
    }
    return 0;
}

static void __exit my_mm_exit(void)
{
    if (kp1.symbol_name) unregister_kprobe(&kp1);
    if (kp2.symbol_name) unregister_kprobe(&kp2);
    
    if(pdir) {
        remove_proc_entry("class", pdir);
        remove_proc_entry("time", pdir);
        remove_proc_entry("position", pdir);
        remove_proc_entry("type", pdir);
        remove_proc_entry("signal", pdir);
        remove_proc_entry("style", pdir);
        remove_proc_entry(PROC_DIR, NULL);
    }
    printk(KERN_INFO "[ARM-MM-Fi] Unloaded.\n");
}

module_init(my_mm_init);
module_exit(my_mm_exit);