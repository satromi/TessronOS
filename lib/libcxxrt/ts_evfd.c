/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_evfd.c
 *	eventfd, epoll, and the wait poll and epoll share (design 17.19).
 *
 *	A message loop of Linux sleeps in epoll_wait and is woken by a write
 *	to an eventfd that another thread makes, or by a socket or a pipe to
 *	another process that has something. eventfds and epolls are
 *	descriptors of their own range (EVFD_BASE up), kept in a table of the
 *	process: an eventfd is a 64-bit counter, an epoll a list of the
 *	descriptors it watches. A pipe within the process is two descriptors
 *	of the range as well: its read end counts the bytes written to its
 *	write end, and is readable while that is above zero. What is read is
 *	zeros: such a pipe wakes a loop, it carries no data.
 *
 *	What a wait watches is of two sorts. An end of a stream between
 *	processes (ts_chan.c) is watched by the system: each epoll that
 *	watches ends has a poll set of its own (OB_S_POLL, include/ts/ob.h),
 *	which holds the ends' keys as epoll_ctl puts them in, and a wait asks
 *	it once for the ends that are ready -- the set is told by the ends
 *	themselves as they change, so that nothing is looked at again when
 *	nothing changed. poll takes a set from a few the process keeps and
 *	puts its ends in it for the one wait. Everything else is looked at
 *	here before the wait: a counter or a pipe in this table, a network
 *	socket asked of the stack (so_poll, not waiting), a descriptor that
 *	only holds a key, and a file, which is always ready to poll and never
 *	reported by an epoll.
 *
 *	A wait that has nothing in the system sleeps on the condition
 *	variable of the process, which every write to a counter and every
 *	change of a watch wakes. One that has ends sleeps in the system on
 *	its set instead, and is listed while it does (kwaits): a write to a
 *	counter it watches, a change of its epoll and the close of its epoll
 *	wake it through the set (OB_PL_WAKE), and it looks here again. The
 *	stack tells nothing of its sockets, so a wait that has sockets and
 *	something else looks at the sockets every SOCK_SLICE_MS; one that has
 *	only sockets waits in the stack. The watch is level triggered: what is
 *	ready is reported until it is not, unless EPOLLONESHOT asks it once
 *	until epoll_ctl changes it again, or EPOLLET (ends only) once each
 *	time it changes.
 *
 *	Every set is made, woken and closed with the process's lock held, so
 *	that a key is never used after it was closed and its number given to
 *	something else; the wait itself is made without it.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <pthread.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>

#define EVFD_BASE	0x3000		/* below the sockets' TS_SOCK_FD_BASE */
#define EVFD_MAX	64
#define WATCH_FIRST	16		/* watches an epoll has room for at first */
#define SOCK_SLICE_MS	10		/* how often sockets are looked at beside other things */
#define KEV_MAX		OB_PL_MAXEV	/* ends one wait takes from its set at once */
#define POOL_MAX	8		/* sets kept for poll */
#define SOCK_LOCAL	64		/* sockets of one look asked about from the stack; more from memory */

#define EFD_SEMAPHORE	1
#define EFD_NONBLOCK	04000

#define EPOLLIN		0x001
#define EPOLLOUT	0x004
#define EPOLLERR	0x008
#define EPOLLHUP	0x010
#define EPOLLRDHUP	0x2000
#define EPOLLONESHOT	( 1U << 30 )
#define EPOLLET		( 1U << 31 )
#define EPOLL_CTL_ADD	1
#define EPOLL_CTL_DEL	2
#define EPOLL_CTL_MOD	3

#define EP_GIVEN	( EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | EPOLLRDHUP )
#define EP_ASKED	( EPOLLIN | EPOLLOUT | EPOLLRDHUP )
#define EP_PRIVATE	( EPOLLONESHOT | EPOLLET )

/* As sys/epoll.h lays it out on aarch64 (not packed) */
struct ts_epoll_event {
	uint32_t	events;
	uint64_t	data;
};

enum { K_FREE, K_EVENT, K_EPOLL, K_PIPER, K_PIPEW };

typedef struct {
	int		fd;
	uint32_t	events;
	uint64_t	data;
	uint32_t	gen;			/* told by the set with the descriptor (token) */
	ID		key;			/* an end's, watched by the set; 0 a watch of here */
} WATCH;

/* What an epoll watches, kept while a wait uses it after its descriptor is closed */
typedef struct {
	int		refs;			/* the descriptor, and each wait under way */
	int		dead;			/* the descriptor closed */
	ID		ks;			/* its set, made with the first end; 0 none */
	int		nw, cap, nkern;
	WATCH		*w;
} EPS;

