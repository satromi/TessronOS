/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_wg.c
 *	The VPN (make WG=1 NET=user KTONLY=wg, docs/tessronos/09-network.md):
 *	through the device object wg0, a tunnel to a WireGuard peer written
 *	separately (tools/wg_peer.py on the host, port 24820): the handshake,
 *	a datagram out and its echo back in; the bytes counted; what is not
 *	a request refused; down, and up again.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/so.h>
#include <ts/net.h>
#include <ts/ob.h>
#include <ts/uuid.h>

#ifdef USE_WG

#define WG_UP_LINE \
	"UP 202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f 0 0a090001 ffffff00 " \
	"79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a - 0a000202 24820 0a090000/24"

LOCAL TS_UUID	wg;

LOCAL ER wg_cmd( CONST char *c )
{
	ID	key = ob_opn_obj(&wg, OB_OP_WRITE);
	SZ	asz = 0;
	ER	er;

	if ( key < E_OK ) return (ER)key;
	er = ob_wri_rec(key, 1, 0, c, (SZ)knl_strlen(c), &asz);
	ob_cls_obj(key);
	return er;
}

LOCAL BOOL state_holds( CONST char *s )
{
	static char	t[128];
	ID		key = ob_opn_obj(&wg, OB_OP_R);
	SZ		asz = 0;
	INT		i, k;

	if ( key < E_OK ) return FALSE;
	(void)ob_rea_rec(key, 1, 0, t, sizeof(t) - 1, &asz);
	ob_cls_obj(key);
	t[asz] = 0;
	for ( i = 0; t[i] != 0; i++ ) {
		for ( k = 0; s[k] != 0 && t[i + k] == s[k]; k++ ) ;
		if ( s[k] == 0 ) return TRUE;
	}
	return FALSE;
}

/* A datagram to the far end of the tunnel until its echo comes */
LOCAL BOOL echo_through( void )
{
	static CONST char	msg[] = "through the tunnel";
	static char		got[64];
	struct sockaddr_in	sa;
	struct timeval		tv;
	INT			s, i, n = -1;

	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	if ( s < 0 ) return FALSE;
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	(void)so_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	knl_memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = lwip_htons(7777);
	sa.sin_addr.s_addr = lwip_htonl(0x0a090002);
	for ( i = 0; i < 15 && n < 0; i++ ) {
		(void)so_sendto(s, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&sa, sizeof(sa));
		n = so_recv(s, got, sizeof(got) - 1, 0);
	}
	(void)so_close(s);
	if ( n <= 0 ) {
		tm_printf((UB *)"wg: no echo (%d)\n", n);
		return FALSE;
	}
	got[n] = 0;
	tm_printf((UB *)"wg: came back [%s] after %d tries\n", got, i);
	return (BOOL)( knl_strcmp(got, "echo:through the tunnel") == 0 );
}

LOCAL void test_wg_tunnel( void )
{
	UW	addr = 0;
	INT	t;

	KT_ASSERT_ER(so_dhcp_start(), E_OK);
	for ( t = 0; t < 150 && ( so_getifaddr(&addr, NULL, NULL) < E_OK || addr == 0 ); t++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(addr != 0);
	KT_ASSERT_ER(knl_obdev_uuid((CONST UB *)"wg0", &wg), E_OK);
	KT_ASSERT(state_holds("STATE down"));

	KT_ASSERT_ER(wg_cmd("UP nonsense"), E_PAR);
	KT_ASSERT_ER(wg_cmd("SOMETHING"), E_PAR);
	KT_ASSERT(state_holds("STATE down"));

	KT_ASSERT_ER(wg_cmd(WG_UP_LINE), E_OK);
	KT_ASSERT(state_holds("STATE up"));
	KT_ASSERT(echo_through());
	KT_ASSERT(!state_holds("RX 0\n"));
	KT_ASSERT(!state_holds("TX 0\n"));

	KT_ASSERT_ER(wg_cmd("DOWN"), E_OK);
	KT_ASSERT(state_holds("STATE down"));

	/* and again: the interface was really taken away, and a new handshake is made */
	KT_ASSERT_ER(wg_cmd(WG_UP_LINE), E_OK);
	KT_ASSERT(echo_through());
	KT_ASSERT_ER(wg_cmd("DOWN"), E_OK);
}

EXPORT void ktest_wg( void )
{
	KT_RUN(test_wg_tunnel);
}

#else

EXPORT void ktest_wg( void )
{
	kt_skip("wg", "make WG=1");
}

#endif
