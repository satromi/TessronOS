/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tray.c
 *	The tray (design 18.16)
 *
 *	The tray is a storage object on the volatile volume, made the first
 *	time it is used. Its records:
 *
 *	  0     the levels in words (xmlTAD), written again at every change,
 *	        so that opening the tray shows what it holds
 *	  1     the place of its own (TR_PUT)
 *	  2-12  the sets of the levels, in no order: which record holds which
 *	        level is kept here and in the metadata ("tessronos.tray"). One
 *	        more than the levels, so that a set can be written whole to a
 *	        spare record before it takes the place of the one it grows from
 *
 *	A set is one record, whole: a head (how many tray records, when, a
 *	name), an entry for each tray record (what it is, where its bytes
 *	start, how many), and the bytes after the entries. Numbers are
 *	little-endian and written a byte at a time, so a set reads the same
 *	wherever it is copied to.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/ob.h>
#include <ts/dt.h>
#include <ts/sysdef.h>
#include <ts/tray.h>

#define TR_SLOTS	( TR_LEVELS + 3 )	/* the words, the place, the levels, a spare */
#define SLOT_PUT	1
#define SLOT_FIRST	2
#define HEAD_SIZE	( 4 + 4 + 8 + TR_NAME_MAX )
#define ENT_SIZE	( 4 * 4 + 8 + 8 )
#define TR_MAGIC	0x31535254		/* "TRS1" */
#define INDEX_MAX	4096

LOCAL ID	tr_key = 0;			/* the tray, open for good once made */
LOCAL ID	tr_mtx = 0;
LOCAL INT	tr_lv[TR_LEVELS + 1];		/* the record of each level, 0 none */
LOCAL INT	tr_hand = 0;

/* ---------------------------------------------------------------- bytes */

LOCAL void wr32( UB *p, UW v )
{
	p[0] = (UB)v;  p[1] = (UB)( v >> 8 );  p[2] = (UB)( v >> 16 );  p[3] = (UB)( v >> 24 );
}

LOCAL UW rd32( CONST UB *p )
{
	return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 );
}

LOCAL void wr64( UB *p, UD v )
{
	wr32(p, (UW)v);
	wr32(p + 4, (UW)( v >> 32 ));
}

LOCAL UD rd64( CONST UB *p )
{
	return (UD)rd32(p) | ( (UD)rd32(p + 4) << 32 );
}

LOCAL INT put( UB *b, INT at, INT max, CONST char *s )
{
	while ( at >= 0 && *s != 0 && at < max - 1 ) {
		b[at++] = (UB)*s++;
	}
	if ( at >= 0 && at < max ) b[at] = 0;
	return at;
}

LOCAL INT put_num( UB *b, INT at, INT max, INT v )
{
	char	d[12];
	INT	n = 0;

	do {
		d[n++] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 );
	while ( n > 0 && at < max - 1 ) {
		b[at++] = (UB)d[--n];
	}
	b[at] = 0;
	return at;
}

/* Text as the content of an XML element */
LOCAL INT put_xml( UB *b, INT at, INT max, CONST UB *s )
{
	for ( ; *s != 0 && at < max - 6; s++ ) {
		switch ( *s ) {
		case '&':	at = put(b, at, max, "&amp;");	break;
		case '<':	at = put(b, at, max, "&lt;");	break;
		case '>':	at = put(b, at, max, "&gt;");	break;
		default:	b[at++] = *s;			break;
		}
	}
	b[at] = 0;
	return at;
}

/* ---------------------------------------------------------------- the object */

LOCAL void lock( void )
{
	if ( tr_mtx == 0 ) {
		T_CMTX	c;
		ID	id;

		c.exinf = NULL;
		c.mtxatr = TA_INHERIT;
		c.ceilpri = 0;
		id = tk_cre_mtx(&c);
		/* the first to set it wins; tasks on other processors race here */
		if ( atomic_cmpxchg((UW *)&tr_mtx, (UW)id, 0) == 0 ) {
			id = 0;
		}
		if ( id > 0 ) tk_del_mtx(id);	/* another task made it first */
	}
	(void)tk_loc_mtx(tr_mtx, TMO_FEVR);
}

LOCAL void unlock( void )
{
	(void)tk_unl_mtx(tr_mtx);
}

