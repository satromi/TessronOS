/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	om.c
 *	Virtual objects over the file system (design 16.3, protection
 *	level 3)
 *
 *	The register of what is on the screen, and the round trip between
 *	the <link> elements of a document and the reference counts the file
 *	system holds.
 *
 *	The FAT store does not read the content of a real object, so it
 *	cannot know that a document has gained or lost a link; the library
 *	that changed the text is the one that has to say so. ob_lnk_obj and
 *	ob_unl_obj are that saying, and they go in the same transaction as
 *	the write of the record (design 11.7, 16.3.4, 18.5). The native
 *	store reads the links of its xmlTAD records itself, and is told
 *	nothing.
 */

#include <tk/tkernel.h>
#include <ts/ob.h>
#include <ts/tad.h>
#include <ts/om.h>
#include <ts/dp.h>
#include <ts/fn.h>
#include <ts/img.h>

typedef struct {
	BOOL		used;
	T_OMVOBJ	v;
} OMENT;

LOCAL OMENT	*om_tab[OM_MAX_VOBJ];
LOCAL ID	om_mtxid = 0;
LOCAL INT	om_count = 0;

/* ---------------------------------------------------------------- text */

LOCAL SZ s_len( CONST char *s )
{
	SZ	n = 0;

	while ( s[n] != '\0' ) n++;

	return n;
}

LOCAL SZ s_put( UB *dst, SZ pos, CONST char *src )
{
	while ( *src != '\0' ) dst[pos++] = (UB)*src++;
	dst[pos] = '\0';

	return pos;
}

/*
 * A name into a place where some characters have to be written out.
 * `xml` picks which set: the text of an element and the value of an
 * attribute own & < > and ", a JSON string owns " and the backslash.
 * A control character is refused either way.
 */
LOCAL ER s_put_esc( UB *dst, SZ *p_pos, CONST UB *src, BOOL xml )
{
	SZ	pos = *p_pos;

	while ( *src != '\0' ) {
		UB c = *src++;

		if ( c < 0x20 ) {
			return E_PAR;
		}
		if ( xml ) {
			switch ( c ) {
			case '&':	pos = s_put(dst, pos, "&amp;");		continue;
			case '<':	pos = s_put(dst, pos, "&lt;");		continue;
			case '>':	pos = s_put(dst, pos, "&gt;");		continue;
			case '"':	pos = s_put(dst, pos, "&quot;");	continue;
			default:	break;
			}
		} else if ( c == '"' || c == 0x5C ) {
			dst[pos++] = 0x5C;
		}
		dst[pos++] = c;
	}
	dst[pos] = '\0';
	*p_pos = pos;

	return E_OK;
}

/* ---------------------------------------------------------------- register */

LOCAL ER om_lock( void )
{
	if ( om_mtxid <= 0 ) {
		return E_OBJ;			/* om_ini has not been called */
	}

	return tk_loc_mtx(om_mtxid, TMO_FEVR);
}

LOCAL void om_unlock( void )
{
	tk_unl_mtx(om_mtxid);
}

EXPORT ER om_ini( void )
{
	T_CMTX	cmtx;
	INT	i;
	ID	id;

	if ( om_mtxid > 0 ) {
		return E_OK;			/* it is already there */
	}
	cmtx.exinf  = NULL;
	cmtx.mtxatr = TA_TFIFO | TA_INHERIT;
	cmtx.ceilpri = 0;
	id = tk_cre_mtx(&cmtx);
	if ( id <= 0 ) {
		return (ER)id;
	}
	for ( i = 0; i < OM_MAX_VOBJ; i++ ) {
		om_tab[i] = NULL;
	}
	om_count = 0;
	om_mtxid = id;

	return E_OK;
}

EXPORT ER om_fin( void )
{
	INT	i;
	ID	id = om_mtxid;

	if ( id <= 0 ) {
		return E_OBJ;
	}
	om_mtxid = 0;
	for ( i = 0; i < OM_MAX_VOBJ; i++ ) {
		if ( om_tab[i] != NULL ) {
			Kfree(om_tab[i]);
			om_tab[i] = NULL;
		}
	}
	om_count = 0;

	return tk_del_mtx(id);
}

