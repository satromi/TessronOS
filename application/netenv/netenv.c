/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netenv.c
 *	ネットワーク設定: the accessory (design 16.5.23)
 *
 *	The sheet 〈ネットワーク〉〈アドレス〉 holds the machine's name and
 *	address, the two name servers with their names, the domain, the
 *	gateway and the subnet mask; 〈ネットワーク〉〈状態〉 shows what the
 *	interface has now. An address of 0.0.0.0 asks the network for one
 *	(DHCP), and name servers of 0.0.0.0 take the network's.
 *
 *	〈ＩＰｖ６〉〈設定〉 says whether IPv6 is used, and how the interface
 *	has its addresses: from the routers' advertisements (with temporary
 *	addresses as well, when asked), a fixed one with its prefix length
 *	and router, or none but the link-local one; and two IPv6 name
 *	servers, asked before those the routers name. 〈ＩＰｖ６〉〈状態〉
 *	shows the link-local address, the others with their lifetimes, the
 *	router and the name servers.
 *
 *	〈Ｗｉ－Ｆｉ〉〈接続〉 is the Wi-Fi: the networks saved (each its SSID,
 *	its security -- none, WPA2-Personal, WPA3-Personal or WPA2/WPA3-
 *	Personal, the last joined with WPA3 where the access point has it
 *	--, and whether it is joined
 *	by itself; the machine's, one list whoever is logged in), the
 *	networks the last scan found to choose an SSID from, or one typed,
 *	and 接続 and 切断. Everything goes through the card's device object
 *	(wlan0, OB_S_NET): its record 1 shows the saved networks without
 *	their keys, and a line written to it asks the card's manager in the
 *	kernel to save a network, forget one, join one or scan. The manager
 *	alone reads and writes the Wi-Fi's settings object, and keeps
 *	each network's key (made from the passphrase for WPA2, the password
 *	itself for WPA3): this program never reads a saved password. A saved network chosen shows XB_STANDIN
 *	marks in the password's box; left as they are, the network keeps
 *	its key, and anything typed replaces them. Nothing of it asks for the
 *	administrator's password. 〈Ｗｉ－Ｆｉ〉〈状態〉 shows the network
 *	joined. A machine without the card shows so, and its parts are not
 *	to be worked.
 *
 *	The pages of the Wi-Fi are read again when the card's object tells
 *	that its state or the networks of a scan changed (OB_E_CHANGE of
 *	records 1 and 2 to the window's channel), and the program sleeps
 *	until then. The signal and the rate, measured values that change all
 *	the time and are not told, are read every second while 〈状態〉 is
 *	shown. A card not there yet when the window opens is waited for on
 *	the list of the devices, which tells of a device that comes.
 *
 *	What is set is written to the network's settings object
 *	(SYSDEF_CONF_NET) as its lines (include/ts/conf.h) when the window
 *	closes with 保存, or at once with 設定を更新 [S]; the system takes
 *	them as it sees the object written, and again at every start. When
 *	other programs have sockets open -- the system object says how many
 *	(OB_SYS_NET) -- the person is told the new settings reach them only
 *	once they have finished, and may give up. What the interface has
 *	now is read from the same record.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ts/ui.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>
#include <ts/ob.h>
#include <ts/part.h>


/* The parts, by the numbers the data box gives them */
#define P_HOST		11
#define P_HOST_IP	12
#define P_DNS1		13
#define P_DNS1_IP	14
#define P_DNS2		15
#define P_DNS2_IP	16
#define P_DOM		17
#define P_GW		18
#define P_GW_IP		19
#define P_MASK		20
#define P_ST_ADDR	31
#define P_ST_MASK	32
#define P_ST_GW		33
#define P_ST_DNS	34
#define P_ST_AGAIN	35
#define P6_ON		41
#define P6_HOW		42		/* 1 自動, 2 固定, 3 使わない */
#define P6_TEMP		43
#define P6_ADDR		44
#define P6_PLEN		45
#define P6_GW		46
#define P6_DNS1		47
#define P6_DNS2		48
#define P6_ST_LINK	51
#define P6_ST_LIST	52
#define P6_ST_GW	53
#define P6_ST_DNS	54
#define P6_ST_AGAIN	55
#define P6_ST_MODE	56

/* 〈Ｗｉ－Ｆｉ〉 */
#define PW_SAVED	61		/* the networks saved: （新しい接続） and each */
#define PW_SSID		62
#define PW_SCAN_LIST	63
#define PW_SEC		64		/* 1 なし, 2 WPA2-Personal, 3 WPA3-Personal, 4 WPA2/WPA3-Personal */
#define SEC_NONE	1
#define SEC_WPA2	2
#define SEC_WPA3	3
#define SEC_WPA2WPA3	4
#define PW_PASS		65
#define PW_AUTO		66
#define PW_SCAN		67
#define PW_SAVE		68
#define PW_DEL		69
#define PW_JOIN		70
#define PW_LEAVE	71
#define PW_STATE	72
#define PW_ST_STATE	73		/* 〈Ｗｉ－Ｆｉ〉〈状態〉, 73 to 80 */
#define PW_ST_SSID	74
#define PW_ST_BSSID	75
#define PW_ST_FREQ	76
#define PW_ST_SIGNAL	77
#define PW_ST_RATE	78
#define PW_ST_SEC	79
#define PW_ST_MAC	80
#define PW_ST_AGAIN	81

/* Its panels: the window's, and those it asks with */
#define PN_MAIN		1
#define PN_WARN		10
#define PN_UPDATE	11
#define PN_FINISH	12
#define PN_BUSY		13
#define B_UPDATE	103
#define B_NOSAVE	104
#define B_SAVE		105

#define TAG_STATE	2		/* 〈ネットワーク〉〈状態〉 */
#define TAG_STATE6	4		/* 〈ＩＰｖ６〉〈状態〉 */
#define TAG_WIFI	5		/* 〈Ｗｉ－Ｆｉ〉〈接続〉 */
#define TAG_WIFI_ST	6		/* 〈Ｗｉ－Ｆｉ〉〈状態〉 */

LOCAL UI	ui;
LOCAL T_CFNET	was;			/* as the settings stood when read or written */
LOCAL T_CFNET6	was6;
LOCAL UB	text[CF_TEXT_MAX];	/* the record */
LOCAL INT	tlen;
LOCAL BOOL	closing;

/* How many lines of a key the text has */
LOCAL INT key_lines( CONST UB *t, INT len, CONST char *key )
{
	CONST UB	*v;
	INT		n, vl;

	for ( n = 0; t != NULL && cf_find(t, len, key, n, &v, &vl); n++ ) ;
	return n;
}

/* The bytes of all the values of a key, each with more bytes after it */
LOCAL INT key_bytes( CONST UB *t, INT len, CONST char *key, INT more )
{
	CONST UB	*v;
	INT		n, vl, all = 0;

	for ( n = 0; t != NULL && cf_find(t, len, key, n, &v, &vl); n++ ) all += vl + more;
	return all;
}

/* The system object's record of the network, as its lines, whole however long; the length, or an error */
LOCAL UB	*net_rec = NULL;

LOCAL INT net_read( void )
{
	TS_UUID	u;
	UB	*b;
	INT	len;

	if ( ui_uuid(SYSDEF_SYSTEM, &u) < E_OK ) return E_SYS;
	len = ui_rec_all(&u, OB_SYS_NET, 0, &b);
	if ( len < 0 ) return len;
	free(net_rec);
	net_rec = b;
	return len;
}

LOCAL void state_line( INT id, INT len, CONST char *key, CONST char *none )
{
	char	v[CF_VAL_MAX];

	if ( len > 0 && cf_get(net_rec, len, key, 0, v, sizeof(v)) && strcmp(v, "0.0.0.0") != 0 ) {
		ui_set_text(&ui, id, v);
	} else {
		ui_set_text(&ui, id, none);
	}
}

/* What the interface has now, on 〈状態〉 */
LOCAL void state_show( void )
{
	char	v[CF_VAL_MAX];
	INT	len = net_read();

	if ( len <= 0 ) {
		ui_set_text(&ui, P_ST_ADDR, "（ネットワークがありません）");
		return;
	}
	state_line(P_ST_ADDR, len, "ADDRESS", "（まだありません）");
	/*
	 * With no address the interface still has the mask of the address
	 * 0.0.0.0 that waits for DHCP (its class's, 255.0.0.0): not shown
	 */
	if ( !cf_get(net_rec, len, "ADDRESS", 0, v, sizeof(v)) || strcmp(v, "0.0.0.0") == 0 ) {
		ui_set_text(&ui, P_ST_MASK, "（なし）");
	} else {
		state_line(P_ST_MASK, len, "MASK", "（なし）");
	}
	state_line(P_ST_GW, len, "GATEWAY", "（なし）");
	state_line(P_ST_DNS, len, "DNS", "（なし）");
}

/* A lifetime as words: seconds, or "infinite" */
LOCAL void life_text( CONST char *w, char *out, INT max )
{
	INT	s;

	if ( strcmp(w, "infinite") == 0 ) {
		snprintf(out, max, "無期限");
		return;
	}
	s = cf_num(w, 0);
	if ( s >= 86400 ) snprintf(out, max, "%d日%d時間", s / 86400, s % 86400 / 3600);
	else if ( s >= 3600 ) snprintf(out, max, "%d時間%d分", s / 3600, s % 3600 / 60);
	else if ( s >= 60 ) snprintf(out, max, "%d分%d秒", s / 60, s % 60);
	else snprintf(out, max, "%d秒", s);
}

/* What is wrong with an address, from the words after its lifetimes: a string of its own (free it) */
LOCAL char *cond_text( CONST char *v )
{
	char	w[24], *out;
	INT	i, at = 0, max = 1;

	for ( i = 4; cf_word(v, i, w, sizeof(w)); i++ ) max += 32;
	out = (char *)malloc((size_t)max);
	if ( out == NULL ) return NULL;
	out[0] = 0;
	for ( i = 4; cf_word(v, i, w, sizeof(w)); i++ ) {
		CONST char *s = ( strcmp(w, "tentative") == 0 ) ? "重複確認中"
			      : ( strcmp(w, "duplicated") == 0 ) ? "重複"
			      : ( strcmp(w, "deprecated") == 0 ) ? "推奨期限切れ" : NULL;

		if ( s != NULL ) at += snprintf(out + at, (size_t)( max - at ), "・%s", s);
	}
	return out;
}

/* What IPv6 of the interface has now, on 〈ＩＰｖ６〉〈状態〉 */
LOCAL void state6_show( void )
{
	static char	**rows = NULL;		/* two rows for each address, as many as there are */
	static INT	nrows = 0;
	CONST char	**names;
	char		v[CF_VAL_MAX], a[64], kind[16], vl[32], pl[32], *cond, link[96], *dns;
	INT		len = net_read(), i, nrow = 0, at, nadr, room;
	BOOL		off;

	if ( len <= 0 ) {
		ui_set_text(&ui, P6_ST_MODE, "（ネットワークがありません）");
		return;
	}
	(void)cf_get(net_rec, len, "IPV6", 0, v, sizeof(v));
	off = ( v[0] == 0 || strcmp(v, "off") == 0 );
	ui_set_text(&ui, P6_ST_MODE, off ? "使用しない"
		    : ( strcmp(v, "none") == 0 ) ? "リンクローカルアドレスのみ"
		    : ( strcmp(v, "fixed") == 0 ) ? "固定"
		    : ( strcmp(v, "auto temp") == 0 ) ? "自動（ＳＬＡＡＣ）・一時アドレスあり" : "自動（ＳＬＡＡＣ）");

	strcpy(link, off ? "（使用していません）" : "（まだありません）");
	/* two rows for every address the interface has, however many */
	nadr = key_lines(net_rec, len, "ADDRESS6");
	for ( i = 0; i < nrows; i++ ) free(rows[i]);
	free(rows);
	nrows = 0;
	rows = (char **)calloc((size_t)( 2 * nadr + 1 ), sizeof(char *));
	names = (CONST char **)malloc(sizeof(char *) * (size_t)( 2 * nadr + 1 ));
	if ( rows == NULL || names == NULL ) {
		free((void *)names);
		return;
	}
	nrows = 2 * nadr + 1;
	for ( i = 0; i < nadr && cf_get(net_rec, len, "ADDRESS6", i, v, sizeof(v)); i++ ) {
		if ( !cf_word(v, 0, a, sizeof(a)) || !cf_word(v, 1, kind, sizeof(kind)) ) continue;
		cond = cond_text(v);
		if ( strcmp(kind, "link") == 0 ) {
			snprintf(link, sizeof(link), "%s%s", a, off ? "・停止中" : ( cond != NULL ) ? cond : "");	/* held back while off */
			free(cond);
			continue;
		}
		if ( off ) {
			free(cond);
			continue;
		}
		(void)cf_word(v, 2, vl, sizeof(vl));
		(void)cf_word(v, 3, pl, sizeof(pl));
		room = 160 + ( ( cond != NULL ) ? (INT)strlen(cond) : 0 );
		rows[nrow] = strdup(a);
		rows[nrow + 1] = (char *)malloc((size_t)room);
		if ( rows[nrow] == NULL || rows[nrow + 1] == NULL ) {
			free(cond);
			break;
		}
		nrow++;
		at = snprintf(rows[nrow], (size_t)room, "　%s", ( strcmp(kind, "temp") == 0 ) ? "一時"
			      : ( strcmp(kind, "auto") == 0 ) ? "自動" : "固定");
		life_text(vl, a, sizeof(a));
		at += snprintf(rows[nrow] + at, (size_t)( room - at ), "・有効 %s", a);
		life_text(pl, a, sizeof(a));
		snprintf(rows[nrow] + at, (size_t)( room - at ), "・推奨 %s%s", a, ( cond != NULL ) ? cond : "");
		nrow++;
		free(cond);
	}
	if ( nrow == 0 ) {
		rows[nrow++] = strdup("（ありません）");
	}
	for ( i = 0; i < nrow; i++ ) names[i] = ( rows[i] != NULL ) ? rows[i] : "";
	ui_set_text(&ui, P6_ST_LINK, link);
	ui_names(&ui, P6_ST_LIST, names, nrow);
	ui_set_value(&ui, P6_ST_LIST, 0);
	free((void *)names);

	if ( !off && cf_get(net_rec, len, "GATEWAY6", 0, v, sizeof(v)) ) ui_set_text(&ui, P6_ST_GW, v);
	else ui_set_text(&ui, P6_ST_GW, "（なし）");
	/* every name server, joined: room for each as long as its line */
	room = 1 + ( off ? 0 : key_bytes(net_rec, len, "DNS6", 4) );
	dns = (char *)malloc((size_t)room);
	if ( dns != NULL ) {
		dns[0] = 0;
		for ( i = 0, at = 0; !off && cf_get(net_rec, len, "DNS6", i, v, sizeof(v)); i++ ) {
			at += snprintf(dns + at, (size_t)( room - at ), "%s%s", ( i > 0 ) ? "、" : "", v);
			if ( at >= room ) break;
		}
	}
	ui_set_text(&ui, P6_ST_DNS, ( dns != NULL && dns[0] != 0 ) ? dns : "（なし）");
	free(dns);
}

/* Sockets other programs hold open */
LOCAL INT busy( void )
{
	char	v[16];
	INT	len = net_read();

	return ( len > 0 && cf_get(net_rec, len, "BUSY", 0, v, sizeof(v)) ) ? cf_num(v, 0) : 0;
}

/* ---------------------------------------------------------------- the settings */

LOCAL ER conf_open( UINT ops, ID *p_key )
{
	TS_UUID	u;
	ID	key;

	if ( ob_fnd_sys(SYSROLE_CONF_NET, SYSDEF_CONF_NET, &u) < E_OK ) return E_SYS;	/* the network's settings, by their relationship to システム */
	key = ui_conf_open(&u, ops);
	if ( key < E_OK ) return (ER)key;
	*p_key = key;
	return E_OK;
}

LOCAL ER conf_read( void )
{
	ID	key;
	SZ	asz = 0;
	ER	er;

	er = conf_open(OB_OP_R, &key);
	if ( er < E_OK ) return er;
	er = ob_rea_rec(key, 1, 0, text, sizeof(text), &asz);
	ob_cls_obj(key);
	if ( er < E_OK ) return er;
	tlen = (INT)( ( asz > (SZ)sizeof(text) ) ? (SZ)sizeof(text) : asz );
	cf_net_read(text, tlen, &was);
	cf_net6_read(text, tlen, &was6);
	return E_OK;
}

LOCAL void to_sheet( CONST T_CFNET *n )
{
	ui_set_text(&ui, P_HOST, n->host);
	ui_set_ip(&ui, P_HOST_IP, n->addr);
	ui_set_text(&ui, P_DNS1, n->dnsname[0]);
	ui_set_ip(&ui, P_DNS1_IP, n->dns[0]);
	ui_set_text(&ui, P_DNS2, n->dnsname[1]);
	ui_set_ip(&ui, P_DNS2_IP, n->dns[1]);
	ui_set_text(&ui, P_DOM, n->domain);
	ui_set_value(&ui, P_GW, n->usegw ? 1 : 0);
	ui_set_ip(&ui, P_GW_IP, n->gw);
	if ( !n->usegw && n->gw == 0 ) ui_blank(&ui, P_GW_IP);
	ui_set_ip(&ui, P_MASK, n->mask);
}

LOCAL void from_sheet( T_CFNET *n )
{
	*n = was;			/* the time server and anything else not on the sheet */
	(void)ui_text(&ui, P_HOST, n->host, sizeof(n->host));
	n->addr = ui_get_ip(&ui, P_HOST_IP);
	(void)ui_text(&ui, P_DNS1, n->dnsname[0], sizeof(n->dnsname[0]));
	n->dns[0] = ui_get_ip(&ui, P_DNS1_IP);
	(void)ui_text(&ui, P_DNS2, n->dnsname[1], sizeof(n->dnsname[1]));
	n->dns[1] = ui_get_ip(&ui, P_DNS2_IP);
	(void)ui_text(&ui, P_DOM, n->domain, sizeof(n->domain));
	n->usegw = ( ui_value(&ui, P_GW) != 0 );
	n->gw = ui_get_ip(&ui, P_GW_IP);
	n->mask = ui_get_ip(&ui, P_MASK);
}

/* The parts of 〈ＩＰｖ６〉〈設定〉 that mean nothing as the others stand, not to be worked */
LOCAL void parts6( void )
{
	BOOL	on = ( ui_value(&ui, P6_ON) != 0 );
	INT	how = ui_value(&ui, P6_HOW);

	ui_off(&ui, P6_HOW, !on);
	ui_off(&ui, P6_TEMP, !on || how != 1);
	ui_off(&ui, P6_ADDR, !on || how != 2);
	ui_off(&ui, P6_PLEN, !on || how != 2);
	ui_off(&ui, P6_GW, !on || how != 2);
	ui_off(&ui, P6_DNS1, !on);
	ui_off(&ui, P6_DNS2, !on);
}

LOCAL void addr_to( INT id, CONST UB *a )
{
	char	s[CF_IP6_STRLEN];

	if ( cf_ip6zero(a) ) {
		ui_set_text(&ui, id, "");
	} else {
		cf_ip6str(a, s);
		ui_set_text(&ui, id, s);
	}
}

LOCAL void to_sheet6( CONST T_CFNET6 *n )
{
	INT	v = n->plen;

	ui_set_value(&ui, P6_ON, n->on ? 1 : 0);
	ui_set_value(&ui, P6_HOW, ( n->how == CF_IP6_FIXED ) ? 2 : ( n->how == CF_IP6_NONE ) ? 3 : 1);
	ui_set_value(&ui, P6_TEMP, n->temp ? 1 : 0);
	addr_to(P6_ADDR, n->addr);
	ui_set_fields(&ui, P6_PLEN, &v, 1);
	addr_to(P6_GW, n->gw);
	addr_to(P6_DNS1, n->dns[0]);
	addr_to(P6_DNS2, n->dns[1]);
	parts6();
}

/*
 * The address a box holds into a: none for an empty box. The fixed
 * address's box may carry its prefix length after a slash. FALSE when
 * what it holds is not an address.
 */
LOCAL BOOL addr_from( INT id, UB *a, INT *p_plen )
{
	char	t[64], *s = t;
	INT	i, k;

	(void)ui_text(&ui, id, t, sizeof(t));
	while ( *s == ' ' ) s++;
	for ( i = (INT)strlen(s); i > 0 && s[i - 1] == ' '; i-- ) s[i - 1] = 0;
	if ( s[0] == 0 ) {
		memset(a, 0, 16);
		return TRUE;
	}
	for ( i = 0; s[i] != 0 && s[i] != '/'; i++ ) ;
	if ( s[i] == '/' ) {
		if ( p_plen == NULL ) return FALSE;
		s[i] = 0;
		k = cf_num(s + i + 1, -1);
		if ( k < 1 || k > 128 ) return FALSE;
		*p_plen = k;
	}
	return cf_ip6(s, a);
}

/* The sheet of IPv6 into n: 0, or the number of the box that holds no address */
LOCAL INT from_sheet6( T_CFNET6 *n )
{
	INT	v[1], how, bad = 0;

	*n = was6;
	n->on = ( ui_value(&ui, P6_ON) != 0 );
	how = ui_value(&ui, P6_HOW);
	n->how = ( how == 2 ) ? CF_IP6_FIXED : ( how == 3 ) ? CF_IP6_NONE : CF_IP6_AUTO;
	n->temp = ( ui_value(&ui, P6_TEMP) != 0 );
	if ( ui_fields(&ui, P6_PLEN, v, 1) >= 1 && v[0] >= 1 && v[0] <= 128 ) n->plen = v[0];
	/* what cannot be worked as the sheet stands is not looked at: it stays as it was */
	if ( !addr_from(P6_ADDR, n->addr, &n->plen) ) {
		if ( n->on && n->how == CF_IP6_FIXED ) bad = P6_ADDR;
		memcpy(n->addr, was6.addr, 16);
		n->plen = was6.plen;
	}
	if ( !addr_from(P6_GW, n->gw, NULL) ) {
		if ( n->on && n->how == CF_IP6_FIXED && bad == 0 ) bad = P6_GW;
		memcpy(n->gw, was6.gw, 16);
	}
	if ( !addr_from(P6_DNS1, n->dns[0], NULL) ) {
		if ( n->on && bad == 0 ) bad = P6_DNS1;
		memcpy(n->dns[0], was6.dns[0], 16);
	}
	if ( !addr_from(P6_DNS2, n->dns[1], NULL) ) {
		if ( n->on && bad == 0 ) bad = P6_DNS2;
		memcpy(n->dns[1], was6.dns[1], 16);
	}
	return bad;
}

LOCAL BOOL same( CONST T_CFNET *a, CONST T_CFNET *b )
{
	return (BOOL)( strcmp(a->host, b->host) == 0 && a->addr == b->addr && a->mask == b->mask
		    && a->usegw == b->usegw && ( !a->usegw || a->gw == b->gw )
		    && strcmp(a->dnsname[0], b->dnsname[0]) == 0 && strcmp(a->dnsname[1], b->dnsname[1]) == 0
		    && a->dns[0] == b->dns[0] && a->dns[1] == b->dns[1]
		    && strcmp(a->domain, b->domain) == 0 && strcmp(a->ntp, b->ntp) == 0 );
}

LOCAL BOOL same6( CONST T_CFNET6 *a, CONST T_CFNET6 *b )
{
	return (BOOL)( memcmp(a, b, sizeof(*a)) == 0 );
}

LOCAL void warn( CONST char *line )
{
	CONST char *lines[3] = { line, "", "" };

	(void)ui_ask(&ui, PN_WARN, lines, 3);
}

/* A name of letters, digits and signs only: what a host name and a domain may be */
LOCAL BOOL plain( CONST char *s )
{
	INT	i;

	for ( i = 0; s[i] != 0; i++ ) {
		if ( (UB)s[i] < 0x21 || (UB)s[i] > 0x7E ) return FALSE;
	}
	return TRUE;
}

/*
 * The sheet written as the settings and taken by the system. A network
 * in use by other programs is said so first, and may be given up.
 */
LOCAL BOOL write_out( CONST T_CFNET *n, CONST T_CFNET6 *n6 )
{
	ID	key;
	SZ	asz = 0;
	INT	len;
	ER	er, adm = E_NOEXS;

	if ( busy() > 0 && ui_ask(&ui, PN_BUSY, NULL, 0) != B_UPDATE ) {
		return FALSE;
	}
	len = cf_net_write(text, tlen, (INT)sizeof(text), n);
	if ( len >= 0 ) len = cf_net6_write(text, len, (INT)sizeof(text), n6);
	er = ( len < 0 ) ? E_LIMIT : conf_open(OB_OP_R | OB_OP_WRITE, &key);
	if ( er == E_OACV ) {
		/* the network's settings are the administrator's: the password asked for, by the desktop */
		adm = ts_admin_ask("ネットワーク設定の変更", &ui.win);
		er = ( adm >= E_OK ) ? conf_open(OB_OP_R | OB_OP_WRITE, &key) : E_OACV;
	}
	if ( er >= E_OK ) {
		/* replaced in one change: a cut in the power leaves the old record or the new one */
		er = ob_rpl_rec(key, 1, text, (SZ)len);
		(void)asz;
		ob_cls_obj(key);
	}
	if ( adm >= E_OK ) (void)ob_end_adm();
	if ( er == E_OACV ) {
		if ( adm != E_ABORT ) {
			CONST char *lines[3] = { "ネットワーク設定を変更する権限がありません。",
						 "管理者のパスワードが必要です。", "" };

			(void)ui_ask(&ui, PN_WARN, lines, 3);
		}
		tm_putstring((CONST UB *)"netenv: not written: no right\n");
		return FALSE;
	}
	if ( er < E_OK ) {
		char	why[64];
		CONST char *lines[3] = { "ネットワーク設定が保存できません。", why, "" };

		snprintf(why, sizeof(why), "（エラー %d）", (INT)er);
		(void)ui_ask(&ui, PN_WARN, lines, 3);
		return FALSE;
	}
	tlen = len;
	was = *n;
	was6 = *n6;
	to_sheet6(&was6);			/* the addresses as the record has them */
	tm_putstring((CONST UB *)"netenv: written\n");
	(void)tk_dly_tsk(500);			/* the system takes it as it sees it written */
	state_show();
	state6_show();
	return TRUE;
}

/* An address a machine may be given: not unspecified, loopback, link-local or multicast */
LOCAL BOOL unicast6( CONST UB *a )
{
	static CONST UB	lo[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };

	return (BOOL)!( cf_ip6zero(a) || memcmp(a, lo, 16) == 0 || a[0] == 0xff
		       || ( a[0] == 0xfe && ( a[1] & 0xc0 ) == 0x80 ) );
}

/* The sheet looked over before it is written: FALSE when something is wrong with it */
LOCAL BOOL check( CONST T_CFNET *n, CONST T_CFNET6 *n6, INT bad6 )
{
	if ( !plain(n->host) || !plain(n->dnsname[0]) || !plain(n->dnsname[1]) || !plain(n->domain) ) {
		warn("英数記号以外の文字は使用できません。");
		return FALSE;
	}
	if ( n->addr != 0 && n->mask == 0 ) {
		warn("サブネットマスクを設定してください。");
		return FALSE;
	}
	if ( bad6 != 0 ) {
		ui_show_sheet(&ui, 3);
		warn("ＩＰｖ６アドレスの書き方が正しくありません。");
		return FALSE;
	}
	if ( n6->on && n6->how == CF_IP6_FIXED && !unicast6(n6->addr) ) {
		ui_show_sheet(&ui, 3);
		warn(cf_ip6zero(n6->addr) ? "固定のＩＰｖ６アドレスを設定してください。"
					  : "このアドレスは固定アドレスに使用できません。");
		return FALSE;
	}
	return TRUE;
}

/* 設定を更新 [S]: written at once, the window left open */
LOCAL void update( void )
{
	T_CFNET		n;
	T_CFNET6	n6;
	INT		bad6;

	from_sheet(&n);
	bad6 = from_sheet6(&n6);
	if ( !check(&n, &n6, bad6) ) return;
	if ( same(&n, &was) && same6(&n6, &was6) ) {
		warn("内容は変更されていません。");
		return;
	}
	if ( ui_ask(&ui, PN_UPDATE, NULL, 0) != B_UPDATE ) return;
	(void)write_out(&n, &n6);
}

/* 終了 [E]: asked about what was changed, then closed */
LOCAL void finish( void )
{
	T_CFNET		n;
	T_CFNET6	n6;
	INT		a, bad6;

	from_sheet(&n);
	bad6 = from_sheet6(&n6);
	if ( bad6 == 0 && same(&n, &was) && same6(&n6, &was6) ) {
		closing = TRUE;
		return;
	}
	a = ui_ask(&ui, PN_FINISH, NULL, 0);
	if ( a == B_NOSAVE ) {
		closing = TRUE;
	} else if ( a == B_SAVE && check(&n, &n6, bad6) && write_out(&n, &n6) ) {
		closing = TRUE;
	}
}

/* ---------------------------------------------------------------- the Wi-Fi */

#define W_LINK_MS	1000		/* the signal and the rate read again, while 〈状態〉 is shown */
#define W_NTF_CARD	9		/* the id of the notices of the card's object */
#define W_NTF_DEVS	10		/* and of the list of the devices, until the card is there */

typedef struct {
	char	hex[72];		/* the SSID in hexadecimal */
	char	text[40];		/* as letters */
	INT	sec;			/* SEC_* */
	BOOL	autojoin;
} WSAVED;

/* The card's word of a security, and the selector's value of one (0 a word of none joined) */
LOCAL CONST char *w_sec_of( INT sec )
{
	return ( sec == SEC_WPA2 ) ? "wpa2" : ( sec == SEC_WPA3 ) ? "wpa3"
	     : ( sec == SEC_WPA2WPA3 ) ? "wpa2wpa3" : "open";
}

LOCAL INT w_sec_val( CONST char *w )
{
	return ( strcmp(w, "open") == 0 ) ? SEC_NONE : ( strcmp(w, "wpa2") == 0 ) ? SEC_WPA2
	     : ( strcmp(w, "wpa3") == 0 ) ? SEC_WPA3 : ( strcmp(w, "wpa2wpa3") == 0 ) ? SEC_WPA2WPA3 : 0;
}

/* A saved password's box: marks standing in for the key kept in the kernel */
LOCAL CONST char w_standin[] = { XB_STANDIN, XB_STANDIN, XB_STANDIN, XB_STANDIN,
				 XB_STANDIN, XB_STANDIN, XB_STANDIN, XB_STANDIN, 0 };

LOCAL TS_UUID	w_dev;			/* wlan0 */
LOCAL BOOL	w_have = FALSE;
/* A network the last scan found */
typedef struct {
	char	hex[72];		/* the SSID in hexadecimal */
	char	sec[12];
	char	row[128];		/* as the list shows it */
} WBSS;

LOCAL UB	*w_rec = NULL;		/* a record of the card's object, whole */
LOCAL WSAVED	*w_saved = NULL;	/* the networks saved, as many as the card has taken */
LOCAL INT	w_nsaved = 0;
LOCAL WBSS	*w_bss = NULL;		/* the networks of the scan, however many */
LOCAL INT	w_nbss = 0;
LOCAL char	(*w_saved_row)[56] = NULL;
LOCAL ID	w_kcard = 0;		/* the card's object, opened for its notices */
LOCAL ID	w_kdevs = 0;		/* the list of the devices, opened while the card is not there */
LOCAL char	*w_scan_said = NULL;	/* the scan's record as last shown */
LOCAL char	*w_saved_said = NULL;	/* the saved networks' names as last set */

LOCAL INT w_hexval( char c )
{
	return ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
	     : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
}

/* An SSID in hexadecimal as letters (its bytes, the control characters left out) */
LOCAL void w_unhex( CONST char *hex, char *out, INT max )
{
	INT	n = 0, h, l;

	while ( hex[0] != 0 && hex[1] != 0 && n < max - 1 ) {
		h = w_hexval(hex[0]);
		l = w_hexval(hex[1]);
		if ( h < 0 || l < 0 ) break;
		if ( h * 16 + l >= 0x20 ) out[n++] = (char)( h * 16 + l );
		hex += 2;
	}
	out[n] = 0;
}

LOCAL void w_hex( CONST char *s, char *out, INT max )
{
	static CONST char d[] = "0123456789abcdef";
	INT	n = 0;

	for ( ; *s != 0 && n + 2 < max; s++ ) {
		out[n++] = d[( (UB)*s ) >> 4];
		out[n++] = d[( (UB)*s ) & 15];
	}
	out[n] = 0;
}

/* The Wi-Fi card's object (wlan0), found once */
LOCAL BOOL w_find( void )
{
	TS_UUID		*ids = NULL;
	T_OBREF		r;
	INT		cnt, i;
	BOOL		found = FALSE;

	cnt = ui_lst_obj(OB_T_DEVICE, OB_S_NET, &ids);
	for ( i = 0; i < cnt && !found; i++ ) {
		if ( ob_ref_obj(&ids[i], &r) >= E_OK && strcmp((CONST char *)r.name, "wlan0") == 0 ) {
			w_dev = ids[i];
			found = TRUE;
		}
	}
	free(ids);
	return found;
}

/*
 * The card's object watched: a change of its records told to the
 * window's channel. Asked before the records are read, so that nothing
 * that changes after that read goes untold. While there is no card, the
 * list of the devices is watched instead, for the one that comes.
 */
LOCAL void w_watch( void )
{
	CONST TS_UUID	devlist = OB_UUID_DEVLIST_INIT;
	T_OBNTF		req;

	if ( !w_have ) w_have = w_find();
	memset(&req, 0, sizeof(req));
	if ( w_have && w_kcard <= 0 ) {
		w_kcard = ob_opn_obj(&w_dev, OB_OP_ATRRD);
		req.events = OB_E_CHANGE | OB_E_DELETE;
		req.id = W_NTF_CARD;
		if ( w_kcard > 0 && ob_ntf_evt(w_kcard, OB_REC_ANY, &req, ui.port) <= 0 ) {
			ob_cls_obj(w_kcard);
			w_kcard = 0;
		}
	}
	if ( w_have && w_kdevs > 0 ) {
		ob_cls_obj(w_kdevs);
		w_kdevs = 0;
	} else if ( !w_have && w_kdevs <= 0 ) {
		w_kdevs = ob_opn_obj(&devlist, OB_OP_ATRRD);
		req.events = OB_E_ATTACH;
		req.id = W_NTF_DEVS;
		if ( w_kdevs > 0 && ob_ntf_evt(w_kdevs, OB_REC_ANY, &req, ui.port) <= 0 ) {
			ob_cls_obj(w_kdevs);
			w_kdevs = 0;
		}
		/* a card that came before the list was watched */
		if ( !w_have && ( w_have = w_find() ) ) w_watch();
	}
}

/* A record of the card's object into w_rec, whole however long: its length, or an error */
LOCAL INT w_read( INT recno )
{
	UB	*b;
	INT	len = ui_rec_all(&w_dev, recno, 0, &b);

	if ( len < 0 ) return len;
	free(w_rec);
	w_rec = b;
	return len;
}

/* A command to the card (SCAN, CONNECT <SSID hex>, DISCONNECT) */
LOCAL ER w_cmd( CONST char *c )
{
	ID	key;
	SZ	asz = 0;
	ER	er;

	key = ob_opn_obj(&w_dev, OB_OP_R | OB_OP_WRITE);
	if ( key < E_OK ) return (ER)key;
	er = ob_wri_rec(key, OB_NET_STATE, 0, c, (SZ)strlen(c), &asz);
	ob_cls_obj(key);
	return er;
}

LOCAL char	w_cur_state[16];	/* the card's STATE as last read */
LOCAL char	w_cur_ssid[72];		/* the SSID (hexadecimal) being joined or joined, as last read */
LOCAL BOOL	w_sae = FALSE;		/* the card joins with WPA3 (its SAE line) */

/* The card cannot scan nor join: it did not start, has stopped, or is still starting */
LOCAL BOOL w_down( void )
{
	return strcmp(w_cur_state, "nofirmware") == 0 || strcmp(w_cur_state, "off") == 0
	    || strcmp(w_cur_state, "starting") == 0;
}

LOCAL CONST char *w_state_name( CONST char *v )
{
	return ( strcmp(v, "connected") == 0 ) ? "接続中"
	     : ( strcmp(v, "handshake") == 0 ) ? "認証中（鍵の交換）"
	     : ( strcmp(v, "joining") == 0 ) ? "接続しています"
	     : ( strcmp(v, "scanning") == 0 ) ? "走査中"
	     : ( strcmp(v, "idle") == 0 ) ? "未接続"
	     : ( strcmp(v, "starting") == 0 ) ? "準備中"
	     : ( strcmp(v, "nofirmware") == 0 ) ? "起動の失敗（ファームウェアを読めないか、カードが応答しません）" : "停止中";
}

LOCAL CONST char *w_reason_name( CONST char *v )
{
	return ( strcmp(v, "password") == 0 ) ? "（パスワードが違う可能性があります）"
	     : ( strcmp(v, "notfound") == 0 ) ? "（ネットワークが見つかりません）"
	     : ( strcmp(v, "unsupported") == 0 ) ? "（対応していない暗号方式です）"
	     : ( strcmp(v, "nowpa3") == 0 ) ? "（このＷｉ－ＦｉのカードはＷＰＡ３に対応していません）"
	     : ( strcmp(v, "timeout") == 0 ) ? "（応答がありません）"
	     : ( strcmp(v, "lost") == 0 ) ? "（接続が切れました）" : "";
}

/* The card's state on both pages, and the networks saved */
LOCAL void w_state( void )
{
	char	v[CF_VAL_MAX], ss[40], line[160], w[24];
	CONST char **names;
	INT	len, i, n, keep = ui_value(&ui, PW_SAVED);

	w_watch();
	if ( !w_have || ( len = w_read(OB_NET_STATE) ) <= 0 ) {
		ui_set_text(&ui, PW_STATE, "（Ｗｉ－Ｆｉの装置がありません）");
		ui_set_text(&ui, PW_ST_STATE, "（Ｗｉ－Ｆｉの装置がありません）");
		return;
	}
	(void)cf_get(w_rec, len, "STATE", 0, v, sizeof(v));
	if ( strcmp(w_cur_state, v) != 0 ) {
		snprintf(line, sizeof(line), "netenv: wifi state %s\n", v);
		tm_putstring((CONST UB *)line);
	}
	snprintf(w_cur_state, sizeof(w_cur_state), "%s", v);
	w_sae = ( cf_get(w_rec, len, "SAE", 0, v, sizeof(v)) && strcmp(v, "yes") == 0 );
	snprintf(line, sizeof(line), "%s", w_state_name(w_cur_state));	/* v holds the SAE line's word now */
	ui_set_text(&ui, PW_ST_STATE, line);
	w_cur_ssid[0] = '\0';
	if ( cf_get(w_rec, len, "SSID", 0, v, sizeof(v)) ) {
		snprintf(w_cur_ssid, sizeof(w_cur_ssid), "%s", v);
		w_unhex(v, ss, sizeof(ss));
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "「%s」", ss);
		ui_set_text(&ui, PW_ST_SSID, ss);
	} else {
		ui_set_text(&ui, PW_ST_SSID, "（なし）");
	}
	if ( cf_get(w_rec, len, "REASON", 0, v, sizeof(v)) ) {
		snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s", w_reason_name(v));
	}
	ui_set_text(&ui, PW_STATE, line);
	ui_set_text(&ui, PW_ST_BSSID, cf_get(w_rec, len, "BSSID", 0, v, sizeof(v)) ? v : "（なし）");
	if ( cf_get(w_rec, len, "FREQ", 0, v, sizeof(v)) ) {
		i = cf_num(v, 0);
		snprintf(line, sizeof(line), "%d MHz（%s帯、チャネル %d）", i, ( i >= 5000 ) ? "5GHz" : "2.4GHz",
			 ( i >= 5000 ) ? ( i - 5000 ) / 5 : ( i == 2484 ) ? 14 : ( i - 2407 ) / 5);
		ui_set_text(&ui, PW_ST_FREQ, line);
	} else {
		ui_set_text(&ui, PW_ST_FREQ, "（なし）");
	}
	if ( cf_get(w_rec, len, "SIGNAL", 0, v, sizeof(v)) ) {
		snprintf(line, sizeof(line), "%s dBm", v);
		ui_set_text(&ui, PW_ST_SIGNAL, line);
	} else {
		ui_set_text(&ui, PW_ST_SIGNAL, "（なし）");
	}
	if ( cf_get(w_rec, len, "RATE", 0, v, sizeof(v)) && cf_num(v, 0) > 0 ) {
		snprintf(line, sizeof(line), "%s Mb/s", v);
		ui_set_text(&ui, PW_ST_RATE, line);
	} else {
		ui_set_text(&ui, PW_ST_RATE, "（なし）");
	}
	ui_set_text(&ui, PW_ST_SEC, !cf_get(w_rec, len, "SECURITY", 0, v, sizeof(v)) ? "（なし）"
		    : ( strcmp(v, "wpa2") == 0 ) ? "ＷＰＡ２－Ｐｅｒｓｏｎａｌ"
		    : ( strcmp(v, "wpa3") == 0 ) ? "ＷＰＡ３－Ｐｅｒｓｏｎａｌ" : "なし");
	ui_set_text(&ui, PW_ST_MAC, cf_get(w_rec, len, "MAC", 0, v, sizeof(v)) ? v : "");

	/* the networks saved, as the card has taken them, however many */
	n = key_lines(w_rec, len, "SAVED");
	free(w_saved);
	free(w_saved_row);
	w_nsaved = 0;
	w_saved = (WSAVED *)calloc((size_t)( n + 1 ), sizeof(WSAVED));
	w_saved_row = malloc(sizeof(*w_saved_row) * (size_t)( n + 1 ));
	names = (CONST char **)malloc(sizeof(char *) * (size_t)( n + 1 ));
	if ( w_saved == NULL || w_saved_row == NULL || names == NULL ) {
		free((void *)names);
		return;
	}
	for ( i = 0; w_nsaved < n && cf_get(w_rec, len, "SAVED", i, v, sizeof(v)); i++ ) {
		WSAVED	*p = &w_saved[w_nsaved];

		if ( !cf_word(v, 0, p->hex, sizeof(p->hex)) ) continue;
		w_unhex(p->hex, p->text, sizeof(p->text));
		p->sec = cf_word(v, 1, w, sizeof(w)) ? w_sec_val(w) : 0;
		if ( p->sec == 0 ) p->sec = SEC_NONE;
		p->autojoin = !( cf_word(v, 2, w, sizeof(w)) && strcmp(w, "manual") == 0 );
		w_nsaved++;
	}
	strcpy(w_saved_row[0], "（新しい接続）");
	names[0] = w_saved_row[0];
	for ( i = 0; i < w_nsaved; i++ ) {
		snprintf(w_saved_row[i + 1], sizeof(w_saved_row[0]), "%s", w_saved[i].text);
		names[i + 1] = w_saved_row[i + 1];
	}
	/*
	 * the names set again only when they are other than shown: the card is
	 * read again as it changes, and every second on 〈状態〉, and the part's
	 * list may be open
	 */
	{
		size_t	n = 1;
		char	*all;

		for ( i = 0; i <= w_nsaved; i++ ) n += strlen(names[i]) + 1;
		all = (char *)malloc(n);
		if ( all != NULL ) {
			all[0] = 0;
			for ( i = 0; i <= w_nsaved; i++ ) {
				strcat(all, names[i]);
				strcat(all, "\n");
			}
		}
		if ( all == NULL || w_saved_said == NULL || strcmp(all, w_saved_said) != 0 ) {
			ui_names(&ui, PW_SAVED, names, w_nsaved + 1);
			ui_set_value(&ui, PW_SAVED, ( keep >= 1 && keep <= w_nsaved + 1 ) ? keep : 1);
			free(w_saved_said);
			w_saved_said = all;
		} else {
			free(all);
		}
	}
	free((void *)names);
}

