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
#include <stdlib.h>
#include <stdio.h>
#include <ts/ui.h>
#include <ts/part.h>
#include <ts/obdrop.h>
#include <ts/obatr.h>
#include <ts/time.h>
#include <ts/hid.h>
#include <ts/uuid.h>

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

/* ---------------------------------------------------------------- lists read whole */

#define ALL_FIRST	4096			/* bytes a record is first read into */
#define ALL_LIMIT	( (SZ)0x7FFFFFFF )	/* what the INT the length is answered in holds */
#define LST_PAGE	256			/* objects asked for at a time */

/*
 * A record read whole: from its start into a buffer twice as large while
 * it fills the buffer, so a record made when it is read comes as one
 * whole, with room bytes more after it and a nought after the text
 */
/* What record `recno` of the object open as key holds, under the right to read it; 0 when it cannot be told */
LOCAL SZ rec_size_of( ID key, INT recno )
{
	SZ	size = 0;

	if ( ob_rea_rec(key, recno, 0, NULL, 0, &size) < E_OK || size < 0 || size >= ALL_LIMIT ) {
		return 0;
	}
	return size;
}

/*
 * A record read whole: its size asked first (under the right to read
 * it) and read once into a buffer one byte larger; one that fills it (it
 * grew, or it is made as it is read and has no size of its own) is read
 * again from its start into a buffer twice as large while it fills it.
 */
EXPORT ID ui_conf_open( CONST TS_UUID *u, UINT ops )
{
	ID	key;
	INT	i;

	if ( ( ops & ( OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRWR ) ) != 0 ) {
		ops |= OB_O_WEXCL;
	}
	for ( i = 0; ; i++ ) {
		key = ob_opn_obj(u, ops);
		if ( key != E_BUSY || i >= 100 ) {
			return key;
		}
		(void)tk_dly_tsk(10);
	}
}

EXPORT INT ui_key_all( ID key, INT recno, INT room, UB **p_buf, INT *p_cap )
{
	UB	*b = NULL, *nb;
	SZ	cap = ALL_FIRST, asz;
	ER	er;

	*p_buf = NULL;
	if ( room < 0 ) return E_PAR;
	asz = rec_size_of(key, recno);
	if ( asz >= cap ) cap = asz + 1;
	for ( ;; ) {
		b = (UB *)malloc((size_t)cap + 1);
		if ( b == NULL ) return E_NOMEM;
		asz = 0;
		er = ob_rea_rec(key, recno, 0, b, cap, &asz);
		if ( er < E_OK ) {
			free(b);
			return (INT)er;
		}
		if ( asz < cap ) break;
		free(b);
		if ( cap >= ALL_LIMIT / 2 ) return E_LIMIT;
		cap *= 2;
	}
	if ( asz + (SZ)room >= ALL_LIMIT ) {
		free(b);
		return E_LIMIT;
	}
	if ( cap - asz < (SZ)room ) {
		nb = (UB *)realloc(b, (size_t)( asz + room + 1 ));
		if ( nb == NULL ) {
			free(b);
			return E_NOMEM;
		}
		b = nb;
		cap = asz + room;
	}
	b[asz] = 0;
	*p_buf = b;
	if ( p_cap != NULL ) *p_cap = (INT)cap;
	return (INT)asz;
}

EXPORT INT ui_rec_all( CONST TS_UUID *u, INT recno, INT room, UB **p_buf )
{
	ID	key;
	INT	len;

	*p_buf = NULL;
	key = ob_opn_obj(u, OB_OP_R);
	if ( key < E_OK ) return (INT)key;
	len = ui_key_all(key, recno, room, p_buf, NULL);
	ob_cls_obj(key);
	return len;
}

