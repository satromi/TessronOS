/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_chan.c
 *	Local sockets between processes on the stream objects (OB_S_STREAM
 *	of include/ts/ob.h, include/tsposix/tschan.h, design 18.8, 17.19).
 *
 *	socketpair(AF_UNIX, SOCK_STREAM) makes a pair of streams and opens
 *	both ends; each descriptor (CHFD_BASE up) holds a key to its end.
 *	read and write move bytes through record OB_ST_DATA, sendmsg and
 *	recvmsg through OB_ST_MSG, which carries keys beside the bytes as
 *	SCM_RIGHTS carries descriptors: a descriptor sent is turned into the
 *	key of its object, and a key received into a descriptor of the kind
 *	of object it names -- an end of a stream, a memory object of
 *	memfd_create, a BO of the GPU (a PRIME descriptor) -- or, for any
 *	other object, a descriptor that only holds the key and can be sent on.
 *	The keys are opened not to wait; each call says whether it waits.
 *
 *	epoll_wait and poll wait for ends in a poll set of the system
 *	(ts_evfd.c), which holds their keys; an end is taken out of every
 *	epoll before its key is closed.
 *
 *	A child is handed ends (or any descriptor that is an object's) with
 *	ts_chan_give: the keys are duplicated into it (ob_dup_key) and their
 *	numbers sent in a process message, which the child takes with
 *	ts_chan_take. An end can also be named by its UUID and opened by
 *	another process of the same user (ts_chan_uuid, ts_chan_open).
 */

#include <config.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ob.h>
#include <ts/proc.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <poll.h>
#include <tschan.h>

#define CHFD_BASE	0x3880		/* between the random sources and the memory objects */
#define CHFD_MAX	512
#define STACK_IO	4096		/* bytes moved through a buffer on the stack */
#define CH_MSG_TYPE	0x00005443	/* the process message ts_chan_give sends */

enum { C_FREE, C_END, C_KEY };

typedef struct {
	int	kind;
	ID	key;
	int	nb;			/* O_NONBLOCK */
	int	cloexec;
	int	has_uuid;
	TS_UUID	uuid;			/* the end's own, when known */
	UINT	type, sub;		/* C_KEY: what it is a key of */
} CHFD;

static CHFD		ch[CHFD_MAX];
static pthread_mutex_t	ch_mu = PTHREAD_MUTEX_INITIALIZER;

/* the memory objects and the GPU's descriptors, when they can be carried */
extern ID	ts_memfd_key( int fd ) __attribute__((weak));
extern int	ts_memfd_from_key( ID key ) __attribute__((weak));
extern ID	ts_gpu_fd_key( int fd ) __attribute__((weak));
extern int	ts_gpu_fd_from_key( ID key ) __attribute__((weak));

int	ts_chan_close( int fd );

/* ts_evfd.c: a descriptor about to be closed taken out of every epoll */
extern int	ts_evfd_forget( int fd );

static CHFD *get( int fd )
{
	int	i = fd - CHFD_BASE;

	if ( i < 0 || i >= CHFD_MAX || ch[i].kind == C_FREE ) return NULL;
	return &ch[i];
}

int ts_chan_is( int fd )
{
	return fd >= CHFD_BASE && fd < CHFD_BASE + CHFD_MAX;
}

static int to_errno( ER er, int writing )
{
	switch ( er ) {
	case E_TMOUT:	return EAGAIN;
	case E_IO:	return writing ? EPIPE : ECONNRESET;
	case E_NOEXS:
	case E_ID:	return EBADF;
	case E_OACV:	return EACCES;
	case E_PAR:	return EINVAL;
	case E_NOMEM:	return ENOMEM;
	case E_LIMIT:	return ETOOMANYREFS;
	case E_DISWAI:	return EINTR;
	default:	return EIO;
	}
}

static int fail( ER er, int writing )
{
	errno = to_errno(er, writing);
	return -1;
}

/* ---------------------------------------------------------------- descriptors */

static int fd_new( int kind, ID key, UINT type, UINT sub, int nb )
{
	int	i;

	pthread_mutex_lock(&ch_mu);
	for ( i = 0; i < CHFD_MAX; i++ ) {
		if ( ch[i].kind == C_FREE ) {
			memset(&ch[i], 0, sizeof(ch[i]));
			ch[i].kind = kind;
			ch[i].key = key;
			ch[i].nb = nb;
			ch[i].type = type;
			ch[i].sub = sub;
			break;
		}
	}
	pthread_mutex_unlock(&ch_mu);
	if ( i == CHFD_MAX ) {
		errno = EMFILE;
		return -1;
	}
	return CHFD_BASE + i;
}

/* A key received made a descriptor of the kind of object it names; -1 when it cannot be */
static int fd_of_key( ID key, UINT type, UINT sub )
{
	int	fd = -1;

	if ( type == OB_T_CHANNEL && sub == OB_S_STREAM ) {
		fd = fd_new(C_END, key, type, sub, 0);
	} else if ( type == OB_T_STORAGE && sub == OB_S_MEMORY && ts_memfd_from_key != NULL ) {
		fd = ts_memfd_from_key(key);
	} else if ( type == OB_T_STORAGE && sub == OB_S_GPUBO && ts_gpu_fd_from_key != NULL ) {
		fd = ts_gpu_fd_from_key(key);
	} else {
		fd = fd_new(C_KEY, key, type, sub, 0);
	}
	return fd;
}

int ts_fd_key( int fd )
{
	CHFD	*c;
	ID	k = 0;

	pthread_mutex_lock(&ch_mu);
	c = get(fd);
	if ( c != NULL ) k = c->key;
	pthread_mutex_unlock(&ch_mu);
	if ( k > 0 ) return k;
	if ( ts_memfd_key != NULL && ( k = ts_memfd_key(fd) ) > 0 ) return k;
	if ( ts_gpu_fd_key != NULL && ( k = ts_gpu_fd_key(fd) ) > 0 ) return k;
	errno = EBADF;
	return -1;
}

int ts_fd_from_key( int key, unsigned type, unsigned sub )
{
	return fd_of_key((ID)key, type, sub);
}

static ER end_state( ID key, T_OBSTSTAT *s )
{
	SZ	asz = 0;

	return ob_rea_rec(key, OB_ST_STATE, 0, s, sizeof(*s), &asz);
}

int ts_chan_pair( int type, int sv[2] )
{
	T_OBCRE		c;
	T_OBSTSTAT	s;
	TS_UUID		ua;
	ID		ka, kb, keys[2];
	int		nb = ( type & SOCK_NONBLOCK ) != 0, a, b;
	ER		er;

	if ( ( type & 0xf ) != SOCK_STREAM ) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.sub = OB_S_STREAM;
	er = ob_cre_key(&c, OB_OP_R | OB_OP_W | OB_O_NOWAIT, keys, &ua);
	if ( er < E_OK ) return fail(er, 0);
	ka = keys[0];
	kb = keys[1];
	a = fd_new(C_END, ka, OB_T_CHANNEL, OB_S_STREAM, nb);
	b = ( a >= 0 ) ? fd_new(C_END, kb, OB_T_CHANNEL, OB_S_STREAM, nb) : -1;
	if ( b < 0 ) {
		if ( a >= 0 ) ts_chan_close(a); else (void)ob_cls_obj(ka);
		(void)ob_cls_obj(kb);
		return -1;
	}
	er = end_state(ka, &s);			/* the other end is named there */
	pthread_mutex_lock(&ch_mu);
	get(a)->uuid = ua;
	get(a)->has_uuid = 1;
	if ( er >= E_OK ) {
		get(b)->uuid = s.peer;
		get(b)->has_uuid = 1;
	}
	if ( type & SOCK_CLOEXEC ) get(a)->cloexec = get(b)->cloexec = 1;
	pthread_mutex_unlock(&ch_mu);
	sv[0] = a;
	sv[1] = b;
	return 0;
}

int ts_chan_close( int fd )
{
	CHFD	*c;
	ID	k = 0;

	if ( !ts_chan_is(fd) ) {
		errno = EBADF;
		return -1;
	}
	(void)ts_evfd_forget(fd);		/* while its key still names it */
	pthread_mutex_lock(&ch_mu);
	c = get(fd);
	if ( c != NULL ) {
		k = c->key;
		c->kind = C_FREE;
	}
	pthread_mutex_unlock(&ch_mu);
	if ( c == NULL ) {
		errno = EBADF;
		return -1;
	}
	(void)ob_cls_obj(k);
	return 0;
}

static int end_key( int fd, ID *p_key, int *p_nb )
{
	CHFD	*c;

	pthread_mutex_lock(&ch_mu);
	c = get(fd);
	if ( c != NULL && c->kind == C_END ) {
		*p_key = c->key;
		*p_nb = c->nb;
	}
	pthread_mutex_unlock(&ch_mu);
	if ( c == NULL || c->kind != C_END ) {
		errno = ( c == NULL ) ? EBADF : ENOTSOCK;
		return -1;
	}
	return 0;
}

long ts_chan_read( int fd, void *buf, size_t n, int flags )
{
	ID	key;
	SZ	asz = 0;
	int	nb;
	ER	er;

	if ( end_key(fd, &key, &nb) < 0 ) return -1;
	if ( flags & MSG_DONTWAIT ) nb = 1;
	er = ob_rea_tmo(key, OB_ST_DATA, 0, buf, (SZ)n, &asz, nb ? TMO_POL : TMO_FEVR);
	return ( er < E_OK ) ? fail(er, 0) : (long)asz;
}

/*
 * Bytes written with keys going with the first of them: as much as there
 * is room for when it may not wait, else all of it.
 */
static long put( ID key, int nb, const struct iovec *iov, size_t niov, const T_OBSTKEY *keys, int nk )
{
	UB	stack[STACK_IO], *b;
	size_t	total = 0, done = 0, i;
	SZ	head = (SZ)OB_STM_HEAD(nk), cap, n, asz;
	ER	er = E_OK;

	for ( i = 0; i < niov; i++ ) total += iov[i].iov_len;
	if ( total == 0 ) return 0;
	cap = ( total > OB_ST_BUF ) ? OB_ST_BUF : (SZ)total;
	b = ( head + cap <= (SZ)sizeof(stack) ) ? stack : (UB *)malloc((size_t)( head + cap ));
	if ( b == NULL ) {
		errno = ENOMEM;
		return -1;
	}
	{
		size_t	at = 0, off = 0;		/* where in iov[at] the next chunk starts */

		while ( done < total ) {
			T_OBSTMSG	*m = (T_OBSTMSG *)b;
			SZ		k = 0;

			memset(m, 0, sizeof(*m));
			m->flags = nb ? OB_STM_NOWAIT : OB_STM_WAIT;
			m->room = nk;
			m->nkey = ( done == 0 ) ? nk : 0;
			if ( nk > 0 ) memcpy(m->key, keys, sizeof(T_OBSTKEY) * (size_t)nk);
			n = ( total - done > (size_t)cap ) ? cap : (SZ)( total - done );
			while ( k < n ) {
				size_t	c = iov[at].iov_len - off;

				if ( c > (size_t)( n - k ) ) c = (size_t)( n - k );
				memcpy(b + head + k, (const UB *)iov[at].iov_base + off, c);
				k += (SZ)c;
				off += c;
				if ( off == iov[at].iov_len ) {
					at++;
					off = 0;
				}
			}
			asz = 0;
			er = ob_wri_rec(key, OB_ST_MSG, 0, b, head + n, &asz);
			if ( er < E_OK ) break;
			done += (size_t)asz;
			if ( nb || asz < n ) break;
		}
	}
	if ( b != stack ) free(b);
	if ( done > 0 ) return (long)done;
	return fail(er, 1);
}

long ts_chan_write( int fd, const void *buf, size_t n, int flags )
{
	struct iovec	v;
	ID		key;
	int		nb;

	if ( end_key(fd, &key, &nb) < 0 ) return -1;
	if ( flags & MSG_DONTWAIT ) nb = 1;
	v.iov_base = (void *)buf;
	v.iov_len = n;
	return put(key, nb, &v, 1, NULL, 0);
}

/* The keys of the descriptors of the SCM_RIGHTS messages in msg; how many, or -1 */
static int keys_of( const struct msghdr *msg, T_OBSTKEY *keys, int max )
{
	const UB	*c = (const UB *)msg->msg_control, *end = c + msg->msg_controllen;
	int		n = 0;

	while ( c != NULL && c + sizeof(struct cmsghdr) <= end ) {
		const struct cmsghdr	*h = (const struct cmsghdr *)c;
		size_t			k, cnt;

		if ( h->cmsg_len < CMSG_LEN(0) || c + h->cmsg_len > end ) break;
		if ( h->cmsg_level == SOL_SOCKET && h->cmsg_type == SCM_RIGHTS ) {
			cnt = ( h->cmsg_len - CMSG_LEN(0) ) / sizeof(int);
			for ( k = 0; k < cnt; k++ ) {
				int	fd;
				ID	key;

				memcpy(&fd, CMSG_DATA(h) + k * sizeof(int), sizeof(int));
				if ( n == max ) {
					errno = ETOOMANYREFS;
					return -1;
				}
				key = ts_fd_key(fd);
				if ( key <= 0 ) {
					errno = EBADF;		/* not a descriptor of an object */
					return -1;
				}
				keys[n].key = key;
				keys[n].type = 0;
				keys[n].sub = 0;
				n++;
			}
		}
		c += CMSG_ALIGN(h->cmsg_len);
	}
	return n;
}

long ts_chan_sendmsg( int fd, const struct msghdr *msg, int flags )
{
	T_OBSTKEY	*keys;
	ID		key;
	long		r;
	int		nb, nk;

	if ( end_key(fd, &key, &nb) < 0 ) return -1;
	if ( flags & MSG_DONTWAIT ) nb = 1;
	keys = (T_OBSTKEY *)malloc(sizeof(T_OBSTKEY) * OB_ST_KEY_MAX);
	if ( keys == NULL ) {
		errno = ENOMEM;
		return -1;
	}
	nk = ( msg->msg_control != NULL ) ? keys_of(msg, keys, OB_ST_KEY_MAX) : 0;
	if ( nk < 0 ) {
		free(keys);
		return -1;
	}
	r = put(key, nb, msg->msg_iov, msg->msg_iovlen, keys, nk);
	free(keys);
	return r;
}

long ts_chan_recvmsg( int fd, struct msghdr *msg, int flags )
{
	T_OBSTMSG	*m;
	ID		key;
	size_t		total = 0, i, k;
	SZ		head, cap, asz = 0;
	int		nb, room = 0, nfd = 0, trunc = 0;
	ER		er;

	if ( end_key(fd, &key, &nb) < 0 ) return -1;
	if ( flags & MSG_DONTWAIT ) nb = 1;
	for ( i = 0; i < msg->msg_iovlen; i++ ) total += msg->msg_iov[i].iov_len;
	if ( msg->msg_control != NULL && msg->msg_controllen >= CMSG_LEN(sizeof(int)) ) {
		room = (int)( ( msg->msg_controllen - CMSG_LEN(0) ) / sizeof(int) );
		if ( room > OB_ST_KEY_MAX ) room = OB_ST_KEY_MAX;
	}
	head = (SZ)OB_STM_HEAD(room);
	cap = ( total > OB_ST_BUF ) ? OB_ST_BUF : (SZ)total;
	m = (T_OBSTMSG *)malloc((size_t)( head + cap ));
	if ( m == NULL ) {
		errno = ENOMEM;
		return -1;
	}
	memset(m, 0, sizeof(*m));
	m->room = room;
	er = ob_rea_tmo(key, OB_ST_MSG, 0, m, head + cap, &asz, nb ? TMO_POL : TMO_FEVR);
	if ( er < E_OK ) {
		free(m);
		return fail(er, 0);
	}
	/* the bytes into the buffers given */
	for ( i = 0, k = 0; i < msg->msg_iovlen && k < (size_t)asz; i++ ) {
		size_t	c = msg->msg_iov[i].iov_len;

		if ( c > (size_t)asz - k ) c = (size_t)asz - k;
		memcpy(msg->msg_iov[i].iov_base, OB_STM_DATA(m) + k, c);
		k += c;
	}
	/* the keys into descriptors */
	if ( m->lost > 0 ) trunc = 1;
	if ( m->nkey > 0 ) {
		struct cmsghdr	*h = (struct cmsghdr *)msg->msg_control;
		int		*fds = (int *)CMSG_DATA(h);

		for ( i = 0; i < (size_t)m->nkey; i++ ) {
			int	nfd1 = fd_of_key(m->key[i].key, m->key[i].type, m->key[i].sub);

			if ( nfd1 < 0 ) {
				(void)ob_cls_obj(m->key[i].key);
				trunc = 1;
				continue;
			}
			memcpy(&fds[nfd], &nfd1, sizeof(int));
			nfd++;
		}
		if ( nfd > 0 ) {
			h->cmsg_level = SOL_SOCKET;
			h->cmsg_type = SCM_RIGHTS;
			h->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfd);
		}
	}
	msg->msg_controllen = ( nfd > 0 ) ? CMSG_SPACE(sizeof(int) * (size_t)nfd) : 0;
	msg->msg_flags = trunc ? MSG_CTRUNC : 0;
	msg->msg_namelen = 0;
	free(m);
	return (long)asz;
}

