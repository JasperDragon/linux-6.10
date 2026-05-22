/**************************************************************************
 *
 * Copyright (c) 2006-2007 Tungsten Graphics, Inc., Cedar Park, TX., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellström <thomas-at-tungstengraphics-dot-com>
 */

/*
 * 中文注释: DRM CPU 缓存管理 (Cache Management)
 *
 * 本文件实现了 DRM 子系统的 CPU 缓存管理功能, 包括缓存刷新 (cache
 * flush) 和高效内存拷贝操作。这些操作对于确保 GPU 和 CPU 之间数据
 * 一致性至关重要。
 *
 * 主要功能:
 *   1. drm_clflush_pages: 刷新指定页数组的 CPU 数据缓存行 (dcache)
 *   2. drm_clflush_sg: 刷新散聚列表 (scatter-gather) 中所有页的缓存
 *   3. drm_clflush_virt_range: 刷新指定虚拟地址范围的缓存行
 *   4. drm_need_swiotlb: 判断是否需要使用 SWIOTLB (Xen 等场景)
 *   5. drm_memcpy_from_wc: 从可能为 write-combining (WC) 的源内存
 *      执行高效拷贝, 使用非临时指令 (movntdqa) 避免缓存污染
 *
 * 架构支持:
 *   - x86: 使用 clflush/clflushopt 指令进行缓存刷新, 使用 MOVNTDQA
 *     进行非临时加载 (NT load) 以避免缓存污染
 *   - powerpc: 使用 flush_dcache_range() 刷新缓存
 *   - 其他架构: 发出警告 (需要实现架构特定的支持)
 *
 * 缓存刷新使用内存屏障 (mfence/sfence) 保证顺序, 因为 clflushopt
 * 是无序指令。
 */

#include <linux/cc_platform.h>
#include <linux/export.h>
#include <linux/highmem.h>
#include <linux/ioport.h>
#include <linux/iosys-map.h>
#include <xen/xen.h>

#include <drm/drm_cache.h>

/* A small bounce buffer that fits on the stack. */
#define MEMCPY_BOUNCE_SIZE 128

#if defined(CONFIG_X86)
#include <asm/smp.h>

/*
 * clflushopt is an unordered instruction which needs fencing with mfence or
 * sfence to avoid ordering issues.  For drm_clflush_page this fencing happens
 * in the caller.
 */
static void
drm_clflush_page(struct page *page)
{
	uint8_t *page_virtual;
	unsigned int i;
	const int size = boot_cpu_data.x86_clflush_size;

	if (unlikely(page == NULL))
		return;

	page_virtual = kmap_atomic(page);
	for (i = 0; i < PAGE_SIZE; i += size)
		clflushopt(page_virtual + i);
	kunmap_atomic(page_virtual);
}

static void drm_cache_flush_clflush(struct page *pages[],
				    unsigned long num_pages)
{
	unsigned long i;

	mb(); /*Full memory barrier used before so that CLFLUSH is ordered*/
	for (i = 0; i < num_pages; i++)
		drm_clflush_page(*pages++);
	mb(); /*Also used after CLFLUSH so that all cache is flushed*/
}
#endif

/**
 * 中文注释: 刷新一组页面的数据缓存行
 * 刷新数组中每个页面所对应的所有 CPU 数据缓存行。在 x86 上优先使用
 * CLFLUSH 指令 (逐个页面刷新), 如果不支持则使用 wbinvd_on_all_cpus()
 * (全局缓存无效化, 开销较大)。在 powerpc 上使用 flush_dcache_range()。
 * 确保 GPU 和 CPU 之间共享内存的数据一致性。
 *
 * drm_clflush_pages - Flush dcache lines of a set of pages.
 * @pages: List of pages to be flushed.
 * @num_pages: Number of pages in the array.
 *
 * Flush every data cache line entry that points to an address belonging
 * to a page in the array.
 */
void
drm_clflush_pages(struct page *pages[], unsigned long num_pages)
{

#if defined(CONFIG_X86)
	if (static_cpu_has(X86_FEATURE_CLFLUSH)) {
		drm_cache_flush_clflush(pages, num_pages);
		return;
	}

	wbinvd_on_all_cpus();

#elif defined(__powerpc__)
	unsigned long i;

	for (i = 0; i < num_pages; i++) {
		struct page *page = pages[i];
		void *page_virtual;

		if (unlikely(page == NULL))
			continue;

		page_virtual = kmap_atomic(page);
		flush_dcache_range((unsigned long)page_virtual,
				   (unsigned long)page_virtual + PAGE_SIZE);
		kunmap_atomic(page_virtual);
	}
#else
	WARN_ONCE(1, "Architecture has no drm_cache.c support\n");
#endif
}
EXPORT_SYMBOL(drm_clflush_pages);

/**
 * 中文注释: 刷新散聚列表中所有页面的缓存行
 * 遍历散聚列表 (scatter-gather table) 中的所有页面, 刷新每个页面
 * 对应的数据缓存行。在 x86 上使用 CLFLUSH + 内存屏障确保顺序。
 *
 * drm_clflush_sg - Flush dcache lines pointing to a scather-gather.
 * @st: struct sg_table.
 *
 * Flush every data cache line entry that points to an address in the
 * sg.
 */