LOCAL CONST char *w_sec_word( CONST char *s )
{
	return ( strcmp(s, "open") == 0 ) ? "なし" : ( strcmp(s, "wpa2") == 0 ) ? "WPA2"
	     : ( strcmp(s, "wpa3") == 0 ) ? ( w_sae ? "WPA3" : "WPA3(未対応)" )
	     : ( strcmp(s, "wpa2wpa3") == 0 ) ? ( w_sae ? "WPA2/WPA3" : "WPA2" )
	     : ( strcmp(s, "wpa") == 0 ) ? "WPA(未対応)"
	     : ( strcmp(s, "wep") == 0 ) ? "WEP(未対応)" : "802.1X(未対応)";
}

/* The networks the last scan found, the strongest first, however many */
LOCAL void w_scan_show( void )
{
	CONST char **names;
	CONST char *none;
	char	v[CF_VAL_MAX], w[5][72], ss[40];
	INT	len, i, k, n;

	if ( !w_have || ( len = w_read(OB_NET_SCAN) ) < 0 ) return;
	if ( w_scan_said != NULL && strcmp(w_scan_said, (CONST char *)w_rec) == 0 && w_nbss > 0 ) return;
	free(w_scan_said);
	w_scan_said = strdup((CONST char *)w_rec);
	n = key_lines(w_rec, len, "BSS");
	free(w_bss);
	w_nbss = 0;
	w_bss = (WBSS *)malloc(sizeof(WBSS) * (size_t)( n + 1 ));
	names = (CONST char **)malloc(sizeof(char *) * (size_t)( n + 1 ));
	if ( w_bss == NULL || names == NULL ) {
		free((void *)names);
		return;
	}
	for ( i = 0; i < n && cf_get(w_rec, len, "BSS", i, v, sizeof(v)); i++ ) {
		for ( k = 0; k < 5; k++ ) {
			if ( !cf_word(v, k, w[k], sizeof(w[k])) ) w[k][0] = 0;
		}
		if ( w[4][0] == 0 ) continue;		/* a hidden network */
		for ( k = 0; k < w_nbss && strcmp(w_bss[k].hex, w[4]) != 0; k++ ) ;
		if ( k < w_nbss ) continue;		/* another access point of one already listed */
		w_unhex(w[4], ss, sizeof(ss));
		snprintf(w_bss[w_nbss].hex, sizeof(w_bss[0].hex), "%s", w[4]);
		snprintf(w_bss[w_nbss].sec, sizeof(w_bss[0].sec), "%s", w[3]);
		snprintf(w_bss[w_nbss].row, sizeof(w_bss[0].row), "%s\t%s dBm\t%s", ss, w[2], w_sec_word(w[3]));
		names[w_nbss] = w_bss[w_nbss].row;
		w_nbss++;
	}
	{
		char	m[64];

		snprintf(m, sizeof(m), "netenv: scan %d networks, %d shown\n", n, w_nbss);
		tm_putstring((CONST UB *)m);
	}
	if ( w_nbss == 0 ) {
		none = !w_down() ? "（見つかったネットワークはありません）"
		       : ( strcmp(w_cur_state, "starting") == 0 ) ? "（カードの準備中です）"
		       : "（カードが動いていないので走査できません）";
		ui_names(&ui, PW_SCAN_LIST, &none, 1);
	} else {
		ui_names(&ui, PW_SCAN_LIST, names, w_nbss);
	}
	ui_set_value(&ui, PW_SCAN_LIST, 0);
	free((void *)names);
}