typedef struct {
	int		kind;
	int		flags;
	uint64_t	count;			/* K_EVENT, K_PIPER: what was written */
	int		peer;			/* K_PIPEW: its read end */
	EPS		*ep;			/* K_EPOLL */
} EVOBJ;

enum { W_LOCAL, W_END, W_KEY, W_SOCK, W_FILE };

typedef struct {
	int		fd;
	int		kind;
	unsigned	want;		/* POLLIN, POLLOUT, POLLRDHUP (the EPOLL bits are the same) */
	unsigned	got;
	ID		key;		/* W_END */
} WITEM;

/* A wait asleep on a set: an epoll's, or poll's with its items */
typedef struct KWAIT {
	struct KWAIT	*next;
	ID		ks;
	EPS		*ep;
	const WITEM	*it;
	int		n;
} KWAIT;

static EVOBJ		ev[EVFD_MAX];
static pthread_mutex_t	ev_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	ev_cv = PTHREAD_COND_INITIALIZER;
static KWAIT		*kwaits;
static uint32_t		watch_gen;
static ID		pool[POOL_MAX];
static int		npool;

/* ts_chan.c: the ends of streams: 1 an end (its key), 0 a descriptor that only holds a key, -1 neither */
extern int	ts_chan_is( int fd );
extern int	ts_chan_end( int fd, ID *p_key );

/* ts_socket.c: the network sockets */
extern int	ts_sock_so( int fd );
extern int	ts_sock_poll( const int *fds, const short *events, short *revents, int n, int ms );

int	ts_evfd_forget( int fd );

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

static int err_of( ER er )
{
	switch ( er ) {
	case E_ID:	return EBADF;
	case E_OACV:
	case E_NOSPT:	return EPERM;
	case E_NOMEM:	return ENOMEM;
	case E_OBJ:	return EEXIST;
	case E_NOEXS:	return ENOENT;
	case E_DISWAI:	return EINTR;
	default:	return EIO;
	}
}

/* ---------------------------------------------------------------- the sets (the lock held) */

static ID set_make( void )
{
	T_OBCRE	c;
	TS_UUID	u;
	ID	k = 0;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.sub = OB_S_POLL;
	if ( ob_cre_key(&c, OB_OP_R | OB_OP_W, &k, &u) < E_OK || k <= 0 ) return 0;
	return k;
}

static ER set_ctl( ID ks, const T_OBPLCTL *c, int n, int *p_done )
{
	SZ	asz = 0;
	ER	er = ob_wri_rec(ks, OB_PL_REC, 0, c, (SZ)sizeof(*c) * n, &asz);

	if ( p_done != NULL ) *p_done = (int)( asz / (SZ)sizeof(*c) );
	return er;
}

static ER set_one( ID ks, int op, ID key, uint32_t events, uint64_t data )
{
	T_OBPLCTL	c;

	memset(&c, 0, sizeof(c));
	c.op = op;
	c.key = key;
	c.events = events;
	c.data = data;
	return set_ctl(ks, &c, 1, NULL);
}

/* The waits asleep on sets that watch fd woken, and those on the condition variable */
static void wake_for( int fd )
{
	KWAIT	*k;
	ID	last = 0;
	int	i;

	pthread_cond_broadcast(&ev_cv);
	for ( k = kwaits; k != NULL; k = k->next ) {
		int	yes = 0;

		if ( k->ep != NULL ) {
			for ( i = 0; i < k->ep->nw && !yes; i++ ) yes = ( k->ep->w[i].fd == fd && k->ep->w[i].key == 0 );
		} else {
			for ( i = 0; i < k->n && !yes; i++ ) yes = ( k->it[i].fd == fd );
		}
		if ( yes && k->ks != last ) {
			(void)set_one(k->ks, OB_PL_WAKE, 0, 0, 0);
			last = k->ks;
		}
	}
}

/* The waits on epoll p woken to look again */
static void wake_ep( EPS *p )
{
	KWAIT	*k;

	pthread_cond_broadcast(&ev_cv);
	for ( k = kwaits; k != NULL; k = k->next ) {
		if ( k->ep == p ) {
			(void)set_one(k->ks, OB_PL_WAKE, 0, 0, 0);
			break;
		}
	}
}

static void eps_drop( EPS *p )
{
	if ( --p->refs > 0 ) return;
	if ( p->ks > 0 ) (void)ob_cls_obj(p->ks);
	free(p->w);
	free(p);
}

