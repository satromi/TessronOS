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
 *	ktest_net.c instead.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/so.h>
#include <ts/net.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/sysdef.h>

#define TEST_PORT	17000
#define LOOPBACK	0x7f000001UL		/* 127.0.0.1 */

LOCAL BOOL	have_stack = FALSE;

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

#if defined(RPI5) || defined(KT_NET_USER)
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
	(void)so_sethostname("tessronos", 7);
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
	T_OBPRT			p;
	T_OBREC			rec[4];
	T_OBNTF			req;
	T_OBNTM			m;
	TS_UUID			u, x, pu;
	UB			*t, b[16];
	SZ			asz = 0;
	INT			s, c, cnt = 0;
	ID			key, nk, port;

	if ( !have_stack ) KT_SKIP("no stack");
	t = (UB *)Kmalloc(OB_ATR_MAX);
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
	KT_ASSERT_ER(ob_get_prt(&u, &p), E_OK);
	KT_ASSERT_EQ(ts_uuid_cmp(&p.owner, &ob_user_system), 0);
	KT_ASSERT_EQ(p.mode, 0600);
	KT_ASSERT(listed(OB_S_SOCKET, &u));
	KT_ASSERT(listed(0, &u));
	KT_ASSERT(!listed(OB_S_QUEUE, &u));
	KT_ASSERT_ER(ob_set_prt(&u, &p), E_NOSPT);		/* fixed */

	/* what the metadata says follows the socket */
	addr_set(&me, LOOPBACK, TEST_PORT + 40);
	KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);
	key = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	KT_ASSERT(key > 0);
	if ( key <= 0 ) goto out;
	KT_ASSERT_ER(ob_get_atr(key, t, OB_ATR_MAX, &asz), E_OK);
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
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, t, OB_ATR_MAX, &asz), E_OK);
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
	KT_ASSERT_ER(ob_rea_rec(key, 0, 0, t, OB_ATR_MAX, &asz), E_NOEXS);
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
	t = (UB *)Kmalloc(OB_ATR_MAX);
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
	KT_ASSERT_ER(ob_get_atr(ks, t, OB_ATR_MAX, &asz), E_OK);
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
	KT_ASSERT_ER(ob_get_atr(ka, t, OB_ATR_MAX, &asz), E_OK);
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
	KT_RUN(test_unsupported);
	KT_RUN(test_errors);
	KT_RUN(test_conf_text);
	KT_RUN(test_conf_apply);
#if defined(RPI5) || defined(KT_NET_USER)
	KT_RUN(test_dhcp);
	KT_RUN(test_sntp);
#endif
}
