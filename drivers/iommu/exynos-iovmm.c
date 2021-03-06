/* linux/drivers/iommu/exynos_iovmm.c
 *
 * Copyright (c) 2011-2012 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifdef CONFIG_EXYNOS_IOMMU_DEBUG
#define DEBUG
#endif

#include <linux/kernel.h>
#include <linux/hardirq.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#include <linux/err.h>

#include <plat/iovmm.h>
#include <plat/sysmmu.h>
#include <plat/cpu.h>

#include <mach/sysmmu.h>

#include "exynos-iommu.h"

#define SZ_768M (SZ_512M + SZ_256M)

static int find_iovm_id(struct exynos_iovmm *vmm,
		struct exynos_vm_region *region)
{
	int i;

	if (region->start < IOVA_START || region->start > (IOVA_START + IOVM_SIZE))
			return -EINVAL;

	for (i = 0; i < MAX_NUM_PLANE; i++) {
		if (region->start < (vmm->iova_start[i] + vmm->iovm_size[i]))
			return i;
	}
	return -EINVAL;
}


/* alloc_iovm_region - Allocate IO virtual memory region
 * vmm: virtual memory allocator
 * size: total size to allocate vm region from @vmm.
 * align: alignment constraints of the allocated virtual address
 * max_align: maximum alignment of allocated virtual address. allocated address
 *            does not need to satisfy larger alignment than max_align.
 * exact_align_mask: constraints of the special case that allocated address
 *            must satisfy when it is multiple of align but of max_align.
 *            If this is not 0, allocated address must satisfy the following
 *            constraint:
 *            ((allocated address) % max_align) / align = exact_align_mask
 * offset: must be smaller than PAGE_SIZE. Just a valut to be added to the
 *         allocated virtual address. This does not effect to the allocaded size
 *         and address.
 *
 * This function returns allocated IO virtual address that satisfies the given
 * constraints. Returns 0 if this function is not able to allocate IO virtual
 * memory
 */
#define DUMMY_VMSIZE	(SZ_256K + SZ_1M)
#define DUMMY_VMALIGN	SZ_256K

#define DUMMY_VMASK	~((SZ_1M / PAGE_SIZE) - 1)

static dma_addr_t alloc_iovm_region(struct exynos_iovmm *vmm, size_t size,
			size_t align, size_t max_align, size_t exact_align_mask,
			off_t offset, int id)
{
	dma_addr_t index = 0;
	dma_addr_t vstart;
	size_t vsize;
	unsigned long end, i;
	struct exynos_vm_region *region;

	BUG_ON(align & (align - 1));
	BUG_ON(offset >= PAGE_SIZE);

	/* To avoid allocating prefetched iovm region */
	vsize = ALIGN(size + DUMMY_VMSIZE, DUMMY_VMALIGN) >> PAGE_SHIFT;
	align >>= PAGE_SHIFT;
	exact_align_mask >>= PAGE_SHIFT;
	max_align >>= PAGE_SHIFT;

	spin_lock(&vmm->bitmap_lock);
again:
	index = find_next_zero_bit(vmm->vm_map[id],
			IOVM_NUM_PAGES(vmm->iovm_size[id]), index);

	if (align) {
		if (exact_align_mask) {
			if ((index & ~(align - 1) & (max_align - 1)) >
							exact_align_mask)
				index = ALIGN(index, max_align);
			index |= exact_align_mask;
		} else {
			index = ALIGN(index, align);
		}

		if (index >= IOVM_NUM_PAGES(vmm->iovm_size[id])) {
			spin_unlock(&vmm->bitmap_lock);
			return 0;
		}

		if (test_bit(index, vmm->vm_map[id]))
			goto again;
	}

	end = index + vsize;

	if (end >= IOVM_NUM_PAGES(vmm->iovm_size[id])) {
		spin_unlock(&vmm->bitmap_lock);
		return 0;
	}

	i = find_next_bit(vmm->vm_map[id], end, index);
	if (i < end) {
		index = i + 1;
		goto again;
	}

	vsize = vsize - ((index + vsize) - ((index + vsize) & DUMMY_VMASK));

	bitmap_set(vmm->vm_map[id], index, vsize);

	spin_unlock(&vmm->bitmap_lock);

	vstart = (index << PAGE_SHIFT) + vmm->iova_start[id] + offset;

	region = kmalloc(sizeof(*region), GFP_KERNEL);
	if (unlikely(!region)) {
		spin_lock(&vmm->bitmap_lock);
		bitmap_clear(vmm->vm_map[id], index, vsize);
		spin_unlock(&vmm->bitmap_lock);
		return 0;
	}

	INIT_LIST_HEAD(&region->node);
	region->start = vstart;
	region->size = vsize << PAGE_SHIFT;
	region->dummy = (region->start + region->size - offset) -
		ALIGN(region->start + size + DUMMY_VMALIGN, DUMMY_VMALIGN);

	if (region->dummy > region->size)
		panic("%s: dummy %#x, start %#x, vsize %#x, size %#x\n",
			__func__, region->dummy, region->start,
			region->size, size);

	spin_lock(&vmm->vmlist_lock);
	list_add_tail(&region->node, &vmm->regions_list);
	vmm->allocated_size[id] += region->size;
	vmm->num_areas[id]++;
	spin_unlock(&vmm->vmlist_lock);

	return region->start;
}

