/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	unp_obj.c
 *	the real objects made from an archive (design 11.20)
 *
 *	What is taken out goes where the root's link was carried to and let
 *	go: the desktop carries the link's outline out of this program's
 *	window, answers which object's window it was let go over, and puts
 *	the link to the root there once the objects are made.
 *
 *	First every object is made, one for each file of the archive, on the
 *	volume of the object it goes into: its name, its window and colour, the
 *	programs that open it (from its 実行機能付箋 and the links to it) and
 *	the icon of the template of the one that opens it first. Only then are the records
 *	written, the TAD ones as xmlTAD first and the rest after them as
 *	they were, because a store that counts the links of the records
 *	itself counts only a link to an object that is already there. On a
 *	store that does not, every link written is counted here. The archive
 *	object itself is left as it is: each carry takes out a set of its own.
 */

#include <ts/bpk.h>
#include "unp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ts/sysdef.h>
#include <ts/json.h>
#include <ts/obatr.h>
#include <ts/dtreq.h>

/* bytes read from and written to a record at a time: each write is made
   durable when it returns, so the fewer they are the less the medium waits
   (design 7.3) */
#define CHUNK		( 4 * 1024 * 1024 )
#define ICON_KEEP	8

/* ---------------------------------------------------------------- UUIDs */

EXPORT void unp_uuid_str( const TS_UUID *u, char *out )
{
	static const char	hex[] = "0123456789abcdef";
	INT			i, n = 0;

	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) out[n++] = '-';
		out[n++] = hex[u->b[i] >> 4];
		out[n++] = hex[u->b[i] & 0x0F];
	}
	out[n] = 0;
}

/* The UUID a text starts with ("019a...-..._0.xtad"); FALSE when it does not */
EXPORT BOOL unp_uuid_parse( const char *s, TS_UUID *u )
{
	INT	i, k = 0;

	for ( i = 0; s[i] != 0 && k < 32; i++ ) {
		char	c = s[i];
		INT	v;

		if ( c == '-' ) continue;
		if ( c >= '0' && c <= '9' ) v = c - '0';
		else if ( c >= 'a' && c <= 'f' ) v = c - 'a' + 10;
		else if ( c >= 'A' && c <= 'F' ) v = c - 'A' + 10;
		else return FALSE;
		if ( ( k & 1 ) == 0 ) u->b[k / 2] = (UB)( v << 4 );
		else u->b[k / 2] |= (UB)v;
		k++;
	}
	return (BOOL)( k == 32 );
}

/* ---------------------------------------------------------------- records */

/* A whole record of an object read, 0 after it; NULL when it cannot be */
EXPORT UB *unp_read_rec( ID key, INT recno, UINT *p_len )
{
	T_OBREC	r[64];
	INT	cnt = 0, i;
	UD	size = 0;
	SZ	asz = 0;
	UB	*buf;
	BOOL	found = FALSE;

	*p_len = 0;
	if ( ob_lst_rec(key, r, 64, &cnt) < E_OK ) return NULL;
	for ( i = 0; i < cnt && i < 64; i++ ) {
		if ( r[i].recno == recno ) {
			size = r[i].size;
			found = TRUE;
		}
	}
	if ( !found || size > 256 * 1024 * 1024 ) return NULL;
	buf = malloc((size_t)size + 1);
	if ( buf == NULL ) return NULL;
	for ( asz = 0; (UD)*p_len < size; ) {
		SZ	n = ( size - *p_len > CHUNK ) ? CHUNK : (SZ)( size - *p_len );

		if ( ob_rea_rec(key, recno, (D)*p_len, buf + *p_len, n, &asz) < E_OK || asz <= 0 ) break;
		*p_len += (UINT)asz;
	}
	if ( (UD)*p_len != size ) {
		free(buf);
		*p_len = 0;
		return NULL;
	}
	buf[size] = 0;
	return buf;
}

LOCAL ER write_all( ID key, INT recno, const void *data, UINT len )
{
	UINT	at = 0;
	SZ	asz = 0;
	ER	er;

	while ( at < len ) {
		SZ	n = ( len - at > CHUNK ) ? CHUNK : (SZ)( len - at );

		er = ob_wri_rec(key, recno, (D)at, (const UB *)data + at, n, &asz);
		if ( er < E_OK ) return er;
		if ( asz != n ) return E_IO;
		at += (UINT)n;
	}
	return E_OK;
}

