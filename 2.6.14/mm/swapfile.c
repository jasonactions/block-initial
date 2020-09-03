/*
 *  linux/mm/swapfile.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>
#include <linux/pagemap.h>
#include <linux/namei.h>
#include <linux/shm.h>
#include <linux/blkdev.h>
#include <linux/writeback.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/rmap.h>
#include <linux/security.h>
#include <linux/backing-dev.h>
#include <linux/syscalls.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <linux/swapops.h>

DEFINE_SPINLOCK(swap_lock);
/*存放swap_info数组中交换区描述符的最后一个索引，实际就是交换区的个数*/
unsigned int nr_swapfiles;
/*包含无缺陷页槽的总数*/
long total_swap_pages;
static int swap_overflow;

EXPORT_SYMBOL(total_swap_pages);

static const char Bad_file[] = "Bad swap file entry ";
static const char Unused_file[] = "Unused swap file entry ";
static const char Bad_offset[] = "Bad swap offset entry ";
static const char Unused_offset[] = "Unused swap offset entry ";
/*交换区链表*/
struct swap_list_t swap_list = {-1, -1};

struct swap_info_struct swap_info[MAX_SWAPFILES];

static DECLARE_MUTEX(swapon_sem);

/*
 * We need this because the bdev->unplug_fn can sleep and we cannot
 * hold swap_lock while calling the unplug_fn. And swap_lock
 * cannot be turned into a semaphore.
 */
static DECLARE_RWSEM(swap_unplug_sem);

void swap_unplug_io_fn(struct backing_dev_info *unused_bdi, struct page *page)
{
	swp_entry_t entry;

	down_read(&swap_unplug_sem);
	entry.val = page_private(page);
	if (PageSwapCache(page)) {
		struct block_device *bdev = swap_info[swp_type(entry)].bdev;
		struct backing_dev_info *bdi;

		/*
		 * If the page is removed from swapcache from under us (with a
		 * racy try_to_unuse/swapoff) we need an additional reference
		 * count to avoid reading garbage from page_private(page) above.
		 * If the WARN_ON triggers during a swapoff it maybe the race
		 * condition and it's harmless. However if it triggers without
		 * swapoff it signals a problem.
		 */
		WARN_ON(page_count(page) <= 1);

		bdi = bdev->bd_inode->i_mapping->backing_dev_info;
		blk_run_backing_dev(bdi, page);
	}
	up_read(&swap_unplug_sem);
}

#define SWAPFILE_CLUSTER	256
#define LATENCY_LIMIT		256


/*在给定的交换区查找一个空闲的页槽*/
static inline unsigned long scan_swap_map(struct swap_info_struct *si)
{
	unsigned long offset, last_in_cluster;
	int latency_ration = LATENCY_LIMIT;

	/* 
	 * We try to cluster swap pages by allocating them sequentially
	 * in swap.  Once we've allocated SWAPFILE_CLUSTER pages this
	 * way, however, we resort to first-free allocation, starting
	 * a new cluster.  This prevents us from scattering swap pages
	 * all over the entire swap partition, so that we reduce
	 * overall disk seek times between swap pages.  -- sct
	 * But we do now try to find an empty cluster.  -Andrea
	 */

	si->flags += SWP_SCANNING;
	if (unlikely(!si->cluster_nr)) {
		si->cluster_nr = SWAPFILE_CLUSTER - 1;
		if (si->pages - si->inuse_pages < SWAPFILE_CLUSTER)
			goto lowest;
		spin_unlock(&swap_lock);

		offset = si->lowest_bit;
		last_in_cluster = offset + SWAPFILE_CLUSTER - 1;

		/* Locate the first empty (unaligned) cluster */
		for (; last_in_cluster <= si->highest_bit; offset++) {
			if (si->swap_map[offset])
				last_in_cluster = offset + SWAPFILE_CLUSTER;
			else if (offset == last_in_cluster) {
				spin_lock(&swap_lock);
				si->cluster_next = offset-SWAPFILE_CLUSTER-1;
				goto cluster;
			}
			if (unlikely(--latency_ration < 0)) {
				cond_resched();
				latency_ration = LATENCY_LIMIT;
			}
		}
		spin_lock(&swap_lock);
		goto lowest;
	}

	si->cluster_nr--;
cluster:
	offset = si->cluster_next;
	if (offset > si->highest_bit)
lowest:		offset = si->lowest_bit;
checks:	if (!(si->flags & SWP_WRITEOK))
		goto no_page;
	if (!si->highest_bit)
		goto no_page;
	if (!si->swap_map[offset]) {
		if (offset == si->lowest_bit)
			si->lowest_bit++;
		if (offset == si->highest_bit)
			si->highest_bit--;
		si->inuse_pages++;
		if (si->inuse_pages == si->pages) {
			si->lowest_bit = si->max;
			si->highest_bit = 0;
		}
		si->swap_map[offset] = 1;
		si->cluster_next = offset + 1;
		si->flags -= SWP_SCANNING;
		return offset;
	}

	spin_unlock(&swap_lock);
	while (++offset <= si->highest_bit) {
		if (!si->swap_map[offset]) {
			spin_lock(&swap_lock);
			goto checks;
		}
		if (unlikely(--latency_ration < 0)) {
			cond_resched();
			latency_ration = LATENCY_LIMIT;
		}
	}
	spin_lock(&swap_lock);
	goto lowest;

no_page:
	si->flags -= SWP_SCANNING;
	return 0;
}

/*通过搜索所有活动的交换区来查找一个空闲页槽*/
swp_entry_t get_swap_page(void)
{
	struct swap_info_struct *si;
	pgoff_t offset;
	int type, next;
	int wrapped = 0;

	spin_lock(&swap_lock);
	if (nr_swap_pages <= 0)
		goto noswap;
	nr_swap_pages--;

	for (type = swap_list.next; type >= 0 && wrapped < 2; type = next) {
		si = swap_info + type;
		next = si->next;
		if (next < 0 ||
		    (!wrapped && si->prio != swap_info[next].prio)) {
			next = swap_list.head;
			wrapped++;
		}

		if (!si->highest_bit)
			continue;
		if (!(si->flags & SWP_WRITEOK))
			continue;

		swap_list.next = next;
		offset = scan_swap_map(si);
		if (offset) {
			spin_unlock(&swap_lock);
			return swp_entry(type, offset);
		}
		next = swap_list.next;
	}

	nr_swap_pages++;
noswap:
	spin_unlock(&swap_lock);
	return (swp_entry_t) {0};
}

static struct swap_info_struct * swap_info_get(swp_entry_t entry)
{
	struct swap_info_struct * p;
	unsigned long offset, type;