struct exynos_vm_region *find_iovm_region(struct exynos_iovmm *vmm,
							dma_addr_t iova)
{
	struct exynos_vm_region *region;

	spin_lock(&vmm->vmlist_lock);

	list_for_each_entry(region, &vmm->regions_list, node) {
		if (region->start <= iova &&
			(region->start + region->size) > iova) {
			spin_unlock(&vmm->vmlist_lock);
			return region;
		}
	}

	spin_unlock(&vmm->vmlist_lock);

	return NULL;
}

static struct exynos_vm_region *remove_iovm_region(struct exynos_iovmm *vmm,
							dma_addr_t iova)
{
	struct exynos_vm_region *region;

	spin_lock(&vmm->vmlist_lock);

	list_for_each_entry(region, &vmm->regions_list, node) {
		if (region->start == iova) {
			int id;

			id = find_iovm_id(vmm, region);
			if (id < 0)
				continue;

			list_del(&region->node);
			vmm->allocated_size[id] -= region->size;
			vmm->num_areas[id]--;
			spin_unlock(&vmm->vmlist_lock);
			return region;
		}
	}

	spin_unlock(&vmm->vmlist_lock);

	return NULL;
}

static void free_iovm_region(struct exynos_iovmm *vmm,
				struct exynos_vm_region *region)
{
	int id;

	if (!region)
		return;

	id = find_iovm_id(vmm, region);
	if (id < 0) {
		kfree(region);
		return;
	}

	spin_lock(&vmm->bitmap_lock);
	bitmap_clear(vmm->vm_map[id],
			(region->start - vmm->iova_start[id]) >> PAGE_SHIFT,
			region->size >> PAGE_SHIFT);
	spin_unlock(&vmm->bitmap_lock);

	kfree(region);
}

static dma_addr_t add_iovm_region(struct exynos_iovmm *vmm,
					dma_addr_t start, size_t size)
{
	struct exynos_vm_region *region, *pos;

	region = kmalloc(sizeof(*region), GFP_KERNEL);
	if (!region)
		return 0;

	INIT_LIST_HEAD(&region->node);
	region->start = start;
	region->size = size;

	spin_lock(&vmm->vmlist_lock);

	list_for_each_entry(pos, &vmm->regions_list, node) {
		if ((start < (pos->start + pos->size)) &&
					((start + size) > pos->start)) {
			spin_unlock(&vmm->vmlist_lock);
			kfree(region);
			return 0;
		}
	}

	list_add(&region->node, &vmm->regions_list);

	spin_unlock(&vmm->vmlist_lock);

	return start;
}

