/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_evfd.c
 *	eventfd and epoll within a process (design 17.19).
 *
 *	A message loop of Linux sleeps in epoll_wait and is woken by a write
 *	to an eventfd that another thread makes. Both are descriptors of
 *	their own range here (EVFD_BASE up), kept in a table of the process:
 *	an eventfd is a 64-bit counter, an epoll a list of the descriptors it
 *	watches. A waiter sleeps on one condition variable of the process and
 *	is woken by every write to a counter; it then looks again at what it
 *	watches. Only counters can become ready: a socket or a file watched
 *	by an epoll is never reported (the port does not wait on sockets
 *	through epoll). The watch is level triggered: a counter above zero
 *	is readable until it is read.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#define EVFD_BASE	0x3000		/* below the sockets' TS_SOCK_FD_BASE */
#define EVFD_MAX	64
#define WATCH_MAX	16

#define EFD_SEMAPHORE	1
#define EFD_NONBLOCK	04000

#define EPOLLIN		0x001
#define EPOLL_CTL_ADD	1
#define EPOLL_CTL_DEL	2
#define EPOLL_CTL_MOD	3

/* As sys/epoll.h lays it out on aarch64 (not packed) */
struct ts_epoll_event {
	uint32_t	events;
	uint64_t	data;
};

enum { K_FREE, K_EVENT, K_EPOLL };

typedef struct {
	int		kind;
	int		flags;
	uint64_t	count;			/* K_EVENT */
	int		nw;			/* K_EPOLL */
	struct {
		int		fd;
		uint32_t	events;
		uint64_t	data;
	} w[WATCH_MAX];
} EVOBJ;

static EVOBJ		ev[EVFD_MAX];
static pthread_mutex_t	ev_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	ev_cv = PTHREAD_COND_INITIALIZER;

static EVOBJ *get( int fd, int kind )
{
	int	i = fd - EVFD_BASE;

	if ( i < 0 || i >= EVFD_MAX || ev[i].kind != kind ) return NULL;
	return &ev[i];
}

static int make( int kind, int flags )
{
	int	i;

	pthread_mutex_lock(&ev_mu);
	for ( i = 0; i < EVFD_MAX; i++ ) {
		if ( ev[i].kind == K_FREE ) {
			memset(&ev[i], 0, sizeof(ev[i]));
			ev[i].kind = kind;
			ev[i].flags = flags;
			pthread_mutex_unlock(&ev_mu);
			return EVFD_BASE + i;
		}
	}
	pthread_mutex_unlock(&ev_mu);
	errno = EMFILE;
	return -1;
}

/* Whether a descriptor is one of this range (ts_syscalls.c asks) */
int ts_evfd_is( int fd )
{
	return fd >= EVFD_BASE && fd < EVFD_BASE + EVFD_MAX;
}

int eventfd( unsigned int initval, int flags )
{
	int	fd = make(K_EVENT, flags);

	if ( fd >= 0 ) ev[fd - EVFD_BASE].count = initval;
	return fd;
}

int ts_evfd_close( int fd )
{
	EVOBJ	*o;

	pthread_mutex_lock(&ev_mu);
	o = get(fd, K_EVENT);
	if ( o == NULL ) o = get(fd, K_EPOLL);
	if ( o != NULL ) o->kind = K_FREE;
	pthread_mutex_unlock(&ev_mu);
	if ( o == NULL ) {
		errno = EBADF;
		return -1;
	}
	return 0;
}

long ts_evfd_write( int fd, const void *buf, size_t n )
{
	EVOBJ		*o;
	uint64_t	v;

	if ( n < sizeof(v) ) {
		errno = EINVAL;
		return -1;
	}
	memcpy(&v, buf, sizeof(v));
	pthread_mutex_lock(&ev_mu);
	o = get(fd, K_EVENT);
	if ( o == NULL ) {
		pthread_mutex_unlock(&ev_mu);
		errno = EBADF;
		return -1;
	}
	o->count += v;
	pthread_cond_broadcast(&ev_cv);
	pthread_mutex_unlock(&ev_mu);
	return sizeof(v);
}

