/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/cma.h>
#include <linux/oom.h>
#include <linux/sched/signal.h>
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/profile.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/swap.h>
#include <linux/fs.h>
#include <linux/cpuset.h>
#include <linux/vmpressure.h>
#include <linux/freezer.h>

#define CREATE_TRACE_POINTS
#include <trace/events/almk.h>
#include <linux/show_mem_notifier.h>
#include <linux/ratelimit.h>

#ifdef CONFIG_HIGHMEM
#define _ZONE ZONE_HIGHMEM
#else
#define _ZONE ZONE_NORMAL
#endif

#define CREATE_TRACE_POINTS
#include "trace/lowmemorykiller.h"

/* to enable lowmemorykiller */
static int enable_lmk = 1;
module_param_named(enable_lmk, enable_lmk, int, 0644);

static uint32_t lmk_count;
static int lmkd_count;
static int lmkd_cricount;

static u32 lowmem_debug_level = 1;
static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};

static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};

static int lowmem_minfree_size = 4;
static int lmk_fast_run = 1;

static unsigned long lowmem_deathpending_timeout;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)

static void show_memory(void)
{
	unsigned long nr_rbin_free, nr_rbin_pool, nr_rbin_alloc, nr_rbin_file;

	nr_rbin_free = global_zone_page_state(NR_FREE_RBIN_PAGES);
	nr_rbin_pool = atomic_read(&rbin_pool_pages);
	nr_rbin_alloc = atomic_read(&rbin_allocated_pages);
	nr_rbin_file = totalrbin_pages - nr_rbin_free - nr_rbin_pool
					- nr_rbin_alloc;

#define K(x) ((x) << (PAGE_SHIFT - 10))
	printk("Mem-Info:"
		" totalram_pages:%lukB"
		" free:%lukB"
		" active_anon:%lukB"
		" inactive_anon:%lukB"
		" active_file:%lukB"
		" inactive_file:%lukB"
		" unevictable:%lukB"
		" isolated(anon):%lukB"
		" isolated(file):%lukB"
		" dirty:%lukB"
		" writeback:%lukB"
		" mapped:%lukB"
		" shmem:%lukB"
		" slab_reclaimable:%lukB"
		" slab_unreclaimable:%lukB"
		" kernel_stack:%lukB"
		" pagetables:%lukB"
		" free_cma:%lukB"
		" rbin_free:%lukB"
		" rbin_pool:%lukB"
		" rbin_alloc:%lukB"
		" rbin_file:%lukB"
		"\n",
		K(totalram_pages),
		K(global_zone_page_state(NR_FREE_PAGES)),
		K(global_node_page_state(NR_ACTIVE_ANON)),
		K(global_node_page_state(NR_INACTIVE_ANON)),
		K(global_node_page_state(NR_ACTIVE_FILE)),
		K(global_node_page_state(NR_INACTIVE_FILE)),
		K(global_node_page_state(NR_UNEVICTABLE)),
		K(global_node_page_state(NR_ISOLATED_ANON)),
		K(global_node_page_state(NR_ISOLATED_FILE)),
		K(global_node_page_state(NR_FILE_DIRTY)),
		K(global_node_page_state(NR_WRITEBACK)),
		K(global_node_page_state(NR_FILE_MAPPED)),
		K(global_node_page_state(NR_SHMEM)),
		K(global_node_page_state(NR_SLAB_RECLAIMABLE)),
		K(global_node_page_state(NR_SLAB_UNRECLAIMABLE)),
		global_zone_page_state(NR_KERNEL_STACK_KB),
		K(global_zone_page_state(NR_PAGETABLE)),
		K(global_zone_page_state(NR_FREE_CMA_PAGES)),
		K(nr_rbin_free),
		K(nr_rbin_pool),
		K(nr_rbin_alloc),
		K(nr_rbin_file)
		);
#undef K
}

static unsigned long lowmem_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	if (!enable_lmk)
		return 0;

	return global_node_page_state(NR_ACTIVE_ANON) +
		global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_ANON) +
		global_node_page_state(NR_INACTIVE_FILE);
}

bool lmk_kill_possible(void);
static atomic_t shift_adj = ATOMIC_INIT(0);
static short adj_max_shift = 353;
module_param_named(adj_max_shift, adj_max_shift, short, 0644);

