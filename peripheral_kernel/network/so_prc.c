/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_prc.c
 *	The socket calls of a process: SVC class 5 (design 12.6, 9.7).
 *
 *	A socket a process makes is its own. Every call that names one is
 *	refused (E_ID) unless the calling process made it, and what a
 *	process leaves open is closed when it ends (knl_so_prc_end). Every
 *	buffer is checked to lie in the process's own pages before the
 *	stack is given it. The interface itself -- a fixed address, or
 *	asking the network for one -- is set by administrators only.
 *
 *	The structures are the so_ calls' own (include/ts/so.h; include/ts/
 *	soapp.h is the process's side of them), so the calls pass them
 *	through: the stack reads and writes them, and the bytes sent and
 *	received, where the process has them, once they are made sure of,
 *	inside UACCESS (PAN). What is looked at here first is copied in.
 */

#include <tk/tkernel.h>
#include "tstdlib.h"
#include <ts/so.h>
#include <ts/svc.h>
#include <ts/proc.h>
#include "obj/obj.h"
#include "cpu_uaccess.h"

#define ADDR_MAX	128		/* bytes of an address the calls take */
#define OPT_MAX		64		/* bytes of a socket option */
#define NAME_MAX	256		/* bytes of a name to look up, the terminator included */
#define POLL_LOCAL	16		/* sockets of a poll taken in on the stack */
#define PAGE_BYTES	4096		/* a user string is asked about a page at a time */

LOCAL BOOL user_ok( CONST void *p, SZ len, BOOL write )
{
	return knl_prc_user_ok(ts_get_pid(), p, len, write);
}

/* A socket the calling process made */
LOCAL BOOL mine( INT s )
{
	ID	pid = ts_get_pid();

	return ( pid > 0 && knl_so_mine(s, pid) );
}

LOCAL BOOL admin( void )
{
	return knl_ob_is_admin(knl_ob_crd_of(ts_get_pid()));
}

/* An address the process gives: len bytes of it readable */
LOCAL BOOL addr_in( CONST struct sockaddr *a, socklen_t len )
{
	return ( len > 0 && len <= ADDR_MAX && user_ok(a, (SZ)len, FALSE) );
}

/* Room for an address to come back in: *p_len writable, and *p_len bytes at a */
LOCAL BOOL addr_out( struct sockaddr *a, socklen_t *p_len )
{
	socklen_t	len;

	if ( !user_ok(p_len, sizeof(*p_len), TRUE) ) {
		return FALSE;
	}
	len = UACCESS(*p_len);
	return ( len <= ADDR_MAX && ( len == 0 || user_ok(a, (SZ)len, TRUE) ) );
}

/* ---------------------------------------------------------------- waiting in slices */

/*
 * A process's call never waits inside the stack for long. Where it
 * would block, it waits for the socket to be ready a slice at a time
 * (so_poll, which leaves the stack clean when its time runs out) and
 * then does what it came for without waiting. Between slices it looks
 * whether the process is being ended (ts_ter_prc), and if so goes back
 * with E_DISWAI; the task then ends itself on its way out of the SVC
 * (knl_prc_call_leave). A task is never taken down in the middle of the
 * stack, where it would leave the stack's state pointing into it.
 */
#define SLICE_MS	200

/* Whether the socket's calls return at once (O_NONBLOCK, or MSG_DONTWAIT for this one) */
LOCAL BOOL nowait( INT s, INT flags )
{
	ER	fl;

	if ( ( flags & MSG_DONTWAIT ) != 0 ) {
		return TRUE;
	}
	fl = so_fcntl(s, F_GETFL, 0);
	return ( fl >= E_OK && ( fl & O_NONBLOCK ) != 0 );
}

