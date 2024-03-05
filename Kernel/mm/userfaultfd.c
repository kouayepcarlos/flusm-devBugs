// SPDX-License-Identifier: GPL-2.0-only
/*
 *  mm/userfaultfd.c
 *
 *  Copyright (C) 2015  Red Hat, Inc.
 */

#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/userfaultfd_k.h>
#include <linux/mmu_notifier.h>
#include <linux/hugetlb.h>
#include <linux/shmem_fs.h>
#include <asm/tlbflush.h>
#include "internal.h"

static __always_inline
struct vm_area_struct *find_dst_vma(struct mm_struct *dst_mm,
				    unsigned long dst_start,
				    unsigned long len)
{
	/*
	 * Make sure that the dst range is both valid and fully within a
	 * single existing vma.
	 */
	struct vm_area_struct *dst_vma;

	dst_vma = find_vma(dst_mm, dst_start);
	if (!dst_vma)
		return NULL;

	if (dst_start < dst_vma->vm_start ||
	    dst_start + len > dst_vma->vm_end)
		return NULL;

	/*
	 * Check the vma is registered in uffd, this is required to
	 * enforce the VM_MAYWRITE check done at uffd registration
	 * time.
	 */
	if (!dst_vma->vm_userfaultfd_ctx.ctx)
		return NULL;

	return dst_vma;
}

/*
 * Install PTEs, to map dst_addr (within dst_vma) to page.
 *
 * This function handles both MCOPY_ATOMIC_NORMAL and _CONTINUE for both shmem
 * and anon, and for both shared and private VMAs.
 */
int mfill_atomic_install_pte(struct mm_struct *dst_mm, pmd_t *dst_pmd,
			     struct vm_area_struct *dst_vma,
			     unsigned long dst_addr, struct page *page,
			     bool newly_allocated, bool wp_copy)
{
	int ret;
	pte_t _dst_pte, *dst_pte;
	bool writable = dst_vma->vm_flags & VM_WRITE;
	bool vm_shared = dst_vma->vm_flags & VM_SHARED;
	bool page_in_cache = page->mapping;
	spinlock_t *ptl;
	struct inode *inode;
	pgoff_t offset, max_off;

	_dst_pte = mk_pte(page, dst_vma->vm_page_prot);
	_dst_pte = pte_mkdirty(_dst_pte);
	if (page_in_cache && !vm_shared)
		writable = false;

	/*
	 * Always mark a PTE as write-protected when needed, regardless of
	 * VM_WRITE, which the user might change.
	 */
	if (wp_copy)
		_dst_pte = pte_mkuffd_wp(_dst_pte);
	else if (writable)
		_dst_pte = pte_mkwrite(_dst_pte);

	dst_pte = pte_offset_map_lock(dst_mm, dst_pmd, dst_addr, &ptl);

	if (vma_is_shmem(dst_vma)) {
		/* serialize against truncate with the page table lock */
		inode = dst_vma->vm_file->f_inode;
		offset = linear_page_index(dst_vma, dst_addr);
		max_off = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
		ret = -EFAULT;
		if (unlikely(offset >= max_off))
			goto out_unlock;
	}

	ret = -EEXIST;
	if (!pte_none(*dst_pte))
		goto out_unlock;

	if (page_in_cache) {
		/* Usually, cache pages are already added to LRU */
		if (newly_allocated)
			lru_cache_add(page);
		page_add_file_rmap(page, dst_vma, false);
	} else {
		page_add_new_anon_rmap(page, dst_vma, dst_addr, false);
		lru_cache_add_inactive_or_unevictable(page, dst_vma);
	}

	/*
	 * Must happen after rmap, as mm_counter() checks mapping (via
	 * PageAnon()), which is set by __page_set_anon_rmap().
	 */
	inc_mm_counter(dst_mm, mm_counter(page));

	set_pte_at(dst_mm, dst_addr, dst_pte, _dst_pte);

	/* No need to invalidate - it was non-present before */
	update_mmu_cache(dst_vma, dst_addr, dst_pte);
	ret = 0;
out_unlock:
	pte_unmap_unlock(dst_pte, ptl);
	return ret;
}

static int mcopy_atomic_pte(struct mm_struct *dst_mm,
			    pmd_t *dst_pmd,
			    struct vm_area_struct *dst_vma,
			    unsigned long dst_addr,
			    unsigned long src_addr,
			    struct page **pagep,
			    bool wp_copy)
{
	void *page_kaddr;
	int ret;
	struct page *page;

	if (!*pagep) {
		ret = -ENOMEM;
		page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, dst_vma, dst_addr);
		if (!page)
			goto out;

		page_kaddr = kmap_atomic(page);
		ret = copy_from_user(page_kaddr,
				     (const void __user *) src_addr,
				     PAGE_SIZE);
		kunmap_atomic(page_kaddr);

		/* fallback to copy_from_user outside mmap_lock */
		if (unlikely(ret)) {
			ret = -ENOENT;
			*pagep = page;
			/* don't free the page */
			goto out;
		}

		flush_dcache_page(page);
	} else {
		page = *pagep;
		*pagep = NULL;
	}

	/*
	 * The memory barrier inside __SetPageUptodate makes sure that
	 * preceding stores to the page contents become visible before
	 * the set_pte_at() write.
	 */
	__SetPageUptodate(page);

	ret = -ENOMEM;
	if (mem_cgroup_charge(page_folio(page), dst_mm, GFP_KERNEL))
		goto out_release;

	ret = mfill_atomic_install_pte(dst_mm, dst_pmd, dst_vma, dst_addr,
				       page, true, wp_copy);
	if (ret)
		goto out_release;
