/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ui.c
 *	A program's window of panels from a data box (design 16.5.23)
 *
 *	The window and its panel are objects (design 18.13): the panel is
 *	made from its definition in the data box, placed on the window, and
 *	its parts opened by the links it keeps to them. What happens to a
 *	part is told to the panel on the part's link record, so the program
 *	asks the panel once. Everything is drawn and worked by the window
 *	manager; here the parts are only read and written.
 */

#include <string.h>
#include <stdio.h>
#include <ts/ui.h>
#include <ts/part.h>

/* ---------------------------------------------------------------- metadata */

/* The text after "key": in a JSON object, a number or a string */
LOCAL CONST char *json_at( CONST char *j, CONST char *key )
{
	char		k[32];
	CONST char	*p;

	snprintf(k, sizeof(k), "\"%s\":", key);
	p = strstr(j, k);
	return ( p != NULL ) ? p + strlen(k) : NULL;
}

LOCAL INT json_num( CONST char *j, CONST char *key, INT dflt )
{
	CONST char *p = json_at(j, key);
	INT	v = 0;
	BOOL	neg = FALSE;

	if ( p == NULL ) return dflt;
	while ( *p == ' ' ) p++;
	if ( *p == '-' ) {
		neg = TRUE;
		p++;
	}
	if ( *p < '0' || *p > '9' ) return dflt;
	while ( *p >= '0' && *p <= '9' ) v = v * 10 + ( *p++ - '0' );
	return neg ? -v : v;
}

LOCAL void json_str( CONST char *j, CONST char *key, char *out, INT max )
{
	CONST char *p = json_at(j, key);
	INT	n = 0;

	out[0] = 0;
	if ( p == NULL ) return;
	while ( *p == ' ' ) p++;
	if ( *p++ != '"' ) return;
	while ( *p != 0 && *p != '"' && n < max - 1 ) {
		if ( *p == '\\' && p[1] != 0 ) p++;
		out[n++] = *p++;
	}
	out[n] = 0;
}

LOCAL INT hexv( char c )
{
	if ( c >= '0' && c <= '9' ) return c - '0';
	if ( c >= 'a' && c <= 'f' ) return c - 'a' + 10;
	if ( c >= 'A' && c <= 'F' ) return c - 'A' + 10;
	return -1;
}

EXPORT ER ui_uuid( CONST char *s, TS_UUID *u )
{
	INT	i = 0, k = 0, h, l;

	while ( k < 16 ) {
		if ( s[i] == '-' ) {
			i++;
			continue;
		}
		h = hexv(s[i]);
		l = ( h >= 0 ) ? hexv(s[i + 1]) : -1;
		if ( l < 0 ) return E_PAR;
		u->b[k++] = (UB)( h * 16 + l );
		i += 2;
	}
	return E_OK;
}

/* ---------------------------------------------------------------- panels */

/* A panel of the box made and placed on the window at (x, y) of its work area */
LOCAL ER panel_make( UI *u, INT num, INT x, INT y, TS_UUID *p_pnl, ID *p_key )
{
	T_OBCRE	c;
	T_OBMAP	m;
	char	json[160], bs[40];
	ID	key;
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		snprintf(bs + i * 2, 3, "%02x", u->box.b[i]);
	}
	snprintf(json, sizeof(json), "{\"tessronos\":{\"part\":{\"sub\":\"panel\",\"box\":\"%.8s-%.4s-%.4s-%.4s-%.12s\",\"num\":%d}}}",
		 bs, bs + 8, bs + 12, bs + 16, bs + 20, num);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_PANEL;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, p_pnl) < E_OK ) return E_OBJ;
	key = ob_opn_obj(p_pnl, OB_OP_ALL);
	if ( key < E_OK ) {
		(void)ob_del_obj(p_pnl);
		return (ER)key;
	}
	*p_key = key;
	if ( u->kw > 0 ) {
		memset(&m, 0, sizeof(m));
		m.x = x;
		m.y = y;
		if ( ob_map_rec(key, 0, u->kw, &m) < E_OK ) {
			ob_cls_obj(key);
			(void)ob_del_obj(p_pnl);
			return E_OBJ;
		}
	}
	return E_OK;
}