/*
 * Put a virtual object in the register. The identity is the vobjid of
 * the link; one that is already in there is refused, because two frames
 * of the same identity would be two views of one thing with no way to
 * tell which is which.
 */
EXPORT ER om_reg_vob( CONST T_VOBJ *vobj, CONST T_OMREG *reg, ID *p_vid )
{
	OMENT	*e;
	INT	i, free_i = -1;
	ER	er;

	if ( vobj == NULL || p_vid == NULL ) {
		return E_PAR;
	}
	er = om_lock();
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < OM_MAX_VOBJ; i++ ) {
		if ( om_tab[i] == NULL || !om_tab[i]->used ) {
			if ( free_i < 0 ) free_i = i;
			continue;
		}
		if ( ts_uuid_cmp(&om_tab[i]->v.vobjid, &vobj->vobjid) == 0 ) {
			om_unlock();
			return E_OBJ;
		}
	}
	if ( free_i < 0 ) {
		om_unlock();
		return E_LIMIT;			/* the register is full */
	}
	e = om_tab[free_i];
	if ( e == NULL ) {
		e = (OMENT *)Kmalloc(sizeof(OMENT));
		if ( e == NULL ) {
			om_unlock();
			return E_NOMEM;
		}
		om_tab[free_i] = e;
	}
	e->used     = TRUE;
	e->v.vobjid = vobj->vobjid;
	e->v.target = vobj->target;
	e->v.vobj   = *vobj;
	if ( reg != NULL ) {
		e->v.reg = *reg;
	} else {
		UB *p = (UB *)&e->v.reg;
		INT n;

		for ( n = 0; n < (INT)sizeof(T_OMREG); n++ ) p[n] = 0;
	}
	om_count++;
	*p_vid = (ID)(free_i + 1);
	om_unlock();

	return E_OK;
}

LOCAL OMENT *ent_of( ID vid )
{
	if ( vid < 1 || vid > OM_MAX_VOBJ ) {
		return NULL;
	}
	if ( om_tab[vid - 1] == NULL || !om_tab[vid - 1]->used ) {
		return NULL;
	}

	return om_tab[vid - 1];
}

EXPORT ER om_del_vob( ID vid )
{
	OMENT	*e;
	ER	er = om_lock();

	if ( er < E_OK ) {
		return er;
	}
	e = ent_of(vid);
	if ( e == NULL ) {
		om_unlock();
		return E_NOEXS;
	}
	e->used = FALSE;
	om_count--;
	om_unlock();

	return E_OK;
}

EXPORT ER om_set_vob( ID vid, CONST T_OMREG *reg )
{
	OMENT	*e;
	ER	er;

	if ( reg == NULL ) {
		return E_PAR;
	}
	er = om_lock();
	if ( er < E_OK ) {
		return er;
	}
	e = ent_of(vid);
	if ( e == NULL ) {
		om_unlock();
		return E_NOEXS;
	}
	e->v.reg = *reg;
	om_unlock();

	return E_OK;
}

EXPORT ER om_upd_vob( ID vid, CONST T_VOBJ *vobj )
{
	OMENT	*e;
	ER	er;

	if ( vobj == NULL ) {
		return E_PAR;
	}
	er = om_lock();
	if ( er < E_OK ) {
		return er;
	}
	e = ent_of(vid);
	if ( e == NULL ) {
		om_unlock();
		return E_NOEXS;
	}
	if ( ts_uuid_cmp(&e->v.vobjid, &vobj->vobjid) != 0
	  || ts_uuid_cmp(&e->v.target, &vobj->target) != 0 ) {
		om_unlock();
		return E_PAR;			/* that would be another link */
	}
	e->v.vobj = *vobj;
	om_unlock();

	return E_OK;
}

EXPORT ER om_ref_vob( ID vid, T_OMVOBJ *pk_vobj )
{
	OMENT	*e;
	ER	er;

	if ( pk_vobj == NULL ) {
		return E_PAR;
	}
	er = om_lock();
	if ( er < E_OK ) {
		return er;
	}
	e = ent_of(vid);
	if ( e == NULL ) {
		om_unlock();
		return E_NOEXS;
	}
	*pk_vobj = e->v;
	om_unlock();

	return E_OK;
}

