/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	elf.c
 *	ELF64 program loader (design 9.6).
 *
 *	Reads an ET_EXEC or static ET_DYN AArch64 image from a file, or from
 *	a record of a real object on the native store, and maps its PT_LOAD
 *	segments into a process space. Each segment is mapped with the
 *	permissions the program header asks for and filled through the
 *	kernel's linear map, so an executable segment is never writable
 *	(W^X, design 5.11). Dynamic linking is not supported; a PT_INTERP
 *	segment is refused.
 *
 *	A program read from a record is kept while a process of it lives,
 *	and another process of it maps the same read only pages instead of
 *	reading the program again ("shared images" below, design 5.1).
 */

#include <sys/machine.h>
#include "kernel.h"
#include "sysdepend/sysdepend.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/proc.h>
#include <ts/time.h>
#include "pfalloc.h"
#include "space.h"
#include <ts/umem.h>

#define EI_NIDENT	16
#define ET_EXEC		2
#define ET_DYN		3
#define PT_LOAD		1
#define PT_INTERP	3
#define PT_GNU_STACK	0x6474e551

#define PF_X		1
#define PF_W		2
#define PF_R		4

typedef struct {
	UB	e_ident[EI_NIDENT];
	UH	e_type;
	UH	e_machine;
	UW	e_version;
	UD	e_entry;
	UD	e_phoff;
	UD	e_shoff;
	UW	e_flags;
	UH	e_ehsize;
	UH	e_phentsize;
	UH	e_phnum;
	UH	e_shentsize;
	UH	e_shnum;
	UH	e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	UW	p_type;
	UW	p_flags;
	UD	p_offset;
	UD	p_vaddr;
	UD	p_paddr;
	UD	p_filesz;
	UD	p_memsz;
	UD	p_align;
} Elf64_Phdr;

#define PHDR_MAX	8

/*
 * Where the image is read from: a file or a record. `read` puts `len`
 * bytes from `off` in `buf` and answers how many it put, or an error.
 */
typedef struct {
	INT	(*read)( void *ctx, UD off, void *buf, UD len );
	void	*ctx;
} ELFSRC;

LOCAL INT file_read( void *ctx, UD off, void *buf, UD len )
{
	INT	fd = *(INT *)ctx;

	if ( fs_lseek(fd, (D)off, SEEK_SET_) != (D)off ) {
		return E_IO;
	}
	return fs_read(fd, buf, (SZ)len);
}

typedef struct {
	ID	od;
	INT	recno;
} RECSRC;

LOCAL INT rec_read( void *ctx, UD off, void *buf, UD len )
{
	RECSRC	*r = (RECSRC *)ctx;
	SZ	asize = 0;
	ER	er;

	er = ts_rea_rec(r->od, r->recno, (D)off, buf, (SZ)len, &asize);
	return ( er < E_OK ) ? (INT)er : (INT)asize;
}

LOCAL BOOL same_bytes( CONST void *a, CONST void *b, INT n )
{
	CONST UB	*p = (CONST UB *)a, *q = (CONST UB *)b;
	INT		i;

	for ( i = 0; i < n; i++ ) {
		if ( p[i] != q[i] ) return FALSE;
	}
	return TRUE;
}

/* ---------------------------------------------------------------- shared images */

/*
 * A program started from a record is read once while a process of it
 * lives (design 5.1). The first load keeps an image of it: the pages of
 * its read only segments (text, read only data) as they are mapped in
 * the first space, and a copy of the file part of each writable segment
 * as it was before the program ran. Another load of the same record
 * while the image is held maps the same read only pages into the new
 * space and copies the writable ones from the image, reading nothing
 * but the headers from the device. The read only pages are mapped with
 * PTE_SW_SHARED: no space owns them, the image does, and it gives them
 * back when the last process mapping them has its space taken down
 * (knl_elf_img_put). The image is told apart by its volume, object and
 * record and by the bytes of its ELF and program headers; opening the
 * object to write it, deleting it, or closing its volume forgets the
 * image (knl_elf_forget), so that a program written again is read again.
 */
typedef struct {
	UBINT	start;			/* the first page */
	UD	npages;			/* pages of the segment */
	UD	nfile;			/* writable: the pages holding file bytes (the rest is zeros) */
	UD	attr;			/* PTE_PAGE_* (with PTE_SW_SHARED when shared) */
	BOOL	shared;			/* read only: the pages are the image's */
	UD	*pa;			/* the pages (shared), or the copies (writable) */
} IMGSEG;

