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

#include <ts/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A record has no bound on its length, nor a line on its value's: the
 * one who reads a record reads it whole (knl_ob_rea_all, ui_rec_all),
 * and a value that is a list of any length -- CPU_CHORE, CPU_RT, IRQ,
 * MODES, KBMAP -- is read where it stands with cf_find and cf_wfind,
 * never copied into a buffer of a fixed size. CF_TEXT_MAX is only the
 * room a record of a few fixed lines is first read into; CF_VAL_MAX the
 * room for a value whose length is bounded by what it is (a number, an
 * address, a name), into which cf_get and cf_word copy.
 */
#define CF_TEXT_MAX	4096		/* bytes a record of fixed lines is first read into */
#define CF_VAL_MAX	128		/* bytes of a value bounded by what it is */

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

/*
 * The same in len bytes of a value as cf_find gives it, however long:
 * *p_w points at the word and *p_wlen is its length
 */
IMPORT BOOL cf_wfind( CONST UB *val, INT len, INT nth, CONST UB **p_w, INT *p_wlen );

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

/*
 * IPv6 of the network's record, on lines of their own:
 *
 *	$$IPV6		<on|off> <auto|fixed|none> [temp]
 *	$$IPV6ADDR	<address>/<prefix length>
 *	$$IPV6GW	<address>
 *	$$IPV6DNS	<address>
 *	$$IPV6DNS	<address>
 *
 * "auto" takes addresses from the routers' advertisements, "fixed" the
 * one of $$IPV6ADDR with $$IPV6GW as the default router, "none" only
 * the link-local address; "temp" makes temporary addresses as well
 * (auto). A line that is only its key has no address. With no $$IPV6
 * line IPv6 is on and automatic.
 */
#define CF_IP6_AUTO	0
#define CF_IP6_FIXED	1
#define CF_IP6_NONE	2

typedef struct {
	BOOL	on;			/* IPv6 used on the card's interface */
	INT	how;			/* CF_IP6_ */
	BOOL	temp;			/* temporary addresses (RFC 8981) */
	UB	addr[16];		/* fixed: the address, in the order sent */
	INT	plen;			/* and its prefix length */
	UB	gw[16];			/* fixed: the default router, all zero none */
	UB	dns[2][16];		/* name servers, all zero none */
} T_CFNET6;

IMPORT void cf_net6_read( CONST UB *text, INT len, T_CFNET6 *net );
IMPORT INT  cf_net6_write( UB *text, INT len, INT max, CONST T_CFNET6 *net );

/*
 * An IPv6 address as text ("2001:db8::1", or with a dotted IPv4 end);
 * FALSE when the text is not one. cf_ip6str writes the shortest form
 * (RFC 5952) into room for CF_IP6_STRLEN bytes; cf_ip6zero says the
 * address is all zero.
 */
#define CF_IP6_STRLEN	46
IMPORT BOOL cf_ip6( CONST char *s, UB *addr );
IMPORT void cf_ip6str( CONST UB *addr, char *out );
IMPORT BOOL cf_ip6zero( CONST UB *addr );

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

/* ------------------------------------------------------------ the windows' look */

/*
 * What システム環境設定〈ウィンドウ〉 sets (LK_LOOK..LK_OPA_WHOLE), each
 * a line of the machine's settings (SYSDEF_CONF_DEV) and an entry of
 * the look table, in the same form as the person's.
 */
IMPORT CONST CF_ITEM	cf_look_items[];
IMPORT CONST INT	cf_look_nitem;

IMPORT CONST CF_ITEM *cf_look_look( UINT look );

/* ------------------------------------------------------------ links in a settings object */

/*
 * A setting that names an object -- the wallpaper, the system's face --
 * is a link in the settings object's record 0 (xmlTAD), told from the
 * others by its own relationship (the link's relationship attribute,
 * words a space apart): the link counts the object as used, and follows
 * it however it is named. cf_link_get finds the link with that word
 * and gives the object it points at; cf_link_put sets it to u (vobj the
 * new link's own identity), in place of the one there, or as a
 * paragraph label and link before the end of the document when there
 * is none, and gives in *p_old the object it pointed at before (zero
 * for none): the new length, -1 when max bytes are not room enough,
 * -2 when the record has no document to put it in. Text only: whoever
 * calls reads and writes the record, and counts the links in and out
 * where the store does not.
 */