/* The socket's own limit on waiting (SO_RCVTIMEO or SO_SNDTIMEO) in ms, -1 for none */
LOCAL INT limit_of( INT s, INT optname )
{
	struct timeval	tv;
	socklen_t	len = sizeof(tv);
	INT		ms;

	if ( optname == 0 || so_getsockopt(s, SOL_SOCKET, optname, &tv, &len) < E_OK
	  || ( tv.tv_sec == 0 && tv.tv_usec == 0 ) ) {
		return -1;
	}
	ms = (INT)( tv.tv_sec * 1000 + tv.tv_usec / 1000 );
	return ( ms > 0 ) ? ms : 1;
}

/*
 * Until the socket is ready for 'ev', or has something to report, as
 * long as its limit (optname) allows: E_TMOUT when that runs out,
 * E_DISWAI when the process is being ended.
 */
LOCAL ER wait_ready( INT s, INT ev, INT optname )
{
	struct pollfd	p;
	INT		left = limit_of(s, optname), slice, n;

	for ( ;; ) {
		if ( knl_prc_ending() ) {
			return E_DISWAI;
		}
		slice = ( left < 0 || left > SLICE_MS ) ? SLICE_MS : left;
		p.fd = s;
		p.events = (short)ev;
		p.revents = 0;
		n = so_poll(&p, 1, (TMO)slice);
		if ( n < 0 ) {
			return n;
		}
		if ( n > 0 ) {
			return E_OK;
		}
		if ( left >= 0 && ( left -= slice ) <= 0 ) {
			return E_TMOUT;
		}
	}
}

/*
 * Received as a blocking call would, in slices: recv or recvfrom, and
 * with the type of service when p_tos is given (so_recvtos)
 */
LOCAL INT recv_once( INT s, void *mem, SZ size, INT flags,
		     struct sockaddr *from, socklen_t *fromlen, INT *p_tos )
{
	return ( p_tos != NULL ) ? so_recvtos(s, mem, size, flags, from, fromlen, p_tos)
				 : so_recvfrom(s, mem, size, flags, from, fromlen);
}

LOCAL INT recv_sliced( INT s, void *mem, SZ size, INT flags,
		       struct sockaddr *from, socklen_t *fromlen, INT *p_tos )
{
	INT	n;
	ER	er;

	if ( nowait(s, flags) ) {
		return recv_once(s, mem, size, flags, from, fromlen, p_tos);
	}
	for ( ;; ) {
		er = wait_ready(s, POLLIN, SO_RCVTIMEO);
		if ( er < E_OK ) {
			return er;
		}
		n = recv_once(s, mem, size, flags | MSG_DONTWAIT, from, fromlen, p_tos);
		if ( n != E_TMOUT ) {
			return n;		/* data, the end, or an error */
		}
	}
}

/* Sent whole as a blocking call would, in slices: send or sendto */
LOCAL INT send_sliced( INT s, CONST void *data, SZ size, INT flags,
		       CONST struct sockaddr *to, socklen_t tolen )
{
	SZ	done = 0;
	INT	n;
	ER	er;

	if ( nowait(s, flags) ) {
		return so_sendto(s, data, size, flags, to, tolen);
	}
	do {
		er = wait_ready(s, POLLOUT, SO_SNDTIMEO);
		if ( er < E_OK ) {
			return ( done > 0 ) ? (INT)done : er;
		}
		n = so_sendto(s, (CONST UB *)data + done, size - done, flags | MSG_DONTWAIT, to, tolen);
		if ( n == E_TMOUT ) {
			continue;		/* the room went to another */
		}
		if ( n < 0 ) {
			return ( done > 0 ) ? (INT)done : n;
		}
		done += n;
	} while ( done < size );

	return (INT)done;
}

/* The same two for the socket's object (obsock.c): its data record read and written */
EXPORT INT knl_so_recv_wait( INT s, void *mem, SZ size, INT flags )
{
	return recv_sliced(s, mem, size, flags, NULL, NULL, NULL);
}

EXPORT INT knl_so_send_wait( INT s, CONST void *data, SZ size, INT flags )
{
	return send_sliced(s, data, size, flags, NULL, 0);
}

