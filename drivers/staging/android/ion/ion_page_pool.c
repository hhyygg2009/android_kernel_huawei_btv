/*
 * drivers/staging/android/ion/ion_mem_pool.c
 *
 * Copyright (C) 2011 Google, Inc.
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

#include <linux/debugfs.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/swap.h>
#include <linux/vmstat.h>

#include "ion_priv.h"

void *ion_page_pool_alloc_pages(struct ion_page_pool *pool)
{
/* zero buffer by caller, that zero buffer faster */
#if 0
	struct page *page = alloc_pages(pool->gfp_mask, pool->order);
#else
	struct page *page = alloc_pages(pool->gfp_mask & (~__GFP_ZERO),
					pool->order);
#endif

	if (!page)
		return NULL;
#if 0
	ion_page_pool_alloc_set_cache_policy(pool, page);

	ion_pages_sync_for_device(NULL, page, PAGE_SIZE << pool->order,
						DMA_BIDIRECTIONAL);
#endif
	return page;
}

static void ion_page_pool_free_pages(struct ion_page_pool *pool,
				     struct page *page)
{
	ion_page_pool_free_set_cache_policy(pool, page);
	__free_pages(page, pool->order);
}

static int ion_page_pool_add(struct ion_page_pool *pool, struct page *page)
{
	mutex_lock(&pool->mutex);
	zone_page_state_add(1 << pool->order, page_zone(page),
			    NR_IONCACHE_PAGES);
	if (PageHighMem(page)) {
		list_add_tail(&page->lru, &pool->high_items);
		pool->high_count++;
	} else {
		list_add_tail(&page->lru, &pool->low_items);
		pool->low_count++;
	}
	mutex_unlock(&pool->mutex);
	return 0;
}

static struct page *ion_page_pool_remove(struct ion_page_pool *pool, bool high)
{
	struct page *page;

	if (high) {
		BUG_ON(!pool->high_count);
		page = list_first_entry(&pool->high_items, struct page, lru);
		pool->high_count--;
	} else {
		BUG_ON(!pool->low_count);
		page = list_first_entry(&pool->low_items, struct page, lru);
		pool->low_count--;
	}
	zone_page_state_add(-(1 << pool->order), page_zone(page),
			    NR_IONCACHE_PAGES);

	list_del(&page->lru);
	return page;
}

struct page *ion_page_pool_alloc(struct ion_page_pool *pool)
{
	struct page *page = NULL;

	BUG_ON(!pool);

	mutex_lock(&pool->mutex);
	if (pool->high_count)
		page = ion_page_pool_remove(pool, true);
	else if (pool->low_count)
		page = ion_page_pool_remove(pool, false);
	mutex_unlock(&pool->mutex);

	if (!page && !(pool->graphic_buffer_flag))
		page = ion_page_pool_alloc_pages(pool);

	return page;
}

/*
 * Tries to allocate from only the specified Pool and returns NULL otherwise
 */
void *ion_page_pool_alloc_pool_only(struct ion_page_pool *pool)
{
	struct page *page = NULL;

	BUG_ON(!pool);

	if (mutex_trylock(&pool->mutex)) {
		if (pool->high_count)
			page = ion_page_pool_remove(pool, true);
		else if (pool->low_count)
			page = ion_page_pool_remove(pool, false);
		mutex_unlock(&pool->mutex);
	}

	return page;
}

void ion_page_pool_free(struct ion_page_pool *pool, struct page *page)
{
	int ret;

	BUG_ON(pool->order != compound_order(page));

	ret = ion_page_pool_add(pool, page);
	if (ret)
		ion_page_pool_free_pages(pool, page);
}

void ion_page_pool_free_immediate(struct ion_page_pool *pool, struct page *page)
{
	ion_page_pool_free_pages(pool, page);
}

static int ion_page_pool_total(struct ion_page_pool *pool, bool high)
{
	int count = pool->low_count;

	if (high)
		count += pool->high_count;

	return count << pool->order;
}

int ion_page_pool_shrink(struct ion_page_pool *pool, gfp_t gfp_mask,
				int nr_to_scan)
{
	bool high;
	int freed = 0;

	if (current_is_kswapd())
		high = true;
	else
		high = !!(gfp_mask & __GFP_HIGHMEM);

	if (nr_to_scan == 0)
		return ion_page_pool_total(pool, high);

	while (freed < nr_to_scan) {
		struct page *page;

		mutex_lock(&pool->mutex);
		if (pool->low_count) {
			page = ion_page_pool_remove(pool, false);
		} else if (high && pool->high_count) {
			page = ion_page_pool_remove(pool, true);
		} else {
			mutex_unlock(&pool->mutex);
			break;
		}
		mutex_unlock(&pool->mutex);
		ion_page_pool_free_pages(pool, page);
		freed += (1 << pool->order);
	}

	return freed;
}

struct ion_page_pool *ion_page_pool_create(gfp_t gfp_mask, unsigned int order,
			bool graphic_buffer_flag)
{
	struct ion_page_pool *pool = kmalloc(sizeof(struct ion_page_pool),
					     GFP_KERNEL);
	if (!pool)
		return NULL;
	pool->high_count = 0;
	pool->low_count = 0;
	INIT_LIST_HEAD(&pool->low_items);
	INIT_LIST_HEAD(&pool->high_items);
	pool->gfp_mask = gfp_mask | __GFP_COMP;
	pool->order = order;
	pool->graphic_buffer_flag = graphic_buffer_flag;
	mutex_init(&pool->mutex);
	plist_node_init(&pool->list, order);

	return pool;
}

void ion_page_pool_destroy(struct ion_page_pool *pool)
{
	kfree(pool);
}

static int __init ion_page_pool_init(void)
{
	return 0;
}

static void __exit ion_page_pool_exit(void)
{
}

module_init(ion_page_pool_init);
module_exit(ion_page_pool_exit);