out:
	return ret;
out_release:
	put_page(page);
	goto out;
}

static int mfill_zeropage_pte(struct mm_struct *dst_mm,
			      pmd_t *dst_pmd,
			      struct vm_area_struct *dst_vma,
			      unsigned long dst_addr)
{
	pte_t _dst_pte, *dst_pte;
	spinlock_t *ptl;
	int ret;
	pgoff_t offset, max_off;
	struct inode *inode;

	_dst_pte = pte_mkspecial(pfn_pte(my_zero_pfn(dst_addr),
					 dst_vma->vm_page_prot));
	dst_pte = pte_offset_map_lock(dst_mm, dst_pmd, dst_addr, &ptl);
	if (dst_vma->vm_file) {
		/* the shmem MAP_PRIVATE case requires checking the i_size */
		inode = dst_vma->vm_file->f_inode;
		offset = linear_page_index(dst_vma, dst_addr);
		max_off = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
		ret = -EFAULT;
		if (unlikely(offset >= max_off))
			goto out_unlock;
	}
	ret = -EEXIST;
	if (!pte_none(*dst_pte))
		goto out_unlock;
	set_pte_at(dst_mm, dst_addr, dst_pte, _dst_pte);
	/* No need to invalidate - it was non-present before */
	update_mmu_cache(dst_vma, dst_addr, dst_pte);
	ret = 0;
out_unlock:
	pte_unmap_unlock(dst_pte, ptl);
	return ret;
}

int uffd_eClr(struct mm_struct * mmp, unsigned long addr) {
	pgd_t *a_pgd;
	p4d_t *a_p4d;
	pud_t *a_pud;
	pmd_t *a_pmd;
	pte_t *a_pte;
	
	struct vm_area_struct *_vma;
	
	spinlock_t *ptl;
	
	_vma = find_vma(mmp, addr);
	
	a_pgd = pgd_offset(mmp, addr);
	if (pgd_none(*a_pgd) || pgd_bad(*a_pgd))
	    return -ENOENT;
	    
	a_p4d = p4d_offset(a_pgd, addr);
	if (p4d_none(*a_p4d) || p4d_bad(*a_p4d))
	    return -ENOENT;
	
	a_pud = pud_offset(a_p4d, addr);
	if (pud_none(*a_pud) || pud_bad(*a_pud))
	    return -ENOENT;

	a_pmd = pmd_offset(a_pud, addr);
	if (pmd_none(*a_pmd) || pmd_bad(*a_pmd))
	    return -ENOENT;
 	
 	a_pte = pte_offset_map_lock(mmp, a_pmd, addr, &ptl);
	if (!pte_present(*a_pte)) {
	    pte_unmap_unlock(a_pte, ptl);
	    return -EINVAL;
	}
	
	pte_clear(mmp, addr, a_pte);
	flush_tlb_page(_vma, addr);
	update_mmu_cache(_vma, addr, a_pte);
	pte_unmap_unlock(a_pte, ptl);

	return 1;
}

int uffd_eClrr(struct mm_struct * mmp, unsigned long addr, unsigned long len) {

	for (int i = 0; i < (len/PAGE_SIZE); i++) {
			if(unlikely(uffd_eClr(mmp,addr+i*4096)!=1))
				return -EAGAIN;
		}

	return 1;
}

int uffd_eChk(struct mm_struct * mmp, unsigned long addr) {
	pgd_t *a_pgd;
	p4d_t *a_p4d;
	pud_t *a_pud;
	pmd_t *a_pmd;
	pte_t *a_pte;

	a_pgd = pgd_offset(mmp, addr);
	if (pgd_none(*a_pgd) || pgd_bad(*a_pgd)) {
	    printk("PUD entry not present\n");
	    return -ENOENT;
	}
	    
	a_p4d = p4d_offset(a_pgd, addr);
	if (p4d_none(*a_p4d) || p4d_bad(*a_p4d)) {
	    printk("P4D entry not present\n");
	    return -ENOENT;
	}

	a_pud = pud_offset(a_p4d, addr);
	if (pud_none(*a_pud) || pud_bad(*a_pud)){
	    printk("PUD entry not present\n");
	    return -ENOENT;
	}

	a_pmd = pmd_offset(a_pud, addr);
	if (pmd_none(*a_pmd) || pmd_bad(*a_pmd)){
	    printk("PMD entry not present\n");
	    return -ENOENT;
	}
	
	a_pte = pte_offset_map(a_pmd, addr);
	if (!pte_present(*a_pte))
	    return -ENOENT;
	pte_unmap(a_pte);
	
	return 1;
}

