/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_wg.c
 *	The VPN (make WG=1, docs/tessronos/09-network.md): the device object
 *	"wg0", whose record 1 says the state and takes what is asked:
 *
 *	  UP <private key> <port> <address> <mask> <peer's key> <preshared key or ->
 *	     <endpoint's address> <endpoint's port> <address/cidr,...>
 *	  DOWN
 *
 *	Keys and addresses are in hexadecimal (addresses as the 4 bytes of the
 *	network's order), the ports and cidr in decimal. The kernel keeps no
 *	key: the program holds it sealed in the person's objects and hands it
 *	over for each connection; the driver holds it until the interface goes.
 */

#ifdef USE_WG

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/ob.h>
#include <ts/conf.h>
#include "so_nb.h"

#define WG_LINE_MAX	1024
#define WG_KEY		32
#define WG_NETS		16

typedef struct {
	UW	addr;
	int	cidr;
} T_WGNET;

IMPORT int	rump_tswg_up( CONST UB *priv, int port, UW addr, UW mask, CONST UB *pub, CONST UB *psk,
			      UW ep_addr, int ep_port, CONST T_WGNET *nets, int nnets, UW gw );
IMPORT int	rump_tswg_down( void );
IMPORT int	rump_tswg_stat( unsigned long long *rx, unsigned long long *tx );

LOCAL BOOL	wg_up = FALSE;
LOCAL ER	wg_last = E_OK;
LOCAL ID	wg_mtx = 0;

LOCAL INT hexv( char c )
{
	return ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
	     : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
}

/* The word at *pp in hexadecimal, exactly n bytes; the word passed */
LOCAL BOOL hex_word( CONST char **pp, UB *out, INT n )
{
	CONST char *p = *pp;
	INT	i;

	while ( *p == ' ' ) p++;
	for ( i = 0; i < n; i++ ) {
		INT	h = hexv(p[0]), l = ( h >= 0 ) ? hexv(p[1]) : -1;

		if ( l < 0 ) return FALSE;
		out[i] = (UB)( h * 16 + l );
		p += 2;
	}
	if ( *p != ' ' && *p != '\0' ) return FALSE;
	*pp = p;
	return TRUE;
}

/* The address of "<address>/<cidr>" */
LOCAL BOOL hex_word_net( CONST char **pp, UW *pa )
{
	CONST char *p = *pp;
	INT	i;
	UB	*o = (UB *)pa;

	for ( i = 0; i < 4; i++ ) {
		INT	h = hexv(p[0]), l = ( h >= 0 ) ? hexv(p[1]) : -1;

		if ( l < 0 ) return FALSE;
		o[i] = (UB)( h * 16 + l );
		p += 2;
	}
	if ( *p != '/' ) return FALSE;
	*pp = p + 1;
	return TRUE;
}

LOCAL BOOL num_word( CONST char **pp, INT *pv, INT max )
{
	CONST char *p = *pp;
	INT	v = 0, n = 0;

	while ( *p == ' ' ) p++;
	while ( *p >= '0' && *p <= '9' && v <= max ) {
		v = v * 10 + ( *p++ - '0' );
		n++;
	}
	if ( n == 0 || v > max || ( *p != ' ' && *p != '\0' && *p != ',' ) ) return FALSE;
	*pv = v;
	*pp = p;
	return TRUE;
}

LOCAL ER wg_do_up( CONST char *p )
{
	static T_WGNET	nets[WG_NETS];
	UB	priv[WG_KEY], pub[WG_KEY], psk[WG_KEY];
	UW	addr, mask, ep, gw = 0;
	INT	port, ep_port, n = 0, e;
	BOOL	have_psk = TRUE, ok;

	ok = hex_word(&p, priv, WG_KEY) && num_word(&p, &port, 65535) && hex_word(&p, (UB *)&addr, 4)
	  && hex_word(&p, (UB *)&mask, 4) && hex_word(&p, pub, WG_KEY);
	if ( ok ) {
		while ( *p == ' ' ) p++;
		if ( p[0] == '-' && ( p[1] == ' ' || p[1] == '\0' ) ) {
			have_psk = FALSE;
			p++;
		} else {
			ok = hex_word(&p, psk, WG_KEY);
		}
	}
	ok = ok && hex_word(&p, (UB *)&ep, 4) && num_word(&p, &ep_port, 65535);
	while ( ok ) {
		INT	c;

		while ( *p == ' ' || *p == ',' ) p++;
		if ( *p == '\0' ) break;
		if ( n >= WG_NETS ) {
			ok = FALSE;
			break;
		}
		ok = hex_word_net(&p, &nets[n].addr) && num_word(&p, &c, 32);
		if ( ok ) nets[n++].cidr = c;
	}
	if ( !ok || n == 0 || ep_port == 0 ) {
		knl_memset(priv, 0, sizeof(priv));
		knl_memset(psk, 0, sizeof(psk));
		return E_PAR;
	}
	if ( wg_up ) (void)rump_tswg_down();
	wg_up = FALSE;
	(void)so_getifaddr(NULL, NULL, &gw);
	e = rump_tswg_up(priv, port, addr, mask, pub, have_psk ? psk : NULL, ep, ep_port, nets, n, gw);
	knl_memset(priv, 0, sizeof(priv));
	knl_memset(psk, 0, sizeof(psk));
	tm_printf((UB *)"wg: wg0 %s (%d)\n", ( e == 0 ) ? "up" : "not up", e);
	if ( e != 0 ) return knl_so_nb_er(e);
	wg_up = TRUE;
	return E_OK;
}

LOCAL ER wg_rea( void *ctx, UD *pos, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	unsigned long long rx = 0, tx = 0;
	char	t[96];
	INT	n;

	(void)ctx;
	(void)pos;
	if ( recno != OB_NET_STATE ) return E_NOEXS;
	if ( wg_up ) (void)rump_tswg_stat(&rx, &tx);
	n = tm_sprintf((UB *)t, (CONST UB *)"STATE %s\nRX %ld\nTX %ld\nLAST %d\n", wg_up ? "up" : "down",
		       (D)rx, (D)tx, (INT)wg_last);
	if ( off >= n ) {
		*p_asize = 0;
		return E_OK;
	}
	if ( size > n - off ) size = n - off;
	knl_memcpy(buf, t + off, size);
	*p_asize = size;
	return E_OK;
}

LOCAL ER wg_wri( void *ctx, UD *pos, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	static char	line[WG_LINE_MAX];
	INT		i;
	ER		er;

	(void)ctx;
	(void)pos;
	(void)off;
	if ( recno != OB_NET_STATE ) return E_RONLY;
	if ( size <= 0 || size >= WG_LINE_MAX ) return E_PAR;
	tk_loc_mtx(wg_mtx, TMO_FEVR);
	for ( i = 0; i < (INT)size && ((CONST char *)buf)[i] != '\n' && ((CONST char *)buf)[i] != '\0'; i++ ) {
		line[i] = ((CONST char *)buf)[i];
	}
	line[i] = '\0';
	if ( line[0] == 'U' && line[1] == 'P' && line[2] == ' ' ) {
		er = wg_do_up(line + 3);
	} else if ( line[0] == 'D' && line[1] == 'O' && line[2] == 'W' && line[3] == 'N' && line[4] == '\0' ) {
		INT	e = wg_up ? rump_tswg_down() : 0;

		wg_up = FALSE;
		er = ( e == 0 ) ? E_OK : knl_so_nb_er(e);
		tm_printf((UB *)"wg: wg0 down (%d)\n", e);
	} else {
		er = E_PAR;
	}
	knl_memset(line, 0, sizeof(line));	/* it may have held a key */
	wg_last = er;
	tk_unl_mtx(wg_mtx);
	if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
	return er;
}

LOCAL UD wg_size( void *ctx, INT recno )
{
	(void)ctx;
	return ( recno == OB_NET_STATE ) ? 96 : 0;
}

LOCAL CONST T_OBDVOPS wg_ops = { OB_S_NET, "vpn", OB_NET_NREC, 0666, 0, 0, NULL, NULL, wg_rea, wg_wri, wg_size, NULL };

/* The stack comes up before the object layer: the device is added once that is there */
LOCAL void wg_adder( INT stacd, void *exinf )
{
	TS_UUID	u;
	INT	i;

	(void)stacd;
	(void)exinf;
	for ( i = 0; i < 600; i++ ) {
		if ( knl_obdev_add((CONST UB *)"wg0", NULL, &wg_ops, NULL, &u) >= E_OK ) {
			tk_exd_tsk();
		}
		tk_dly_tsk(100);
	}
	tm_putstring((CONST UB *)"wg: the device object wg0 could not be made\n");
	tk_exd_tsk();
}

EXPORT void knl_wg_start( void )
{
	T_CMTX	cm;
	T_CTSK	ct;
	ID	t;

	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	wg_mtx = tk_cre_mtx(&cm);
	if ( wg_mtx <= 0 ) return;
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)wg_adder;
	ct.itskpri = CNF_MAX_TSKPRI - 2;
	ct.stksz = 8192;
	t = tk_cre_tsk(&ct);
	if ( t > 0 ) (void)tk_sta_tsk(t, 0);
}

#endif /* USE_WG */