/* ---------------------------------------------------------------- descriptors */

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

int pipe( int fds[2] )
{
	int	r = make(K_PIPER, 0), w;

	if ( r < 0 ) return -1;
	w = make(K_PIPEW, 0);
	if ( w < 0 ) {
		ev[r - EVFD_BASE].kind = K_FREE;
		return -1;
	}
	ev[w - EVFD_BASE].peer = r;
	fds[0] = r;
	fds[1] = w;
	return 0;
}

int ts_evfd_close( int fd )
{
	EVOBJ	*o;
	EPS	*p = NULL;

	(void)ts_evfd_forget(fd);
	pthread_mutex_lock(&ev_mu);
	o = get(fd, K_EVENT);
	if ( o == NULL ) o = get(fd, K_EPOLL);
	if ( o == NULL ) o = get(fd, K_PIPER);
	if ( o == NULL ) o = get(fd, K_PIPEW);
	if ( o != NULL ) {
		if ( o->kind == K_EPOLL ) {
			p = o->ep;
			o->ep = NULL;
		}
		o->kind = K_FREE;
		if ( p != NULL ) {
			p->dead = 1;
			wake_ep(p);		/* a wait on it finds it closed (EBADF) */
			eps_drop(p);
		} else {
			wake_for(fd);
		}
	}
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

	pthread_mutex_lock(&ev_mu);
	o = get(fd, K_PIPEW);
	if ( o != NULL ) {
		EVOBJ *r = get(o->peer, K_PIPER);

		if ( r == NULL ) {
			pthread_mutex_unlock(&ev_mu);
			errno = EPIPE;
			return -1;
		}
		r->count += n;
		wake_for(o->peer);
		pthread_mutex_unlock(&ev_mu);
		return (long)n;
	}
	pthread_mutex_unlock(&ev_mu);
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
	wake_for(fd);
	pthread_mutex_unlock(&ev_mu);
	return sizeof(v);
}

long ts_evfd_read( int fd, void *buf, size_t n )
{
	EVOBJ		*o;
	uint64_t	v;

	pthread_mutex_lock(&ev_mu);
	if ( ( o = get(fd, K_PIPER) ) != NULL ) {
		while ( o->count == 0 ) {
			pthread_cond_wait(&ev_cv, &ev_mu);
			if ( ( o = get(fd, K_PIPER) ) == NULL ) {
				pthread_mutex_unlock(&ev_mu);
				errno = EBADF;
				return -1;
			}
		}
		if ( n > o->count ) n = (size_t)o->count;
		o->count -= n;
		pthread_mutex_unlock(&ev_mu);
		memset(buf, 0, n);
		return (long)n;
	}
	pthread_mutex_unlock(&ev_mu);
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
	EPS	*p = (EPS *)calloc(1, sizeof(EPS));
	int	fd;

	if ( p == NULL ) {
		errno = ENOMEM;
		return -1;
	}
	p->refs = 1;
	fd = make(K_EPOLL, flags);
	if ( fd < 0 ) {
		free(p);
		return -1;
	}
	ev[fd - EVFD_BASE].ep = p;
	return fd;
}

int epoll_create( int size )
{
	if ( size <= 0 ) {
		errno = EINVAL;
		return -1;
	}
	return epoll_create1(0);
}

static uint64_t token_of( const WATCH *w )
{
	return ( (uint64_t)w->gen << 32 ) | (uint32_t)w->fd;
}

