#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/sched/signal.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>
#include <linux/mmu_notifier.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/file.h>
#include <linux/bug.h>
#include <linux/anon_inodes.h>
#include <linux/syscalls.h>
#include <linux/userfaultfd_k.h>
#include <linux/mempolicy.h>
#include <linux/ioctl.h>
#include <linux/security.h>
#include <linux/hugetlb.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/miscdevice.h>
#include <linux/dma-mapping.h>
#include <linux/proc_fs.h>

#include <linux/string.h>
#include <linux/pfn.h>

#include <asm/pgtable.h>

MODULE_AUTHOR("None");
MODULE_DESCRIPTION("LKP USMRM");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.011");

static const unsigned long usmMemSize = (unsigned long)9*1024*1024*1024;

static struct proc_dir_entry *proc_entry;
static struct proc_dir_entry *proc_entry_pgs;
static int ver = 0;
struct task_struct *toWake = NULL;

static dma_addr_t paddr;
static unsigned char * buf;
static volatile unsigned long index = 0;

__poll_t usm_poll(struct file *file, poll_table *wait)
{
    __poll_t ret = usm_poll_ufd(file,wait);
    if (ret)
        ver++;
    return ret;
}

static void usm_release(struct device *dev)
{
    printk(KERN_INFO "Releasing USMd\n");
}

static struct device dev = {
    .release = usm_release
};

static int usm_misc_f_release(struct inode *, struct file *)
{
    printk(KERN_INFO "Releasing USMMcd\n");
    index=0;
    return 0;
}

ssize_t usm_read(struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
    int fd = 0;
    if (ver) {
        if (count<TASK_COMM_LEN) {									// not quite needed..?
            printk(KERN_INFO "Size nah gud (%ld) #USMReadHandleUsmRM\n", count);
            goto outNope;
        }
        ver--;
        fd = handle_usm(&toWake);
        if (fd<0)
            return -EFAULT;
        if (copy_to_user((__u64 __user *) buf, &toWake->comm, TASK_COMM_LEN)) {
            printk(KERN_INFO "[mayday/usm_read] Failed copy to user!\n");
            goto outNope;
        }

        return fd;
    }
outNope:
    return -EAGAIN;
}

ssize_t usm_write(struct file *file, const char __user *buf, size_t count, loff_t *f_pos)
{
    if(toWake==NULL) {
        printk(KERN_INFO "\t[mayday/usm_write] No process in queue!\n");
        return -EAGAIN;
    }
    wake_up_process(toWake);
    return 0;
}

int usm_mmap(struct file *filp, struct vm_area_struct *vma)
{
    // unsigned long page = virt_to_phys(buf+index) >> PAGE_SHIFT;
    unsigned long page = PFN_DOWN(virt_to_phys(bus_to_virt(paddr)))+vma->vm_pgoff;
    printk("Base PFN : %lu",page);
    printk(KERN_INFO "µ%lu\t%lu #USMRM\n", vma->vm_start, vma->vm_end);
    /*if (remap_pfn_range(vma, vma->vm_start, page, (vma->vm_end - vma->vm_start),
                        vma->vm_page_prot)) {*/
    if (remap_pfn_range(vma, vma->vm_start, page, (vma->vm_end - vma->vm_start),
                        vma->vm_page_prot)) {
        printk(KERN_INFO "[mayday/usm_mmap] remap failed..\n");
        return -1;
    }
    index+=vma->vm_end - vma->vm_start;
    printk(KERN_INFO "remap_pfn_range ; cool, index : %ld.\n", index);
    printk(KERN_INFO "\tpgoff : %ld, | %ld.\n", vma->vm_pgoff, (unsigned long)vma->vm_pgoff+vma->vm_start-vma->vm_end);
    return 0;
}

static const struct proc_ops usm_fops = {	// file_operations
    .proc_poll		= usm_poll,
    .proc_read		= usm_read,
    .proc_write 	= usm_write//,
    //.proc_mmap 		= usm_mmap,
    //.proc_ioctl 	= usm_ioctl,
    //.proc_release 	= usm_release		// TODO imp.!		...proc_open mndty?
};

static struct file_operations cma_malloc_fileops = {
    .mmap           =   usm_mmap,
    .release        =   usm_misc_f_release
};

static struct miscdevice usm_miscdevice = {
    .minor           =   MISC_DYNAMIC_MINOR,
    .name            =   "USMMcd",
    .fops            =   &cma_malloc_fileops,
    .mode            =   0777
};