/*
 * A connection made as a blocking call would: begun without waiting,
 * then waited for in slices. The socket's own setting is put back.
 */
LOCAL ER connect_sliced( INT s, CONST struct sockaddr *name, socklen_t namelen )
{
	struct pollfd	p;
	socklen_t	len;
	INT		e = 0;
	ER		fl, er;

	if ( nowait(s, 0) ) {
		return so_connect(s, name, namelen);
	}
	fl = so_fcntl(s, F_GETFL, 0);
	if ( fl < E_OK ) {
		return fl;
	}
	so_fcntl(s, F_SETFL, fl | O_NONBLOCK);
	er = so_connect(s, name, namelen);
	while ( er == E_BUSY ) {		/* in progress */
		if ( knl_prc_ending() ) {
			er = E_DISWAI;
			break;
		}
		p.fd = s;
		p.events = POLLOUT;
		p.revents = 0;
		if ( so_poll(&p, 1, SLICE_MS) != 0 ) {
			len = sizeof(e);
			er = so_getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &len);
			if ( er >= E_OK && e != 0 ) {
				er = knl_so_errcode(e);
			}
		}
	}
	so_fcntl(s, F_SETFL, fl);

	return er;
}

/* A connection taken as a blocking call would, in slices */
LOCAL INT accept_sliced( INT s, struct sockaddr *addr, socklen_t *addrlen )
{
	ER	fl, er;
	INT	n;

	if ( nowait(s, 0) ) {
		return knl_so_accept_as(s, addr, addrlen, ts_get_pid());
	}
	fl = so_fcntl(s, F_GETFL, 0);
	if ( fl < E_OK ) {
		return fl;
	}
	for ( ;; ) {
		er = wait_ready(s, POLLIN, SO_RCVTIMEO);
		if ( er < E_OK ) {
			return er;
		}
		so_fcntl(s, F_SETFL, fl | O_NONBLOCK);
		n = knl_so_accept_as(s, addr, addrlen, ts_get_pid());
		so_fcntl(s, F_SETFL, fl);
		if ( n >= 0 ) {
			/* the connection takes the listening socket's flags, and with
			   them the O_NONBLOCK lent for the call: it blocks as that one does */
			so_fcntl(n, F_SETFL, fl);
		}
		if ( n != E_TMOUT ) {
			return n;
		}
	}
}

LOCAL INT svc_so_socket( INT domain, INT type, INT protocol )
{
	ID	pid = ts_get_pid();

	if ( pid <= 0 ) {
		return E_OBJ;			/* not a process: nobody would own it */
	}
	return knl_so_socket_as(domain, type, protocol, pid);
}

LOCAL ER svc_so_close( INT s )
{
	return mine(s) ? so_close(s) : E_ID;
}

LOCAL ER svc_so_bind( INT s, CONST struct sockaddr *name, socklen_t namelen )
{
	if ( !mine(s) ) return E_ID;
	if ( !addr_in(name, namelen) ) return E_MACV;
	return UACCESS(so_bind(s, name, namelen));
}

LOCAL ER svc_so_connect( INT s, CONST struct sockaddr *name, socklen_t namelen )
{
	if ( !mine(s) ) return E_ID;
	if ( !addr_in(name, namelen) ) return E_MACV;
	return UACCESS(connect_sliced(s, name, namelen));
}

LOCAL ER svc_so_listen( INT s, INT backlog )
{
	return mine(s) ? so_listen(s, backlog) : E_ID;
}

/* The connection that comes is the process's, as the socket it came to is */
LOCAL INT svc_so_accept( INT s, struct sockaddr *addr, socklen_t *addrlen )
{
	if ( !mine(s) ) return E_ID;
	if ( addr != NULL && !addr_out(addr, addrlen) ) return E_MACV;
	return UACCESS(accept_sliced(s, addr, ( addr != NULL ) ? addrlen : NULL));
}

LOCAL ER svc_so_shutdown( INT s, INT how )
{
	return mine(s) ? so_shutdown(s, how) : E_ID;
}