static void show_iovm_regions(struct exynos_iovmm *vmm)
{
	struct exynos_vm_region *pos;

	pr_err("LISTING IOVMM REGIONS...\n");
	spin_lock(&vmm->vmlist_lock);
	list_for_each_entry(pos, &vmm->regions_list, node) {
		pr_err("REGION: %#x ~ %#x (SIZE: %#x)\n", pos->start,
				pos->start + pos->size, pos->size);
	}
	spin_unlock(&vmm->vmlist_lock);
	pr_err("END OF LISTING IOVMM REGIONS...\n");
}

int iovmm_activate(struct device *dev)
{
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);

	return iommu_attach_device(vmm->domain, dev);
}

void iovmm_deactivate(struct device *dev)
{
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);

	iommu_detach_device(vmm->domain, dev);
}

/* iovmm_reserve - reserve IO virtual memory for the given device
 * dev: device that has IO virtual address space managed by IOVMM
 * base: address that point to start of reserved memory region
 * size: size in bytes to be mapped and accessed by dev.
 */
int iovmm_reserve(struct device *dev, dma_addr_t base, size_t size)
{
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);
	struct exynos_vm_region region;
	dma_addr_t reserve_end, index = 0;
	size_t vsize;
	unsigned long end, i;
	int id;

	reserve_end = PAGE_ALIGN(base + size);
	base = round_down(base, PAGE_SIZE);
	size = reserve_end - base;
	if (WARN_ON((base + size) >= (IOVA_START + IOVM_SIZE))) {
		dev_err(dev,
			"Unable to reserve IOVM from %#x to %#x",
			base, base + size);
		return -EINVAL;
	}

	region.start = base;
	region.size = size;
	id = find_iovm_id(vmm, &region);
	if (id < 0) {
		pr_err("Base address doesn't exist within IOVA space");
		return id;
	}

	vsize = size >> PAGE_SHIFT;
	index = (base >> PAGE_SHIFT) - (vmm->iova_start[id] >> PAGE_SHIFT);
	end = index + vsize;
	if (end >= IOVM_NUM_PAGES(vmm->iovm_size[id])) {
		pr_err("Reserved size is out of IOVA space");
		return -EINVAL;
	}

	spin_lock(&vmm->bitmap_lock);

	i = find_next_bit(vmm->vm_map[id], end, index);
	if (i < end) {
		spin_unlock(&vmm->bitmap_lock);
		pr_err("Failed to reserve IOVA at %#x", base);
		return -EINVAL;
	}

	bitmap_set(vmm->vm_map[id], index, vsize);

	spin_unlock(&vmm->bitmap_lock);

	pr_info("Reserved IOVM region from %#x to %#x successfully",
			base, base + size);

	return 0;
}

/* iovmm_map - allocate and map IO virtual memory for the given device
 * dev: device that has IO virtual address space managed by IOVMM
 * sg: list of physically contiguous memory chunks. The preceding chunk needs to
 *     be larger than the following chunks in sg for efficient mapping and
 *     performance. If elements of sg are more than one, physical address of
 *     each chunk needs to be aligned by its size for efficent mapping and TLB
 *     utilization.
 * offset: offset in bytes to be mapped and accessed by dev.
 * size: size in bytes to be mapped and accessed by dev.
 *
 * This function allocates IO virtual memory for the given device and maps the
 * given physical memory conveyed by sg into the allocated IO memory region.
 * Returns allocated IO virtual address if it allocates and maps successfull.
 * Otherwise, minus error number. Caller must check if the return value of this
 * function with IS_ERR_VALUE().
 */