/* The parts not to be worked when there is no card */
LOCAL void w_parts( void )
{
	CONST INT	ids[] = { PW_SAVED, PW_SSID, PW_SCAN_LIST, PW_SEC, PW_PASS, PW_AUTO, PW_SCAN,
				  PW_SAVE, PW_DEL, PW_JOIN, PW_LEAVE };
	INT		i;

	for ( i = 0; i < (INT)( sizeof(ids) / sizeof(ids[0]) ); i++ ) ui_off(&ui, ids[i], !w_have);
	if ( w_have && w_down() ) {
		ui_off(&ui, PW_SCAN, TRUE);		/* nothing to scan or join with */
		ui_off(&ui, PW_JOIN, TRUE);
	}
	if ( w_have ) ui_off(&ui, PW_PASS, ui_value(&ui, PW_SEC) <= SEC_NONE);
}

/*
 * A saved network chosen: its SSID, security and whether it joins by
 * itself. A network with a password shows marks standing in for it:
 * the password itself is the kernel's, and never comes here.
 */
LOCAL void w_pick_saved( void )
{
	INT	k = ui_value(&ui, PW_SAVED) - 2;

	if ( k < 0 || k >= w_nsaved ) {
		ui_set_text(&ui, PW_PASS, "");
		ui_set_text(&ui, PW_SSID, "");
		ui_set_value(&ui, PW_SEC, SEC_WPA2);
		ui_set_value(&ui, PW_AUTO, 1);
	} else {
		ui_set_text(&ui, PW_PASS, ( w_saved[k].sec > SEC_NONE ) ? w_standin : "");
		ui_set_text(&ui, PW_SSID, w_saved[k].text);
		ui_set_value(&ui, PW_SEC, w_saved[k].sec);
		ui_set_value(&ui, PW_AUTO, w_saved[k].autojoin ? 1 : 0);
	}
	w_parts();
}

