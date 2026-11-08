/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	usbdma.c
 *	Memory a USB controller can reach (design 10.4, 10.14).
 *
 *	Every structure the controller walks is one page, because a page is
 *	what the allocator gives out whole and every xHCI structure either
 *	fits in one or is a list of page pointers. The one exception is the
 *	scratchpad array, which grows to two pages for a controller that
 *	asks for more than 512 pages. Data buffers are made of
 *	pages too, reached one ring entry per page, so no transfer needs
 *	memory that is contiguous beyond a page.
 *
 *	The address written into such a structure is a bus address. On a
 *	machine where the controller sits on the same bus as memory that is
 *	the physical address; behind a bridge it is the physical address
 *	plus whatever offset the bridge was set up with. Keeping the two
 *	apart here means the controller code never has to know which kind
 *	of machine it is on.
 *
 *	The other thing that differs is whether the controller sees the
 *	processor's caches. The emulated machine's does; the Raspberry Pi
 *	5's controllers sit behind the RP1 and do not (design 10.8). There
 *	every page is mapped a second time, uncached, and only that mapping
 *	is used: a ring entry written by the processor then reaches memory
 *	without anyone having to remember to clean it, and an event the
 *	controller writes is read from memory rather than from a stale line.
 *	The lines the linear mapping may hold are cleaned and invalidated
 *	once, before the uncached mapping is handed out.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tk/smem.h>
#include "sysman/pfalloc.h"
#include "usbdev.h"

/*
 * What the controller adds to a physical address to reach memory, and
 * whether it sees the caches.
 */
#if defined(RPI5)
#define USB_DMA_BUS_OFFSET	RP1_DMA_BUS_OFFSET
#define USB_DMA_UNCACHED	1
#elif defined(RPI4)
/* the VL805 (or a card in the CM4's slot) through the PCIe inbound window, not coherent */
#define USB_DMA_BUS_OFFSET	PCIE_DMA_BUS_OFFSET
#define USB_DMA_UNCACHED	1
#else
#define USB_DMA_BUS_OFFSET	0
#define USB_DMA_UNCACHED	0
#endif

#if USB_DMA_UNCACHED
LOCAL void dcache_clean_inval( UBINT a, UBINT len )
{
	DCacheFlushRange((CONST void *)a, len);
}
#endif

/*
 * 2^order pages next to each other, zeroed. Most structures are one
 * page; the scratchpad array of a controller that lends itself more than
 * 512 pages is two.
 */
EXPORT void *knl_usb_pages_alloc( INT order, UD *p_bus )
{
	PFRAME	*pf;
	UD	pa;
	void	*va;

	pf = knl_alloc_pages((UINT)order, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		return NULL;
	}
	pa = knl_pf_to_pa(pf);
	va = PA2VA(pa);

#if USB_DMA_UNCACHED
	/* the zeroes went through the cache: push them out first */
	dcache_clean_inval((UBINT)va, (UBINT)USB_PAGE << order);
	va = knl_vmap_pa(pa, (UD)1 << order, VMAP_NOCACHE);
	if ( va == NULL ) {
		knl_free_pages(pf, (UINT)order);
		return NULL;
	}
#endif
	if ( p_bus != NULL ) {
		*p_bus = pa + USB_DMA_BUS_OFFSET;
	}

	return va;
}

/*
 * The page frame is found again from the mapping itself: the uncached
 * mapping's page table entry holds it, so no table of the pages handed
 * out is kept and their number is not bounded by one.
 */
EXPORT void knl_usb_pages_free( void *page, INT order )
{
	PFRAME	*pf;
	UD	pa;

	if ( page == NULL ) {
		return;
	}
#if USB_DMA_UNCACHED
	{
		void	*p = NULL;

		if ( ConvPhysicalAddress(page, USB_PAGE, &p) <= 0 ) {
			return;			/* not mapped: not one of ours */
		}
		pa = (UD)(UBINT)p;
		knl_vunmap_pa(page, (UD)1 << order);
	}
#else
	pa = VA2PA((UBINT)page);
#endif
	pf = knl_pa_to_pf(pa);
	if ( pf != NULL ) {
		knl_free_pages(pf, (UINT)order);
	}
}

EXPORT UD knl_usb_bus_pa( UD bus )
{
	return bus - USB_DMA_BUS_OFFSET;
}

EXPORT void *knl_usb_page_alloc( UD *p_bus )
{
	return knl_usb_pages_alloc(0, p_bus);
}

EXPORT void knl_usb_page_free( void *page )
{
	knl_usb_pages_free(page, 0);
}

/* ---------------------------------------------------------------- buffers */

EXPORT ER knl_dmabuf_alloc( DMABUF *b, INT bytes )
{
	INT	n = (bytes + USB_PAGE - 1) / USB_PAGE, i;

	knl_memset(b, 0, sizeof(*b));
	if ( n <= 0 || n > DMABUF_PAGES ) {
		return E_PAR;
	}
	for ( i = 0; i < n; i++ ) {
		b->virt[i] = (UB *)knl_usb_page_alloc(&b->bus[i]);
		if ( b->virt[i] == NULL ) {
			knl_dmabuf_free(b);
			return E_NOMEM;
		}
		b->npages = i + 1;
	}

	return E_OK;
}

EXPORT void knl_dmabuf_free( DMABUF *b )
{
	INT	i;

	for ( i = 0; i < b->npages; i++ ) {
		knl_usb_page_free(b->virt[i]);
	}
	knl_memset(b, 0, sizeof(*b));
}

/* A range that may cross pages that are not next to each other */
EXPORT void knl_dmabuf_read( DMABUF *b, INT off, UB *dst, INT len )
{
	USB_MB();
	while ( len > 0 ) {
		INT	pg = off / USB_PAGE, po = off % USB_PAGE;
		INT	n = USB_PAGE - po;

		if ( pg >= b->npages ) {
			break;
		}
		if ( n > len ) {
			n = len;
		}
		knl_memcpy(dst, b->virt[pg] + po, n);
		dst += n;
		off += n;
		len -= n;
	}
}

EXPORT void knl_dmabuf_write( DMABUF *b, INT off, CONST UB *src, INT len )
{
	while ( len > 0 ) {
		INT	pg = off / USB_PAGE, po = off % USB_PAGE;
		INT	n = USB_PAGE - po;

		if ( pg >= b->npages ) {
			break;
		}
		if ( n > len ) {
			n = len;
		}
		knl_memcpy(b->virt[pg] + po, src, n);
		src += n;
		off += n;
		len -= n;
	}
	USB_MB();
}