LOCAL INT svc_so_send( INT s, CONST void *data, SZ size, INT flags )
{
	if ( !mine(s) ) return E_ID;
	if ( size < 0 ) return E_PAR;
	if ( size > 0 && !user_ok(data, size, FALSE) ) return E_MACV;
	return UACCESS(send_sliced(s, data, size, flags, NULL, 0));
}

LOCAL INT svc_so_recv( INT s, void *mem, SZ size, INT flags )
{
	if ( !mine(s) ) return E_ID;
	if ( size < 0 ) return E_PAR;
	if ( size > 0 && !user_ok(mem, size, TRUE) ) return E_MACV;
	return UACCESS(recv_sliced(s, mem, size, flags, NULL, NULL, NULL));
}

LOCAL INT svc_so_sendto( INT s, CONST void *data, SZ size, INT flags,
			 CONST struct sockaddr *to, socklen_t tolen )
{
	if ( !mine(s) ) return E_ID;
	if ( size < 0 ) return E_PAR;
	if ( size > 0 && !user_ok(data, size, FALSE) ) return E_MACV;
	if ( to != NULL && !addr_in(to, tolen) ) return E_MACV;
	return UACCESS(send_sliced(s, data, size, flags, to, ( to != NULL ) ? tolen : 0));
}

LOCAL INT svc_so_recvfrom( INT s, void *mem, SZ size, INT flags,
			   struct sockaddr *from, socklen_t *fromlen )
{
	if ( !mine(s) ) return E_ID;
	if ( size < 0 ) return E_PAR;
	if ( size > 0 && !user_ok(mem, size, TRUE) ) return E_MACV;
	if ( from != NULL && !addr_out(from, fromlen) ) return E_MACV;
	return UACCESS(recv_sliced(s, mem, size, flags, from, ( from != NULL ) ? fromlen : NULL, NULL));
}

LOCAL INT svc_so_recvtos( INT s, void *mem, SZ size, INT flags,
			  struct sockaddr *from, socklen_t *fromlen, INT *p_tos )
{
	INT	tos = -1, n;

	if ( !mine(s) ) return E_ID;
	if ( size < 0 ) return E_PAR;
	if ( size > 0 && !user_ok(mem, size, TRUE) ) return E_MACV;
	if ( from != NULL && !addr_out(from, fromlen) ) return E_MACV;
	if ( !user_ok(p_tos, sizeof(*p_tos), TRUE) ) return E_MACV;
	n = UACCESS(recv_sliced(s, mem, size, flags, from, ( from != NULL ) ? fromlen : NULL, &tos));
	if ( n >= 0 ) {
		knl_ua_put(p_tos, &tos, sizeof(tos));
	}
	return n;
}

/*
 * Options go through a buffer of the kernel's. SO_RCVTIMEO and
 * SO_SNDTIMEO take two 64 bit words, seconds and microseconds, as the
 * stack's struct timeval has them.
 */
LOCAL ER svc_so_getsockopt( INT s, INT level, INT optname, void *optval, socklen_t *optlen )
{
	UB		v[OPT_MAX];
	socklen_t	len;
	ER		er;

	if ( !mine(s) ) return E_ID;
	if ( !user_ok(optlen, sizeof(*optlen), TRUE) ) return E_MACV;
	knl_ua_get(&len, optlen, sizeof(len));
	if ( len > OPT_MAX ) len = OPT_MAX;
	if ( len > 0 && !user_ok(optval, (SZ)len, TRUE) ) return E_MACV;
	er = so_getsockopt(s, level, optname, v, &len);
	if ( er >= E_OK ) {
		knl_ua_put(optval, v, len);
		knl_ua_put(optlen, &len, sizeof(len));
	}
	return er;
}

