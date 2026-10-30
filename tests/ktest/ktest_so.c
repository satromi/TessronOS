/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_so.c
 *	The socket calls (design 12.6).
 *
 *	Everything here runs over the loopback interface, so it holds on a
 *	machine with nothing plugged in and does not depend on what is at
 *	the other end of the wire. What the card itself does is checked by
 *	ktest_net.c instead. IPv6 is tried the same way, over ::1 and the
 *	card's own link-local address; what needs a network (DHCP, a
 *	router's advertisements, names through DNS, the host over IPv6)
 *	runs on the board and on QEMU's user mode network only.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/so.h>
#include <ts/net.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#ifdef __x86_64__
#include <ts/dt.h>
#endif

#define TEST_PORT	17000
#define LOOPBACK	0x7f000001UL		/* 127.0.0.1 */

LOCAL BOOL	have_stack = FALSE;

/* n bytes the same */
LOCAL BOOL kt_same( CONST void *a, CONST void *b, SZ n )
{
	CONST UB	*p = (CONST UB *)a, *q = (CONST UB *)b;
	SZ		i;

	for ( i = 0; i < n && p[i] == q[i]; i++ ) ;
	return (BOOL)( i == n );
}

LOCAL void addr_set( struct sockaddr_in *sa, UW addr, UH port )
{
	INT	i;
	UB	*p = (UB *)sa;

	for ( i = 0; i < (INT)sizeof(*sa); i++ ) {
		p[i] = 0;
	}
	sa->sin_family = AF_INET;
	sa->sin_port = lwip_htons(port);
	sa->sin_addr.s_addr = lwip_htonl(addr);
}

/* the stack came up and the interface has an address of some sort */
LOCAL void test_stack( void )
{
	UW	addr = 0, mask = 0, gw = 0;

	if ( so_getifaddr(&addr, &mask, &gw) < E_OK ) {
		KT_SKIP("the stack did not start (no card)");
	}
	have_stack = TRUE;

	/* the name of the machine is there and comes back whole */
	{
		char	name[32];

		KT_ASSERT_ER(so_gethostname(name, sizeof(name)), E_OK);
		KT_ASSERT(name[0] != '\0');
		KT_ASSERT_ER(so_sethostname("tf-test", 7), E_OK);
		KT_ASSERT_ER(so_gethostname(name, sizeof(name)), E_OK);
		KT_ASSERT_EQ(name[0], 't');
		KT_ASSERT_EQ(name[6], 't');
		KT_ASSERT_EQ(name[7], '\0');
	}

	/* an argument that makes no sense is refused, not acted on */
	KT_ASSERT_ER(so_gethostname(NULL, 8), E_PAR);
	KT_ASSERT_ER(so_sethostname("x", 0), E_PAR);
}

/* a datagram sent to this machine comes back to it */
LOCAL void test_udp( void )
{
	struct sockaddr_in	me, peer;
	socklen_t		len;
	UB			msg[16], got[32];
	INT			s, n, i;

	if ( !have_stack ) KT_SKIP("no stack");

	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(s >= 0);

	addr_set(&me, LOOPBACK, TEST_PORT);
	KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);

	/* the address it was given reads back the way it went in */
	len = sizeof(peer);
	KT_ASSERT_ER(so_getsockname(s, (struct sockaddr *)&peer, &len), E_OK);
	KT_ASSERT_EQ(lwip_ntohs(peer.sin_port), TEST_PORT);

	for ( i = 0; i < (INT)sizeof(msg); i++ ) {
		msg[i] = (UB)('a' + i);
	}
	n = so_sendto(s, msg, sizeof(msg), 0,
		      (struct sockaddr *)&me, sizeof(me));
	KT_ASSERT_EQ(n, (INT)sizeof(msg));

	len = sizeof(peer);
	n = so_recvfrom(s, got, sizeof(got), 0,
			(struct sockaddr *)&peer, &len);
	KT_ASSERT_EQ(n, (INT)sizeof(msg));
	for ( i = 0; i < (INT)sizeof(msg); i++ ) {
		KT_ASSERT_EQ(got[i], msg[i]);
	}
	/* it came from this machine, on the port it was sent from */
	KT_ASSERT_EQ(lwip_ntohl(peer.sin_addr.s_addr), LOOPBACK);
	KT_ASSERT_EQ(lwip_ntohs(peer.sin_port), TEST_PORT);

	KT_ASSERT_ER(so_close(s), E_OK);
}

/* a connection to this machine is accepted and carries bytes both ways */
LOCAL void test_tcp( void )
{
	struct sockaddr_in	me;
	UB			out[8], in[16];
	INT			srv, cli, acc, n, i;

	if ( !have_stack ) KT_SKIP("no stack");

	srv = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(srv >= 0);

	addr_set(&me, LOOPBACK, TEST_PORT + 1);
	KT_ASSERT_ER(so_bind(srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_listen(srv, 2), E_OK);

	/* the connection is made from this same task, so the listening
	   socket has to be taken up afterwards rather than waited on first */
	cli = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(cli >= 0);
	KT_ASSERT_ER(so_connect(cli, (struct sockaddr *)&me, sizeof(me)), E_OK);

	acc = so_accept(srv, NULL, NULL);
	KT_ASSERT(acc >= 0);

	for ( i = 0; i < (INT)sizeof(out); i++ ) {
		out[i] = (UB)(i + 1);
	}
	n = so_send(cli, out, sizeof(out), 0);
	KT_ASSERT_EQ(n, (INT)sizeof(out));

	n = so_recv(acc, in, sizeof(in), 0);
	KT_ASSERT_EQ(n, (INT)sizeof(out));
	for ( i = 0; i < (INT)sizeof(out); i++ ) {
		KT_ASSERT_EQ(in[i], out[i]);
	}

	/* and the other way, through the read/write names */
	n = so_write(acc, out, 4);
	KT_ASSERT_EQ(n, 4);
	n = so_read(cli, in, sizeof(in));
	KT_ASSERT_EQ(n, 4);

	KT_ASSERT_ER(so_close(acc), E_OK);
	KT_ASSERT_ER(so_close(cli), E_OK);
	KT_ASSERT_ER(so_close(srv), E_OK);
}

/* waiting on several sockets at once */
LOCAL void test_select( void )
{
	struct sockaddr_in	me;
	fd_set			rd;
	UB			msg[4];
	INT			s, n;

	if ( !have_stack ) KT_SKIP("no stack");

	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(s >= 0);
	addr_set(&me, LOOPBACK, TEST_PORT + 2);
	KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);

	/* nothing has arrived, so a look without waiting finds nothing */
	FD_ZERO(&rd);
	FD_SET(s, &rd);
	n = so_select(s + 1, &rd, NULL, NULL, TMO_POL);
	KT_ASSERT_EQ(n, 0);

	msg[0] = 'p'; msg[1] = 'i'; msg[2] = 'n'; msg[3] = 'g';
	KT_ASSERT_EQ(so_sendto(s, msg, sizeof(msg), 0,
			       (struct sockaddr *)&me, sizeof(me)),
		     (INT)sizeof(msg));

	/* now there is something, and it is on that socket */
	FD_ZERO(&rd);
	FD_SET(s, &rd);
	n = so_select(s + 1, &rd, NULL, NULL, 2000);
	KT_ASSERT_EQ(n, 1);
	KT_ASSERT(FD_ISSET(s, &rd));

	KT_ASSERT_EQ(so_recv(s, msg, sizeof(msg), 0), (INT)sizeof(msg));
	KT_ASSERT_ER(so_close(s), E_OK);
}

/* the calls answer with an error rather than acting on nonsense */
LOCAL void test_errors( void )
{
	struct sockaddr_in	me;
	UB			b[4];

	if ( !have_stack ) KT_SKIP("no stack");

	addr_set(&me, LOOPBACK, TEST_PORT + 3);

	/* a descriptor that was never a socket */
	KT_ASSERT(so_close(999) < E_OK);
	KT_ASSERT(so_bind(999, (struct sockaddr *)&me, sizeof(me)) < E_OK);
	KT_ASSERT(so_recv(999, b, sizeof(b), 0) < E_OK);

	/* a kind of socket the stack does not carry */
	KT_ASSERT(so_socket(AF_INET, 99, 0) < E_OK);

	/* a timeout that means nothing */
	{
		fd_set	rd;

		FD_ZERO(&rd);
		KT_ASSERT_ER(so_select(1, &rd, NULL, NULL, -5), E_PAR);
	}
}

/* a datagram sent and taken back through the scatter/gather calls */
LOCAL void test_msg( void )
{
	struct sockaddr_in	me, from;
	struct msghdr		mh;
	struct iovec		iov[2];
	UB			a[4], b[6], got[16];
	INT			s, n, i;

	if ( !have_stack ) KT_SKIP("no stack");

	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(s >= 0);
	addr_set(&me, LOOPBACK, TEST_PORT + 4);
	KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);

	/* two pieces go out as one datagram */
	for ( i = 0; i < 4; i++ ) a[i] = (UB)('A' + i);
	for ( i = 0; i < 6; i++ ) b[i] = (UB)('0' + i);
	iov[0].iov_base = a; iov[0].iov_len = sizeof(a);
	iov[1].iov_base = b; iov[1].iov_len = sizeof(b);

	knl_memset(&mh, 0, sizeof(mh));
	mh.msg_name = &me;
	mh.msg_namelen = sizeof(me);
	mh.msg_iov = iov;
	mh.msg_iovlen = 2;
	n = so_sendmsg(s, &mh, 0);
	KT_ASSERT_EQ(n, (INT)(sizeof(a) + sizeof(b)));

	/* and come back as one piece */
	iov[0].iov_base = got; iov[0].iov_len = sizeof(got);
	knl_memset(&mh, 0, sizeof(mh));
	mh.msg_name = &from;
	mh.msg_namelen = sizeof(from);
	mh.msg_iov = iov;
	mh.msg_iovlen = 1;
	n = so_recvmsg(s, &mh, 0);
	KT_ASSERT_EQ(n, (INT)(sizeof(a) + sizeof(b)));
	for ( i = 0; i < 4; i++ ) {
		KT_ASSERT_EQ(got[i], a[i]);
	}
	for ( i = 0; i < 6; i++ ) {
		KT_ASSERT_EQ(got[4 + i], b[i]);
	}

	KT_ASSERT_ER(so_close(s), E_OK);
}

/* how much has arrived, and whether a call may block */
LOCAL void test_ioctl( void )
{
	struct sockaddr_in	me;
	UB			msg[8], got[8];
	INT			s, n, avail = 0, on = 1;

	if ( !have_stack ) KT_SKIP("no stack");

	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(s >= 0);
	addr_set(&me, LOOPBACK, TEST_PORT + 5);
	KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);

	/* nothing yet */
	KT_ASSERT_ER(so_ioctl(s, FIONREAD, &avail), E_OK);
	KT_ASSERT_EQ(avail, 0);

	for ( n = 0; n < (INT)sizeof(msg); n++ ) msg[n] = (UB)n;
	KT_ASSERT_EQ(so_sendto(s, msg, sizeof(msg), 0,
			       (struct sockaddr *)&me, sizeof(me)),
		     (INT)sizeof(msg));
	tk_dly_tsk(50);
	KT_ASSERT_ER(so_ioctl(s, FIONREAD, &avail), E_OK);
	KT_ASSERT_EQ(avail, (INT)sizeof(msg));
	KT_ASSERT_EQ(so_recv(s, got, sizeof(got), 0), (INT)sizeof(msg));

	/* once it must not block, an empty socket answers at once */
	if ( so_ioctl(s, FIONBIO, &on) == E_OK ) {
		KT_ASSERT(so_recv(s, got, sizeof(got), 0) < E_OK);
	} else {
		KT_ASSERT(0);	/* not asked to read: that would wait */
	}

	/* the same through fcntl */
	n = so_fcntl(s, F_GETFL, 0);
	KT_ASSERT(n >= 0);
	KT_ASSERT_ER(so_fcntl(s, F_SETFL, 0), E_OK);

	KT_ASSERT_ER(so_close(s), E_OK);
}