/* A record added at the end and written; E_OK or why not */
LOCAL ER add_rec( ID key, UINT rt, UINT sub, const void *data, UINT len )
{
	INT	recno = -1;
	ER	er = ob_apd_rec(key, rt, sub, &recno);

	if ( er < E_OK ) return er;
	return write_all(key, recno, data, len);
}

/* ---------------------------------------------------------------- the programs */

typedef struct {
	char	id[48];
	TS_UUID	base;
	BOOL	hasbase;
} PROG;

LOCAL PROG	*progs = NULL;		/* the programs of the program box, as many as there are */
LOCAL INT	nprogs = -1;

typedef struct {
	char	id[48];
	UB	*ico;
	INT	len;
} ICON;

LOCAL ICON	icons[ICON_KEEP];

/* The programs of the program box, every one: their ids and their templates */
LOCAL void progs_read( void )
{
	TS_UUID		box, *lk;
	INT		n = 0, all = 0, i;

	nprogs = 0;
	if ( ob_fnd_sys(SYSROLE_PROGS, SYSDEF_PROG_BOX, &box) < E_OK ) return;
	/* every link of the box: asked how many, then taken */
	if ( ob_lst_lnk(&box, NULL, NULL, 0, &n) < E_OK || n <= 0 ) return;
	lk = (TS_UUID *)malloc(sizeof(TS_UUID) * (size_t)n);
	progs = (PROG *)calloc((size_t)n, sizeof(PROG));
	if ( lk == NULL || progs == NULL || ob_lst_lnk(&box, NULL, lk, n, &all) < E_OK ) {
		free(lk);
		return;
	}
	if ( all < n ) n = all;
	for ( i = 0; i < n; i++ ) {
		UB		*meta = NULL;
		TS_UUID		u = lk[i];
		T_JSON		root, tf, pg, base, it;
		INT		asz;
		ID		kp;
		PROG		*g = &progs[nprogs];

		kp = ob_opn_obj(&u, OB_OP_ATRRD);
		if ( kp <= 0 ) continue;
		asz = ob_atr_read(kp, 0, &meta);
		if ( asz >= 0 && js_parse(meta, asz, &root) >= E_OK
		  && js_get(&root, "tessronos", &tf) >= E_OK && js_get(&tf, "program", &pg) >= E_OK
		  && js_get_str(&pg, "id", (UB *)g->id, sizeof(g->id)) > 0 ) {
			char	bs[48];

			g->hasbase = FALSE;
			it.s = NULL;
			if ( js_get(&pg, "base", &base) >= E_OK && js_next(&base, &it)
			  && js_str(&it, (UB *)bs, sizeof(bs)) > 0 && unp_uuid_parse(bs, &g->base) ) {
				g->hasbase = TRUE;
			}
			nprogs++;
		}
		free(meta);
		ob_cls_obj(kp);
	}
	free(lk);
}

LOCAL PROG *prog_find( const char *id )
{
	INT	i;

	if ( nprogs < 0 ) progs_read();
	for ( i = 0; i < nprogs; i++ ) {
		if ( strcmp(progs[i].id, id) == 0 ) return &progs[i];
	}
	return NULL;
}

/* The icon of a program's template, kept once read; NULL when there is none */
LOCAL const UB *icon_of( const char *id, INT *p_len )
{
	PROG	*g = prog_find(id);
	INT	i, free_at = -1;
	SZ	asz = 0;
	ID	k;
	UB	*buf;

	*p_len = 0;
	for ( i = 0; i < ICON_KEEP; i++ ) {
		if ( icons[i].ico != NULL && strcmp(icons[i].id, id) == 0 ) {
			*p_len = icons[i].len;
			return icons[i].ico;
		}
		if ( icons[i].ico == NULL && free_at < 0 ) free_at = i;
	}
	if ( g == NULL || !g->hasbase || free_at < 0 ) return NULL;
	k = ob_opn_obj(&g->base, OB_OP_ATRRD);
	if ( k <= 0 ) return NULL;
	buf = malloc(OB_ICO_MAX);
	if ( buf != NULL && ob_get_ico(k, buf, OB_ICO_MAX, &asz) >= E_OK && asz > 0 ) {
		strncpy(icons[free_at].id, id, sizeof(icons[free_at].id) - 1);
		icons[free_at].ico = buf;
		icons[free_at].len = (INT)asz;
		*p_len = (INT)asz;
	} else {
		free(buf);
		buf = NULL;
	}
	ob_cls_obj(k);
	return buf;
}