static DECLARE_WAIT_QUEUE_HEAD(usm_pagesWait);
static int verPs = 0;
__poll_t usm_poll_pages(struct file *file, poll_table *wait)
{
    __poll_t ret = 0;
    poll_wait(file, &usm_pagesWait, wait);
    spin_lock(&usmPagesLock);
    if (usmPagesCount>0) {
        verPs++;
        ret=POLLIN;
    }
    spin_unlock(&usmPagesLock);
    return ret;
}

ssize_t usm_read_page(struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
    unsigned long pfn = 0;
    struct page * page;
    if (verPs) {
        if (count<sizeof(pfn)) {
            printk(KERN_INFO "Size nah gud (%ld) #USMReadHandleUsmRMPGs\n", count);
            goto outNope;
        }
        verPs--;
        pfn = handle_page();
        page = pfn_to_page(pfn);
        if (page) {
            //memset(buf+(pfn-index)*4096, 0, 4096);
            //void * toZero=kmap(page);
            //memset(toZero, 0, 4096);                                                        /* Should / shall be done somewhere else */
            //kunmap(page);
            //printk("Passed\n");
            atomic_set(&(page)->_mapcount, -1);
            atomic_set(&(page)->_refcount, 1);
            set_bit(PG_usm, &page->flags);                                                     // it is lost somewhere.. TODO ..investigate it srsly..
            page->page_type = PG_usm;
            //pg->mapping=NULL;
            //pg->page_type = PG_usm;
            //set_bit(PG_uptodate, &page->flags);
            /*page->memcg_data=0;
            page->flags=0;
            page->private=0;*/
            //flush_dcache_page(page);
            //ClearPageHWPoisonTakenOff(page);

            /* if (pfn-index==0)
                printk("\tGud!\n"); */
            //__page_cache_release(page);
            /*
            ClearPageActive(page);
            ClearPageReferenced(page);
            */
            //ClearPageSwapBacked(page);
        } else {  // ifdef TODO
            printk("[mayday/usm_read_page] No page linked to fetched PFN : %lu.\n", pfn);
            return -EFAULT;
        }
        if (copy_to_user((__u64 __user *) buf, &pfn, sizeof(pfn))) {
            printk(KERN_INFO "[mayday/usm_read_page] Failed copy to user!\n");
            goto outNope;
        }

        return count;
    }
outNope:
    return -EAGAIN;
}
static const struct proc_ops usm_pages_fops = {
    .proc_poll		= usm_poll_pages,
    .proc_read		= usm_read_page
};

static int __init hwusm_lkm_init(void)
{
    int ret = misc_register(&usm_miscdevice);
    if (unlikely(ret)) {
        printk("Misc. dev. reg. failed : %d.\n", ret);
        return -EAGAIN;
    } else {
        dev = *usm_miscdevice.this_device;
        dev.dma_mask=DMA_BIT_MASK(64);
        dev.coherent_dma_mask=DMA_BIT_MASK(64);
    }
    proc_entry=proc_create("usm", 0777, NULL, &usm_fops);
    if (!proc_entry) {
        printk(KERN_INFO "Processes' USM tagging file operations not created!\n");
        misc_deregister(&usm_miscdevice);
        return -EAGAIN;
    }
    proc_entry_pgs=proc_create("usmPgs", 0777, NULL, &usm_pages_fops);
    if (!proc_entry_pgs) {
        printk(KERN_INFO "USM pages' freeing file operations not created!\n");
        proc_remove(proc_entry);
        misc_deregister(&usm_miscdevice);
        return -EAGAIN;
    }
    buf = dma_alloc_coherent(&dev, usmMemSize, &paddr, GFP_KERNEL);
    if(!buf) {
        printk(KERN_WARNING "[Mayday] Unable to allocate USM's CM pool!\n");
        proc_remove(proc_entry);
        proc_remove(proc_entry_pgs);
        misc_deregister(&usm_miscdevice);
        return -EAGAIN;
    }
    printk(KERN_INFO "Yo! #USMRM\n");
    return 0;
}

static void __exit hwusm_lkm_exit(void)
{
    printk(KERN_INFO "Ciaossu! #USMRM\n");
    dma_free_coherent(usm_miscdevice.this_device,usmMemSize,buf,paddr);
    proc_remove(proc_entry);
    proc_remove(proc_entry_pgs);
    misc_deregister(&usm_miscdevice);
}

module_init(hwusm_lkm_init);
module_exit(hwusm_lkm_exit);