/* an address from a number, and a number back from an address */
LOCAL void test_names( void )
{
	struct addrinfo		hints, *res = NULL;
	struct sockaddr_in	sa;
	struct sockaddr_in	*out;
	char			host[24], serv[8];
	UINT			idx;
	char			ifname[8];

	if ( !have_stack ) KT_SKIP("no stack");

	knl_memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	KT_ASSERT_ER(so_getaddrinfo("127.0.0.1", "80", &hints, &res), E_OK);
	KT_ASSERT(res != NULL);
	out = (struct sockaddr_in *)res->ai_addr;
	KT_ASSERT_EQ(lwip_ntohl(out->sin_addr.s_addr), LOOPBACK);
	KT_ASSERT_EQ(lwip_ntohs(out->sin_port), 80);
	so_freeaddrinfo(res);

	/* and back the other way */
	addr_set(&sa, LOOPBACK, 8080);
	KT_ASSERT_ER(so_getnameinfo((struct sockaddr *)&sa, sizeof(sa),
				    host, sizeof(host), serv, sizeof(serv), 0),
		     E_OK);
	KT_ASSERT_EQ(host[0], '1');
	KT_ASSERT_EQ(host[1], '2');
	KT_ASSERT_EQ(host[2], '7');
	KT_ASSERT_EQ(host[3], '.');
	KT_ASSERT_EQ(serv[0], '8');
	KT_ASSERT_EQ(serv[3], '0');
	KT_ASSERT_EQ(serv[4], '\0');

	/* a name of the machine cannot be asked for */
	KT_ASSERT_ER(so_getnameinfo((struct sockaddr *)&sa, sizeof(sa),
				    host, sizeof(host), NULL, 0, NI_NAMEREQD),
		     E_NOSPT);

	/* The interfaces the stack has. What they are called depends on
	   what was added, so they are found by number and the name is
	   then mapped back. */
	{
		INT	found = 0;
		UINT	i;

		for ( i = 1; i <= 4; i++ ) {
			if ( so_ifindextoname(i, ifname) != E_OK ) {
				continue;
			}
			found++;
			tm_printf((UB*)"  interface %d is %s\n", (INT)i, ifname);
			idx = so_ifnametoindex(ifname);
			KT_ASSERT_EQ(idx, i);	/* and back again */
		}
		KT_ASSERT(found > 0);
	}
	KT_ASSERT_ER(so_ifindextoname(200, ifname), E_NOEXS);
	KT_ASSERT_EQ(so_ifnametoindex("zz9"), 0);
}

/* what the stack underneath does not carry says so */
LOCAL void test_unsupported( void )
{
	if ( !have_stack ) KT_SKIP("no stack");

	KT_ASSERT_ER(so_sockatmark(0), E_NOSPT);
	KT_ASSERT_ER(so_resctl(0, NULL), E_NOSPT);
	KT_ASSERT_ER(so_break(0), E_NOSPT);
}

#if KT_BOARD || defined(KT_NET_USER)
/*
 * The board on a real network (or QEMU's user mode network): an address
 * from its DHCP server, and a name looked up through the DNS server that
 * came with it. Skipped with no cable in, so as not to hold the book up
 * for the whole wait.
 */
#define DHCP_WAIT_S	20

LOCAL void ip_print( CONST char *what, UW a )
{
	a = lwip_ntohl(a);
	tm_printf((UB *)"  %s %d.%d.%d.%d\n", what, (INT)(a >> 24), (INT)((a >> 16) & 0xff),
		  (INT)((a >> 8) & 0xff), (INT)(a & 0xff));
}

LOCAL void test_dhcp( void )
{
	T_NETSTAT		st;
	UW			addr = 0, mask = 0, gw = 0;
	struct addrinfo		hints, *res = NULL;
	INT			i;

	if ( !have_stack ) KT_SKIP("no stack");
	if ( net_stat(&st) < E_OK || !st.up ) KT_SKIP("no cable in");

	KT_ASSERT(so_dhcp_start() >= E_OK);
	for ( i = 0; i < DHCP_WAIT_S * 10; i++ ) {
		if ( so_getifaddr(&addr, &mask, &gw) >= E_OK && addr != 0 ) break;
		tk_dly_tsk(100);
	}
	KT_ASSERT(addr != 0);
	if ( addr == 0 ) {
		(void)net_stat(&st);
		tm_printf((UB *)"  no answer: %d frames sent, %d received, %d dropped, link %s\n",
			  (INT)st.sent, (INT)st.recv, (INT)st.drop, st.up ? "up" : "down");
		return;
	}
	tm_printf((UB *)"  DHCP answered after %d ms\n", i * 100);
	ip_print("address", addr);
	ip_print("mask   ", mask);
	ip_print("gateway", gw);

	for ( i = 0; i < (INT)sizeof(hints); i++ ) ((UB *)&hints)[i] = 0;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	KT_ASSERT_ER(so_getaddrinfo("www.raspberrypi.com", "80", &hints, &res), E_OK);
	if ( res != NULL ) {
		ip_print("www.raspberrypi.com", ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
		so_freeaddrinfo(res);
	}
}

/*
 * The clock set from a time server on the network (so_sntp.c): within
 * half a minute of the address, and to a time after 2020.
 */
LOCAL void test_sntp( void )
{
	D	ms = 0;
	SYSTIM	t;
	INT	i;

	if ( !have_stack ) KT_SKIP("no stack");
	/* the time server named: the client starts if the build has not
	   started it (CNF_NET_SNTP), or asks at once if it has */
	(void)so_sntp_server(CNF_NTP_SERVER);
	for ( i = 0; i < 300; i++ ) {
		if ( so_sntp_state(&ms) == E_OK ) break;
		tk_dly_tsk(100);
	}
	KT_ASSERT_ER(so_sntp_state(&ms), E_OK);
	KT_ASSERT(ms > 1577836800000LL);			/* after 2020 */
	KT_ASSERT_ER(tk_get_tim(&t), E_OK);
	tm_printf((UB *)"  set to Unix %d s; the clock now says Unix %d s\n", (INT)( ms / 1000 ),
		  (INT)( ( ( ( (D)t.hi << 32 ) | t.lo ) + 473385600000LL ) / 1000 ));
#ifdef __x86_64__
	{
		/* the PC's RTC written as it is kept (9.5.1): the local time, or UTC */
		TS_TIME	now = 0, loc = 0;
		TS_TM	tm;
		D	w = 0, want;
		INT	m = 0;

		KT_ASSERT_ER(dt_getrtc(&m), E_OK);
		KT_ASSERT_ER(knl_rtc_read_wall(&w), E_OK);
		KT_ASSERT_ER(dt_gettime(&now), E_OK);
		KT_ASSERT_ER(dt_localtime(&now, &tm), E_OK);
		KT_ASSERT_ER(dt_mktime(&tm, &loc), E_OK);
		want = ( ( ( m & ~DT_RTC_FIRMWARE ) == DT_RTC_LOCAL ) ? (D)loc : (D)now ) + 473385600LL;
		tm_printf((UB *)"  the RTC reads Unix %d s, kept as %d: %d s from what it should\n",
			  (INT)w, m, (INT)( w - want ));
		if ( ( m & DT_RTC_FIRMWARE ) == 0 ) {
			KT_ASSERT(w - want >= -3 && w - want <= 3);
		}
	}
#endif
}
#endif

/*
 * The settings as text (lib/libconf): a line put and found again, a key
 * of two lines, a value grown and shrunk in place, and the network's
 * lines written and read back the same.
 */
LOCAL void test_conf_text( void )
{
	static UB	t[CF_TEXT_MAX];		/* kept off the test task's stack */
	static T_CFNET	a, b;
	char	v[CF_VAL_MAX];
	INT	n;
	UW	ip = 0;

	n = cf_put(t, 0, sizeof(t), "KEY", 0, "one");
	KT_ASSERT(n > 0);
	KT_ASSERT(cf_get(t, n, "KEY", 0, v, sizeof(v)));
	KT_ASSERT(knl_strcmp(v, "one") == 0);
	n = cf_put(t, n, sizeof(t), "$$DNS", 1, "second");	/* the first made empty on the way */
	KT_ASSERT(cf_get(t, n, "$$DNS", 0, v, sizeof(v)) && v[0] == 0);
	KT_ASSERT(cf_get(t, n, "$$DNS", 1, v, sizeof(v)) && knl_strcmp(v, "second") == 0);
	n = cf_put(t, n, sizeof(t), "KEY", 0, "a longer value than before");
	KT_ASSERT(cf_get(t, n, "KEY", 0, v, sizeof(v)) && knl_strcmp(v, "a longer value than before") == 0);
	KT_ASSERT(cf_get(t, n, "$$DNS", 1, v, sizeof(v)) && knl_strcmp(v, "second") == 0);
	n = cf_put(t, n, sizeof(t), "KEY", 0, "x");
	KT_ASSERT(cf_get(t, n, "KEY", 0, v, sizeof(v)) && knl_strcmp(v, "x") == 0);
	KT_ASSERT(!cf_get(t, n, "KE", 0, v, sizeof(v)));		/* a key is a whole word */
	KT_ASSERT(cf_ip("192.168.1.20", &ip));
	KT_ASSERT_EQ(ip, 0x1401A8C0U);				/* network byte order */
	KT_ASSERT(!cf_ip("192.168.1", &ip) && !cf_ip("1.2.3.256", &ip));
	KT_ASSERT_EQ(cf_num("0x41", 0), 0x41);

	knl_memset(&a, 0, sizeof(a));
	knl_strcpy(a.host, "kt-host");
	(void)cf_ip("10.0.2.15", &a.addr);
	(void)cf_ip("255.255.255.0", &a.mask);
	(void)cf_ip("10.0.2.2", &a.gw);
	a.usegw = TRUE;
	(void)cf_ip("10.0.2.3", &a.dns[0]);
	knl_strcpy(a.domain, "example.test");
	knl_strcpy(a.ntp, "ntp.example.test");
	n = cf_net_write(t, 0, sizeof(t), &a);
	KT_ASSERT(n > 0);
	cf_net_read(t, n, &b);
	KT_ASSERT(knl_strcmp(b.host, "kt-host") == 0 && b.addr == a.addr && b.mask == a.mask);
	KT_ASSERT(b.usegw && b.gw == a.gw && b.dns[0] == a.dns[0] && b.dns[1] == 0);
	KT_ASSERT(knl_strcmp(b.domain, "example.test") == 0 && knl_strcmp(b.ntp, "ntp.example.test") == 0);
	KT_ASSERT(b.dnsname[0][0] == 0);			/* ___1 read back as no name */
}

/*
 * IPv6 in the settings (lib/libconf): addresses read and written in
 * their shortest form, what is not an address refused, and the IPv6
 * lines written after the IPv4 ones and read back the same, the IPv4
 * ones left as they were.
 */
LOCAL void test_conf_text6( void )
{
	static UB	t[CF_TEXT_MAX];
	static T_CFNET	a4, b4;
	static T_CFNET6	a, b;
	static CONST char *const good[][2] = {
		{ "2001:db8::1", "2001:db8::1" },
		{ "2001:0DB8:0000:0000:0000:0000:0000:0001", "2001:db8::1" },
		{ "::", "::" },
		{ "::1", "::1" },
		{ "fe80::", "fe80::" },
		{ "2001:db8:0:1:0:0:0:1", "2001:db8:0:1::1" },	/* the longer run of zeros */
		{ "2001:0:0:1:0:0:0:1", "2001:0:0:1::1" },
		{ "2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1" },	/* the first of two as long */
		{ "1:2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8" },
		{ "1:0:3:4:5:6:7:8", "1:0:3:4:5:6:7:8" },		/* one zero group stays */
		{ "::ffff:10.0.2.15", "::ffff:10.0.2.15" },
		{ "64:ff9b::192.0.2.1", "64:ff9b::c000:201" },
	};
	static CONST char *const bad[] = {
		"", ":", ":::", "1::2::3", "1:2:3:4:5:6:7:8:9", "1:2:3:4:5:6:7", "12345::",
		"2001:db8::g", "1:", ":1", "1.2.3.4", "::1.2.3", "1:2:3:4:5:6:7::8:9",
	};
	char	s[CF_IP6_STRLEN], v[CF_VAL_MAX];
	UB	x[16];
	INT	i, n;

	for ( i = 0; i < (INT)( sizeof(good) / sizeof(good[0]) ); i++ ) {
		KT_ASSERT(cf_ip6(good[i][0], x));
		cf_ip6str(x, s);
		if ( knl_strcmp(s, good[i][1]) != 0 ) tm_printf((UB *)"  %s -> %s\n", good[i][0], s);
		KT_ASSERT(knl_strcmp(s, good[i][1]) == 0);
	}
	for ( i = 0; i < (INT)( sizeof(bad) / sizeof(bad[0]) ); i++ ) {
		if ( cf_ip6(bad[i], x) ) tm_printf((UB *)"  taken: %s\n", bad[i]);
		KT_ASSERT(!cf_ip6(bad[i], x));
	}

	/* nothing said: on and automatic */
	cf_net6_read((CONST UB *)"", 0, &b);
	KT_ASSERT(b.on && b.how == CF_IP6_AUTO && !b.temp && b.plen == 64 && cf_ip6zero(b.addr));

	knl_memset(&a4, 0, sizeof(a4));
	knl_strcpy(a4.host, "kt-host6");
	knl_strcpy(a4.ntp, "pool.ntp.org");
	n = cf_net_write(t, 0, sizeof(t), &a4);
	knl_memset(&a, 0, sizeof(a));
	a.on = TRUE;
	a.how = CF_IP6_FIXED;
	a.temp = TRUE;
	(void)cf_ip6("2001:db8:1::15", a.addr);
	a.plen = 48;
	(void)cf_ip6("fe80::1", a.gw);
	(void)cf_ip6("2001:db8:1::53", a.dns[1]);
	n = cf_net6_write(t, n, sizeof(t), &a);
	KT_ASSERT(n > 0);
	KT_ASSERT(cf_get(t, n, "$$IPV6", 0, v, sizeof(v)) && knl_strcmp(v, "on fixed temp") == 0);
	KT_ASSERT(cf_get(t, n, "$$IPV6ADDR", 0, v, sizeof(v)) && knl_strcmp(v, "2001:db8:1::15/48") == 0);
	KT_ASSERT(cf_get(t, n, "$$IPV6DNS", 0, v, sizeof(v)) && v[0] == 0);
	cf_net6_read(t, n, &b);
	KT_ASSERT(kt_same(&a, &b, sizeof(a)));
	cf_net_read(t, n, &b4);
	KT_ASSERT(knl_strcmp(b4.host, "kt-host6") == 0 && knl_strcmp(b4.ntp, "pool.ntp.org") == 0);

	/* off, and the address taken away: its line is the key alone */
	a.on = FALSE;
	a.how = CF_IP6_NONE;
	a.temp = FALSE;
	knl_memset(a.addr, 0, 16);
	a.plen = 64;
	n = cf_net6_write(t, n, sizeof(t), &a);
	KT_ASSERT(cf_get(t, n, "$$IPV6", 0, v, sizeof(v)) && knl_strcmp(v, "off none") == 0);
	KT_ASSERT(cf_get(t, n, "$$IPV6ADDR", 0, v, sizeof(v)) && v[0] == 0);
	cf_net6_read(t, n, &b);
	KT_ASSERT(kt_same(&a, &b, sizeof(a)));
	KT_ASSERT(!cf_get(t, n, "$$IPV6", 1, v, sizeof(v)));	/* replaced, not added */
}

/*
 * The network's settings object taken (so_conf.c): a fixed address,
 * gateway, name server and machine name written into it and taken are
 * what the interface then has. What was there is put back.
 */
LOCAL void test_conf_apply( void )
{
	UB	*keep, *t;
	TS_UUID	u;
	ID	key;
	SZ	asz = 0, klen = 0;
	UW	a0 = 0, m0 = 0, g0 = 0, d0 = 0, a = 0, m = 0, g = 0, d = 0;
	T_CFNET	n;
	INT	len;
	char	host[32];

	if ( !have_stack ) KT_SKIP("no stack");
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_CONF_NET, &u), E_OK);
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( key < E_OK ) KT_SKIP("no settings object");
	keep = (UB *)Kmalloc(CF_TEXT_MAX);
	t = (UB *)Kmalloc(CF_TEXT_MAX);
	KT_ASSERT(keep != NULL && t != NULL);
	if ( keep == NULL || t == NULL ) {
		ob_cls_obj(key);
		return;
	}
	KT_ASSERT_ER(ob_rea_rec(key, 1, 0, keep, CF_TEXT_MAX, &klen), E_OK);
	(void)so_getifaddr(&a0, &m0, &g0);
	(void)so_getdns(&d0);

	knl_memset(&n, 0, sizeof(n));
	knl_strcpy(n.host, "kt-conf");
	(void)cf_ip("10.0.2.15", &n.addr);
	(void)cf_ip("255.255.255.0", &n.mask);
	(void)cf_ip("10.0.2.2", &n.gw);
	n.usegw = TRUE;
	(void)cf_ip("10.0.2.3", &n.dns[0]);
	knl_strcpy(n.domain, "example.test");
	knl_memcpy(t, keep, (SZ)klen);
	len = cf_net_write(t, (INT)klen, CF_TEXT_MAX, &n);
	KT_ASSERT(len > 0);
	KT_ASSERT_ER(ob_wri_rec(key, 1, 0, t, len, &asz), E_OK);
	KT_ASSERT_ER(ob_trn_rec(key, 1, (UD)len), E_OK);
	ob_cls_obj(key);

	KT_ASSERT_ER(so_conf(SO_CONF_APPLY), E_OK);
	KT_ASSERT_ER(so_getifaddr(&a, &m, &g), E_OK);
	KT_ASSERT_EQ(a, n.addr);
	KT_ASSERT_EQ(m, n.mask);
	KT_ASSERT_EQ(g, n.gw);
	KT_ASSERT_ER(so_getdns(&d), E_OK);
	KT_ASSERT_EQ(d, n.dns[0]);
	KT_ASSERT_ER(so_gethostname(host, sizeof(host)), E_OK);
	KT_ASSERT(knl_strcmp(host, "kt-conf") == 0);
	KT_ASSERT(!so_dhcp_on());
	KT_ASSERT_EQ(so_conf(SO_CONF_BUSY), 0);		/* no process has a socket */

	/* what there was, back */
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(key > 0);
	if ( key > 0 ) {
		KT_ASSERT_ER(ob_wri_rec(key, 1, 0, keep, klen, &asz), E_OK);
		KT_ASSERT_ER(ob_trn_rec(key, 1, (UD)klen), E_OK);
		ob_cls_obj(key);
	}
	(void)so_setdns(0, 0);
	(void)so_setdomain("");
	(void)so_sethostname("tessronos", 9);
	(void)so_setifaddr(a0, m0, g0, d0);
	(void)so_sntp_server(CNF_NTP_SERVER);
	Kfree(keep);
	Kfree(t);
}