int epoll_ctl( int epfd, int op, int fd, struct ts_epoll_event *event )
{
	EVOBJ	*o;
	EPS	*p;
	WATCH	*w;
	ID	key = 0;
	int	i, er = 0, end;
	ER	ker;

	pthread_mutex_lock(&ev_mu);
	o = get(epfd, K_EPOLL);
	if ( o == NULL ) {
		er = EBADF;
		goto out;
	}
	p = o->ep;
	if ( op != EPOLL_CTL_DEL && event == NULL ) {
		er = EFAULT;
		goto out;
	}
	if ( fd == epfd ) {
		er = EINVAL;
		goto out;
	}
	for ( i = 0; i < p->nw && p->w[i].fd != fd; i++ ) ;
	w = ( i < p->nw ) ? &p->w[i] : NULL;
	switch ( op ) {
	case EPOLL_CTL_ADD:
		if ( w != NULL ) { er = EEXIST; break; }
		end = ( ts_chan_is(fd) && ts_chan_end(fd, &key) == 1 );
		if ( !end ) key = 0;
		if ( p->nw == p->cap ) {
			int	cap = ( p->cap == 0 ) ? WATCH_FIRST : p->cap * 2;
			WATCH	*nw = (WATCH *)realloc(p->w, sizeof(WATCH) * (size_t)cap);

			if ( nw == NULL ) { er = ENOMEM; break; }
			p->w = nw;
			p->cap = cap;
		}
		w = &p->w[p->nw];
		w->fd = fd;
		w->events = event->events;
		w->data = event->data;
		w->gen = ++watch_gen;
		w->key = key;
		if ( key > 0 ) {
			if ( p->ks == 0 ) p->ks = set_make();
			if ( p->ks == 0 ) { er = ENOMEM; break; }
			ker = set_one(p->ks, OB_PL_ADD, key, event->events & ( EP_ASKED | EP_PRIVATE ), token_of(w));
			if ( ker < E_OK ) { er = err_of(ker); break; }
			p->nkern++;
		}
		p->nw++;
		break;
	case EPOLL_CTL_MOD:
		if ( w == NULL ) { er = ENOENT; break; }
		if ( w->key > 0 ) {
			ker = set_one(p->ks, OB_PL_MOD, w->key, event->events & ( EP_ASKED | EP_PRIVATE ), token_of(w));
			if ( ker < E_OK ) { er = err_of(ker); break; }
		}
		w->events = event->events;
		w->data = event->data;
		break;
	case EPOLL_CTL_DEL:
		if ( w == NULL ) { er = ENOENT; break; }
		if ( w->key > 0 ) {
			(void)set_one(p->ks, OB_PL_DEL, w->key, 0, 0);
			p->nkern--;
		}
		*w = p->w[--p->nw];
		break;
	default:
		er = EINVAL;
		break;
	}
	if ( er == 0 ) wake_ep(p);		/* a wait on it looks again at what it has here */
    out:
	pthread_mutex_unlock(&ev_mu);
	if ( er != 0 ) {
		errno = er;
		return -1;
	}
	return 0;
}

/*
 * A descriptor about to be closed taken out of every epoll, as Linux does
 * with the last close of a file (ts_chan.c and the close here call it)
 */
int ts_evfd_forget( int fd )
{
	int	i, j;

	pthread_mutex_lock(&ev_mu);
	for ( i = 0; i < EVFD_MAX; i++ ) {
		EPS	*p = ( ev[i].kind == K_EPOLL ) ? ev[i].ep : NULL;

		if ( p == NULL ) continue;
		for ( j = 0; j < p->nw; j++ ) {
			if ( p->w[j].fd != fd ) continue;
			if ( p->w[j].key > 0 ) {
				(void)set_one(p->ks, OB_PL_DEL, p->w[j].key, 0, 0);
				p->nkern--;
			}
			p->w[j] = p->w[--p->nw];
			wake_ep(p);
			break;
		}
	}
	pthread_mutex_unlock(&ev_mu);
	return 0;
}

/* ---------------------------------------------------------------- what is looked at here */

static int kind_of( int fd, ID *p_key )
{
	*p_key = 0;
	if ( ts_evfd_is(fd) ) return W_LOCAL;
	if ( ts_chan_is(fd) ) {
		switch ( ts_chan_end(fd, p_key) ) {
		case 1:		return W_END;
		case 0:		return W_KEY;
		default:	return W_LOCAL;		/* closed: as a closed one of this range */
		}
	}
	if ( ts_sock_so(fd) >= 0 ) return W_SOCK;
	return W_FILE;
}

/* What holds for one item looked at here (the lock held); sockets are asked apart */
static unsigned look( int fd, int kind, unsigned want, int epoll )
{
	EVOBJ	*e;

	switch ( kind ) {
	case W_LOCAL:
		e = get(fd, K_EVENT);
		if ( e == NULL ) e = get(fd, K_PIPER);
		if ( e != NULL && e->count > 0 ) return POLLIN & want;
		if ( e == NULL && !epoll && get(fd, K_EPOLL) == NULL && get(fd, K_PIPEW) == NULL ) return POLLNVAL;
		return 0;
	case W_KEY:
		return ( POLLIN | POLLOUT ) & want;
	case W_FILE:
		return epoll ? 0 : want & ( POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM );
	default:
		return 0;
	}
}

/*
 * A list of n sockets for ts_sock_poll: on the stack for SOCK_LOCAL, in
 * memory of its own past that, as many as there are
 */