#define HEAD_MAX	( sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) * PHDR_MAX )

typedef struct progimg {
	struct progimg	*next;
	ID		vol;
	TS_UUID		uuid;
	INT		recno;
	UINT		users;		/* spaces that map it, and a load under way */
	BOOL		listed;		/* in img_list, to be found */
	BOOL		whole;		/* every segment kept: another load can use it */
	UD		entry, brk;
	UD		bytes;		/* of the read only pages */
	INT		nseg;
	IMGSEG		seg[PHDR_MAX];
	INT		headsz;
	UB		head[HEAD_MAX];
} PROGIMG;

LOCAL PROGIMG	*img_list = NULL;
LOCAL T_SPLOCK	img_lock;

/* Loads that used an image, and loads that read the program from a record */
LOCAL UW	img_hits = 0, img_reads = 0;

EXPORT void knl_elf_counts( UW *p_hits, UW *p_reads )
{
	if ( p_hits != NULL ) *p_hits = img_hits;
	if ( p_reads != NULL ) *p_reads = img_reads;
}

/* What an image holds given back: its pages, its tables, itself */
LOCAL void img_free( PROGIMG *e )
{
	INT	i;
	UD	k;

	for ( i = 0; i < e->nseg; i++ ) {
		IMGSEG	*s = &e->seg[i];

		if ( s->pa == NULL ) continue;
		for ( k = 0; k < s->npages; k++ ) {
			if ( s->pa[k] != 0 ) knl_free_pages(knl_pa_to_pf(s->pa[k]), 0);
		}
		Kfree(s->pa);
	}
	Kfree(e);
}

/* The image of a space let go: the last to let it go gives back its pages */
EXPORT void knl_elf_img_put( void *img )
{
	PROGIMG	*e = (PROGIMG *)img, **pp;
	UINT	imask;
	BOOL	last;

	if ( e == NULL ) return;
	ISpinLock(&img_lock, &imask);
	last = (BOOL)( --e->users == 0 );
	if ( last && e->listed ) {
		for ( pp = &img_list; *pp != NULL; pp = &(*pp)->next ) {
			if ( *pp == e ) { *pp = e->next; break; }
		}
		e->listed = FALSE;
	}
	ISpinUnlock(&img_lock, &imask);
	if ( last ) img_free(e);
}

/* The images of an object (uuid NULL: every image of the volume) not to be found again */
EXPORT void knl_elf_forget( ID vol, CONST TS_UUID *uuid )
{
	PROGIMG	**pp;
	UINT	imask;

	if ( img_list == NULL ) return;
	ISpinLock(&img_lock, &imask);
	for ( pp = &img_list; *pp != NULL; ) {
		PROGIMG	*e = *pp;

		if ( e->vol == vol && ( uuid == NULL || same_bytes(&e->uuid, uuid, sizeof(TS_UUID)) ) ) {
			*pp = e->next;
			e->listed = FALSE;	/* held on by the processes that map it */
		} else {
			pp = &e->next;
		}
	}
	ISpinUnlock(&img_lock, &imask);
}

/* The image of this record with these headers, taken for a load; NULL when none is held */
LOCAL PROGIMG *img_find( ID vol, CONST TS_UUID *uuid, INT recno, CONST UB *head, INT headsz )
{
	PROGIMG	*e;
	UINT	imask;

	ISpinLock(&img_lock, &imask);
	for ( e = img_list; e != NULL; e = e->next ) {
		if ( e->vol == vol && e->recno == recno && e->headsz == headsz
		  && same_bytes(&e->uuid, uuid, sizeof(TS_UUID))
		  && same_bytes(e->head, head, headsz) ) {
			e->users++;
			break;
		}
	}
	ISpinUnlock(&img_lock, &imask);
	return e;
}

/* A new image made findable, unless one of the same record came first */
LOCAL void img_add( PROGIMG *e )
{
	PROGIMG	*o;
	UINT	imask;

	ISpinLock(&img_lock, &imask);
	for ( o = img_list; o != NULL; o = o->next ) {
		if ( o->vol == e->vol && o->recno == e->recno && same_bytes(&o->uuid, &e->uuid, sizeof(TS_UUID)) ) {
			break;
		}
	}
	if ( o == NULL ) {
		e->next = img_list;
		img_list = e;
		e->listed = TRUE;
	}
	ISpinUnlock(&img_lock, &imask);
}