unsigned long uffd_eVal(struct mm_struct * mmp, unsigned long addr, unsigned long nb) {
	pgd_t *a_pgd;
	p4d_t *a_p4d;
	pud_t *a_pud;
	pmd_t *a_pmd;
	pte_t *a_pte;
	
	spinlock_t *ptl;
	struct page *pg;
	
	unsigned long pfn;
	
	struct vm_area_struct *dst_vma;
	dst_vma = find_vma(mmp, addr);
	
	a_pgd = pgd_offset(mmp, addr);
	if (pgd_none(*a_pgd) || pgd_bad(*a_pgd))
	    return 0;
	    
	a_p4d = p4d_offset(a_pgd, addr);
	if (p4d_none(*a_p4d) || p4d_bad(*a_p4d))
	    return 0;
	
	a_pud = pud_offset(a_p4d, addr);
	if (pud_none(*a_pud) || pud_bad(*a_pud))
	    return 0;

	a_pmd = pmd_offset(a_pud, addr);
	if (pmd_none(*a_pmd) || pmd_bad(*a_pmd))
	    return 0;
 
	a_pte=pte_offset_map_lock(mmp, a_pmd, addr, &ptl);
	if (!pte_present(*a_pte)) {
	    pte_unmap_unlock(a_pte, ptl);
	    return 0;
	}
	
	printk(KERN_INFO "PTE val. %lu", pte_val(*a_pte));
	printk(KERN_INFO "| %lu", ((*a_pte).pte)&PAGE_MASK);
	
	pg=pte_page(*a_pte);
	printk(KERN_INFO "| %lu\n", (page_to_pfn(pg) << PAGE_SHIFT));
	printk(KERN_INFO "| %lu\n", pte_val(mk_pte(pg, dst_vma->vm_page_prot)));
	printk(KERN_INFO "| %lu\n", ((pte_val(*a_pte) & PAGE_MASK) | pgprot_val(dst_vma->vm_page_prot)));

	/* TODO mem_cgroup_uncharge&co #pgmap->ops->page_free */
	if (pg) {
			printk("Init mapcount : %d\n", atomic_read(&(pg)->_mapcount));
			//atomic_add_negative(-1, &page[i]._mapcount)
	        //atomic_set(&(pg)->_mapcount, atomic_read(&(pg)->_mapcount)+1);
			//atomic_set(&(pg)->_mapcount, 0);
			printk("G. mapcount : %d\n", atomic_read(&(pg)->_mapcount));
			printk("Init refcount : %d\n", atomic_read(&(pg)->_refcount));
			//atomic_set(&(pg)->_refcount, atomic_read(&(pg)->_refcount)+1);
			//atomic_set(&(pg)->_refcount, 0);
			printk("G. refcount : %d\n", atomic_read(&(pg)->_refcount));
			atomic_set(&(pg)->_mapcount, -1);
			atomic_set(&(pg)->_refcount, 1);
			//pg->mapping=NULL;
			pg->page_type = PG_usm;
			set_bit(PG_usm, &pg->flags);
			if(test_bit(PG_slab, &pg->flags)) {
				printk("PG_slab %d\n", PG_slab);
				clear_bit(PG_slab, &pg->flags);
			}
			if(test_bit(PG_owner_priv_1, &pg->flags)) {
				printk("PG_owner_priv_1 %d\n", PG_owner_priv_1);
				clear_bit(PG_owner_priv_1, &pg->flags);
			}
			if(test_bit(PG_private, &pg->flags)) {
				printk("PG_private %d\n", PG_private);
				clear_bit(PG_private, &pg->flags);
			}
			if(test_bit(PG_private_2, &pg->flags)) {
				printk("PG_private_2 %d\n", PG_private_2);
				clear_bit(PG_private_2, &pg->flags);
			}
			if(test_bit(PG_head, &pg->flags)) {
				printk("PG_head %d\n", PG_head);
				clear_bit(PG_head, &pg->flags);
			}
			if(test_bit(PG_writeback, &pg->flags)) {
				printk("PG_writeback %d\n", PG_writeback);
				clear_bit(PG_writeback, &pg->flags);
			}
			if(test_bit(PG_arch_1, &pg->flags)) {
				printk("PG_arch_1 %d\n", PG_arch_1);
				clear_bit(PG_arch_1, &pg->flags);
			}
			if(test_bit(PG_swapbacked, &pg->flags)) {
				printk("PG_swapbacked %d\n", PG_swapbacked);
				clear_bit(PG_swapbacked, &pg->flags);
			}
			if(test_bit(PG_reclaim, &pg->flags)) {
				printk("PG_reclaim %d\n", PG_reclaim);
				clear_bit(PG_reclaim, &pg->flags);
			}																		/* Idea'd be to do "this" for every freed* page */
			printk(KERN_INFO "Page : %x", pg);
			printk(KERN_INFO "CHPage : %x", compound_head(pg));
	}
	else
	    printk(KERN_ERR "[USM/eVal] Page nope");  /* TODO : treat case */
	pfn = pte_pfn(*a_pte);
	for(int i=1; i<nb; i++) {
		pg=pfn_to_page(pfn+i);
		if (pg) {
			//atomic_set(&(pg)->_mapcount, atomic_read(&(pg)->_mapcount)+1);
	        //atomic_set(&(pg)->_refcount, atomic_read(&(pg)->_refcount)+1);
			//pg->mapping=NULL;
			atomic_set(&(pg)->_mapcount, -1);
			atomic_set(&(pg)->_refcount, 1);
			set_bit(PG_usm, &pg->flags);
			pg->page_type = PG_usm;												// TODO : investigate this
			if(test_bit(PG_slab, &pg->flags)) {
				printk("OFg\n");
				clear_bit(PG_slab, &pg->flags);
			}
			else if(PageSlab(pg))
					printk("Huge liar!\n");
			if(test_bit(PG_owner_priv_1, &pg->flags)) {
				printk("OOFg\n");
				clear_bit(PG_owner_priv_1, &pg->flags);
			}
			if(test_bit(PG_private, &pg->flags)) {
				printk("OOOFg\n");
				clear_bit(PG_private, &pg->flags);
			}
			if(test_bit(PG_private_2, &pg->flags)) {
				printk("BFg\n");
				clear_bit(PG_private_2, &pg->flags);
			}
			if(test_bit(PG_head, &pg->flags)) {
				printk("CFg\n");
				clear_bit(PG_head, &pg->flags);
			}
			if(test_bit(PG_writeback, &pg->flags)) {
				printk("VFg\n");
				clear_bit(PG_writeback, &pg->flags);
			}
			if(test_bit(PG_arch_1, &pg->flags)) {
				printk("DFg\n");
				clear_bit(PG_arch_1, &pg->flags);
			}
			if(test_bit(PG_swapbacked, &pg->flags)) {
				printk("MFg\n");
				clear_bit(PG_swapbacked, &pg->flags);
			}
			if(test_bit(PG_reclaim, &pg->flags)) {
				printk("PFg\n");
				clear_bit(PG_reclaim, &pg->flags);
			}
			//pg->index=indx;
			//indx+=4096;
		}
		else
			printk(KERN_WARNING "[USM/eVal] Page nope #Tagging, %d", i);
	}
	pte_unmap_unlock(a_pte, ptl);	/* TODO : get PFN just once, and before unlocking... */
	printk(KERN_INFO "^^ %lu\n", pfn);
	return pfn;
}