/* The tray, made the first time it is asked for */
LOCAL ER ready( void )
{
	CONST char *meta = "{\"name\":\"トレー\",\"refCount\":1,\"recordCount\":13,"
			   "\"tessronos\":{\"tray\":{\"hand\":0,\"levels\":[]}}}";
	T_OBCRE	c;
	T_OBREF	r;
	TS_UUID	u, got;
	INT	i, rec;
	ER	er;

	if ( tr_key > 0 ) {
		return E_OK;
	}
	er = ts_str_to_uuid(SYSDEF_TRAY, &u);
	if ( er < E_OK ) {
		return er;
	}
	if ( ob_ref_obj(&u, &r) < E_OK ) {
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_MEMORY;
		c.name = (CONST UB *)"トレー";
		c.json = (CONST UB *)meta;
		for ( i = 0; meta[i] != 0; i++ ) ;
		c.jsonsz = i;
		c.uuid = u;
		er = ob_cre_obj(&c, &got);
		if ( er < E_OK ) {
			return er;
		}
		tr_key = ob_opn_obj(&u, OB_OP_ALL);
		if ( tr_key <= 0 ) {
			return (ER)tr_key;
		}
		for ( i = 0; i < TR_SLOTS && er >= E_OK; i++ ) {
			er = ob_apd_rec(tr_key, ( i == 0 ) ? OB_RT_TAD : OB_RT_SYSDATA, 0, &rec);
		}
		return er;
	}
	tr_key = ob_opn_obj(&u, OB_OP_ALL);
	return ( tr_key > 0 ) ? E_OK : (ER)tr_key;
}

LOCAL INT nlevel( void )
{
	INT	n = 0;

	while ( n < TR_LEVELS && tr_lv[n + 1] != 0 ) n++;
	return n;
}

/* The record a set is in: TR_PUT, a level, or the one in hand */
LOCAL INT slot_of( INT level )
{
	if ( level == TR_HAND ) {
		level = tr_hand;
		if ( level == 0 ) return 0;
	}
	if ( level == TR_PUT ) return SLOT_PUT;
	if ( level < 1 || level > TR_LEVELS ) return 0;
	return tr_lv[level];
}

LOCAL ER read_head( INT slot, UB *head )
{
	SZ	asz = 0;
	ER	er;

	er = ob_rea_rec(tr_key, slot, 0, head, HEAD_SIZE, &asz);
	if ( er < E_OK ) return er;
	if ( asz < HEAD_SIZE || rd32(head) != TR_MAGIC ) return E_NOEXS;
	return E_OK;
}

LOCAL ER read_ent( INT slot, INT rec, UB *ent )
{
	UB	head[HEAD_SIZE];
	SZ	asz = 0;
	ER	er;

	er = read_head(slot, head);
	if ( er < E_OK ) return er;
	if ( rec < 0 || rec >= (INT)rd32(head + 4) ) return E_NOEXS;
	er = ob_rea_rec(tr_key, slot, HEAD_SIZE + rec * ENT_SIZE, ent, ENT_SIZE, &asz);
	return ( er >= E_OK && asz == ENT_SIZE ) ? E_OK : E_IO;
}

/* ---------------------------------------------------------------- what the tray says of itself */

LOCAL CONST char *kind_word( UINT kinds )
{
	if ( kinds == ( 1U << TR_TAD ) ) return "文章・図形・仮身";
	if ( kinds == ( 1U << TR_PNG ) ) return "画像";
	if ( kinds == ( 1U << TR_REC ) ) return "レコード";
	return "複数の種類";
}

LOCAL ER set_info( INT slot, T_TRSET *s )
{
	UB	head[HEAD_SIZE], ent[ENT_SIZE];
	INT	i, k;
	ER	er;

	knl_memset(s, 0, sizeof(*s));
	if ( slot == 0 || read_head(slot, head) < E_OK ) {
		return E_OK;				/* nothing there */
	}
	s->nrec = (INT)rd32(head + 4);
	s->time = (D)rd64(head + 8);
	for ( k = 0; k < TR_NAME_MAX - 1 && head[16 + k] != 0; k++ ) {
		s->name[k] = head[16 + k];
	}
	s->name[k] = 0;
	for ( i = 0; i < s->nrec; i++ ) {
		er = read_ent(slot, i, ent);
		if ( er < E_OK ) return er;
		s->kinds |= 1U << rd32(ent);
	}
	return E_OK;
}