/*
 * How fast TCP carries bytes over the loopback interface: 16 MB sent in
 * pieces of 32 KB to a task that takes them. Measured, not judged; it
 * only has to arrive whole.
 */
#define RATE_BYTES	(16 * 1024 * 1024)
#define RATE_CHUNK	(32 * 1024)

LOCAL INT		rate_srv;
LOCAL volatile INT	rate_got;
LOCAL volatile BOOL	rate_done;

LOCAL void rate_sink( INT stacd, void *exinf )
{
	UB	*buf = (UB *)Kmalloc(RATE_CHUNK);
	INT	c, n;

	c = so_accept(rate_srv, NULL, NULL);
	while ( buf != NULL && c >= 0 && ( n = so_recv(c, buf, RATE_CHUNK, 0) ) > 0 ) {
		rate_got += n;
	}
	if ( c >= 0 ) so_close(c);
	if ( buf != NULL ) Kfree(buf);
	rate_done = TRUE;
	tk_exd_tsk();
}

LOCAL void test_tcp_rate( void )
{
	struct sockaddr_in	me;
	T_CTSK			ct;
	UB			*buf;
	UD			t0 = 0, t1 = 0;
	INT			cli, n, sent = 0, i, ms;
	ID			tid;

	if ( !have_stack ) KT_SKIP("no stack");

	buf = (UB *)Kmalloc(RATE_CHUNK);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	for ( i = 0; i < RATE_CHUNK; i++ ) buf[i] = (UB)i;

	rate_srv = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(rate_srv >= 0);
	addr_set(&me, LOOPBACK, TEST_PORT + 6);
	KT_ASSERT_ER(so_bind(rate_srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_listen(rate_srv, 1), E_OK);
	rate_got = 0;
	rate_done = FALSE;
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG;
	ct.task = (FP)rate_sink;
	ct.itskpri = KT_PRI_HIGH;
	ct.stksz = 16 * 1024;
	tid = tk_cre_tsk(&ct);
	KT_ASSERT(tid > 0);
	if ( tid > 0 ) (void)tk_sta_tsk(tid, 0);

	cli = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(cli >= 0);
	KT_ASSERT_ER(so_connect(cli, (struct sockaddr *)&me, sizeof(me)), E_OK);
	(void)ts_get_mono(&t0);
	while ( sent < RATE_BYTES ) {
		n = so_send(cli, buf, RATE_CHUNK, 0);
		if ( n <= 0 ) break;
		sent += n;
	}
	so_close(cli);
	for ( i = 0; i < 600 && !rate_done; i++ ) tk_dly_tsk(100);
	(void)ts_get_mono(&t1);
	ms = (INT)( ( t1 - t0 ) / 1000000ULL );
	tm_printf((UB *)"  TCP over the loopback interface: %d bytes in %d ms, %d KB/s\n",
		  rate_got, ms, ( ms > 0 ) ? (INT)( (D)rate_got * 1000 / 1024 / ms ) : 0);
	KT_ASSERT_EQ(sent, RATE_BYTES);
	KT_ASSERT_EQ(rate_got, RATE_BYTES);
	so_close(rate_srv);
	Kfree(buf);
}

/* How long a call that does next to nothing takes: the cost of entering the stack */
LOCAL void test_call_rate( void )
{
	UD	t0 = 0, t1 = 0;
	INT	s, i, bad = 0;

	if ( !have_stack ) KT_SKIP("no stack");
	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(s >= 0);
	(void)ts_get_mono(&t0);
	for ( i = 0; i < 20000; i++ ) {
		if ( so_fcntl(s, F_GETFL, 0) < E_OK ) bad++;
	}
	(void)ts_get_mono(&t1);
	tm_printf((UB *)"  so_fcntl: %d ns a call\n", (INT)( ( t1 - t0 ) / 20000 ));
	KT_ASSERT_EQ(bad, 0);
	so_close(s);
}

/* ---------------------------------------------------------------- sockets as objects */

/* Whether the text of len bytes has 'want' in it */
LOCAL BOOL text_has( CONST UB *t, SZ len, CONST char *want )
{
	SZ	i, k;

	for ( i = 0; i < len; i++ ) {
		for ( k = 0; want[k] != 0 && i + k < len && t[i + k] == (UB)want[k]; k++ ) ;
		if ( want[k] == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL BOOL listed( UINT sub, CONST TS_UUID *u )
{
	TS_UUID	l[SO_MAX + 16];
	INT	cnt = 0, i;

	if ( ob_lst_obj(OB_T_CHANNEL, sub, NULL, l, SO_MAX + 16, &cnt) < E_OK ) {
		return FALSE;
	}
	for ( i = 0; i < cnt; i++ ) {
		if ( ts_uuid_cmp(&l[i], u) == 0 ) return TRUE;
	}
	return FALSE;
}

/* A port to take notices at: a channel, read without waiting */
LOCAL ID port_make( TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, u) < E_OK ) {
		return E_SYS;
	}
	return ob_opn_obj(u, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
}

/*
 * A datagram socket as an object (OB_S_SOCKET): what it is, who owns it,
 * its metadata and records, data through record 1, and its going told
 * when it is closed.
 */
LOCAL void test_sockobj( void )
{
	struct sockaddr_in	me;
	T_OBREF			r;
	static UD	p_b[KT_PRT_UD];	/* a protection with room for its entries */
	T_OBPRT		*p = (T_OBPRT *)p_b;
	T_OBREC			rec[4];
	T_OBNTF			req;
	T_OBNTM			m;
	TS_UUID			u, x, pu;
	UB			*t, b[16];
	SZ			asz = 0;
	INT			s, c, cnt = 0;
	ID			key, nk, port;

	if ( !have_stack ) KT_SKIP("no stack");
	t = (UB *)Kmalloc(KT_META_ROOM);
	if ( t == NULL ) KT_SKIP("no memory");
	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	c = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(s >= 0 && c >= 0);
	if ( s < 0 || c < 0 ) goto out;

	/* each socket has one, of its own */
	KT_ASSERT_ER(so_getobj(s, &u), E_OK);
	KT_ASSERT_ER(so_getobj(c, &x), E_OK);
	KT_ASSERT(ts_uuid_cmp(&u, &x) != 0);
	KT_ASSERT_ER(so_getobj(-1, &x), E_ID);
	KT_ASSERT_ER(so_getobj(SO_MAX, &x), E_ID);
	KT_ASSERT_ER(so_getobj(s, NULL), E_PAR);

	/* a channel of the subtype socket, the kernel's, rw------- for the system */
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.type, OB_T_CHANNEL);
	KT_ASSERT_EQ(r.sub, OB_S_SOCKET);
	KT_ASSERT_EQ(r.nrec, OB_SK_NREC);
	KT_ASSERT(( r.flags & OB_F_VOLATILE ) != 0);
	KT_ASSERT(text_has(r.name, OB_NAME_MAX, "socket "));
	KT_ASSERT(text_has(r.name, OB_NAME_MAX, "(kernel)"));
	KT_ASSERT_ER(ob_get_prt(&u, p, KT_PRT_SIZE, NULL), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&p->owner, &ob_user_system), 0);
	KT_ASSERT_EQ(p->mode, 0600);
	KT_ASSERT(listed(OB_S_SOCKET, &u));
	KT_ASSERT(listed(0, &u));
	KT_ASSERT(!listed(OB_S_QUEUE, &u));
	KT_ASSERT_ER(ob_set_prt(&u, p), E_NOSPT);		/* fixed */

	/* what the metadata says follows the socket */
	addr_set(&me, LOOPBACK, TEST_PORT + 40);
	KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) goto out;
	KT_ASSERT_ER(ob_get_atr(key, t, KT_META_ROOM, &asz), E_OK);
	KT_ASSERT(text_has(t, asz, "\"socket\":{\"number\":"));
	KT_ASSERT(text_has(t, asz, "\"process\":0"));
	KT_ASSERT(text_has(t, asz, "\"family\":\"inet\""));
	KT_ASSERT(text_has(t, asz, "\"type\":\"datagram\""));
	KT_ASSERT(text_has(t, asz, "\"protocol\":\"udp\""));
	KT_ASSERT(text_has(t, asz, "\"local\":{\"address\":\"127.0.0.1\",\"port\":17040}"));
	KT_ASSERT(text_has(t, asz, "\"remote\":null"));
	KT_ASSERT(text_has(t, asz, "\"state\":\"bound\""));
	KT_ASSERT_ER(ob_set_atr(key, (CONST UB *)"{}", 2), E_NOSPT);

	/* record 0 says what it is; record 1 is the data */
	asz = 0;
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, t, KT_META_ROOM, &asz), E_OK);
	KT_ASSERT(text_has(t, asz, "<tad "));
	KT_ASSERT(text_has(t, asz, "udp, datagram, 127.0.0.1:17040, bound"));
	KT_ASSERT(!text_has(t, asz, "<link"));			/* the kernel's: no process */
	KT_ASSERT_ER(ob_lst_rec(key, rec, 4, &cnt), E_OK);
	KT_ASSERT_EQ(cnt, 2);
	KT_ASSERT_EQ(rec[0].rt, OB_RT_TAD);
	KT_ASSERT_EQ(rec[1].recno, OB_SK_DATA);
	KT_ASSERT_EQ(rec[1].size, 0);

	KT_ASSERT_EQ(so_sendto(c, "obj1", 4, 0, (struct sockaddr *)&me, sizeof(me)), 4);
	tk_dly_tsk(50);
	KT_ASSERT_ER(ob_lst_rec(key, rec, 4, &cnt), E_OK);
	KT_ASSERT(rec[1].size >= 4);				/* what waits to be read */
	asz = 0;
	KT_ASSERT_ER(ob_rea_rec(key, OB_SK_DATA, 0, b, sizeof(b), &asz), E_OK);
	KT_ASSERT_EQ(asz, 4);
	KT_ASSERT(b[0] == 'o' && b[3] == '1');

	/* a key opened not to wait answers at once when nothing is there */
	nk = ob_opn_obj(&u, OB_OP_R | OB_O_NOWAIT);
	KT_ASSERT(nk > 0);
	KT_ASSERT_ER(ob_rea_rec(nk, OB_SK_DATA, 0, b, sizeof(b), &asz), E_TMOUT);
	ob_cls_obj(nk);
	KT_ASSERT_ER(ob_wri_rec(key, 0, 0, "x", 1, &asz), E_RONLY);
	KT_ASSERT_ER(ob_rea_rec(key, OB_SK_DATA, 1, b, sizeof(b), &asz), E_PAR);
	KT_ASSERT_ER(ob_rea_rec(key, 2, 0, b, sizeof(b), &asz), E_NOEXS);

	/* closed: its going is told, and a key kept past it finds nothing */
	port = port_make(&pu);
	KT_ASSERT(port > 0);
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_DELETE;
	KT_ASSERT(ob_ntf_evt(key, OB_REC_ANY, &req, port) > 0);
	KT_ASSERT_ER(so_close(s), E_OK);
	s = -1;
	asz = 0;
	KT_ASSERT_ER(ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz), E_OK);
	KT_ASSERT_EQ(m.event, OB_E_DELETE);
	KT_ASSERT_EQ(ts_uuid_cmp(&m.uuid, &u), 0);
	KT_ASSERT_ER(ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz), E_TMOUT);	/* once */
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_NOEXS);
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, t, KT_META_ROOM, &asz), E_NOEXS);
	KT_ASSERT_ER(ob_opn_obj(&u, OB_OP_R), E_NOEXS);
	KT_ASSERT(!listed(OB_S_SOCKET, &u));
	ob_cls_obj(key);
	ob_cls_obj(port);
	(void)ob_del_obj(&pu);

	/* deleting the object closes the socket */
	KT_ASSERT_ER(so_getobj(c, &x), E_OK);
	KT_ASSERT_ER(ob_del_obj(&x), E_OK);
	KT_ASSERT_ER(so_getobj(c, &x), E_ID);
	KT_ASSERT(so_close(c) < E_OK);
	c = -1;

    out:
	if ( s >= 0 ) so_close(s);
	if ( c >= 0 ) so_close(c);
	Kfree(t);
}