int uffd_eMod(struct mm_struct * mmp, unsigned long addr, unsigned long uaddr) {
	pgd_t *a_pgd;
	p4d_t *a_p4d;
	pud_t *a_pud;
	pmd_t *a_pmd;
	pte_t *a_pte;
	
	struct vm_area_struct *dst_vma;
	struct page *pg;
	spinlock_t *ptl;

	struct anon_vma * ffs;

	dst_vma = find_vma(mmp, addr);
	
	a_pgd = pgd_offset(mmp, addr);
	if (unlikely(pgd_none(*a_pgd) || pgd_bad(*a_pgd))) {
	    printk("PGD entry not present... aborting.\n");
	    return -EINVAL;
	}
	a_p4d = p4d_offset(a_pgd, addr);
	if (unlikely(p4d_none(*a_p4d) || p4d_bad(*a_p4d))) {
	    printk("\tP4D entry not present, creating it..\n");
	    a_p4d = p4d_alloc(mmp, a_pgd, addr);
	    if (!a_p4d) {
	    	printk("P4D entry creation failed, aborting.\n");
	    	return -ENOMEM;
	    }
	}
	
	a_pud = pud_offset(a_p4d, addr);
	if (unlikely(pud_none(*a_pud) || pud_bad(*a_pud))) {
	    printk("\tPUD entry not present, creating it..\n");
	    a_pud = pud_alloc(mmp, a_p4d, addr);
	    if (!a_pud) {
	    	printk("PUD entry creation failed, aborting.\n");
	    	return -ENOMEM;
	    }
	}

	a_pmd = pmd_offset(a_pud, addr);
	if (unlikely(pmd_none(*a_pmd) || pmd_bad(*a_pmd))) {
	    printk("\tPMD entry not present, creating it..\n");
	    a_pmd = pmd_alloc(mmp, a_pud, addr);
	    if (!a_pmd || __pte_alloc(mmp, a_pmd)) {
	    	printk("PMD entry creation failed, aborting.\n");
	    	return -ENOMEM;
	    }
	}
	
	a_pte = pte_offset_map_lock(mmp, a_pmd, addr, &ptl);
	
	pg=pfn_to_page(uaddr);
	if (!pg)
		printk(KERN_ALERT "[USM/eMod] Page not present...");  /* TODO : treat case */
	if(dst_vma->vm_flags & VM_SHARED) {
		//printk(KERN_INFO "Shared case\n");
		shmem_usm(mmp, dst_vma, addr, pg);
	}
	else {
		if(dst_vma->vm_file==NULL) {			/* TODO vm_ops ver. */
			//printk(KERN_INFO "Anon.!\n");
			ffs=(void *)dst_vma->anon_vma + PAGE_MAPPING_ANON;
			//WRITE_ONCE(pg->mapping,(struct address_space *)((void *)(dst_vma->anon_vma + PAGE_MAPPING_ANON)));
			WRITE_ONCE(pg->mapping,(struct address_space *)ffs);
			/*if (pg->mapping != (struct address_space*)ffs)
				printk("IOUHDQSJONDKQJS\n");*/
			//pg->index=linear_page_index(dst_vma,addr);
			//if (PageAnon(pg))
				;//printk("Gud...\n");
			//else
				//printk("Wut!?.. | 1NA \n");
		}
		else {
			printk(KERN_INFO "Non anon.!\n");
			if (dst_vma->vm_file->f_inode->i_mapping != NULL) {
				WRITE_ONCE(pg->mapping,dst_vma->vm_file->f_inode->i_mapping);//f_mapping; //f_entry->d_inode->i_mapping;
				printk(KERN_INFO "Host: %lu\n", pg->mapping->host);
			}
			else
				printk(KERN_INFO "NULL it IS!\n");
		}
	}
	/* TODO : check them page flags, man.... high duplicate probability */
	//pg->index=linear_page_index(dst_vma,addr);				/* all these in.. freaking.. cool if... */
	WRITE_ONCE(pg->index,linear_page_index(dst_vma,addr));
	//printk(KERN_INFO "LNPI : %lu!", linear_page_index(dst_vma,addr));
	inc_mm_counter(mmp, mm_counter(pg));
	
	*a_pte=pfn_pte(uaddr,dst_vma->vm_page_prot);
	*a_pte=pte_mkwrite(*a_pte);
	*a_pte=pte_mkdirty(*a_pte);

        update_mmu_cache(dst_vma, addr, a_pte);
        /* TODO : TLB update & ANON_PAGE case : minor fault, space saving... although perf. worth thinking about */
        //printk(KERN_INFO "Transformed : %lu\n", uaddr);
        //printk(KERN_INFO "Given : %lu", (*a_pte).pte);
	pte_unmap_unlock(a_pte, ptl);
	// __flush_tlb_all();		/* Specific case &/| Clr/Clrr */
	return 1;
}