/* Record 0 and the metadata, after a change */
LOCAL void tell( void )
{
	T_TRSET	s;
	TS_TM	tm;
	TS_TIME	t;
	UB	*b, when[32];
	SZ	asz = 0;
	INT	at = 0, i, n = nlevel();

	b = (UB *)Kmalloc(INDEX_MAX);
	if ( b == NULL ) {
		return;
	}
	at = put(b, at, INDEX_MAX, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"トレー\">"
		 "<document><p>トレー</p>");
	if ( n == 0 ) {
		at = put(b, at, INDEX_MAX, "<p>空です。</p>");
	}
	for ( i = 1; i <= n; i++ ) {
		(void)set_info(tr_lv[i], &s);
		t = s.time;
		when[0] = 0;
		if ( dt_localtime(&t, &tm) >= E_OK ) {
			(void)dt_strftime((char *)when, sizeof(when), "%m/%d %H:%M", &tm);
		}
		at = put(b, at, INDEX_MAX, ( i == tr_hand ) ? "<p>▶" : "<p>　");
		at = put_num(b, at, INDEX_MAX, i);
		at = put(b, at, INDEX_MAX, " ");
		at = put_xml(b, at, INDEX_MAX, s.name);
		at = put(b, at, INDEX_MAX, "(");
		at = put(b, at, INDEX_MAX, kind_word(s.kinds));
		at = put(b, at, INDEX_MAX, " ");
		at = put_xml(b, at, INDEX_MAX, when);
		at = put(b, at, INDEX_MAX, ")</p>");
	}
	at = put(b, at, INDEX_MAX, "</document></tad>");
	(void)ob_wri_rec(tr_key, 0, 0, b, at, &asz);
	(void)ob_trn_rec(tr_key, 0, (UD)at);

	/* which record holds which level */
	at = put(b, 0, INDEX_MAX, "{\"name\":\"トレー\",\"refCount\":1,\"recordCount\":13,"
		 "\"tessronos\":{\"tray\":{\"hand\":");
	at = put_num(b, at, INDEX_MAX, tr_hand);
	at = put(b, at, INDEX_MAX, ",\"levels\":[");
	for ( i = 1; i <= n; i++ ) {
		if ( i > 1 ) at = put(b, at, INDEX_MAX, ",");
		at = put_num(b, at, INDEX_MAX, tr_lv[i]);
	}
	at = put(b, at, INDEX_MAX, "]}}}");
	(void)ob_set_atr(tr_key, b, at);
	Kfree(b);
}

/* ---------------------------------------------------------------- writing a set */

/* A set made of old records (from slot, when not 0) and new ones, written to 'to' */
LOCAL ER write_set( INT to, INT from, CONST T_TRREC *recs, INT n, CONST UB *name )
{
	UB	head[HEAD_SIZE], ent[ENT_SIZE], *b;
	INT	old = 0, i, k;
	SZ	total, off, asz = 0;
	TS_TIME	now = 0;
	ER	er = E_OK;

	if ( from != 0 && read_head(from, head) >= E_OK ) {
		old = (INT)rd32(head + 4);
	} else {
		from = 0;
		knl_memset(head, 0, sizeof(head));
		for ( k = 0; name != NULL && name[k] != 0 && k < TR_NAME_MAX - 1; k++ ) {
			head[16 + k] = name[k];
		}
	}
	if ( old + n > TR_REC_MAX ) {
		return E_LIMIT;
	}
	total = HEAD_SIZE + ( old + n ) * ENT_SIZE;
	for ( i = 0; i < old; i++ ) {
		if ( read_ent(from, i, ent) < E_OK ) return E_IO;
		total += (SZ)rd64(ent + 24);
	}
	for ( i = 0; i < n; i++ ) {
		if ( recs[i].size < 0 || ( recs[i].size > 0 && recs[i].data == NULL ) ) {
			return E_PAR;
		}
		total += recs[i].size;
	}
	b = (UB *)Kmalloc(total);
	if ( b == NULL ) {
		return E_NOMEM;
	}
	(void)dt_gettime(&now);
	wr32(b, TR_MAGIC);
	wr32(b + 4, (UW)( old + n ));
	wr64(b + 8, (UD)now);
	knl_memcpy(b + 16, head + 16, TR_NAME_MAX);
	off = HEAD_SIZE + ( old + n ) * ENT_SIZE;
	for ( i = 0; i < old && er >= E_OK; i++ ) {
		UB	*e = b + HEAD_SIZE + i * ENT_SIZE;
		SZ	size;

		er = read_ent(from, i, ent);
		size = (SZ)rd64(ent + 24);
		knl_memcpy(e, ent, ENT_SIZE);
		wr64(e + 16, (UD)off);
		if ( er >= E_OK && size > 0 ) {
			er = ob_rea_rec(tr_key, from, (D)rd64(ent + 16), b + off, size, &asz);
		}
		off += size;
	}
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		UB	*e = b + HEAD_SIZE + ( old + i ) * ENT_SIZE;

		wr32(e, recs[i].kind);
		wr32(e + 4, recs[i].rt);
		wr32(e + 8, recs[i].sub);
		wr32(e + 12, 0);
		wr64(e + 16, (UD)off);
		wr64(e + 24, (UD)recs[i].size);
		if ( recs[i].size > 0 ) {
			knl_memcpy(b + off, recs[i].data, (INT)recs[i].size);
		}
		off += recs[i].size;
	}
	if ( er >= E_OK ) {
		er = ob_wri_rec(tr_key, to, 0, b, total, &asz);
	}
	if ( er >= E_OK ) {
		er = ob_trn_rec(tr_key, to, (UD)total);
	}
	Kfree(b);

	return er;
}