	if (!entry.val)
		goto out;
	type = swp_type(entry);
	if (type >= nr_swapfiles)
		goto bad_nofile;
	p = & swap_info[type];
	if (!(p->flags & SWP_USED))
		goto bad_device;
	offset = swp_offset(entry);
	if (offset >= p->max)
		goto bad_offset;
	if (!p->swap_map[offset])
		goto bad_free;
	spin_lock(&swap_lock);
	return p;

bad_free:
	printk(KERN_ERR "swap_free: %s%08lx\n", Unused_offset, entry.val);
	goto out;
bad_offset:
	printk(KERN_ERR "swap_free: %s%08lx\n", Bad_offset, entry.val);
	goto out;
bad_device:
	printk(KERN_ERR "swap_free: %s%08lx\n", Unused_file, entry.val);
	goto out;
bad_nofile:
	printk(KERN_ERR "swap_free: %s%08lx\n", Bad_file, entry.val);
out:
	return NULL;
}	

static int swap_entry_free(struct swap_info_struct *p, unsigned long offset)
{
	int count = p->swap_map[offset];

	if (count < SWAP_MAP_MAX) {
		count--;
		p->swap_map[offset] = count;
		if (!count) {
			if (offset < p->lowest_bit)
				p->lowest_bit = offset;
			if (offset > p->highest_bit)
				p->highest_bit = offset;
			if (p->prio > swap_info[swap_list.next].prio)
				swap_list.next = p - swap_info;
			nr_swap_pages++;
			p->inuse_pages--;
		}
	}
	return count;
}

/*
 * Caller has made sure that the swapdevice corresponding to entry
 * is still around or has not been recycled.
 */
/*当换入页时，对交换区描述符相应的swap_map计数器减一操作*/
void swap_free(swp_entry_t entry)
{
	struct swap_info_struct * p;

	p = swap_info_get(entry);
	if (p) {
		swap_entry_free(p, swp_offset(entry));
		spin_unlock(&swap_lock);
	}
}

/*
 * How many references to page are currently swapped out?
 */
static inline int page_swapcount(struct page *page)
{
	int count = 0;
	struct swap_info_struct *p;
	swp_entry_t entry;

	entry.val = page_private(page);
	p = swap_info_get(entry);
	if (p) {
		/* Subtract the 1 for the swap cache itself */
		count = p->swap_map[swp_offset(entry)] - 1;
		spin_unlock(&swap_lock);
	}
	return count;
}

/*
 * We can use this swap cache entry directly
 * if there are no other references to it.
 */
int can_share_swap_page(struct page *page)
{
	int count;

	BUG_ON(!PageLocked(page));
	count = page_mapcount(page);
	if (count <= 1 && PageSwapCache(page))
		count += page_swapcount(page);
	return count == 1;
}

/*
 * Work out if there are any other processes sharing this
 * swap cache page. Free it if you can. Return success.
 */
int remove_exclusive_swap_page(struct page *page)
{
	int retval;
	struct swap_info_struct * p;
	swp_entry_t entry;

	BUG_ON(PagePrivate(page));
	BUG_ON(!PageLocked(page));

	if (!PageSwapCache(page))
		return 0;
	if (PageWriteback(page))
		return 0;
	if (page_count(page) != 2) /* 2: us + cache */
		return 0;

	entry.val = page_private(page);
	p = swap_info_get(entry);
	if (!p)
		return 0;

	/* Is the only swap cache user the cache itself? */
	retval = 0;
	/*仅有一个进程引用此页槽*/
	if (p->swap_map[swp_offset(entry)] == 1) {
		/* Recheck the page count with the swapcache lock held.. */
		write_lock_irq(&swapper_space.tree_lock);
		if ((page_count(page) == 2) && !PageWriteback(page)) {
			__delete_from_swap_cache(page);
			SetPageDirty(page);
			retval = 1;
		}
		write_unlock_irq(&swapper_space.tree_lock);
	}
	spin_unlock(&swap_lock);

	if (retval) {
		swap_free(entry);
		page_cache_release(page);
	}

	return retval;
}

/*
 * Free the swap entry like above, but also try to
 * free the page cache entry if it is the last user.
 */
/*释放一个交换表项，并检查该表项引用的页是否在交换高速缓存*/
void free_swap_and_cache(swp_entry_t entry)
{
	struct swap_info_struct * p;
	struct page *page = NULL;

	p = swap_info_get(entry);
	if (p) {
		if (swap_entry_free(p, swp_offset(entry)) == 1)
			page = find_trylock_page(&swapper_space, entry.val);
		spin_unlock(&swap_lock);
	}
	if (page) {
		int one_user;

		BUG_ON(PagePrivate(page));
		page_cache_get(page);
		one_user = (page_count(page) == 2);
		/* Only cache user (+us), or swap space full? Free it! */
		if (!PageWriteback(page) && (one_user || vm_swap_full())) {
			delete_from_swap_cache(page);
			SetPageDirty(page);
		}
		unlock_page(page);
		page_cache_release(page);
	}
}

/*
 * No need to decide whether this PTE shares the swap entry with others,
 * just let do_wp_page work it out if a write is requested later - to
 * force COW, vm_page_prot omits write permission from any private vma.
 */
static void unuse_pte(struct vm_area_struct *vma, pte_t *pte,
		unsigned long addr, swp_entry_t entry, struct page *page)
{
	inc_mm_counter(vma->vm_mm, anon_rss);
	get_page(page);
	set_pte_at(vma->vm_mm, addr, pte,
		   pte_mkold(mk_pte(page, vma->vm_page_prot)));
	page_add_anon_rmap(page, vma, addr);
	swap_free(entry);
	/*
	 * Move the page to the active list so it is not
	 * immediately swapped out again after swapon.
	 */
	activate_page(page);
}

static int unuse_pte_range(struct vm_area_struct *vma, pmd_t *pmd,
				unsigned long addr, unsigned long end,
				swp_entry_t entry, struct page *page)
{
	pte_t swp_pte = swp_entry_to_pte(entry);
	pte_t *pte;
	spinlock_t *ptl;
	int found = 0;

	pte = pte_offset_map_lock(vma->vm_mm, pmd, addr, &ptl);
	do {
		/*
		 * swapoff spends a _lot_ of time in this loop!
		 * Test inline before going to call unuse_pte.
		 */
		if (unlikely(pte_same(*pte, swp_pte))) {
			unuse_pte(vma, pte++, addr, entry, page);
			found = 1;
			break;
		}
	} while (pte++, addr += PAGE_SIZE, addr != end);
	pte_unmap_unlock(pte - 1, ptl);
	return found;
}

static inline int unuse_pmd_range(struct vm_area_struct *vma, pud_t *pud,
				unsigned long addr, unsigned long end,
				swp_entry_t entry, struct page *page)
{
	pmd_t *pmd;
	unsigned long next;

	pmd = pmd_offset(pud, addr);
	do {
		next = pmd_addr_end(addr, end);
		if (pmd_none_or_clear_bad(pmd))
			continue;
		if (unuse_pte_range(vma, pmd, addr, next, entry, page))
			return 1;
	} while (pmd++, addr = next, addr != end);
	return 0;
}

