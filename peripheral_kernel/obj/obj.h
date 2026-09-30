/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obj.h
 *	Inside the object layer: what a manager gives the name manager,
 *	and the helpers the managers share (design 18.3)
 */

#ifndef __OBJ_H__
#define __OBJ_H__

#include <ts/ob.h>

#define OB_META_MAX	OB_ATR_MAX	/* bytes of metadata handled at a time */
#define OB_MGR_MAX	16		/* managers */

/*
 * A manager: the part of the peripheral kernel that serves one kind of
 * object. The handle is the manager's own number for an open object. A
 * call a manager has no use for is left NULL and answers E_NOSPT.
 *
 * Protection is the name manager's: a manager only keeps what it is
 * given and hands it back (prot/setprot), and never checks it.
 */
typedef struct {
	UINT	type;			/* OB_T_* */
	UINT	sub;			/* OB_S_*, 0 for any */
	CONST char *name;

	/* E_OK when the object is this manager's, E_NOEXS when it is not */
	ER	(*find)( CONST TS_UUID *uuid );
	ER	(*ref)( CONST TS_UUID *uuid, T_OBREF *pk_ref );
	ER	(*prot)( CONST TS_UUID *uuid, T_OBPRT *prt );
	ER	(*setprot)( CONST TS_UUID *uuid, CONST T_OBPRT *prt );
	ER	(*create)( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid );
	ER	(*remove)( CONST TS_UUID *uuid );
	INT	(*open)( CONST TS_UUID *uuid, UINT ops );	/* a handle, above 0 */
	ER	(*close)( INT h );
	ER	(*rea)( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize,
			BOOL nowait );
	ER	(*wri)( INT h, INT recno, D off, CONST void *buf, SZ size,
			SZ *p_asize, BOOL nowait );
	ER	(*apd)( INT h, UINT rt, UINT sub, INT *p_recno );
	ER	(*trn)( INT h, INT recno, UD size );
	ER	(*drc)( INT h, INT recno );
	ER	(*lrc)( INT h, T_OBREC *buf, INT n, INT *p_cnt );
	ER	(*gat)( INT h, UB *json, SZ size, SZ *p_asize );
	ER	(*sat)( INT h, CONST UB *json, SZ size );
	ER	(*lnk)( CONST TS_UUID *uuid, INT delta );
	ER	(*lst)( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt );
	ER	(*fndnam)( CONST UB *name, TS_UUID *p_uuid );

	/* resources, by the name after the UUID */
	ER	(*rres)( INT h, CONST UB *name, D off, void *buf, SZ size, SZ *p_asize );
	ER	(*wres)( INT h, CONST UB *name, CONST void *buf, SZ size );
	ER	(*dres)( INT h, CONST UB *name );
	ER	(*lres)( INT h, UB *buf, SZ size, INT *p_cnt );

	/* one of its records onto another object of the same manager */
	/* target above 0: a handle of the same manager; below 0: -pid, a process's space */
	ER	(*map)( INT h, INT recno, INT target, T_OBMAP *m );
	ER	(*unm)( INT h, INT recno, INT target );

	/* the icon, beside the attributes: read (its size in *p_asize), replaced (0 bytes: none) */
	ER	(*gic)( INT h, UB *buf, SZ size, SZ *p_asize );
	ER	(*sic)( INT h, CONST UB *buf, SZ size );
} T_OBMGR;

/* The name manager (obname.c) */
IMPORT INT  knl_ob_regist( CONST T_OBMGR *mgr );	/* the manager's number */
IMPORT CONST T_OBMGR *knl_ob_mgr_of( CONST TS_UUID *uuid );
IMPORT CONST T_OBCRD *knl_ob_crd_of( ID pid );
IMPORT ER   knl_ob_meta_read( CONST TS_UUID *uuid, UB *buf, INT max, INT *p_len );
IMPORT ER   knl_ob_meta_write( CONST TS_UUID *uuid, CONST UB *buf, INT len );
IMPORT BOOL knl_ob_is_admin( CONST T_OBCRD *crd );
IMPORT ER   knl_ob_set_crd( ID pid, CONST T_OBCRD *crd );

/*
 * What a process may do to another (obprc.c, design 18.8): through the
 * protection of the other's process object, and as its parent or child
 * when 'kin' says so
 */
#define OBP_PARENT	0x0001		/* the caller is the other's parent */
#define OBP_CHILD	0x0002		/* the caller is the other's child */
IMPORT ER   knl_obprc_may( ID pid, UINT ops, UINT kin );

/* Users (obuser.c): the credentials a user's password opens, checked */
IMPORT ER   knl_ob_login_check( CONST TS_UUID *user, CONST UB *password, T_OBCRD *crd );
IMPORT CONST T_OBCRD knl_ob_crd_system;

/* The managers, each registering itself */
IMPORT ER knl_obfile_init( void );
IMPORT ER knl_obmem_init( void );
IMPORT ER knl_obdev_init( void );
IMPORT ER knl_obprc_init( void );
IMPORT ER knl_obchan_init( void );
IMPORT ER knl_obsock_init( void );		/* obsock.c: the sockets of the network */