/* ---------------------------------------------------------------- drawing */

LOCAL UW colour_or( UW c, UW def )
{
	return ( c == TAD_COL_NONE ) ? def : c;
}

/* What a band shows: any of the picture, the name, the relationship, the kind and the date */
#define OM_D_BAND	( TAD_D_NAME | TAD_D_PICT | TAD_D_ROLE | TAD_D_TYPE | TAD_D_UPDATE )

/*
 * How tall the band along the top of a virtual object is: the letters
 * it holds, and a little either side. A link that shows none of the
 * picture, the name, the relationship, the kind and the date has no
 * band at all: it is a plain box in tbcol.
 */
EXPORT INT om_band_h( CONST T_VOBJ *vobj )
{
	INT	ch;

	if ( vobj == NULL || (vobj->disp & OM_D_BAND) == 0 ) {
		return 0;
	}
	ch = ( vobj->chsz > 0 ) ? vobj->chsz : 14;

	/*
	 * The band is the letters and eleven pixels: a line above and
	 * below the name, the frame, and the room the picture needs
	 * beside it. The number is not chosen here -- it is what the
	 * records say. A closed virtual object written at fourteen point
	 * is twenty-five pixels tall in every one of them, and a band
	 * drawn any other height makes every closed object in a document
	 * the wrong size.
	 */
	return ch + 11;
}

/*
 * The picture that stands at the left of the band when the object has
 * no icon of its own: a leaf of paper with its corner turned. It is
 * drawn rather than left out because its absence would move the name.
 */
LOCAL void draw_leaf( INT gid, CONST T_DPRECT *box, UW ink, UW paper )
{
	T_DPRECT	q = *box;
	INT		fold;

	if ( q.right - q.left < 6 || q.bottom - q.top < 6 ) {
		return;
	}
	q.right = q.left + ( q.bottom - q.top ) * 3 / 4;
	fold = ( q.right - q.left ) / 3;
	dp_fill_rect(gid, &q, paper);
	dp_frame_rect(gid, &q, ink, 1);
	/* the turned corner */
	dp_line(gid, q.right - 1 - fold, q.top, q.right - 1, q.top + fold, ink);
	dp_line(gid, q.right - 1 - fold, q.top,
		q.right - 1 - fold, q.top + fold, ink);
	dp_line(gid, q.right - 1 - fold, q.top + fold, q.right - 1, q.top + fold,
		ink);
}

/*
 * The object's own icon in a square box, the nearest pixel of it for
 * each of the box's, its clear pixels left as they are.
 */
#define OM_ICON_MAX	64

LOCAL void draw_icon( INT gid, CONST T_DPRECT *box, CONST UW *px, INT w, INT h )
{
	UW	*sq;
	INT	side = box->bottom - box->top, x, y;

	if ( side > OM_ICON_MAX ) side = OM_ICON_MAX;
	if ( side <= 0 || w <= 0 || h <= 0 ) {
		return;
	}
	/* from the heap: this may run on a process's small stack of calls */
	sq = (UW *)Kmalloc((SZ)side * side * sizeof(UW));
	if ( sq == NULL ) {
		return;
	}
	for ( y = 0; y < side; y++ ) {
		for ( x = 0; x < side; x++ ) {
			sq[y * side + x] = px[(SZ)( y * h / side ) * w + x * w / side];
		}
	}
	dp_put_argb(gid, box->left, box->top, sq, side, side, side, IMG_CLEAR);
	Kfree(sq);
}

/*
 * A virtual object drawn in a rectangle given, whether or not it is in
 * the register. Showing a document means drawing the links that stand
 * in it, and showing something is not a reason to register it.
 *
 * What is drawn is what the link's own attributes ask for and nothing
 * else: the box in tbcol, holding the picture and the words in chcol
 * at chsz, the frame in frcol. An opened one has the band in tbcol and
 * the room under it in bgcol. The words are what the caller made of
 * the link (om_draw_vob_as): its name, its relationship, the kind of
 * program it opens with and the date it was changed, each only when
 * the link asks; without them, the link's own name.
 */