/* ---------------------------------------------------------------- the metadata */

#define DEF_W		600
#define DEF_H		400

LOCAL INT clamp( INT v, INT lo, INT hi )
{
	return ( v < lo ) ? lo : ( v > hi ) ? hi : v;
}

/* The first of a file's TAD records, or NULL */
LOCAL const BPKREC *first_tad( const BPKFILE *f )
{
	INT	i;

	for ( i = 0; i < f->nrec; i++ ) {
		if ( f->rec[i].type == BPK_RT_TAD ) return &f->rec[i];
	}
	return NULL;
}

/*
 * One program more for a file's applist, once and only when the system
 * has it: its place in the list, or -1
 */
#define APPL_MAX	16

LOCAL INT appl_put( const char **ids, const char **names, INT *n, const UH *appl )
{
	const char	*name, *id = bpk_appl_prog(appl, &name);
	INT		k;

	if ( id == NULL || prog_find(id) == NULL ) return -1;
	for ( k = 0; k < *n && strcmp(ids[k], id) != 0; k++ ) ;
	if ( k == *n ) {
		if ( *n >= APPL_MAX ) return -1;
		ids[k] = id;
		names[k] = name;
		(*n)++;
	}
	return k;
}

/*
 * The programs that open a file. Its 実行機能付箋 records name those it
 * can be opened with, in their order. A link to it names the one it is
 * opened with from there (VLINK.appl, which BTRON keeps as the link's
 * own choice among them): that one opens it, and joins the list when the
 * records do not name it. With no link that names one, the first the
 * records name opens it. Only programs the system has are listed. With
 * none of them, a マイクロカード opens in マイクロカード, a figure in
 * 基本図形編集 and anything else in 基本文章編集.
 */
LOCAL const char *put_applist( BPKBUF *b, const BPKFILE *f )
{
	const char	*first = NULL, *ids[APPL_MAX], *names[APPL_MAX];
	INT		i, n = 0, dflt = -1;
	const BPKREC	*t;

	for ( i = 0; i < f->nexec; i++ ) (void)appl_put(ids, names, &n, f->exec[i].appl);
	if ( f->vappl[0] != 0 ) dflt = appl_put(ids, names, &n, f->vappl);
	if ( n > 0 && dflt < 0 ) dflt = 0;
	bpk_buf_puts(b, "{");
	for ( i = 0; i < n; i++ ) {
		bpk_buf_printf(b, "%s\"%s\":{\"name\":\"%s\",\"defaultOpen\":%s}", i ? "," : "", ids[i], names[i],
			       ( i == dflt ) ? "true" : "false");
	}
	if ( n > 0 ) first = ids[dflt];
	if ( first == NULL && bpk_mc_file(f) && prog_find(BPK_MC_PROG) != NULL ) {
		bpk_buf_printf(b, "\"%s\":{\"name\":\"%s\",\"defaultOpen\":true},"
			       "\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":false}", BPK_MC_PROG, BPK_MC_NAME);
		first = BPK_MC_PROG;
	}
	if ( first == NULL ) {
		t = first_tad(f);
		if ( t != NULL && bpk_tad_is_fig(t->data, t->size) ) {
			bpk_buf_puts(b, "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":true},"
				   "\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":false}");
			first = "basic-figure-editor";
		} else {
			bpk_buf_puts(b, "\"basic-text-editor\":{\"name\":\"基本文章編集\",\"defaultOpen\":true},"
				   "\"basic-figure-editor\":{\"name\":\"基本図形編集\",\"defaultOpen\":false}");
			first = "basic-text-editor";
		}
		bpk_buf_puts(b, ",\"virtual-object-list\":{\"name\":\"仮身一覧\",\"defaultOpen\":false}");
	}
	bpk_buf_puts(b, "}");
	return first;
}

