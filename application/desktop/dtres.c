/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dtres.c
 *	The system's faces and the pictures of the ground as real objects
 *	(design 18.18)
 *
 *	The system's faces and pictures are objects on the disk from the
 *	start, linked from the 書体箱 and the 壁紙箱 -- boxes of fixed
 *	identity -- each with the file it was made from named in its
 *	metadata ("tessronos.file") and the file's bytes in record 1
 *	(tools/mkobj.py). Nothing is taken in when the desktop starts. The
 *	boxes are searched by the object layer (ob_lst_lnk, ob_fnd_lnk),
 *	which knows a link by its object name or its file name.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/wm.h>
#include <ts/fn.h>
#include <ts/tad.h>
#include <ts/om.h>
#include <ts/ob.h>
#include <ts/xf.h>
#include <ts/sysdef.h>
#include <ts/json.h>
#include <ts/dtreq.h>
#include <ts/part.h>
#include <ts/conf.h>
#include "desktop.h"

#define RES_MAX		32		/* faces, or pictures, in one box */

LOCAL CONST char font_box_id[] = SYSDEF_FONT_BOX;
LOCAL CONST char wall_box_id[] = SYSDEF_WALL_BOX;

/*
 * The face the letters of the system are drawn in: the one the link of
 * 機器の設定 with the relationship CF_ROLE_FACE points at. Until there
 * is such a link, the first of these files the 書体箱 has, and the link
 * is then made to it, so that the face is followed by its object from
 * then on, however it is named.
 */
LOCAL CONST char *system_face[] = {
	"NotoSansJP-Regular.otf", "NOTOSANS.OTF", NULL
};

/* The picture the ground is laid with, when it is there */
LOCAL CONST char *system_wall[] = {
	"TESSRON.PIC", NULL
};

/* ---------------------------------------------------------------- names */

LOCAL UB lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c - 'A' + 'a' ) : c;
}