LOCAL ER draw_vob_body( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj,
			CONST T_OMLOOK *look, BOOL open, T_DPRECT *p_inner )
{
	T_DPRECT	r, band, pict;
	UW		frcol, tbcol, bgcol, chcol;
	INT		bh, ch, tx;
	CONST UB	*text = NULL;

	if ( rect == NULL || vobj == NULL ) {
		return E_PAR;
	}
	r = *rect;
	if ( r.right <= r.left || r.bottom <= r.top ) {
		return E_PAR;
	}
	frcol = colour_or(vobj->frcol, OM_DEF_FRCOL);
	tbcol = colour_or(vobj->tbcol, OM_DEF_TBCOL);
	bgcol = colour_or(vobj->bgcol, OM_DEF_BGCOL);
	chcol = colour_or(vobj->chcol, OM_DEF_CHCOL);
	ch = ( vobj->chsz > 0 ) ? vobj->chsz : 14;
	bh = om_band_h(vobj);
	if ( look != NULL && look->text != NULL ) {
		text = look->text;
	} else if ( (vobj->disp & TAD_D_NAME) != 0 ) {
		text = vobj->name;
	}

	/* the whole of it first: a closed one is all band, an opened one the room below */
	dp_fill_rect(gid, &r, open ? bgcol : tbcol);

	if ( bh > 0 ) {
		band = r;
		band.left   = r.left + OM_FRAME_W;
		band.right  = r.right - OM_FRAME_W;
		band.top    = r.top + OM_FRAME_W;
		band.bottom = band.top + bh;
		if ( band.bottom > r.bottom - OM_FRAME_W ) {
			band.bottom = r.bottom - OM_FRAME_W;
		}
		if ( band.bottom > band.top && band.right > band.left ) {
			dp_fill_rect(gid, &band, tbcol);
		}
		tx = band.left + 2;
		if ( (vobj->disp & TAD_D_PICT) != 0 ) {
			pict = band;
			pict.left   = band.left + 2;
			pict.top    = band.top + 1;
			pict.bottom = band.bottom - 1;
			pict.right  = pict.left + ( pict.bottom - pict.top );
			if ( look != NULL && look->icon != NULL ) {
				draw_icon(gid, &pict, look->icon, look->icon_w, look->icon_h);
			} else {
				draw_leaf(gid, &pict, chcol, bgcol);
			}
			tx = pict.right + 3;
		}
		if ( text != NULL && text[0] != '\0' ) {
			ID	fid = fn_system();

			if ( fid > 0 ) {
				T_FNMET	met;
				INT	room = band.right - tx - 2;

				fn_set_size(fid, ch);
				if ( room > 0 && fn_metrics(fid, &met) >= E_OK ) {
					UB	cut[OM_LOOK_TEXT];
					INT	i, n;

					/* what does not fit is cut, not squeezed */
					n = fn_fit(fid, text, room);
					if ( n < 0 ) {
						n = 0;
					}
					for ( i = 0; i < n && i < OM_LOOK_TEXT - 1; i++ ) {
						cut[i] = text[i];
					}
					cut[i] = '\0';
					fn_draw(gid, fid, tx,
						band.top + ( band.bottom - band.top
							     + met.ascent
							     - met.descent ) / 2,
						cut, chcol);
				}
			}
		}
		if ( open && (vobj->disp & TAD_D_FRAME) != 0 && band.bottom < r.bottom - OM_FRAME_W ) {
			/* the line under an opened object's band */
			dp_line(gid, r.left, band.bottom, r.right - 1, band.bottom, frcol);
		}
	}
	if ( (vobj->disp & TAD_D_FRAME) != 0 ) {
		dp_frame_rect(gid, &r, frcol, OM_FRAME_W);
	}
	if ( p_inner != NULL ) {
		p_inner->left   = r.left + OM_FRAME_W;
		p_inner->right  = r.right - OM_FRAME_W;
		p_inner->top    = r.top + OM_FRAME_W + bh + ( ( bh > 0 && (vobj->disp & TAD_D_FRAME) != 0 ) ? 1 : 0 );
		p_inner->bottom = r.bottom - OM_FRAME_W;
		if ( (vobj->disp & TAD_D_FRAME) == 0 ) {
			/* without a frame the room keeps its distance from the edge */
			p_inner->left   += OM_OPEN_MARGIN;
			p_inner->right  -= OM_OPEN_MARGIN;
			p_inner->top    += OM_OPEN_MARGIN;
			p_inner->bottom -= OM_OPEN_MARGIN;
		}
	}

	return E_OK;
}