/*
 * The times the local header keeps. The period of keeping goes into the
 * metadata as periodDate (BPK_NO_TIME none). The store gives an object
 * made here its own times of making, changing and looking at, so the
 * ones the archive had are kept beside them as "tessronos":{"btron":
 * {"ctime","mtime","atime"}}, as バックアップ keeps them, for a book
 * made again to carry. A time of 0 says nothing and is left out.
 */
#define BPK_NO_TIME	0xFFFFFFFFU

LOCAL void put_times( BPKBUF *b, const BPKLHEAD *h )
{
	char	t[24];
	INT	n = 0;

	if ( h->f_ltime != 0 && h->f_ltime != BPK_NO_TIME && bpk_stime_iso(h->f_ltime, t) > 0 ) {
		bpk_buf_printf(b, "\"periodDate\":\"%s\",", t);
	}
	if ( h->f_ctime == 0 && h->f_mtime == 0 && h->f_atime == 0 ) return;
	bpk_buf_puts(b, "\"tessronos\":{\"btron\":{");
	if ( h->f_ctime != 0 && bpk_stime_iso(h->f_ctime, t) > 0 ) bpk_buf_printf(b, "%s\"ctime\":\"%s\"", n++ ? "," : "", t);
	if ( h->f_mtime != 0 && bpk_stime_iso(h->f_mtime, t) > 0 ) bpk_buf_printf(b, "%s\"mtime\":\"%s\"", n++ ? "," : "", t);
	if ( h->f_atime != 0 && bpk_stime_iso(h->f_atime, t) > 0 ) bpk_buf_printf(b, "%s\"atime\":\"%s\"", n++ ? "," : "", t);
	bpk_buf_puts(b, "}},");
}

/* A file's metadata, and the program that opens it first; maker says who made the object */
LOCAL const char *meta_of( BPKBUF *b, const BPKFILE *f, const char *maker )
{
	const BPKEXEC	*win = NULL, *bg = NULL, *sv = NULL;
	const BPKREC	*t = first_tad(f);
	const char	*first;
	char		colour[8] = "#ffffff";
	INT		i, x = 100, y = 100, w = DEF_W, h = DEF_H;

	for ( i = f->nexec - 1; i >= 0; i-- ) {
		if ( f->exec[i].haswin && f->exec[i].wr > f->exec[i].wl && f->exec[i].wb > f->exec[i].wt ) win = &f->exec[i];
		if ( f->exec[i].hasbg ) bg = &f->exec[i];
		if ( f->exec[i].hassv ) sv = &f->exec[i];
	}
	if ( win != NULL ) {
		/* the window the archive kept, brought onto a screen of this size */
		x = clamp(win->wl, 0, 400);
		y = clamp(win->wt, 0, 300);
		w = clamp(win->wr - win->wl, 240, 960);
		h = clamp(win->wb - win->wt, 160, 640);
	}
	if ( bg != NULL ) {
		UINT	*cmap = malloc(sizeof(UINT) * BPK_CMAP_MAX);
		INT	n = ( cmap != NULL && t != NULL ) ? bpk_tad_cmap(t->data, t->size, cmap, BPK_CMAP_MAX) : 0;

		bpk_colour(bg->bgraw, cmap, n, colour);
		free(cmap);
	}
	bpk_buf_puts(b, "{\"name\":\"");
	bpk_buf_json(b, f->name);
	bpk_buf_printf(b, "\",\"relationship\":[],\"linktype\":false,\"refCount\":0,"
		       "\"editable\":%s,\"deletable\":%s,\"readable\":true,\"maker\":\"",
		       ( f->head.f_type & BPK_F_RONLY ) ? "false" : "true", ( f->head.f_type & BPK_F_PERM ) ? "false" : "true");
	bpk_buf_json(b, ( maker != NULL && maker[0] != 0 ) ? maker : "書庫管理");
	bpk_buf_puts(b, "\",");
	put_times(b, &f->head);
	bpk_buf_printf(b, "\"window\":{\"pos\":{\"x\":%d,\"y\":%d},\"width\":%d,\"height\":%d,\"backgroundColor\":\"%s\"},",
		  x, y, w, h, colour);
	bpk_buf_puts(b, "\"applist\":");
	first = put_applist(b, f);
	if ( sv != NULL ) {
		bpk_buf_puts(b, ",\"microscriptSV\":[");
		for ( i = 0; i < 50; i++ ) bpk_buf_printf(b, "%s%d", i ? "," : "", sv->sv[i]);
		bpk_buf_puts(b, "]");
	}
	bpk_buf_puts(b, "}");
	return first;
}