/*
 * The space given the image: the read only pages mapped as they are,
 * the writable ones copied. Runs of pages one after another in memory
 * go to the tables in one call.
 */
LOCAL ER img_map( PROGIMG *e, T_SPACE *sp )
{
	INT	i;
	UD	k, n;

	for ( i = 0; i < e->nseg; i++ ) {
		IMGSEG	*s = &e->seg[i];

		if ( s->shared ) {
			for ( k = 0; k < s->npages; k += n ) {
				for ( n = 1; k + n < s->npages && s->pa[k + n] == s->pa[k] + n * PAGE_SIZE; n++ ) ;
				if ( knl_pt_map(sp->l0_pa, s->start + k * PAGE_SIZE, s->pa[k], n * PAGE_SIZE,
						s->attr) != E_OK ) {
					return E_NOMEM;
				}
			}
			continue;
		}
		for ( k = 0; k < s->npages; k++ ) {
			PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, ( k < s->nfile ) ? 0 : KAF_ZERO);

			if ( pf == NULL ) return E_NOMEM;
			if ( k < s->nfile ) {
				knl_memcpy(knl_pf_to_va(pf), PA2VA(s->pa[k]), PAGE_SIZE);
			}
			if ( knl_pt_map(sp->l0_pa, s->start + k * PAGE_SIZE, knl_pf_to_pa(pf), PAGE_SIZE,
					s->attr) != E_OK ) {
				knl_free_pages(pf, 0);
				return E_NOMEM;
			}
		}
	}
	knl_icache_sync_all();
	return E_OK;
}

/*
 * A new image for the load about to be made, its tables of pages made
 * now, before anything is mapped: once a read only page is mapped as
 * the image's, only the image gives it back. NULL when the segments
 * share a page (one page cannot be both shared and the space's own)
 * or there is no memory for it; the program is then loaded as before.
 */
LOCAL PROGIMG *img_new( CONST Elf64_Ehdr *eh, CONST Elf64_Phdr *ph, INT nph )
{
	PROGIMG	*e;
	INT	i, j;

	for ( i = 0; i < nph; i++ ) {
		if ( ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0 ) continue;
		for ( j = 0; j < i; j++ ) {
			UBINT	s1, e1, s2, e2;

			if ( ph[j].p_type != PT_LOAD || ph[j].p_memsz == 0 ) continue;
			s1 = (UBINT)ph[i].p_vaddr & PAGE_MASK;
			e1 = ( (UBINT)ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1 ) & PAGE_MASK;
			s2 = (UBINT)ph[j].p_vaddr & PAGE_MASK;
			e2 = ( (UBINT)ph[j].p_vaddr + ph[j].p_memsz + PAGE_SIZE - 1 ) & PAGE_MASK;
			if ( s1 < e2 && s2 < e1 ) return NULL;
		}
	}
	e = (PROGIMG *)Kmalloc(sizeof(PROGIMG));
	if ( e == NULL ) return NULL;
	knl_memset(e, 0, sizeof(PROGIMG));
	e->users = 1;
	e->headsz = (INT)( sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) * nph );
	knl_memcpy(e->head, eh, sizeof(Elf64_Ehdr));
	knl_memcpy(e->head + sizeof(Elf64_Ehdr), ph, sizeof(Elf64_Phdr) * nph);
	for ( i = 0; i < nph; i++ ) {
		IMGSEG	*s;
		UBINT	start, end;

		if ( ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0 ) continue;
		s = &e->seg[e->nseg++];
		start = (UBINT)ph[i].p_vaddr & PAGE_MASK;
		end   = ( (UBINT)ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1 ) & PAGE_MASK;
		s->start = start;
		s->npages = ( end - start ) >> PAGE_SHIFT;
		s->shared = (BOOL)( ( ph[i].p_flags & PF_W ) == 0 );
		s->pa = (UD *)Kmalloc((SZ)( s->npages * sizeof(UD) ));
		if ( s->pa == NULL ) {
			img_free(e);
			return NULL;
		}
		knl_memset(s->pa, 0, (SZ)( s->npages * sizeof(UD) ));
	}
	return e;
}

/* The image's segment for a program header */
LOCAL IMGSEG *img_seg( PROGIMG *e, UBINT start )
{
	INT	i;

	for ( i = 0; e != NULL && i < e->nseg; i++ ) {
		if ( e->seg[i].start == start ) return &e->seg[i];
	}
	return NULL;
}

/* ---------------------------------------------------------------- loading */

