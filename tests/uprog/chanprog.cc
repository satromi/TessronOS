/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	chanprog.cc
 *	Local sockets between two processes (lib/libcxxrt/ts_chan.c, the
 *	streams of design 18.8), run by tests/ktest/ktest_chan2.c as
 *	/boot/CHANPROG.ELF.
 *
 *	The test starts it twice: with "child", then with "parent:" and the
 *	child's process number; the two act as the same user. The parent
 *	makes a pair, hands the child one end (ts_chan_give) and goes through
 *	the steps with it: bytes both ways, a read that may not wait,
 *	epoll_wait woken by the end and by an eventfd, an end carried by
 *	SCM_RIGHTS and used by the child, an end carried and never received
 *	when the child ends, and what is left of the pair once the child is
 *	gone. Before it takes its end the child goes through epoll and poll
 *	on pairs of its own (local_waits). Each one's exit code is the first
 *	of its steps that failed, 0 when all passed.
 */

#include <tk/typedef.h>
#include <tk/errno.h>
extern "C" {
#include <tk/syscall.h>
}
#include <ts/proc.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <tschan.h>

extern "C" {
int	eventfd( unsigned int initval, int flags );
int	epoll_create1( int flags );
struct ep_ev {
	uint32_t	events;
	uint64_t	data;
};
int	epoll_ctl( int epfd, int op, int fd, struct ep_ev *ev );
int	epoll_wait( int epfd, struct ep_ev *evs, int max, int timeout );
}

#define EP_IN		0x001
#define EP_HUP		0x010
#define EP_ADD		1
#define WAIT_MS		20000

static int	failed = 0;
static const char *who = "chanprog";

#define STEP(n, name, cond) \
	do { \
		if ( cond ) { \
			std::printf("%s: %s ok\n", who, name); \
		} else { \
			std::printf("%s: %s FAILED (errno %d)\n", who, name, errno); \
			if ( failed == 0 ) failed = (n); \
		} \
	} while (0)

/* n bytes read, waiting up to WAIT_MS for them */
static int get( int fd, char *buf, int n )
{
	struct pollfd	p;
	int		k = 0, r;

	while ( k < n ) {
		p.fd = fd;
		p.events = POLLIN;
		p.revents = 0;
		if ( poll(&p, 1, WAIT_MS) <= 0 ) return k;
		r = (int)read(fd, buf + k, (size_t)( n - k ));
		if ( r <= 0 ) return k;
		k += r;
	}
	return k;
}

static int says( int fd, const char *want )
{
	char	b[32];
	int	n = (int)std::strlen(want);

	std::memset(b, 0, sizeof(b));
	return get(fd, b, n) == n && std::memcmp(b, want, (size_t)n) == 0;
}

static int put( int fd, const char *s )
{
	return write(fd, s, std::strlen(s)) == (ssize_t)std::strlen(s);
}

static void sleep_ms( int ms )
{
	struct timespec	t = { ms / 1000, ( ms % 1000 ) * 1000000L };

	nanosleep(&t, nullptr);
}

/* ---------------------------------------------------------------- epoll and poll within one process */

#define EP_OUT		0x004
#define EP_RDHUP	0x2000
#define EP_ONESHOT	( 1U << 30 )
#define EP_DEL		2
#define EP_MOD		3

/* What another thread does to an epoll while one waits on it, after a while */
typedef struct {
	int	what;		/* 1 add fd, 2 close ep, 3 close fd, 4 write the eventfd */
	int	ep, fd;
} LTHR;

static void *later( void *arg )
{
	LTHR		*l = (LTHR *)arg;
	struct ep_ev	e = { EP_IN, 15 };
	uint64_t	one = 1;

	sleep_ms(50);
	switch ( l->what ) {
	case 1:	(void)epoll_ctl(l->ep, EP_ADD, l->fd, &e); break;
	case 2:	(void)close(l->ep); break;
	case 3:	(void)close(l->fd); break;
	case 4:	(void)write(l->fd, &one, sizeof(one)); break;
	}
	return nullptr;
}