/* The saved network of an SSID (hexadecimal); -1 none */
LOCAL INT w_saved_of( CONST char *hex )
{
	INT	i;

	for ( i = 0; i < w_nsaved; i++ ) {
		if ( strcmp(w_saved[i].hex, hex) == 0 ) return i;
	}
	return -1;
}

/* A network of the scan chosen: its SSID and security into the parts, and the saved one of it */
LOCAL void w_pick_bss( void )
{
	INT	k = ui_value(&ui, PW_SCAN_LIST) - 1, i;
	char	ss[40];

	if ( k < 0 || k >= w_nbss ) return;
	i = w_saved_of(w_bss[k].hex);
	if ( i >= 0 ) {
		ui_set_value(&ui, PW_SAVED, i + 2);
		w_pick_saved();
		tm_putstring((CONST UB *)"netenv: saved network chosen\n");
	} else {
		ui_set_value(&ui, PW_SAVED, 1);
		ui_set_text(&ui, PW_PASS, "");
	}
	w_unhex(w_bss[k].hex, ss, sizeof(ss));
	ui_set_text(&ui, PW_SSID, ss);
	i = w_sec_val(w_bss[k].sec);
	if ( i == SEC_WPA2WPA3 && !w_sae ) i = SEC_WPA2;	/* a card with no SAE joins it with WPA2 */
	ui_set_value(&ui, PW_SEC, ( i > 0 ) ? i : SEC_WPA2);
	w_parts();
	if ( i == SEC_WPA3 && !w_sae ) {
		warn("このＷｉ－ＦｉのカードはＷＰＡ３に対応していません。");
	} else if ( i == 0 ) {
		warn("このネットワークの暗号方式には対応していません。");
	}
}