LOCAL ER svc_so_setsockopt( INT s, INT level, INT optname, CONST void *optval, socklen_t optlen )
{
	UB	v[OPT_MAX];

	if ( !mine(s) ) return E_ID;
	if ( optlen > OPT_MAX ) return E_PAR;
	if ( optlen > 0 && !user_ok(optval, (SZ)optlen, FALSE) ) return E_MACV;
	if ( optlen > 0 ) knl_ua_get(v, optval, optlen);
	return so_setsockopt(s, level, optname, v, optlen);
}

LOCAL ER svc_so_getsockname( INT s, struct sockaddr *name, socklen_t *namelen )
{
	if ( !mine(s) ) return E_ID;
	if ( !addr_out(name, namelen) ) return E_MACV;
	return UACCESS(so_getsockname(s, name, namelen));
}

LOCAL ER svc_so_getpeername( INT s, struct sockaddr *name, socklen_t *namelen )
{
	if ( !mine(s) ) return E_ID;
	if ( !addr_out(name, namelen) ) return E_MACV;
	return UACCESS(so_getpeername(s, name, namelen));
}

/* The answers to a list taken into k (every socket in it the process's), put back into fds */
LOCAL INT poll_list( struct pollfd *fds, struct pollfd *k, INT nfds, TMO tmout )
{
	INT	i, n;

	for ( i = 0; i < nfds; i++ ) {
		knl_ua_get(&k[i], &fds[i], sizeof(k[i]));
		if ( !mine(k[i].fd) ) return E_ID;
		k[i].revents = 0;
	}
	/* in slices, looking between them whether the process is being ended */
	do {
		TMO	slice = ( tmout == TMO_FEVR || tmout > SLICE_MS ) ? SLICE_MS : tmout;

		if ( knl_prc_ending() ) {
			return E_DISWAI;
		}
		n = so_poll(k, nfds, ( tmout == TMO_POL ) ? TMO_POL : slice);
		if ( tmout != TMO_FEVR && tmout != TMO_POL ) {
			tmout -= slice;
		}
	} while ( n == 0 && ( tmout == TMO_FEVR || tmout > 0 ) );
	if ( n >= 0 ) {
		for ( i = 0; i < nfds; i++ ) {
			UACCESS_DO(fds[i].revents = k[i].revents);
		}
	}
	return n;
}

/*
 * The list may be as long as the process's limit, as Linux's poll takes
 * up to RLIMIT_NOFILE: a short one is taken in on the stack, a longer one
 * into memory of its own for the call.
 */
LOCAL INT svc_so_poll( struct pollfd *fds, INT nfds, TMO tmout )
{
	struct pollfd	local[POLL_LOCAL], *k = local;
	INT		n;

	if ( nfds < 0 || nfds > knl_prc_so_cur(ts_get_pid()) ) return E_PAR;
	if ( nfds > 0 && !user_ok(fds, (SZ)nfds * (SZ)sizeof(*fds), TRUE) ) return E_MACV;
	if ( nfds > POLL_LOCAL ) {
		k = (struct pollfd *)Kmalloc(sizeof(*k) * (SZ)nfds);
		if ( k == NULL ) return E_NOMEM;
	}
	n = poll_list(fds, k, nfds, tmout);
	if ( k != local ) Kfree(k);
	return n;
}

/*
 * The sockets the process may hold (include/ts/soapp.h). The hard limit
 * goes up only for an administrator, as Linux's only for CAP_SYS_RESOURCE.
 */
LOCAL ER svc_so_getlimit( INT *p_cur, INT *p_max )
{
	INT	cur, max;
	ER	er;

	if ( ( p_cur != NULL && !user_ok(p_cur, sizeof(*p_cur), TRUE) )
	  || ( p_max != NULL && !user_ok(p_max, sizeof(*p_max), TRUE) ) ) return E_MACV;
	er = knl_prc_so_limit(ts_get_pid(), &cur, &max);
	if ( er >= E_OK ) {
		if ( p_cur != NULL ) knl_ua_put(p_cur, &cur, sizeof(cur));
		if ( p_max != NULL ) knl_ua_put(p_max, &max, sizeof(max));
	}
	return er;
}