/* User knob to enable/disable adaptive lmk feature */
static int enable_adaptive_lmk;
module_param_named(enable_adaptive_lmk, enable_adaptive_lmk, int, 0644);

/*
 * This parameter controls the behaviour of LMK when vmpressure is in
 * the range of 90-94. Adaptive lmk triggers based on number of file
 * pages wrt vmpressure_file_min, when vmpressure is in the range of
 * 90-94. Usually this is a pseudo minfree value, higher than the
 * highest configured value in minfree array.
 */
static int vmpressure_file_min;
module_param_named(vmpressure_file_min, vmpressure_file_min, int, 0644);

/* User knob to enable/disable oom reaping feature */
static int oom_reaper;
module_param_named(oom_reaper, oom_reaper, int, 0644);

/* Variable that helps in feed to the reclaim path  */
static atomic64_t lmk_feed = ATOMIC64_INIT(0);

/*
 * This function can be called whether to include the anon LRU pages
 * for accounting in the page reclaim.
 */
bool lmk_kill_possible(void)
{
	unsigned long val = atomic64_read(&lmk_feed);

	return !val || time_after_eq(jiffies, val);
}

enum {
	VMPRESSURE_NO_ADJUST = 0,
	VMPRESSURE_ADJUST_ENCROACH,
	VMPRESSURE_ADJUST_NORMAL,
};

static int adjust_minadj(short *min_score_adj)
{
	int ret = VMPRESSURE_NO_ADJUST;

	if (!enable_adaptive_lmk)
		return 0;

	if (atomic_read(&shift_adj) &&
	    (*min_score_adj > adj_max_shift)) {
		if (*min_score_adj == OOM_SCORE_ADJ_MAX + 1)
			ret = VMPRESSURE_ADJUST_ENCROACH;
		else
			ret = VMPRESSURE_ADJUST_NORMAL;
		*min_score_adj = adj_max_shift;
	}
	atomic_set(&shift_adj, 0);

	return ret;
}

static int lmk_vmpressure_notifier(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	int other_free, other_file;
	unsigned long pressure = action;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (!enable_adaptive_lmk)
		return 0;

	if (pressure >= 95) {
		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			total_swapcache_pages();
		other_free = global_zone_page_state(NR_FREE_PAGES);

		atomic_set(&shift_adj, 1);
		trace_almk_vmpressure(pressure, other_free, other_file);
	} else if (pressure >= 90) {
		if (lowmem_adj_size < array_size)
			array_size = lowmem_adj_size;
		if (lowmem_minfree_size < array_size)
			array_size = lowmem_minfree_size;

		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			total_swapcache_pages();

		other_free = global_zone_page_state(NR_FREE_PAGES);

		if (other_free < lowmem_minfree[array_size - 1] &&
		    other_file < vmpressure_file_min) {
			atomic_set(&shift_adj, 1);
			trace_almk_vmpressure(pressure, other_free, other_file);
		}
	} else if (atomic_read(&shift_adj)) {
		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			total_swapcache_pages();

		other_free = global_zone_page_state(NR_FREE_PAGES);
		/*
		 * shift_adj would have been set by a previous invocation
		 * of notifier, which is not followed by a lowmem_shrink yet.
		 * Since vmpressure has improved, reset shift_adj to avoid
		 * false adaptive LMK trigger.
		 */
		trace_almk_vmpressure(pressure, other_free, other_file);
		atomic_set(&shift_adj, 0);
	}

	return 0;
}