/*
 * The image is read in pieces of up to ELF_PIECE bytes into a buffer of
 * the kernel's and copied from there into the pages of the space, which
 * are not one after another in memory. A read for each page instead is
 * a call into the store for every 4KB -- tens of thousands for a large
 * program, each finding the object and its record again -- and that,
 * not the bytes, is what the reading of a large program costs.
 */
#define ELF_PIECE	( 1280 * 1024 )	/* a whole request of the SD card (TDN_IOMAX, design 7.3) */
#define ELF_PIECE_MIN	PAGE_SIZE

/*
 * Copy 'len' bytes of the image at 'off' into the space at 'va'. The
 * pages are already mapped; they are written through the kernel's
 * linear map. 'buf' holds 'bufsz' bytes. Instructions ('code') are
 * cleaned out to where the instruction side fetches from as they are
 * written; the instruction caches are invalidated once, after the
 * whole program (knl_icache_sync_all).
 */
LOCAL ER load_bytes( ELFSRC *src, T_SPACE *sp, UBINT va, UD off, UD len, UB *buf, UD bufsz,
		     BOOL code )
{
	UB	*page;
	UD	got, k, at;

	while ( len > 0 ) {
		got = ( len < bufsz ) ? len : bufsz;
		if ( src->read(src->ctx, off, buf, got) != (INT)got ) {
			return E_IO;
		}
		for ( at = 0; at < got; at += k ) {
			k = PAGE_SIZE - ( ( va + at ) & ~PAGE_MASK );
			if ( k > got - at ) k = got - at;

			page = (UB *)knl_space_page(sp, va + at);
			if ( page == NULL ) {
				return E_SYS;
			}
			knl_memcpy(page, buf + at, (INT)k);
			if ( code ) {
				knl_dcache_clean_pou((UBINT)page, k);
			}
		}
		va  += got;
		off += got;
		len -= got;
	}

	return E_OK;
}

