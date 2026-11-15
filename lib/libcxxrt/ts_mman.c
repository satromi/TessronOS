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
 *
 *	memfd_create makes a memory object of the system without a name
 *	(include/ts/ob.h, OB_F_TRANSIENT, design 18.6), and a descriptor of
 *	their own range (MEMFD_BASE up) is a key to it. ftruncate sets the
 *	size of its record, and mmap with MAP_SHARED maps the record at the
 *	one address the kernel gives it in every process it is mapped
 *	into. The key can be handed to another process (ts_memfd_key, and
 *	ts_memfd_from_key there), which then maps the same pages; a
 *	descriptor made read-only (F_SETFL O_RDONLY) is a key that may only
 *	read, and a process it is handed to can map it only so. The object
 *	goes when its last key in any process is closed and its last
 *	mapping let go. The protection asked for an anonymous mapping is
 *	applied as ts_ctl_mem has it.
 */

#include <config.h>
#include <stdint.h>
#include <string.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/umem.h>
#include <ts/ob.h>
#include <ts/proc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#define PAGE	4096UL

/*
 * The GPU's descriptors (GPUFD_BASE up), when the program has OpenGL ES
 * linked in: the DRM shim (lib/libgl/gl_drm.c) maps a BO for an mmap of
 * one, and knows the BOs' mappings, which stay until the BO goes.
 * Without it these are not there and the descriptors are not either.
 */
extern void	*ts_gpu_fd_mmap( int fd, size_t len, off_t off ) __attribute__((weak));
extern int	ts_gpu_fd_munmap( void *addr, size_t len ) __attribute__((weak));

/* ---------------------------------------------------------------- memory objects */

#define MEMFD_BASE	0x3c00		/* between the random sources and the sockets */
/*
 * As many descriptors as the range up to the sockets holds: a page that
 * reads many Blobs and fetches many bodies has a data pipe, and so a
 * memory object, for each until the collector lets it go
 */
#define MEMFD_MAX	1024
/* Mappings: one for each object mapped, however many times it is (mmap counts them) */
#define MFMAP_MAX	1024

/*
 * A descriptor is a key to the memory object (ts/ob.h, OB_F_TRANSIENT),
 * read-only when the key may not write. Record 0 is its memory.
 */
static struct {
	ID	key;			/* 0: free */
	int	rdonly;
} mfd[MEMFD_MAX];

/*
 * An object mapped into this process: where (the kernel maps an object
 * at one address in every process), how many mmaps of it there are,
 * and a key of its own, so that the mapping stays when the descriptors
 * are closed, as it does on Linux.
 */
static struct {
	char	*base;			/* NULL: free */
	size_t	len;
	int	count;
	ID	key;
} mfmap[MFMAP_MAX];

static pthread_mutex_t	mfd_mu = PTHREAD_MUTEX_INITIALIZER;

static ID mfd_key( int fd )
{
	int	i = fd - MEMFD_BASE;

	return ( i < 0 || i >= MEMFD_MAX ) ? 0 : mfd[i].key;
}

static int er_errno( ER er )
{
	switch ( er ) {
	case E_NOMEM:	return ENOMEM;
	case E_OACV:	return EACCES;
	case E_LIMIT:	return ENOMEM;
	case E_ID:	return EBADF;
	default:	return EINVAL;
	}
}

/* A descriptor for a key, which it holds from now on (under mfd_mu) */
static int mfd_new( ID key, int rdonly )
{
	int	i;

	for ( i = 0; i < MEMFD_MAX; i++ ) {
		if ( mfd[i].key == 0 ) {
			mfd[i].key = key;
			mfd[i].rdonly = rdonly;
			return MEMFD_BASE + i;
		}
	}
	errno = EMFILE;
	return -1;
}

int ts_memfd_is( int fd )
{
	return fd >= MEMFD_BASE && fd < MEMFD_BASE + MEMFD_MAX;
}

/*
 * A memory object without a name, which goes when its last key is
 * closed and its last mapping let go, in whatever processes they are
 */