/* A placed panel's parts opened by the links it keeps, and known by number */
LOCAL void parts_open( UI *u )
{
	TS_UUID	pu;
	SZ	asz = 0;
	char	j[512];
	INT	i;

	u->np = 0;
	u->tags = 0;
	for ( i = 0; i < UI_PART_MAX; i++ ) {
		UIPART	*p = &u->p[i];

		if ( ob_rea_rec(u->pk, OB_WR_KIND + i, 0, &pu, sizeof(pu), &asz) < E_OK
		  || asz < (SZ)sizeof(pu) ) {
			break;
		}
		p->key = ob_opn_obj(&pu, OB_OP_ALL);
		p->num = 0;
		p->kind = 0;
		if ( p->key > 0 && ob_get_atr(p->key, (UB *)j, sizeof(j) - 1, &asz) >= E_OK ) {
			j[( asz < (SZ)sizeof(j) ) ? asz : (SZ)sizeof(j) - 1] = 0;
			p->num = json_num(j, "part", 0);
			p->kind = (UINT)json_num(j, "kind", 0);
			if ( p->kind == TG_PARTS && u->tags == 0 ) u->tags = p->num;
		}
		/* a part the program has no number for is not worked: let go */
		if ( p->key > 0 && p->num == 0 ) {
			ob_cls_obj(p->key);
			p->key = 0;
		}
		u->np = i + 1;
	}
}

LOCAL void parts_close( UI *u )
{
	INT	i;

	for ( i = 0; i < u->np; i++ ) {
		if ( u->p[i].key > 0 ) ob_cls_obj(u->p[i].key);
		u->p[i].key = 0;
	}
	u->np = 0;
}

LOCAL UIPART *part( UI *u, INT num )
{
	INT	i;

	for ( i = 0; i < u->np; i++ ) {
		if ( u->p[i].num == num && u->p[i].key > 0 ) return &u->p[i];
	}
	return NULL;
}

/* ---------------------------------------------------------------- the window */

LOCAL char	ui_prog[40];

EXPORT void ui_icon( CONST char *prog )
{
	strncpy(ui_prog, prog, sizeof(ui_prog) - 1);
	ui_prog[sizeof(ui_prog) - 1] = 0;
}