/* The ELF header and the program headers read and checked: their number, or an error */
LOCAL INT read_heads( ELFSRC *src, Elf64_Ehdr *eh, Elf64_Phdr *ph )
{
	INT	i, nph, n;

	n = src->read(src->ctx, 0, eh, sizeof(Elf64_Ehdr));
	if ( n < 0 ) return E_IO;
	if ( n != (INT)sizeof(Elf64_Ehdr) ) {
		return E_OBJ;		/* too short to be a program */
	}
	if ( eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E'
	  || eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F'
	  || eh->e_ident[4] != 2			/* ELFCLASS64 */
	  || eh->e_ident[5] != 1 ) {			/* ELFDATA2LSB */
		return E_OBJ;
	}
	if ( eh->e_machine != KNL_ELF_MACHINE
	  || (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ) {
		return E_OBJ;
	}
	if ( eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 ) {
		return E_OBJ;
	}
	nph = ( eh->e_phnum > PHDR_MAX ) ? PHDR_MAX : eh->e_phnum;

	if ( src->read(src->ctx, eh->e_phoff, ph, sizeof(Elf64_Phdr) * nph)
	     != (INT)(sizeof(Elf64_Phdr) * nph) ) {
		return E_OBJ;			/* the headers are not there */
	}

	/* A program that needs an interpreter cannot be started */
	for ( i = 0; i < nph; i++ ) {
		if ( ph[i].p_type == PT_INTERP ) return E_NOSPT;
		if ( ph[i].p_type == PT_GNU_STACK && (ph[i].p_flags & PF_X) != 0 ) {
			return E_NOSPT;		/* an executable stack is refused */
		}
	}
	return nph;
}

/*
 * Load a program, its headers already read, into a space.
 *	p_entry receives the entry point and p_brk the first address past
 *	the last segment, where the heap starts. 'img', when not NULL, is
 *	the image the load fills: the read only pages are mapped as its,
 *	and the writable ones copied into it once they are loaded.
 */
LOCAL ER elf_load( ELFSRC *src, CONST Elf64_Ehdr *eh, CONST Elf64_Phdr *ph, INT nph,
		   T_SPACE *sp, UD *p_entry, UD *p_brk, PROGIMG *img )
{
	UB		*buf = NULL;
	UD		bufsz;
	INT		i;
	UD		brk = 0;
	ER		er = E_OK;

	/* the buffer the image comes through: smaller when memory is short */
	for ( bufsz = ELF_PIECE; bufsz >= ELF_PIECE_MIN; bufsz /= 4 ) {
		buf = (UB *)Kmalloc((SZ)bufsz);
		if ( buf != NULL ) break;
	}
	if ( buf == NULL ) return E_NOMEM;

	/*
	 * Map every loadable segment with its final permissions and copy the
	 * file into it. The copy goes through the kernel's linear map, never
	 * through the program's mapping, so the program's pages need not be
	 * writable while they are filled, and the space never holds a page
	 * that is both writable and executable. Mapping them once, in a
	 * space no processor has used yet, invalidates no TLB entry; changing
	 * the permissions afterwards would invalidate each page in every
	 * processor's TLB, which for a program of a hundred megabytes is
	 * seconds of stopping all the processors.
	 */
	for ( i = 0; i < nph; i++ ) {
		UBINT	start, end;
		UD	attr, k;
		IMGSEG	*s;

		if ( ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0 ) continue;
		if ( ph[i].p_filesz > ph[i].p_memsz ) { er = E_OBJ; goto exit; }

		start = (UBINT)ph[i].p_vaddr & PAGE_MASK;
		end   = ((UBINT)ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
		if ( start < USER_TEXT_VA || end >= USER_STACK_TOP
		  || ( end > TS_MEM_BASE && start < TS_MEM_END ) ) {
			er = E_PAR;		/* outside the area a program may use */
			goto exit;
		}

		if ( (ph[i].p_flags & PF_X) != 0 ) {
			attr = PTE_PAGE_UTEXT;		/* RO + X at EL0 */
		} else if ( (ph[i].p_flags & PF_W) != 0 ) {
			attr = PTE_PAGE_UDATA;		/* RW + XN */
		} else {
			attr = PTE_PAGE_URO;
		}
		s = img_seg(img, start);
		if ( s != NULL && s->shared ) attr |= PTE_SW_SHARED;
		if ( s != NULL ) s->attr = attr;
		er = knl_space_map_zero(sp, start, end - start, attr);
		if ( s != NULL && s->shared ) {
			/* the image's pages, those mapped even when the mapping stopped short */
			for ( k = 0; k < s->npages; k++ ) {
				UD	pa = knl_pt_lookup(sp->l0_pa, start + k * PAGE_SIZE);

				if ( pa != ~(UD)0 ) s->pa[k] = pa;
			}
			img->bytes += s->npages * PAGE_SIZE;
		}
		if ( er < E_OK ) goto exit;

		if ( ph[i].p_filesz > 0 ) {
			er = load_bytes(src, sp, (UBINT)ph[i].p_vaddr, ph[i].p_offset, ph[i].p_filesz,
					buf, bufsz, (BOOL)( ( ph[i].p_flags & PF_X ) != 0 ));
			if ( er < E_OK ) goto exit;
		}
		if ( end > brk ) brk = end;
	}

	/*
	 * The pages were written through the kernel's linear map, so the
	 * instruction side has to be told before the program runs.
	 */
	knl_icache_sync_all();

	*p_entry = eh->e_entry;
	*p_brk   = brk;

	/* the writable segments as the program finds them, kept in the image for the next load */
	if ( img != NULL ) {
		img->whole = TRUE;
		img->entry = eh->e_entry;
		img->brk = brk;
		for ( i = 0; i < nph && img->whole; i++ ) {
			IMGSEG	*s;
			UD	k;

			if ( ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0 ) continue;
			s = img_seg(img, (UBINT)ph[i].p_vaddr & PAGE_MASK);
			if ( s == NULL || s->shared ) continue;
			s->nfile = ( ( (UBINT)ph[i].p_vaddr + ph[i].p_filesz + PAGE_SIZE - 1 ) & PAGE_MASK ) - s->start;
			s->nfile >>= PAGE_SHIFT;
			for ( k = 0; k < s->nfile; k++ ) {
				PFRAME	*pf = knl_alloc_pages(0, ZONE_NORMAL, 0);
				void	*from = knl_space_page(sp, s->start + k * PAGE_SIZE);

				if ( pf == NULL || from == NULL ) {
					if ( pf != NULL ) knl_free_pages(pf, 0);
					img->whole = FALSE;	/* its read only pages are its own all the same */
					break;
				}
				knl_memcpy(knl_pf_to_va(pf), from, PAGE_SIZE);
				s->pa[k] = knl_pf_to_pa(pf);
			}
		}
	}

    exit:
	Kfree(buf);
	return er;
}

EXPORT ER knl_elf_load( CONST char *path, void *space, UD *p_entry, UD *p_brk )
{
	ELFSRC		src;
	Elf64_Ehdr	*eh;
	Elf64_Phdr	*ph;
	INT		fd, nph;
	ER		er;

	fd = fs_open(path, O_RDONLY);
	if ( fd < 0 ) {
		return E_NOEXS;
	}
	src.read = file_read;
	src.ctx = &fd;
	eh = (Elf64_Ehdr *)Kmalloc(sizeof(Elf64_Ehdr));
	ph = (Elf64_Phdr *)Kmalloc(sizeof(Elf64_Phdr) * PHDR_MAX);
	if ( eh == NULL || ph == NULL ) {
		er = E_NOMEM;
	} else {
		nph = read_heads(&src, eh, ph);
		er = ( nph < 0 ) ? (ER)nph : elf_load(&src, eh, ph, nph, (T_SPACE *)space, p_entry, p_brk, NULL);
	}
	if ( eh != NULL ) Kfree(eh);
	if ( ph != NULL ) Kfree(ph);
	fs_close(fd);

	return er;
}

/*
 * The image in a record of a real object (design 9.6, 18.20). *p_img is
 * the shared image the space maps (knl_elf_img_put when the space is
 * taken down), NULL when it maps none; on an error the caller takes the
 * space down first and then lets the image go.
 */
EXPORT ER knl_elf_load_rec( ID vol, CONST TS_UUID *uuid, INT recno, void *space,
			    UD *p_entry, UD *p_brk, void **p_img )
{
	ELFSRC		src;
	RECSRC		r;
	Elf64_Ehdr	*eh;
	Elf64_Phdr	*ph;
	PROGIMG		*img = NULL;
	INT		nph;
	UD		t0 = 0, t1 = 0;
	ER		er;

	*p_img = NULL;
	(void)ts_get_mono(&t0);
	r.od = ts_opn_obj(vol, uuid, TFO_READ);
	if ( r.od < E_OK ) {
		return (ER)r.od;
	}
	r.recno = recno;
	src.read = rec_read;
	src.ctx = &r;
	eh = (Elf64_Ehdr *)Kmalloc(sizeof(Elf64_Ehdr));
	ph = (Elf64_Phdr *)Kmalloc(sizeof(Elf64_Phdr) * PHDR_MAX);
	if ( eh == NULL || ph == NULL ) {
		er = E_NOMEM;
		goto exit;
	}
	nph = read_heads(&src, eh, ph);
	if ( nph < 0 ) {
		er = (ER)nph;
		goto exit;
	}

	/* the image a process of this program holds: nothing more to read */
	{
		UB	head[HEAD_MAX];

		knl_memcpy(head, eh, sizeof(Elf64_Ehdr));
		knl_memcpy(head + sizeof(Elf64_Ehdr), ph, sizeof(Elf64_Phdr) * nph);
		img = img_find(vol, uuid, recno, head, (INT)( sizeof(Elf64_Ehdr) + sizeof(Elf64_Phdr) * nph ));
	}
	if ( img != NULL ) {
		ts_cls_obj(r.od);
		r.od = 0;
		er = img_map(img, (T_SPACE *)space);
		*p_img = img;
		if ( er >= E_OK ) {
			*p_entry = img->entry;
			*p_brk = img->brk;
			img_hits++;
			(void)ts_get_mono(&t1);
			if ( img->bytes >= 8 * 1024 * 1024 ) {
				tm_printf((UB *)"elf: a program of %d MB shared, its process made in %d ms\n",
					  (INT)( img->bytes >> 20 ), (INT)( ( t1 - t0 ) / 1000000 ));
			}
		}
		goto exit;
	}

	img = img_new(eh, ph, nph);
	if ( img != NULL ) {
		img->vol = vol;
		img->uuid = *uuid;
		img->recno = recno;
	}
	er = elf_load(&src, eh, ph, nph, (T_SPACE *)space, p_entry, p_brk, img);
	*p_img = img;
	img_reads++;
	if ( er >= E_OK && img != NULL ) {
		if ( img->whole ) img_add(img);
		(void)ts_get_mono(&t1);
		if ( img->bytes >= 8 * 1024 * 1024 ) {
			tm_printf((UB *)"elf: a program of %d MB read, its process made in %d ms\n",
				  (INT)( img->bytes >> 20 ), (INT)( ( t1 - t0 ) / 1000000 ));
		}
	}

    exit:
	if ( eh != NULL ) Kfree(eh);
	if ( ph != NULL ) Kfree(ph);
	if ( r.od > 0 ) ts_cls_obj(r.od);

	return er;
}