EXPORT ER om_draw_vob( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj )
{
	return draw_vob_body(gid, rect, vobj, NULL, FALSE, NULL);
}

EXPORT ER om_draw_open( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj,
			T_DPRECT *p_inner )
{
	return draw_vob_body(gid, rect, vobj, NULL, TRUE, p_inner);
}

EXPORT ER om_draw_vob_as( INT gid, CONST T_DPRECT *rect, CONST T_VOBJ *vobj,
			  CONST T_OMLOOK *look, BOOL open, T_DPRECT *p_inner )
{
	return draw_vob_body(gid, rect, vobj, look, open, p_inner);
}

EXPORT ER om_dsp_vob( INT gid, ID vid )
{
	T_OMVOBJ	v;
	T_DPRECT	r;
	ER		er;

	er = om_ref_vob(vid, &v);
	if ( er < E_OK ) {
		return er;
	}
	r.left   = v.reg.left;
	r.top    = v.reg.top;
	r.right  = v.reg.right;
	r.bottom = v.reg.bottom;

	return om_draw_vob(gid, &r, &v.vobj);
}

EXPORT ER om_sel_vob( INT gid, ID vid, BOOL on )
{
	T_OMVOBJ	v;
	T_DPRECT	r;
	T_DPENV		env;
	ER		er;

	(void)on;				/* inverting twice puts it back */
	er = om_ref_vob(vid, &v);
	if ( er < E_OK ) {
		return er;
	}
	r.left   = v.reg.left;
	r.top    = v.reg.top;
	r.right  = v.reg.right;
	r.bottom = v.reg.bottom;
	if ( r.right <= r.left || r.bottom <= r.top ) {
		return E_PAR;
	}
	er = dp_ref(gid, &env);
	if ( er < E_OK ) {
		return er;
	}
	dp_set_mode(gid, DP_MODE_XOR);
	dp_fill_rect(gid, &r, 0x00FFFFFFU);
	dp_set_mode(gid, env.mode);

	return E_OK;
}

EXPORT ER om_fnd_vob( CONST TS_UUID *vobjid, ID *p_vid )
{
	INT	i;
	ER	er;

	if ( vobjid == NULL || p_vid == NULL ) {
		return E_PAR;
	}
	er = om_lock();
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < OM_MAX_VOBJ; i++ ) {
		if ( om_tab[i] == NULL || !om_tab[i]->used ) continue;
		if ( ts_uuid_cmp(&om_tab[i]->v.vobjid, vobjid) == 0 ) {
			*p_vid = (ID)(i + 1);
			om_unlock();
			return E_OK;
		}
	}
	om_unlock();

	return E_NOEXS;
}

EXPORT INT om_cnt_vob( void )
{
	return om_count;
}

/* ---------------------------------------------------------------- objects */

/*
 * A new real object holding one empty document, made on a volume given
 * by the path it was attached with (NULL: the first). The metadata is
 * the smallest that says what 11.5 wants said; the object layer adds
 * the maker's protection to it.
 */
