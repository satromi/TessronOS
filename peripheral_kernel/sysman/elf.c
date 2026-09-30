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
 *	segments into a process space. Each segment is first
 *	mapped writable so that the file contents can be copied in, then
 *	given the permissions the program header asks for, so an executable
 *	segment is never writable at the same time (W^X, design 5.11).
 *	Dynamic linking is not supported; a PT_INTERP segment is refused.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/proc.h>
#include "pfalloc.h"
#include "space.h"
#include <ts/umem.h>

#define EI_NIDENT	16
#define ET_EXEC		2
#define ET_DYN		3
#define EM_AARCH64	183
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

/*
 * The image is read in pieces of up to ELF_PIECE bytes into a buffer of
 * the kernel's and copied from there into the pages of the space, which
 * are not one after another in memory. A read for each page instead is
 * a call into the store for every 4KB -- tens of thousands for a large
 * program, each finding the object and its record again -- and that,
 * not the bytes, is what the reading of a large program costs.
 */
#define ELF_PIECE	( 256 * 1024 )
#define ELF_PIECE_MIN	PAGE_SIZE

/*
 * Copy 'len' bytes of the image at 'off' into the space at 'va'. The
 * pages are already mapped writable. 'buf' holds 'bufsz' bytes.
 */
LOCAL ER load_bytes( ELFSRC *src, T_SPACE *sp, UBINT va, UD off, UD len, UB *buf, UD bufsz )
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
		}
		va  += got;
		off += got;
		len -= got;
	}

	return E_OK;
}

/*
 * Load a program into a space.
 *	p_entry receives the entry point and p_brk the first address past
 *	the last segment, where the heap starts.
 */