typedef struct {
	int	lfd[SOCK_LOCAL], lmap[SOCK_LOCAL];
	short	lev[SOCK_LOCAL], lrev[SOCK_LOCAL];
	int	*fds, *map;
	short	*evs, *rev;
} SOCKLIST;

static int socks_room( SOCKLIST *l, int n )
{
	l->fds = l->lfd;
	l->map = l->lmap;
	l->evs = l->lev;
	l->rev = l->lrev;
	if ( n <= SOCK_LOCAL ) return 0;
	l->fds = (int *)malloc(sizeof(int) * (size_t)n * 2);
	l->evs = (short *)malloc(sizeof(short) * (size_t)n * 2);
	if ( l->fds == NULL || l->evs == NULL ) {
		free(l->fds);
		free(l->evs);
		l->fds = l->lfd;
		l->evs = l->lev;
		return -1;
	}
	l->map = l->fds + n;
	l->rev = l->evs + n;
	return 0;
}

static void socks_free( SOCKLIST *l )
{
	if ( l->fds != l->lfd ) free(l->fds);
	if ( l->evs != l->lev ) free(l->evs);
}

/* The sockets among n items asked of the stack, not waiting; their got set */
static void look_socks( WITEM *it, int n )
{
	SOCKLIST	l;
	int		ns = 0, i;

	for ( i = 0; i < n; i++ ) {
		if ( it[i].kind == W_SOCK ) ns++;
	}
	if ( ns == 0 ) return;
	if ( socks_room(&l, ns) < 0 ) {
		for ( i = 0; i < n; i++ ) if ( it[i].kind == W_SOCK ) it[i].got = POLLERR;
		return;
	}
	for ( i = 0, ns = 0; i < n; i++ ) {
		if ( it[i].kind != W_SOCK ) continue;
		l.fds[ns] = it[i].fd;
		l.evs[ns] = (short)( it[i].want & ( POLLIN | POLLOUT ) );
		l.rev[ns] = 0;
		l.map[ns++] = i;
	}
	if ( ts_sock_poll(l.fds, l.evs, l.rev, ns, 0) > 0 ) {
		for ( i = 0; i < ns; i++ ) it[l.map[i]].got = (unsigned short)l.rev[i];
	}
	socks_free(&l);
}