int ts_chan_shutdown( int fd, int how )
{
	ID	key;
	SZ	asz = 0;
	INT	w = OB_SHUT_WR;
	int	nb;
	ER	er;

	if ( end_key(fd, &key, &nb) < 0 ) return -1;
	if ( how == SHUT_RD ) return 0;		/* what comes is still read, as Linux has it */
	er = ob_wri_rec(key, OB_ST_STATE, 0, &w, sizeof(w), &asz);
	return ( er < E_OK ) ? fail(er, 0) : 0;
}

int ts_chan_fcntl( int fd, int cmd, int arg )
{
	CHFD	*c;
	int	r = 0;

	pthread_mutex_lock(&ch_mu);
	c = get(fd);
	if ( c != NULL ) {
		switch ( cmd ) {
		case F_GETFL:	r = O_RDWR | ( c->nb ? O_NONBLOCK : 0 ); break;
		case F_SETFL:	c->nb = ( arg & O_NONBLOCK ) != 0; break;
		case F_GETFD:	r = c->cloexec ? FD_CLOEXEC : 0; break;
		case F_SETFD:	c->cloexec = ( arg & FD_CLOEXEC ) != 0; break;
		default:	break;
		}
	}
	pthread_mutex_unlock(&ch_mu);
	if ( c == NULL ) {
		errno = EBADF;
		return -1;
	}
	return r;
}