dma_addr_t iovmm_map(struct device *dev, struct scatterlist *sg, off_t offset,
		size_t size, enum dma_data_direction direction, int id)
{
	off_t start_off;
	dma_addr_t addr, start = 0;
	size_t mapped_size = 0;
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);
	size_t exact_align_mask = 0;
	size_t max_align, align;
	int ret = 0;
	struct scatterlist *tsg;

	if ((id < 0) || (id >= MAX_NUM_PLANE)) {
		dev_err(dev, "%s: Invalid plane ID %d\n", __func__, id);
		return -EINVAL;
	}

	for (; (sg != NULL) && (sg_dma_len(sg) < offset); sg = sg_next(sg))
		offset -= sg_dma_len(sg);

	if (sg == NULL) {
		dev_err(dev, "IOVMM: invalid offset to %s.\n", __func__);
		return -EINVAL;
	}

	if (soc_is_exynos5410())
		id = 0;
	else if (direction == DMA_FROM_DEVICE)
		id += vmm->inplanes;

	if (id >= (vmm->inplanes + vmm->onplanes)) {
		dev_err(dev, "%s: id(%d) is larger than the number of IOVMs\n",
				__func__, id);
		return -EINVAL;
	}

	tsg = sg;

	start_off = offset_in_page(sg_phys(sg) + offset);
	size = PAGE_ALIGN(size + start_off);
	offset = round_down(offset, PAGE_SIZE);

	if (size >= SECT_SIZE)
		max_align = SECT_SIZE;
	else if (size < LPAGE_SIZE)
		max_align = SPAGE_SIZE;
	else
		max_align = LPAGE_SIZE;

	if (sg_next(sg) == NULL) {/* physically contiguous chunk */
		/* 'align' must be biggest 2^n that satisfies:
		 * 'address of physical memory' % 'align' = 0
		 */
		align = 1 << __ffs(page_to_phys(sg_page(sg)));

		exact_align_mask = page_to_phys(sg_page(sg)) & (max_align - 1);

		if ((size - exact_align_mask) < max_align) {
			max_align /= 16;
			exact_align_mask = exact_align_mask & (max_align - 1);
		}

		if (align > max_align)
			align = max_align;

		exact_align_mask &= ~(align - 1);
	} else {
		align = 1 << __ffs(page_to_phys(sg_page(sg)));
		align = min_t(size_t, align, max_align);
		max_align = align;
	}

	start = alloc_iovm_region(vmm, size, align, max_align,
				exact_align_mask, start_off, id);
	if (!start) {
		spin_lock(&vmm->vmlist_lock);
		dev_err(dev, "%s: Not enough IOVM space to allocate %#x/%#x\n",
				__func__, size, align);
		dev_err(dev, "%s: Total %#x (%d), Allocated %#x , Chunks %d\n",
				__func__, vmm->iovm_size[id], id,
				vmm->allocated_size[id], vmm->num_areas[id]);
		spin_unlock(&vmm->vmlist_lock);
		ret = -ENOMEM;
		goto err_map_nomem;
	}

	addr = start - start_off;
	do {
		phys_addr_t phys;
		size_t len;

		phys = sg_phys(sg);
		len = sg_dma_len(sg);

		/* if back to back sg entries are contiguous consolidate them */
		while (sg_next(sg) &&
		       sg_phys(sg) + sg_dma_len(sg) == sg_phys(sg_next(sg))) {
			len += sg_dma_len(sg_next(sg));
			sg = sg_next(sg);
		}

		if (offset > 0) {
			len -= offset;
			phys += offset;
			offset = 0;
		}

		if (offset_in_page(phys)) {
			len += offset_in_page(phys);
			phys = round_down(phys, PAGE_SIZE);
		}

		len = PAGE_ALIGN(len);

		if (len > (size - mapped_size))
			len = size - mapped_size;

		ret = iommu_map(vmm->domain, addr, phys, len, 0);
		if (ret) {
			dev_err(dev, "iommu_map failed w/ err: %d\n", ret);
			break;
		}

		addr += len;
		mapped_size += len;
	} while ((sg = sg_next(sg)) && (mapped_size < size));

	BUG_ON(mapped_size > size);

	if (mapped_size < size) {
		dev_err(dev, "mapped_size(%d) is smaller than size(%d)\n",
				mapped_size, size);
		if (!ret) {
			dev_err(dev, "ret: %d\n", ret);
			ret = -EINVAL;
		}
		goto err_map_map;
	}

	dev_dbg(dev, "IOVMM: Allocated VM region @ %#x/%#x bytes.\n",
								start, size);

	return start;