IMPORT BOOL cf_link_get( CONST UB *text, INT len, CONST char *role, TS_UUID *p_u );
/* The nth (from 0) of the links with that word, for a setting that is a list of objects */
IMPORT BOOL cf_link_nth( CONST UB *text, INT len, CONST char *role, INT nth, TS_UUID *p_u );
IMPORT INT  cf_link_put( UB *text, INT len, INT max, CONST char *role, CONST TS_UUID *u,
			 CONST TS_UUID *vobj, CONST char *label, TS_UUID *p_old );

/*
 * The link to u given the word role in its relationship (a link made
 * before the words were given): the new length, the same when it has
 * the word, -1 for no room, -2 when no link points at u
 */
IMPORT INT  cf_link_tag( UB *text, INT len, INT max, CONST TS_UUID *u, CONST char *role );

/*
 * A setting that is a list of objects, links with the same word: one to
 * u added as a paragraph of its own after label (the same length when
 * there is one already, -1 for no room), or the one to u taken out with
 * its paragraph when it held only the link (-2 when none points at u)
 */
IMPORT INT  cf_link_add( UB *text, INT len, INT max, CONST char *role, CONST TS_UUID *u,
			 CONST TS_UUID *vobj, CONST char *label );
IMPORT INT  cf_link_drop( UB *text, INT len, CONST char *role, CONST TS_UUID *u );

#define CF_ROLE_WALL	"tessronos.wallpaper"	/* 機器の設定: the picture the ground is laid with */
#define CF_ROLE_FACE	"tessronos.systemface"	/* 機器の設定: the face the system's letters are drawn in */
/*
 * 機器の設定: an object emptied at each start (tmp), one link each.
 * The list is the rule: every object a link with this word points at
 * has its links taken off as the volume it is on is attached, before
 * anything else uses it. The line CF_CLEAR_LIST of record 1 says the
 * list has been made, so a volume made before there was one gets tmp
 * in it once, and a list the administrator emptied is left so.
 */
#define CF_ROLE_CLEAR	"tessronos.clearAtStart"
#define CF_CLEAR_LIST	"CLEAR_LIST"
/* The objects of the list on the volume attached at path emptied (after the list made, once, for an old volume) */
IMPORT void knl_sys_clear_start( CONST char *path );

/* In the kernel: the same on a settings object (its UUID as text), read and written whole (uconf.c) */
IMPORT BOOL knl_conf_link_get( CONST char *uuid, CONST char *role, TS_UUID *p_u );
IMPORT ER   knl_conf_link_put( CONST char *uuid, CONST char *role, CONST TS_UUID *target, CONST char *label );
/* A list of objects there: every link with role (how many, up to max put in buf), one added, one taken out */
IMPORT INT  knl_conf_links_get( CONST char *uuid, CONST char *role, TS_UUID *buf, INT max );
IMPORT ER   knl_conf_link_add( CONST char *uuid, CONST char *role, CONST TS_UUID *target, CONST char *label );
IMPORT ER   knl_conf_link_drop( CONST char *uuid, CONST char *role, CONST TS_UUID *target );

/* 機器の設定: a Bluetooth device paired, its object holding the keys (device/bt), one link each */
#define CF_ROLE_BTDEV	"tessronos.btdevice"

/*
 * In the kernel: a system object found from the root システム by the
 * relationship of its link, or else by the identity it was made with
 * (fixed); and the root's links given their relationships at the start
 */
IMPORT BOOL knl_sys_find( CONST char *role, CONST char *fixed, TS_UUID *p_u );
/* The same as the text of its UUID, in buf of TS_UUID_STRLEN + 1 bytes (or fixed itself) */
IMPORT CONST char *knl_sys_id( CONST char *role, CONST char *fixed, char *buf );
IMPORT void knl_sys_mark( void );

#ifdef __cplusplus
}
#endif

#endif /* __TS_CONF_H__ */