void
drm_clflush_sg(struct sg_table *st)
{
#if defined(CONFIG_X86)
	if (static_cpu_has(X86_FEATURE_CLFLUSH)) {
		struct sg_page_iter sg_iter;

		mb(); /*CLFLUSH is ordered only by using memory barriers*/
		for_each_sgtable_page(st, &sg_iter, 0)
			drm_clflush_page(sg_page_iter_page(&sg_iter));
		mb(); /*Make sure that all cache line entry is flushed*/

		return;
	}

	wbinvd_on_all_cpus();
#else
	WARN_ONCE(1, "Architecture has no drm_cache.c support\n");
#endif
}
EXPORT_SYMBOL(drm_clflush_sg);

/**
 * 中文注释: 刷新指定虚拟地址范围的缓存行
 * 刷新从 @addr 开始的 @length 字节范围内的所有缓存行。将起始地址
 * 向下对齐到缓存行边界, 然后逐缓存行执行 clflushopt, 最后再刷新
 * 末尾字节以保证序列化。全程使用内存屏障确保刷新的顺序正确性。
 *
 * drm_clflush_virt_range - Flush dcache lines of a region
 * @addr: Initial kernel memory address.
 * @length: Region size.
 *
 * Flush every data cache line entry that points to an address in the
 * region requested.
 */
void
drm_clflush_virt_range(void *addr, unsigned long length)
{
#if defined(CONFIG_X86)
	if (static_cpu_has(X86_FEATURE_CLFLUSH)) {
		const int size = boot_cpu_data.x86_clflush_size;
		void *end = addr + length;

		addr = (void *)(((unsigned long)addr) & -size);
		mb(); /*CLFLUSH is only ordered with a full memory barrier*/
		for (; addr < end; addr += size)
			clflushopt(addr);
		clflushopt(end - 1); /* force serialisation */
		mb(); /*Ensure that every data cache line entry is flushed*/
		return;
	}

	wbinvd_on_all_cpus();
#else
	WARN_ONCE(1, "Architecture has no drm_cache.c support\n");
#endif
}
EXPORT_SYMBOL(drm_clflush_virt_range);

/**
 * 中文注释: 判断是否需要使用 SWIOTLB
 * 检查当前系统是否需要软件 I/O TLB 反弹缓冲 (SWIOTLB)。以下情况
 * 需要 SWIOTLB:
 *   1. Xen 半虚拟化主机 (Xen PV domain): DMA 传输需要反弹缓冲
 *   2. 内存加密启用 (如 AMD SEV): 原因与 Xen 类似
 *   3. 系统中存在超出 DMA 寻址范围的内存: 如果 I/O 内存的物理地址
 *      超过了指定的 dma_bits 寻址范围, 则需要 SWIOTLB
 *
 * bool drm_need_swiotlb(int dma_bits)
 */
{
	struct resource *tmp;
	resource_size_t max_iomem = 0;

	/*
	 * Xen paravirtual hosts require swiotlb regardless of requested dma
	 * transfer size.
	 *
	 * NOTE: Really, what it requires is use of the dma_alloc_coherent
	 *       allocator used in ttm_dma_populate() instead of
	 *       ttm_populate_and_map_pages(), which bounce buffers so much in
	 *       Xen it leads to swiotlb buffer exhaustion.
	 */
	if (xen_pv_domain())
		return true;

	/*
	 * Enforce dma_alloc_coherent when memory encryption is active as well
	 * for the same reasons as for Xen paravirtual hosts.
	 */
	if (cc_platform_has(CC_ATTR_MEM_ENCRYPT))
		return true;

	for (tmp = iomem_resource.child; tmp; tmp = tmp->sibling)
		max_iomem = max(max_iomem,  tmp->end);

	return max_iomem > ((u64)1 << dma_bits);
}
EXPORT_SYMBOL(drm_need_swiotlb);

static void memcpy_fallback(struct iosys_map *dst,
			    const struct iosys_map *src,
			    unsigned long len)
{
	if (!dst->is_iomem && !src->is_iomem) {
		memcpy(dst->vaddr, src->vaddr, len);
	} else if (!src->is_iomem) {
		iosys_map_memcpy_to(dst, 0, src->vaddr, len);
	} else if (!dst->is_iomem) {
		memcpy_fromio(dst->vaddr, src->vaddr_iomem, len);
	} else {
		/*
		 * Bounce size is not performance tuned, but using a
		 * bounce buffer like this is significantly faster than
		 * resorting to ioreadxx() + iowritexx().
		 */
		char bounce[MEMCPY_BOUNCE_SIZE];
		void __iomem *_src = src->vaddr_iomem;
		void __iomem *_dst = dst->vaddr_iomem;

		while (len >= MEMCPY_BOUNCE_SIZE) {
			memcpy_fromio(bounce, _src, MEMCPY_BOUNCE_SIZE);
			memcpy_toio(_dst, bounce, MEMCPY_BOUNCE_SIZE);
			_src += MEMCPY_BOUNCE_SIZE;
			_dst += MEMCPY_BOUNCE_SIZE;
			len -= MEMCPY_BOUNCE_SIZE;
		}
		if (len) {
			memcpy_fromio(bounce, _src, MEMCPY_BOUNCE_SIZE);
			memcpy_toio(_dst, bounce, MEMCPY_BOUNCE_SIZE);
		}
	}
}

