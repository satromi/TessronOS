/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_mman.c
 *	mmap, munmap, mprotect and madvise on ts_map_mem, ts_unm_mem and
 *	ts_ctl_mem (include/tsposix/sys/mman.h, design 9.8.1).
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/umem.h>
#include <sys/mman.h>
#include <errno.h>

#define PAGE	4096UL

static UINT to_prot( int prot )
{
	if ( prot & PROT_WRITE ) return TS_MEM_RW;
	if ( prot & PROT_READ ) return TS_MEM_READ;
	return TS_MEM_NONE;
}

static int to_errno( ER er )
{
	switch ( er ) {
	case E_NOMEM:	return ENOMEM;
	case E_NOSPT:	return EACCES;
	default:	return EINVAL;
	}
}

void *mmap( void *addr, size_t len, int prot, int flags, int fd, off_t off )
{
	void	*p = addr;
	ER	er;

	(void)off;
	if ( len == 0 || ( flags & MAP_ANONYMOUS ) == 0 || fd != -1 ) {
		errno = ( len == 0 ) ? EINVAL : ENODEV;
		return MAP_FAILED;
	}
	if ( prot & PROT_EXEC ) {
		errno = EACCES;
		return MAP_FAILED;
	}
	if ( flags & MAP_FIXED ) {
		/* over what is there: the pages go and come back zero filled */
		er = ts_unm_mem(addr, (SZ)len);
		(void)er;
	}
	er = ts_map_mem(&p, (SZ)len, 0, to_prot(prot));
	if ( er < E_OK ) {
		errno = to_errno(er);
		return MAP_FAILED;
	}
	if ( ( flags & ( MAP_FIXED | MAP_FIXED_NOREPLACE ) ) != 0 && p != addr ) {
		(void)ts_unm_mem(p, (SZ)len);
		errno = ( flags & MAP_FIXED ) ? ENOMEM : EEXIST;
		return MAP_FAILED;
	}
	return p;
}

void *mmap_aligned_tf( size_t len, size_t align, int prot )
{
	void	*p = NULL;
	ER	er = ts_map_mem(&p, (SZ)len, (SZ)align, to_prot(prot));

	if ( er < E_OK ) {
		errno = to_errno(er);
		return MAP_FAILED;
	}
	return p;
}

int munmap( void *addr, size_t len )
{
	ER	er = ts_unm_mem(addr, (SZ)( ( len + PAGE - 1 ) & ~( PAGE - 1 ) ));

	if ( er < E_OK ) {
		errno = to_errno(er);
		return -1;
	}
	return 0;
}

int mprotect( void *addr, size_t len, int prot )
{
	ER	er;

	if ( prot & PROT_EXEC ) {
		errno = EACCES;
		return -1;
	}
	er = ts_ctl_mem(addr, (SZ)( ( len + PAGE - 1 ) & ~( PAGE - 1 ) ), to_prot(prot));
	if ( er < E_OK ) {
		errno = to_errno(er);
		return -1;
	}
	return 0;
}

/* The pages given up: they come back zero filled, readable and writable */
int madvise( void *addr, size_t len, int advice )
{
	SZ	n = (SZ)( ( len + PAGE - 1 ) & ~( PAGE - 1 ) );

	if ( advice == MADV_DONTNEED || advice == MADV_FREE || advice == MADV_REMOVE ) {
		if ( ts_ctl_mem(addr, n, TS_MEM_NONE) < E_OK
		  || ts_ctl_mem(addr, n, TS_MEM_RW) < E_OK ) {
			errno = ENOMEM;
			return -1;
		}
	}
	return 0;
}

int posix_madvise( void *addr, size_t len, int advice )
{
	return ( madvise(addr, len, advice) == 0 ) ? 0 : errno;
}

int memfd_create( const char *name, unsigned int flags )
{
	(void)name; (void)flags;
	errno = ENOSYS;
	return -1;
}

int msync( void *addr, size_t len, int flags )
{
	(void)addr; (void)len; (void)flags;
	return 0;
}

/* Pages are made when their memory is made accessible and are not paged
   out; every page asked about is counted present */
int mincore( void *addr, size_t len, unsigned char *vec )
{
	size_t	i, n;

	if ( ( (uintptr_t)addr & ( PAGE - 1 ) ) != 0 || vec == NULL ) {
		errno = EINVAL;
		return -1;
	}
	n = ( len + PAGE - 1 ) / PAGE;
	for ( i = 0; i < n; i++ ) vec[i] = 1;
	return 0;
}

int mlock( const void *addr, size_t len )
{
	(void)addr; (void)len;
	return 0;
}

int munlock( const void *addr, size_t len )
{
	(void)addr; (void)len;
	return 0;
}