/* Handles UFFDIO_CONTINUE for all shmem VMAs (shared or private). */
static int mcontinue_atomic_pte(struct mm_struct *dst_mm,
				pmd_t *dst_pmd,
				struct vm_area_struct *dst_vma,
				unsigned long dst_addr,
				bool wp_copy)
{
	struct inode *inode = file_inode(dst_vma->vm_file);
	pgoff_t pgoff = linear_page_index(dst_vma, dst_addr);
	struct page *page;
	int ret;

	ret = shmem_getpage(inode, pgoff, &page, SGP_READ);
	if (ret)
		goto out;
	if (!page) {
		ret = -EFAULT;
		goto out;
	}

	if (PageHWPoison(page)) {
		ret = -EIO;
		goto out_release;
	}

	ret = mfill_atomic_install_pte(dst_mm, dst_pmd, dst_vma, dst_addr,
				       page, false, wp_copy);
	if (ret)
		goto out_release;

	unlock_page(page);
	ret = 0;
out:
	return ret;
out_release:
	unlock_page(page);
	put_page(page);
	goto out;
}

static pmd_t *mm_alloc_pmd(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;

	pgd = pgd_offset(mm, address);
	p4d = p4d_alloc(mm, pgd, address);
	if (!p4d)
		return NULL;
	pud = pud_alloc(mm, p4d, address);
	if (!pud)
		return NULL;
	/*
	 * Note that we didn't run this because the pmd was
	 * missing, the *pmd may be already established and in
	 * turn it may also be a trans_huge_pmd.
	 */
	return pmd_alloc(mm, pud, address);
}

#ifdef CONFIG_HUGETLB_PAGE
/*
 * __mcopy_atomic processing for HUGETLB vmas.  Note that this routine is
 * called with mmap_lock held, it will release mmap_lock before returning.
 */
static __always_inline ssize_t __mcopy_atomic_hugetlb(struct mm_struct *dst_mm,
					      struct vm_area_struct *dst_vma,
					      unsigned long dst_start,
					      unsigned long src_start,
					      unsigned long len,
					      enum mcopy_atomic_mode mode)
{
	int vm_shared = dst_vma->vm_flags & VM_SHARED;
	ssize_t err;
	pte_t *dst_pte;
	unsigned long src_addr, dst_addr;
	long copied;
	struct page *page;
	unsigned long vma_hpagesize;
	pgoff_t idx;
	u32 hash;
	struct address_space *mapping;