static int has( const struct ep_ev *got, int n, uint64_t data, uint32_t ev )
{
	int	i;

	for ( i = 0; i < n; i++ ) {
		if ( got[i].data == data && ( got[i].events & ev ) == ev ) return 1;
	}
	return 0;
}

/*
 * Steps 30 on, in the child before it takes its end: one end in two
 * epolls, level, for once, room to write, taken out, another thread
 * adding, closing the epoll and closing the end while one waits, the
 * end of the other end, and poll over an end and an eventfd.
 */
static void local_waits( void )
{
	int		a[2], c[2], h[2], ep1, ep2, ep3, efd, n;
	struct ep_ev	e, got[4];
	char		b[8];
	pthread_t	t;
	LTHR		l;

	if ( socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, a) != 0
	  || socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, c) != 0
	  || socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, h) != 0 ) {
		STEP(30, "pairs", 0);
		return;
	}
	ep1 = epoll_create1(0);
	ep2 = epoll_create1(0);
	e.events = EP_IN;
	e.data = 11;
	STEP(31, "an end in two epolls", epoll_ctl(ep1, EP_ADD, a[0], &e) == 0
	     && ( e.data = 22, epoll_ctl(ep2, EP_ADD, a[0], &e) == 0 ));
	STEP(32, "nothing yet", epoll_wait(ep1, got, 4, 0) == 0);
	(void)put(a[1], "x");
	n = epoll_wait(ep1, got, 4, WAIT_MS);
	STEP(33, "told in the first", n == 1 && got[0].data == 11 && got[0].events == EP_IN);
	n = epoll_wait(ep2, got, 4, 0);
	STEP(34, "and in the second", n == 1 && got[0].data == 22);
	STEP(35, "again while it stays ready", epoll_wait(ep1, got, 4, 0) == 1);
	(void)read(a[0], b, sizeof(b));
	STEP(36, "no more once read", epoll_wait(ep1, got, 4, 0) == 0 && epoll_wait(ep2, got, 4, 0) == 0);

	e.events = EP_IN | EP_ONESHOT;
	e.data = 12;
	(void)epoll_ctl(ep1, EP_MOD, a[0], &e);
	(void)put(a[1], "y");
	n = epoll_wait(ep1, got, 4, WAIT_MS);
	STEP(37, "for once", n == 1 && got[0].data == 12 && epoll_wait(ep1, got, 4, 0) == 0);
	e.data = 13;
	(void)epoll_ctl(ep1, EP_MOD, a[0], &e);
	n = epoll_wait(ep1, got, 4, 0);
	STEP(38, "again once asked again", n == 1 && got[0].data == 13);
	(void)read(a[0], b, sizeof(b));

	e.events = EP_OUT;
	e.data = 14;
	(void)epoll_ctl(ep1, EP_ADD, a[1], &e);
	n = epoll_wait(ep1, got, 4, 0);
	STEP(39, "room to write", n == 1 && got[0].data == 14 && got[0].events == EP_OUT);
	STEP(40, "taken out", epoll_ctl(ep1, EP_DEL, a[1], nullptr) == 0 && epoll_wait(ep1, got, 4, 0) == 0
	     && epoll_ctl(ep1, EP_DEL, a[1], nullptr) < 0 && errno == ENOENT);

	/* another thread adds an end that has something while one waits */
	(void)put(c[1], "z");
	l.what = 1;
	l.ep = ep1;
	l.fd = c[0];
	pthread_create(&t, nullptr, later, &l);
	n = epoll_wait(ep1, got, 4, WAIT_MS);
	pthread_join(t, nullptr);
	STEP(41, "added while waiting", n == 1 && got[0].data == 15);
	(void)read(c[0], b, sizeof(b));

	/* another thread closes an end watched while one waits: nothing told */
	l.what = 3;
	l.fd = c[0];
	pthread_create(&t, nullptr, later, &l);
	n = epoll_wait(ep1, got, 4, 300);
	pthread_join(t, nullptr);
	(void)put(c[1], "w");
	STEP(42, "an end closed while waiting", n == 0 && epoll_wait(ep1, got, 4, 0) == 0);

	/* another thread closes the epoll while one waits on it */
	ep3 = epoll_create1(0);
	e.events = EP_IN;
	e.data = 16;
	(void)epoll_ctl(ep3, EP_ADD, a[0], &e);
	l.what = 2;
	l.ep = ep3;
	pthread_create(&t, nullptr, later, &l);
	errno = 0;
	n = epoll_wait(ep3, got, 4, WAIT_MS);
	pthread_join(t, nullptr);
	STEP(43, "the epoll closed while waiting: EBADF", n < 0 && errno == EBADF);

	/* the other end says it writes no more, then goes */
	e.events = EP_IN | EP_RDHUP;
	e.data = 17;
	(void)epoll_ctl(ep2, EP_ADD, h[0], &e);
	(void)shutdown(h[1], SHUT_WR);
	n = epoll_wait(ep2, got, 4, WAIT_MS);
	STEP(44, "nothing more to come: EPOLLRDHUP", n == 1 && got[0].data == 17
	     && ( got[0].events & ( EP_IN | EP_RDHUP ) ) == ( EP_IN | EP_RDHUP ) && !( got[0].events & EP_HUP ));
	close(h[1]);
	n = epoll_wait(ep2, got, 4, WAIT_MS);
	STEP(45, "the other end gone: EPOLLHUP", n == 1 && ( got[0].events & ( EP_HUP | EP_RDHUP ) ) == ( EP_HUP | EP_RDHUP ));

	/* poll over an end and an eventfd another thread writes */
	efd = eventfd(0, 0);
	{
		struct pollfd	p[2] = { { a[0], POLLIN, 0 }, { efd, POLLIN, 0 } };

		l.what = 4;
		l.fd = efd;
		pthread_create(&t, nullptr, later, &l);
		n = poll(p, 2, WAIT_MS);
		pthread_join(t, nullptr);
		STEP(46, "poll woken by the eventfd", n == 1 && p[0].revents == 0 && ( p[1].revents & POLLIN ));
		(void)put(a[1], "v");
		p[1].revents = 0;
		n = poll(p, 2, WAIT_MS);
		STEP(47, "and by the end", n == 2 && ( p[0].revents & POLLIN ) && ( p[1].revents & POLLIN ));
	}
	close(efd);
	close(ep1);
	close(ep2);
	close(a[0]);
	close(a[1]);
	close(c[1]);
	close(h[0]);
}

