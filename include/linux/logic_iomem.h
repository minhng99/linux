/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Intel Corporation
 * Author: johannes@sipsolutions.net
 */
#ifndef __LOGIC_IOMEM_H
#define __LOGIC_IOMEM_H
#include <linux/types.h>
#include <linux/ioport.h>

/**
 * struct logic_iomem_ops - emulated IO memory ops
 * @read: read an 8, 16, 32 or 64 bit quantity from the given offset,
 *	size is given in bytes (1, 2, 4 or 8)
 *	(64-bit only necessary if CONFIG_64BIT is set)
 * @write: write an 8, 16 32 or 64 bit quantity to the given offset,
 *	size is given in bytes (1, 2, 4 or 8)
 *	(64-bit only necessary if CONFIG_64BIT is set)
 * @set: optional, for memset_io()
 * @copy_from: optional, for memcpy_fromio()
 * @copy_to: optional, for memcpy_toio()
 * @unmap: optional, this region is getting unmapped
 */
struct logic_iomem_ops {
	unsigned long (*read)(void *priv, unsigned int offset, int size);
	void (*write)(void *priv, unsigned int offset, int size,
		      unsigned long val);

	void (*set)(void *priv, unsigned int offset, u8 value, int size);
	void (*copy_from)(void *priv, void *buffer, unsigned int offset,
			  int size);
	void (*copy_to)(void *priv, unsigned int offset, const void *buffer,
			int size);

	void (*unmap)(void *priv);
};

/**
 * struct logic_iomem_region_ops - ops for an IO memory handler
 * @map: map a range within the supplied resource, must
 *	fill *ops with the ops and may fill *priv to be passed
 *	to the ops. The offset is given as the offset into the
 *	registered resource region.
 *	The return value is negative for errors, or >= 0 for
 *	success. On success, the return value is added to the
 *	offset for later ops, to allow for partial mappings.
 */
struct logic_iomem_region_ops {
	long (*map)(struct resource *resource, unsigned long offset, size_t size,
		    const struct logic_iomem_ops **ops,
		    void **priv);
};

/**
 * logic_iomem_add_region - register an IO memory region
 * @resource: the resource description for this region
 * @ops: the IO memory mapping ops for this resource
 */
int logic_iomem_add_region(struct resource *resource,
			   const struct logic_iomem_region_ops *ops);

/* Allocate and register an address range within the supplied resource limits. */
int logic_iomem_alloc_region(struct resource *resource,
			     const struct logic_iomem_region_ops *ops,
			     resource_size_t size, resource_size_t minimum,
			     resource_size_t maximum, resource_size_t alignment);

/* Consumers must release mappings and child resources before unregistering. */
int logic_iomem_remove_region(struct resource *resource);

#ifdef CONFIG_INDIRECT_IOMEM
#ifdef CONFIG_64BIT
#define LOGIC_IOMEM_ADDR_BASE 0xDEAD000000000000UL
#define LOGIC_IOMEM_ADDR_MASK 0xFFFFFFFF00000000UL
#else
#define LOGIC_IOMEM_ADDR_BASE 0x80000000UL
#define LOGIC_IOMEM_ADDR_MASK 0x80000000UL
#endif

static inline bool logic_iomem_is_addr(const volatile void __iomem *addr)
{
	return ((unsigned long __force)addr & LOGIC_IOMEM_ADDR_MASK) == LOGIC_IOMEM_ADDR_BASE;
}

/* Unregistered ranges return ERR_PTR(-ENOENT); provider failures remain distinct. */
void __iomem *logic_iomem_map(phys_addr_t offset, size_t size);
void __iomem *logic_iomem_ioremap(phys_addr_t offset, size_t size);
void logic_iomem_iounmap(volatile void __iomem *addr);
u8 logic_iomem_readb(const volatile void __iomem *addr);
u16 logic_iomem_readw(const volatile void __iomem *addr);
u32 logic_iomem_readl(const volatile void __iomem *addr);
void logic_iomem_writeb(u8 value, volatile void __iomem *addr);
void logic_iomem_writew(u16 value, volatile void __iomem *addr);
void logic_iomem_writel(u32 value, volatile void __iomem *addr);
#ifdef CONFIG_64BIT
u64 logic_iomem_readq(const volatile void __iomem *addr);
void logic_iomem_writeq(u64 value, volatile void __iomem *addr);
#endif
void logic_iomem_memset_io(volatile void __iomem *addr, int value, size_t size);
void logic_iomem_memcpy_fromio(void *buffer, const volatile void __iomem *addr, size_t size);
void logic_iomem_memcpy_toio(volatile void __iomem *addr, const void *buffer, size_t size);
#endif

#endif /* __LOGIC_IOMEM_H */