	/*
	 * There is no default zero huge page for all huge page sizes as
	 * supported by hugetlb.  A PMD_SIZE huge pages may exist as used
	 * by THP.  Since we can not reliably insert a zero page, this
	 * feature is not supported.
	 */
	if (mode == MCOPY_ATOMIC_ZEROPAGE) {
		mmap_read_unlock(dst_mm);
		return -EINVAL;
	}

	src_addr = src_start;
	dst_addr = dst_start;
	copied = 0;
	page = NULL;
	vma_hpagesize = vma_kernel_pagesize(dst_vma);

	/*
	 * Validate alignment based on huge page size
	 */
	err = -EINVAL;
	if (dst_start & (vma_hpagesize - 1) || len & (vma_hpagesize - 1))
		goto out_unlock;

retry:
	/*
	 * On routine entry dst_vma is set.  If we had to drop mmap_lock and
	 * retry, dst_vma will be set to NULL and we must lookup again.
	 */
	if (!dst_vma) {
		err = -ENOENT;
		dst_vma = find_dst_vma(dst_mm, dst_start, len);
		if (!dst_vma || !is_vm_hugetlb_page(dst_vma))
			goto out_unlock;

		err = -EINVAL;
		if (vma_hpagesize != vma_kernel_pagesize(dst_vma))
			goto out_unlock;

		vm_shared = dst_vma->vm_flags & VM_SHARED;
	}

	/*
	 * If not shared, ensure the dst_vma has a anon_vma.
	 */
	err = -ENOMEM;
	if (!vm_shared) {
		if (unlikely(anon_vma_prepare(dst_vma)))
			goto out_unlock;
	}

	while (src_addr < src_start + len) {
		BUG_ON(dst_addr >= dst_start + len);

		/*
		 * Serialize via i_mmap_rwsem and hugetlb_fault_mutex.
		 * i_mmap_rwsem ensures the dst_pte remains valid even
		 * in the case of shared pmds.  fault mutex prevents
		 * races with other faulting threads.
		 */
		mapping = dst_vma->vm_file->f_mapping;
		i_mmap_lock_read(mapping);
		idx = linear_page_index(dst_vma, dst_addr);
		hash = hugetlb_fault_mutex_hash(mapping, idx);
		mutex_lock(&hugetlb_fault_mutex_table[hash]);

		err = -ENOMEM;
		dst_pte = huge_pte_alloc(dst_mm, dst_vma, dst_addr, vma_hpagesize);
		if (!dst_pte) {
			mutex_unlock(&hugetlb_fault_mutex_table[hash]);
			i_mmap_unlock_read(mapping);
			goto out_unlock;
		}

		if (mode != MCOPY_ATOMIC_CONTINUE &&
		    !huge_pte_none(huge_ptep_get(dst_pte))) {
			err = -EEXIST;
			mutex_unlock(&hugetlb_fault_mutex_table[hash]);
			i_mmap_unlock_read(mapping);
			goto out_unlock;
		}

		err = hugetlb_mcopy_atomic_pte(dst_mm, dst_pte, dst_vma,
					       dst_addr, src_addr, mode, &page);

		mutex_unlock(&hugetlb_fault_mutex_table[hash]);
		i_mmap_unlock_read(mapping);

		cond_resched();

		if (unlikely(err == -ENOENT)) {
			mmap_read_unlock(dst_mm);
			BUG_ON(!page);

			err = copy_huge_page_from_user(page,
						(const void __user *)src_addr,
						vma_hpagesize / PAGE_SIZE,
						true);
			if (unlikely(err)) {
				err = -EFAULT;
				goto out;
			}
			mmap_read_lock(dst_mm);

			dst_vma = NULL;
			goto retry;
		} else
			BUG_ON(page);

		if (!err) {
			dst_addr += vma_hpagesize;
			src_addr += vma_hpagesize;
			copied += vma_hpagesize;

			if (fatal_signal_pending(current))
				err = -EINTR;
		}
		if (err)
			break;
	}

out_unlock:
	mmap_read_unlock(dst_mm);
out:
	if (page)
		put_page(page);
	BUG_ON(copied < 0);
	BUG_ON(err > 0);
	BUG_ON(!copied && !err);
	return copied ? copied : err;
}
#else /* !CONFIG_HUGETLB_PAGE */
/* fail at build time if gcc attempts to use this */
extern ssize_t __mcopy_atomic_hugetlb(struct mm_struct *dst_mm,
				      struct vm_area_struct *dst_vma,
				      unsigned long dst_start,
				      unsigned long src_start,
				      unsigned long len,
				      enum mcopy_atomic_mode mode);
#endif /* CONFIG_HUGETLB_PAGE */