/* Every object of a kind, a page at a time from the last one given */
EXPORT INT ui_lst_obj( UINT type, UINT sub, TS_UUID **p_ids )
{
	TS_UUID	*ids = NULL, *ni;
	INT	n = 0, got;
	ER	er;

	*p_ids = NULL;
	for ( ;; ) {
		ni = (TS_UUID *)realloc(ids, sizeof(TS_UUID) * (size_t)( n + LST_PAGE ));
		if ( ni == NULL ) {
			free(ids);
			return E_NOMEM;
		}
		ids = ni;
		got = 0;
		er = ob_lst_obj(type, sub, ( n > 0 ) ? &ids[n - 1] : NULL, ids + n, LST_PAGE, &got);
		if ( er < E_OK ) {
			free(ids);
			return (INT)er;
		}
		n += got;
		if ( got < LST_PAGE ) break;
	}
	*p_ids = ids;
	return n;
}

/* Every link of a box: asked how many, then taken, again when more came between */
EXPORT INT ui_lst_lnk( CONST TS_UUID *box, TS_UUID **p_ids )
{
	TS_UUID	*ids;
	INT	cnt = 0, all = 0;
	ER	er;

	*p_ids = NULL;
	er = ob_lst_lnk(box, NULL, NULL, 0, &cnt);
	if ( er < E_OK ) return (INT)er;
	for ( ;; ) {
		ids = (TS_UUID *)malloc(sizeof(TS_UUID) * (size_t)( ( cnt > 0 ) ? cnt : 1 ));
		if ( ids == NULL ) return E_NOMEM;
		er = ob_lst_lnk(box, NULL, ids, cnt, &all);
		if ( er < E_OK ) {
			free(ids);
			return (INT)er;
		}
		if ( all <= cnt ) break;
		free(ids);
		cnt = all;
	}
	*p_ids = ids;
	return all;
}

/* ---------------------------------------------------------------- panels */