static struct notifier_block lmk_vmpr_nb = {
	.notifier_call = lmk_vmpressure_notifier,
};

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static int test_task_state(struct task_struct *p, int state)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (t->state & state) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static int test_task_lmk_waiting(struct task_struct *p)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (task_lmk_waiting(t)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static DEFINE_MUTEX(scan_mutex);

static int can_use_cma_pages(gfp_t gfp_mask)
{
	if (gfpflags_to_migratetype(gfp_mask) == MIGRATE_MOVABLE &&
	    (gfp_mask & __GFP_CMA))
		return 1;

	return 0;
}

static int can_use_rbin_pages(gfp_t gfp_mask)
{
	return !!(gfp_mask & __GFP_RBIN);
}

void tune_lmk_zone_param(struct zonelist *zonelist, int classzone_idx,
					int *other_free, int *other_file,
					int use_cma_pages, int use_rbin_pages)
{
	struct zone *zone;
	struct zoneref *zoneref;
	int zone_idx;
	unsigned long nr_zone_free;

	for_each_zone_zonelist(zone, zoneref, zonelist, MAX_NR_ZONES) {
		zone_idx = zonelist_zone_idx(zoneref);

		nr_zone_free = zone_page_state(zone, NR_FREE_PAGES);
		if (!use_rbin_pages)
			nr_zone_free -= zone_page_state(zone,
							NR_FREE_RBIN_PAGES);
		if (zone_idx > classzone_idx) {
			if (other_free != NULL)
				*other_free -= nr_zone_free;
			if (other_file != NULL)
				*other_file -= zone_page_state(zone,
					NR_ZONE_INACTIVE_FILE) +
					zone_page_state(zone,
					NR_ZONE_ACTIVE_FILE);
		} else if (zone_idx < classzone_idx) {
			if (zone_watermark_ok(zone, 0, 0, classzone_idx, 0) &&
			    other_free) {
				if (!use_cma_pages) {
					*other_free -= min(
					  zone->lowmem_reserve[classzone_idx] +
					  zone_page_state(
					    zone, NR_FREE_CMA_PAGES),
					  nr_zone_free);
				} else {
					*other_free -=
					  zone->lowmem_reserve[classzone_idx];
				}
			} else {
				if (other_free)
					*other_free -=
					  zone_page_state(zone, NR_FREE_PAGES);
			}
		}
	}
}

#ifdef CONFIG_HIGHMEM
static void adjust_gfp_mask(gfp_t *gfp_mask)
{
	struct zone *preferred_zone;
	struct zoneref *zref;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx;

	if (current_is_kswapd()) {
		zonelist = node_zonelist(0, *gfp_mask);
		high_zoneidx = gfp_zone(*gfp_mask);
		zref = first_zones_zonelist(zonelist, high_zoneidx, NULL);
		preferred_zone = zref->zone;

		if (high_zoneidx == ZONE_NORMAL) {
			if (zone_watermark_ok_safe(
					preferred_zone, 0,
					high_wmark_pages(preferred_zone), 0))
				*gfp_mask |= __GFP_HIGHMEM;
		} else if (high_zoneidx == ZONE_HIGHMEM) {
			*gfp_mask |= __GFP_HIGHMEM;
		}
	}
}
#else
static void adjust_gfp_mask(gfp_t *unused)
{
}
#endif

void tune_lmk_param(int *other_free, int *other_file, struct shrink_control *sc)
{
	gfp_t gfp_mask;
	struct zone *preferred_zone;
	struct zoneref *zref;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx, classzone_idx;
	unsigned long balance_gap;
	int use_cma_pages;
	int use_rbin_pages;
	unsigned long nr_zone_free;

	gfp_mask = sc->gfp_mask;
	adjust_gfp_mask(&gfp_mask);

	zonelist = node_zonelist(0, gfp_mask);
	high_zoneidx = gfp_zone(gfp_mask);
	zref = first_zones_zonelist(zonelist, high_zoneidx, NULL);
	preferred_zone = zref->zone;
	classzone_idx = zone_idx(preferred_zone);
	use_cma_pages = can_use_cma_pages(gfp_mask);
	use_rbin_pages = can_use_rbin_pages(gfp_mask);

	balance_gap = min(low_wmark_pages(preferred_zone),
			  (preferred_zone->present_pages +
			   100-1) /
			   100);

	if (likely(current_is_kswapd() && zone_watermark_ok(preferred_zone, 0,
			  high_wmark_pages(preferred_zone) + SWAP_CLUSTER_MAX +
			  balance_gap, 0, 0))) {
		if (lmk_fast_run)
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
					    other_file, use_cma_pages,
					    use_rbin_pages);
		else
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
					    NULL, use_cma_pages,
					    use_rbin_pages);

		nr_zone_free = zone_page_state(preferred_zone, NR_FREE_PAGES);
		if (!use_rbin_pages)
			nr_zone_free -= zone_page_state(preferred_zone,
							NR_FREE_RBIN_PAGES);
		if (zone_watermark_ok(preferred_zone, 0, 0, _ZONE, 0)) {
			if (!use_cma_pages) {
				*other_free -= min(
				  preferred_zone->lowmem_reserve[_ZONE]
				  + zone_page_state(
				    preferred_zone, NR_FREE_CMA_PAGES),
				  nr_zone_free);
			} else {
				*other_free -=
				  preferred_zone->lowmem_reserve[_ZONE];
			}
		} else {
			*other_free -= nr_zone_free;
		}

		lowmem_print(4, "lowmem_shrink of kswapd tunning for highmem "
			     "ofree %d, %d\n", *other_free, *other_file);
	} else {
		tune_lmk_zone_param(zonelist, classzone_idx, other_free,
			       other_file, use_cma_pages, use_rbin_pages);

		if (!use_cma_pages) {
			*other_free -=
			  zone_page_state(preferred_zone, NR_FREE_CMA_PAGES);
		}

		lowmem_print(4, "lowmem_shrink tunning for others ofree %d, "
			     "%d\n", *other_free, *other_file);
	}
}