LOCAL ER svc_so_setlimit( INT cur, INT max )
{
	return knl_prc_so_setlimit(ts_get_pid(), cur, max, admin());
}

/* Only whether a call may block (F_GETFL, F_SETFL with O_NONBLOCK) */
LOCAL ER svc_so_fcntl( INT s, INT cmd, INT val )
{
	if ( !mine(s) ) return E_ID;
	if ( cmd != F_GETFL && cmd != F_SETFL ) return E_PAR;
	return so_fcntl(s, cmd, val);
}

/* A name the process gives, taken into n (NAME_MAX bytes) a page at a time */
LOCAL ER name_in( CONST char *name, char *n )
{
	INT	i;

	if ( name == NULL ) return E_PAR;
	for ( i = 0; i < NAME_MAX; i++ ) {
		if ( ( i == 0 || ( ( (UBINT)( name + i ) ) & ( PAGE_BYTES - 1 ) ) == 0 )
		  && !user_ok(name + i, 1, FALSE) ) {
			return E_MACV;
		}
		n[i] = UACCESS(name[i]);
		if ( n[i] == '\0' ) break;
	}
	return ( i >= NAME_MAX ) ? E_PAR : E_OK;
}

LOCAL ER svc_so_resolve( CONST char *name, UW *p_addr )
{
	char	n[NAME_MAX];
	UW	a = 0;
	ER	er;

	if ( name == NULL ) return E_PAR;
	if ( !user_ok(p_addr, sizeof(*p_addr), TRUE) ) return E_MACV;
	er = name_in(name, n);
	if ( er < E_OK ) return er;
	er = so_resolve(n, &a);
	if ( er >= E_OK ) {
		knl_ua_put(p_addr, &a, sizeof(a));
	}
	return er;
}

/*
 * The addresses go through a list of the kernel's, as many as the
 * program has room for: the list starts small and is made twice as
 * large while it comes back full and the program has more room
 */
#define RESOLVE_FIRST	16

LOCAL INT svc_so_resolveall( CONST char *name, INT family, INT flags, T_SOADDR *addrs, INT max )
{
	T_SOADDR	*l;
	char		n[NAME_MAX];
	INT		k, cap;
	ER		er;

	if ( name == NULL || addrs == NULL || max <= 0 ) return E_PAR;
	if ( !user_ok(addrs, (SZ)max * (SZ)sizeof(T_SOADDR), TRUE) ) return E_MACV;
	er = name_in(name, n);
	if ( er < E_OK ) return er;
	cap = ( max < RESOLVE_FIRST ) ? max : RESOLVE_FIRST;
	for ( ;; ) {
		l = (T_SOADDR *)Kmalloc((size_t)cap * sizeof(T_SOADDR));
		if ( l == NULL ) return E_NOMEM;
		k = so_resolveall(n, family, flags, l, cap);
		if ( k < cap || cap >= max ) break;
		Kfree(l);
		cap = ( cap > max / 2 ) ? max : cap * 2;
	}
	if ( k > 0 ) {
		knl_ua_put(addrs, l, (SZ)k * (SZ)sizeof(T_SOADDR));
	}
	Kfree(l);
	return k;
}

LOCAL BOOL word_out( UW *p )
{
	return ( p == NULL || user_ok(p, sizeof(*p), TRUE) );
}

LOCAL ER svc_so_getifaddr( UW *p_addr, UW *p_mask, UW *p_gw )
{
	if ( !word_out(p_addr) || !word_out(p_mask) || !word_out(p_gw) ) return E_MACV;
	return UACCESS(so_getifaddr(p_addr, p_mask, p_gw));
}

LOCAL ER svc_so_getdns( UW *p_dns )
{
	if ( p_dns == NULL || !user_ok(p_dns, sizeof(*p_dns), TRUE) ) return E_MACV;
	return UACCESS(so_getdns(p_dns));
}