/* A change of the saved networks asked of the card's manager (SAVE, FORGET): what it answered */
LOCAL ER w_change( CONST char *line )
{
	ER	er = w_cmd(line);
	char	m[64];

	snprintf(m, sizeof(m), ( er >= E_OK ) ? "netenv: wifi changed\n" : "netenv: wifi not changed: %d\n", (INT)er);
	tm_putstring((CONST UB *)m);
	return er;
}

/*
 * The parts' network saved (and joined, when join); FALSE when something
 * is wrong with it. The marks standing in for a saved password, left as
 * they are, keep the network's key; a password typed replaces it. A
 * network joined as it was saved is only joined.
 */
LOCAL BOOL w_save( BOOL join )
{
	char	ssid[CF_VAL_MAX], hex[72], pass[72], line[256], c[96];
	INT	sec = ui_value(&ui, PW_SEC);
	BOOL	keyed = ( sec > SEC_NONE ), autojoin = ( ui_value(&ui, PW_AUTO) != 0 );
	BOOL	sae = ( sec == SEC_WPA3 || sec == SEC_WPA2WPA3 );
	BOOL	kept, hexkey = TRUE;
	INT	i, plen, k;
	ER	er;

	(void)ui_text(&ui, PW_SSID, ssid, sizeof(ssid));
	(void)ui_text(&ui, PW_PASS, pass, sizeof(pass));
	kept = ( (UB)pass[0] == XB_STANDIN );
	plen = kept ? 0 : (INT)strlen(pass);
	if ( ssid[0] == 0 ) {
		warn("ＳＳＩＤを入れてください。");
		return FALSE;
	}
	if ( strlen(ssid) > 32 ) {
		warn("ＳＳＩＤが長すぎます（３２バイトまで）。");
		return FALSE;
	}
	w_hex(ssid, hex, sizeof(hex));
	k = w_saved_of(hex);
	if ( keyed && ( kept ? ( k < 0 || w_saved[k].sec <= SEC_NONE ) : plen == 0 ) ) {
		memset(pass, 0, sizeof(pass));
		warn("パスワードを入れてください。");
		return FALSE;
	}
	for ( i = 0; i < plen; i++ ) {
		if ( (UB)pass[i] < 0x20 || (UB)pass[i] > 0x7e ) {
			memset(pass, 0, sizeof(pass));
			warn("パスワードに使用できない文字があります。");
			return FALSE;
		}
		if ( w_hexval(pass[i]) < 0 ) hexkey = FALSE;
	}
	/* WPA2's PSK may be 64 hexadecimal digits; SAE takes a password as it is */
	if ( keyed && !kept && ( plen < 8 || plen > 63 ) && !( plen == 64 && hexkey && !sae ) ) {
		memset(pass, 0, sizeof(pass));
		warn("パスワードは８文字から６３文字です。");
		return FALSE;
	}
	if ( join && k >= 0 && ( kept || !keyed ) && w_saved[k].sec == sec && w_saved[k].autojoin == autojoin ) {
		tm_putstring((CONST UB *)"netenv: wifi joined as saved\n");
	} else {
		snprintf(line, sizeof(line), "SAVE %s %s %s%s%s", hex, w_sec_of(sec),
			 autojoin ? "auto" : "manual", ( keyed && !kept ) ? " " : "", ( keyed && !kept ) ? pass : "");
		er = w_change(line);
		memset(line, 0, sizeof(line));
		memset(pass, 0, sizeof(pass));
		if ( er < E_OK ) {
			/* E_NOEXS: the key the security needs is not kept (WPA3's password for a network saved as WPA2) */
			warn(( er == E_PAR ) ? "パスワードは８文字から６３文字です。"
			     : ( er == E_NOEXS ) ? "パスワードを入れてください。" : "Ｗｉ－Ｆｉの設定が保存できません。");
			return FALSE;
		}
		w_state();
		k = w_saved_of(hex);
		if ( k >= 0 ) {
			ui_set_value(&ui, PW_SAVED, k + 2);
			w_pick_saved();
		}
	}
	memset(pass, 0, sizeof(pass));
	ui_set_text(&ui, PW_PASS, keyed ? w_standin : "");
	if ( join ) {
		w_state();
		if ( strcmp(w_cur_state, "connected") == 0 && strcmp(w_cur_ssid, hex) == 0 ) {
			/* that network is joined already: nothing is asked of the card */
			tm_putstring((CONST UB *)"netenv: that network is joined already\n");
			return TRUE;
		}
		snprintf(c, sizeof(c), "CONNECT %s", hex);
		if ( w_cmd(c) < E_OK ) warn("Ｗｉ－Ｆｉの装置に接続を指示できません。");
	}
	return TRUE;
}