EXPORT ER om_cre_obj( CONST char *vol, CONST UB *name, UINT body, TS_UUID *p_uuid )
{
	T_OBCRE	c;
	TS_UUID	uuid;
	UB	*buf;
	SZ	pos = 0, xoff;
	INT	k;
	ER	er;

	if ( p_uuid == NULL || name == NULL ) {
		return E_PAR;
	}
	if ( s_len((CONST char *)name) >= OB_NAME_MAX ) {
		return E_PAR;
	}
	buf = (UB *)Kmalloc(2048);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	pos = s_put(buf, 0, "{\"name\":\"");
	er = s_put_esc(buf, &pos, name, FALSE);
	if ( er >= E_OK ) {
		pos = s_put(buf, pos, "\",\"refCount\":0,\"recordCount\":1,"
				      "\"editable\":true,\"deletable\":true,"
				      "\"readable\":true}");
	}
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	for ( k = 0; k < (INT)sizeof(c); k++ ) {
		((UB *)&c)[k] = 0;
	}
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = buf;
	c.jsonsz = pos;
	c.vol = vol;
	er = ob_cre_obj(&c, &uuid);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}

	/* the record: an empty document of the kind that was asked for */
	xoff = pos + 1;
	pos = xoff;
	pos = s_put(buf, pos, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	er = s_put_esc(buf, &pos, name, TRUE);
	if ( er >= E_OK ) {
		pos = s_put(buf, pos, "\">\n");
		pos = s_put(buf, pos, ( body == OM_BODY_FIG )
				    ? "<figure>\n</figure>\n" : "<document>\n</document>\n");
		pos = s_put(buf, pos, "</tad>\n");
		er = om_obj_record_put(&uuid, 0, OB_RT_TAD, 0, buf + xoff, pos - xoff);
	}
	Kfree(buf);
	if ( er < E_OK ) {
		ob_del_obj(&uuid);		/* it never came to anything */
		return er;
	}
	*p_uuid = uuid;

	return E_OK;
}

/* ---------------------------------------------------------------- documents */

/* A record of an object parsed; the document is the caller's to free */
EXPORT ER om_rea_doc( CONST TS_UUID *uuid, INT recno, CONST T_TADLIM *lim, T_TAD **p_doc )
{
	T_OBREC	rec[OM_REC_MAX];
	UB	*buf;
	SZ	size = 0;
	INT	cnt, i;
	ER	er;

	if ( uuid == NULL || p_doc == NULL || recno < 0 ) {
		return E_PAR;
	}
	cnt = om_obj_records(uuid, rec, OM_REC_MAX);
	if ( cnt < 0 ) {
		return (ER)cnt;
	}
	for ( i = 0; i < cnt && rec[i].recno != recno; i++ ) ;
	if ( i == cnt ) {
		return E_NOEXS;
	}
	if ( rec[i].rt != OB_RT_TAD || rec[i].size == 0 ) {
		return E_OBJ;			/* not a document */
	}
	buf = om_obj_record(uuid, recno, &size);
	if ( buf == NULL ) {
		return E_IO;
	}
	er = tad_parse(buf, size, lim, p_doc);
	Kfree(buf);

	return er;
}

/*
 * Write the document back and settle what its edits owe the reference
 * counts, both in one transaction of the volume where the volume keeps
 * a log. The counts go first: a link whose target is gone has to stop
 * the write, and an abort takes back the header of an object but not
 * the bytes of a record.
 */
EXPORT ER om_wri_doc( CONST TS_UUID *uuid, INT recno, T_TAD *doc )
{
	TS_UUID	target;
	BOOL	trx = FALSE;
	ID	key;
	INT	i, n, delta;
	ER	er;

	if ( uuid == NULL || doc == NULL || recno < 0 ) {
		return E_PAR;
	}
	key = ob_opn_obj(uuid, OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRRD);
	if ( key < E_OK ) {
		return (ER)key;
	}
	er = ob_beg_trx(uuid);
	if ( er >= E_OK ) {
		trx = TRUE;
	} else if ( er != E_NOSPT ) {
		ob_cls_obj(key);
		return er;			/* a volume with a log that would not start one */
	}
	er = E_OK;

	n = om_store_counts(uuid) ? 0 : tad_lnk_ndelta(doc);
	for ( i = 0; i < n && er >= E_OK; i++ ) {
		er = tad_lnk_delta(doc, i, &target, &delta);
		while ( er >= E_OK && delta > 0 ) {
			er = ob_lnk_obj(&target);
			delta--;
		}
		while ( er >= E_OK && delta < 0 ) {
			er = ob_unl_obj(&target);
			delta++;
		}
	}
	if ( er >= E_OK ) {
		er = tad_write(key, recno, doc);
	}
	if ( trx ) {
		ER er2 = ob_end_trx(uuid, ( er >= E_OK ) ? TRUE : FALSE);

		if ( er >= E_OK ) er = er2;
	}
	ob_cls_obj(key);
	if ( er >= E_OK ) {
		tad_lnk_clr_delta(doc);
	}

	return er;
}
