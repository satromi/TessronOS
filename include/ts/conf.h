/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	conf.h
 *	The system's settings as text (design 16.5.23)
 *
 *	Three objects of the system box keep what the settings accessories
 *	set, each in its record 1 as lines of UTF-8 text: the network's
 *	(SYSDEF_CONF_NET), the machine's (SYSDEF_CONF_DEV) and the person's
 *	(SYSDEF_CONF_USER). A line is a key, a tab or spaces, and the rest;
 *	a line that starts with # says something to whoever reads it and
 *	nothing to the system. A key may stand on more than one line (the
 *	network's $$DNS does), and a line is then known by its key and
 *	which of them it is.
 *
 *	The kernel reads these at start and when an accessory says it has
 *	written one (so_conf, wm_conf_apply); the accessories read and
 *	write them. Both use this one file (lib/libconf), which calls
 *	nothing of the system.
 */

#ifndef __TS_CONF_H__
#define __TS_CONF_H__

#ifdef __cplusplus
extern "C" {
#endif

#define CF_TEXT_MAX	4096		/* bytes of one settings record */
#define CF_VAL_MAX	128		/* bytes of one line's value */

/*
 * The value of the nth (0 up) line with this key: *p_val points into
 * the text and *p_len is its length, without the line's end. Answers
 * TRUE when there is such a line; a line that is only its key has a
 * value of length 0.
 */
IMPORT BOOL cf_find( CONST UB *text, INT len, CONST char *key, INT nth,
		     CONST UB **p_val, INT *p_len );

/* The same, copied out as a string of at most max bytes with its nought */
IMPORT BOOL cf_get( CONST UB *text, INT len, CONST char *key, INT nth,
		    char *out, INT max );

/*
 * Put a value on the nth line with this key: the line is replaced, or
 * added at the end when the text has fewer such lines. A NULL or empty
 * value leaves the key alone on its line. Answers the text's new
 * length, or -1 when it would not fit in max bytes.
 */
IMPORT INT cf_put( UB *text, INT len, INT max, CONST char *key, INT nth,
		   CONST char *val );

/* The nth (0 up) word of a value, words being parted by blanks */
IMPORT BOOL cf_word( CONST char *val, INT nth, char *out, INT max );

/* A number, decimal or 0x hexadecimal; dflt when it is not one */
IMPORT INT  cf_num( CONST char *s, INT dflt );

/*
 * An IPv4 address a.b.c.d, in network byte order (the order the
 * sockets use); FALSE when the text is not one.
 */
IMPORT BOOL cf_ip( CONST char *s, UW *p_addr );
IMPORT void cf_ipstr( UW addr, char *out );	/* room for 16 bytes */

/* A number as decimal text; answers its length */
IMPORT INT  cf_itoa( INT v, char *out );

/* ------------------------------------------------------------ the network */

/*
 * The network's record keeps these lines:
 *
 *	$$HOST	<name|___0> cnet <mask> <address> NETDRV DEVICE=neta
 *	$$DNS	<name|___1> cnet <mask> <address> DOMAIN=<domain>
 *	$$DNS	<name|___2> cnet <mask> <address>
 *	$$GW	gateway cnet <mask> <address>
 *	$$NTP	<server>
 *
 * An address of 0.0.0.0 for the host asks the network for one (DHCP),
 * and 0.0.0.0 for a name server takes the one the network gives. A
 * $$GW or $$NTP line that is only its key has no gateway, or sets no
 * clock. The ___n names stand for a name left empty.
 */
typedef struct {
	char	host[80];		/* the machine's name, "" none */
	UW	addr, mask, gw;		/* network byte order; addr 0: DHCP */
	BOOL	usegw;
	char	dnsname[2][80];
	UW	dns[2];
	char	domain[80];
	char	ntp[80];		/* the time server, "" none */
} T_CFNET;

IMPORT void cf_net_read( CONST UB *text, INT len, T_CFNET *net );
IMPORT INT  cf_net_write( UB *text, INT len, INT max, CONST T_CFNET *net );

/* ------------------------------------------------------------ the person */

/*
 * What ユーザ環境設定 sets, each a line of the person's record and an
 * entry of the look table (include/ts/look.h): the key its line has,
 * the entry, the least and most it may be and what it is when nothing
 * says. USER_NAME is the one line that is not a number.
 */
typedef struct {
	CONST char	*key;
	UINT		look;
	INT		lo, hi, dflt;
} CF_ITEM;

IMPORT CONST CF_ITEM	cf_user_items[];
IMPORT CONST INT	cf_user_nitem;

IMPORT CONST CF_ITEM *cf_user_item( CONST char *key );
IMPORT CONST CF_ITEM *cf_user_look( UINT look );

#define CF_USER_NAME	"USER_NAME"
#define CF_NAME_MAX	12		/* letters of a person's name */

#ifdef __cplusplus
}
#endif

#endif /* __TS_CONF_H__ */