int ts_chan_fstat( int fd, struct stat *st )
{
	CHFD	*c;

	pthread_mutex_lock(&ch_mu);
	c = get(fd);
	pthread_mutex_unlock(&ch_mu);
	if ( c == NULL ) {
		errno = EBADF;
		return -1;
	}
	memset(st, 0, sizeof(*st));
	st->st_mode = ( c->kind == C_END ) ? ( S_IFSOCK | 0600 ) : ( S_IFREG | 0600 );
	return 0;
}

/* What a descriptor is, for a wait: 1 an end (its key in *p_key), 0 one that only holds a key, -1 neither */
int ts_chan_end( int fd, ID *p_key )
{
	CHFD	*c;
	int	r = -1;

	pthread_mutex_lock(&ch_mu);
	c = get(fd);
	if ( c != NULL ) {
		r = ( c->kind == C_END ) ? 1 : 0;
		*p_key = c->key;
	}
	pthread_mutex_unlock(&ch_mu);
	return r;
}

/* ---------------------------------------------------------------- naming and handing on */

int ts_chan_uuid( int fd, unsigned char uuid[16] )
{
	CHFD	*c;
	int	ok = 0;

	pthread_mutex_lock(&ch_mu);
	c = get(fd);
	if ( c != NULL && c->has_uuid ) {
		memcpy(uuid, c->uuid.b, 16);
		ok = 1;
	}
	pthread_mutex_unlock(&ch_mu);
	if ( !ok ) {
		errno = ( c == NULL ) ? EBADF : ENOENT;
		return -1;
	}
	return 0;
}

