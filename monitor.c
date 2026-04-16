// monitor.c
// Kernel module: container memory monitor
// Creates /dev/container_monitor with ioctl interface
// Maintains a linked list of PIDs, checks RSS every 2 seconds,
// prints soft-limit warnings via printk, sends SIGKILL on hard limit.
//
// Tested on: Ubuntu 22.04 / 24.04, kernel 5.15+ and 6.x

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include <linux/version.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS-Jackfruit");
MODULE_DESCRIPTION("Container memory monitor with soft/hard limits");
MODULE_VERSION("1.0");

#define DEVICE_NAME       "container_monitor"
#define CLASS_NAME        "container_mon"
#define CHECK_INTERVAL_MS 2000

/* per-PID record in linked list */
struct pid_entry {
    struct list_head list;
    pid_t            pid;
    long             soft_limit_bytes;
    long             hard_limit_bytes;
    bool             soft_warned;
};

static LIST_HEAD(pid_list);
static DEFINE_MUTEX(pid_list_lock);

/* char device */
static int            major_num;
static struct class  *mon_class  = NULL;
static struct device *mon_device = NULL;
static struct cdev    mon_cdev;
static dev_t          dev_num;

/* periodic timer */
static struct timer_list rss_timer;

/* ------------------------------------------------------------------
 * Helper: get RSS in bytes for a given host PID. Returns -1 if gone.
 * ------------------------------------------------------------------ */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss = -1;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    mm = get_task_mm(task);
    rcu_read_unlock();

    if (mm) {
        rss = (long)get_mm_rss(mm) << PAGE_SHIFT;
        mmput(mm);
    }
    return rss;
}

/* Helper: send SIGKILL to a host PID */
static void kill_pid_entry(pid_t pid)
{
    struct pid *p;
    rcu_read_lock();
    p = find_vpid(pid);
    if (p)
        kill_pid(p, SIGKILL, 1);
    rcu_read_unlock();
}

/* ------------------------------------------------------------------
 * Timer callback: check RSS for every registered PID
 * ------------------------------------------------------------------ */
static void rss_check_timer(struct timer_list *t)
{
    struct pid_entry *entry, *tmp;

    mutex_lock(&pid_list_lock);
    list_for_each_entry_safe(entry, tmp, &pid_list, list) {
        long rss = get_rss_bytes(entry->pid);

        if (rss < 0) {
            pr_info("container_monitor: pid %d gone, removing\n", entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->hard_limit_bytes > 0 && rss > entry->hard_limit_bytes) {
            pr_warn("container_monitor: pid %d RSS %ld bytes exceeds HARD limit %ld bytes -- sending SIGKILL\n",
                    entry->pid, rss, entry->hard_limit_bytes);
            kill_pid_entry(entry->pid);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        if (entry->soft_limit_bytes > 0 && rss > entry->soft_limit_bytes
                && !entry->soft_warned) {
            pr_warn("container_monitor: pid %d RSS %ld bytes exceeds SOFT limit %ld bytes -- WARNING\n",
                    entry->pid, rss, entry->soft_limit_bytes);
            entry->soft_warned = true;
        }
    }
    mutex_unlock(&pid_list_lock);

    mod_timer(&rss_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));
}

/* ------------------------------------------------------------------
 * File operations
 * ------------------------------------------------------------------ */
static int mon_open(struct inode *inode, struct file *filp)
{
    pr_info("container_monitor: opened by pid %d\n", current->pid);
    return 0;
}

static int mon_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static long mon_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct monitor_cmd kcmd;
    struct pid_entry *entry, *tmp;
    int found;

    if (copy_from_user(&kcmd, (void __user *)arg, sizeof(kcmd)))
        return -EFAULT;

    switch (cmd) {

    case MONITOR_REGISTER:
        mutex_lock(&pid_list_lock);
        found = 0;
        list_for_each_entry(entry, &pid_list, list) {
            if (entry->pid == kcmd.pid) {
                entry->soft_limit_bytes = kcmd.soft_limit_bytes;
                entry->hard_limit_bytes = kcmd.hard_limit_bytes;
                entry->soft_warned = false;
                found = 1;
                break;
            }
        }
        if (!found) {
            entry = kmalloc(sizeof(*entry), GFP_KERNEL);
            if (!entry) {
                mutex_unlock(&pid_list_lock);
                return -ENOMEM;
            }
            INIT_LIST_HEAD(&entry->list);
            entry->pid              = kcmd.pid;
            entry->soft_limit_bytes = kcmd.soft_limit_bytes;
            entry->hard_limit_bytes = kcmd.hard_limit_bytes;
            entry->soft_warned      = false;
            list_add_tail(&entry->list, &pid_list);
            pr_info("container_monitor: registered pid %d soft=%ld hard=%ld\n",
                    kcmd.pid, kcmd.soft_limit_bytes, kcmd.hard_limit_bytes);
        }
        mutex_unlock(&pid_list_lock);
        return 0;

    case MONITOR_UNREGISTER:
        mutex_lock(&pid_list_lock);
        list_for_each_entry_safe(entry, tmp, &pid_list, list) {
            if (entry->pid == kcmd.pid) {
                list_del(&entry->list);
                kfree(entry);
                pr_info("container_monitor: unregistered pid %d\n", kcmd.pid);
                break;
            }
        }
        mutex_unlock(&pid_list_lock);
        return 0;

    case MONITOR_QUERY:
        kcmd.rss_kb      = 0;
        kcmd.kill_reason = 0;
        mutex_lock(&pid_list_lock);
        list_for_each_entry(entry, &pid_list, list) {
            if (entry->pid == kcmd.pid) {
                long rss = get_rss_bytes(kcmd.pid);
                kcmd.rss_kb = (rss > 0) ? rss / 1024 : 0;
                kcmd.kill_reason = entry->soft_warned ? 1 : 0;
                break;
            }
        }
        mutex_unlock(&pid_list_lock);
        if (copy_to_user((void __user *)arg, &kcmd, sizeof(kcmd)))
            return -EFAULT;
        return 0;

    default:
        return -ENOTTY;
    }
}