static __always_inline ssize_t mfill_atomic_pte(struct mm_struct *dst_mm,
						pmd_t *dst_pmd,
						struct vm_area_struct *dst_vma,
						unsigned long dst_addr,
						unsigned long src_addr,
						struct page **page,
						enum mcopy_atomic_mode mode,
						bool wp_copy)
{
	ssize_t err;

	if (mode == MCOPY_ATOMIC_CONTINUE) {
		return mcontinue_atomic_pte(dst_mm, dst_pmd, dst_vma, dst_addr,
					    wp_copy);
	}

	/*
	 * The normal page fault path for a shmem will invoke the
	 * fault, fill the hole in the file and COW it right away. The
	 * result generates plain anonymous memory. So when we are
	 * asked to fill an hole in a MAP_PRIVATE shmem mapping, we'll
	 * generate anonymous memory directly without actually filling
	 * the hole. For the MAP_PRIVATE case the robustness check
	 * only happens in the pagetable (to verify it's still none)
	 * and not in the radix tree.
	 */
	if (!(dst_vma->vm_flags & VM_SHARED)) {
		if (mode == MCOPY_ATOMIC_NORMAL)
			err = mcopy_atomic_pte(dst_mm, dst_pmd, dst_vma,
					       dst_addr, src_addr, page,
					       wp_copy);
		else
			err = mfill_zeropage_pte(dst_mm, dst_pmd,
						 dst_vma, dst_addr);
	} else {
		VM_WARN_ON_ONCE(wp_copy);
		err = shmem_mfill_atomic_pte(dst_mm, dst_pmd, dst_vma,
					     dst_addr, src_addr,
					     mode != MCOPY_ATOMIC_NORMAL,
					     page);
	}

	return err;
}

static __always_inline ssize_t __mcopy_atomic(struct mm_struct *dst_mm,
					      unsigned long dst_start,
					      unsigned long src_start,
					      unsigned long len,
					      enum mcopy_atomic_mode mcopy_mode,
					      atomic_t *mmap_changing,
					      __u64 mode)
{
	struct vm_area_struct *dst_vma;
	ssize_t err;
	pmd_t *dst_pmd;
	unsigned long src_addr, dst_addr;
	long copied;
	struct page *page;
	bool wp_copy;

	/*
	 * Sanitize the command parameters:
	 */
	BUG_ON(dst_start & ~PAGE_MASK);
	BUG_ON(len & ~PAGE_MASK);

	/* Does the address range wrap, or is the span zero-sized? */
	BUG_ON(src_start + len <= src_start);
	BUG_ON(dst_start + len <= dst_start);

	src_addr = src_start;
	dst_addr = dst_start;
	copied = 0;
	page = NULL;
retry:
	mmap_read_lock(dst_mm);

	/*
	 * If memory mappings are changing because of non-cooperative
	 * operation (e.g. mremap) running in parallel, bail out and
	 * request the user to retry later
	 */
	err = -EAGAIN;
	if (mmap_changing && atomic_read(mmap_changing))
		goto out_unlock;						/* FIXME : co-op mode mmap_changing never updated afterwards (pure hang), hence commented ; not bothered by WP & our mecanisms */

	/*
	 * Make sure the vma is not shared, that the dst range is
	 * both valid and fully within a single existing vma.
	 */
	err = -ENOENT;
	dst_vma = find_dst_vma(dst_mm, dst_start, len);
	if (!dst_vma)
		goto out_unlock;

	err = -EINVAL;
	/*
	 * shmem_zero_setup is invoked in mmap for MAP_ANONYMOUS|MAP_SHARED but
	 * it will overwrite vm_ops, so vma_is_anonymous must return false.
	 */
	if (WARN_ON_ONCE(vma_is_anonymous(dst_vma) &&
	    dst_vma->vm_flags & VM_SHARED))
		goto out_unlock;

	/*
	 * validate 'mode' now that we know the dst_vma: don't allow
	 * a wrprotect copy if the userfaultfd didn't register as WP.
	 */
	wp_copy = mode & UFFDIO_COPY_MODE_WP;
	if (wp_copy && !(dst_vma->vm_flags & VM_UFFD_WP))
		goto out_unlock;

	/*
	 * If this is a HUGETLB vma, pass off to appropriate routine
	 */
	if (is_vm_hugetlb_page(dst_vma))
		return  __mcopy_atomic_hugetlb(dst_mm, dst_vma, dst_start,
						src_start, len, mcopy_mode);

	if (!vma_is_anonymous(dst_vma) && !vma_is_shmem(dst_vma))
		goto out_unlock;
	if (!vma_is_shmem(dst_vma) && mcopy_mode == MCOPY_ATOMIC_CONTINUE)
		goto out_unlock;

	/*
	 * Ensure the dst_vma has a anon_vma or this page
	 * would get a NULL anon_vma when moved in the
	 * dst_vma.
	 */
	err = -ENOMEM;
	if (!(dst_vma->vm_flags & VM_SHARED) &&
	    unlikely(anon_vma_prepare(dst_vma)))
		goto out_unlock;

	while (src_addr < src_start + len) {
		pmd_t dst_pmdval;

		BUG_ON(dst_addr >= dst_start + len);

		dst_pmd = mm_alloc_pmd(dst_mm, dst_addr);
		if (unlikely(!dst_pmd)) {
			err = -ENOMEM;
			break;
		}

		dst_pmdval = pmd_read_atomic(dst_pmd);
		/*
		 * If the dst_pmd is mapped as THP don't
		 * override it and just be strict.
		 */
		if (unlikely(pmd_trans_huge(dst_pmdval))) {
			err = -EEXIST;
			break;
		}
		if (unlikely(pmd_none(dst_pmdval)) &&
		    unlikely(__pte_alloc(dst_mm, dst_pmd))) {
			err = -ENOMEM;
			break;
		}
		/* If an huge pmd materialized from under us fail */
		if (unlikely(pmd_trans_huge(*dst_pmd))) {
			err = -EFAULT;
			break;
		}

		BUG_ON(pmd_none(*dst_pmd));
		BUG_ON(pmd_trans_huge(*dst_pmd));

		err = mfill_atomic_pte(dst_mm, dst_pmd, dst_vma, dst_addr,
				       src_addr, &page, mcopy_mode, wp_copy);
		cond_resched();

		if (unlikely(err == -ENOENT)) {
			void *page_kaddr;

			mmap_read_unlock(dst_mm);
			BUG_ON(!page);

			page_kaddr = kmap(page);
			err = copy_from_user(page_kaddr,
					     (const void __user *) src_addr,
					     PAGE_SIZE);
			kunmap(page);
			if (unlikely(err)) {
				err = -EFAULT;
				goto out;
			}
			flush_dcache_page(page);
			goto retry;
		} else
			BUG_ON(page);

		if (!err) {
			dst_addr += PAGE_SIZE;
			src_addr += PAGE_SIZE;
			copied += PAGE_SIZE;

			if (fatal_signal_pending(current))
				err = -EINTR;
		}
		if (err)
			break;
	}

out_unlock:
	mmap_read_unlock(dst_mm);
out:
	if (page)
		put_page(page);
	BUG_ON(copied < 0);
	BUG_ON(err > 0);
	BUG_ON(!copied && !err);
	return copied ? copied : err;
}