/*
 * A connection's objects: the listening socket's state, the one
 * accepted taking its type and protocol, and data both ways through
 * record 1.
 */
LOCAL void test_sockobj_tcp( void )
{
	struct sockaddr_in	me;
	TS_UUID			us, ua;
	UB			*t, b[16];
	SZ			asz = 0;
	INT			srv, cli, acc = -1;
	ID			ks, ka;

	if ( !have_stack ) KT_SKIP("no stack");
	t = (UB *)Kmalloc(KT_META_ROOM);
	if ( t == NULL ) KT_SKIP("no memory");
	srv = so_socket(AF_INET, SOCK_STREAM, 0);
	cli = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(srv >= 0 && cli >= 0);
	addr_set(&me, LOOPBACK, TEST_PORT + 41);
	KT_ASSERT_ER(so_bind(srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_listen(srv, 1), E_OK);
	KT_ASSERT_ER(so_getobj(srv, &us), E_OK);
	ks = ob_opn_obj(&us, OB_OP_R);
	KT_ASSERT(ks > 0);
	asz = 0;
	KT_ASSERT_ER(ob_get_atr(ks, t, KT_META_ROOM, &asz), E_OK);
	KT_ASSERT(text_has(t, asz, "\"protocol\":\"tcp\""));
	KT_ASSERT(text_has(t, asz, "\"type\":\"stream\""));
	KT_ASSERT(text_has(t, asz, "\"state\":\"listening\""));
	ob_cls_obj(ks);

	KT_ASSERT_ER(so_connect(cli, (struct sockaddr *)&me, sizeof(me)), E_OK);
	acc = so_accept(srv, NULL, NULL);
	KT_ASSERT(acc >= 0);
	if ( acc < 0 ) goto out;
	KT_ASSERT_ER(so_getobj(acc, &ua), E_OK);
	ka = ob_opn_obj(&ua, OB_OP_R | OB_OP_W);
	KT_ASSERT(ka > 0);
	if ( ka <= 0 ) goto out;
	asz = 0;
	KT_ASSERT_ER(ob_get_atr(ka, t, KT_META_ROOM, &asz), E_OK);
	KT_ASSERT(text_has(t, asz, "\"protocol\":\"tcp\""));
	KT_ASSERT(text_has(t, asz, "\"state\":\"connected\""));
	KT_ASSERT(text_has(t, asz, "\"local\":{\"address\":\"127.0.0.1\",\"port\":17041}"));
	KT_ASSERT(text_has(t, asz, "\"remote\":{\"address\":\"127.0.0.1\",\"port\":"));

	/* written to record 1, it is sent; what comes is read from it */
	asz = 0;
	KT_ASSERT_ER(ob_wri_rec(ka, OB_SK_DATA, 0, "PING", 4, &asz), E_OK);
	KT_ASSERT_EQ(asz, 4);
	KT_ASSERT_EQ(so_recv(cli, b, sizeof(b), 0), 4);
	KT_ASSERT(b[0] == 'P' && b[3] == 'G');
	KT_ASSERT_EQ(so_send(cli, "pong", 4, 0), 4);
	asz = 0;
	KT_ASSERT_ER(ob_rea_rec(ka, OB_SK_DATA, 0, b, sizeof(b), &asz), E_OK);
	KT_ASSERT_EQ(asz, 4);
	KT_ASSERT(b[0] == 'p' && b[3] == 'g');

	/* the other end closed: a read gives nothing, as recv does */
	KT_ASSERT_ER(so_close(cli), E_OK);
	cli = -1;
	asz = 99;
	KT_ASSERT_ER(ob_rea_rec(ka, OB_SK_DATA, 0, b, sizeof(b), &asz), E_OK);
	KT_ASSERT_EQ(asz, 0);
	ob_cls_obj(ka);

    out:
	if ( acc >= 0 ) so_close(acc);
	if ( cli >= 0 ) so_close(cli);
	if ( srv >= 0 ) so_close(srv);
	Kfree(t);
}

/*
 * Sockets of a process: owned by it, listed as its own and gone with
 * it when it ends, with a process number no process has
 */
LOCAL void test_sockobj_prc( void )
{
	ID	fake = 9998;
	TS_UUID	u, l[4];
	T_OBREF	r;
	INT	s, n[4];

	if ( !have_stack ) KT_SKIP("no stack");
	s = knl_so_socket_as(AF_INET, SOCK_DGRAM, 0, fake);
	KT_ASSERT(s >= 0);
	if ( s < 0 ) return;
	KT_ASSERT_ER(so_getobj(s, &u), E_OK);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT(text_has(r.name, OB_NAME_MAX, "(process 9998)"));
	KT_ASSERT_EQ(knl_so_obj_list(fake, n, l, 4), 1);
	KT_ASSERT_EQ(n[0], s);
	KT_ASSERT_EQ(ts_uuid_cmp(&l[0], &u), 0);
	KT_ASSERT_EQ(knl_so_obj_list(fake, NULL, NULL, 0), 1);
	knl_so_prc_end(fake);
	KT_ASSERT_EQ(knl_so_obj_list(fake, n, l, 4), 0);
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_NOEXS);
	KT_ASSERT_ER(so_getobj(s, &u), E_ID);
}