/* ---------------------------------------------------------------- the child */

static int	ev_fd;

static void *kick( void * )
{
	uint64_t	one = 1;

	sleep_ms(50);
	(void)write(ev_fd, &one, sizeof(one));
	return nullptr;
}

static int child( void )
{
	int		fd, ep, n, x;
	char		b[8];
	struct ep_ev	e, got[4];
	pthread_t	t;

	who = "chanprog child";
	local_waits();
	n = ts_chan_take(&fd, 1, WAIT_MS);
	STEP(1, "take the end", n == 1);
	if ( n != 1 ) return failed;

	STEP(2, "ping", says(fd, "ping"));
	STEP(3, "pong", put(fd, "pong"));

	/* a read that may not wait finds nothing yet */
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	errno = 0;
	STEP(4, "nothing there: EAGAIN", read(fd, b, 1) < 0 && errno == EAGAIN);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
	STEP(5, "nb-ok", put(fd, "nb-ok"));

	/* epoll woken by the end, then by an eventfd another thread writes */
	ep = epoll_create1(0);
	ev_fd = eventfd(0, 0);
	e.events = EP_IN;
	e.data = 1;
	STEP(6, "watch the end", epoll_ctl(ep, EP_ADD, fd, &e) == 0);
	e.data = 2;
	STEP(7, "watch the eventfd", epoll_ctl(ep, EP_ADD, ev_fd, &e) == 0);
	n = epoll_wait(ep, got, 4, WAIT_MS);
	STEP(8, "woken by the end", n == 1 && got[0].data == 1 && ( got[0].events & EP_IN ));
	STEP(9, "wake", says(fd, "wake"));
	pthread_create(&t, nullptr, kick, nullptr);
	n = epoll_wait(ep, got, 4, WAIT_MS);
	STEP(10, "woken by the eventfd", n == 1 && got[0].data == 2);
	pthread_join(t, nullptr);
	STEP(11, "ep-ok", put(fd, "ep-ok"));
	close(ev_fd);
	close(ep);

	/* an end carried to it: read and written through */
	{
		struct msghdr	m;
		struct iovec	v;
		union {
			struct cmsghdr	h;
			char		b[CMSG_SPACE(sizeof(int) * 4)];
		} c;
		char		one;
		struct pollfd	p = { fd, POLLIN, 0 };

		std::memset(&m, 0, sizeof(m));
		v.iov_base = &one;
		v.iov_len = 1;
		m.msg_iov = &v;
		m.msg_iovlen = 1;
		m.msg_control = c.b;
		m.msg_controllen = sizeof(c.b);
		(void)poll(&p, 1, WAIT_MS);
		n = (int)recvmsg(fd, &m, 0);
		x = -1;
		if ( n == 1 && m.msg_controllen >= CMSG_LEN(sizeof(int)) && c.h.cmsg_type == SCM_RIGHTS ) {
			std::memcpy(&x, CMSG_DATA(&c.h), sizeof(int));
		}
		STEP(12, "an end received", n == 1 && one == 'R' && x >= 0);
		STEP(13, "read through it", x >= 0 && says(x, "via"));
		STEP(14, "written through it", x >= 0 && put(x, "back"));
		if ( x >= 0 ) close(x);
	}

	/* told to go: the end carried after this is never received */
	STEP(15, "bye", says(fd, "bye"));
	sleep_ms(300);				/* the parent sends an end meanwhile, never taken */
	return failed;
}