LOCAL void w_delete( void )
{
	INT	k = ui_value(&ui, PW_SAVED) - 2;
	char	line[128];

	if ( k < 0 || k >= w_nsaved ) {
		warn("削除する接続を選んでください。");
		return;
	}
	snprintf(line, sizeof(line), "FORGET %s", w_saved[k].hex);
	if ( w_change(line) >= E_OK ) {
		w_state();
		ui_set_value(&ui, PW_SAVED, 1);
		w_pick_saved();
	} else {
		warn("接続を削除できません。");
	}
}

LOCAL void w_button( INT id )
{
	switch ( id ) {
	case PW_SCAN:
		if ( w_cmd("SCAN") < E_OK ) warn("Ｗｉ－Ｆｉの装置に走査を指示できません。");
		if ( w_scan_said != NULL ) w_scan_said[0] = 0;
		break;
	case PW_SAVE:
		(void)w_save(FALSE);
		break;
	case PW_DEL:
		w_delete();
		break;
	case PW_JOIN:
		(void)w_save(TRUE);
		break;
	case PW_LEAVE:
		if ( w_cmd("DISCONNECT") < E_OK ) warn("Ｗｉ－Ｆｉの装置に切断を指示できません。");
		break;
	case PW_ST_AGAIN:
		w_state();
		break;
	default:
		break;
	}
}