/* Whether two names are the same, without case: FAT keeps none */
LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( lower(a[i]) != lower(b[i]) ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/* ---------------------------------------------------------------- boxes */

/* The box: found from the root システム by its link's relationship, or else by its identity */
LOCAL BOOL box_of( CONST char *id, TS_UUID *p_u )
{
	CONST char	*role = ( id == font_box_id ) ? SYSROLE_FONTS : ( id == wall_box_id ) ? SYSROLE_WALLS : NULL;

	return knl_sys_find(role, id, p_u);
}

/* What a box links to */
LOCAL INT box_links( CONST char *id, TS_UUID *ids, INT max )
{
	TS_UUID	u;
	INT	n = 0;

	if ( !box_of(id, &u) || ob_lst_lnk(&u, NULL, ids, max, &n) < E_OK ) {
		return 0;
	}
	return ( n > max ) ? max : n;
}

/* The first of the names that the box has a link of: the one most wanted there is */
LOCAL BOOL box_pick( CONST char *id, CONST char **names, TS_UUID *p_uuid )
{
	TS_UUID	u;
	INT	k;

	if ( !box_of(id, &u) ) {
		return FALSE;
	}
	for ( k = 0; names[k] != NULL; k++ ) {
		if ( ob_fnd_lnk(&u, (CONST UB *)names[k], p_uuid) >= E_OK ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( ts_uuid_cmp(a, b) == 0 );
}

/* ---------------------------------------------------------------- what the desktop asks */

/* ---------------------------------------------------------------- what a face is registered as */

/*
 * A face object's resource DT_FONT_RES says what each of its faces is
 * registered as: {"faces":[{"index":0,"name":…,"use":…,"family":…,
 * "class":"6050","attr":"0010","base":…}, …]}. A face it does not name
 * is its file's first face (index 0), used, as the file says it is.
 */
#define FACE_MAX	8		/* faces of one object kept track of */

typedef struct {
	INT	index;
	BOOL	use;
	UINT	cls, attr;
	UB	name[FN_NAME_MAX];
	UB	family[FN_NAME_MAX];
	UB	base[FN_NAME_MAX];
} FACESET;

/* Four hexadecimal digits, as the settings write them */
LOCAL UINT hex_of( CONST UB *s )
{
	UINT	v = 0;
	INT	i;

	for ( i = 0; s[i] != 0 && i < 8; i++ ) {
		UB	c = s[i];

		if ( c >= '0' && c <= '9' )      v = ( v << 4 ) | (UINT)( c - '0' );
		else if ( c >= 'a' && c <= 'f' ) v = ( v << 4 ) | (UINT)( c - 'a' + 10 );
		else if ( c >= 'A' && c <= 'F' ) v = ( v << 4 ) | (UINT)( c - 'A' + 10 );
		else if ( c == 'x' || c == 'X' ) v = 0;
		else break;
	}
	return v & 0xFFFFU;
}

/* One face's settings from its JSON object */
LOCAL void face_read( CONST T_JSON *o, FACESET *fs )
{
	UB	h[16];

	knl_memset(fs, 0, sizeof(*fs));
	fs->index = (INT)js_get_num(o, "index", 0);
	fs->use = js_get_bool(o, "use", TRUE);
	(void)js_get_str(o, "name", fs->name, sizeof(fs->name));
	(void)js_get_str(o, "family", fs->family, sizeof(fs->family));
	(void)js_get_str(o, "base", fs->base, sizeof(fs->base));
	if ( js_get_str(o, "class", h, sizeof(h)) > 0 ) fs->cls = hex_of(h);
	if ( js_get_str(o, "attr", h, sizeof(h)) > 0 ) fs->attr = hex_of(h);
}

/* What an object's faces are registered as: how many it says */
LOCAL INT faces_of( CONST TS_UUID *u, FACESET *fs, INT max )
{
	UB	*raw;
	SZ	size = 0;
	T_JSON	root, list, it;
	INT	n = 0;

	raw = om_obj_res(u, (CONST UB *)DT_FONT_RES, &size);
	if ( raw == NULL ) {
		return 0;
	}
	if ( js_parse(raw, (INT)size, &root) >= E_OK && js_get(&root, "faces", &list) >= E_OK ) {
		for ( it.s = NULL; n < max && js_next(&list, &it); ) {
			face_read(&it, &fs[n]);
			if ( fs[n].index >= 0 && fs[n].index < 64 ) n++;
		}
	}
	Kfree(raw);
	return n;
}

/* Bytes added to a buffer, as far as there is room; where it ends */
LOCAL INT bput( UB *b, INT at, INT max, CONST char *s )
{
	while ( *s != 0 && at < max - 1 ) b[at++] = (UB)*s++;
	b[at] = 0;
	return at;
}

/* A string as JSON writes it, in its quotes */
LOCAL INT bput_str( UB *b, INT at, INT max, CONST UB *s )
{
	at = bput(b, at, max, "\"");
	for ( ; *s != 0 && at < max - 3; s++ ) {
		if ( *s == '"' || *s == '\\' ) {
			b[at++] = '\\';
			b[at++] = *s;
		} else if ( *s >= 0x20 ) {
			b[at++] = *s;
		}
	}
	b[at] = 0;
	return bput(b, at, max, "\"");
}

LOCAL INT bput_hex( UB *b, INT at, INT max, UINT v )
{
	CONST char	*hx = "0123456789abcdef";
	char		t[5];
	INT		i;

	for ( i = 0; i < 4; i++ ) t[i] = hx[( v >> ( 12 - 4 * i ) ) & 15];
	t[4] = 0;
	at = bput(b, at, max, "\"");
	at = bput(b, at, max, t);
	return bput(b, at, max, "\"");
}

LOCAL INT bput_num( UB *b, INT at, INT max, INT v )
{
	char	t[12];
	INT	i = 11;

	t[i] = 0;
	do {
		t[--i] = (char)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 && i > 0 );
	return bput(b, at, max, t + i);
}

/* An object's faces' settings written as its resource */
LOCAL ER faces_put( CONST TS_UUID *u, CONST FACESET *fs, INT n )
{
	UB	*b;
	INT	at = 0, i, max = 512 * ( n + 1 );
	ER	er;

	b = (UB *)Kmalloc((SZ)max);
	if ( b == NULL ) {
		return E_NOMEM;
	}
	at = bput(b, at, max, "{\"faces\":[");
	for ( i = 0; i < n; i++ ) {
		at = bput(b, at, max, ( i > 0 ) ? ",{\"index\":" : "{\"index\":");
		at = bput_num(b, at, max, fs[i].index);
		at = bput(b, at, max, ",\"name\":");
		at = bput_str(b, at, max, fs[i].name);
		at = bput(b, at, max, fs[i].use ? ",\"use\":true" : ",\"use\":false");
		at = bput(b, at, max, ",\"family\":");
		at = bput_str(b, at, max, fs[i].family);
		at = bput(b, at, max, ",\"class\":");
		at = bput_hex(b, at, max, fs[i].cls);
		at = bput(b, at, max, ",\"attr\":");
		at = bput_hex(b, at, max, fs[i].attr);
		at = bput(b, at, max, ",\"base\":");
		at = bput_str(b, at, max, fs[i].base);
		at = bput(b, at, max, "}");
	}
	at = bput(b, at, max, "]}");
	er = om_obj_res_put(u, (CONST UB *)DT_FONT_RES, b, (SZ)at);
	Kfree(b);
	return er;
}

/* The settings of face 'index' among n, added when they are not there; NULL no room */
LOCAL FACESET *face_in( FACESET *fs, INT *p_n, INT index )
{
	INT	i;

	for ( i = 0; i < *p_n; i++ ) {
		if ( fs[i].index == index ) return &fs[i];
	}
	if ( *p_n >= FACE_MAX ) {
		return NULL;
	}
	knl_memset(&fs[*p_n], 0, sizeof(fs[0]));
	fs[*p_n].index = index;
	fs[*p_n].use = TRUE;
	return &fs[( *p_n )++];
}

/* ---------------------------------------------------------------- the faces opened */

/*
 * The faces this layer opened from the 書体箱, and from which object:
 * a face whose object has gone from the box is no longer used. The
 * faces stay open for the life of the system (the windows, the menus
 * and the message line draw with them); taking them in again, as a
 * desktop started a second time does or a face registered asks, opens
 * only those not open yet.
 */
typedef struct {
	ID	fid;
	TS_UUID	obj;
	INT	index;
} OPENED;

LOCAL OPENED	dt_faces[FN_MAX_FONT];
LOCAL INT	dt_nfaces = 0;

LOCAL void opened_note( ID fid, CONST TS_UUID *obj, INT index )
{
	INT	i;

	for ( i = 0; i < dt_nfaces; i++ ) {
		if ( dt_faces[i].fid == fid ) return;
	}
	if ( dt_nfaces < FN_MAX_FONT ) {
		dt_faces[dt_nfaces].fid = fid;
		dt_faces[dt_nfaces].obj = *obj;
		dt_faces[dt_nfaces].index = index;
		dt_nfaces++;
	}
}

/* A face registered as its settings say; with none, as its file says it is, used */
LOCAL void face_reg( ID fid, CONST FACESET *fs, BOOL force_on )
{
	T_FNREG	r;

	knl_memset(&r, 0, sizeof(r));
	if ( fs != NULL ) {
		knl_memcpy(r.family, fs->family, sizeof(r.family));
		knl_memcpy(r.base, fs->base, sizeof(r.base));
		r.cls = fs->cls;
		r.attr = fs->attr;
		r.use = fs->use ? FN_USE_ON : FN_USE_OFF;
	}
	if ( force_on ) {
		r.use = FN_USE_ON;
	}
	(void)fn_set_reg(fid, &r);
}

/* Whether an object is one of those the box links to */
LOCAL BOOL in_list( CONST TS_UUID *u, CONST TS_UUID *ids, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( same_uuid(u, &ids[i]) ) return TRUE;
	}
	return FALSE;
}

/*
 * Every face in the 書体箱 opened, as its settings say, the system's own
 * face chosen among them; a face opened before whose object has left the
 * box is no longer used. Answers how many are open; the system face is
 * set when one is.
 */
EXPORT INT dt_res_faces( void )
{
	TS_UUID	*ids, sys;
	FACESET	*fs;
	ID	fid, pick = 0;
	INT	n, i, k, nf, opened = 0;
	BOOL	have_sys, is_sys = FALSE;
	char	idb[TS_UUID_STRLEN + 1];

	ids = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * RES_MAX);
	fs = (FACESET *)Kmalloc(sizeof(FACESET) * FACE_MAX);
	if ( ids == NULL || fs == NULL ) {
		if ( ids != NULL ) Kfree(ids);
		if ( fs != NULL ) Kfree(fs);
		return 0;
	}
	have_sys = knl_conf_link_get(knl_sys_id(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, idb), CF_ROLE_FACE, &sys);
	if ( !have_sys && box_pick(font_box_id, system_face, &sys) ) {
		have_sys = TRUE;
		(void)knl_conf_link_put(knl_sys_id(SYSROLE_CONF_DEV, SYSDEF_CONF_DEV, idb), CF_ROLE_FACE, &sys,
					"システムの書体：");
	}
	n = box_links(font_box_id, ids, RES_MAX);
	for ( i = 0; i < n; i++ ) {
		BOOL	this_sys = (BOOL)( have_sys && same_uuid(&ids[i], &sys) );

		nf = faces_of(&ids[i], fs, FACE_MAX);
		for ( k = 0; k < ( ( nf > 0 ) ? nf : 1 ); k++ ) {
			INT	index = ( nf > 0 ) ? fs[k].index : 0;

			fid = fn_of_obj(&ids[i], index);
			if ( fid <= 0 && ( fn_open_obj(&ids[i], xf_data_rec(&ids[i]), index, &fid) < E_OK
					   || fid <= 0 ) ) {
				continue;
			}
			opened_note(fid, &ids[i], index);
			face_reg(fid, ( nf > 0 ) ? &fs[k] : NULL, (BOOL)( this_sys && index == 0 ));
			opened++;
			if ( this_sys && !is_sys && index == 0 ) {
				is_sys = TRUE;
				pick = fid;
			}
			if ( pick == 0 && ( nf == 0 || fs[k].use ) ) {
				pick = fid;		/* any face rather than none */
			}
		}
	}
	/* what has gone from the box is not used any more */
	for ( k = 0; k < dt_nfaces; k++ ) {
		if ( !in_list(&dt_faces[k].obj, ids, n) ) {
			T_FNREG	r;

			if ( fn_get_reg(dt_faces[k].fid, &r) >= E_OK && r.use != FN_USE_OFF
			  && dt_faces[k].fid != fn_system() ) {
				r.use = FN_USE_OFF;
				(void)fn_set_reg(dt_faces[k].fid, &r);
			}
		}
	}
	Kfree(fs);
	Kfree(ids);
	if ( pick > 0 && pick != fn_system() ) {
		fn_set_size(pick, 14);
		fn_set_system(pick);
	}
	return opened;
}

/* ---------------------------------------------------------------- what システム環境設定 asks */

/* The object the system's own face is in, when the box has it */
LOCAL BOOL system_obj( TS_UUID *p_u )
{
	INT	k;

	for ( k = 0; k < dt_nfaces; k++ ) {
		if ( dt_faces[k].fid == fn_system() ) {
			*p_u = dt_faces[k].obj;
			return TRUE;
		}
	}
	return FALSE;
}

/* Every face of an object opened here no longer used */
LOCAL void faces_off( CONST TS_UUID *u )
{
	INT	k;

	for ( k = 0; k < dt_nfaces; k++ ) {
		T_FNREG	r;

		if ( same_uuid(&dt_faces[k].obj, u) && fn_get_reg(dt_faces[k].fid, &r) >= E_OK ) {
			r.use = FN_USE_OFF;
			(void)fn_set_reg(dt_faces[k].fid, &r);
		}
	}
}

/* An object deleted, whatever keeps it from being so by mistake */
LOCAL ER del_font_obj( CONST TS_UUID *u )
{
	T_OBPRT	*prt = knl_ob_prt_get(u);

	if ( prt != NULL && ( prt->attr & ( OB_A_PERM | OB_A_RONLY ) ) ) {
		prt->attr &= ~( OB_A_PERM | OB_A_RONLY );
		(void)ob_set_prt(u, prt);
	}
	if ( prt != NULL ) Kfree(prt);
	return ob_del_obj(u);
}

/* One face's settings, as the request's text says them, into the object's resource */
LOCAL ER face_set( CONST TS_UUID *u, INT index, CONST UB *text )
{
	FACESET	*fs, *f, got;
	T_JSON	o;
	INT	n, len = 0;
	ER	er;

	while ( len < DT_CARRY_TEXT && text[len] != 0 ) len++;
	if ( len == 0 ) {
		return E_OK;
	}
	if ( js_parse(text, len, &o) < E_OK ) {
		return E_PAR;
	}
	face_read(&o, &got);
	got.index = index;
	fs = (FACESET *)Kmalloc(sizeof(FACESET) * FACE_MAX);
	if ( fs == NULL ) {
		return E_NOMEM;
	}
	n = faces_of(u, fs, FACE_MAX);
	f = face_in(fs, &n, index);
	if ( f == NULL ) {
		Kfree(fs);
		return E_LIMIT;
	}
	*f = got;
	er = faces_put(u, fs, n);
	Kfree(fs);
	return er;
}

/* The faces of the box of the same family and look as fid, other than u, taken out and deleted */
LOCAL void same_out( CONST TS_UUID *u, ID fid )
{
	UB	fam[FN_NAME_MAX], other[FN_NAME_MAX];
	T_FNREG	r, q;
	TS_UUID	box, gone;
	INT	k;

	if ( fn_family(fid, fam, sizeof(fam)) <= 0 || fn_get_reg(fid, &r) < E_OK
	  || !box_of(font_box_id, &box) ) {
		return;
	}
	for ( k = 0; k < dt_nfaces; k++ ) {
		if ( same_uuid(&dt_faces[k].obj, u) || dt_faces[k].fid == fn_system()
		  || fn_get_reg(dt_faces[k].fid, &q) < E_OK || q.use == FN_USE_OFF
		  || fn_family(dt_faces[k].fid, other, sizeof(other)) <= 0
		  || !same_name(fam, other) || ( q.attr & FN_FA_LOOK ) != ( r.attr & FN_FA_LOOK ) ) {
			continue;
		}
		gone = dt_faces[k].obj;
		(void)om_obj_link_del(&box, &gone);
		faces_off(&gone);
		(void)del_font_obj(&gone);
		tm_printf((UB *)"desktop: font replaced\n");
	}
}

/* The settings an object's faces are drawn with now, for a copy to carry */
LOCAL INT faces_now( CONST TS_UUID *u, FACESET *fs )
{
	INT	n = faces_of(u, fs, FACE_MAX), k;

	for ( k = 0; k < dt_nfaces; k++ ) {
		T_FNREG	r;
		FACESET	*f;

		if ( !same_uuid(&dt_faces[k].obj, u) || fn_get_reg(dt_faces[k].fid, &r) < E_OK ) {
			continue;
		}
		f = face_in(fs, &n, dt_faces[k].index);
		if ( f == NULL ) {
			continue;
		}
		if ( f->family[0] == 0 ) {
			(void)fn_family(dt_faces[k].fid, f->family, sizeof(f->family));
		}
		f->use = (BOOL)( r.use != FN_USE_OFF );
	}
	return n;
}

/*
 * 保存: a copy of the face object under a name and on a disk asked for,
 * a link to it in that disk's top -- the first window for the system's
 * disk. The copy carries the settings its faces are drawn with.
 */
#define FS_PN_NAME	1
#define FS_PN_TO	10
#define FS_PN_TO_MAX	8

LOCAL ER font_save( CONST T_DTREQ *rq, T_DTANS *an )
{
	DTWIN		*host = dt_host_of(&rq->win);
	UB		(*to)[64], t[WM_LABEL_MAX];
	T_OBFVREF	ref;
	T_OBREF		or;
	T_OBCRE		c;
	T_WMPANEL	*def;
	T_WMPART	*pt;
	FACESET		*fs;
	TS_UUID		top, nu;
	UB		name[OB_NAME_MAX];
	INT		n, m = 0, i, pid = -1, w = 460, h, pick = -1;
	UINT		ans;
	ER		er;

	if ( host == NULL || ob_ref_obj(&rq->target, &or) < E_OK ) {
		return E_NOEXS;
	}
	to = (UB (*)[64])Kmalloc(sizeof(UB) * 64 * FS_PN_TO_MAX);
	if ( to == NULL ) {
		return E_NOMEM;
	}
	n = knl_obfile_vols((UB *)to, 64, FS_PN_TO_MAX);
	for ( i = 0; i < n; i++ ) {
		if ( knl_obfile_vol_ref((CONST char *)to[i], &ref) >= E_OK && !ref.rdonly ) {
			if ( m != i ) knl_memcpy(to[m], to[i], 64);
			m++;
		}
	}
	h = 128 + 26 * m;
	def = dt_pn_new(w, h);
	if ( def == NULL || m == 0 ) {
		if ( def != NULL ) Kfree(def);
		Kfree(to);
		return ( m == 0 ) ? E_NOEXS : E_NOMEM;
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 4, w - 28, 22, "保存先のディスク：");
	for ( i = 0; i < m; i++ ) {
		INT	k = 0;

		if ( knl_obfile_vol_ref((CONST char *)to[i], &ref) >= E_OK && ref.system ) {
			k = bput(t, 0, sizeof(t), "システムのディスク（初期ウィンドウ）");
		} else if ( knl_obfile_vol_ref((CONST char *)to[i], &ref) >= E_OK && ref.label[0] != 0 ) {
			k = bput(t, 0, sizeof(t), (CONST char *)ref.label);
		} else {
			k = bput(t, 0, sizeof(t), "ディスク ");
			(void)bput(t, k, sizeof(t), (CONST char *)to[i]);
		}
		pt = dt_pn_add(def, WM_PT_CHOICE, FS_PN_TO + i, 24, 30 + 26 * i, w - 48, 22, (CONST char *)t);
		if ( pt != NULL ) {
			pt->group = FS_PN_TO;
			pt->value = ( i == 0 ) ? 1 : 0;
		}
	}
	dt_pn_add(def, WM_PT_LABEL, 0, 14, 36 + 26 * m, 80, 22, "実身名：");
	pt = dt_pn_add(def, WM_PT_BOX | P_JP, FS_PN_NAME, 96, 34 + 26 * m, w - 110, 26, "");
	if ( pt != NULL ) {
		for ( i = 0; or.name[i] != 0 && i < WM_LABEL_MAX - 1; i++ ) pt->text[i] = or.name[i];
		pt->text[i] = 0;
		pt->caret = i;
	}
	dt_pn_buttons(def, "保存");
	ans = dt_pn_run(host, def, &pid);
	name[0] = 0;
	if ( ans == WM_ANS_OK && pid >= 0 ) {
		for ( i = 0; i < m; i++ ) {
			if ( dt_pn_value(pid, FS_PN_TO + i) != 0 ) pick = i;
		}
		wm_panel_text(pid, FS_PN_NAME, name, sizeof(name));
	}
	dt_pn_done(host, pid);
	wm_update();
	if ( ans != WM_ANS_OK || pick < 0 ) {
		Kfree(to);
		return E_ABORT;
	}
	if ( name[0] == 0 ) {
		knl_memcpy(name, or.name, sizeof(name));
	}
	knl_memset(&c, 0, sizeof(c));
	c.name = name;
	c.vol = (CONST char *)to[pick];
	er = ob_cpy_obj(&rq->target, &c, &nu);
	if ( er >= E_OK ) {
		fs = (FACESET *)Kmalloc(sizeof(FACESET) * FACE_MAX);
		if ( fs != NULL ) {
			INT	nf = faces_now(&rq->target, fs);

			if ( nf > 0 ) (void)faces_put(&nu, fs, nf);
			Kfree(fs);
		}
		if ( !dt_disk_top((CONST char *)to[pick], &top) || dt_place_in(&top, &nu, name) < E_OK ) {
			dt_place_on_desktop(&nu, name);
		}
		an->into = nu;
		tm_printf((UB *)"desktop: font saved \"%s\" on %s\n", (CONST char *)name,
			  (CONST char *)to[pick]);
	}
	Kfree(to);
	return er;
}

/*
 * A face opened to be looked at (見本): a face of the object in use that
 * its family finds is named by its family; any other -- not in use, not
 * registered, or not the one its family finds first (a bold face of a
 * family) -- is opened once more, on its own, under a family nothing
 * else has.
 */
LOCAL ER font_try( CONST T_DTREQ *rq, T_DTANS *an )
{
	char	us[TS_UUID_STRLEN + 1];
	UB	fam[FN_NAME_MAX];
	T_FNREG	r;
	ID	fid = 0;
	INT	k, i;

	(void)ts_uuid_to_str(&rq->target, us, sizeof(us));
	k = bput(fam, 0, sizeof(fam), "見本:");
	k = bput(fam, k, sizeof(fam), us);
	k = bput(fam, k, sizeof(fam), ":");
	(void)bput_num(fam, k, sizeof(fam), ( rq->recno > 0 ) ? rq->recno : 0);
	/* a face opened before to be looked at */
	for ( i = 1; i <= FN_MAX_FONT; i++ ) {
		UB	f[FN_NAME_MAX];

		if ( fn_get_reg(i, &r) >= E_OK && r.use == FN_USE_TRIAL
		  && fn_family(i, f, sizeof(f)) > 0 && same_name(f, fam) ) {
			fid = i;
		}
	}
	if ( ( rq->flags & DT_F_FONT_DONE ) != 0 ) {
		return ( fid > 0 ) ? fn_close(fid) : E_OK;
	}
	if ( fid == 0 ) {
		ID	have = fn_of_obj(&rq->target, ( rq->recno > 0 ) ? rq->recno : 0);

		/* in use, and what its family names */
		if ( have > 0 && fn_get_reg(have, &r) >= E_OK && r.use == FN_USE_ON
		  && fn_family(have, an->text, DT_TEXT_MAX) > 0 && fn_find(an->text) == have ) {
			return E_OK;
		}
		if ( fn_open_obj(&rq->target, xf_data_rec(&rq->target), ( rq->recno > 0 ) ? rq->recno : 0,
				 &fid) < E_OK || fid <= 0 ) {
			return E_OBJ;
		}
		knl_memset(&r, 0, sizeof(r));
		knl_memcpy(r.family, fam, sizeof(r.family));
		r.use = FN_USE_TRIAL;
		(void)fn_set_reg(fid, &r);
	}
	(void)bput(an->text, 0, DT_TEXT_MAX, (CONST char *)fam);
	return E_OK;
}

EXPORT ER dt_font_req( CONST T_DTREQ *rq, T_DTANS *an )
{
	TS_UUID	box, u, sysu;
	INT	index = ( rq->recno > 0 ) ? rq->recno : 0;
	ER	er = E_OK;

	if ( !box_of(font_box_id, &box) ) {
		return E_SYS;
	}
	switch ( rq->req ) {
	case DT_RQ_FONT_REG:
	{
		T_OBREF	r;
		T_OBCRE	c;
		T_OBPRT	prt;

		if ( ob_ref_obj(&rq->target, &r) < E_OK ) {
			return E_NOEXS;
		}
		dt_sys_prt(&prt);
		prt.attr = 0;			/* what is registered can be deleted */
		knl_memset(&c, 0, sizeof(c));
		c.name = r.name;
		c.near = box;
		c.prt = &prt;
		er = ob_cpy_obj(&rq->target, &c, &u);
		if ( er < E_OK ) {
			break;
		}
		er = face_set(&u, index, rq->text);
		if ( er >= E_OK ) er = om_obj_link_add(&box, &u);
		if ( er < E_OK ) {
			(void)del_font_obj(&u);
			break;
		}
		(void)dt_res_faces();
		if ( ( rq->flags & DT_F_FONT_UPDATE ) != 0 && fn_of_obj(&u, index) > 0 ) {
			same_out(&u, fn_of_obj(&u, index));
			(void)dt_res_faces();
		}
		an->into = u;
		tm_printf((UB *)"desktop: font registered \"%s\"%s\n", (CONST char *)r.name,
			  ( ( rq->flags & DT_F_FONT_UPDATE ) != 0 ) ? " (update)" : "");
		break;
	}
	case DT_RQ_FONT_SET:
		er = face_set(&rq->target, index, rq->text);
		if ( er >= E_OK ) (void)dt_res_faces();
		break;
	case DT_RQ_FONT_USE:
	{
		FACESET	*fs, *f;
		INT	n;

		if ( ( rq->flags & DT_F_FONT_OFF ) != 0 && system_obj(&sysu) && same_uuid(&sysu, &rq->target)
		  && index == 0 ) {
			return E_RONLY;
		}
		fs = (FACESET *)Kmalloc(sizeof(FACESET) * FACE_MAX);
		if ( fs == NULL ) {
			return E_NOMEM;
		}
		n = faces_of(&rq->target, fs, FACE_MAX);
		f = face_in(fs, &n, index);
		if ( f != NULL ) {
			f->use = (BOOL)( ( rq->flags & DT_F_FONT_OFF ) == 0 );
			er = faces_put(&rq->target, fs, n);
		} else {
			er = E_LIMIT;
		}
		Kfree(fs);
		if ( er >= E_OK ) (void)dt_res_faces();
		break;
	}
	case DT_RQ_FONT_DEL:
		if ( system_obj(&sysu) && same_uuid(&sysu, &rq->target) ) {
			return E_RONLY;
		}
		er = om_obj_link_del(&box, &rq->target);
		if ( er >= E_OK ) {
			faces_off(&rq->target);
			er = del_font_obj(&rq->target);
			(void)dt_res_faces();
			tm_printf((UB *)"desktop: font deleted (%d)\n", (INT)er);
		}
		break;
	case DT_RQ_FONT_SAVE:
		er = font_save(rq, an);
		break;
	case DT_RQ_FONT_TRY:
		er = font_try(rq, an);
		break;
	default:
		er = E_PAR;
		break;
	}
	return er;
}

/*
 * The picture the ground is laid with: the system's own among those in
 * the 壁紙箱, or the first there when it is not among them.
 */
EXPORT ER dt_res_wall( UINT mode )
{
	TS_UUID	box, pick;

	if ( !box_pick(wall_box_id, system_wall, &pick) ) {
		if ( !box_of(wall_box_id, &box) || ob_fnd_lnk(&box, NULL, &pick) < E_OK ) {
			return E_NOEXS;
		}
	}
	return wm_load_wall_obj(&pick, xf_data_rec(&pick), mode);
}
