/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/mman.h
 *	mmap and its relatives over the memory a process asks for
 *	(include/ts/umem.h; lib/libcxxrt/ts_mman.c).
 *
 *	Only anonymous private mappings exist. PROT_NONE holds addresses
 *	without pages; mprotect to readable or writable makes the pages
 *	(zero filled when they were not there), mprotect to PROT_NONE and
 *	madvise(MADV_DONTNEED) take them away, and what they held with
 *	them. PROT_EXEC is refused (EACCES): nothing a process makes can
 *	be run.
 */

#ifndef __TSPOSIX_SYS_MMAN_H__
#define __TSPOSIX_SYS_MMAN_H__

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE	0x0
#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define PROT_EXEC	0x4

#define MAP_SHARED	0x01
#define MAP_PRIVATE	0x02
#define MAP_FIXED	0x10
#define MAP_ANONYMOUS	0x20
#define MAP_ANON	MAP_ANONYMOUS
#define MAP_NORESERVE	0x4000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED	((void *)-1)

#define MADV_NORMAL	0
#define MADV_RANDOM	1
#define MADV_SEQUENTIAL	2
#define MADV_WILLNEED	3
#define MADV_DONTNEED	4
#define MADV_FREE	8
#define MADV_REMOVE	9
#define MADV_DONTFORK	10
#define MADV_DODUMP	17
#define MADV_DONTDUMP	16

#define POSIX_MADV_DONTNEED	MADV_DONTNEED

#define MS_ASYNC	1
#define MS_SYNC		4
#define MS_INVALIDATE	2

void	*mmap( void *addr, size_t len, int prot, int flags, int fd, off_t off );
int	munmap( void *addr, size_t len );
int	mprotect( void *addr, size_t len, int prot );
int	madvise( void *addr, size_t len, int advice );
int	posix_madvise( void *addr, size_t len, int advice );
int	msync( void *addr, size_t len, int flags );
int	mlock( const void *addr, size_t len );
int	munlock( const void *addr, size_t len );
int	mincore( void *addr, size_t len, unsigned char *vec );	/* every page counted present */

/* Memory as a descriptor: none (ENOSYS) */
#define MFD_CLOEXEC		0x0001U
#define MFD_ALLOW_SEALING	0x0002U
int	memfd_create( const char *name, unsigned int flags );

/* An aligned reservation in one call, which mmap cannot ask for */
void	*mmap_aligned_tf( size_t len, size_t align, int prot );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_SYS_MMAN_H__ */