static int64_t now_ms( void )
{
	struct timespec	t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void cv_wait_ms( int ms )
{
	struct timespec	until;

	if ( ms < 0 ) {
		pthread_cond_wait(&ev_cv, &ev_mu);
		return;
	}
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += ms / 1000;
	until.tv_nsec += (long)( ms % 1000 ) * 1000000L;
	if ( until.tv_nsec >= 1000000000L ) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	(void)pthread_cond_timedwait(&ev_cv, &ev_mu, &until);
}

/* The ends ready in set ks, at most n, waited for up to tmo (the lock not held); how many, or an error */
static int set_wait( ID ks, T_OBPLEV *kev, int n, TMO tmo )
{
	SZ	asz = 0;
	ER	er;

	if ( n > KEV_MAX ) n = KEV_MAX;
	er = ob_rea_tmo(ks, OB_PL_REC, 0, kev, (SZ)sizeof(*kev) * n, &asz, tmo);
	if ( er == E_TMOUT ) return 0;
	if ( er < E_OK ) return (int)er;
	return (int)( asz / (SZ)sizeof(*kev) );
}

static void kw_on( KWAIT *k )
{
	k->next = kwaits;
	kwaits = k;
}

static void kw_off( KWAIT *k )
{
	KWAIT	**pp;

	for ( pp = &kwaits; *pp != NULL && *pp != k; pp = &(*pp)->next ) ;
	if ( *pp != NULL ) *pp = k->next;
}

/* How long a wait may sleep now: -1 without end, 0 not at all */
static int sleep_for( int timeout, int64_t until, int sock )
{
	int	rem = ( timeout < 0 ) ? -1 : ( timeout == 0 ) ? 0 : (int)( until - now_ms() );

	if ( timeout > 0 && rem < 0 ) rem = 0;
	if ( sock && ( rem < 0 || rem > SOCK_SLICE_MS ) ) rem = SOCK_SLICE_MS;
	return rem;
}

static int ran_out( int timeout, int64_t until )
{
	return timeout == 0 || ( timeout > 0 && now_ms() >= until );
}

/* ---------------------------------------------------------------- epoll_wait */

/* The watches of p looked at here put in events, at most max (the lock held); how many */
static int ep_look( EPS *p, struct ts_epoll_event *events, int max, int *p_sock )
{
	WITEM	lsk[SOCK_LOCAL], *sk = lsk;
	int	lwidx[SOCK_LOCAL], *widx = lwidx, ns = 0, i, k = 0;

	*p_sock = 0;
	if ( p->nw > SOCK_LOCAL ) {
		/* room for every watch to be a socket's */
		sk = (WITEM *)malloc(sizeof(WITEM) * (size_t)p->nw);
		widx = (int *)malloc(sizeof(int) * (size_t)p->nw);
		if ( sk == NULL || widx == NULL ) {
			free(sk);
			free(widx);
			sk = lsk;
			widx = lwidx;
		}
	}
	for ( i = 0; i < p->nw && k < max; i++ ) {
		WATCH		*w = &p->w[i];
		ID		key;
		int		kind;
		unsigned	want = w->events & EP_ASKED, got;

		if ( w->key > 0 || want == 0 ) continue;
		kind = kind_of(w->fd, &key);
		if ( kind == W_SOCK ) {
			if ( sk != lsk || ns < SOCK_LOCAL ) {
				sk[ns].fd = w->fd;
				sk[ns].kind = W_SOCK;
				sk[ns].want = want;
				sk[ns].got = 0;
				widx[ns++] = i;
			}
			continue;
		}
		got = look(w->fd, kind, want, 1) & EP_GIVEN;
		if ( got == 0 ) continue;
		events[k].events = got;
		events[k].data = w->data;
		k++;
		if ( w->events & EPOLLONESHOT ) w->events &= EP_PRIVATE;
	}
	if ( ns > 0 ) {
		*p_sock = 1;
		look_socks(sk, ns);
		for ( i = 0; i < ns && k < max; i++ ) {
			WATCH	*w = &p->w[widx[i]];
			unsigned got = sk[i].got & ( w->events | EPOLLERR | EPOLLHUP ) & EP_GIVEN;

			if ( got == 0 ) continue;
			events[k].events = got;
			events[k].data = w->data;
			k++;
			if ( w->events & EPOLLONESHOT ) w->events &= EP_PRIVATE;
		}
	}
	if ( sk != lsk ) free(sk);
	if ( widx != lwidx ) free(widx);
	return k;
}

/* What the set told put in events (the lock held): only for watches still there */
static int ep_told( EPS *p, const T_OBPLEV *kev, int nk, struct ts_epoll_event *events )
{
	int	i, j, k = 0;

	for ( i = 0; i < nk; i++ ) {
		int		fd = (int)(uint32_t)kev[i].data;
		uint32_t	gen = (uint32_t)( kev[i].data >> 32 );

		for ( j = 0; j < p->nw; j++ ) {
			if ( p->w[j].fd == fd && p->w[j].gen == gen && p->w[j].key > 0 ) break;
		}
		if ( j == p->nw ) continue;		/* taken off meanwhile */
		events[k].events = kev[i].events & EP_GIVEN;
		events[k].data = p->w[j].data;
		k++;
	}
	return k;
}

int epoll_wait( int epfd, struct ts_epoll_event *events, int maxevents, int timeout )
{
	T_OBPLEV	kev[KEV_MAX];
	KWAIT		kw;
	EVOBJ		*o;
	EPS		*p;
	int64_t		until = ( timeout > 0 ) ? now_ms() + timeout : 0;
	int		n = 0, er = 0, sock, nk;

	if ( maxevents <= 0 || events == NULL ) {
		errno = EINVAL;
		return -1;
	}
	pthread_mutex_lock(&ev_mu);
	o = get(epfd, K_EPOLL);
	if ( o == NULL ) {
		pthread_mutex_unlock(&ev_mu);
		errno = EBADF;
		return -1;
	}
	p = o->ep;
	p->refs++;
	memset(&kw, 0, sizeof(kw));
	kw.ep = p;
	for ( ;; ) {
		int	slp;

		if ( p->dead ) {
			er = EBADF;
			break;
		}
		n = ep_look(p, events, maxevents, &sock);
		if ( p->nkern > 0 && n < maxevents ) {
			int	room = maxevents - n;

			slp = ( n > 0 ) ? 0 : sleep_for(timeout, until, sock);
			kw.ks = p->ks;
			if ( slp != 0 ) kw_on(&kw);
			pthread_mutex_unlock(&ev_mu);
			nk = set_wait(kw.ks, kev, room, ( slp < 0 ) ? TMO_FEVR : (TMO)slp);
			pthread_mutex_lock(&ev_mu);
			if ( slp != 0 ) kw_off(&kw);
			if ( p->dead ) {
				er = EBADF;
				break;
			}
			if ( nk < 0 ) {
				er = err_of(nk);	/* the process ending (EINTR), or the set gone */
				break;
			}
			if ( slp != 0 ) n = ep_look(p, events, maxevents, &sock);
			if ( nk > 0 ) {
				if ( nk > maxevents - n ) nk = maxevents - n;
				n += ep_told(p, kev, nk, events + n);
			}
			if ( n > 0 || ran_out(timeout, until) ) break;
			continue;
		}
		if ( n > 0 || ran_out(timeout, until) ) break;
		slp = sleep_for(timeout, until, sock);
		cv_wait_ms(slp);
	}
	eps_drop(p);
	pthread_mutex_unlock(&ev_mu);
	if ( er != 0 ) {
		errno = er;
		return -1;
	}
	return n;
}

int epoll_pwait( int epfd, struct ts_epoll_event *events, int maxevents, int timeout,
		 const void *sigmask )
{
	(void)sigmask;
	return epoll_wait(epfd, events, maxevents, timeout);
}

/* ---------------------------------------------------------------- poll */

/* A set for one poll, with nothing in it that it must keep (the lock held) */
static ID pool_take( void )
{
	if ( npool > 0 ) return pool[--npool];
	return set_make();
}

static void pool_give( ID ks )
{
	if ( npool < POOL_MAX ) pool[npool++] = ks;
	else (void)ob_cls_obj(ks);
}

/* The ends of the items put in set ks for this wait, its old keys taken out; an end refused is told POLLNVAL */
static void pool_fill( ID ks, WITEM *it, int n, int nend )
{
	T_OBPLCTL	stack[17], *c = stack;
	int		i, k = 0, at = 0, done;
	ER		er;

	if ( nend + 1 > (int)( sizeof(stack) / sizeof(stack[0]) ) ) {
		c = (T_OBPLCTL *)malloc(sizeof(T_OBPLCTL) * (size_t)( nend + 1 ));
		if ( c == NULL ) {
			for ( i = 0; i < n; i++ ) if ( it[i].kind == W_END ) it[i].got = POLLERR;
			return;
		}
	}
	memset(c, 0, sizeof(T_OBPLCTL) * (size_t)( nend + 1 ));
	c[k++].op = OB_PL_CLEAR;
	for ( i = 0; i < n; i++ ) {
		if ( it[i].kind != W_END ) continue;
		c[k].op = OB_PL_ADD;
		c[k].key = it[i].key;
		c[k].events = it[i].want & ( POLLIN | POLLOUT | POLLRDHUP );
		c[k].data = (UD)i;
		k++;
	}
	while ( at < k ) {
		done = 0;
		er = set_ctl(ks, c + at, k - at, &done);
		if ( er >= E_OK ) break;
		at += done;			/* this one refused: told as it is, the rest put in */
		if ( at < k ) {
			int	j = (int)c[at].data;

			if ( c[at].op == OB_PL_ADD ) {
				it[j].kind = W_FILE;	/* looked at no more */
				it[j].want = 0;
				it[j].got = ( er == E_ID ) ? POLLNVAL : POLLERR;
			}
			at++;
		}
	}
	if ( c != stack ) free(c);
}

/* The items looked at here (the lock held); the ends' got left as it is */
static void poll_look( WITEM *it, int n )
{
	int	i;

	for ( i = 0; i < n; i++ ) {
		if ( it[i].kind == W_END ) continue;
		if ( it[i].kind == W_FILE && it[i].want == 0 && it[i].got != 0 ) continue;	/* refused by the set */
		it[i].got = ( it[i].kind == W_SOCK ) ? 0 : look(it[i].fd, it[i].kind, it[i].want, 0);
	}
	look_socks(it, n);
}

static int poll_count( const WITEM *it, int n )
{
	int	i, r = 0;

	for ( i = 0; i < n; i++ ) if ( it[i].got != 0 ) r++;
	return r;
}

/*
 * poll over descriptors of every kind (ts_socket.c's poll comes here when
 * any of them is not a network socket)
 */
int ts_wait_fds( struct pollfd *fds, unsigned long nfds, int timeout )
{
	WITEM		stack[32], *it = stack;
	T_OBPLEV	kev[KEV_MAX];
	KWAIT		kw;
	unsigned long	i;
	int64_t		until = ( timeout > 0 ) ? now_ms() + timeout : 0;
	int		n = (int)nfds, nend = 0, sock = 0, local = 0, r = 0, er = 0, k;

	if ( nfds > 32 ) {
		it = (WITEM *)malloc(sizeof(WITEM) * nfds);
		if ( it == NULL ) {
			errno = ENOMEM;
			return -1;
		}
	}
	for ( i = 0; i < nfds; i++ ) {
		it[i].fd = fds[i].fd;
		it[i].key = 0;
		it[i].kind = ( fds[i].fd < 0 ) ? W_FILE : kind_of(fds[i].fd, &it[i].key);
		it[i].want = (unsigned)fds[i].events & ( POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM | POLLRDHUP );
		if ( it[i].want & POLLRDNORM ) it[i].want |= POLLIN;
		if ( it[i].want & POLLWRNORM ) it[i].want |= POLLOUT;
		if ( fds[i].fd < 0 ) it[i].want = 0;
		it[i].got = 0;
		if ( it[i].kind == W_END ) nend++;
		else if ( it[i].kind == W_SOCK ) sock++;
		else local++;
	}
	memset(&kw, 0, sizeof(kw));
	kw.it = it;
	kw.n = n;
	pthread_mutex_lock(&ev_mu);
	if ( nend > 0 ) {
		kw.ks = pool_take();
		if ( kw.ks > 0 ) {
			pool_fill(kw.ks, it, n, nend);
		} else {
			for ( k = 0; k < n; k++ ) {
				if ( it[k].kind != W_END ) continue;
				it[k].kind = W_FILE;
				it[k].want = 0;
				it[k].got = POLLERR;
			}
			nend = 0;
		}
	}
	for ( ;; ) {
		int	slp;

		poll_look(it, n);
		r = poll_count(it, n);
		if ( nend > 0 ) {
			int	nk;

			slp = ( r > 0 ) ? 0 : sleep_for(timeout, until, sock);
			if ( slp != 0 ) kw_on(&kw);
			pthread_mutex_unlock(&ev_mu);
			nk = set_wait(kw.ks, kev, nend, ( slp < 0 ) ? TMO_FEVR : (TMO)slp);
			pthread_mutex_lock(&ev_mu);
			if ( slp != 0 ) {
				kw_off(&kw);
				poll_look(it, n);
			}
			if ( nk < 0 ) {
				er = err_of(nk);
				break;
			}
			for ( k = 0; k < n; k++ ) if ( it[k].kind == W_END ) it[k].got = 0;
			for ( k = 0; k < nk; k++ ) {
				int	j = (int)kev[k].data;

				if ( j >= 0 && j < n && it[j].kind == W_END ) {
					it[j].got |= kev[k].events & ( it[j].want | POLLHUP | POLLERR );
				}
			}
			r = poll_count(it, n);
			if ( r > 0 || ran_out(timeout, until) ) break;
			continue;
		}
		if ( r > 0 || ran_out(timeout, until) ) break;
		if ( sock > 0 && local == 0 ) {
			/* sockets alone: the stack waits for them */
			SOCKLIST	l;

			pthread_mutex_unlock(&ev_mu);
			if ( socks_room(&l, n) < 0 ) {
				pthread_mutex_lock(&ev_mu);
				er = ENOMEM;
				break;
			}
			for ( k = 0; k < n; k++ ) {
				l.fds[k] = it[k].fd;
				l.evs[k] = (short)( it[k].want & ( POLLIN | POLLOUT ) );
				l.rev[k] = 0;
			}
			slp = sleep_for(timeout, until, 0);
			r = ts_sock_poll(l.fds, l.evs, l.rev, n, slp);
			for ( k = 0; k < n; k++ ) it[k].got = (unsigned short)l.rev[k];
			if ( r < 0 ) er = errno;
			socks_free(&l);
			pthread_mutex_lock(&ev_mu);
			break;
		}
		slp = sleep_for(timeout, until, sock);
		cv_wait_ms(slp);
	}
	if ( kw.ks > 0 ) pool_give(kw.ks);
	pthread_mutex_unlock(&ev_mu);
	for ( i = 0; i < nfds; i++ ) {
		unsigned g = it[i].got;

		if ( g & POLLIN ) g |= ( fds[i].events & POLLRDNORM );
		if ( g & POLLOUT ) g |= ( fds[i].events & POLLWRNORM );
		fds[i].revents = (short)( g & ( (unsigned)fds[i].events | POLLHUP | POLLERR | POLLNVAL ) );
	}
	if ( it != stack ) free(it);
	if ( er != 0 ) {
		errno = er;
		return -1;
	}
	return r;
}
