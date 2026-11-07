/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2021 Intel Corporation
 * Author: johannes@sipsolutions.net
 */
#ifndef _LOGIC_IO_H
#define _LOGIC_IO_H
#include <linux/logic_iomem.h>

/* include this file into asm/io.h */

#ifdef CONFIG_INDIRECT_IOMEM

#ifdef CONFIG_INDIRECT_IOMEM_FALLBACK
/*
 * If you want emulated IO memory to fall back to 'normal' IO memory
 * if a region wasn't registered as emulated, then you need to have
 * all of the real_* functions implemented.
 */
#if !defined(real_ioremap) || !defined(real_iounmap) || \
    !defined(real_raw_readb) || !defined(real_raw_writeb) || \
    !defined(real_raw_readw) || !defined(real_raw_writew) || \
    !defined(real_raw_readl) || !defined(real_raw_writel) || \
    (defined(CONFIG_64BIT) && \
     (!defined(real_raw_readq) || !defined(real_raw_writeq))) || \
    !defined(real_memset_io) || \
    !defined(real_memcpy_fromio) || \
    !defined(real_memcpy_toio)
#error "Must provide fallbacks for real IO memory access"
#endif /* defined ... */
#endif /* CONFIG_INDIRECT_IOMEM_FALLBACK */

#define ioremap logic_iomem_ioremap
#define iounmap logic_iomem_iounmap
#define __raw_readb logic_iomem_readb
#define __raw_writeb logic_iomem_writeb
#define __raw_readw logic_iomem_readw
#define __raw_writew logic_iomem_writew
#define __raw_readl logic_iomem_readl
#define __raw_writel logic_iomem_writel
#ifdef CONFIG_64BIT
#define __raw_readq logic_iomem_readq
#define __raw_writeq logic_iomem_writeq
#endif
#define memset_io logic_iomem_memset_io
#define memcpy_fromio logic_iomem_memcpy_fromio
#define memcpy_toio logic_iomem_memcpy_toio

#endif /* CONFIG_INDIRECT_IOMEM */
#endif /* _LOGIC_IO_H */
