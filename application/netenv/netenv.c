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
#include <ts/ui.h>
#include <ts/conf.h>
#include <ts/sysdef.h>
#include <ts/uuid.h>

#define TICK_MS		20

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

/* Its panels: the window's, and those it asks with */
#define PN_MAIN		1
#define PN_WARN		10
#define PN_UPDATE	11
#define PN_FINISH	12
#define PN_BUSY		13
#define B_UPDATE	103
#define B_NOSAVE	104
#define B_SAVE		105

#define TAG_STATE	2		/* 〈状態〉 */

LOCAL UI	ui;
LOCAL T_CFNET	was;			/* as the settings stood when read or written */
LOCAL UB	text[CF_TEXT_MAX];	/* the record */
LOCAL INT	tlen;
LOCAL BOOL	closing;

/* The system object's record of the network, as its lines; the length, or an error */
LOCAL UB	net_rec[2048];

LOCAL INT net_read( void )
{
	TS_UUID	u;
	ID	key;
	SZ	asz = 0;
	ER	er;

	if ( ui_uuid(SYSDEF_SYSTEM, &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, OB_OP_R);
	if ( key < E_OK ) return (INT)key;
	er = ob_rea_rec(key, OB_SYS_NET, 0, net_rec, sizeof(net_rec), &asz);
	ob_cls_obj(key);
	return ( er < E_OK ) ? (INT)er : (INT)asz;
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
	INT	len = net_read();

	if ( len <= 0 ) {
		ui_set_text(&ui, P_ST_ADDR, "（ネットワークがありません）");
		return;
	}
	state_line(P_ST_ADDR, len, "ADDRESS", "（まだありません）");
	state_line(P_ST_MASK, len, "MASK", "（なし）");
	state_line(P_ST_GW, len, "GATEWAY", "（なし）");
	state_line(P_ST_DNS, len, "DNS", "（なし）");
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

	if ( ui_uuid(SYSDEF_CONF_NET, &u) < E_OK ) return E_SYS;
	key = ob_opn_obj(&u, ops);
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

LOCAL BOOL same( CONST T_CFNET *a, CONST T_CFNET *b )
{
	return (BOOL)( strcmp(a->host, b->host) == 0 && a->addr == b->addr && a->mask == b->mask
		    && a->usegw == b->usegw && ( !a->usegw || a->gw == b->gw )
		    && strcmp(a->dnsname[0], b->dnsname[0]) == 0 && strcmp(a->dnsname[1], b->dnsname[1]) == 0
		    && a->dns[0] == b->dns[0] && a->dns[1] == b->dns[1]
		    && strcmp(a->domain, b->domain) == 0 && strcmp(a->ntp, b->ntp) == 0 );
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
LOCAL BOOL write_out( CONST T_CFNET *n )
{
	ID	key;
	SZ	asz = 0;
	INT	len;
	ER	er;

	if ( busy() > 0 && ui_ask(&ui, PN_BUSY, NULL, 0) != B_UPDATE ) {
		return FALSE;
	}
	len = cf_net_write(text, tlen, (INT)sizeof(text), n);
	er = ( len < 0 ) ? E_LIMIT : conf_open(OB_OP_R | OB_OP_WRITE, &key);
	if ( er >= E_OK ) {
		er = ob_wri_rec(key, 1, 0, text, len, &asz);
		if ( er >= E_OK ) er = ob_trn_rec(key, 1, (UD)len);
		ob_cls_obj(key);
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
	tm_putstring((CONST UB *)"netenv: written\n");
	(void)tk_dly_tsk(500);			/* the system takes it as it sees it written */
	state_show();
	return TRUE;
}

/* The sheet looked over before it is written: FALSE when something is wrong with it */
LOCAL BOOL check( CONST T_CFNET *n )
{
	if ( !plain(n->host) || !plain(n->dnsname[0]) || !plain(n->dnsname[1]) || !plain(n->domain) ) {
		warn("英数記号以外の文字は使用できません。");
		return FALSE;
	}
	if ( n->addr != 0 && n->mask == 0 ) {
		warn("サブネットマスクを設定してください。");
		return FALSE;
	}
	return TRUE;
}

/* 設定を更新 [S]: written at once, the window left open */
LOCAL void update( void )
{
	T_CFNET	n;

	from_sheet(&n);
	if ( !check(&n) ) return;
	if ( same(&n, &was) ) {
		warn("内容は変更されていません。");
		return;
	}
	if ( ui_ask(&ui, PN_UPDATE, NULL, 0) != B_UPDATE ) return;
	(void)write_out(&n);
}

/* 終了 [E]: asked about what was changed, then closed */
LOCAL void finish( void )
{
	T_CFNET	n;
	INT	a;

	from_sheet(&n);
	if ( same(&n, &was) ) {
		closing = TRUE;
		return;
	}
	a = ui_ask(&ui, PN_FINISH, NULL, 0);
	if ( a == B_NOSAVE ) {
		closing = TRUE;
	} else if ( a == B_SAVE && check(&n) && write_out(&n) ) {
		closing = TRUE;
	}
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
		if ( ui.last == P_HOST || ui.last == P_DNS1 || ui.last == P_DNS2 || ui.last == P_DOM ) {
			ui_set_text(&ui, ui.last, "");
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
	SZ	asz = 0;
	INT	id, ev;

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
	state_show();
	tm_putstring((CONST UB *)"netenv: ready\n");

	while ( !closing ) {
		while ( !closing && ob_rea_rec(ui.port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
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
				break;
			case UI_EV_SHEET:
				if ( id == TAG_STATE ) state_show();
				break;
			case UI_EV_CHANGE:
				if ( id == P_GW_IP && ui_value(&ui, P_GW) == 0 ) {
					ui_set_value(&ui, P_GW, 1);	/* an address given: the gateway is used */
				}
				break;
			default:
				break;
			}
		}
		if ( !closing ) {
			(void)tk_dly_tsk(TICK_MS);
		}
	}
	ui_close(&ui);
	tm_putstring((CONST UB *)"netenv: closed\n");
	return 0;
}