int memfd_create( const char *name, unsigned int flags )
{
	T_OBCRE	c;
	TS_UUID	u;
	ID	key;
	INT	recno;
	int	fd;

	(void)name; (void)flags;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.flags = OB_F_TRANSIENT;
	if ( ob_cre_key(&c, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD, &key, &u) < E_OK || key <= 0 ) {
		errno = ENOMEM;
		return -1;
	}
	if ( ob_apd_rec(key, OB_RT_SYSDATA, 0, &recno) < E_OK ) {
		(void)ob_cls_obj(key);
		errno = ENOMEM;
		return -1;
	}
	pthread_mutex_lock(&mfd_mu);
	fd = mfd_new(key, 0);
	pthread_mutex_unlock(&mfd_mu);
	if ( fd < 0 ) {
		(void)ob_cls_obj(key);		/* the object goes with it */
	}
	return fd;
}

/*
 * The key a descriptor is, for handing it to another process
 * (ob_dup_key, or the channels' references); 0 with EBADF when it is
 * not one of these
 */
ID ts_memfd_key( int fd )
{
	ID	key;

	pthread_mutex_lock(&mfd_mu);
	key = mfd_key(fd);
	pthread_mutex_unlock(&mfd_mu);
	if ( key == 0 ) errno = EBADF;
	return key;
}

/*
 * A key to a memory object that was handed to this process made a
 * descriptor, which holds it from now on (closing the descriptor closes
 * the key). Read-only when the key may not write: a write of nothing
 * asks the key without changing the record.
 */
int ts_memfd_from_key( ID key )
{
	SZ	asz = 0;
	int	fd, rdonly;

	if ( key <= 0 ) {
		errno = EBADF;
		return -1;
	}
	rdonly = ( ob_wri_rec(key, 0, 0, "", 0, &asz) == E_OACV );
	pthread_mutex_lock(&mfd_mu);
	fd = mfd_new(key, rdonly);
	pthread_mutex_unlock(&mfd_mu);
	return fd;
}

/* The bytes of the objects this process has mapped, and how many (the browser's report of its memory) */
size_t ts_memfd_bytes( int *p_n )
{
	size_t	sum = 0;
	int	i, n = 0;

	pthread_mutex_lock(&mfd_mu);
	for ( i = 0; i < MFMAP_MAX; i++ ) {
		if ( mfmap[i].base != NULL ) {
			n++;
			sum += mfmap[i].len;
		}
	}
	pthread_mutex_unlock(&mfd_mu);
	if ( p_n != NULL ) *p_n = n;
	return sum;
}

int ts_memfd_close( int fd )
{
	ID	key;

	pthread_mutex_lock(&mfd_mu);
	key = mfd_key(fd);
	if ( key != 0 ) mfd[fd - MEMFD_BASE].key = 0;
	pthread_mutex_unlock(&mfd_mu);
	if ( key == 0 ) {
		errno = EBADF;
		return -1;
	}
	(void)ob_cls_obj(key);
	return 0;
}

int ts_memfd_dup( int fd )
{
	ID	key, nk;
	int	r = -1, ro = 0;

	pthread_mutex_lock(&mfd_mu);
	key = mfd_key(fd);
	if ( key != 0 ) ro = mfd[fd - MEMFD_BASE].rdonly;
	pthread_mutex_unlock(&mfd_mu);
	if ( key == 0 ) {
		errno = EBADF;
		return -1;
	}
	nk = ob_dup_key(key, OB_OP_ALL, ts_get_pid());
	if ( nk <= 0 ) {
		errno = EMFILE;
		return -1;
	}
	pthread_mutex_lock(&mfd_mu);
	r = mfd_new(nk, ro);
	pthread_mutex_unlock(&mfd_mu);
	if ( r < 0 ) (void)ob_cls_obj(nk);
	return r;
}

/*
 * The size: grown with zero filled pages, or cut. Not grown past the
 * pages it was mapped with while it is mapped anywhere.
 */
int ts_memfd_truncate( int fd, size_t len )
{
	ID	key;
	int	ro = 0;
	ER	er;

	pthread_mutex_lock(&mfd_mu);
	key = mfd_key(fd);
	if ( key != 0 ) ro = mfd[fd - MEMFD_BASE].rdonly;
	pthread_mutex_unlock(&mfd_mu);
	if ( key == 0 || ro ) {
		errno = ( key == 0 ) ? EBADF : EINVAL;
		return -1;
	}
	er = ob_trn_rec(key, 0, (UD)len);
	if ( er < E_OK ) {
		errno = ( er == E_LIMIT ) ? EINVAL : er_errno(er);
		return -1;
	}
	return 0;
}