static inline int unuse_pud_range(struct vm_area_struct *vma, pgd_t *pgd,
				unsigned long addr, unsigned long end,
				swp_entry_t entry, struct page *page)
{
	pud_t *pud;
	unsigned long next;

	pud = pud_offset(pgd, addr);
	do {
		next = pud_addr_end(addr, end);
		if (pud_none_or_clear_bad(pud))
			continue;
		if (unuse_pmd_range(vma, pud, addr, next, entry, page))
			return 1;
	} while (pud++, addr = next, addr != end);
	return 0;
}

static int unuse_vma(struct vm_area_struct *vma,
				swp_entry_t entry, struct page *page)
{
	pgd_t *pgd;
	unsigned long addr, end, next;

	if (page->mapping) {
		addr = page_address_in_vma(page, vma);
		if (addr == -EFAULT)
			return 0;
		else
			end = addr + PAGE_SIZE;
	} else {
		addr = vma->vm_start;
		end = vma->vm_end;
	}

	pgd = pgd_offset(vma->vm_mm, addr);
	do {
		next = pgd_addr_end(addr, end);
		if (pgd_none_or_clear_bad(pgd))
			continue;
		if (unuse_pud_range(vma, pgd, addr, next, entry, page))
			return 1;
	} while (pgd++, addr = next, addr != end);
	return 0;
}

static int unuse_mm(struct mm_struct *mm,
				swp_entry_t entry, struct page *page)
{
	struct vm_area_struct *vma;

	if (!down_read_trylock(&mm->mmap_sem)) {
		/*
		 * Activate page so shrink_cache is unlikely to unmap its
		 * ptes while lock is dropped, so swapoff can make progress.
		 */
		activate_page(page);
		unlock_page(page);
		down_read(&mm->mmap_sem);
		lock_page(page);
	}
	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma->anon_vma && unuse_vma(vma, entry, page))
			break;
	}
	up_read(&mm->mmap_sem);
	/*
	 * Currently unuse_mm cannot fail, but leave error handling
	 * at call sites for now, since we change it from time to time.
	 */
	return 0;
}

/*
 * Scan swap_map from current position to next entry still in use.
 * Recycle to start on reaching the end, returning 0 when empty.
 */
static unsigned int find_next_to_unuse(struct swap_info_struct *si,
					unsigned int prev)
{
	unsigned int max = si->max;
	unsigned int i = prev;
	int count;

	/*
	 * No need for swap_lock here: we're just looking
	 * for whether an entry is in use, not modifying it; false
	 * hits are okay, and sys_swapoff() has already prevented new
	 * allocations from this area (while holding swap_lock).
	 */
	for (;;) {
		if (++i >= max) {
			if (!prev) {
				i = 0;
				break;
			}
			/*
			 * No entries in use at top of swap_map,
			 * loop back to start and recheck there.
			 */
			max = prev + 1;
			prev = 0;
			i = 1;
		}
		count = si->swap_map[i];
		if (count && count != SWAP_MAP_BAD)
			break;
	}
	return i;
}

/*
 * We completely avoid races by reading each swap page in advance,
 * and then search for the process using it.  All the necessary
 * page table adjustments can then be made atomically.
 */
/*制把这个交换区中剩余的所有页移到RAM中，并相应的修改使用这些页的进程的页表*/
static int try_to_unuse(unsigned int type)
{
	struct swap_info_struct * si = &swap_info[type];
	struct mm_struct *start_mm;
	unsigned short *swap_map;
	unsigned short swcount;
	struct page *page;
	swp_entry_t entry;
	unsigned int i = 0;
	int retval = 0;
	int reset_overflow = 0;
	int shmem;

	/*
	 * When searching mms for an entry, a good strategy is to
	 * start at the first mm we freed the previous entry from
	 * (though actually we don't notice whether we or coincidence
	 * freed the entry).  Initialize this start_mm with a hold.
	 *
	 * A simpler strategy would be to start at the last mm we
	 * freed the previous entry from; but that would take less
	 * advantage of mmlist ordering, which clusters forked mms
	 * together, child after parent.  If we race with dup_mmap(), we
	 * prefer to resolve parent before child, lest we miss entries
	 * duplicated after we scanned child: using last mm would invert
	 * that.  Though it's only a serious concern when an overflowed
	 * swap count is reset from SWAP_MAP_MAX, preventing a rescan.
	 */
	start_mm = &init_mm;
	atomic_inc(&init_mm.mm_users);

	/*
	 * Keep on scanning until all entries have gone.  Usually,
	 * one pass through swap_map is enough, but not necessarily:
	 * there are races when an instance of an entry might be missed.
	 */
	while ((i = find_next_to_unuse(si, i)) != 0) {
		if (signal_pending(current)) {
			retval = -EINTR;
			break;
		}

		/* 
		 * Get a page for the entry, using the existing swap
		 * cache page if there is one.  Otherwise, get a clean
		 * page and read the swap into it. 
		 */
		swap_map = &si->swap_map[i];
		entry = swp_entry(type, i);
		page = read_swap_cache_async(entry, NULL, 0);
		if (!page) {
			/*
			 * Either swap_duplicate() failed because entry
			 * has been freed independently, and will not be
			 * reused since sys_swapoff() already disabled
			 * allocation from here, or alloc_page() failed.
			 */
			if (!*swap_map)
				continue;
			retval = -ENOMEM;
			break;
		}

		/*
		 * Don't hold on to start_mm if it looks like exiting.
		 */
		if (atomic_read(&start_mm->mm_users) == 1) {
			mmput(start_mm);
			start_mm = &init_mm;
			atomic_inc(&init_mm.mm_users);
		}

		/*
		 * Wait for and lock page.  When do_swap_page races with
		 * try_to_unuse, do_swap_page can handle the fault much
		 * faster than try_to_unuse can locate the entry.  This
		 * apparently redundant "wait_on_page_locked" lets try_to_unuse
		 * defer to do_swap_page in such a case - in some tests,
		 * do_swap_page and try_to_unuse repeatedly compete.
		 */
		wait_on_page_locked(page);
		wait_on_page_writeback(page);
		lock_page(page);
		wait_on_page_writeback(page);

		/*
		 * Remove all references to entry.
		 * Whenever we reach init_mm, there's no address space
		 * to search, but use it as a reminder to search shmem.
		 */
		shmem = 0;
		swcount = *swap_map;
		if (swcount > 1) {
			if (start_mm == &init_mm)
				shmem = shmem_unuse(entry, page);
			else
				retval = unuse_mm(start_mm, entry, page);
		}
		if (*swap_map > 1) {
			int set_start_mm = (*swap_map >= swcount);
			struct list_head *p = &start_mm->mmlist;
			struct mm_struct *new_start_mm = start_mm;
			struct mm_struct *prev_mm = start_mm;
			struct mm_struct *mm;

			atomic_inc(&new_start_mm->mm_users);
			atomic_inc(&prev_mm->mm_users);
			spin_lock(&mmlist_lock);
			while (*swap_map > 1 && !retval &&
					(p = p->next) != &start_mm->mmlist) {
				mm = list_entry(p, struct mm_struct, mmlist);
				if (atomic_inc_return(&mm->mm_users) == 1) {
					atomic_dec(&mm->mm_users);
					continue;
				}
				spin_unlock(&mmlist_lock);
				mmput(prev_mm);
				prev_mm = mm;

				cond_resched();

				swcount = *swap_map;
				if (swcount <= 1)
					;
				else if (mm == &init_mm) {
					set_start_mm = 1;
					shmem = shmem_unuse(entry, page);
				} else
					retval = unuse_mm(mm, entry, page);
				if (set_start_mm && *swap_map < swcount) {
					mmput(new_start_mm);
					atomic_inc(&mm->mm_users);
					new_start_mm = mm;
					set_start_mm = 0;
				}
				spin_lock(&mmlist_lock);
			}
			spin_unlock(&mmlist_lock);
			mmput(prev_mm);
			mmput(start_mm);
			start_mm = new_start_mm;
		}
		if (retval) {
			unlock_page(page);
			page_cache_release(page);
			break;
		}

		/*
		 * How could swap count reach 0x7fff when the maximum
		 * pid is 0x7fff, and there's no way to repeat a swap
		 * page within an mm (except in shmem, where it's the
		 * shared object which takes the reference count)?
		 * We believe SWAP_MAP_MAX cannot occur in Linux 2.4.
		 *
		 * If that's wrong, then we should worry more about
		 * exit_mmap() and do_munmap() cases described above:
		 * we might be resetting SWAP_MAP_MAX too early here.
		 * We know "Undead"s can happen, they're okay, so don't
		 * report them; but do report if we reset SWAP_MAP_MAX.
		 */
		if (*swap_map == SWAP_MAP_MAX) {
			spin_lock(&swap_lock);
			*swap_map = 1;
			spin_unlock(&swap_lock);
			reset_overflow = 1;
		}

		/*
		 * If a reference remains (rare), we would like to leave
		 * the page in the swap cache; but try_to_unmap could
		 * then re-duplicate the entry once we drop page lock,
		 * so we might loop indefinitely; also, that page could
		 * not be swapped out to other storage meanwhile.  So:
		 * delete from cache even if there's another reference,
		 * after ensuring that the data has been saved to disk -
		 * since if the reference remains (rarer), it will be
		 * read from disk into another page.  Splitting into two
		 * pages would be incorrect if swap supported "shared
		 * private" pages, but they are handled by tmpfs files.
		 *
		 * Note shmem_unuse already deleted a swappage from
		 * the swap cache, unless the move to filepage failed:
		 * in which case it left swappage in cache, lowered its
		 * swap count to pass quickly through the loops above,
		 * and now we must reincrement count to try again later.
		 */
		if ((*swap_map > 1) && PageDirty(page) && PageSwapCache(page)) {
			struct writeback_control wbc = {
				.sync_mode = WB_SYNC_NONE,
			};

			swap_writepage(page, &wbc);
			lock_page(page);
			wait_on_page_writeback(page);
		}
		if (PageSwapCache(page)) {
			if (shmem)
				swap_duplicate(entry);
			else
				delete_from_swap_cache(page);
		}

		/*
		 * So we could skip searching mms once swap count went
		 * to 1, we did not mark any present ptes as dirty: must
		 * mark page dirty so shrink_list will preserve it.
		 */
		SetPageDirty(page);
		unlock_page(page);
		page_cache_release(page);

		/*
		 * Make sure that we aren't completely killing
		 * interactive performance.
		 */
		cond_resched();
	}

	mmput(start_mm);
	if (reset_overflow) {
		printk(KERN_WARNING "swapoff: cleared swap entry overflow\n");
		swap_overflow = 0;
	}
	return retval;
}