/* ---------------------------------------------------------------- the parent */

static int parent( int pid )
{
	int		sv[2], cd[2], ef[2];
	char		b[8];

	who = "chanprog";
	STEP(1, "socketpair", socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
	if ( failed ) return failed;
	STEP(2, "the child's number", pid > 0);
	if ( pid <= 0 ) return failed;
	STEP(3, "hand it an end", ts_chan_give(pid, &sv[1], 1) == 0);
	close(sv[1]);

	STEP(4, "ping", put(sv[0], "ping"));
	STEP(5, "pong", says(sv[0], "pong"));
	STEP(6, "nb-ok", says(sv[0], "nb-ok"));
	sleep_ms(100);				/* the child sleeps in epoll_wait */
	STEP(7, "wake", put(sv[0], "wake"));
	STEP(8, "ep-ok", says(sv[0], "ep-ok"));

	/* an end carried by SCM_RIGHTS */
	STEP(9, "a second pair", socketpair(AF_UNIX, SOCK_STREAM, 0, cd) == 0);
	{
		struct msghdr	m;
		struct iovec	v;
		union {
			struct cmsghdr	h;
			char		b[CMSG_SPACE(sizeof(int))];
		} c;
		char		one = 'R';

		std::memset(&m, 0, sizeof(m));
		std::memset(&c, 0, sizeof(c));
		v.iov_base = &one;
		v.iov_len = 1;
		m.msg_iov = &v;
		m.msg_iovlen = 1;
		m.msg_control = c.b;
		m.msg_controllen = sizeof(c.b);
		c.h.cmsg_level = SOL_SOCKET;
		c.h.cmsg_type = SCM_RIGHTS;
		c.h.cmsg_len = CMSG_LEN(sizeof(int));
		std::memcpy(CMSG_DATA(&c.h), &cd[1], sizeof(int));
		STEP(10, "send an end", sendmsg(sv[0], &m, 0) == 1);
		close(cd[1]);
	}
	STEP(11, "via", put(cd[0], "via"));
	STEP(12, "back", says(cd[0], "back"));
	close(cd[0]);

	/* an end carried and never received: let go when the child goes */
	STEP(13, "a third pair", socketpair(AF_UNIX, SOCK_STREAM, 0, ef) == 0);
	STEP(14, "bye", put(sv[0], "bye"));
	{
		struct msghdr	m;
		struct iovec	v;
		union {
			struct cmsghdr	h;
			char		b[CMSG_SPACE(sizeof(int))];
		} c;
		char		one = 'U';

		std::memset(&m, 0, sizeof(m));
		std::memset(&c, 0, sizeof(c));
		v.iov_base = &one;
		v.iov_len = 1;
		m.msg_iov = &v;
		m.msg_iovlen = 1;
		m.msg_control = c.b;
		m.msg_controllen = sizeof(c.b);
		c.h.cmsg_level = SOL_SOCKET;
		c.h.cmsg_type = SCM_RIGHTS;
		c.h.cmsg_len = CMSG_LEN(sizeof(int));
		std::memcpy(CMSG_DATA(&c.h), &ef[1], sizeof(int));
		STEP(15, "send an end it will not take", sendmsg(sv[0], &m, 0) == 1);
		close(ef[1]);
	}

	/* what is left: its end went with it, and the end it never took */
	{
		struct pollfd	p = { sv[0], POLLIN, 0 };
		int		ep = epoll_create1(0), n;
		struct ep_ev	e = { EP_IN, 7 }, got[2];

		STEP(16, "the end hung up", poll(&p, 1, WAIT_MS) == 1 && ( p.revents & POLLHUP ));
		STEP(17, "read finds the end", read(sv[0], b, sizeof(b)) == 0);
		errno = 0;
		STEP(18, "write: EPIPE", write(sv[0], "x", 1) < 0 && errno == EPIPE);
		(void)epoll_ctl(ep, EP_ADD, sv[0], &e);
		n = epoll_wait(ep, got, 2, WAIT_MS);
		STEP(19, "epoll tells the hang-up", n == 1 && ( got[0].events & EP_HUP ));
		close(ep);
		p.fd = ef[0];
		p.revents = 0;
		STEP(20, "the end never taken is let go", poll(&p, 1, WAIT_MS) == 1 && ( p.revents & POLLHUP ));
		close(ef[0]);
	}
	close(sv[0]);
	return failed;
}

/* ---------------------------------------------------------------- round trips timed */

/*
 * "latc" and "latp:<pid>:<n>": n round trips of 1, 64 and 4096 bytes
 * timed one by one, in each of the ways a stream is waited on, and for
 * comparison two threads of one process over a pair, a condition
 * variable and a semaphore of the kernel. Every line printed is
 * "lat <what> <bytes>: mean, median, p99, max" in microseconds.
 */

#define LAT_MAX		10000
#define LAT_SIZES	3

enum { L_BLOCK = 1, L_EPOLL = 2, L_THREAD = 3, L_DONE = 9 };

static const int	lat_size[LAT_SIZES] = { 1, 64, 4096 };
static uint32_t		lat_ns[LAT_MAX];
static uint32_t		lat_sorted[LAT_MAX];
static char		lat_buf[2][4096];

static uint64_t now_ns( void )
{
	struct timespec	t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static int cmp_u32( const void *a, const void *b )
{
	uint32_t	x = *(const uint32_t *)a, y = *(const uint32_t *)b;

	return ( x < y ) ? -1 : ( x > y ) ? 1 : 0;
}

static void lat_report( const char *what, int size, int n )
{
	uint64_t	sum = 0;
	int		i;

	if ( n <= 0 ) {
		std::printf("lat %s %d: none\n", what, size);
		return;
	}
	for ( i = 0; i < n; i++ ) {
		lat_sorted[i] = lat_ns[i];
		sum += lat_ns[i];
	}
	std::qsort(lat_sorted, (size_t)n, sizeof(uint32_t), cmp_u32);
	std::printf("lat %s %d: n %d mean %llu.%01llu median %u.%01u p99 %u.%01u max %u.%01u us\n",
		    what, size, n,
		    (unsigned long long)( sum / (uint64_t)n / 1000 ),
		    (unsigned long long)( sum / (uint64_t)n % 1000 / 100 ),
		    lat_sorted[n / 2] / 1000, lat_sorted[n / 2] % 1000 / 100,
		    lat_sorted[n * 99 / 100] / 1000, lat_sorted[n * 99 / 100] % 1000 / 100,
		    lat_sorted[n - 1] / 1000, lat_sorted[n - 1] % 1000 / 100);
}

/* n bytes read, the descriptor waiting */
static int lat_get_block( int fd, char *b, int n )
{
	int	k = 0, r;

	while ( k < n ) {
		r = (int)read(fd, b + k, (size_t)( n - k ));
		if ( r <= 0 ) return -1;
		k += r;
	}
	return 0;
}

/* n bytes read, an epoll waited on before each read that finds nothing */
static int lat_get_epoll( int ep, int fd, char *b, int n )
{
	struct ep_ev	got[2];
	int		k = 0, r;

	while ( k < n ) {
		if ( epoll_wait(ep, got, 2, WAIT_MS) <= 0 ) return -1;
		for ( ;; ) {
			r = (int)read(fd, b + k, (size_t)( n - k ));
			if ( r > 0 ) {
				k += r;
				if ( k == n ) break;
				continue;
			}
			if ( r < 0 && errno == EAGAIN ) break;
			return -1;
		}
	}
	return 0;
}

static int lat_put( int fd, const char *b, int n )
{
	int	k = 0, r;

	while ( k < n ) {
		r = (int)write(fd, b + k, (size_t)( n - k ));
		if ( r <= 0 ) return -1;
		k += r;
	}
	return 0;
}

static void lat_nb( int fd, int on )
{
	int	f = fcntl(fd, F_GETFL, 0);

	fcntl(fd, F_SETFL, on ? ( f | O_NONBLOCK ) : ( f & ~O_NONBLOCK ));
}

/*
 * The shape of a synchronous call: a thread that waits in epoll reads
 * what came and hands it, by a condition variable, to the thread that
 * sent or answers, which writes.
 */
typedef struct {
	int		fd, ep, size, n, leader;
	pthread_mutex_t	mu;
	pthread_cond_t	cv;
	int		got;		/* messages read and not yet taken */
	int		stop;
} LATIO;

static void *lat_io( void *arg )
{
	LATIO	*s = (LATIO *)arg;
	int	i;

	for ( i = 0; i < s->n; i++ ) {
		if ( lat_get_epoll(s->ep, s->fd, lat_buf[1], s->size) < 0 ) break;
		pthread_mutex_lock(&s->mu);
		s->got++;
		pthread_cond_signal(&s->cv);
		pthread_mutex_unlock(&s->mu);
	}
	pthread_mutex_lock(&s->mu);
	s->stop = 1;
	pthread_cond_signal(&s->cv);
	pthread_mutex_unlock(&s->mu);
	return nullptr;
}

static int lat_take( LATIO *s )
{
	int	ok;

	pthread_mutex_lock(&s->mu);
	while ( s->got == 0 && !s->stop ) pthread_cond_wait(&s->cv, &s->mu);
	ok = ( s->got > 0 );
	if ( ok ) s->got--;
	pthread_mutex_unlock(&s->mu);
	return ok ? 0 : -1;
}

/* n round trips over fd in a way, as the side that starts each (lead) or answers */
static int lat_run( int fd, int how, int size, int n, int lead )
{
	LATIO		s;
	pthread_t	t;
	struct ep_ev	e = { EP_IN, 1 };
	int		ep = -1, i, er = 0;
	uint64_t	t0;

	if ( how != L_BLOCK ) {
		ep = epoll_create1(0);
		(void)epoll_ctl(ep, EP_ADD, fd, &e);
		lat_nb(fd, 1);
	}
	if ( how == L_THREAD ) {
		std::memset(&s, 0, sizeof(s));
		s.fd = fd;
		s.ep = ep;
		s.size = size;
		s.n = n;
		pthread_mutex_init(&s.mu, nullptr);
		pthread_cond_init(&s.cv, nullptr);
		pthread_create(&t, nullptr, lat_io, &s);
	}
	for ( i = 0; i < n && er == 0; i++ ) {
		t0 = now_ns();
		if ( !lead ) {
			er = ( how == L_BLOCK ) ? lat_get_block(fd, lat_buf[0], size)
			   : ( how == L_EPOLL ) ? lat_get_epoll(ep, fd, lat_buf[0], size)
			   : lat_take(&s);
		}
		if ( er == 0 ) er = lat_put(fd, lat_buf[0], size);
		if ( er == 0 && lead ) {
			er = ( how == L_BLOCK ) ? lat_get_block(fd, lat_buf[0], size)
			   : ( how == L_EPOLL ) ? lat_get_epoll(ep, fd, lat_buf[0], size)
			   : lat_take(&s);
		}
		lat_ns[i] = (uint32_t)( now_ns() - t0 );
	}
	if ( how == L_THREAD ) {
		if ( er != 0 ) {
			pthread_mutex_lock(&s.mu);
			s.stop = 1;
			pthread_mutex_unlock(&s.mu);
		}
		pthread_join(t, nullptr);
	}
	if ( ep >= 0 ) {
		close(ep);
		lat_nb(fd, 0);
	}
	return ( er == 0 ) ? i : -1;
}

/* Two threads of one process over a pair, each waiting in read */
static int	lat2_fd, lat2_size, lat2_n;

static void *lat2_echo( void * )
{
	int	i;

	for ( i = 0; i < lat2_n; i++ ) {
		if ( lat_get_block(lat2_fd, lat_buf[1], lat2_size) < 0 ) break;
		if ( lat_put(lat2_fd, lat_buf[1], lat2_size) < 0 ) break;
	}
	return nullptr;
}

/* Two threads by a condition variable, and by two semaphores of the kernel */
static pthread_mutex_t	lc_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	lc_cv = PTHREAD_COND_INITIALIZER;
static int		lc_turn, lc_n;
static ID		ls_a, ls_b;

static void *lat_cv_echo( void * )
{
	int	i;

	pthread_mutex_lock(&lc_mu);
	for ( i = 0; i < lc_n; i++ ) {
		while ( lc_turn != 1 ) pthread_cond_wait(&lc_cv, &lc_mu);
		lc_turn = 0;
		pthread_cond_broadcast(&lc_cv);
	}
	pthread_mutex_unlock(&lc_mu);
	return nullptr;
}

static void *lat_sem_echo( void * )
{
	int	i;

	for ( i = 0; i < lc_n; i++ ) {
		if ( tk_wai_sem(ls_a, 1, WAIT_MS) < E_OK ) break;
		(void)tk_sig_sem(ls_b, 1);
	}
	return nullptr;
}

static void lat_local( int n )
{
	pthread_t	t;
	int		sv[2], i, k;
	uint64_t	t0;
	T_CSEM		c;

	/* the cost of a call of the system that does nothing */
	for ( i = 0; i < n; i++ ) {
		t0 = now_ns();
		(void)tk_get_tid();
		lat_ns[i] = (uint32_t)( now_ns() - t0 );
	}
	lat_report("syscall", 0, n);

	for ( k = 0; k < LAT_SIZES; k++ ) {
		if ( socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ) return;
		lat2_fd = sv[1];
		lat2_size = lat_size[k];
		lat2_n = n;
		pthread_create(&t, nullptr, lat2_echo, nullptr);
		for ( i = 0; i < n; i++ ) {
			t0 = now_ns();
			if ( lat_put(sv[0], lat_buf[0], lat_size[k]) < 0 ) break;
			if ( lat_get_block(sv[0], lat_buf[0], lat_size[k]) < 0 ) break;
			lat_ns[i] = (uint32_t)( now_ns() - t0 );
		}
		pthread_join(t, nullptr);
		close(sv[0]);
		close(sv[1]);
		lat_report("threads-pair", lat_size[k], i);
	}

	lc_n = n;
	lc_turn = 0;
	pthread_create(&t, nullptr, lat_cv_echo, nullptr);
	pthread_mutex_lock(&lc_mu);
	for ( i = 0; i < n; i++ ) {
		t0 = now_ns();
		lc_turn = 1;
		pthread_cond_broadcast(&lc_cv);
		while ( lc_turn != 0 ) pthread_cond_wait(&lc_cv, &lc_mu);
		lat_ns[i] = (uint32_t)( now_ns() - t0 );
	}
	pthread_mutex_unlock(&lc_mu);
	pthread_join(t, nullptr);
	lat_report("threads-cond", 0, n);

	std::memset(&c, 0, sizeof(c));
	c.sematr = TA_TFIFO | TA_FIRST;
	c.isemcnt = 0;
	c.maxsem = 1;
	ls_a = tk_cre_sem(&c);
	ls_b = tk_cre_sem(&c);
	if ( ls_a <= 0 || ls_b <= 0 ) return;
	pthread_create(&t, nullptr, lat_sem_echo, nullptr);
	for ( i = 0; i < n; i++ ) {
		t0 = now_ns();
		(void)tk_sig_sem(ls_a, 1);
		if ( tk_wai_sem(ls_b, 1, WAIT_MS) < E_OK ) break;
		lat_ns[i] = (uint32_t)( now_ns() - t0 );
	}
	pthread_join(t, nullptr);
	(void)tk_del_sem(ls_a);
	(void)tk_del_sem(ls_b);
	lat_report("threads-sem", 0, i);
}

/* What the side that starts tells the other before each run */
typedef struct {
	int	how, size, n;
} LATCTL;

static int lat_child( void )
{
	LATCTL	c;
	int	fd;

	who = "chanprog lat child";
	if ( ts_chan_take(&fd, 1, WAIT_MS) != 1 ) return 1;
	for ( ;; ) {
		if ( lat_get_block(fd, (char *)&c, (int)sizeof(c)) < 0 ) return 2;
		if ( c.how == L_DONE ) break;
		if ( lat_run(fd, c.how, c.size, c.n, 0) != c.n ) return 3;
	}
	close(fd);
	return 0;
}

static int lat_parent( int pid, int n )
{
	static const char	*name[] = { "", "procs-block", "procs-epoll", "procs-thread" };
	LATCTL	c;
	int	sv[2], how, k, r;

	who = "chanprog lat";
	if ( n <= 0 || n > LAT_MAX ) n = 1000;
	lat_local(n);
	if ( socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ) return 1;
	if ( ts_chan_give(pid, &sv[1], 1) != 0 ) return 2;
	close(sv[1]);
	for ( how = L_BLOCK; how <= L_THREAD; how++ ) {
		for ( k = 0; k < LAT_SIZES; k++ ) {
			c.how = how;
			c.size = lat_size[k];
			c.n = n;
			if ( lat_put(sv[0], (const char *)&c, (int)sizeof(c)) < 0 ) return 3;
			r = lat_run(sv[0], how, lat_size[k], n, 1);
			if ( r != n ) {
				std::printf("lat %s %d: FAILED at %d\n", name[how], lat_size[k], r);
				return 4;
			}
			lat_report(name[how], lat_size[k], n);
		}
	}
	c.how = L_DONE;
	(void)lat_put(sv[0], (const char *)&c, (int)sizeof(c));
	close(sv[0]);
	return 0;
}

int main( void )
{
	char	mode[32];
	SZ	n = ts_get_arg(mode, sizeof(mode) - 1);

	mode[( n > 0 ) ? n : 0] = 0;
	if ( std::strcmp(mode, "child") == 0 ) return child();
	if ( std::strncmp(mode, "parent:", 7) == 0 ) return parent(std::atoi(mode + 7));
	if ( std::strcmp(mode, "latc") == 0 ) return lat_child();
	if ( std::strncmp(mode, "latp:", 5) == 0 ) {
		const char	*p = std::strchr(mode + 5, ':');

		return lat_parent(std::atoi(mode + 5), ( p != nullptr ) ? std::atoi(p + 1) : 0);
	}
	std::printf("chanprog: say parent or child\n");
	return 99;
}