int ts_memfd_fstat( int fd, struct stat *st )
{
	T_OBREC	r;
	ID	key = ts_memfd_key(fd);
	INT	n = 0;

	memset(st, 0, sizeof(*st));
	if ( key == 0 || ob_lst_rec(key, &r, 1, &n) < E_OK || n < 1 ) {
		errno = EBADF;
		return -1;
	}
	st->st_mode = S_IFREG | 0600;
	st->st_size = (off_t)r.size;
	st->st_ino = (ino_t)key;
	st->st_nlink = 1;
	st->st_blksize = PAGE;
	return 0;
}

/*
 * F_GETFL says whether the descriptor is read-only. F_SETFL may make it
 * so: its key is replaced by one that may only read, so that whoever it
 * is handed to cannot write either. Seals are not kept: the commands
 * of them, as any other, are taken and do nothing (Mesa seals what it
 * makes and does not depend on the seals holding).
 */
int ts_memfd_fcntl( int fd, int cmd, int arg )
{
	ID	key, nk = 0;
	int	r = 0;

	key = ts_memfd_key(fd);
	if ( key == 0 ) {
		return -1;
	}
	if ( cmd == F_SETFL && ( arg & O_ACCMODE ) == O_RDONLY ) {
		nk = ob_dup_key(key, OB_OP_R, ts_get_pid());
		if ( nk <= 0 ) {
			errno = EMFILE;
			return -1;
		}
	}
	pthread_mutex_lock(&mfd_mu);
	if ( mfd_key(fd) != key ) {
		errno = EBADF;			/* closed meanwhile */
		r = -1;
	} else if ( cmd == F_GETFL ) {
		r = mfd[fd - MEMFD_BASE].rdonly ? O_RDONLY : O_RDWR;
	} else if ( nk > 0 ) {
		mfd[fd - MEMFD_BASE].key = nk;
		mfd[fd - MEMFD_BASE].rdonly = 1;
		nk = key;			/* the old key is closed below */
	}
	pthread_mutex_unlock(&mfd_mu);
	if ( nk > 0 ) (void)ob_cls_obj(nk);
	return r;
}

/*
 * A mapping of an object, from the offset. The kernel maps the whole
 * record at its one address, with pages for at least off + len bytes
 * the first time; a second mmap of an object mapped here already is
 * the same pages and only counted. PROT_WRITE asks the key to write
 * (EACCES on a read-only one).
 */
static void *mfd_map( int fd, size_t len, int prot, off_t off )
{
	T_OBMAP	m;
	ID	key, mk;
	ER	er;
	int	i, fr = -1;
	void	*p = MAP_FAILED;

	key = ts_memfd_key(fd);
	if ( key == 0 ) {
		return MAP_FAILED;
	}
	if ( off < 0 || ( (size_t)off & ( PAGE - 1 ) ) != 0 || len > SIZE_MAX - (size_t)off ) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	mk = ob_dup_key(key, OB_OP_ALL, ts_get_pid());
	if ( mk <= 0 ) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	memset(&m, 0, sizeof(m));
	m.flags = ( prot & PROT_WRITE ) ? OB_M_WRITE : 0;
	m.size = (SZ)( (size_t)off + len );
	/*
	 * The kernel's mapping and the count of it here change together: a
	 * last munmap of the object in another thread takes the kernel's
	 * mapping away after it lets the count go, and a mapping made
	 * between the two would be counted with nothing behind it.
	 */
	pthread_mutex_lock(&mfd_mu);
	er = ob_map_rec(mk, 0, 0, &m);
	if ( er < E_OK ) {
		pthread_mutex_unlock(&mfd_mu);
		(void)ob_cls_obj(mk);
		errno = er_errno(er);
		return MAP_FAILED;
	}
	for ( i = 0; i < MFMAP_MAX; i++ ) {
		if ( mfmap[i].base == (char *)m.addr ) break;
		if ( fr < 0 && mfmap[i].base == NULL ) fr = i;
	}
	if ( (size_t)off + len > (size_t)m.size ) {
		/* mapped already with fewer pages, which it keeps to */
		errno = ENOMEM;
		if ( i < MFMAP_MAX ) i = -1;	/* that mapping is the other mmap's */
		else fr = -1;
	}
	if ( i < 0 ) {
		/* nothing of this one to keep */
	} else if ( i < MFMAP_MAX ) {
		mfmap[i].count++;		/* mapped here already: the same pages */
		p = (char *)m.addr + off;
	} else if ( fr >= 0 ) {
		mfmap[fr].base = (char *)m.addr;
		mfmap[fr].len = (size_t)m.size;
		mfmap[fr].count = 1;
		mfmap[fr].key = mk;
		mk = 0;
		p = (char *)m.addr + off;
	} else {
		errno = ENOMEM;
	}
	if ( p == MAP_FAILED && i >= 0 ) {
		(void)ob_unm_rec(mk, 0, 0);	/* mapped by this call alone: taken off again */
	}
	pthread_mutex_unlock(&mfd_mu);
	if ( mk > 0 ) (void)ob_cls_obj(mk);
	return p;
}