/*
 * After a successful try_to_unuse, if no swap is now in use, we know
 * we can empty the mmlist.  swap_lock must be held on entry and exit.
 * Note that mmlist_lock nests inside swap_lock, and an mm must be
 * added to the mmlist just after page_duplicate - before would be racy.
 */
static void drain_mmlist(void)
{
	struct list_head *p, *next;
	unsigned int i;

	for (i = 0; i < nr_swapfiles; i++)
		if (swap_info[i].inuse_pages)
			return;
	spin_lock(&mmlist_lock);
	list_for_each_safe(p, next, &init_mm.mmlist)
		list_del_init(p);
	spin_unlock(&mmlist_lock);
}

/*
 * Use this swapdev's extent info to locate the (PAGE_SIZE) block which
 * corresponds to page offset `offset'.
 */
sector_t map_swap_page(struct swap_info_struct *sis, pgoff_t offset)
{
	struct swap_extent *se = sis->curr_swap_extent;
	struct swap_extent *start_se = se;

	for ( ; ; ) {
		struct list_head *lh;

		if (se->start_page <= offset &&
				offset < (se->start_page + se->nr_pages)) {
			return se->start_block + (offset - se->start_page);
		}
		lh = se->list.next;
		if (lh == &sis->extent_list)
			lh = lh->next;
		se = list_entry(lh, struct swap_extent, list);
		sis->curr_swap_extent = se;
		BUG_ON(se == start_se);		/* It *must* be present */
	}
}

/*
 * Free all of a swapdev's extent information
 */
static void destroy_swap_extents(struct swap_info_struct *sis)
{
	while (!list_empty(&sis->extent_list)) {
		struct swap_extent *se;

		se = list_entry(sis->extent_list.next,
				struct swap_extent, list);
		list_del(&se->list);
		kfree(se);
	}
}

/*
 * Add a block range (and the corresponding page range) into this swapdev's
 * extent list.  The extent list is kept sorted in page order.
 *
 * This function rather assumes that it is called in ascending page order.
 */
static int
add_swap_extent(struct swap_info_struct *sis, unsigned long start_page,
		unsigned long nr_pages, sector_t start_block)
{
	struct swap_extent *se;
	struct swap_extent *new_se;
	struct list_head *lh;

	lh = sis->extent_list.prev;	/* The highest page extent */
	if (lh != &sis->extent_list) {
		se = list_entry(lh, struct swap_extent, list);
		BUG_ON(se->start_page + se->nr_pages != start_page);
		if (se->start_block + se->nr_pages == start_block) {
			/* Merge it */
			se->nr_pages += nr_pages;
			return 0;
		}
	}

	/*
	 * No merge.  Insert a new extent, preserving ordering.
	 */
	new_se = kmalloc(sizeof(*se), GFP_KERNEL);
	if (new_se == NULL)
		return -ENOMEM;
	new_se->start_page = start_page;
	new_se->nr_pages = nr_pages;
	new_se->start_block = start_block;

	list_add_tail(&new_se->list, &sis->extent_list);
	return 1;
}