err_map_map:
	iommu_unmap(vmm->domain, start - start_off, mapped_size);
	free_iovm_region(vmm, remove_iovm_region(vmm, start));

	dev_err(dev,
	"Failed(%d) to map IOVMM REGION %#lx ~ %#lx (SIZE: %#x, mapped: %#x)\n",
		ret, start - start_off, start - start_off + size,
		size, mapped_size);
	addr = 0;
	do {
		pr_err("SGLIST[%d].size = %#x\n", addr++, tsg->length);
	} while ((tsg = sg_next(tsg)));

	show_iovm_regions(vmm);

err_map_nomem:
	dev_dbg(dev, "IOVMM: Failed to allocated VM region for %#x bytes.\n",
									size);
	return (dma_addr_t)ret;
}

void iovmm_unmap(struct device *dev, dma_addr_t iova)
{
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);
	struct exynos_vm_region *region;
	size_t unmap_size;

	/* This function must not be called in IRQ handlers */
	BUG_ON(in_irq());

	region = remove_iovm_region(vmm, iova);
	if (region) {
		size_t mapped_size;

		if (WARN_ON(region->start != iova)) {
			dev_err(dev,
			"IOVMM: iova %#x and region %#x @ %#x mismatch\n",
				iova, region->size, region->start);
			show_iovm_regions(vmm);
			/* reinsert iovm region */
			add_iovm_region(vmm, region->start, region->size);
			kfree(region);
			return;
		}

		mapped_size = region->size - region->dummy;

		unmap_size = iommu_unmap(vmm->domain, iova & PAGE_MASK,
							mapped_size);
		if (unlikely(unmap_size != mapped_size)) {
			dev_err(dev, "Failed to unmap IOVMM REGION %#x ~ %#x "\
				"(SIZE: %#x, iova: %#x, unmapped: %#x)\n",
				region->start, region->start + mapped_size,
				mapped_size, iova, unmap_size);
			show_iovm_regions(vmm);
			kfree(region);
			BUG();
			return;
		}

		exynos_sysmmu_tlb_invalidate(dev);

		free_iovm_region(vmm, region);

		dev_dbg(dev, "IOVMM: Unmapped %#x bytes from %#x.\n",
						unmap_size, iova);
	} else {
		dev_err(dev, "IOVMM: No IOVM region %#x to free.\n", iova);
	}
}

int iovmm_map_oto(struct device *dev, phys_addr_t phys, size_t size)
{
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);
	int ret;

	BUG_ON(!IS_ALIGNED(phys, PAGE_SIZE));
	BUG_ON(!IS_ALIGNED(size, PAGE_SIZE));

	if (WARN_ON((phys < PHYS_OFFSET) ||
			(phys > IOVA_START + IOVM_SIZE))) {
		dev_err(dev,
			"Unable to create one to one mapping for %#x @ %#x\n",
			size, phys);
		return -EINVAL;
	}

	if (!add_iovm_region(vmm, (dma_addr_t)phys, size))
		return -EADDRINUSE;

	ret = iommu_map(vmm->domain, (dma_addr_t)phys, phys, size, 0);
	if (ret < 0)
		free_iovm_region(vmm,
				remove_iovm_region(vmm, (dma_addr_t)phys));

	return ret;
}

void iovmm_unmap_oto(struct device *dev, phys_addr_t phys)
{
	struct exynos_iovmm *vmm = exynos_get_iovmm(dev);
	struct exynos_vm_region *region;
	size_t unmap_size;

	/* This function must not be called in IRQ handlers */
	BUG_ON(in_irq());
	BUG_ON(!IS_ALIGNED(phys, PAGE_SIZE));

	region = remove_iovm_region(vmm, (dma_addr_t)phys);
	if (region) {
		unmap_size = iommu_unmap(vmm->domain, (dma_addr_t)phys,
							region->size);
		WARN_ON(unmap_size != region->size);

		exynos_sysmmu_tlb_invalidate(dev);

		free_iovm_region(vmm, region);

		dev_dbg(dev, "IOVMM: Unmapped %#x bytes from %#x.\n",
						unmap_size, phys);
	}
}