long ts_evfd_read( int fd, void *buf, size_t n )
{
	EVOBJ		*o;
	uint64_t	v;

	if ( n < sizeof(v) ) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&ev_mu);
	for ( ;; ) {
		o = get(fd, K_EVENT);
		if ( o == NULL ) {
			pthread_mutex_unlock(&ev_mu);
			errno = EBADF;
			return -1;
		}
		if ( o->count > 0 ) break;
		if ( o->flags & EFD_NONBLOCK ) {
			pthread_mutex_unlock(&ev_mu);
			errno = EAGAIN;
			return -1;
		}
		pthread_cond_wait(&ev_cv, &ev_mu);
	}
	if ( o->flags & EFD_SEMAPHORE ) {
		v = 1;
		o->count--;
	} else {
		v = o->count;
		o->count = 0;
	}
	pthread_mutex_unlock(&ev_mu);
	memcpy(buf, &v, sizeof(v));
	return sizeof(v);
}

int eventfd_read( int fd, uint64_t *value )
{
	return ( ts_evfd_read(fd, value, sizeof(*value)) == sizeof(*value) ) ? 0 : -1;
}

int eventfd_write( int fd, uint64_t value )
{
	return ( ts_evfd_write(fd, &value, sizeof(value)) == sizeof(value) ) ? 0 : -1;
}

int epoll_create1( int flags )
{
	return make(K_EPOLL, flags);
}

int epoll_create( int size )
{
	if ( size <= 0 ) {
		errno = EINVAL;
		return -1;
	}
	return make(K_EPOLL, 0);
}

int epoll_ctl( int epfd, int op, int fd, struct ts_epoll_event *event )
{
	EVOBJ	*o;
	int	i, er = 0;

	pthread_mutex_lock(&ev_mu);
	o = get(epfd, K_EPOLL);
	if ( o == NULL ) {
		er = EBADF;
		goto out;
	}
	for ( i = 0; i < o->nw && o->w[i].fd != fd; i++ ) ;
	switch ( op ) {
	case EPOLL_CTL_ADD:
		if ( i < o->nw ) { er = EEXIST; break; }
		if ( o->nw >= WATCH_MAX ) { er = ENOSPC; break; }
		o->w[o->nw].fd = fd;
		o->w[o->nw].events = event->events;
		o->w[o->nw].data = event->data;
		o->nw++;
		break;
	case EPOLL_CTL_MOD:
		if ( i >= o->nw ) { er = ENOENT; break; }
		o->w[i].events = event->events;
		o->w[i].data = event->data;
		break;
	case EPOLL_CTL_DEL:
		if ( i >= o->nw ) { er = ENOENT; break; }
		o->w[i] = o->w[--o->nw];
		break;
	default:
		er = EINVAL;
		break;
	}
	pthread_cond_broadcast(&ev_cv);
    out:
	pthread_mutex_unlock(&ev_mu);
	if ( er != 0 ) {
		errno = er;
		return -1;
	}
	return 0;
}

/* What of an epoll is ready now, into events (the lock held) */
static int ready( EVOBJ *o, struct ts_epoll_event *events, int max )
{
	int	i, n = 0;

	for ( i = 0; i < o->nw && n < max; i++ ) {
		EVOBJ	*e = get(o->w[i].fd, K_EVENT);

		if ( e != NULL && e->count > 0 && ( o->w[i].events & EPOLLIN ) ) {
			events[n].events = EPOLLIN;
			events[n].data = o->w[i].data;
			n++;
		}
	}
	return n;
}

int epoll_wait( int epfd, struct ts_epoll_event *events, int maxevents, int timeout )
{
	struct timespec	until;
	EVOBJ		*o;
	int		n;

	if ( maxevents <= 0 ) {
		errno = EINVAL;
		return -1;
	}
	if ( timeout > 0 ) {
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += timeout / 1000;
		until.tv_nsec += (long)( timeout % 1000 ) * 1000000L;
		if ( until.tv_nsec >= 1000000000L ) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
	}
	pthread_mutex_lock(&ev_mu);
	for ( ;; ) {
		o = get(epfd, K_EPOLL);
		if ( o == NULL ) {
			pthread_mutex_unlock(&ev_mu);
			errno = EBADF;
			return -1;
		}
		n = ready(o, events, maxevents);
		if ( n > 0 || timeout == 0 ) break;
		if ( timeout < 0 ) {
			pthread_cond_wait(&ev_cv, &ev_mu);
		} else if ( pthread_cond_timedwait(&ev_cv, &ev_mu, &until) == ETIMEDOUT ) {
			n = ready(o, events, maxevents);
			break;
		}
	}
	pthread_mutex_unlock(&ev_mu);
	return n;
}

int epoll_pwait( int epfd, struct ts_epoll_event *events, int maxevents, int timeout,
		 const void *sigmask )
{
	(void)sigmask;
	return epoll_wait(epfd, events, maxevents, timeout);
}