/*
 * The card read again, while a page of 〈Ｗｉ－Ｆｉ〉 is shown: its state,
 * and, on 〈接続〉, the networks of the scan when scan
 */
LOCAL void w_again( BOOL scan )
{
	INT	sh = ui_sheet(&ui);
	BOOL	was = w_down();

	if ( sh != TAG_WIFI && sh != TAG_WIFI_ST ) return;
	w_state();
	if ( w_down() != was ) w_parts();	/* the card came up or went down */
	if ( scan && sh == TAG_WIFI ) w_scan_show();
}

/*
 * A notice of the card's object or of the list of the devices: TRUE
 * when it was one. What changed is read again whole, so a notice that
 * finds nothing new costs only the read, and notices lost on the way
 * (lost) are made up for by reading everything.
 */
LOCAL BOOL w_told( CONST T_OBNTM *m )
{
	if ( m->id == W_NTF_DEVS ) {
		if ( !w_have ) {
			w_watch();
			if ( w_have ) {
				w_again(TRUE);
				w_parts();
			}
		}
		return TRUE;
	}
	if ( m->id != W_NTF_CARD ) return FALSE;
	if ( m->event == OB_E_DELETE ) {
		/* the card is gone: its object no more, the list watched for the next */
		ob_cls_obj(w_kcard);
		w_kcard = 0;
		w_have = FALSE;
		w_watch();
		w_again(TRUE);
		w_parts();
	} else if ( m->event == OB_E_CHANGE ) {
		w_again(m->lost > 0 || m->recno == OB_NET_SCAN);
	}
	return TRUE;
}