LOCAL ER svc_so_setifaddr( UW addr, UW mask, UW gw, UW dns )
{
	return admin() ? so_setifaddr(addr, mask, gw, dns) : E_OACV;
}

LOCAL ER svc_so_dhcp_start( void )
{
	return admin() ? so_dhcp_start() : E_OACV;
}

LOCAL ER svc_so_getobj( INT s, TS_UUID *p_uuid )
{
	if ( !mine(s) ) return E_ID;
	if ( !user_ok(p_uuid, sizeof(*p_uuid), TRUE) ) return E_MACV;
	return UACCESS(so_getobj(s, p_uuid));
}

/* ---------------------------------------------------------------- the network's configuration changing */

/*
 * A number counted up each time what a program sees of the network's
 * configuration changes: the interface's IPv4 address, mask or gateway,
 * the name servers, IPv6's addresses, router or name servers (knl_so_netchg,
 * called by the stack, DHCP, the router advertisements' task and the
 * settings). A program waits for it to move with so_netwait, as a Linux
 * program reads its netlink socket: the browser drops its connections and
 * the names it holds when it moves (docs/tessronos 9.3.5).
 *
 * A waiter takes a bit of nc_flg of its own and knl_so_netchg sets the
 * bits of every waiter, so that one is let go however close to the change
 * its look at the number was. With more waiters than bits, the ones over
 * look at the number each slice.
 */
#define NC_SLOTS	32

LOCAL volatile UW	nc_gen = 1;
LOCAL volatile UW	nc_used = 0;	/* the bits of nc_flg taken by waiters */
LOCAL volatile ID	nc_flg = 0;

EXPORT void knl_so_netchg( void )
{
	UW	used;

	(void)__atomic_add_fetch(&nc_gen, 1, __ATOMIC_SEQ_CST);
	used = __atomic_load_n(&nc_used, __ATOMIC_SEQ_CST);
	if ( used != 0 && nc_flg > 0 ) {
		(void)tk_set_flg(nc_flg, used);
	}
}