/*
 * A `swap extent' is a simple thing which maps a contiguous range of pages
 * onto a contiguous range of disk blocks.  An ordered list of swap extents
 * is built at swapon time and is then used at swap_writepage/swap_readpage
 * time for locating where on disk a page belongs.
 *
 * If the swapfile is an S_ISBLK block device, a single extent is installed.
 * This is done so that the main operating code can treat S_ISBLK and S_ISREG
 * swap files identically.
 *
 * Whether the swapdev is an S_ISREG file or an S_ISBLK blockdev, the swap
 * extent list operates in PAGE_SIZE disk blocks.  Both S_ISREG and S_ISBLK
 * swapfiles are handled *identically* after swapon time.
 *
 * For S_ISREG swapfiles, setup_swap_extents() will walk all the file's blocks
 * and will parse them into an ordered extent list, in PAGE_SIZE chunks.  If
 * some stray blocks are found which do not fall within the PAGE_SIZE alignment
 * requirements, they are simply tossed out - we will never use those blocks
 * for swapping.
 *
 * For S_ISREG swapfiles we set S_SWAPFILE across the life of the swapon.  This
 * prevents root from shooting her foot off by ftruncating an in-use swapfile,
 * which will scribble on the fs.
 *
 * The amount of disk space which a single swap extent represents varies.
 * Typically it is in the 1-4 megabyte range.  So we can have hundreds of
 * extents in the list.  To avoid much list walking, we cache the previous
 * search location in `curr_swap_extent', and start new searches from there.
 * This is extremely effective.  The average number of iterations in
 * map_swap_page() has been measured at about 0.3 per page.  - akpm.
 */
/*为交换区创建交换子区链表extent_list*/
static int setup_swap_extents(struct swap_info_struct *sis, sector_t *span)
{
	struct inode *inode;
	unsigned blocks_per_page;
	unsigned long page_no;
	unsigned blkbits;
	sector_t probe_block;
	sector_t last_block;
	sector_t lowest_block = -1;
	sector_t highest_block = 0;
	int nr_extents = 0;
	int ret;

	inode = sis->swap_file->f_mapping->host;
	/*如果交换区建立在磁盘分区上，则只有一个子区*/
	if (S_ISBLK(inode->i_mode)) {
		ret = add_swap_extent(sis, 0, sis->max, 0);
		*span = sis->pages;
		goto done;
	}

	blkbits = inode->i_blkbits;
	blocks_per_page = PAGE_SIZE >> blkbits;

	/*
	 * Map all the blocks into the extent list.  This code doesn't try
	 * to be very smart.
	 */
	probe_block = 0;
	page_no = 0;
	last_block = i_size_read(inode) >> blkbits;
	while ((probe_block + blocks_per_page) <= last_block &&
			page_no < sis->max) {
		unsigned block_in_page;
		sector_t first_block;

		first_block = bmap(inode, probe_block);
		if (first_block == 0)
			goto bad_bmap;

		/*
		 * It must be PAGE_SIZE aligned on-disk
		 */
		if (first_block & (blocks_per_page - 1)) {
			probe_block++;
			goto reprobe;
		}

		for (block_in_page = 1; block_in_page < blocks_per_page;
					block_in_page++) {
			sector_t block;

			block = bmap(inode, probe_block + block_in_page);
			if (block == 0)
				goto bad_bmap;
			if (block != first_block + block_in_page) {
				/* Discontiguity */
				probe_block++;
				goto reprobe;
			}
		}

		first_block >>= (PAGE_SHIFT - blkbits);
		if (page_no) {	/* exclude the header page */
			if (first_block < lowest_block)
				lowest_block = first_block;
			if (first_block > highest_block)
				highest_block = first_block;
		}

		/*
		 * We found a PAGE_SIZE-length, PAGE_SIZE-aligned run of blocks
		 */
		ret = add_swap_extent(sis, page_no, 1, first_block);
		if (ret < 0)
			goto out;
		nr_extents += ret;
		page_no++;
		probe_block += blocks_per_page;
reprobe:
		continue;
	}
	ret = nr_extents;
	*span = 1 + highest_block - lowest_block;
	if (page_no == 0)
		page_no = 1;	/* force Empty message */
	sis->max = page_no;
	sis->pages = page_no - 1;
	sis->highest_bit = page_no - 1;
done:
	/*初始化最近使用的子区描述符*/
	sis->curr_swap_extent = list_entry(sis->extent_list.prev,
					struct swap_extent, list);
	goto out;
bad_bmap:
	printk(KERN_ERR "swapon: swapfile has holes\n");
	ret = -EINVAL;
out:
	return ret;
}

#if 0	/* We don't need this yet */
#include <linux/backing-dev.h>
int page_queue_congested(struct page *page)
{
	struct backing_dev_info *bdi;

	BUG_ON(!PageLocked(page));	/* It pins the swap_info_struct */

	if (PageSwapCache(page)) {
		swp_entry_t entry = { .val = page_private(page) };
		struct swap_info_struct *sis;

		sis = get_swap_info_struct(swp_type(entry));
		bdi = sis->bdev->bd_inode->i_mapping->backing_dev_info;
	} else
		bdi = page->mapping->backing_dev_info;
	return bdi_write_congested(bdi);
}
#endif
/* 
 * 使specialfile指定的交换区无效
 * @specialfile: 指向设备文件（或分区）的路径名，或指向实现交换区的普通文件的路径名
 */