/* A panel of the box made and placed on the window at (x, y) of its work area */
/* beside: placed beside what the program draws itself (ui_strip): its boxes take no keys until pressed */
LOCAL ER panel_make( UI *u, INT num, INT x, INT y, TS_UUID *p_pnl, ID *p_key, BOOL beside )
{
	T_OBCRE	c;
	T_OBMAP	m;
	char	json[160], bs[40];
	ID	key;
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		snprintf(bs + i * 2, 3, "%02x", u->box.b[i]);
	}
	snprintf(json, sizeof(json), "{\"tessronos\":{\"part\":{\"sub\":\"panel\",\"box\":\"%.8s-%.4s-%.4s-%.4s-%.12s\",\"num\":%d%s}}}",
		 bs, bs + 8, bs + 12, bs + 16, bs + 20, num, beside ? ",\"beside\":1" : "");
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_PANEL;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, p_pnl) < E_OK ) return E_OBJ;
	key = ob_opn_obj(p_pnl, OB_OP_R | OB_OP_W | OB_OP_PROT);
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
	char	*j = NULL;
	INT	i;

	u->np = 0;
	u->tags = 0;
	for ( i = 0; i < UI_PART_MAX; i++ ) {
		UIPART	*p = &u->p[i];

		if ( ob_rea_rec(u->pk, OB_WR_KIND + i, 0, &pu, sizeof(pu), &asz) < E_OK
		  || asz < (SZ)sizeof(pu) ) {
			break;
		}
		p->key = ob_opn_obj(&pu, OB_OP_R | OB_OP_W | OB_OP_PROT);
		p->num = 0;
		p->kind = 0;
		if ( p->key > 0 && ob_atr_read(p->key, 0, (UB **)&j) >= 0 ) {
			p->num = json_num(j, "part", 0);	/* read whole: the keys may come anywhere */
			p->kind = (UINT)json_num(j, "kind", 0);
			if ( p->kind == TG_PARTS && u->tags == 0 ) u->tags = p->num;
			free(j);
			j = NULL;
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

LOCAL UINT	ui_more_attr = 0;		/* what ui_attr asked for the next window */
LOCAL char	ui_parent[40];			/* what ui_sub asked for the next window: its window's UUID */

EXPORT void ui_attr( UINT attr )
{
	ui_more_attr = attr;
}

EXPORT void ui_bar( UI *u, INT lo, INT hi, INT clo, INT chi )
{
	T_OBWBAR	b;
	SZ		asz = 0;

	memset(&b, 0, sizeof(b));
	b.lo = lo;
	b.hi = hi;
	b.clo = clo;
	b.chi = chi;
	(void)ob_wri_rec(u->kw, OB_WR_BARS, 0, &b, sizeof(b), &asz);
}

EXPORT void ui_work( UI *u, INT *p_w, INT *p_h )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	*p_w = u->w;
	*p_h = u->h;
	if ( ob_rea_rec(u->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		*p_w = wp.wright - wp.wleft;
		*p_h = wp.wbottom - wp.wtop;
	}
}

EXPORT void ui_sub( CONST TS_UUID *parent )
{
	if ( parent == NULL ) {
		ui_parent[0] = 0;
		return;
	}
	ts_uuid_text(parent, ui_parent);
}

EXPORT void ui_icon( CONST char *prog )
{
	strncpy(ui_prog, prog, sizeof(ui_prog) - 1);
	ui_prog[sizeof(ui_prog) - 1] = 0;
}

LOCAL ER open_win( UI *u, ID port, CONST char *box, INT num, INT x, INT y );

EXPORT ER ui_open( UI *u, CONST char *box, INT num, INT x, INT y )
{
	return open_win(u, 0, box, num, x, y);
}

EXPORT ER ui_open_on( UI *u, CONST UI *with, CONST char *box, INT num, INT x, INT y )
{
	if ( with == NULL || with->port <= 0 ) return E_PAR;
	return open_win(u, with->port, box, num, x, y);
}

/* A window made hidden (UI_ATTR_HIDDEN) shown, in front, its parts as they were set */
EXPORT void ui_reveal( UI *u )
{
	T_OBWPOS	wp;
	SZ		asz = 0;

	if ( u->kw <= 0 || ob_rea_rec(u->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) return;
	wp.flags |= OB_WP_SHOWN;
	wp.z = 0;
	(void)ob_wri_rec(u->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
}

EXPORT BOOL ui_mine( CONST UI *u, CONST T_OBNTM *m )
{
	return (BOOL)( u->kw > 0 && ( memcmp(&m->uuid, &u->win, sizeof(TS_UUID)) == 0
				   || memcmp(&m->uuid, &u->pnl, sizeof(TS_UUID)) == 0 ) );
}

/* The window, its notices to port, or to a channel of its own made for it (port 0) */
LOCAL ER open_win( UI *u, ID port, CONST char *box, INT num, INT x, INT y )
{
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBWPOS	wp;
	SZ		asz = 0;
	char		json[224], title[128];
	TS_UUID		pnl;
	ID		pk;
	UINT		attr;

	memset(u, 0, sizeof(*u));
	if ( ui_uuid(box, &u->box) < E_OK ) return E_PAR;

	/* the panel first, standing on its own, for its size and its name */
	if ( panel_make(u, num, 0, 0, &pnl, &pk, FALSE) < E_OK ) return E_OBJ;
	if ( ob_rea_rec(pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) < E_OK ) {
		ob_cls_obj(pk);
		(void)ob_del_obj(&pnl);
		return E_OBJ;
	}
	u->w = wp.right - wp.left;
	u->h = wp.bottom - wp.top;
	title[0] = 0;
	{
		char	*meta = NULL;

		if ( ob_atr_read(pk, 0, (UB **)&meta) >= 0 ) {
			json_str(meta, "name", title, sizeof(title));
			free(meta);
		}
	}

	attr = ui_more_attr;
	ui_more_attr = 0;			/* what ui_attr asked was for this window alone */
	{
		BOOL	hidden = (BOOL)( ( attr & UI_ATTR_HIDDEN ) != 0 );

		/* framed, with a title bar unless asked not to */
		UINT	wa = ( ( attr & UI_ATTR_UNTITLED ) != 0 ) ? WM_ATTR_FRAME : WM_ATTR_FRAME | WM_ATTR_TITLE;

		attr &= ~( UI_ATTR_UNTITLED | UI_ATTR_HIDDEN );
		if ( ui_prog[0] != 0 ) {
			snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%u,\"icon\":\"%s\"",
				 x, y, x + u->w + 16, y + u->h + 34, wa | attr, ui_prog);
		} else {
			snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":%u", x, y, x + u->w + 16,
				 y + u->h + 34, wa | attr);
		}
		if ( hidden ) snprintf(json + strlen(json), sizeof(json) - strlen(json), ",\"hidden\":1");
	}
	if ( ui_parent[0] != 0 ) {
		/* a subordinate window of that one, as the system keeps it (wm_set_parent) */
		snprintf(json + strlen(json), sizeof(json) - strlen(json), ",\"sub\":\"%s\"", ui_parent);
		ui_parent[0] = 0;
	}
	snprintf(json + strlen(json), sizeof(json) - strlen(json), "}");
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
	u->kw = ob_opn_obj(&u->win, OB_OP_R | OB_OP_W | OB_OP_PROT);
	/*
	 * The work area exactly the panel's size, whatever the frame takes,
	 * at the place asked for: the system keeps it on a smaller screen
	 * and gives it back on a larger one.
	 */
	if ( u->kw > 0 && ob_rea_rec(u->kw, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		INT	fw = ( wp.right - wp.left ) - ( wp.wright - wp.wleft );
		INT	fh = ( wp.bottom - wp.top ) - ( wp.wbottom - wp.wtop );

		wp.left = x;
		wp.top = y;
		wp.right = x + u->w + fw;
		wp.bottom = y + u->h + fh;
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

	if ( port > 0 ) {
		u->port = port;			/* the other window's: u->ch stays none */
	} else {
		memset(&c, 0, sizeof(c));
		c.type = OB_T_CHANNEL;
		u->port = ( ob_cre_obj(&c, &u->ch) >= E_OK )
			? ob_opn_obj(&u->ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT) : E_NOEXS;
	}
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CLOSE | OB_E_DELETE | OB_E_PRESS | OB_E_KEY | OB_E_DROP;
	if ( attr != 0 ) {
		req.events |= OB_E_WHEEL | OB_E_SCROLL | OB_E_REDRAW;	/* its bar and its size */
	}
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

EXPORT T_OBDROP *ui_drop( UI *u )
{
	static T_OBDROP	*last = NULL;	/* the one read before: kept until the next */

	free(last);
	last = ob_drop_read(u->kw);
	if ( last != NULL && last->n <= 0 ) {
		free(last);
		last = NULL;
	}
	return last;
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
	static CONST TS_UUID	none;

	parts_close(u);
	if ( u->port > 0 && memcmp(&u->ch, &none, sizeof(none)) != 0 ) {
		ob_cls_obj(u->port);		/* its own channel, not another window's (ui_open_on) */
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

EXPORT ER ui_wait( UI *u, T_OBNTM *m, TMO tmo )
{
	SZ	asz = 0;

	return ob_rea_tmo(u->port, 0, 0, m, sizeof(*m), &asz, tmo);
}

EXPORT UD ui_now( void )
{
	UD	ns = 0;

	(void)ts_get_mono(&ns);
	return ns / 1000000;
}

EXPORT TMO ui_left( UD at )
{
	UD	now = ui_now();

	return ( at > now ) ? (TMO)( at - now ) : 0;
}

EXPORT INT ui_ticks( UD *p_at, INT tick, INT max )
{
	UD	now = ui_now(), n;

	if ( now < *p_at || tick <= 0 ) {
		return 0;
	}
	n = ( now - *p_at ) / (UD)tick;
	if ( n > (UD)max ) {
		*p_at = now;
		return 1;
	}
	*p_at += n * (UD)tick;
	return (INT)n;
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
		/* the second button opens the window's menu wherever it is pressed, on a part as well */
		return ( m->code == 1 ) ? UI_EV_MENU : UI_EV_NONE;
	case OB_E_KEY:
		return UI_EV_KEY;
	case OB_E_DROP:
		return UI_EV_DROP;
	case OB_E_WHEEL:
		/* what the accessories show goes only up and down: the wheel across is let go */
		if ( m->code != HID_WHEEL_V ) return UI_EV_NONE;
		*p_num = m->dz;
		return UI_EV_WHEEL;
	case OB_E_SCROLL:
		*p_num = m->x;
		return ( m->code == OB_BAR_R ) ? UI_EV_SCROLL : UI_EV_NONE;
	case OB_E_REDRAW:
		return UI_EV_SIZE;
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

EXPORT void ui_set_swatch( UI *u, INT num, UW colour, BOOL chosen )
{
	INT	a[2];

	a[0] = (INT)colour;
	a[1] = chosen ? 1 : 0;
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

/* The names a list or a popup offers, each ending in its nought, as many as are given */
EXPORT void ui_names( UI *u, INT num, CONST char *const *names, INT n )
{
	UIPART	*p = part(u, num);
	char	*buf;
	SZ	len = 0, asz = 0;
	INT	i;

	if ( p == NULL ) return;
	for ( i = 0; i < n; i++ ) len += (SZ)strlen(names[i]) + 1;
	buf = (char *)malloc(( len > 0 ) ? (size_t)len : 1);
	if ( buf == NULL ) return;
	len = 0;
	for ( i = 0; i < n; i++ ) {
		SZ	k = (SZ)strlen(names[i]);

		memcpy(buf + len, names[i], (size_t)k + 1);
		len += k + 1;
	}
	(void)ob_wri_rec(p->key, OB_WR_KIND, 0, buf, len, &asz);
	free(buf);
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

EXPORT void ui_focus( UI *u, INT num )
{
	flags(u, num, OB_WP_FOCUS, 0);
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

/*
 * Where a panel of pw x ph that asks something stands: in the middle of
 * the main screen and kept on it, as the system's own panels that ask
 * something stand (wm_panel_open_centre).
 */
LOCAL void ask_place( INT pw, INT ph, INT *p_x, INT *p_y )
{
	INT	sw = 0, sh = 0, x, y;

	wm_screen(&sw, &sh);
	if ( sw <= 0 || sh <= 0 ) {
		sw = 1280;
		sh = 800;
	}
	x = ( sw - pw ) / 2;
	y = ( sh - ph ) / 2;
	if ( x + pw > sw ) x = sw - pw;
	if ( y + ph > sh ) y = sh - ph;
	if ( x < 0 ) x = 0;
	if ( y < 0 ) y = 0;
	*p_x = x;
	*p_y = y;
}

EXPORT INT ui_ask( UI *u, INT num, CONST char *const *lines, INT nline )
{
	UI		q;
	T_OBNTM		m;
	T_OBNTF		req;
	SZ		asz = 0;
	INT		i, answer = -1, x, y, n;

	/*
	 * The panel in a window of its own in the middle of the screen,
	 * so that the keys and the presses are its alone.
	 */
	memset(&q, 0, sizeof(q));
	q.box = u->box;
	{
		T_OBWPOS	wp;
		T_OBCRE		c;
		char		json[96];
		INT		pw, ph;

		if ( panel_make(&q, num, 0, 0, &q.pnl, &q.pk, FALSE) < E_OK ) return 0;
		(void)ob_rea_rec(q.pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
		pw = wp.right - wp.left;
		ph = wp.bottom - wp.top;
		ask_place(pw, ph, &x, &y);
		snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":256}", x, y, x + pw, y + ph);
		memset(&c, 0, sizeof(c));
		c.type = OB_T_WINDOW;
		c.sub = OB_S_WINDOW;
		c.json = (CONST UB *)json;
		c.jsonsz = (SZ)strlen(json);
		if ( ob_cre_obj(&c, &q.win) < E_OK || ( q.kw = ob_opn_obj(&q.win, OB_OP_R | OB_OP_W | OB_OP_PROT) ) <= 0 ) {
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

	/* each notice waited for: the program sleeps until the panel is worked */
	while ( answer < 0 ) {
		if ( ob_rea_tmo(u->port, 0, 0, &m, sizeof(m), &asz, TMO_FEVR) < E_OK ) {
			answer = 0;			/* the channel is gone */
		} else {
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
	}
	parts_close(&q);
	ob_cls_obj(q.pk);
	(void)ob_del_obj(&q.pnl);
	ob_cls_obj(q.kw);
	(void)ob_del_obj(&q.win);
	return answer;
}

/* ---------------------------------------------------------------- a panel on a window of the program's own */

EXPORT ER ui_strip( UI *u, CONST char *box, INT num, ID kw, ID port, INT x, INT y )
{
	T_OBWPOS	wp;
	T_OBNTF		req;
	SZ		asz = 0;

	memset(u, 0, sizeof(*u));
	if ( ui_uuid(box, &u->box) < E_OK || kw <= 0 ) return E_PAR;
	u->kw = kw;
	if ( panel_make(u, num, x, y, &u->pnl, &u->pk, TRUE) < E_OK ) {
		u->kw = 0;
		return E_OBJ;
	}
	if ( ob_rea_rec(u->pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz) >= E_OK ) {
		u->w = wp.right - wp.left;
		u->h = wp.bottom - wp.top;
	}
	parts_open(u);
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	req.id = 1;
	if ( port <= 0 || ob_ntf_evt(u->pk, OB_REC_ANY, &req, port) <= 0 ) {
		ui_strip_close(u);
		return E_OBJ;
	}
	u->port = port;
	return E_OK;
}

EXPORT void ui_strip_close( UI *u )
{
	parts_close(u);
	if ( u->pk > 0 ) {
		ob_cls_obj(u->pk);
		(void)ob_del_obj(&u->pnl);
	}
	u->pk = 0;
	u->kw = 0;			/* the window is the program's: not closed here */
	u->port = 0;
}

/* ---------------------------------------------------------------- a panel worked while it is up */

EXPORT ER ui_dlg_open( UI *u, INT num, UI *q )
{
	T_OBWPOS	wp;
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBMAP		m;
	SZ		asz = 0;
	char		json[128], title[128], *meta = NULL;
	INT		pw, ph, x, y;

	memset(q, 0, sizeof(*q));
	q->box = u->box;
	if ( panel_make(q, num, 0, 0, &q->pnl, &q->pk, FALSE) < E_OK ) return E_OBJ;
	(void)ob_rea_rec(q->pk, OB_WR_PLACE, 0, &wp, sizeof(wp), &asz);
	pw = wp.right - wp.left;
	ph = wp.bottom - wp.top;
	title[0] = 0;
	if ( ob_atr_read(q->pk, 0, (UB **)&meta) >= 0 ) {
		json_str(meta, "name", title, sizeof(title));
		free(meta);
	}

	ask_place(pw, ph, &x, &y);
	snprintf(json, sizeof(json), "{\"rect\":[%d,%d,%d,%d],\"attr\":256}", x, y, x + pw, y + ph);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)title;
	c.json = (CONST UB *)json;
	c.jsonsz = (SZ)strlen(json);
	if ( ob_cre_obj(&c, &q->win) < E_OK || ( q->kw = ob_opn_obj(&q->win, OB_OP_R | OB_OP_W | OB_OP_PROT) ) <= 0 ) {
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

	*p_num = 0;
	if ( ob_rea_rec(u->port, 0, 0, m, sizeof(*m), &asz) < E_OK ) return UI_EV_NONE;
	return ui_dlg_take(q, m, p_num);
}

EXPORT INT ui_dlg_take( UI *q, CONST T_OBNTM *m, INT *p_num )
{
	INT	n, i;

	*p_num = 0;
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
