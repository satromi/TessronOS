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
 *	servers, and the time server. The desktop takes them once the store
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

/* The record as it stands, into a buffer of CF_TEXT_MAX; its length or an error */
LOCAL INT conf_text( UB *buf )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	if ( ts_str_to_uuid(SYSDEF_CONF_NET, &u) < E_OK ) {
		return E_SYS;
	}
	key = ob_opn_obj(&u, OB_OP_READ);
	if ( key < E_OK ) {
		return (INT)key;
	}
	er = ob_rea_rec(key, 1, 0, buf, CF_TEXT_MAX, &asz);
	ob_cls_obj(key);
	if ( er < E_OK ) {
		return (INT)er;
	}
	return (INT)( ( asz > CF_TEXT_MAX ) ? CF_TEXT_MAX : asz );
}

LOCAL void say_ip( CONST char *what, UW a )
{
	char	s[16];

	cf_ipstr(a, s);
	tm_printf((UB *)" %s %s", what, s);
}

LOCAL ER conf_apply( void )
{
	T_CFNET	*net;
	UB	*text;
	INT	len, i;
	ER	er = E_OK;

	text = (UB *)Kmalloc(CF_TEXT_MAX);
	net = (T_CFNET *)Kmalloc(sizeof(T_CFNET));
	if ( text == NULL || net == NULL ) {
		if ( text != NULL ) Kfree(text);
		if ( net != NULL ) Kfree(net);
		return E_NOMEM;
	}
	len = conf_text(text);
	if ( len < 0 ) {
		tm_printf((UB *)"net: the settings could not be read (%d)\n", len);
		Kfree(text);
		Kfree(net);
		return (ER)len;
	}
	cf_net_read(text, len, net);

	for ( i = 0; net->host[i] != '\0'; i++ ) ;
	if ( i > 0 ) {
		(void)so_sethostname(net->host, i);
	} else {
		(void)so_sethostname("tessronos", 7);
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