asmlinkage long sys_swapoff(const char __user * specialfile)
{
	struct swap_info_struct * p = NULL;
	unsigned short *swap_map;
	struct file *swap_file, *victim;
	struct address_space *mapping;
	struct inode *inode;
	char * pathname;
	int i, type, prev;
	int err;
	
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	pathname = getname(specialfile);
	err = PTR_ERR(pathname);
	if (IS_ERR(pathname))
		goto out;

	victim = filp_open(pathname, O_RDWR|O_LARGEFILE, 0);
	putname(pathname);
	err = PTR_ERR(victim);
	if (IS_ERR(victim))
		goto out;

	mapping = victim->f_mapping;
	prev = -1;
	spin_lock(&swap_lock);
	/*找到与交换文件对应的交换区描述符，type保存了其位于swap_info数组的下标*/
	for (type = swap_list.head; type >= 0; type = swap_info[type].next) {
		p = swap_info + type;
		if ((p->flags & SWP_ACTIVE) == SWP_ACTIVE) {
			if (p->swap_file->f_mapping == mapping)
				break;
		}
		prev = type;
	}
	if (type < 0) {
		err = -EINVAL;
		spin_unlock(&swap_lock);
		goto out_dput;
	}
	/*检查是否有足够的空闲页框将交换区上存放的所有页调入*/
	if (!security_vm_enough_memory(p->pages))
		vm_unacct_memory(p->pages);
	else {
		err = -ENOMEM;
		spin_unlock(&swap_lock);
		goto out_dput;
	}
	/*删除查到的交换区描述符*/
	if (prev < 0) {
		/*如果swap_list链表的第一个元素就是要查找的交换区描述符,则让链表的head指向下一个元素*/
		swap_list.head = p->next;
	} else {
		/*如果swap_list链表的第一个元素不是是要查找的交换区描述符,则让前一个元素的next指向下一个元素*/
		swap_info[prev].next = p->next;
	}
	if (type == swap_list.next) {
		/* just pick something that's safe... */
		swap_list.next = swap_list.head;
	}
	nr_swap_pages -= p->pages;
	total_swap_pages -= p->pages;
	p->flags &= ~SWP_WRITEOK;
	spin_unlock(&swap_lock);

	current->flags |= PF_SWAPOFF;
	/*
	 * 强制把这个交换区中剩余的所有页移到RAM中，并相应的修改使用这些页的进程的页表
	 */
	err = try_to_unuse(type);
	current->flags &= ~PF_SWAPOFF;

	/*如果try_to_unuse分配所有请求页框时失败，则不能禁用这个交换区*/
	if (err) {
		/* re-insert swap space back into swap_list */
		spin_lock(&swap_lock);
		for (prev = -1, i = swap_list.head; i >= 0; prev = i, i = swap_info[i].next)
			if (p->prio >= swap_info[i].prio)
				break;
		/*将删除的交换区重新放回*/
		p->next = i;
		if (prev < 0)
			swap_list.head = swap_list.next = p - swap_info;
		else
			swap_info[prev].next = p - swap_info;
		nr_swap_pages += p->pages;
		total_swap_pages += p->pages;
		p->flags |= SWP_WRITEOK;
		spin_unlock(&swap_lock);
		goto out_dput;
	}

	/* wait for any unplug function to finish */
	/*为了保证在交换区禁用之前所有交换区页读入page，因此持锁等待*/
	down_write(&swap_unplug_sem);
	up_write(&swap_unplug_sem);

	/*从交换子区链表中删除所有子区，并释放交换子区分配内存*/
	destroy_swap_extents(p);
	down(&swapon_sem);
	spin_lock(&swap_lock);
	drain_mmlist();

	/* wait for anyone still in scan_swap_map */
	p->highest_bit = 0;		/* cuts scans short */
	while (p->flags >= SWP_SCANNING) {
		spin_unlock(&swap_lock);
		schedule_timeout_uninterruptible(1);
		spin_lock(&swap_lock);
	}

	swap_file = p->swap_file;
	p->swap_file = NULL;
	p->max = 0;
	swap_map = p->swap_map;
	p->swap_map = NULL;
	p->flags = 0;
	spin_unlock(&swap_lock);
	up(&swapon_sem);
	/*释放交换区的引用计数器数组*/
	vfree(swap_map);
	inode = mapping->host;
	/*如果交换区存放在磁盘分区*/
	if (S_ISBLK(inode->i_mode)) {
		struct block_device *bdev = I_BDEV(inode);
		/*恢复块大小原值*/
		set_blocksize(bdev, p->old_block_size);
		/*取消交换子系统占有块设备*/
		bd_release(bdev);
	} else {
		down(&inode->i_sem);
		inode->i_flags &= ~S_SWAPFILE;
		up(&inode->i_sem);
	}
	filp_close(swap_file, NULL);
	err = 0;

out_dput:
	filp_close(victim, NULL);
out:
	return err;
}

#ifdef CONFIG_PROC_FS
/* iterator */
static void *swap_start(struct seq_file *swap, loff_t *pos)
{
	struct swap_info_struct *ptr = swap_info;
	int i;
	loff_t l = *pos;

	down(&swapon_sem);

	for (i = 0; i < nr_swapfiles; i++, ptr++) {
		if (!(ptr->flags & SWP_USED) || !ptr->swap_map)
			continue;
		if (!l--)
			return ptr;
	}

	return NULL;
}

static void *swap_next(struct seq_file *swap, void *v, loff_t *pos)
{
	struct swap_info_struct *ptr = v;
	struct swap_info_struct *endptr = swap_info + nr_swapfiles;

	for (++ptr; ptr < endptr; ptr++) {
		if (!(ptr->flags & SWP_USED) || !ptr->swap_map)
			continue;
		++*pos;
		return ptr;
	}

	return NULL;
}

static void swap_stop(struct seq_file *swap, void *v)
{
	up(&swapon_sem);
}

static int swap_show(struct seq_file *swap, void *v)
{
	struct swap_info_struct *ptr = v;
	struct file *file;
	int len;

	if (v == swap_info)
		seq_puts(swap, "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");

	file = ptr->swap_file;
	len = seq_path(swap, file->f_vfsmnt, file->f_dentry, " \t\n\\");
	seq_printf(swap, "%*s%s\t%u\t%u\t%d\n",
		       len < 40 ? 40 - len : 1, " ",
		       S_ISBLK(file->f_dentry->d_inode->i_mode) ?
				"partition" : "file\t",
		       ptr->pages << (PAGE_SHIFT - 10),
		       ptr->inuse_pages << (PAGE_SHIFT - 10),
		       ptr->prio);
	return 0;
}

static struct seq_operations swaps_op = {
	.start =	swap_start,
	.next =		swap_next,
	.stop =		swap_stop,
	.show =		swap_show
};

static int swaps_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &swaps_op);
}

