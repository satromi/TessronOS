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
 *	fits in one or is a list of page pointers. Data buffers are made of
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
 * Taken with interrupts disabled around the table of uncached DMA pages: disabling
 * interrupts alone keeps out only this processor.
 */
LOCAL T_SPLOCK	usbdma_lock;

/*
 * What the controller adds to a physical address to reach memory, and
 * whether it sees the caches.
 */
#ifdef RPI5
#define USB_DMA_BUS_OFFSET	RP1_DMA_BUS_OFFSET
#define USB_DMA_UNCACHED	1
#else
#define USB_DMA_BUS_OFFSET	0
#define USB_DMA_UNCACHED	0
#endif

#if USB_DMA_UNCACHED
/*
 * The uncached mapping of each page handed out, so that giving it back
 * finds the page frame again. A small table rather than a list: a list
 * would have to live in memory of its own.
 */
#define USB_DMA_MAXPAGES	1024

typedef struct {
	void	*va;
	UD	pa;
} DMAPAGE;

LOCAL DMAPAGE	dma_page[USB_DMA_MAXPAGES];

LOCAL void dcache_clean_inval( UBINT a, UBINT len )
{
	UBINT	end = a + len;

	Asm("dsb sy" ::: "memory");
	for ( a &= ~63ULL; a < end; a += 64 ) {
		Asm("dc civac, %0" :: "r"(a) : "memory");
	}
	Asm("dsb sy" ::: "memory");
}
#endif

EXPORT void *knl_usb_page_alloc( UD *p_bus )
{
	PFRAME	*pf;
	UD	pa;
	void	*va;

	pf = knl_alloc_pages(0, ZONE_DMA32, KAF_ZERO | KAF_DMA32);
	if ( pf == NULL ) {
		return NULL;
	}
	pa = knl_pf_to_pa(pf);
	va = PA2VA(pa);

#if USB_DMA_UNCACHED
	{
		UINT	imask;
		INT	i;

		/* the zeroes went through the cache: push them out first */
		dcache_clean_inval((UBINT)va, USB_PAGE);
		va = knl_vmap_pa(pa, 1, VMAP_NOCACHE);
		if ( va == NULL ) {
			knl_free_pages(pf, 0);
			return NULL;
		}
		ISpinLock(&usbdma_lock, &imask);
		for ( i = 0; i < USB_DMA_MAXPAGES; i++ ) {
			if ( dma_page[i].va == NULL ) {
				dma_page[i].va = va;
				dma_page[i].pa = pa;
				break;
			}
		}
		ISpinUnlock(&usbdma_lock, &imask);
		if ( i == USB_DMA_MAXPAGES ) {
			knl_vunmap_pa(va, 1);
			knl_free_pages(pf, 0);
			return NULL;
		}
	}
#endif
	if ( p_bus != NULL ) {
		*p_bus = pa + USB_DMA_BUS_OFFSET;
	}

	return va;
}

EXPORT void knl_usb_page_free( void *page )
{
	PFRAME	*pf;
	UD	pa;

	if ( page == NULL ) {
		return;
	}
#if USB_DMA_UNCACHED
	{
		UINT	imask;
		INT	i;

		pa = 0;
		ISpinLock(&usbdma_lock, &imask);
		for ( i = 0; i < USB_DMA_MAXPAGES; i++ ) {
			if ( dma_page[i].va == page ) {
				pa = dma_page[i].pa;
				dma_page[i].va = NULL;
				break;
			}
		}
		ISpinUnlock(&usbdma_lock, &imask);
		if ( i == USB_DMA_MAXPAGES ) {
			return;			/* not one of ours */
		}
		knl_vunmap_pa(page, 1);
	}
#else
	pa = VA2PA((UBINT)page);
#endif
	pf = knl_pa_to_pf(pa);
	if ( pf != NULL ) {
		knl_free_pages(pf, 0);
	}
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