#ifdef CONFIG_X86

static DEFINE_STATIC_KEY_FALSE(has_movntdqa);

static void __memcpy_ntdqa(void *dst, const void *src, unsigned long len)
{
	kernel_fpu_begin();

	while (len >= 4) {
		asm("movntdqa	(%0), %%xmm0\n"
		    "movntdqa 16(%0), %%xmm1\n"
		    "movntdqa 32(%0), %%xmm2\n"
		    "movntdqa 48(%0), %%xmm3\n"
		    "movaps %%xmm0,   (%1)\n"
		    "movaps %%xmm1, 16(%1)\n"
		    "movaps %%xmm2, 32(%1)\n"
		    "movaps %%xmm3, 48(%1)\n"
		    :: "r" (src), "r" (dst) : "memory");
		src += 64;
		dst += 64;
		len -= 4;
	}
	while (len--) {
		asm("movntdqa (%0), %%xmm0\n"
		    "movaps %%xmm0, (%1)\n"
		    :: "r" (src), "r" (dst) : "memory");
		src += 16;
		dst += 16;
	}

	kernel_fpu_end();
}

/*
 * __drm_memcpy_from_wc copies @len bytes from @src to @dst using
 * non-temporal instructions where available. Note that all arguments
 * (@src, @dst) must be aligned to 16 bytes and @len must be a multiple
 * of 16.
 */
static void __drm_memcpy_from_wc(void *dst, const void *src, unsigned long len)
{
	if (unlikely(((unsigned long)dst | (unsigned long)src | len) & 15))
		memcpy(dst, src, len);
	else if (likely(len))
		__memcpy_ntdqa(dst, src, len >> 4);
}

/**
 * 中文注释: 从可能为 WC (Write-Combining) 的源执行最优 memcpy
 * 从可能为 write-combining 属性的内存区域执行拷贝, 优先使用架构
 * 优化的非临时加载指令 (MOVNTDQA), 以避免缓存污染。在 x86 上,
 * MOVNTDQA 指令直接从内存加载到 XMM 寄存器而不填充缓存行, 这对于
 * 从 WC 内存读数特别高效 (WC 内存通常用于帧缓冲区等 GPU 内存)。
 * 如果架构不支持或源/目标不是 WC, 回退到普通 memcpy。
 *
 * drm_memcpy_from_wc - Perform the fastest available memcpy from a source
 * that may be WC.
 * @dst: The destination pointer
 * @src: The source pointer
 * @len: The size of the area o transfer in bytes
 *
 * Tries an arch optimized memcpy for prefetching reading out of a WC region,
 * and if no such beast is available, falls back to a normal memcpy.
 */
void drm_memcpy_from_wc(struct iosys_map *dst,
			const struct iosys_map *src,
			unsigned long len)
{
	if (WARN_ON(in_interrupt())) {
		memcpy_fallback(dst, src, len);
		return;
	}

	if (static_branch_likely(&has_movntdqa)) {
		__drm_memcpy_from_wc(dst->is_iomem ?
				     (void __force *)dst->vaddr_iomem :
				     dst->vaddr,
				     src->is_iomem ?
				     (void const __force *)src->vaddr_iomem :
				     src->vaddr,
				     len);
		return;
	}

	memcpy_fallback(dst, src, len);
}
EXPORT_SYMBOL(drm_memcpy_from_wc);

/**
 * 中文注释: WC memcpy 的一次性早期初始化
 * 在启动早期检查 CPU 是否支持 MOVNTDQA 指令 (SSE4.1 扩展) 以及
 * 是否运行在虚拟机中。在虚拟机中 VEX 前缀指令可能无法被模拟, 因此
 * 禁用在虚拟机中使用 MOVNTDQA。在裸机上如果 CPU 支持则启用
 * MOVNTDQA 加速路径。
 *
 * void drm_memcpy_init_early(void)
 */
void drm_memcpy_init_early(void)
{
	/*
	 * Some hypervisors (e.g. KVM) don't support VEX-prefix instructions
	 * emulation. So don't enable movntdqa in hypervisor guest.
	 */
	if (static_cpu_has(X86_FEATURE_XMM4_1) &&
	    !boot_cpu_has(X86_FEATURE_HYPERVISOR))
		static_branch_enable(&has_movntdqa);
}
#else
void drm_memcpy_from_wc(struct iosys_map *dst,
			const struct iosys_map *src,
			unsigned long len)
{
	WARN_ON(in_interrupt());

	memcpy_fallback(dst, src, len);
}
EXPORT_SYMBOL(drm_memcpy_from_wc);

void drm_memcpy_init_early(void)
{
}
#endif /* CONFIG_X86 */
