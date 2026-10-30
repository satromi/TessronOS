/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_conf.c
 *	The network as its settings say (design 12.6.3, 16.5.23)
 *
 *	The settings are the record 1 of the network's settings object
 *	(SYSDEF_CONF_NET), lines of text that ネットワーク設定 writes
 *	(include/ts/conf.h). Taking them sets, in this order: the machine's
 *	name and the domain, the interface -- a fixed address, or the
 *	network asked for one when the address is 0.0.0.0 -- with its name
 *	servers, the time server, and IPv6 -- off, the link-local address
 *	only, addresses from the routers, or a fixed address with its
 *	router -- with its name servers. The desktop takes them once the store
 *	is there; the accessory asks for them to be taken again when it has
 *	written them.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/so.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include "obj/obj.h"

/* The record as it stands, read whole into a buffer of its own (Kfree it); its length or an error */
LOCAL INT conf_text( UB **p_buf )
{
	TS_UUID	u;
	ID	key;
	INT	len;

	*p_buf = NULL;
	if ( ob_fnd_sys(SYSROLE_CONF_NET, SYSDEF_CONF_NET, &u) < E_OK ) {
		return E_SYS;
	}
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key < E_OK ) {
		return (INT)key;
	}
	len = knl_ob_rea_all(key, 1, 0, p_buf, NULL);
	ob_cls_obj(key);
	return len;
}

LOCAL void say_ip( CONST char *what, UW a )
{
	char	s[16];

	cf_ipstr(a, s);
	tm_printf((UB *)" %s %s", what, s);
}

LOCAL void say_ip6( CONST char *what, CONST UB *a )
{
	char	s[CF_IP6_STRLEN];

	cf_ip6str(a, s);
	tm_printf((UB *)" %s %s", what, s);
}

/*
 * IPv6: its name servers, and how the card's interface is to have its
 * addresses -- none at all, its link-local one only, from the routers
 * (with temporary ones as well), or the fixed one with its router.
 */
LOCAL ER conf_apply6( CONST UB *text, INT len )
{
	T_CFNET6	n6;
	UINT		mode;
	ER		er;

	cf_net6_read(text, len, &n6);
	(void)so_setdns6(0, n6.on ? n6.dns[0] : NULL);
	(void)so_setdns6(1, n6.on ? n6.dns[1] : NULL);
	mode = !n6.on ? SO_IP6_OFF
	     : ( n6.how == CF_IP6_FIXED ) ? SO_IP6_FIXED
	     : ( n6.how == CF_IP6_NONE ) ? SO_IP6_LINK
	     : ( SO_IP6_AUTO | ( n6.temp ? SO_IP6_TEMP : 0 ) );
	er = so_setip6(mode, n6.addr, n6.plen, n6.gw);
	if ( er == E_NOSPT ) {
		return er;			/* a stack without IPv6 */
	}
	if ( er < E_OK ) {
		tm_printf((UB *)"net: IPv6 settings not taken (%d)\n", er);
		return er;
	}
	tm_printf((UB *)"net: IPv6 settings taken: %s",
		  ( mode == SO_IP6_OFF ) ? "off" : ( mode == SO_IP6_FIXED ) ? "fixed"
		  : ( mode == SO_IP6_LINK ) ? "link-local only" : n6.temp ? "auto, temporary addresses" : "auto");
	if ( mode == SO_IP6_FIXED ) {
		say_ip6("address", n6.addr);
		tm_printf((UB *)"/%d", n6.plen);
		if ( !cf_ip6zero(n6.gw) ) say_ip6("router", n6.gw);
	}
	if ( n6.on && !cf_ip6zero(n6.dns[0]) ) say_ip6("dns", n6.dns[0]);
	if ( n6.on && !cf_ip6zero(n6.dns[1]) ) say_ip6("dns", n6.dns[1]);
	tm_printf((UB *)"\n");
	return E_OK;
}

LOCAL ER conf_apply( void )
{
	T_CFNET	*net;
	UB	*text;
	INT	len, i;
	ER	er = E_OK, er6;

	net = (T_CFNET *)Kmalloc(sizeof(T_CFNET));
	if ( net == NULL ) {
		return E_NOMEM;
	}
	len = conf_text(&text);
	if ( len < 0 ) {
		tm_printf((UB *)"net: the settings could not be read (%d)\n", len);
		Kfree(net);
		return (ER)len;
	}
	cf_net_read(text, len, net);

	for ( i = 0; net->host[i] != '\0'; i++ ) ;
	if ( i > 0 ) {
		(void)so_sethostname(net->host, i);
	} else {
		(void)so_sethostname("tessronos", 9);
	}
	(void)so_setdomain(net->domain);
	(void)so_setdns(0, net->dns[0]);
	(void)so_setdns(1, net->dns[1]);
	if ( net->addr != 0 ) {
		er = so_setifaddr(net->addr, net->mask, net->usegw ? net->gw : 0, net->dns[0]);
	} else if ( !so_dhcp_on() ) {
		er = so_dhcp_start();
	}
	(void)so_sntp_server(net->ntp);

	tm_printf((UB *)"net: settings taken: %s", ( net->addr != 0 ) ? "fixed" : "DHCP");
	if ( net->addr != 0 ) {
		say_ip("address", net->addr);
		say_ip("mask", net->mask);
		if ( net->usegw ) say_ip("gateway", net->gw);
	}
	if ( net->dns[0] != 0 ) say_ip("dns", net->dns[0]);
	if ( net->dns[1] != 0 ) say_ip("dns", net->dns[1]);
	tm_printf((UB *)" ntp %s\n", ( net->ntp[0] != '\0' ) ? net->ntp : "(none)");

	er6 = conf_apply6(text, len);
	if ( er >= E_OK && er6 < E_OK && er6 != E_NOSPT ) {
		er = er6;
	}

	Kfree(text);
	Kfree(net);
	return er;
}

EXPORT ER so_conf( UINT op )
{
	switch ( op ) {
	case SO_CONF_APPLY:	return conf_apply();
	case SO_CONF_BUSY:	return (ER)so_busy(0);
	default:		return E_PAR;
	}
}