/* ---------------------------------------------------------------- the menu */

LOCAL BOOL is_code( CONST T_MNSEL *sel, CONST char *code )
{
	return (BOOL)( strcmp((CONST char *)sel->code, code) == 0 );
}

LOCAL void menu( CONST T_OBNTM *m )
{
	TS_UUID	def;
	T_MNSEL	sel;
	ID	mid;
	BOOL	chosen;

	if ( ui_uuid(SYSDEF_MENU_NETENV, &def) < E_OK || mn_cre_men(&def, &mid) < E_OK ) return;
	if ( ui.last == 0 ) {
		(void)mn_chg_atr(mid, NULL, "clear", MN_GREY);
	}
	chosen = (BOOL)( mn_pop_men(mid, ui.kw, m->x, m->y, m->when, &sel) >= E_OK );
	(void)mn_del_men(mid);
	if ( !chosen ) return;
	if ( is_code(&sel, "close") ) {
		finish();
	} else if ( is_code(&sel, "update") ) {
		update();
	} else if ( is_code(&sel, "clear") && ui.last != 0 ) {
		/* what was typed into last, cleared */
		if ( ui.last == P_HOST || ui.last == P_DNS1 || ui.last == P_DNS2 || ui.last == P_DOM
		  || ui.last == P6_ADDR || ui.last == P6_GW || ui.last == P6_DNS1 || ui.last == P6_DNS2 ) {
			ui_set_text(&ui, ui.last, "");
		} else if ( ui.last == P6_PLEN ) {
			INT	v = 64;

			ui_set_fields(&ui, P6_PLEN, &v, 1);
		} else {
			ui_set_ip(&ui, ui.last, 0);
		}
	}
}

LOCAL void key( CONST T_OBNTM *m )
{
	UB	c = wm_key_char(m->code, m->mods);

	if ( c == 'e' || c == 'E' ) finish();
	else if ( c == 's' || c == 'S' ) update();
}

int main( void )
{
	T_OBNTM	m;
	INT	id, ev;
	UD	link_at;

	ui_icon(SYSDEF_PROG_NETENV);
	if ( ui_open(&ui, SYSDEF_BOX_NETENV, PN_MAIN, 200, 120) < E_OK ) {
		tm_putstring((CONST UB *)"netenv: no window\n");
		return 1;
	}
	if ( conf_read() < E_OK ) {
		warn("ネットワーク設定が読み込めません。");
		ui_close(&ui);
		return 2;
	}
	to_sheet(&was);
	to_sheet6(&was6);
	state_show();
	state6_show();
	w_state();
	w_scan_show();
	ui_set_value(&ui, PW_SEC, SEC_WPA2);
	ui_set_value(&ui, PW_AUTO, 1);
	w_parts();
	tm_putstring((CONST UB *)"netenv: ready\n");

	link_at = ui_now() + W_LINK_MS;
	while ( !closing ) {
		/* until something is told, or, on 〈状態〉, the next read of the signal and the rate */
		TMO	wait = ( ui_sheet(&ui) == TAG_WIFI_ST ) ? ui_left(link_at) : TMO_FEVR;

		for ( TMO tmo = wait; !closing && ui_wait(&ui, &m, tmo) >= E_OK; tmo = TMO_POL ) {
			if ( w_told(&m) ) continue;
			ev = ui_event(&ui, &m, &id);
			switch ( ev ) {
			case UI_EV_CLOSE:
				if ( m.event == OB_E_DELETE ) closing = TRUE;
				else finish();
				break;
			case UI_EV_MENU:
				menu(&m);
				break;
			case UI_EV_KEY:
				if ( m.mods & ( 0x01 | 0x10 ) ) key(&m);
				break;
			case UI_EV_BUTTON:
				if ( id == P_ST_AGAIN ) state_show();
				if ( id == P6_ST_AGAIN ) state6_show();
				w_button(id);
				break;
			case UI_EV_SHEET:
				if ( id == TAG_STATE ) state_show();
				if ( id == TAG_STATE6 ) state6_show();
				if ( id == TAG_WIFI || id == TAG_WIFI_ST ) {
					link_at = ui_now() + W_LINK_MS;
					w_state();
					w_scan_show();
					w_parts();
				}
				break;
			case UI_EV_CHANGE:
				if ( id == P_GW_IP && ui_value(&ui, P_GW) == 0 ) {
					ui_set_value(&ui, P_GW, 1);	/* an address given: the gateway is used */
				}
				if ( id == P6_ON || id == P6_HOW ) parts6();
				if ( id == PW_SAVED ) {
					w_pick_saved();
					if ( ui_value(&ui, PW_SAVED) >= 2 ) tm_putstring((CONST UB *)"netenv: saved network chosen\n");
				}
				if ( id == PW_SCAN_LIST ) w_pick_bss();
				if ( id == PW_SEC ) w_parts();
				break;
			default:
				break;
			}
		}
		if ( !closing && ui_sheet(&ui) == TAG_WIFI_ST && ui_left(link_at) == 0 ) {
			link_at = ui_now() + W_LINK_MS;
			w_again(FALSE);
		}
	}
	if ( w_kcard > 0 ) ob_cls_obj(w_kcard);
	if ( w_kdevs > 0 ) ob_cls_obj(w_kdevs);
	ui_close(&ui);
	tm_putstring((CONST UB *)"netenv: closed\n");
	return 0;
}