/*
 * What the objects cost: a socket made and closed, and the part of it
 * that is the object's (made and taken away again on one number)
 */
LOCAL void test_sockobj_rate( void )
{
	UD	t0 = 0, t1 = 0;
	INT	s, i, bad = 0;

	if ( !have_stack ) KT_SKIP("no stack");
	(void)ts_get_mono(&t0);
	for ( i = 0; i < 1000; i++ ) {
		s = so_socket(AF_INET, SOCK_DGRAM, 0);
		if ( s < 0 || so_close(s) < E_OK ) bad++;
	}
	(void)ts_get_mono(&t1);
	tm_printf((UB *)"  so_socket and so_close: %d ns a pair\n", (INT)( ( t1 - t0 ) / 1000 ));
	KT_ASSERT_EQ(bad, 0);

	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(s >= 0);
	if ( s < 0 ) return;
	(void)ts_get_mono(&t0);
	for ( i = 0; i < 1000; i++ ) {
		knl_so_obj_gone(s);
		knl_so_obj_made(s, 0, AF_INET, SOCK_DGRAM, 0);
	}
	(void)ts_get_mono(&t1);
	tm_printf((UB *)"  the object made and taken away: %d ns\n", (INT)( ( t1 - t0 ) / 1000 ));
	so_close(s);
}

#if TS_NETSTACK_NETBSD
/* ---------------------------------------------------------------- IPv6 */

/*
 * IPv6 over the loopback interface (::1) and the card's link-local
 * address, which holds with nothing on the wire; the names of
 * addresses both ways; and, with a router on the link (QEMU's user mode
 * network, or the board on a network with IPv6), the addresses made
 * from its advertisements, a connection to the host through them and
 * the IPv6 addresses of a name.
 */
#define TEST6_PORT	17400
#define SLAAC_WAIT_S	20

IMPORT void	knl_so_ip6_text( char *buf, SZ len, CONST UB *a );	/* so_api.c */

LOCAL CONST UB	in6_lo[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
LOCAL CONST UB	in6_any[16];

LOCAL void addr6_set( struct sockaddr_in6 *sa, CONST UB *a, UH port, UINT scope )
{
	knl_memset(sa, 0, sizeof(*sa));
	sa->sin6_len = sizeof(*sa);
	sa->sin6_family = AF_INET6;
	sa->sin6_port = so_htons(port);
	knl_memcpy(&sa->sin6_addr, a, 16);
	sa->sin6_scope_id = scope;
}

LOCAL BOOL same16( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; i < 16 && a[i] == b[i]; i++ ) ;
	return ( i == 16 );
}

LOCAL void ip6_print( CONST char *what, CONST UB *a )
{
	char	t[48];

	knl_so_ip6_text(t, sizeof(t), a);
	tm_printf((UB *)"  %s %s\n", what, t);
}