static const struct file_operations mon_fops = {
    .owner          = THIS_MODULE,
    .open           = mon_open,
    .release        = mon_release,
    .unlocked_ioctl = mon_ioctl,
};

/* ------------------------------------------------------------------
 * Module init/exit
 * ------------------------------------------------------------------ */
static int __init monitor_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("container_monitor: alloc_chrdev_region failed: %d\n", ret);
        return ret;
    }
    major_num = MAJOR(dev_num);

    cdev_init(&mon_cdev, &mon_fops);
    mon_cdev.owner = THIS_MODULE;
    ret = cdev_add(&mon_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("container_monitor: cdev_add failed: %d\n", ret);
        goto err_cdev;
    }

    /* class_create API changed in kernel 6.4 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    mon_class = class_create(CLASS_NAME);
#else
    mon_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(mon_class)) {
        ret = PTR_ERR(mon_class);
        pr_err("container_monitor: class_create failed: %d\n", ret);
        goto err_class;
    }

    mon_device = device_create(mon_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(mon_device)) {
        ret = PTR_ERR(mon_device);
        pr_err("container_monitor: device_create failed: %d\n", ret);
        goto err_device;
    }

    timer_setup(&rss_timer, rss_check_timer, 0);
    mod_timer(&rss_timer, jiffies + msecs_to_jiffies(CHECK_INTERVAL_MS));

    pr_info("container_monitor: loaded, /dev/%s ready\n", DEVICE_NAME);
    return 0;

err_device:
    class_destroy(mon_class);
err_class:
    cdev_del(&mon_cdev);
err_cdev:
    unregister_chrdev_region(dev_num, 1);
    return ret;
}

static void __exit monitor_exit(void)
{
    struct pid_entry *entry, *tmp;

    del_timer_sync(&rss_timer);

    mutex_lock(&pid_list_lock);
    list_for_each_entry_safe(entry, tmp, &pid_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    mutex_unlock(&pid_list_lock);

    device_destroy(mon_class, dev_num);
    class_destroy(mon_class);
    cdev_del(&mon_cdev);
    unregister_chrdev_region(dev_num, 1);

    pr_info("container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