ssize_t mcopy_atomic(struct mm_struct *dst_mm, unsigned long dst_start,
		     unsigned long src_start, unsigned long len,
		     atomic_t *mmap_changing, __u64 mode)
{
	return __mcopy_atomic(dst_mm, dst_start, src_start, len,
			      MCOPY_ATOMIC_NORMAL, mmap_changing, mode);
}

ssize_t mfill_zeropage(struct mm_struct *dst_mm, unsigned long start,
		       unsigned long len, atomic_t *mmap_changing)
{
	return __mcopy_atomic(dst_mm, start, 0, len, MCOPY_ATOMIC_ZEROPAGE,
			      mmap_changing, 0);
}

ssize_t mcopy_continue(struct mm_struct *dst_mm, unsigned long start,
		       unsigned long len, atomic_t *mmap_changing)
{
	return __mcopy_atomic(dst_mm, start, 0, len, MCOPY_ATOMIC_CONTINUE,
			      mmap_changing, 0);
}

int mwriteprotect_range(struct mm_struct *dst_mm, unsigned long start,
			unsigned long len, bool enable_wp,
			atomic_t *mmap_changing)
{
	struct vm_area_struct *dst_vma;
	pgprot_t newprot;
	int err;

	/*
	 * Sanitize the command parameters:
	 */
	BUG_ON(start & ~PAGE_MASK);
	BUG_ON(len & ~PAGE_MASK);

	/* Does the address range wrap, or is the span zero-sized? */
	BUG_ON(start + len <= start);

	mmap_read_lock(dst_mm);

	/*
	 * If memory mappings are changing because of non-cooperative
	 * operation (e.g. mremap) running in parallel, bail out and
	 * request the user to retry later
	 */
	err = -EAGAIN;
	if (mmap_changing && atomic_read(mmap_changing))
		goto out_unlock;

	err = -ENOENT;
	dst_vma = find_dst_vma(dst_mm, start, len);
	/*
	 * Make sure the vma is not shared, that the dst range is
	 * both valid and fully within a single existing vma.
	 */
	if (!dst_vma || (dst_vma->vm_flags & VM_SHARED))
		goto out_unlock;
	if (!userfaultfd_wp(dst_vma))
		goto out_unlock;
	if (!vma_is_anonymous(dst_vma))
		goto out_unlock;

	if (enable_wp)
		newprot = vm_get_page_prot(dst_vma->vm_flags & ~(VM_WRITE));
	else
		newprot = vm_get_page_prot(dst_vma->vm_flags);

	change_protection(dst_vma, start, start + len, newprot,
			  enable_wp ? MM_CP_UFFD_WP : MM_CP_UFFD_WP_RESOLVE);

	err = 0;
out_unlock:
	mmap_read_unlock(dst_mm);
	return err;
}