/* Volumes of the file manager */
IMPORT ER   knl_obfile_attach( CONST char *path, UINT flags );
IMPORT ER   knl_obfile_detach( CONST char *path );
IMPORT ER   knl_obfile_trx( CONST TS_UUID *on, BOOL begin, BOOL commit );

/* Protection domains (18.9): the one an object is under, E_NOEXS when none */
IMPORT ER   knl_obfile_domain( CONST TS_UUID *uuid, T_OBPRT *prt );
IMPORT ER   knl_obfile_get_dom( CONST char *path, T_OBPRT *prt );
IMPORT ER   knl_obfile_set_dom( CONST char *path, CONST T_OBPRT *prt );

/* What one manager asks of another */
IMPORT ID   knl_obfile_vol_of( CONST TS_UUID *uuid );	/* the TSFS volume, or an error */
IMPORT ID   knl_obfile_first_vol( void );
IMPORT void knl_obdev_volume_up( void );	/* a volume came: the device box can be kept */
IMPORT BOOL knl_obmem_name_taken( CONST UB *name );
IMPORT BOOL knl_obchan_name_taken( CONST UB *name );

/* The devices outside the device management, each serving its own (obdev.c knl_obdev_start) */
IMPORT void knl_obrand_start( void );		/* obrand.c */
IMPORT void knl_obdisp_start( void );		/* obdisp.c */
IMPORT void knl_obinput_start( void );		/* obinput.c */
IMPORT void knl_obgpio_start( void );		/* obgpio.c */
IMPORT void knl_obusb_start( void );		/* obusb.c, and the keyboards and pointers */
IMPORT void knl_obinput_sync( void );		/* the keyboards and pointers there are now */

/* ---------------------------------------------------------------- the system object (obsys.c) */

IMPORT INT  knl_obsys_text( INT recno, UB *t, INT max );
IMPORT ER   knl_obsys_write( INT recno, CONST UB *buf, SZ size );
IMPORT void knl_obsys_power( FP fn );			/* ER fn( INT how ): 0 off, 1 restart */
IMPORT void knl_obsys_wall( CONST TS_UUID *u, INT mode );
IMPORT void knl_obsys_changed( INT recno );

/* ---------------------------------------------------------------- JSON (objson.c) */

/*
 * A small reader and writer of JSON text in a buffer, enough for the
 * metadata: finding a member of an object, walking an array, reading a
 * string or a number, and replacing or adding a member in place. The
 * rest of the text is kept byte for byte.
 */
IMPORT INT  knl_oj_ws( CONST UB *j, INT len, INT p );
IMPORT INT  knl_oj_skip( CONST UB *j, INT len, INT p );		/* past one value */
IMPORT INT  knl_oj_root( CONST UB *j, INT len );		/* the '{' of the text */
IMPORT INT  knl_oj_member( CONST UB *j, INT len, INT obj, CONST char *key );
IMPORT INT  knl_oj_next( CONST UB *j, INT len, INT arr, INT *p_pos );
IMPORT INT  knl_oj_str( CONST UB *j, INT len, INT p, UB *out, INT max );
IMPORT BOOL knl_oj_num( CONST UB *j, INT len, INT p, D *p_val );
IMPORT BOOL knl_oj_bool( CONST UB *j, INT len, INT p, BOOL *p_val );
IMPORT INT  knl_oj_set( UB *j, INT len, INT max, INT obj, CONST char *key,
			CONST UB *val, INT vlen );
IMPORT INT  knl_oj_path( CONST UB *j, INT len, CONST char *k1, CONST char *k2 );
IMPORT INT  knl_oj_set_path( UB *j, INT len, INT max, CONST char *k1,
			     CONST char *k2, CONST UB *val, INT vlen );

/* Building text */
IMPORT INT  knl_oj_put( UB *out, INT pos, INT max, CONST char *s );
IMPORT INT  knl_oj_put_str( UB *out, INT pos, INT max, CONST UB *s );	/* quoted */
IMPORT INT  knl_oj_put_num( UB *out, INT pos, INT max, D v );
IMPORT INT  knl_oj_put_uuid( UB *out, INT pos, INT max, CONST TS_UUID *u );

/* ---------------------------------------------------------------- protection (obprot.c) */

IMPORT void knl_ob_prt_legacy( T_OBPRT *prt, BOOL readable, BOOL editable,
			       BOOL deletable );
IMPORT void knl_ob_prt_default( T_OBPRT *prt, CONST T_OBCRD *crd );
IMPORT ER   knl_ob_prt_parse( CONST UB *json, INT len, T_OBPRT *prt );
IMPORT INT  knl_ob_prt_store( UB *json, INT len, INT max, CONST T_OBPRT *prt );
IMPORT INT  knl_ob_ops_text( UB *out, INT pos, INT max, UINT ops );
IMPORT BOOL knl_ob_uuid_zero( CONST TS_UUID *u );

/* ---------------------------------------------------------------- hashing (obhash.c) */

#define OB_SHA256_LEN	32
IMPORT void knl_ob_sha256( CONST UB *data, SZ len, UB out[OB_SHA256_LEN] );
IMPORT void knl_ob_pbkdf2( CONST UB *pw, SZ pwlen, CONST UB *salt, SZ saltlen,
			   INT iter, UB out[OB_SHA256_LEN] );

#endif /* __OBJ_H__ */