static struct file_operations proc_swaps_operations = {
	.open		= swaps_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int __init procswaps_init(void)
{
	struct proc_dir_entry *entry;

	entry = create_proc_entry("swaps", 0, NULL);
	if (entry)
		entry->proc_fops = &proc_swaps_operations;
	return 0;
}
__initcall(procswaps_init);
#endif /* CONFIG_PROC_FS */

/*
 * Written 01/25/92 by Simmule Turner, heavily changed by Linus.
 *
 * The swapon system call
 */
/*  
 * 对创建交换区时放入第一个页槽中的swap_header联合体字段进行检查,并初始化交换区描述符，链入swap_list链表
 *
 * @specialfile: 指向设备文件（或分区）的路径名，或指向实现交换区的普通文件的路径名
 * @swap_flags：有一个单独的SWAP_FLAG_PREFER位加上交换区优先级的31位组成
 */
asmlinkage long sys_swapon(const char __user * specialfile, int swap_flags)
{
	struct swap_info_struct * p;
	char *name = NULL;
	struct block_device *bdev = NULL;
	struct file *swap_file = NULL;
	struct address_space *mapping;
	unsigned int type;
	int i, prev;
	int error;
	static int least_priority;
	union swap_header *swap_header = NULL;
	int swap_header_version;
	unsigned int nr_good_pages = 0;
	int nr_extents = 0;
	sector_t span;
	unsigned long maxpages = 1;
	int swapfilesize;
	unsigned short *swap_map;
	struct page *page = NULL;
	struct inode *inode = NULL;
	int did_down = 0;

	/*不具有CAP_SYS_ADMIN权能则返回*/
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	spin_lock(&swap_lock);
	/*获取swap_info数组的第一个交换区描述符*/
	p = swap_info;
	/*查找第一个未激活的交换区，交换区描述符数组swap_info索引为type,一般为nr_swapfiles*/
	for (type = 0 ; type < nr_swapfiles ; type++,p++)
		if (!(p->flags & SWP_USED))
			break;
	error = -EPERM;
	/*
	 * Test if adding another swap device is possible. There are
	 * two limiting factors: 1) the number of bits for the swap
	 * type swp_entry_t definition and 2) the number of bits for
	 * the swap type in the swap ptes as defined by the different
	 * architectures. To honor both limitations a swap entry
	 * with swap offset 0 and swap type ~0UL is created, encoded
	 * to a swap pte, decoded to a swp_entry_t again and finally
	 * the swap type part is extracted. This will mask all bits
	 * from the initial ~0UL that can't be encoded in either the
	 * swp_entry_t or the architecture definition of a swap pte.
	 */
	/*检查获取的索引值是否超过交换区描述符数组的最大有效索引*/
	if (type > swp_type(pte_to_swp_entry(swp_entry_to_pte(swp_entry(~0UL,0))))) {
		spin_unlock(&swap_lock);
		goto out;
	}
	if (type >= nr_swapfiles)
		nr_swapfiles = type+1;
	/*初始化找到的交换区描述符*/
	INIT_LIST_HEAD(&p->extent_list);
	p->flags = SWP_USED;
	p->swap_file = NULL;
	p->old_block_size = 0;
	p->swap_map = NULL;
	p->lowest_bit = 0;
	p->highest_bit = 0;
	p->cluster_nr = 0;
	p->inuse_pages = 0;
	p->next = -1;
	/*表示swap_flags已经指定了交换区描述符的优先级*/
	if (swap_flags & SWAP_FLAG_PREFER) {
		p->prio =
		  (swap_flags & SWAP_FLAG_PRIO_MASK)>>SWAP_FLAG_PRIO_SHIFT;
	} else {
		/*将所有活动交换区的最低优先级-1赋值*/
		p->prio = --least_priority;
	}
	spin_unlock(&swap_lock);
	name = getname(specialfile);
	error = PTR_ERR(name);
	if (IS_ERR(name)) {
		name = NULL;
		goto bad_swap_2;
	}
	/*打开交换区文件,如果没有则创建???*/
	swap_file = filp_open(name, O_RDWR|O_LARGEFILE, 0);
	error = PTR_ERR(swap_file);
	if (IS_ERR(swap_file)) {
		swap_file = NULL;
		goto bad_swap_2;
	}

	p->swap_file = swap_file;
	mapping = swap_file->f_mapping;
	inode = mapping->host;

	/*确认该交换区还未激活*/
	error = -EBUSY;
	for (i = 0; i < nr_swapfiles; i++) {
		struct swap_info_struct *q = &swap_info[i];

		if (i == type || !q->swap_file)
			continue;
		/*选取的新交换区已经被激活*/
		if (mapping == q->swap_file->f_mapping)
			goto bad_swap;
	}

	error = -EINVAL;
	if (S_ISBLK(inode->i_mode)) {
		bdev = I_BDEV(inode);
		/*设置块设备的占有者为交换子系统*/
		error = bd_claim(bdev, sys_swapon);
		if (error < 0) {
			bdev = NULL;
			error = -EINVAL;
			goto bad_swap;
		}
		p->old_block_size = block_size(bdev);
		/*设置块设备大小*/
		error = set_blocksize(bdev, PAGE_SIZE);
		if (error < 0)
			goto bad_swap;
		p->bdev = bdev;
	} else if (S_ISREG(inode->i_mode)) {
		p->bdev = inode->i_sb->s_bdev;
		down(&inode->i_sem);
		did_down = 1;
		/*文件是否已经被用作交换区*/
		if (IS_SWAPFILE(inode)) {
			error = -EBUSY;
			goto bad_swap;
		}
	} else {
		goto bad_swap;
	}

	/*获取交换文件大小，单位：页*/
	swapfilesize = i_size_read(inode) >> PAGE_SHIFT;

	/*
	 * Read the swap header.
	 */
	if (!mapping->a_ops->readpage) {
		error = -EINVAL;
		goto bad_swap;
	}
	/*从交换文件读取第一个页槽，它保存了swap_header描述符*/
	page = read_cache_page(mapping, 0,
			(filler_t *)mapping->a_ops->readpage, swap_file);
	if (IS_ERR(page)) {
		error = PTR_ERR(page);
		goto bad_swap;
	}
	/*等待页被读取内存*/
	wait_on_page_locked(page);
	if (!PageUptodate(page))
		goto bad_swap;
	/*为page创建永久性映射*/
	kmap(page);
	/*获取page的虚拟地址，他就是交换区第一个页即swap_header所在的地址*/
	swap_header = page_address(page);

	/*检查swap_header所在页的最后10个字节,得到swap_header的版本*/
	if (!memcmp("SWAP-SPACE",swap_header->magic.magic,10))
		swap_header_version = 1;
	else if (!memcmp("SWAPSPACE2",swap_header->magic.magic,10))
		swap_header_version = 2;
	else {
		printk("Unable to find swap-space signature\n");
		error = -EINVAL;
		goto bad_swap;
	}

	switch (swap_header_version) {
	case 1:
		printk(KERN_ERR "version 0 swap is no longer supported. "
			"Use mkswap -v1 %s\n", name);
		error = -EINVAL;
		goto bad_swap;
	case 2:
		/* Check the swap header's sub-version and the size of
                   the swap file and bad block lists */
		if (swap_header->info.version != 1) {
			printk(KERN_WARNING
			       "Unable to handle swap header version %d\n",
			       swap_header->info.version);
			error = -EINVAL;
			goto bad_swap;
		}

		/*初始化交换区搜索空闲页槽的起始页槽*/
		p->lowest_bit  = 1;
		/*初始化交换区搜索空闲页槽的下一个页槽*/
		p->cluster_next = 1;

		/*
		 * Find out how many pages are allowed for a single swap
		 * device. There are two limiting factors: 1) the number of
		 * bits for the swap offset in the swp_entry_t type and
		 * 2) the number of bits in the a swap pte as defined by
		 * the different architectures. In order to find the
		 * largest possible bit mask a swap entry with swap type 0
		 * and swap offset ~0UL is created, encoded to a swap pte,
		 * decoded to a swp_entry_t again and finally the swap
		 * offset is extracted. This will mask all the bits from
		 * the initial ~0UL mask that can't be encoded in either
		 * the swp_entry_t or the architecture definition of a
		 * swap pte.
		 */
		maxpages = swp_offset(pte_to_swp_entry(swp_entry_to_pte(swp_entry(0,~0UL)))) - 1;
		if (maxpages > swap_header->info.last_page)
			maxpages = swap_header->info.last_page;
		/*初始化交换区搜索空闲页槽时的最后一个页槽*/
		p->highest_bit = maxpages - 1;

		error = -EINVAL;
		if (!maxpages)
			goto bad_swap;
		if (swap_header->info.nr_badpages && S_ISREG(inode->i_mode))
			goto bad_swap;
		if (swap_header->info.nr_badpages > MAX_SWAP_BADPAGES)
			goto bad_swap;
		
		/* OK, set up the swap map and apply the bad block list */
		/*为页槽计数器数组分配空间*/
		if (!(p->swap_map = vmalloc(maxpages * sizeof(short)))) {
			error = -ENOMEM;
			goto bad_swap;
		}

		error = 0;
		memset(p->swap_map, 0, maxpages * sizeof(short));
		/*对坏的页槽，将页框计数器数组对应的元素设置为SWAP_MAP_BAD，表示此页槽坏*/
		for (i=0; i<swap_header->info.nr_badpages; i++) {
			int page = swap_header->info.badpages[i];
			if (page <= 0 || page >= swap_header->info.last_page)
				error = -EINVAL;
			else
				p->swap_map[page] = SWAP_MAP_BAD;
		}
		nr_good_pages = swap_header->info.last_page -
				swap_header->info.nr_badpages -
				1 /* header page */;
		if (error) 
			goto bad_swap;
	}

	if (swapfilesize && maxpages > swapfilesize) {
		printk(KERN_WARNING
		       "Swap area shorter than signature indicates\n");
		error = -EINVAL;
		goto bad_swap;
	}
	if (nr_good_pages) {
		p->swap_map[0] = SWAP_MAP_BAD;
		p->max = maxpages;
		p->pages = nr_good_pages;
		/*创建交换子区*/
		nr_extents = setup_swap_extents(p, &span);
		if (nr_extents < 0) {
			error = nr_extents;
			goto bad_swap;
		}
		nr_good_pages = p->pages;
	}
	if (!nr_good_pages) {
		printk(KERN_WARNING "Empty swap-file\n");
		error = -EINVAL;
		goto bad_swap;
	}

	down(&swapon_sem);
	spin_lock(&swap_lock);
	p->flags = SWP_ACTIVE;
	nr_swap_pages += nr_good_pages;
	total_swap_pages += nr_good_pages;

	printk(KERN_INFO "Adding %uk swap on %s.  "
			"Priority:%d extents:%d across:%lluk\n",
		nr_good_pages<<(PAGE_SHIFT-10), name, p->prio,
		nr_extents, (unsigned long long)span<<(PAGE_SHIFT-10));

	/* insert swap space into swap_list: */
	/*按照优先级，将新交换区插入到swap_list链表*/
	prev = -1;
	for (i = swap_list.head; i >= 0; i = swap_info[i].next) {
		if (p->prio >= swap_info[i].prio) {
			break;
		}
		prev = i;
	}
	p->next = i;
	if (prev < 0) {
		swap_list.head = swap_list.next = p - swap_info;
	} else {
		swap_info[prev].next = p - swap_info;
	}
	spin_unlock(&swap_lock);
	up(&swapon_sem);
	error = 0;
	goto out;
bad_swap:
	if (bdev) {
		set_blocksize(bdev, p->old_block_size);
		bd_release(bdev);
	}
	destroy_swap_extents(p);
bad_swap_2:
	spin_lock(&swap_lock);
	swap_map = p->swap_map;
	p->swap_file = NULL;
	p->swap_map = NULL;
	p->flags = 0;
	if (!(swap_flags & SWAP_FLAG_PREFER))
		++least_priority;
	spin_unlock(&swap_lock);
	vfree(swap_map);
	if (swap_file)
		filp_close(swap_file, NULL);
out:
	if (page && !IS_ERR(page)) {
		kunmap(page);
		page_cache_release(page);
	}
	if (name)
		putname(name);
	if (did_down) {
		if (!error)
			inode->i_flags |= S_SWAPFILE;
		up(&inode->i_sem);
	}
	return error;
}

void si_swapinfo(struct sysinfo *val)
{
	unsigned int i;
	unsigned long nr_to_be_unused = 0;

	spin_lock(&swap_lock);
	for (i = 0; i < nr_swapfiles; i++) {
		if (!(swap_info[i].flags & SWP_USED) ||
		     (swap_info[i].flags & SWP_WRITEOK))
			continue;
		nr_to_be_unused += swap_info[i].inuse_pages;
	}
	val->freeswap = nr_swap_pages + nr_to_be_unused;
	val->totalswap = total_swap_pages + nr_to_be_unused;
	spin_unlock(&swap_lock);
}

/*
 * Verify that a swap entry is valid and increment its swap map count.
 *
 * Note: if swap_map[] reaches SWAP_MAP_MAX the entries are treated as
 * "permanent", but will be reclaimed by the next swapoff.
 */
/*
 * 试图换出一个已经换出的页，增加交换区的引用计数
 * @entry: 换出页标识符，包含交换区描述符位于swap_info数组中的索引以及交换区的页槽索引
 */
int swap_duplicate(swp_entry_t entry)
{
	struct swap_info_struct * p;
	unsigned long offset, type;
	int result = 0;

	type = swp_type(entry);
	if (type >= nr_swapfiles)
		goto bad_file;
	/*获取交换区描述符在swap_info数组的地址*/
	p = type + swap_info;
	/*获取交换区的页槽索引*/
	offset = swp_offset(entry);

	spin_lock(&swap_lock);
	/*交换区被激活*/
	if (offset < p->max && p->swap_map[offset]) {
		/*交换区有效且不为空闲*/
		if (p->swap_map[offset] < SWAP_MAP_MAX - 1) {
			/*增加交换区页槽的引用计数*/
			p->swap_map[offset]++;
			result = 1;
		} else if (p->swap_map[offset] <= SWAP_MAP_MAX) {
			if (swap_overflow++ < 5)
				printk(KERN_WARNING "swap_dup: swap entry overflow\n");
			p->swap_map[offset] = SWAP_MAP_MAX;
			result = 1;
		}
	}
	spin_unlock(&swap_lock);
out:
	return result;

bad_file:
	printk(KERN_ERR "swap_dup: %s%08lx\n", Bad_file, entry.val);
	goto out;
}

struct swap_info_struct *
get_swap_info_struct(unsigned type)
{
	return &swap_info[type];
}

/*
 * swap_lock prevents swap_map being freed. Don't grab an extra
 * reference on the swaphandle, it doesn't matter if it becomes unused.
 */
int valid_swaphandles(swp_entry_t entry, unsigned long *offset)
{
	int ret = 0, i = 1 << page_cluster;
	unsigned long toff;
	struct swap_info_struct *swapdev = swp_type(entry) + swap_info;

	if (!page_cluster)	/* no readahead */
		return 0;
	toff = (swp_offset(entry) >> page_cluster) << page_cluster;
	if (!toff)		/* first page is swap header */
		toff++, i--;
	*offset = toff;

	spin_lock(&swap_lock);
	do {
		/* Don't read-ahead past the end of the swap area */
		if (toff >= swapdev->max)
			break;
		/* Don't read in free or bad pages */
		if (!swapdev->swap_map[toff])
			break;
		if (swapdev->swap_map[toff] == SWAP_MAP_BAD)
			break;
		toff++;
		ret++;
	} while (--i);
	spin_unlock(&swap_lock);
	return ret;
}