/* a connection and a datagram over ::1, with the addresses read back */
LOCAL void test_ip6_loop( void )
{
	struct sockaddr_in6	me, peer;
	socklen_t		len;
	UB			buf[16];
	INT			srv, cli, acc, u, n;

	if ( !have_stack ) KT_SKIP("no stack");

	srv = so_socket(AF_INET6, SOCK_STREAM, 0);
	KT_ASSERT(srv >= 0);
	addr6_set(&me, in6_lo, TEST6_PORT, 0);
	KT_ASSERT_ER(so_bind(srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_listen(srv, 2), E_OK);
	cli = so_socket(AF_INET6, SOCK_STREAM, 0);
	KT_ASSERT(cli >= 0);
	KT_ASSERT_ER(so_connect(cli, (struct sockaddr *)&me, sizeof(me)), E_OK);
	len = sizeof(peer);
	knl_memset(&peer, 0, sizeof(peer));
	acc = so_accept(srv, (struct sockaddr *)&peer, &len);
	KT_ASSERT(acc >= 0);
	KT_ASSERT_EQ(len, sizeof(peer));
	KT_ASSERT_EQ(peer.sin6_family, AF_INET6);
	KT_ASSERT(same16(peer.sin6_addr.s6_addr, in6_lo));

	/* both ends read back as IPv6 addresses */
	len = sizeof(peer);
	KT_ASSERT_ER(so_getpeername(cli, (struct sockaddr *)&peer, &len), E_OK);
	KT_ASSERT_EQ(peer.sin6_family, AF_INET6);
	KT_ASSERT_EQ(so_ntohs(peer.sin6_port), TEST6_PORT);
	len = sizeof(peer);
	KT_ASSERT_ER(so_getsockname(cli, (struct sockaddr *)&peer, &len), E_OK);
	KT_ASSERT(same16(peer.sin6_addr.s6_addr, in6_lo));
	KT_ASSERT(peer.sin6_port != 0);

	KT_ASSERT_EQ(so_send(cli, "hello6", 6, 0), 6);
	n = so_recv(acc, buf, sizeof(buf), 0);
	KT_ASSERT_EQ(n, 6);
	KT_ASSERT(n == 6 && buf[0] == 'h' && buf[5] == '6');
	so_close(acc);
	so_close(cli);
	so_close(srv);

	/* a datagram to itself */
	u = so_socket(AF_INET6, SOCK_DGRAM, 0);
	KT_ASSERT(u >= 0);
	addr6_set(&me, in6_lo, TEST6_PORT + 1, 0);
	KT_ASSERT_ER(so_bind(u, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_EQ(so_sendto(u, "dgram", 5, 0, (struct sockaddr *)&me, sizeof(me)), 5);
	len = sizeof(peer);
	n = so_recvfrom(u, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &len);
	KT_ASSERT_EQ(n, 5);
	KT_ASSERT_EQ(peer.sin6_family, AF_INET6);
	KT_ASSERT(same16(peer.sin6_addr.s6_addr, in6_lo));
	KT_ASSERT_EQ(so_ntohs(peer.sin6_port), TEST6_PORT + 1);
	so_close(u);
}

/*
 * IPV6_V6ONLY: off, an IPv6 socket bound to :: takes an IPv4 connection,
 * whose address comes as ::ffff:127.0.0.1; on, it takes none
 */
LOCAL void test_ip6_v6only( void )
{
	struct sockaddr_in6	me, peer;
	struct sockaddr_in	to;
	socklen_t		len;
	INT			srv, cli, acc, v, i;

	if ( !have_stack ) KT_SKIP("no stack");

	srv = so_socket(AF_INET6, SOCK_STREAM, 0);
	KT_ASSERT(srv >= 0);
	v = 0;
	KT_ASSERT_ER(so_setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof(v)), E_OK);
	v = 7;
	len = sizeof(v);
	KT_ASSERT_ER(so_getsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &v, &len), E_OK);
	KT_ASSERT_EQ(v, 0);
	addr6_set(&me, in6_any, TEST6_PORT + 2, 0);
	KT_ASSERT_ER(so_bind(srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_listen(srv, 2), E_OK);
	cli = so_socket(AF_INET, SOCK_STREAM, 0);
	addr_set(&to, LOOPBACK, TEST6_PORT + 2);
	KT_ASSERT_ER(so_connect(cli, (struct sockaddr *)&to, sizeof(to)), E_OK);
	len = sizeof(peer);
	knl_memset(&peer, 0, sizeof(peer));
	acc = so_accept(srv, (struct sockaddr *)&peer, &len);
	KT_ASSERT(acc >= 0);
	KT_ASSERT_EQ(peer.sin6_family, AF_INET6);
	for ( i = 0; i < 10; i++ ) KT_ASSERT_EQ(peer.sin6_addr.s6_addr[i], 0);
	KT_ASSERT_EQ(peer.sin6_addr.s6_addr[10], 0xff);
	KT_ASSERT_EQ(peer.sin6_addr.s6_addr[11], 0xff);
	KT_ASSERT_EQ(peer.sin6_addr.s6_addr[12], 127);
	KT_ASSERT_EQ(peer.sin6_addr.s6_addr[15], 1);
	if ( acc >= 0 ) so_close(acc);
	so_close(cli);
	so_close(srv);

	srv = so_socket(AF_INET6, SOCK_STREAM, 0);
	v = 1;
	KT_ASSERT_ER(so_setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &v, sizeof(v)), E_OK);
	addr6_set(&me, in6_any, TEST6_PORT + 3, 0);
	KT_ASSERT_ER(so_bind(srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_listen(srv, 2), E_OK);
	cli = so_socket(AF_INET, SOCK_STREAM, 0);
	addr_set(&to, LOOPBACK, TEST6_PORT + 3);
	KT_ASSERT_ER(so_connect(cli, (struct sockaddr *)&to, sizeof(to)), E_IO);	/* refused */
	so_close(cli);
	so_close(srv);
}

/* An address as so_getnameinfo writes it */
LOCAL BOOL text_is( CONST char *node, INT family, CONST char *want )
{
	struct addrinfo	hints, *res = NULL;
	char		host[64];
	ER		er;

	knl_memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_flags = AI_NUMERICHOST;
	er = so_getaddrinfo(node, "443", &hints, &res);
	if ( er < E_OK || res == NULL ) {
		tm_printf((UB *)"  %s: not taken (%d)\n", node, er);
		return FALSE;
	}
	er = so_getnameinfo(res->ai_addr, res->ai_addrlen, host, sizeof(host), NULL, 0, NI_NUMERICHOST);
	so_freeaddrinfo(res);
	if ( er < E_OK || knl_strcmp(host, want) != 0 ) {
		tm_printf((UB *)"  %s: read back as %s, not %s\n", node, ( er < E_OK ) ? "nothing" : host, want);
		return FALSE;
	}
	return TRUE;
}

LOCAL BOOL refused( CONST char *node, INT family )
{
	struct addrinfo	hints, *res = NULL;
	ER		er;

	knl_memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_flags = AI_NUMERICHOST;
	er = so_getaddrinfo(node, NULL, &hints, &res);
	if ( res != NULL ) so_freeaddrinfo(res);
	return ( er == E_NOEXS && res == NULL );
}

/* numeric IPv6 addresses both ways, and names with addresses of both families */
LOCAL void test_ip6_names( void )
{
	struct addrinfo		hints, *res = NULL, *ai;
	struct sockaddr_in6	*sin6;
	T_SOADDR		l[4];
	UW			a4;
	INT			n;

	if ( !have_stack ) KT_SKIP("no stack");

	knl_memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6;
	hints.ai_socktype = SOCK_STREAM;
	KT_ASSERT_ER(so_getaddrinfo("::1", "80", &hints, &res), E_OK);
	KT_ASSERT(res != NULL && res->ai_next == NULL);
	if ( res != NULL ) {
		sin6 = (struct sockaddr_in6 *)res->ai_addr;
		KT_ASSERT_EQ(res->ai_family, AF_INET6);
		KT_ASSERT_EQ(res->ai_addrlen, sizeof(*sin6));
		KT_ASSERT(same16(sin6->sin6_addr.s6_addr, in6_lo));
		KT_ASSERT_EQ(so_ntohs(sin6->sin6_port), 80);
		so_freeaddrinfo(res);
	}

	/* the shortest form, the longest run of zeros shortened (RFC 5952) */
	KT_ASSERT(text_is("2001:db8::1", AF_INET6, "2001:db8::1"));
	KT_ASSERT(text_is("2001:0DB8:0000:0000:0001:0000:0000:0001", AF_UNSPEC, "2001:db8::1:0:0:1"));
	KT_ASSERT(text_is("::", AF_INET6, "::"));
	KT_ASSERT(text_is("1::", AF_INET6, "1::"));
	KT_ASSERT(text_is("1:0:0:2:0:0:0:3", AF_INET6, "1:0:0:2::3"));
	KT_ASSERT(text_is("::ffff:10.0.2.2", AF_INET6, "::ffff:10.0.2.2"));
	KT_ASSERT(text_is("10.0.2.2", AF_UNSPEC, "10.0.2.2"));
	if ( so_ifnametoindex("neta0") > 0 ) {
		char	z[24];

		KT_ASSERT(text_is("fe80::1%neta0", AF_INET6, "fe80::1%neta0"));
		tm_sprintf((UB *)z, (UB *)"fe80::2%%%d", (INT)so_ifnametoindex("neta0"));
		KT_ASSERT(text_is(z, AF_INET6, "fe80::2%neta0"));
	}
	KT_ASSERT(refused("1:2", AF_INET6));
	KT_ASSERT(refused("1:::2", AF_INET6));
	KT_ASSERT(refused("12345::", AF_INET6));
	KT_ASSERT(refused(":1", AF_INET6));
	KT_ASSERT(refused("1:2:3:4:5:6:7:8:9", AF_INET6));
	KT_ASSERT(refused("::1%zz9", AF_INET6));
	KT_ASSERT(refused("::1", AF_INET));
	KT_ASSERT(refused("127.0.0.1", AF_INET6));

	/* "localhost" is both loopback addresses, ::1 first (RFC 6724: its precedence is the higher) */
	knl_memset(&hints, 0, sizeof(hints));
	KT_ASSERT_ER(so_getaddrinfo("localhost", "7", &hints, &res), E_OK);
	n = 0;
	for ( ai = res; ai != NULL; ai = ai->ai_next ) n++;
	KT_ASSERT_EQ(n, 2);
	if ( n == 2 ) {
		KT_ASSERT_EQ(res->ai_family, AF_INET6);
		KT_ASSERT_EQ(res->ai_next->ai_family, AF_INET);
	}
	so_freeaddrinfo(res);
	n = so_resolveall("localhost", AF_INET6, 0, l, 4);
	KT_ASSERT_EQ(n, 1);
	KT_ASSERT(n == 1 && l[0].family == AF_INET6 && same16(l[0].addr, in6_lo));
	n = so_resolveall("localhost", AF_INET, 0, l, 4);
	KT_ASSERT_EQ(n, 1);
	KT_ASSERT(n == 1 && l[0].family == AF_INET && l[0].addr[0] == 127);
	KT_ASSERT_ER(so_resolve("localhost", &a4), E_OK);
	KT_ASSERT_EQ(so_ntohl(a4), LOOPBACK);

	/* with no name: the loopback addresses, or any (AI_PASSIVE) */
	knl_memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6;
	hints.ai_flags = AI_PASSIVE;
	KT_ASSERT_ER(so_getaddrinfo(NULL, "8080", &hints, &res), E_OK);
	KT_ASSERT(res != NULL && res->ai_family == AF_INET6
		  && same16(((struct sockaddr_in6 *)res->ai_addr)->sin6_addr.s6_addr, in6_any));
	so_freeaddrinfo(res);
	knl_memset(&hints, 0, sizeof(hints));
	KT_ASSERT_ER(so_getaddrinfo(NULL, "8080", &hints, &res), E_OK);
	KT_ASSERT(res != NULL && res->ai_family == AF_INET && res->ai_next != NULL
		  && res->ai_next->ai_family == AF_INET6);
	so_freeaddrinfo(res);
	KT_ASSERT_ER(so_resolveall(NULL, AF_INET6, 0, l, 4), E_PAR);
	KT_ASSERT_ER(so_resolveall("x", 99, 0, l, 4), E_NOSPT);
}

/* The card's link-local address once it is no longer tentative; FALSE when there is none */
LOCAL BOOL link_local( UB *a )
{
	T_SOIF6	l[8];
	INT	n, i, t;

	for ( t = 0; t < 50; t++ ) {
		n = so_getifaddr6(l, 8, NULL);
		for ( i = 0; i < n; i++ ) {
			if ( l[i].addr[0] == 0xfe && ( l[i].addr[1] & 0xc0 ) == 0x80
			  && ( l[i].flags & ( SO_IF6_TENTATIVE | SO_IF6_DUPLICATED ) ) == 0 ) {
				knl_memcpy(a, l[i].addr, 16);
				return TRUE;
			}
		}
		if ( n < 0 ) return FALSE;
		tk_dly_tsk(100);
	}
	return FALSE;
}

/* a connection to the card's own link-local address, which needs its zone */
LOCAL void test_ip6_linklocal( void )
{
	struct sockaddr_in6	me, peer;
	socklen_t		len;
	UB			ll[16], buf[4];
	UINT			idx;
	INT			srv, cli, acc;

	if ( !have_stack ) KT_SKIP("no stack");
	idx = so_ifnametoindex("neta0");
	if ( idx == 0 ) KT_SKIP("no card");
	KT_ASSERT(link_local(ll));
	if ( !link_local(ll) ) return;
	ip6_print("link-local", ll);

	srv = so_socket(AF_INET6, SOCK_STREAM, 0);
	addr6_set(&me, ll, TEST6_PORT + 4, idx);
	KT_ASSERT_ER(so_bind(srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_listen(srv, 2), E_OK);
	cli = so_socket(AF_INET6, SOCK_STREAM, 0);
	KT_ASSERT_ER(so_connect(cli, (struct sockaddr *)&me, sizeof(me)), E_OK);
	len = sizeof(peer);
	acc = so_accept(srv, (struct sockaddr *)&peer, &len);
	KT_ASSERT(acc >= 0);
	KT_ASSERT(same16(peer.sin6_addr.s6_addr, ll));
	KT_ASSERT_EQ(peer.sin6_scope_id, idx);
	KT_ASSERT_EQ(so_send(cli, "ll", 2, 0), 2);
	KT_ASSERT_EQ(so_recv(acc, buf, sizeof(buf), 0), 2);
	if ( acc >= 0 ) so_close(acc);
	so_close(cli);
	so_close(srv);
}

#if KT_BOARD || defined(KT_NET_USER)
/* An address made from a router's prefix, once checked; FALSE when none came */
LOCAL BOOL slaac_addr( UB *a, INT wait_s )
{
	T_SOIF6	l[8];
	INT	n, i, t;

	for ( t = 0; t < wait_s * 10; t++ ) {
		n = so_getifaddr6(l, 8, NULL);
		for ( i = 0; i < n; i++ ) {
			if ( ( l[i].flags & SO_IF6_AUTOCONF ) && !( l[i].flags & ( SO_IF6_TENTATIVE | SO_IF6_DUPLICATED ) ) ) {
				knl_memcpy(a, l[i].addr, 16);
				return TRUE;
			}
		}
		tk_dly_tsk(100);
	}
	return FALSE;
}

/*
 * An address from the router's advertisement (SLAAC), the default route
 * through the router and its name server (RDNSS). QEMU's user mode
 * network advertises fec0::/64 from fe80::2, with fec0::3 as the name
 * server.
 */
LOCAL void test_slaac( void )
{
	T_SOIF6	l[8];
	UB	a[16], gw[16], ns[16];
	INT	n, i;

	if ( !have_stack ) KT_SKIP("no stack");
	if ( !slaac_addr(a, SLAAC_WAIT_S) ) {
#ifdef KT_NET_USER
		KT_ASSERT(FALSE);
		return;
#else
		KT_SKIP("no IPv6 router on the link");
#endif
	}
	n = so_getifaddr6(l, 8, gw);
	for ( i = 0; i < n; i++ ) {
		char	t[48];

		knl_so_ip6_text(t, sizeof(t), l[i].addr);
		tm_printf((UB *)"  %s/%d flags 0x%x valid %d s preferred %d s\n", t, l[i].plen, l[i].flags,
			  (INT)l[i].vltime, (INT)l[i].pltime);
	}
	ip6_print("default router", gw);
	KT_ASSERT(gw[0] == 0xfe && ( gw[1] & 0xc0 ) == 0x80);
	if ( so_getdns6(0, ns) == E_OK ) {
		ip6_print("name server", ns);
	}
#ifdef KT_NET_USER
	{
		static CONST UB	prefix[8] = { 0xfe, 0xc0, 0, 0, 0, 0, 0, 0 };
		static CONST UB	router[16] = { 0xfe, 0x80, 0,0,0,0,0,0, 0,0,0,0,0,0,0, 2 };
		static CONST UB	server[16] = { 0xfe, 0xc0, 0,0,0,0,0,0, 0,0,0,0,0,0,0, 3 };

		for ( i = 0; i < 8; i++ ) KT_ASSERT_EQ(a[i], prefix[i]);
		KT_ASSERT(same16(gw, router));
		KT_ASSERT_ER(so_getdns6(0, ns), E_OK);
		KT_ASSERT(same16(ns, server));
	}
#endif
	KT_ASSERT_ER(so_getdns6(-1, ns), E_PAR);
}

#if TS_NETSTACK_NETBSD
IMPORT void	knl_so_slaac_relink( void );	/* so_slaac.c */
IMPORT UINT	knl_so_slaac_solicits( void );

/*
 * The link come up again (netadp.c tells it when the Wi-Fi joined a
 * network, a cable came back or the card changed): the routers are asked
 * again at once, three times, and the address made from their prefix
 * stays
 */
LOCAL void test_slaac_relink( void )
{
	UB	a[16], b[16];
	UINT	n0, n1;
	INT	t;

	if ( !have_stack ) KT_SKIP("no stack");
	if ( !slaac_addr(a, 1) ) KT_SKIP("no IPv6 address made");
	n0 = knl_so_slaac_solicits();
	knl_so_slaac_relink();
	for ( t = 0; t < 30 && knl_so_slaac_solicits() == n0; t++ ) tk_dly_tsk(100);
	n1 = knl_so_slaac_solicits();
	tm_printf((UB *)"  router solicitations: %u before, %u within %d00 ms of the new link\n", n0, n1, t);
	KT_ASSERT(n1 == n0 + 1);
	for ( t = 0; t < 100 && knl_so_slaac_solicits() < n0 + 3; t++ ) tk_dly_tsk(100);
	KT_ASSERT_EQ(knl_so_slaac_solicits(), n0 + 3);	/* 4 s apart */
	KT_ASSERT(slaac_addr(b, 1) && same16(a, b));
}
#endif

/*
 * The IPv6 addresses of a name (AAAA). Where the machine's only IPv6
 * address is of a smaller scope than the world's (QEMU's site-local
 * fec0::/64), the IPv4 addresses of the name are to be tried first, and
 * AI_ADDRCONFIG leaves the IPv6 ones out. With a global IPv6 address
 * (the board on a network with IPv6) a connection is made to the IPv6
 * address of the name.
 */
#define AAAA_NAME	"www.google.com"

LOCAL void test_aaaa( void )
{
	struct addrinfo	hints, *res = NULL, *ai;
	T_SOADDR	l[16];
	UB		me[16];
	INT		n, i, v4 = 0, v6 = 0;
	BOOL		global;

	if ( !have_stack ) KT_SKIP("no stack");
	if ( !slaac_addr(me, 1) ) KT_SKIP("no IPv6 address made");
	global = !( me[0] == 0xfe && ( me[1] & 0xc0 ) == 0xc0 );

	n = so_resolveall(AAAA_NAME, AF_INET6, 0, l, 16);
	tm_printf((UB *)"  %s: %d IPv6 addresses\n", AAAA_NAME, n);
	KT_ASSERT(n >= 1);
	for ( i = 0; i < n; i++ ) KT_ASSERT_EQ(l[i].family, AF_INET6);
	if ( n >= 1 ) ip6_print("first", l[0].addr);

	n = so_resolveall(AAAA_NAME, AF_UNSPEC, 0, l, 16);
	for ( i = 0; i < n; i++ ) {
		if ( l[i].family == AF_INET ) v4++;
		if ( l[i].family == AF_INET6 ) v6++;
	}
	tm_printf((UB *)"  both: %d IPv4 and %d IPv6, the first %s\n", v4, v6,
		  ( n > 0 && l[0].family == AF_INET6 ) ? "IPv6" : "IPv4");
	KT_ASSERT(v4 >= 1 && v6 >= 1);
	KT_ASSERT(n > 0 && l[0].family == ( global ? AF_INET6 : AF_INET ));

	knl_memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG;
	KT_ASSERT_ER(so_getaddrinfo(AAAA_NAME, "80", &hints, &res), E_OK);
	v4 = v6 = 0;
	for ( ai = res; ai != NULL; ai = ai->ai_next ) {
		if ( ai->ai_family == AF_INET ) v4++;
		if ( ai->ai_family == AF_INET6 ) {
			v6++;
			KT_ASSERT_EQ(so_ntohs(((struct sockaddr_in6 *)ai->ai_addr)->sin6_port), 80);
		}
	}
	tm_printf((UB *)"  AI_ADDRCONFIG: %d IPv4 and %d IPv6\n", v4, v6);
	KT_ASSERT(v4 >= 1);
	KT_ASSERT(global ? v6 >= 1 : v6 == 0);

	/* with an address to reach the world with, a connection to the name's IPv6 address */
	if ( global ) {
		for ( ai = res; ai != NULL && ai->ai_family != AF_INET6; ai = ai->ai_next ) ;
		if ( ai != NULL ) {
			INT	s = so_socket(AF_INET6, SOCK_STREAM, 0);
			ER	er = so_connect(s, ai->ai_addr, ai->ai_addrlen);

			tm_printf((UB *)"  connected over IPv6: %d\n", er);
			KT_ASSERT_ER(er, E_OK);
			so_close(s);
		}
	}
	so_freeaddrinfo(res);
}
#endif

#ifdef KT_ECHO6_PORT
/*
 * A connection to the host over IPv6: QEMU's user mode network takes
 * fec0::2 to the host's ::1, where the test's server (tools/ftpd.py
 * --echo6) sends back what it is sent.
 */
LOCAL void test_ip6_host( void )
{
	static CONST UB		host[16] = { 0xfe, 0xc0, 0,0,0,0,0,0, 0,0,0,0,0,0,0, 2 };
	struct sockaddr_in6	to, me;
	socklen_t		len;
	UB			out[200], in[200], a[16];
	INT			s, n, got, i;
	ER			er;

	if ( !have_stack ) KT_SKIP("no stack");
	if ( !slaac_addr(a, 1) ) KT_SKIP("no IPv6 address made");

	s = so_socket(AF_INET6, SOCK_STREAM, 0);
	KT_ASSERT(s >= 0);
	addr6_set(&to, host, KT_ECHO6_PORT, 0);
	er = so_connect(s, (struct sockaddr *)&to, sizeof(to));
	KT_ASSERT_ER(er, E_OK);
	if ( er < E_OK ) {
		so_close(s);
		return;
	}
	len = sizeof(me);
	KT_ASSERT_ER(so_getsockname(s, (struct sockaddr *)&me, &len), E_OK);
	ip6_print("from", me.sin6_addr.s6_addr);
	KT_ASSERT(same16(me.sin6_addr.s6_addr, a));
	for ( i = 0; i < (INT)sizeof(out); i++ ) out[i] = (UB)( i * 7 + 1 );
	KT_ASSERT_EQ(so_send(s, out, sizeof(out), 0), (INT)sizeof(out));
	for ( got = 0; got < (INT)sizeof(in); got += n ) {
		n = so_recv(s, in + got, sizeof(in) - got, 0);
		if ( n <= 0 ) break;
	}
	KT_ASSERT_EQ(got, (INT)sizeof(in));
	for ( i = 0; i < got; i++ ) {
		if ( in[i] != out[i] ) break;
	}
	KT_ASSERT_EQ(i, (INT)sizeof(out));
	so_close(s);
}
#endif

#ifdef KT_NET_USER
/*
 * IPv6 as the network's settings say (so_conf.c, so_setip6): the
 * settings' record written and taken, as the accessory does, and what
 * the interface then has read back both through the calls and from the
 * system object's record 7 (what the accessory shows). On QEMU's user
 * mode network: a fixed address in the advertised prefix with the
 * router fe80::2, then addresses from the routers again with temporary
 * ones, without them, the link-local address only, and IPv6 off.
 */
#define CONF6_WAIT_S	20

LOCAL UB	*c6_keep;
LOCAL SZ	c6_klen;

/* The settings' record as it was, with these IPv6 lines, written and taken */
LOCAL ER conf6_take( CONST T_CFNET6 *n6 )
{
	static UB	t[CF_TEXT_MAX];
	TS_UUID		u;
	ID		key;
	SZ		asz = 0;
	INT		len;
	ER		er;

	knl_memcpy(t, c6_keep, c6_klen);
	len = ( n6 != NULL ) ? cf_net6_write(t, (INT)c6_klen, CF_TEXT_MAX, n6) : (INT)c6_klen;
	if ( len < 0 || ts_str_to_uuid(SYSDEF_CONF_NET, &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	if ( key < E_OK ) return (ER)key;
	er = ob_wri_rec(key, 1, 0, t, len, &asz);
	if ( er >= E_OK ) er = ob_trn_rec(key, 1, (UD)len);
	ob_cls_obj(key);
	if ( er < E_OK ) return er;
	return so_conf(SO_CONF_APPLY);
}

/*
 * An address of the interface other than the link-local one with all
 * of the flags 'want' and none of 'not', no longer tentative, within
 * wait_ms; FALSE when none came.
 */
LOCAL BOOL addr6_with( UINT want, UINT not, UB *a, INT wait_ms )
{
	T_SOIF6	l[16];
	INT	n, i, t;

	for ( t = 0; ; t += 100 ) {
		n = so_getifaddr6(l, 16, NULL);
		for ( i = 0; i < n; i++ ) {
			if ( l[i].addr[0] == 0xfe && ( l[i].addr[1] & 0xc0 ) == 0x80 ) continue;
			if ( ( l[i].flags & want ) != want || ( l[i].flags & ( not | SO_IF6_TENTATIVE ) ) ) continue;
			if ( a != NULL ) knl_memcpy(a, l[i].addr, 16);
			return TRUE;
		}
		if ( t >= wait_ms ) return FALSE;
		tk_dly_tsk(100);
	}
}

/* Whether the interface has this address, and its flags */
LOCAL BOOL has6( CONST UB *a, UINT *p_flags, INT *p_plen )
{
	T_SOIF6	l[16];
	INT	n, i;

	n = so_getifaddr6(l, 16, NULL);
	for ( i = 0; i < n; i++ ) {
		if ( same16(l[i].addr, a) ) {
			if ( p_flags != NULL ) *p_flags = l[i].flags;
			if ( p_plen != NULL ) *p_plen = l[i].plen;
			return TRUE;
		}
	}
	return FALSE;
}

/* The address a connection to the host (fec0::2) would come from; FALSE when there is none to send from */
LOCAL BOOL source6( UB *a )
{
	static CONST UB		host[16] = { 0xfe, 0xc0, 0,0,0,0,0,0, 0,0,0,0,0,0,0, 2 };
	struct sockaddr_in6	to, me;
	socklen_t		len = sizeof(me);
	INT			s;
	BOOL			ok;

	s = so_socket(AF_INET6, SOCK_DGRAM, 0);
	if ( s < 0 ) return FALSE;
	addr6_set(&to, host, 9, 0);
	ok = ( so_connect(s, (struct sockaddr *)&to, sizeof(to)) == E_OK
	    && so_getsockname(s, (struct sockaddr *)&me, &len) == E_OK );
	if ( ok ) knl_memcpy(a, me.sin6_addr.s6_addr, 16);
	so_close(s);
	return ok;
}

/* The system object's record 7, the network as it is now: its length */
LOCAL INT sys_net( UB *buf, INT max )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;

	if ( ts_str_to_uuid(SYSDEF_SYSTEM, &u) < E_OK ) return 0;
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key < E_OK ) return 0;
	if ( ob_rea_rec(key, OB_SYS_NET, 0, buf, max, &asz) < E_OK ) asz = 0;
	ob_cls_obj(key);
	return (INT)( ( asz > (SZ)max ) ? (SZ)max : asz );
}

/* A line of the record 7 with this key whose value starts with 'start' */
LOCAL BOOL sys_line( CONST UB *t, INT len, CONST char *key, CONST char *start )
{
	char	v[CF_VAL_MAX];
	INT	i, k = (INT)knl_strlen(start);

	for ( i = 0; cf_get(t, len, key, i, v, sizeof(v)); i++ ) {
		if ( kt_same(v, start, k) ) return TRUE;
	}
	return FALSE;
}

LOCAL BOOL sys_word( CONST UB *t, INT len, CONST char *key, CONST char *word )
{
	char	v[CF_VAL_MAX], w[64];
	INT	i, j;

	for ( i = 0; cf_get(t, len, key, i, v, sizeof(v)); i++ ) {
		for ( j = 0; cf_word(v, j, w, sizeof(w)); j++ ) {
			if ( knl_strcmp(w, word) == 0 ) return TRUE;
		}
	}
	return FALSE;
}

LOCAL void sys_print( CONST UB *t, INT len )
{
	char	v[CF_VAL_MAX];
	INT	i;

	for ( i = 0; cf_get(t, len, "ADDRESS6", i, v, sizeof(v)); i++ ) tm_printf((UB *)"  ADDRESS6 %s\n", v);
	if ( cf_get(t, len, "GATEWAY6", 0, v, sizeof(v)) ) tm_printf((UB *)"  GATEWAY6 %s\n", v);
	for ( i = 0; cf_get(t, len, "DNS6", i, v, sizeof(v)); i++ ) tm_printf((UB *)"  DNS6 %s\n", v);
}

LOCAL void test_ip6_conf( void )
{
	static UB	rec[4096];
	static T_CFNET6	n6;
	static CONST UB	router[16] = { 0xfe, 0x80, 0,0,0,0,0,0, 0,0,0,0,0,0,0, 2 };
	TS_UUID		u;
	ID		key;
	UB		fixed[16], ns[16], stable[16], temp[16], src[16], gw[16], ll[16], x[16];
	UINT		fl = 0;
	INT		len, plen = 0, i;
	char		s[CF_IP6_STRLEN];

	if ( !have_stack ) KT_SKIP("no stack");
	if ( !slaac_addr(stable, CONF6_WAIT_S) ) KT_SKIP("no IPv6 address made");
	KT_ASSERT_EQ(so_getip6(), SO_IP6_AUTO);
	KT_ASSERT(link_local(ll));
	KT_ASSERT_ER(ts_str_to_uuid(SYSDEF_CONF_NET, &u), E_OK);
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key < E_OK ) KT_SKIP("no settings object");
	c6_keep = (UB *)Kmalloc(CF_TEXT_MAX);
	KT_ASSERT(c6_keep != NULL);
	if ( c6_keep == NULL ) {
		ob_cls_obj(key);
		return;
	}
	KT_ASSERT_ER(ob_rea_rec(key, 1, 0, c6_keep, CF_TEXT_MAX, &c6_klen), E_OK);
	ob_cls_obj(key);

	/* a fixed address and router, and a name server of the settings' own */
	knl_memset(&n6, 0, sizeof(n6));
	n6.on = TRUE;
	n6.how = CF_IP6_FIXED;
	(void)cf_ip6("fec0::15", fixed);
	knl_memcpy(n6.addr, fixed, 16);
	n6.plen = 64;
	knl_memcpy(n6.gw, router, 16);
	(void)cf_ip6("fec0::53", n6.dns[0]);
	KT_ASSERT_ER(conf6_take(&n6), E_OK);
	KT_ASSERT_EQ(so_getip6(), SO_IP6_FIXED);
	KT_ASSERT(has6(fixed, &fl, &plen));
	KT_ASSERT_EQ(plen, 64);
	KT_ASSERT(( fl & ( SO_IF6_AUTOCONF | SO_IF6_TEMPORARY ) ) == 0);
	KT_ASSERT(!has6(stable, NULL, NULL));			/* the router's address went */
	KT_ASSERT(!addr6_with(SO_IF6_AUTOCONF, 0, NULL, 0));
	KT_ASSERT(addr6_with(0, 0, x, 3000) && same16(x, fixed));
	KT_ASSERT(so_getifaddr6(NULL, 0, gw) == 0 && same16(gw, router));
	KT_ASSERT_ER(so_getdns6(0, ns), E_OK);
	KT_ASSERT(same16(ns, n6.dns[0]));
	KT_ASSERT_ER(so_getdns6(1, ns), E_NOEXS);		/* the router's (RDNSS) forgotten */
	KT_ASSERT(source6(src) && same16(src, fixed));
	len = sys_net(rec, sizeof(rec));
	sys_print(rec, len);
	KT_ASSERT(sys_line(rec, len, "IPV6", "fixed"));
	KT_ASSERT(sys_line(rec, len, "ADDRESS6", "fec0::15/64 fixed infinite infinite"));
	KT_ASSERT(sys_line(rec, len, "ADDRESS6", "fe80::"));
	KT_ASSERT(sys_line(rec, len, "GATEWAY6", "fe80::2"));
	KT_ASSERT(sys_line(rec, len, "DNS6", "fec0::53"));
	KT_ASSERT_ER(conf6_take(&n6), E_OK);			/* the same again: nothing changes */
	KT_ASSERT(has6(fixed, NULL, NULL));

	/* from the routers again, with temporary addresses */
	n6.how = CF_IP6_AUTO;
	n6.temp = TRUE;
	knl_memset(n6.dns[0], 0, 16);
	KT_ASSERT_ER(conf6_take(&n6), E_OK);
	KT_ASSERT_EQ(so_getip6(), SO_IP6_AUTO | SO_IP6_TEMP);
	KT_ASSERT(!has6(fixed, NULL, NULL));
	KT_ASSERT(addr6_with(SO_IF6_AUTOCONF, SO_IF6_TEMPORARY, x, CONF6_WAIT_S * 1000));
	KT_ASSERT(same16(x, stable));				/* the same stable address as before */
	KT_ASSERT(addr6_with(SO_IF6_AUTOCONF | SO_IF6_TEMPORARY, 0, temp, CONF6_WAIT_S * 1000));
	cf_ip6str(temp, s);
	tm_printf((UB *)"  temporary %s\n", s);
	KT_ASSERT(kt_same(temp, stable, 8) && !kt_same(temp + 8, stable + 8, 8));
	KT_ASSERT(source6(src) && same16(src, temp));		/* chosen before the stable one */
	KT_ASSERT_ER(so_getdns6(0, ns), E_OK);			/* the router's name server again */
	KT_ASSERT(so_getifaddr6(NULL, 0, gw) == 0 && same16(gw, router));
	len = sys_net(rec, sizeof(rec));
	sys_print(rec, len);
	KT_ASSERT(sys_line(rec, len, "IPV6", "auto temp"));
	KT_ASSERT(sys_word(rec, len, "ADDRESS6", "temp"));
	KT_ASSERT(sys_word(rec, len, "ADDRESS6", "auto"));

	/* temporary addresses ended */
	n6.temp = FALSE;
	KT_ASSERT_ER(conf6_take(&n6), E_OK);
	KT_ASSERT_EQ(so_getip6(), SO_IP6_AUTO);
	for ( i = 0; i < 30 && has6(temp, NULL, NULL); i++ ) tk_dly_tsk(100);
	KT_ASSERT(!has6(temp, NULL, NULL));
	KT_ASSERT(has6(stable, NULL, NULL));
	KT_ASSERT(source6(src) && same16(src, stable));

	/* the link-local address only */
	n6.how = CF_IP6_NONE;
	KT_ASSERT_ER(conf6_take(&n6), E_OK);
	KT_ASSERT_EQ(so_getip6(), SO_IP6_LINK);
	KT_ASSERT(!addr6_with(0, 0, NULL, 0));
	KT_ASSERT(link_local(x) && same16(x, ll));
	KT_ASSERT_ER(so_getdns6(0, ns), E_NOEXS);
	KT_ASSERT(so_getifaddr6(NULL, 0, gw) == 0 && cf_ip6zero(gw));
	len = sys_net(rec, sizeof(rec));
	KT_ASSERT(sys_line(rec, len, "IPV6", "none"));
	KT_ASSERT(!sys_line(rec, len, "GATEWAY6", ""));

	/* IPv6 off: the link-local address is held back, and nothing goes out */
	n6.on = FALSE;
	(void)cf_ip6("fec0::53", n6.dns[0]);
	KT_ASSERT_ER(conf6_take(&n6), E_OK);
	KT_ASSERT_EQ(so_getip6(), SO_IP6_OFF);
	KT_ASSERT(has6(ll, &fl, NULL) && ( fl & SO_IF6_TENTATIVE ));
	KT_ASSERT_ER(so_getdns6(0, ns), E_NOEXS);		/* not asked while off */
	KT_ASSERT(!source6(src));
	len = sys_net(rec, sizeof(rec));
	KT_ASSERT(sys_line(rec, len, "IPV6", "off"));

	/* what there was, back: on and from the routers */
	KT_ASSERT_ER(conf6_take(NULL), E_OK);
	KT_ASSERT_EQ(so_getip6(), SO_IP6_AUTO);
	KT_ASSERT(slaac_addr(x, CONF6_WAIT_S) && same16(x, stable));
	KT_ASSERT(link_local(x) && same16(x, ll));
	(void)so_setdns6(0, NULL);
	Kfree(c6_keep);
	c6_keep = NULL;
}
#endif

/*
 * ECN: the type of service (IPv4, IP_TOS) and the traffic class (IPv6,
 * IPV6_TCLASS) a datagram is sent with come with it when the receiving
 * socket asks for them (IP_RECVTOS, IPV6_RECVTCLASS) and reads with
 * so_recvtos; a socket that did not ask gets -1. The ECN codepoints and a
 * DSCP beside one go through the loopback as they were sent.
 */
LOCAL void test_ecn( void )
{
	struct sockaddr_in	me, peer;
	struct sockaddr_in6	me6, peer6;
	socklen_t		len;
	UB			buf[16];
	INT			u, n, on = 1, v, tos;

	if ( !have_stack ) KT_SKIP("no stack");

	u = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(u >= 0);
	if ( u < 0 ) return;
	addr_set(&me, LOOPBACK, TEST_PORT + 42);
	KT_ASSERT_ER(so_bind(u, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_EQ(so_sendto(u, "ecn0", 4, 0, (struct sockaddr *)&me, sizeof(me)), 4);
	len = sizeof(peer);
	tos = 0;
	n = so_recvtos(u, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &len, &tos);
	KT_ASSERT_EQ(n, 4);
	KT_ASSERT_EQ(tos, -1);					/* not asked for */
	KT_ASSERT_EQ(so_ntohs(peer.sin_port), TEST_PORT + 42);

	KT_ASSERT_ER(so_setsockopt(u, IPPROTO_IP, IP_RECVTOS, &on, sizeof(on)), E_OK);
	v = 0x01;						/* ECT(1) */
	KT_ASSERT_ER(so_setsockopt(u, IPPROTO_IP, IP_TOS, &v, sizeof(v)), E_OK);
	KT_ASSERT_EQ(so_sendto(u, "ecn1", 4, 0, (struct sockaddr *)&me, sizeof(me)), 4);
	len = sizeof(peer);
	n = so_recvtos(u, buf, sizeof(buf), 0, (struct sockaddr *)&peer, &len, &tos);
	KT_ASSERT_EQ(n, 4);
	KT_ASSERT_EQ(tos, 0x01);
	KT_ASSERT(n == 4 && buf[3] == '1');
	v = 0xb8 | 0x03;					/* EF and CE */
	KT_ASSERT_ER(so_setsockopt(u, IPPROTO_IP, IP_TOS, &v, sizeof(v)), E_OK);
	v = 0;
	len = sizeof(v);
	KT_ASSERT_ER(so_getsockopt(u, IPPROTO_IP, IP_TOS, &v, &len), E_OK);
	KT_ASSERT_EQ(v, 0xbb);
	KT_ASSERT_EQ(so_sendto(u, "ecn2", 4, 0, (struct sockaddr *)&me, sizeof(me)), 4);
	n = so_recvtos(u, buf, sizeof(buf), 0, NULL, NULL, &tos);
	KT_ASSERT_EQ(n, 4);
	KT_ASSERT_EQ(tos, 0xbb);
	KT_ASSERT_EQ(so_sendto(u, "ecn3", 4, 0, (struct sockaddr *)&me, sizeof(me)), 4);
	KT_ASSERT_EQ(so_recvfrom(u, buf, sizeof(buf), 0, NULL, NULL), 4);	/* read as ever */
	so_close(u);

	u = so_socket(AF_INET6, SOCK_DGRAM, 0);
	KT_ASSERT(u >= 0);
	if ( u < 0 ) return;
	addr6_set(&me6, in6_lo, TEST6_PORT + 5, 0);
	KT_ASSERT_ER(so_bind(u, (struct sockaddr *)&me6, sizeof(me6)), E_OK);
	KT_ASSERT_ER(so_setsockopt(u, IPPROTO_IPV6, IPV6_RECVTCLASS, &on, sizeof(on)), E_OK);
	v = 0x02;						/* ECT(0) */
	KT_ASSERT_ER(so_setsockopt(u, IPPROTO_IPV6, IPV6_TCLASS, &v, sizeof(v)), E_OK);
	KT_ASSERT_EQ(so_sendto(u, "ecn6", 4, 0, (struct sockaddr *)&me6, sizeof(me6)), 4);
	len = sizeof(peer6);
	tos = -2;
	n = so_recvtos(u, buf, sizeof(buf), 0, (struct sockaddr *)&peer6, &len, &tos);
	KT_ASSERT_EQ(n, 4);
	KT_ASSERT_EQ(tos, 0x02);
	KT_ASSERT_EQ(peer6.sin6_family, AF_INET6);
	v = 0;
	len = sizeof(v);
	KT_ASSERT_ER(so_getsockopt(u, IPPROTO_IPV6, IPV6_TCLASS, &v, &len), E_OK);
	KT_ASSERT_EQ(v, 0x02);
	so_close(u);
}
#endif /* TS_NETSTACK_NETBSD */

EXPORT void ktest_so( void )
{
	KT_RUN(test_stack);
	KT_RUN(test_udp);
	KT_RUN(test_tcp);
	KT_RUN(test_tcp_rate);
	KT_RUN(test_call_rate);
	KT_RUN(test_sockobj);
	KT_RUN(test_sockobj_tcp);
	KT_RUN(test_sockobj_prc);
	KT_RUN(test_sockobj_rate);
	KT_RUN(test_select);
	KT_RUN(test_msg);
	KT_RUN(test_ioctl);
	KT_RUN(test_names);
#if TS_NETSTACK_NETBSD
	KT_RUN(test_ip6_loop);
	KT_RUN(test_ip6_v6only);
	KT_RUN(test_ip6_names);
	KT_RUN(test_ip6_linklocal);
	KT_RUN(test_ecn);
#endif
	KT_RUN(test_unsupported);
	KT_RUN(test_errors);
	KT_RUN(test_conf_text);
	KT_RUN(test_conf_text6);
	KT_RUN(test_conf_apply);
#if KT_BOARD || defined(KT_NET_USER)
	KT_RUN(test_dhcp);
	KT_RUN(test_sntp);
#if TS_NETSTACK_NETBSD
	KT_RUN(test_slaac);
	KT_RUN(test_slaac_relink);
	KT_RUN(test_aaaa);
#endif
#endif
#if TS_NETSTACK_NETBSD && defined(KT_ECHO6_PORT)
	KT_RUN(test_ip6_host);
#endif
#if TS_NETSTACK_NETBSD && defined(KT_NET_USER)
	KT_RUN(test_ip6_conf);
#endif
}