EXPORT ER ui_open( UI *u, CONST char *box, INT num, INT x, INT y )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[224], title[128], meta[512];
	TS_UUID		pnl;
	ID		pk;

	memset(u, 0, sizeof(*u));
	if ( ui_uuid(box, &u->box) < E_OK ) return E_PAR;

	/* the panel first, standing on its own, for its size and its name */
	if ( panel_make(u, num, 0, 0, &pnl, &pk) < E_OK ) return E_OBJ;
	if ( ob_rea_rec(pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		ob_cls_obj(pk);
		(void)ob_del_obj(&pnl);
		return E_OBJ;
	}
	u->w = wp.right - wp.left;
	u->h = wp.bottom - wp.top;
	title[0] = 0;
	if ( ob_get_atr(pk, (UB *)meta, sizeof(meta) - 1, &asz) >= E_OK ) {
		meta[( asz < (SZ)sizeof(meta) ) ? asz : (SZ)sizeof(meta) - 1] = 0;
		json_str(meta, "name", title, sizeof(title));
	}

	if ( ui_prog[0] != 0 ) {
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":3,\"icon\":\"%s\"}",
			 x, y, x + u->w + 16, y + u->h + 34, ui_prog);
	} else {
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":3}", x, y, x + u->w + 16, y + u->h + 34);
	}
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)title;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &u->win) < E_OK ) {
		ob_cls_obj(pk);
		(void)ob_del_obj(&pnl);
		return E_OBJ;
	}
	u->kw = ob_opn_obj(&u->win, OB_OP_ALL);
	/* the work area exactly the panel's size, whatever the frame takes */
	if ( u->kw > 0 && ob_rea_rec(u->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		wp.right = wp.left + u->w + ( ( wp.right - wp.left ) - ( wp.wright - wp.wleft ) );
		wp.bottom = wp.top + u->h + ( ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop ) );
		(void)ob_wri_rec(u->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	}
	/* placed on the window, filling its work area */
	{
		T_OBMAP	m;

		memset(&m, 0, sizeof(m));
		if ( u->kw <= 0 || ob_map_rec(pk, 0, u->kw, &m) < E_OK ) {
			ob_cls_obj(pk);
			(void)ob_del_obj(&pnl);
			ui_close(u);
			return E_OBJ;
		}
	}
	u->pnl = pnl;
	u->pk = pk;
	parts_open(u);

	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	u->port = ( ob_cre_obj(&c, &u->ch) >= E_OK )
		? ob_opn_obj(&u->ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_DELETE | OB_E_PRESS | OB_E_KEY | OB_E_DROP;
	if ( u->port <= 0 || ob_ntf_evt(u->kw, OB_REC_ANY, &req, u->port) <= 0 ) {
		ui_close(u);
		return E_OBJ;
	}
	req.events = OB_E_CHANGE;
	req.id = 1;
	if ( ob_ntf_evt(u->pk, OB_REC_ANY, &req, u->port) <= 0 ) {
		ui_close(u);
		return E_OBJ;
	}
	return E_OK;
}

EXPORT ER ui_drop( UI *u, T_OBDROP *d )
{
	SZ	asz = 0;
	ER	er;

	memset(d, 0, sizeof(*d));
	er = ob_rea_rec(u->kw, OB_WR_DROP, 0, d, sizeof(*d), &asz);
	return ( er >= E_OK && d->n > 0 ) ? E_OK : ( er < E_OK ) ? er : E_NOEXS;
}

EXPORT void ui_drop_answer( UI *u, CONST T_OBDROP *d, UINT answer, CONST char *msg )
{
	T_OBDRANS	a;
	SZ		asz = 0;

	memset(&a, 0, sizeof(a));
	a.seq = d->seq;
	a.answer = answer;
	if ( msg != NULL ) strncpy((char *)a.msg, msg, sizeof(a.msg) - 1);
	(void)ob_wri_rec(u->kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
}

EXPORT void ui_close( UI *u )
{
	parts_close(u);
	if ( u->port > 0 ) {
		ob_cls_obj(u->port);
		(void)ob_del_obj(&u->ch);
	}
	if ( u->pk > 0 ) {
		ob_cls_obj(u->pk);
		(void)ob_del_obj(&u->pnl);
	}
	if ( u->kw > 0 ) ob_cls_obj(u->kw);
	(void)ob_del_obj(&u->win);
	u->port = u->kw = u->pk = 0;
}

/* The number of the part a panel's notice names, by its link record */
LOCAL INT num_of( UI *u, CONST T_OBNTM *m )
{
	INT	i = m->recno - OB_WR_KIND;

	return ( i >= 0 && i < u->np ) ? u->p[i].num : 0;
}

EXPORT INT ui_event( UI *u, CONST T_OBNTM *m, INT *p_num )
{
	INT	num;
	UIPART	*p;

	*p_num = 0;
	switch ( m->event ) {
	case OB_E_CLOSE:
	case OB_E_DELETE:
		return UI_EV_CLOSE;
	case OB_E_PRESS:
		return ( m->code == 1 && m->recno < 0 ) ? UI_EV_MENU : UI_EV_NONE;
	case OB_E_KEY:
		return UI_EV_KEY;
	case OB_E_DROP:
		return UI_EV_DROP;
	case OB_E_CHANGE:
		num = num_of(u, m);
		p = part(u, num);
		*p_num = num;
		if ( p == NULL ) return UI_EV_NONE;
		if ( p->kind == MS_PARTS ) return UI_EV_BUTTON;
		if ( p->kind == TG_PARTS ) {
			*p_num = ui_value(u, num);
			return UI_EV_SHEET;
		}
		if ( p->kind == TB_PARTS || p->kind == SB_PARTS || p->kind == XB_PARTS ) u->last = num;
		return UI_EV_CHANGE;
	default:
		return UI_EV_NONE;
	}
}

/* ---------------------------------------------------------------- what the parts hold */

LOCAL INT state( UI *u, INT num, void *buf, SZ max )
{
	UIPART	*p = part(u, num);
	SZ	asz = 0;

	if ( p == NULL || ob_rea_rec(p->key, OB_WR_STATE, 0, buf, max, &asz) < E_OK ) return -1;
	return (INT)asz;
}

LOCAL void set_state( UI *u, INT num, CONST void *buf, SZ n )
{
	UIPART	*p = part(u, num);
	SZ	asz = 0;

	if ( p != NULL ) (void)ob_wri_rec(p->key, OB_WR_STATE, 0, buf, n, &asz);
}

EXPORT INT ui_value( UI *u, INT num )
{
	INT	v[2] = { 0, 0 };

	(void)state(u, num, v, sizeof(v));
	return v[0];
}

EXPORT void ui_set_value( UI *u, INT num, INT v )
{
	set_state(u, num, &v, sizeof(v));
}

EXPORT void ui_set_now( UI *u, INT num, INT v, INT now )
{
	INT	a[2];

	a[0] = v;
	a[1] = now;
	set_state(u, num, a, sizeof(a));
}

EXPORT void ui_set_range( UI *u, INT num, INT v, INT lo, INT hi )
{
	INT	a[3];

	a[0] = v;
	a[1] = lo;
	a[2] = hi;
	set_state(u, num, a, sizeof(a));
}

EXPORT INT ui_text( UI *u, INT num, char *buf, INT max )
{
	INT	n = state(u, num, buf, max - 1);

	if ( n < 0 ) n = 0;
	buf[n] = 0;
	return n;
}

EXPORT void ui_set_text( UI *u, INT num, CONST char *s )
{
	set_state(u, num, s, (SZ)strlen(s));
}

EXPORT INT ui_fields( UI *u, INT num, INT *v, INT n )
{
	INT	b[8], k = state(u, num, b, sizeof(b)) / (INT)sizeof(INT), i;

	for ( i = 0; i < n; i++ ) v[i] = ( i < k ) ? b[i] : 0;
	return k;
}

EXPORT void ui_set_fields( UI *u, INT num, CONST INT *v, INT n )
{
	set_state(u, num, v, (SZ)n * sizeof(INT));
}

EXPORT void ui_blank( UI *u, INT num )
{
	set_state(u, num, "", 0);
}

EXPORT void ui_names( UI *u, INT num, CONST char *const *names, INT n )
{
	UIPART	*p = part(u, num);
	char	buf[2048];
	SZ	len = 0, asz = 0;
	INT	i;

	if ( p == NULL ) return;
	for ( i = 0; i < n; i++ ) {
		SZ	k = (SZ)strlen(names[i]);

		if ( len + k + 1 > (SZ)sizeof(buf) ) break;
		memcpy(buf + len, names[i], k + 1);
		len += k + 1;
	}
	(void)ob_wri_rec(p->key, OB_WR_KIND, 0, buf, len, &asz);
}

LOCAL void flags( UI *u, INT num, UINT set, UINT clear )
{
	UIPART		*p = part(u, num);
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( p == NULL || ob_rea_rec(p->key, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	wp.flags = ( wp.flags | set ) & ~clear;
	(void)ob_wri_rec(p->key, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
}

EXPORT void ui_off( UI *u, INT num, BOOL off )
{
	flags(u, num, off ? OB_WP_OFF : 0, off ? 0 : OB_WP_OFF);
}

EXPORT void ui_show( UI *u, INT num, BOOL shown )
{
	flags(u, num, shown ? OB_WP_SHOWN : 0, shown ? 0 : OB_WP_SHOWN);
}

EXPORT void ui_set_ip( UI *u, INT num, UW addr )
{
	INT	v[4], i;

	for ( i = 0; i < 4; i++ ) v[i] = (INT)( ( addr >> ( i * 8 ) ) & 0xFF );
	ui_set_fields(u, num, v, 4);
}

EXPORT UW ui_get_ip( UI *u, INT num )
{
	INT	v[4], i;
	UW	a = 0;

	if ( ui_fields(u, num, v, 4) < 4 ) return 0;
	for ( i = 0; i < 4; i++ ) a |= (UW)( v[i] & 0xFF ) << ( i * 8 );
	return a;
}

EXPORT INT ui_sheet( UI *u )
{
	return ( u->tags > 0 ) ? ui_value(u, u->tags) : 0;
}

EXPORT void ui_show_sheet( UI *u, INT tag )
{
	if ( u->tags > 0 ) ui_set_value(u, u->tags, tag);
}

/* ---------------------------------------------------------------- asking */

EXPORT INT ui_ask( UI *u, INT num, CONST char *const *lines, INT nline )
{
	UI		q;
	T_OBNTM		m;
	T_OBNTF		req;
	SZ		asz = 0;
	INT		i, answer = -1, x, y, n;

	/*
	 * The panel in a window of its own over the middle of the
	 * program's, so that the keys and the presses are its alone.
	 */
	memset(&q, 0, sizeof(q));
	q.box = u->box;
	{
		T_OBWPOS	wp, ww;
		T_OBCRE		c;
		char		json[96];
		INT		pw, ph;

		if ( panel_make(&q, num, 0, 0, &q.pnl, &q.pk) < E_OK ) return 0;
		(void)ob_rea_rec(q.pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
		pw = wp.right - wp.left;
		ph = wp.bottom - wp.top;
		if ( ob_rea_rec(u->kw, OB_WR_PLACE, 0, &ww, sizeof(ww), &asz) < E_OK ) {
			ww.wleft = ww.wtop = 100;
		}
		x = ww.wleft + ( u->w - pw ) / 2;
		y = ww.wtop + ( u->h - ph ) / 2;
		if ( x < 0 ) x = 0;
		if ( y < 0 ) y = 0;
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":256}", x, y, x + pw, y + ph);
		memset(&c, 0, sizeof(c));
		c.type = OB_T_WINDOW;
		c.sub = OB_S_WINDOW;
		c.json = (CONST UB *)json;
		c.jsonsz = (SZ)strlen(json);
		if ( ob_cre_obj(&c, &q.win) < E_OK || ( q.kw = ob_opn_obj(&q.win, OB_OP_ALL) ) <= 0 ) {
			ob_cls_obj(q.pk);
			(void)ob_del_obj(&q.pnl);
			return 0;
		}
		{
			T_OBMAP	m;

			memset(&m, 0, sizeof(m));
			(void)ob_map_rec(q.pk, 0, q.kw, &m);
		}
	}
	parts_open(&q);
	for ( i = 0; i < nline && lines != NULL; i++ ) {
		ui_set_text(&q, i + 1, lines[i]);
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 2;
	(void)ob_ntf_evt(q.pk, OB_REC_ANY, &req, u->port);
	req.events = OB_E_KEY | OB_E_CLOSE;
	req.id = 3;
	(void)ob_ntf_evt(q.kw, OB_REC_ANY, &req, u->port);

	while ( answer < 0 ) {
		while ( answer < 0 && ob_rea_rec(u->port, 0, 0, &m, sizeof(m), &asz) >= E_OK ) {
			if ( m.id == 2 && m.event == OB_E_CHANGE ) {
				n = num_of(&q, &m);
				for ( i = 0; i < q.np; i++ ) {
					if ( q.p[i].num == n && q.p[i].kind == MS_PARTS ) answer = n;
				}
			} else if ( m.id != 2 && m.id != 3 && m.event == OB_E_DELETE ) {
				answer = 0;
			} else if ( m.id == 3 && ( m.event == OB_E_CLOSE
					       || ( m.event == OB_E_KEY && m.code == 0x29 ) ) ) {
				answer = 0;
			}
		}
		if ( answer < 0 ) (void)tk_dly_tsk(20);
	}
	parts_close(&q);
	ob_cls_obj(q.pk);
	(void)ob_del_obj(&q.pnl);
	ob_cls_obj(q.kw);
	(void)ob_del_obj(&q.win);
	return answer;
}

/* ---------------------------------------------------------------- a panel worked while it is up */

EXPORT ER ui_dlg_open( UI *u, INT num, UI *q )
{
	T_OBWPOS	wp, ww;
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBMAP		m;
	SZ		asz = 0;
	char		json[128], title[128], meta[512];
	INT		pw, ph, x, y;

	memset(q, 0, sizeof(*q));
	q->box = u->box;
	if ( panel_make(q, num, 0, 0, &q->pnl, &q->pk) < E_OK ) return E_OBJ;
	(void)ob_rea_rec(q->pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	pw = wp.right - wp.left;
	ph = wp.bottom - wp.top;
	title[0] = 0;
	if ( ob_get_atr(q->pk, (UB *)meta, sizeof(meta) - 1, &asz) >= E_OK ) {
		meta[( asz < (SZ)sizeof(meta) ) ? asz : (SZ)sizeof(meta) - 1] = 0;
		json_str(meta, "name", title, sizeof(title));
	}

	/* over the middle of the program's window, and on the screen */
	if ( u->kw <= 0 || ob_rea_rec(u->kw, OB_WR_PLACE, 0, &ww, sizeof(ww), &asz) < E_OK ) {
		memset(&ww, 0, sizeof(ww));
		ww.wleft = ww.wtop = 100;
	}
	x = ww.wleft + ( u->w - pw ) / 2;
	y = ww.wtop + ( u->h - ph ) / 2;
	if ( ww.sw > 0 && x + pw > ww.sw ) x = ww.sw - pw;
	if ( ww.sh > 0 && y + ph > ww.sh ) y = ww.sh - ph;
	if ( x < 0 ) x = 0;
	if ( y < 0 ) y = 0;
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":256}", x, y, x + pw, y + ph);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)title;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &q->win) < E_OK || ( q->kw = ob_opn_obj(&q->win, OB_OP_ALL) ) <= 0 ) {
		ob_cls_obj(q->pk);
		(void)ob_del_obj(&q->pnl);
		return E_OBJ;
	}
	memset(&m, 0, sizeof(m));
	if ( ob_map_rec(q->pk, 0, q->kw, &m) < E_OK ) {
		ui_dlg_close(q);
		return E_OBJ;
	}
	q->w = pw;
	q->h = ph;
	parts_open(q);
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 2;
	(void)ob_ntf_evt(q->pk, OB_REC_ANY, &req, u->port);
	req.events = OB_E_KEY | OB_E_CLOSE;
	req.id = 3;
	(void)ob_ntf_evt(q->kw, OB_REC_ANY, &req, u->port);
	return E_OK;
}

EXPORT INT ui_dlg_next( UI *u, UI *q, T_OBNTM *m, INT *p_num )
{
	SZ	asz = 0;
	INT	n, i;

	*p_num = 0;
	if ( ob_rea_rec(u->port, 0, 0, m, sizeof(*m), &asz) < E_OK ) return UI_EV_NONE;
	if ( m->id == 2 && m->event == OB_E_CHANGE ) {
		n = num_of(q, m);
		*p_num = n;
		for ( i = 0; i < q->np; i++ ) {
			if ( q->p[i].num == n && n != 0 ) {
				return ( q->p[i].kind == MS_PARTS ) ? UI_EV_BUTTON : UI_EV_CHANGE;
			}
		}
		return UI_EV_NONE;
	}
	if ( m->id == 2 ) return UI_EV_NONE;
	if ( m->id == 3 ) {
		if ( m->event == OB_E_CLOSE || ( m->event == OB_E_KEY && m->code == 0x29 ) ) return UI_EV_CLOSE;
		return UI_EV_NONE;
	}
	return UI_EV_OTHER;
}

EXPORT void ui_dlg_close( UI *q )
{
	parts_close(q);
	if ( q->pk > 0 ) {
		ob_cls_obj(q->pk);
		(void)ob_del_obj(&q->pnl);
	}
	if ( q->kw > 0 ) {
		ob_cls_obj(q->kw);
		(void)ob_del_obj(&q->win);
	}
	q->kw = q->pk = 0;
}