/* ---------------------------------------------------------------- the hooks of the converter */

/* What the n-th link record of the object being written points at */
LOCAL const char *hook_target( void *ctx, INT n )
{
	UNPJOB		*j = ctx;
	const BPKFILE	*f = &j->arc.file[j->curfile];
	INT		t = ( n >= 0 && n < f->nlink ) ? f->link[n] : -1;

	return ( t >= 0 && t < j->nmade ) ? j->ids[t] : NULL;
}

LOCAL void hook_vobjid( void *ctx, char *out )
{
	TS_UUID	u;

	memset(&u, 0, sizeof(u));
	if ( ts_gen_uuid(&u) < E_OK ) {
		/* no clock yet: any identity unique among these will do */
		UNPJOB	*j = ctx;

		u = j->made[0];
		u.b[15] ^= (UB)( ++j->vseq );
		u.b[14] ^= (UB)( j->vseq >> 8 );
	}
	unp_uuid_str(&u, out);
}

LOCAL ER hook_picture( void *ctx, INT recno, INT n, const UB *png, INT len )
{
	UNPJOB	*j = ctx;
	char	name[OB_RES_NAME];

	snprintf(name, sizeof(name), "_%d_%d.png", recno, n);
	j->npic++;
	return ob_wri_res(j->curkey, (const UB *)name, png, len);
}

/* ---------------------------------------------------------------- the work */

LOCAL void seterr( UNPJOB *j, const char *s, ER er )
{
	snprintf(j->err, sizeof(j->err), "%s (%d)", s, (INT)er);
}

/* Every link of an xmlTAD counted in its target, for a store that does not */
LOCAL ER count_links( const char *xml )
{
	const char	*p;
	ER		er;

	for ( p = xml; ( p = strstr(p, "<link id=\"") ) != NULL; p++ ) {
		TS_UUID	u;

		if ( !unp_uuid_parse(p + 10 + ts_lnk_split((const UB *)p + 10, (INT)strlen(p + 10), NULL, NULL), &u) ) continue;
		er = ob_lnk_obj(&u);
		if ( er < E_OK ) return er;
	}
	return E_OK;
}

/* The objects made so far taken away again */
LOCAL void undo( UNPJOB *j )
{
	INT	i;

	for ( i = j->nmade - 1; i >= 0; i-- ) (void)ob_del_obj(&j->made[i]);
	j->nmade = 0;
}

EXPORT ER unp_job_load( UNPJOB *j, const TS_UUID *archive )
{
	UB		*meta = NULL;
	T_OBREC		*r;
	T_OBREF		ref;
	T_JSON		root;
	INT		cnt = 0, i, asz;
	ID		k;
	ER		er = E_OBJ;

	memset(j, 0, sizeof(*j));
	j->archive = *archive;
	if ( ob_ref_obj(archive, &ref) < E_OK ) {
		seterr(j, "書庫の実身がありません", E_NOEXS);
		return E_NOEXS;
	}
	k = ob_opn_obj(archive, OB_OP_R);
	if ( k <= 0 ) {
		seterr(j, "書庫の実身を開けません", (ER)k);
		return (ER)k;
	}
	asz = ob_atr_read(k, 0, &meta);
	if ( asz >= 0 && js_parse(meta, asz, &root) >= E_OK ) {
		(void)js_get_str(&root, "name", (UB *)j->name, sizeof(j->name));
	}
	free(meta);
	/* the archive's bytes: the first record of plain data that is one */
	r = ob_rec_read(k, &cnt);
	for ( i = 0; i < cnt && er < E_OK; i++ ) {
		if ( r[i].rt != OB_RT_SYSDATA ) continue;
		free(j->raw);
		j->raw = unp_read_rec(k, r[i].recno, &j->rawlen);
		if ( j->raw == NULL ) continue;
		bpk_free(&j->arc);
		er = bpk_parse(&j->arc, j->raw, j->rawlen, j->err, sizeof(j->err));
	}
	free(r);
	ob_cls_obj(k);
	if ( er < E_OK && j->err[0] == 0 ) seterr(j, "書庫のデータがありません", er);
	return er;
}