/* The flag the waiters wait on, made by the first of them */
LOCAL ID nc_flag( void )
{
	T_CFLG	cf;
	ID	id, none = 0;

	if ( nc_flg > 0 ) {
		return nc_flg;
	}
	knl_memset(&cf, 0, sizeof(cf));
	cf.flgatr = TA_TFIFO | TA_WMUL;
	id = tk_cre_flg(&cf);
	if ( id <= 0 ) {
		return id;
	}
	if ( !__atomic_compare_exchange_n(&nc_flg, &none, id, FALSE, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ) {
		(void)tk_del_flg(id);
	}
	return nc_flg;
}

/*
 * Until the number differs from *p_gen, or tmo ms (TMO_FEVR: no limit,
 * TMO_POL: only a look) have gone by: E_OK when it differs, E_TMOUT when
 * the time ran out, E_DISWAI when the process is being ended. *p_gen is
 * given the number as it is now in each case.
 */
LOCAL ER svc_so_netwait( UW *p_gen, TMO tmo )
{
	UW	known, now, bit = 0, b, ptn;
	INT	k, left = ( tmo < 0 ) ? -1 : tmo, slice;
	ID	flg;
	ER	er = E_TMOUT;

	if ( p_gen == NULL || !user_ok(p_gen, sizeof(*p_gen), TRUE) ) {
		return E_MACV;
	}
	known = UACCESS(*p_gen);
	flg = ( left != 0 ) ? nc_flag() : 0;
	for ( k = 0; flg > 0 && k < NC_SLOTS; k++ ) {
		b = 1U << k;
		if ( ( __atomic_fetch_or(&nc_used, b, __ATOMIC_SEQ_CST) & b ) == 0 ) {
			bit = b;
			(void)tk_clr_flg(flg, ~bit);	/* a change told before it was this waiter's */
			break;
		}
	}
	for ( ;; ) {
		if ( __atomic_load_n(&nc_gen, __ATOMIC_SEQ_CST) != known ) {
			er = E_OK;
			break;
		}
		if ( left == 0 ) {
			break;
		}
		if ( knl_prc_ending() ) {
			er = E_DISWAI;
			break;
		}
		slice = ( left < 0 || left > SLICE_MS ) ? SLICE_MS : left;
		if ( bit != 0 ) {
			(void)tk_wai_flg(flg, bit, TWF_ORW | TWF_BITCLR, &ptn, (TMO)slice);
		} else {
			(void)tk_dly_tsk((RELTIM)slice);
		}
		if ( left > 0 ) {
			left = ( left > slice ) ? left - slice : 0;
		}
	}
	if ( bit != 0 ) {
		(void)__atomic_fetch_and(&nc_used, ~bit, __ATOMIC_SEQ_CST);
	}
	now = __atomic_load_n(&nc_gen, __ATOMIC_SEQ_CST);
	knl_ua_put(p_gen, &now, sizeof(now));
	return er;
}

#define F(fn)	((FP)(fn))

EXPORT const FP knl_so_svc_tbl[TSN_SO_MAX + 1] = {
	[TSN_NUMBER(TSN_SO_SOCKET)]	= F(svc_so_socket),
	[TSN_NUMBER(TSN_SO_CLOSE)]	= F(svc_so_close),
	[TSN_NUMBER(TSN_SO_BIND)]	= F(svc_so_bind),
	[TSN_NUMBER(TSN_SO_CONNECT)]	= F(svc_so_connect),
	[TSN_NUMBER(TSN_SO_LISTEN)]	= F(svc_so_listen),
	[TSN_NUMBER(TSN_SO_ACCEPT)]	= F(svc_so_accept),
	[TSN_NUMBER(TSN_SO_SHUTDOWN)]	= F(svc_so_shutdown),
	[TSN_NUMBER(TSN_SO_SEND)]	= F(svc_so_send),
	[TSN_NUMBER(TSN_SO_RECV)]	= F(svc_so_recv),
	[TSN_NUMBER(TSN_SO_SENDTO)]	= F(svc_so_sendto),
	[TSN_NUMBER(TSN_SO_RECVFROM)]	= F(svc_so_recvfrom),
	[TSN_NUMBER(TSN_SO_GETSOCKOPT)]	= F(svc_so_getsockopt),
	[TSN_NUMBER(TSN_SO_SETSOCKOPT)]	= F(svc_so_setsockopt),
	[TSN_NUMBER(TSN_SO_GETSOCKNAME)] = F(svc_so_getsockname),
	[TSN_NUMBER(TSN_SO_GETPEERNAME)] = F(svc_so_getpeername),
	[TSN_NUMBER(TSN_SO_POLL)]	= F(svc_so_poll),
	[TSN_NUMBER(TSN_SO_FCNTL)]	= F(svc_so_fcntl),
	[TSN_NUMBER(TSN_SO_RESOLVE)]	= F(svc_so_resolve),
	[TSN_NUMBER(TSN_SO_GETIFADDR)]	= F(svc_so_getifaddr),
	[TSN_NUMBER(TSN_SO_GETDNS)]	= F(svc_so_getdns),
	[TSN_NUMBER(TSN_SO_SETIFADDR)]	= F(svc_so_setifaddr),
	[TSN_NUMBER(TSN_SO_DHCP_START)]	= F(svc_so_dhcp_start),
	[TSN_NUMBER(TSN_SO_GETOBJ)]	= F(svc_so_getobj),
	[TSN_NUMBER(TSN_SO_RESOLVEALL)]	= F(svc_so_resolveall),
	[TSN_NUMBER(TSN_SO_RECVTOS)]	= F(svc_so_recvtos),
	[TSN_NUMBER(TSN_SO_NETWAIT)]	= F(svc_so_netwait),
	[TSN_NUMBER(TSN_SO_GETLIMIT)]	= F(svc_so_getlimit),
	[TSN_NUMBER(TSN_SO_SETLIMIT)]	= F(svc_so_setlimit),
};