/*
 * Return the percent of memory which gfp_mask is allowed to allocate from.
 * CMA memory is assumed to be a small percent and is not considered.
 * The goal is to apply a margin of minfree over all zones, rather than to
 * each zone individually.
 */
static int get_minfree_scalefactor(gfp_t gfp_mask)
{
	struct zonelist *zonelist = node_zonelist(0, gfp_mask);
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_usable = 0;

	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask))
		nr_usable += zone->managed_pages;

	return max_t(int, 1, mult_frac(100, nr_usable, totalram_pages));
}

static void mark_lmk_victim(struct task_struct *tsk)
{
	struct mm_struct *mm = tsk->mm;

	if (!cmpxchg(&tsk->signal->oom_mm, NULL, mm)) {
		atomic_inc(&tsk->signal->oom_mm->mm_count);
		set_bit(MMF_OOM_VICTIM, &mm->flags);
	}
}

static unsigned long lowmem_scan(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	unsigned long rem = 0;
	int tasksize;
	int i;
	int ret = 0;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
	int scale_percent;
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	bool lock_required = true;
	unsigned long nr_rbin_free, nr_rbin_pool, nr_rbin_alloc, nr_rbin_file;
	static DEFINE_RATELIMIT_STATE(lmk_rs, DEFAULT_RATELIMIT_INTERVAL, 1);
#if defined(CONFIG_SWAP)
	unsigned long swap_orig_nrpages;
	unsigned long swap_comp_nrpages;
	int swap_rss;
	int selected_swap_rss;

	swap_orig_nrpages = get_swap_orig_data_nrpages();
	swap_comp_nrpages = get_swap_comp_pool_nrpages();
#endif

	other_free = global_zone_page_state(NR_FREE_PAGES) - totalreserve_pages;
	if (global_node_page_state(NR_SHMEM) + total_swapcache_pages() +
			global_node_page_state(NR_UNEVICTABLE) <
			global_node_page_state(NR_FILE_PAGES))
		other_file = global_node_page_state(NR_FILE_PAGES) -
					global_node_page_state(NR_SHMEM) -
					global_node_page_state(NR_UNEVICTABLE) -
					total_swapcache_pages();
	else
		other_file = 0;

	if ((sc->gfp_mask & __GFP_RBIN) != __GFP_RBIN) {
		nr_rbin_free = global_zone_page_state(NR_FREE_RBIN_PAGES);
		nr_rbin_pool = atomic_read(&rbin_pool_pages);
		nr_rbin_alloc = atomic_read(&rbin_allocated_pages);
		nr_rbin_file = totalrbin_pages - nr_rbin_free - nr_rbin_pool
						- nr_rbin_alloc;
		other_free -= nr_rbin_free;
		other_file -= nr_rbin_file;
	}

	if (!get_nr_swap_pages() && (other_free <= lowmem_minfree[0] >> 1) &&
	    (other_file <= lowmem_minfree[0] >> 1))
		lock_required = false;

	if (likely(lock_required) && !mutex_trylock(&scan_mutex))
		return SHRINK_STOP;

	tune_lmk_param(&other_free, &other_file, sc);

	scale_percent = get_minfree_scalefactor(sc->gfp_mask);
	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		minfree = mult_frac(lowmem_minfree[i], scale_percent, 100);
		if (other_free < minfree && other_file < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}

	ret = adjust_minadj(&min_score_adj);

	lowmem_print(3, "%s %lu, %x, ofree %d %d, ma %hd\n",
		     __func__, sc->nr_to_scan, sc->gfp_mask, other_free,
		     other_file, min_score_adj);

	if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		trace_almk_shrink(0, ret, other_free, other_file, 0);
		lowmem_print(5, "%s %lu, %x, return 0\n",
			     __func__, sc->nr_to_scan, sc->gfp_mask);
		if (lock_required)
			mutex_unlock(&scan_mutex);
		return SHRINK_STOP;
	}

	selected_oom_score_adj = min_score_adj;

	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		if (oom_reaper) {
			p = find_lock_task_mm(tsk);
			if (!p)
				continue;

			if (test_bit(MMF_OOM_VICTIM, &p->mm->flags)) {
				if (test_bit(MMF_OOM_SKIP, &p->mm->flags)) {
					task_unlock(p);
					continue;
				} else if (time_before_eq(jiffies,
						lowmem_deathpending_timeout)) {
					task_unlock(p);
					rcu_read_unlock();
					if (lock_required)
						mutex_unlock(&scan_mutex);
					return SHRINK_STOP;
				}
			}
		} else {
			if (time_before_eq(jiffies,
					   lowmem_deathpending_timeout))
				if (test_task_lmk_waiting(tsk)) {
					rcu_read_unlock();
					if (lock_required)
						mutex_unlock(&scan_mutex);
					return SHRINK_STOP;
				}

			p = find_lock_task_mm(tsk);
			if (!p)
				continue;
		}

		if (task_lmk_waiting(p)) {
			task_unlock(p);
			continue;
		}
		if (p->state & TASK_UNINTERRUPTIBLE) {
			task_unlock(p);
			continue;
		}
		oom_score_adj = p->signal->oom_score_adj;
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
#if defined(CONFIG_SWAP)
		swap_rss = get_mm_counter(p->mm, MM_SWAPENTS) *
				swap_comp_nrpages / swap_orig_nrpages;
		lowmem_print(3, "%s tasksize rss: %d swap_rss: %d swap: %lu/%lu\n",
			     __func__, tasksize, swap_rss, swap_comp_nrpages,
			     swap_orig_nrpages);
		tasksize += swap_rss;