EXPORT ER unp_job_run( UNPJOB *j, const TS_UUID *into, void (*progress)( UNPJOB *, INT, INT ) )
{
	BPKARC		*a = &j->arc;
	BPKHOOK		hook;
	BPKCONV		*cv = NULL;
	BPKBUF		b;
	T_OBCRE		c;
	T_OBREF		ref;
	INT		i, r, total = (INT)a->nfiles * 2;
	ID		k;
	ER		er = E_OK;

	/* a set of its own each time: what the last carry made is not touched */
	free(j->made);
	free(j->ids);
	j->made = NULL;
	j->ids = NULL;
	j->nmade = j->npic = j->noicon = 0;
	j->nskipped = 0;
	j->nskip = 0;
	j->vseq = 0;
	memset(&j->root, 0, sizeof(j->root));
	if ( ob_ref_obj(into, &ref) < E_OK ) {
		seterr(j, "置き先の実身がありません", E_NOEXS);
		return E_NOEXS;
	}
	j->autoref = (BOOL)( ( ref.flags & OB_F_AUTOREF ) != 0 );
	j->made = calloc(a->nfiles, sizeof(TS_UUID));
	j->ids = calloc(a->nfiles, sizeof(*j->ids));
	cv = malloc(sizeof(BPKCONV));
	if ( j->made == NULL || j->ids == NULL || cv == NULL ) {
		free(cv);
		seterr(j, "作業の場所がありません", E_NOMEM);
		return E_NOMEM;
	}

	/* 1: the objects, all of them before any record */
	for ( i = 0; i < (INT)a->nfiles; i++ ) {
		const char	*prog;
		const UB	*ico;
		INT		icolen = 0;

		bpk_buf_init(&b);
		prog = meta_of(&b, &a->file[i], j->maker);
		ico = icon_of(prog, &icolen);
		if ( ico == NULL ) ico = icon_of("basic-text-editor", &icolen);
		if ( ico == NULL ) j->noicon++;
		if ( b.fail || b.n > OB_META_LIMIT ) {
			bpk_buf_free(&b);
			er = E_LIMIT;
			seterr(j, "管理情報が大きすぎます", er);
			break;
		}
		memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.json = (const UB *)b.s;
		c.jsonsz = b.n;
		c.near = *into;
		c.icon = ico;
		c.iconsz = ( ico != NULL ) ? icolen : 0;
		er = ob_cre_obj(&c, &j->made[i]);
		bpk_buf_free(&b);
		if ( er < E_OK ) {
			seterr(j, "実身を作れません", er);
			break;
		}
		unp_uuid_str(&j->made[i], j->ids[i]);
		j->nmade = i + 1;
		if ( progress != NULL ) progress(j, i + 1, total);
	}

	/* 2: the records, the TAD ones first as xmlTAD */
	hook.link_target = hook_target;
	hook.new_vobjid = hook_vobjid;
	hook.picture = hook_picture;
	hook.ctx = j;
	bpk_conv_init(cv, &hook);
	for ( i = 0; er >= E_OK && i < (INT)a->nfiles; i++ ) {
		const BPKFILE	*f = &a->file[i];
		INT		t = 0;
		BOOL		archive = FALSE, mc = bpk_mc_file(f);

		k = ob_opn_obj(&j->made[i], OB_OP_R | OB_OP_W | OB_O_EXCL);	/* filled record by record */
		if ( k <= 0 ) {
			er = (ER)k;
			seterr(j, "作った実身を開けません", er);
			break;
		}
		j->curkey = k;
		j->curfile = i;
		bpk_conv_object(cv);
		if ( mc ) {
			/* マイクロカード実身: record 0 names the link records' objects in their order */
			bpk_buf_init(&b);
			bpk_mc_xml(&b, f->name, f->nlink, &hook);
			er = b.fail ? E_NOMEM : add_rec(k, OB_RT_TAD, 0, b.s, (UINT)b.n);
			if ( er >= E_OK && !j->autoref ) er = count_links(b.s);
			bpk_buf_free(&b);
			if ( er < E_OK ) seterr(j, "レコードを書けません", er);
			t = 1;
		}
		for ( r = 0; !mc && er >= E_OK && r < f->nrec; r++ ) {
			if ( f->rec[r].type != BPK_RT_TAD || !bpk_tad_is_doc(f->rec[r].data, f->rec[r].size) ) continue;
			bpk_buf_init(&b);
			er = bpk_tad_to_xml(cv, f->name, j->ids[i], t++, f->rec[r].data, f->rec[r].size, &b);
			if ( er >= E_OK ) er = add_rec(k, OB_RT_TAD, 0, b.s, (UINT)b.n);
			if ( er >= E_OK && !j->autoref ) er = count_links(b.s);
			bpk_buf_free(&b);
			if ( er < E_OK ) seterr(j, "レコードを書けません", er);
		}
		if ( er >= E_OK && t == 0 ) {
			/* nothing to show: a record 0 that says what it is */
			bpk_buf_init(&b);
			bpk_buf_puts(&b, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
			bpk_buf_xml(&b, f->name);
			bpk_buf_puts(&b, "\"><document><p>");
			bpk_buf_xml(&b, f->name);
			bpk_buf_puts(&b, "</p></document></tad>\n");
			er = b.fail ? E_NOMEM : add_rec(k, OB_RT_TAD, 0, b.s, (UINT)b.n);
			bpk_buf_free(&b);
			if ( er < E_OK ) seterr(j, "レコードを書けません", er);
		}
		/*
		 * The rest as they were: an archive inside keeps its TAD too, to
		 * be opened in turn, and so does a TAD record that is neither a
		 * text nor a figure.
		 */
		for ( r = 0; r < f->nexec; r++ ) {
			if ( f->exec[r].appl[1] == BPK_APPL_ARCHIVE ) archive = TRUE;
		}
		for ( r = 0; er >= E_OK && r < f->nrec; r++ ) {
			UH	ty = f->rec[r].type;

			if ( ty == BPK_RT_LINK || ty == BPK_RT_EXEC
			  || ( ty == BPK_RT_TAD && !archive && !mc && bpk_tad_is_doc(f->rec[r].data, f->rec[r].size) ) ) continue;
			er = add_rec(k, OB_RT_SYSDATA, f->rec[r].sub, f->rec[r].data, f->rec[r].size);
			if ( er < E_OK ) seterr(j, "レコードを書けません", er);
		}
		ob_cls_obj(k);
		j->curkey = 0;
		if ( progress != NULL ) progress(j, (INT)a->nfiles + i + 1, total);
	}
	if ( cv->er < E_OK && er >= E_OK ) {
		er = cv->er;
		seterr(j, "画像を書けません", er);
	}
	j->nskipped = cv->nskipped;
	j->nskip = cv->nskip;
	memcpy(j->skip, cv->skip, sizeof(j->skip));
	free(cv);
	if ( er < E_OK ) {
		undo(j);
		return er;
	}
	j->root = j->made[a->root];
	if ( progress != NULL ) progress(j, total, total);
	return E_OK;
}

/* What the last run made taken away again: its link could not be put */
EXPORT void unp_job_undo( UNPJOB *j )
{
	undo(j);
}

/* ---------------------------------------------------------------- the desktop */

LOCAL UINT	ask_seq = 0;

/*
 * A request written to the desktop's channel and its answer waited for,
 * up to wait_ms. A carry is answered only when the hand lets go, which
 * takes as long as the user carries.
 */
LOCAL ER ask_desktop( T_DTREQ *rq, T_DTANS *an, INT wait_ms )
{
	TS_UUID	d, ch;
	T_OBCRE	c;
	SZ	asz = 0;
	ID	k, ka;
	ER	er;

	if ( ob_fnd_nam((const UB *)DT_REQ_NAME, &d) < E_OK ) return E_NOEXS;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) < E_OK ) return E_NOMEM;
	ka = ob_opn_obj(&ch, OB_OP_READ | OB_O_NOWAIT);
	if ( ka <= 0 ) {
		(void)ob_del_obj(&ch);
		return E_OBJ;
	}
	rq->seq = ++ask_seq;
	rq->reply = ch;
	k = ob_opn_obj(&d, OB_OP_WRITE);
	er = ( k > 0 ) ? ob_wri_rec(k, 0, 0, rq, sizeof(*rq), &asz) : (ER)k;
	if ( k > 0 ) ob_cls_obj(k);
	/* the answer waited for on its channel, at most wait_ms */
	while ( er >= E_OK ) {
		er = ob_rea_tmo(ka, 0, 0, an, sizeof(*an), &asz, wait_ms);
		if ( er >= E_OK && asz == (SZ)sizeof(*an) && an->seq == rq->seq ) {
			er = an->er;
			break;
		}
	}
	ob_cls_obj(ka);
	(void)ob_del_obj(&ch);
	return er;
}