int exynos_create_iovmm(struct device *dev, int inplanes, int onplanes)
{
	static unsigned long iovmcfg[MAX_NUM_PLANE + 1][MAX_NUM_PLANE] = {
		{IOVM_SIZE, 0, 0, 0, 0, 0},
		{SZ_2G, IOVM_SIZE - SZ_2G, 0, 0, 0, 0},
		{SZ_1G + SZ_256M, SZ_1G, SZ_1G, 0, 0, 0},
		{SZ_1G, SZ_1G + SZ_256M, SZ_512M, SZ_512M , 0, 0},
		{SZ_1G, SZ_1G, SZ_768M, SZ_256M, SZ_256M, 0},
		{SZ_1G, SZ_512M, SZ_256M, SZ_768M, SZ_512M, SZ_256M},
		{SZ_256M, 0, 0, 0, 0, 0}, /* special case for MFC */
		};
	int i, nplanes, ret = 0;
	int cfgsel;
	size_t sum_iovm = 0;
	struct exynos_iovmm *vmm;
	struct exynos_iommu_owner *owner = dev->archdata.iommu;

	if (owner->vmm_data)
		return 0;

	/* special case for MFC */
	if (inplanes > 6) {
		nplanes = 1;
		cfgsel = MAX_NUM_PLANE;
	} else {
		nplanes = inplanes + onplanes;
		cfgsel = nplanes - 1;
	}

	if (WARN_ON(!owner) || nplanes > MAX_NUM_PLANE || nplanes < 1) {
		ret = -ENOSYS;
		goto err_alloc_vmm;
	}

	vmm = kzalloc(sizeof(*vmm), GFP_KERNEL);
	if (!vmm) {
		ret = -ENOMEM;
		goto err_alloc_vmm;
	}

	for (i = 0; i < nplanes; i++) {
		/*As per MFC5.1 UM only 17 bits of a register is valid
			due to which there is a size limitation of 256MB */
#ifdef CONFIG_EXYNOS_MFC_V5
		if ((!strcmp(dev_name(dev), "s5p-mfc")) && (i == 0))
			vmm->iovm_size[i] = SZ_256M;
		else
#endif
			vmm->iovm_size[i] = iovmcfg[cfgsel][i];

		vmm->iova_start[i] = IOVA_START + sum_iovm;
		vmm->vm_map[i] = kzalloc(IOVM_BITMAP_SIZE(vmm->iovm_size[i]),
					 GFP_KERNEL);
		if (!vmm->vm_map[i]) {
			ret = -ENOMEM;
			goto err_setup_domain;
		}
		sum_iovm += iovmcfg[cfgsel][i];
		dev_info(dev, "IOVMM: IOVM SIZE = %#x B, IOVMM from %#x.\n",
				vmm->iovm_size[i], vmm->iova_start[i]);
	}

	vmm->inplanes = inplanes;
	vmm->onplanes = onplanes;
	vmm->domain = iommu_domain_alloc(&platform_bus_type);
	if (!vmm->domain) {
		ret = -ENOMEM;
		goto err_setup_domain;
	}

	spin_lock_init(&vmm->vmlist_lock);
	spin_lock_init(&vmm->bitmap_lock);

	INIT_LIST_HEAD(&vmm->regions_list);

	vmm->dev = dev;
	owner->vmm_data = vmm;

	dev_dbg(dev, "IOVMM: Created %#x B IOVMM from %#x.\n",
						IOVM_SIZE, IOVA_START);
	return 0;
err_setup_domain:
	for (i = 0; i < nplanes; i++)
		kfree(vmm->vm_map[i]);
	kfree(vmm);
err_alloc_vmm:
	dev_dbg(dev, "IOVMM: Failed to create IOVMM (%d)\n", ret);

	return ret;
}