LOCAL BOOL kinds_ok( CONST T_TRREC *recs, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( recs[i].kind < TR_TAD || recs[i].kind > TR_REC ) return FALSE;
	}
	return TRUE;
}

/* A record no level holds */
LOCAL INT free_slot( void )
{
	INT	s, i;
	BOOL	used;

	for ( s = SLOT_FIRST; s < TR_SLOTS; s++ ) {
		used = FALSE;
		for ( i = 1; i <= TR_LEVELS && !used; i++ ) {
			used = (BOOL)( tr_lv[i] == s );
		}
		if ( !used ) return s;
	}
	return 0;
}

/* ---------------------------------------------------------------- the calls */

EXPORT ER tr_psh_dat( CONST T_TRREC *recs, INT n, CONST UB *name )
{
	INT	slot, i;
	ER	er;

	if ( recs == NULL || n <= 0 || n > TR_REC_MAX || !kinds_ok(recs, n) ) {
		return E_PAR;
	}
	lock();
	er = ready();
	if ( er >= E_OK ) {
		/* the oldest falls off when every level is full */
		if ( nlevel() == TR_LEVELS ) {
			(void)ob_trn_rec(tr_key, tr_lv[TR_LEVELS], 0);
			tr_lv[TR_LEVELS] = 0;
		}
		slot = free_slot();
		er = ( slot > 0 ) ? write_set(slot, 0, recs, n, name) : E_LIMIT;
		if ( er >= E_OK ) {
			for ( i = TR_LEVELS; i > 1; i-- ) {
				tr_lv[i] = tr_lv[i - 1];
			}
			tr_lv[1] = slot;
			tr_hand = 1;
			tell();
		}
	}
	unlock();
	return ( er >= E_OK ) ? (ER)n : er;
}

EXPORT ER tr_add_dat( CONST T_TRREC *recs, INT n )
{
	INT	slot;
	ER	er;

	if ( recs == NULL || n <= 0 || !kinds_ok(recs, n) ) {
		return E_PAR;
	}
	lock();
	er = ready();
	slot = slot_of(TR_HAND);
	if ( er >= E_OK && slot == 0 ) {
		er = E_NOEXS;
	}
	if ( er >= E_OK ) {
		/* written whole to a spare record, then that record becomes the level */
		INT	spare = free_slot();

		er = ( spare > 0 ) ? write_set(spare, slot, recs, n, NULL) : E_LIMIT;
		if ( er >= E_OK ) {
			(void)ob_trn_rec(tr_key, slot, 0);
			tr_lv[tr_hand] = spare;
			tell();
		}
	}
	unlock();
	return ( er >= E_OK ) ? (ER)n : er;
}

EXPORT ER tr_set_dat( CONST T_TRREC *recs, INT n )
{
	ER	er;

	if ( n < 0 || n > TR_REC_MAX || ( n > 0 && ( recs == NULL || !kinds_ok(recs, n) ) ) ) {
		return E_PAR;
	}
	lock();
	er = ready();
	if ( er >= E_OK ) {
		er = ( n == 0 ) ? ob_trn_rec(tr_key, SLOT_PUT, 0)
				: write_set(SLOT_PUT, 0, recs, n, NULL);
	}
	unlock();
	return er;
}

EXPORT ER tr_get_sts( T_TRSTS *sts )
{
	INT	i;
	ER	er;

	if ( sts == NULL ) {
		return E_PAR;
	}
	knl_memset(sts, 0, sizeof(*sts));
	lock();
	er = ready();
	if ( er >= E_OK ) {
		sts->nlevel = nlevel();
		sts->hand = tr_hand;
		(void)set_info(SLOT_PUT, &sts->set[TR_PUT]);
		for ( i = 1; i <= sts->nlevel; i++ ) {
			(void)set_info(tr_lv[i], &sts->set[i]);
		}
	}
	unlock();
	return er;
}