EXPORT ER unp_carry( UNPJOB *j, const T_DTLOOK *look, INT grab_x, INT grab_y, TS_UUID *into, UINT *p_seq )
{
	const BPKARC	*a = &j->arc;
	T_DTREQ		rq;
	T_DTANS		an;
	char		size[24];
	ER		er;
	UINT		v = a->origsize, n;
	INT		k = 0, m;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_CARRY;
	/*
	 * over a program's window too, once it says it would take the link:
	 * the objects are made only then, and deleted when it cannot be put
	 */
	rq.flags = DT_F_TO_PROG | DT_F_ASK;
	rq.look = *look;
	rq.grab_x = grab_x;
	rq.grab_y = grab_y;
	{
		/* the root's name, cut at a letter's edge so that the line fits */
		char	nm[60];
		INT	l = 0;

		while ( a->file[a->root].name[l] != 0 && l < (INT)sizeof(nm) - 1 ) l++;
		while ( l > 0 && a->file[a->root].name[l] != 0 && ( a->file[a->root].name[l] & 0xC0 ) == 0x80 ) l--;
		memcpy(nm, a->file[a->root].name, (size_t)l);
		nm[l] = 0;
		snprintf((char *)rq.ask[0], sizeof(rq.ask[0]), "「%s」を解凍します。", nm);
	}
	/* 147780 as "147,780" */
	m = snprintf(size, sizeof(size), "%u", v);
	{
		char	g[32];

		for ( n = 0; n < (UINT)m; n++ ) {
			if ( n > 0 && ( m - (INT)n ) % 3 == 0 ) g[k++] = ',';
			g[k++] = size[n];
		}
		g[k] = 0;
		snprintf((char *)rq.ask[1], sizeof(rq.ask[1]), "実身 %u 個、%s バイト", a->nfiles, g);
	}
	snprintf((char *)rq.yes, sizeof(rq.yes), "解凍");
	memset(&an, 0, sizeof(an));
	er = ask_desktop(&rq, &an, 30 * 60 * 1000);
	if ( er >= E_OK ) {
		*into = an.into;
		*p_seq = rq.seq;
	}
	return er;
}

EXPORT ER unp_place( UNPJOB *j, const T_DTLOOK *look, UINT seq )
{
	T_DTREQ	rq;
	T_DTANS	an;

	memset(&rq, 0, sizeof(rq));
	rq.req = DT_RQ_PLACE;
	rq.target = j->root;
	rq.recno = -1;
	rq.look = *look;
	rq.carry = seq;
	memset(&an, 0, sizeof(an));
	return ask_desktop(&rq, &an, 5000);
}

EXPORT void unp_job_free( UNPJOB *j )
{
	bpk_free(&j->arc);
	free(j->raw);
	free(j->made);
	free(j->ids);
	j->raw = NULL;
	j->made = NULL;
	j->ids = NULL;
}