#endif
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
				continue;
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
		selected = p;
		selected_tasksize = tasksize;
#if defined(CONFIG_SWAP)
		selected_swap_rss = swap_rss;
#endif
		selected_oom_score_adj = oom_score_adj;
		lowmem_print(3, "select '%s' (%d), adj %hd, size %d, to kill\n",
			     p->comm, p->pid, oom_score_adj, tasksize);
	}
	if (selected) {
		long cache_size = other_file * (long)(PAGE_SIZE / 1024);
		long cache_limit = minfree * (long)(PAGE_SIZE / 1024);
		long free = other_free * (long)(PAGE_SIZE / 1024);
#if defined(CONFIG_SWAP)
		int orig_tasksize = selected_tasksize - selected_swap_rss;
#endif

		atomic64_set(&lmk_feed, 0);
		if (test_task_lmk_waiting(selected) &&
		    (test_task_state(selected, TASK_UNINTERRUPTIBLE))) {
			lowmem_print(2, "'%s' (%d) is already killed\n",
				     selected->comm,
				     selected->pid);
			rcu_read_unlock();
			if (lock_required)
				mutex_unlock(&scan_mutex);
			return SHRINK_STOP;
		}

		task_lock(selected);
		send_sig(SIGKILL, selected, 0);
		if (selected->mm) {
			task_set_lmk_waiting(selected);
			if (!test_bit(MMF_OOM_SKIP, &selected->mm->flags) &&
			    oom_reaper) {
				mark_lmk_victim(selected);
				wake_oom_reaper(selected);
			}
		}
		task_unlock(selected);
		trace_lowmemory_kill(selected, cache_size, cache_limit, free);
		lowmem_print(1, "Killing '%s' (%d) (tgid %d), adj %hd,\n"
#if defined(CONFIG_SWAP)
			"to free %ldkB (%ldKB %ldKB) on behalf of '%s' (%d) because\n"
#else
			"to free %ldkB on behalf of '%s' (%d) because\n"
#endif
			"cache %ldkB is below limit %ldkB for oom score %hd\n"
			"Free memory is %ldkB above reserved.\n"
			"Free CMA is %ldkB\n"
			"Total reserve is %ldkB\n"
			"Total free pages is %ldkB\n"
			"Total file cache is %ldkB\n"
			"GFP mask is %#x(%pGg)\n",
			selected->comm, selected->pid, selected->tgid,
			selected_oom_score_adj,
#if defined(CONFIG_SWAP)
			selected_tasksize * (long)(PAGE_SIZE / 1024),
			orig_tasksize * (long)(PAGE_SIZE / 1024),
			selected_swap_rss * (long)(PAGE_SIZE / 1024),
#else
			selected_tasksize * (long)(PAGE_SIZE / 1024),
#endif
			current->comm, current->pid,
			cache_size, cache_limit,
			min_score_adj,
			free,
			global_zone_page_state(NR_FREE_CMA_PAGES) *
			(long)(PAGE_SIZE / 1024),
			totalreserve_pages * (long)(PAGE_SIZE / 1024),
			global_zone_page_state(NR_FREE_PAGES) *
			(long)(PAGE_SIZE / 1024),
			global_node_page_state(NR_FILE_PAGES) *
			(long)(PAGE_SIZE / 1024),
			sc->gfp_mask, &sc->gfp_mask);

		show_mem_extra_call_notifiers();
		show_memory();
		if ((selected_oom_score_adj <= 100) && (__ratelimit(&lmk_rs)))
			dump_tasks(NULL, NULL);

		if (lowmem_debug_level >= 2 && selected_oom_score_adj == 0) {
			show_mem(SHOW_MEM_FILTER_NODES, NULL);
			show_mem_call_notifiers();
			dump_tasks(NULL, NULL);
		}

		lowmem_deathpending_timeout = jiffies + HZ;
		rem += selected_tasksize;
		rcu_read_unlock();
		lmk_count++;
		/* give the system time to free up the memory */
		msleep_interruptible(20);
		trace_almk_shrink(selected_tasksize, ret,
				  other_free, other_file,
				  selected_oom_score_adj);
	} else {
		trace_almk_shrink(1, ret, other_free, other_file, 0);
		rcu_read_unlock();
		if (other_free < lowmem_minfree[0] &&
		    other_file < lowmem_minfree[0])
			atomic64_set(&lmk_feed, jiffies + HZ);
		else
			atomic64_set(&lmk_feed, 0);

	}

	lowmem_print(4, "%s %lu, %x, return %lu\n",
		     __func__, sc->nr_to_scan, sc->gfp_mask, rem);
	if (lock_required)
		mutex_unlock(&scan_mutex);

	if (!rem)
		rem = SHRINK_STOP;
	return rem;
}

static struct shrinker lowmem_shrinker = {
	.scan_objects = lowmem_scan,
	.count_objects = lowmem_count,
	.seeks = DEFAULT_SEEKS * 16
};

static int __init lowmem_init(void)
{
	register_shrinker(&lowmem_shrinker);
	vmpressure_notifier_register(&lmk_vmpr_nb);
	return 0;
}
device_initcall(lowmem_init);

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	short oom_adj;
	short oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param_named(cost, lowmem_shrinker.seeks, int, 0644);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
module_param_cb(adj, &lowmem_adj_array_ops,
		.arr = &__param_arr_adj,
		0644);
__MODULE_PARM_TYPE(adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size, 0644);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmk_fast_run, lmk_fast_run, int, S_IRUGO | S_IWUSR);
module_param_named(lmkcount, lmk_count, uint, 0444);
module_param_named(lmkd_count, lmkd_count, int, 0644);
module_param_named(lmkd_cricount, lmkd_cricount, int, 0644);