int ts_chan_open( const unsigned char uuid[16] )
{
	TS_UUID	u;
	ID	k;
	int	fd;

	memcpy(u.b, uuid, 16);
	k = ob_opn_obj(&u, OB_OP_R | OB_OP_W | OB_O_NOWAIT);
	if ( k < E_OK ) return fail(k, 0);
	fd = fd_new(C_END, k, OB_T_CHANNEL, OB_S_STREAM, 0);
	if ( fd < 0 ) {
		(void)ob_cls_obj(k);
		return -1;
	}
	pthread_mutex_lock(&ch_mu);
	get(fd)->uuid = u;
	get(fd)->has_uuid = 1;
	pthread_mutex_unlock(&ch_mu);
	return fd;
}

/* What kind of object a descriptor is a key of, for the child to make it again */
static void kind_of( int fd, UINT *type, UINT *sub )
{
	CHFD	*c;

	*type = 0;
	*sub = 0;
	pthread_mutex_lock(&ch_mu);
	c = get(fd);
	if ( c != NULL ) {
		*type = c->type;
		*sub = c->sub;
	}
	pthread_mutex_unlock(&ch_mu);
	if ( c != NULL ) return;
	if ( ts_memfd_key != NULL && ts_memfd_key(fd) > 0 ) {
		*type = OB_T_STORAGE;
		*sub = OB_S_MEMORY;
	} else if ( ts_gpu_fd_key != NULL && ts_gpu_fd_key(fd) > 0 ) {
		*type = OB_T_STORAGE;
		*sub = OB_S_GPUBO;
	}
}