EXPORT ER tr_ref_rec( INT level, INT rec, T_TRINF *inf )
{
	UB	ent[ENT_SIZE];
	INT	slot;
	ER	er;

	if ( inf == NULL ) {
		return E_PAR;
	}
	lock();
	er = ready();
	slot = slot_of(level);
	if ( er >= E_OK ) {
		er = ( slot != 0 ) ? read_ent(slot, rec, ent) : E_NOEXS;
	}
	if ( er >= E_OK ) {
		inf->kind = rd32(ent);
		inf->rt = rd32(ent + 4);
		inf->sub = rd32(ent + 8);
		inf->size = (SZ)rd64(ent + 24);
	}
	unlock();
	return er;
}

EXPORT ER tr_rea_rec( INT level, INT rec, void *buf, SZ size, SZ *p_asize )
{
	UB	ent[ENT_SIZE];
	SZ	have, asz = 0;
	INT	slot;
	ER	er;

	lock();
	er = ready();
	slot = slot_of(level);
	if ( er >= E_OK ) {
		er = ( slot != 0 ) ? read_ent(slot, rec, ent) : E_NOEXS;
	}
	if ( er >= E_OK ) {
		have = (SZ)rd64(ent + 24);
		if ( p_asize != NULL ) *p_asize = have;
		if ( buf != NULL && size > 0 && have > 0 ) {
			er = ob_rea_rec(tr_key, slot, (D)rd64(ent + 16), buf,
					( size < have ) ? size : have, &asz);
		}
	}
	unlock();
	return er;
}

EXPORT ER tr_sel_dat( INT level )
{
	ER	er;

	lock();
	er = ready();
	if ( er >= E_OK ) {
		if ( level < 1 || level > nlevel() ) {
			er = E_PAR;
		} else {
			tr_hand = level;
			tell();
		}
	}
	unlock();
	return er;
}

EXPORT ER tr_mov_dat( INT level, INT to )
{
	INT	n, slot, i;
	ER	er;

	lock();
	er = ready();
	n = nlevel();
	if ( level == TR_HAND ) level = tr_hand;
	if ( er >= E_OK && ( level < 1 || level > n || to < 1 || to > n ) ) {
		er = E_PAR;
	}
	if ( er >= E_OK && level != to ) {
		slot = tr_lv[level];
		if ( level < to ) {
			for ( i = level; i < to; i++ ) tr_lv[i] = tr_lv[i + 1];
		} else {
			for ( i = level; i > to; i-- ) tr_lv[i] = tr_lv[i - 1];
		}
		tr_lv[to] = slot;
		if ( tr_hand == level ) tr_hand = to;
		tell();
	}
	unlock();
	return er;
}

EXPORT ER tr_del_dat( INT level )
{
	INT	n, i;
	ER	er;

	lock();
	er = ready();
	n = nlevel();
	if ( level == TR_HAND ) level = tr_hand;
	if ( er >= E_OK && level == TR_PUT ) {
		er = ob_trn_rec(tr_key, SLOT_PUT, 0);
	} else if ( er >= E_OK && ( level < 1 || level > n ) ) {
		er = E_NOEXS;
	} else if ( er >= E_OK ) {
		(void)ob_trn_rec(tr_key, tr_lv[level], 0);
		for ( i = level; i < TR_LEVELS; i++ ) tr_lv[i] = tr_lv[i + 1];
		tr_lv[TR_LEVELS] = 0;
		n--;
		/* the hand stays on the set it was on; on the one taken out, the next */
		if ( tr_hand > level ) {
			tr_hand--;
		} else if ( tr_hand == level ) {
			tr_hand = ( level <= n ) ? level : n;
		}
		tell();
	}
	unlock();
	return er;
}

EXPORT ER tr_clr_tra( void )
{
	INT	i;
	ER	er;

	lock();
	er = ready();
	if ( er >= E_OK ) {
		for ( i = 1; i < TR_SLOTS; i++ ) {
			(void)ob_trn_rec(tr_key, i, 0);
		}
		for ( i = 0; i <= TR_LEVELS; i++ ) tr_lv[i] = 0;
		tr_hand = 0;
		tell();
	}
	unlock();
	return er;
}
