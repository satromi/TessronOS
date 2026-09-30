/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_urandom.c
 *	getrandom, getentropy, and /dev/urandom and /dev/random, all reading
 *	the random source object 乱数 (lib/libts/ts_random.c keeps the key).
 *
 *	The two paths open to a descriptor of a range of their own
 *	(RND_BASE up); reading it gives random bytes, writing it stirs what
 *	is written into the generator, which the object allows only to the
 *	system and the administrators (EPERM otherwise). The source never
 *	has to wait, so neither does any of these, whatever the flags say.
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

#define RND_BASE	0x3800		/* between the eventfds and the sockets */
#define RND_MAX		16
#define RND_CHUNK	65536		/* bytes asked of the object at a time */

extern ID	ts_random_key( void );

static volatile int	rnd_used[RND_MAX];

static int rnd_errno( ER er )
{
	return ( er == E_OACV ) ? EPERM : ( er == E_MACV ) ? EFAULT : EIO;
}

ssize_t getrandom( void *buf, size_t len, unsigned int flags )
{
	size_t	done = 0;
	ER	er;

	(void)flags;
	while ( done < len ) {
		SZ	n = ( len - done > RND_CHUNK ) ? RND_CHUNK : (SZ)( len - done );

		er = ts_get_random((UB *)buf + done, n);
		if ( er < E_OK ) {
			if ( done > 0 ) break;
			errno = rnd_errno(er);
			return -1;
		}
		done += (size_t)n;
	}
	return (ssize_t)done;
}

int getentropy( void *buf, size_t len )
{
	if ( len > 256 ) {
		errno = EIO;			/* as POSIX has it: at most 256 at a time */
		return -1;
	}
	return ( getrandom(buf, len, 0) == (ssize_t)len ) ? 0 : -1;
}

/* The descriptor of /dev/urandom or /dev/random, -2 for any other path */
int ts_rnd_open( const char *path )
{
	int	i;

	if ( strcmp(path, "/dev/urandom") != 0 && strcmp(path, "/dev/random") != 0 ) {
		return -2;
	}
	if ( ts_random_key() <= 0 ) {
		errno = ENOENT;
		return -1;
	}
	for ( i = 0; i < RND_MAX; i++ ) {
		if ( __atomic_exchange_n(&rnd_used[i], 1, __ATOMIC_ACQ_REL) == 0 ) {
			return RND_BASE + i;
		}
	}
	errno = EMFILE;
	return -1;
}

int ts_rnd_is( int fd )
{
	return fd >= RND_BASE && fd < RND_BASE + RND_MAX && rnd_used[fd - RND_BASE];
}

long ts_rnd_read( int fd, void *buf, size_t n )
{
	(void)fd;
	return (long)getrandom(buf, n, 0);
}

long ts_rnd_write( int fd, const void *buf, size_t n )
{
	SZ	asz = 0;
	ER	er;
	CONST TS_UUID	u = OB_UUID_RANDOM_INIT;
	ID	k;

	(void)fd;
	k = ob_opn_obj(&u, OB_OP_WRITE);
	if ( k <= 0 ) {
		errno = rnd_errno((ER)k);
		return -1;
	}
	er = ob_wri_rec(k, OB_RND_DATA, 0, buf, (SZ)n, &asz);
	(void)ob_cls_obj(k);
	if ( er < E_OK ) {
		errno = rnd_errno(er);
		return -1;
	}
	return (long)asz;
}

int ts_rnd_close( int fd )
{
	rnd_used[fd - RND_BASE] = 0;
	return 0;
}