LOCAL ER elf_load( ELFSRC *src, T_SPACE *sp, UD *p_entry, UD *p_brk )
{
	Elf64_Ehdr	*eh = NULL;
	Elf64_Phdr	*ph = NULL;
	UB		*buf = NULL;
	UD		bufsz;
	INT		i, nph;
	UD		brk = 0;
	ER		er = E_OK;

	eh = (Elf64_Ehdr *)Kmalloc(sizeof(Elf64_Ehdr));
	ph = (Elf64_Phdr *)Kmalloc(sizeof(Elf64_Phdr) * PHDR_MAX);
	if ( eh == NULL || ph == NULL ) { er = E_NOMEM; goto exit; }

	{
		INT n = src->read(src->ctx, 0, eh, sizeof(Elf64_Ehdr));

		if ( n < 0 ) { er = E_IO; goto exit; }
		if ( n != (INT)sizeof(Elf64_Ehdr) ) {
			er = E_OBJ;		/* too short to be a program */
			goto exit;
		}
	}
	if ( eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E'
	  || eh->e_ident[2] != 'L'  || eh->e_ident[3] != 'F'
	  || eh->e_ident[4] != 2			/* ELFCLASS64 */
	  || eh->e_ident[5] != 1 ) {			/* ELFDATA2LSB */
		er = E_OBJ;
		goto exit;
	}
	if ( eh->e_machine != EM_AARCH64
	  || (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) ) {
		er = E_OBJ;
		goto exit;
	}
	if ( eh->e_phentsize != sizeof(Elf64_Phdr) || eh->e_phnum == 0 ) {
		er = E_OBJ;
		goto exit;
	}
	nph = ( eh->e_phnum > PHDR_MAX ) ? PHDR_MAX : eh->e_phnum;

	if ( src->read(src->ctx, eh->e_phoff, ph, sizeof(Elf64_Phdr) * nph)
	     != (INT)(sizeof(Elf64_Phdr) * nph) ) {
		er = E_OBJ;			/* the headers are not there */
		goto exit;
	}

	/* A program that needs an interpreter cannot be started */
	for ( i = 0; i < nph; i++ ) {
		if ( ph[i].p_type == PT_INTERP ) { er = E_NOSPT; goto exit; }
		if ( ph[i].p_type == PT_GNU_STACK && (ph[i].p_flags & PF_X) != 0 ) {
			er = E_NOSPT;		/* an executable stack is refused */
			goto exit;
		}
	}

	/* the buffer the image comes through: smaller when memory is short */
	for ( bufsz = ELF_PIECE; bufsz >= ELF_PIECE_MIN; bufsz /= 4 ) {
		buf = (UB *)Kmalloc((SZ)bufsz);
		if ( buf != NULL ) break;
	}
	if ( buf == NULL ) { er = E_NOMEM; goto exit; }

	/* Map every loadable segment writable and copy the file into it */
	for ( i = 0; i < nph; i++ ) {
		UBINT	start, end;

		if ( ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0 ) continue;
		if ( ph[i].p_filesz > ph[i].p_memsz ) { er = E_OBJ; goto exit; }

		start = (UBINT)ph[i].p_vaddr & PAGE_MASK;
		end   = ((UBINT)ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1) & PAGE_MASK;
		if ( start < USER_TEXT_VA || end >= USER_STACK_TOP
		  || ( end > TS_MEM_BASE && start < TS_MEM_END ) ) {
			er = E_PAR;		/* outside the area a program may use */
			goto exit;
		}

		er = knl_space_map_zero(sp, start, end - start, PTE_PAGE_UDATA);
		if ( er < E_OK ) goto exit;

		if ( ph[i].p_filesz > 0 ) {
			er = load_bytes(src, sp, (UBINT)ph[i].p_vaddr, ph[i].p_offset, ph[i].p_filesz,
					buf, bufsz);
			if ( er < E_OK ) goto exit;
		}
		if ( end > brk ) brk = end;
	}

	/*
	 * Give each segment its final permissions. Executable pages become
	 * read only first, so the space never holds a page that is both
	 * writable and executable.
	 */
	Asm("dsb ish" ::: "memory");
	for ( i = 0; i < nph; i++ ) {
		UBINT	start, end;
		UD	attr;

		if ( ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0 ) continue;
		start = (UBINT)ph[i].p_vaddr & PAGE_MASK;
		end   = ((UBINT)ph[i].p_vaddr + ph[i].p_memsz + PAGE_SIZE - 1) & PAGE_MASK;

		if ( (ph[i].p_flags & PF_X) != 0 ) {
			attr = PTE_PAGE_UTEXT;		/* RO + X at EL0 */
		} else if ( (ph[i].p_flags & PF_W) != 0 ) {
			continue;			/* already RW + XN */
		} else {
			attr = PTE_PAGE_URO;
		}
		er = knl_space_protect(sp, start, end - start, attr);
		if ( er < E_OK ) goto exit;
	}

	/*
	 * The pages were written through the kernel's linear map, so the
	 * instruction side has to be told before the program runs.
	 */
	Asm("dsb ish" ::: "memory");
	Asm("ic ialluis" ::: "memory");
	Asm("dsb ish; isb" ::: "memory");

	*p_entry = eh->e_entry;
	*p_brk   = brk;

    exit:
	if ( buf != NULL ) Kfree(buf);
	if ( eh != NULL ) Kfree(eh);
	if ( ph != NULL ) Kfree(ph);

	return er;
}

EXPORT ER knl_elf_load( CONST char *path, void *space, UD *p_entry, UD *p_brk )
{
	ELFSRC	src;
	INT	fd;
	ER	er;

	fd = fs_open(path, O_RDONLY);
	if ( fd < 0 ) {
		return E_NOEXS;
	}
	src.read = file_read;
	src.ctx = &fd;
	er = elf_load(&src, (T_SPACE *)space, p_entry, p_brk);
	fs_close(fd);

	return er;
}

/* The image in a record of a real object (design 9.6, 18.20) */
EXPORT ER knl_elf_load_rec( ID vol, CONST TS_UUID *uuid, INT recno, void *space,
			    UD *p_entry, UD *p_brk )
{
	ELFSRC	src;
	RECSRC	r;
	ER	er;

	r.od = ts_opn_obj(vol, uuid, TFO_READ);
	if ( r.od < E_OK ) {
		return (ER)r.od;
	}
	r.recno = recno;
	src.read = rec_read;
	src.ctx = &r;
	er = elf_load(&src, (T_SPACE *)space, p_entry, p_brk);
	ts_cls_obj(r.od);

	return er;
}