/* Whether addr is in an object's mapping: the mmap let go (1), or not one (0) */
static int mfd_unmap( void *addr )
{
	ID	key = 0;
	int	i, r = 0;

	pthread_mutex_lock(&mfd_mu);
	for ( i = 0; i < MFMAP_MAX; i++ ) {
		if ( mfmap[i].base != NULL && (char *)addr >= mfmap[i].base
		  && (char *)addr < mfmap[i].base + mfmap[i].len ) {
			if ( --mfmap[i].count == 0 ) {
				key = mfmap[i].key;
				mfmap[i].base = NULL;
				mfmap[i].key = 0;
			}
			r = 1;
			break;
		}
	}
	if ( key > 0 ) {
		(void)ob_unm_rec(key, 0, 0);	/* before another mmap here can count the object again */
	}
	pthread_mutex_unlock(&mfd_mu);
	if ( key > 0 ) {
		(void)ob_cls_obj(key);
	}
	return r;
}

/* Executable and writable: written and run by turns (TS_MEM_JIT) */
static UINT to_prot( int prot )
{
	if ( ( prot & ( PROT_WRITE | PROT_EXEC ) ) == ( PROT_WRITE | PROT_EXEC ) ) return TS_MEM_JIT;
	if ( prot & PROT_EXEC ) return TS_MEM_READ | TS_MEM_EXEC;
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

	if ( len > 0 && ts_memfd_is(fd) && ( flags & MAP_SHARED ) != 0 && ( flags & MAP_FIXED ) == 0 ) {
		return mfd_map(fd, len, prot, off);
	}
	if ( fd >= 0 && ts_gpu_fd_mmap != NULL && ( flags & MAP_FIXED ) == 0 ) {
		p = ts_gpu_fd_mmap(fd, len, off);
		if ( p != MAP_FAILED || errno != EBADF ) {
			return p;
		}
		p = addr;
	}
	if ( len == 0 || ( flags & MAP_ANONYMOUS ) == 0 || fd != -1 ) {
		errno = ( len == 0 ) ? EINVAL : ENODEV;
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
	ER	er;

	if ( mfd_unmap(addr) ) {
		return 0;			/* a mapping of a memory object */
	}
	if ( ts_gpu_fd_munmap != NULL && ts_gpu_fd_munmap(addr, len) == 0 ) {
		return 0;			/* a BO's, which stays mapped while the BO is */
	}
	er = ts_unm_mem(addr, (SZ)( ( len + PAGE - 1 ) & ~( PAGE - 1 ) ));
	if ( er < E_OK ) {
		errno = to_errno(er);
		return -1;
	}
	return 0;
}

int mprotect( void *addr, size_t len, int prot )
{
	ER	er;

	er = ts_ctl_mem(addr, (SZ)( ( len + PAGE - 1 ) & ~( PAGE - 1 ) ), to_prot(prot));
	if ( er < E_OK ) {
		errno = to_errno(er);
		return -1;
	}
	return 0;
}

/*
 * The pages given up: they read as zeros afterwards and keep their
 * protection. Writable ones go back to the system and are made again
 * when touched, as on Linux.
 */
int madvise( void *addr, size_t len, int advice )
{
	SZ	n = (SZ)( ( len + PAGE - 1 ) & ~( PAGE - 1 ) );

	if ( advice == MADV_DONTNEED || advice == MADV_FREE || advice == MADV_REMOVE ) {
		if ( ts_ctl_mem(addr, n, TS_MEM_DISCARD) < E_OK ) {
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

int msync( void *addr, size_t len, int flags )
{
	(void)addr; (void)len; (void)flags;
	return 0;
}

/*
 * Pages are not paged out; every page asked about is counted present,
 * though a writable page not yet touched has none behind it (it is
 * made, zero filled, by the first access)
 */
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