int ts_chan_give( int pid, const int *fds, int n )
{
	T_TSMSG		m;
	T_OBSTKEY	k;
	int		i;
	ER		er;

	if ( n < 0 || (size_t)n * sizeof(k) + sizeof(int) > sizeof(m.body) ) {
		errno = EINVAL;
		return -1;
	}
	memset(&m, 0, sizeof(m));
	m.type = CH_MSG_TYPE;
	memcpy(m.body, &n, sizeof(int));
	for ( i = 0; i < n; i++ ) {
		ID	mine = ts_fd_key(fds[i]);

		if ( mine <= 0 ) return -1;
		k.key = ob_dup_key(mine, OB_OP_ALL, (ID)pid);
		if ( k.key < E_OK ) return fail(k.key, 0);
		kind_of(fds[i], &k.type, &k.sub);
		memcpy(m.body + sizeof(int) + sizeof(k) * (size_t)i, &k, sizeof(k));
	}
	m.size = (SZ)( sizeof(int) + sizeof(k) * (size_t)n );
	er = ts_snd_msg((ID)pid, &m, TMO_FEVR);
	return ( er < E_OK ) ? fail(er, 0) : 0;
}

int ts_chan_take( int *fds, int max, int ms )
{
	T_TSMSG		m;
	T_OBSTKEY	k;
	int		n, i, got = 0;
	ER		er;

	er = ts_rcv_msg(&m, ( ms < 0 ) ? TMO_FEVR : (TMO)ms);
	if ( er < E_OK ) return fail(er, 0);
	if ( m.type != CH_MSG_TYPE || m.size < (SZ)sizeof(int) ) {
		errno = EPROTO;
		return -1;
	}
	memcpy(&n, m.body, sizeof(int));
	for ( i = 0; i < n && (size_t)( i + 1 ) * sizeof(k) + sizeof(int) <= (size_t)m.size; i++ ) {
		int	fd;

		memcpy(&k, m.body + sizeof(int) + sizeof(k) * (size_t)i, sizeof(k));
		fd = ( got < max ) ? fd_of_key(k.key, k.type, k.sub) : -1;
		if ( fd < 0 ) {
			(void)ob_cls_obj(k.key);
			continue;
		}
		fds[got++] = fd;
	}
	return got;
}
