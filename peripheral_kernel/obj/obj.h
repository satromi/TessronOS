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

#define OB_MGR_MAX	16		/* managers */

/*
 * A watcher of an object for a poll set (obpoll.c), on a list the object
 * keeps. The lists are the poll sets' own, under their lock: an object
 * only holds the head.
 */
typedef struct OBPWAIT {
	struct OBPWAIT	*next, *prev;
	struct OBPLIST	*on;		/* the list it is on; NULL none */
} OBPWAIT;

typedef struct OBPLIST {
	OBPWAIT	*first;
} OBPLIST;

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
	/* the protection, in memory of its own (*p_prt, Kfree it) */
	ER	(*prot)( CONST TS_UUID *uuid, T_OBPRT **p_prt );
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

	/* a record's bytes replaced whole in one change (ob_rpl_rec); NULL: written over and cut */
	ER	(*rpl)( INT h, INT recno, CONST void *buf, SZ size );

	/* what was changed through the handle on the medium (ob_syn_obj); NULL: nothing waits */
	ER	(*syn)( INT h );

	/*
	 * a record read, waited for up to tmo (ob_rea_tmo): whatever the key
	 * says of waiting, TMO_POL not at all, TMO_FEVR until there is
	 * something; NULL: the manager has nothing that comes in (E_NOSPT)
	 */
	ER	(*rtm)( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, TMO tmo );

	/*
	 * What holds for handle h now, as a poll set asks (OB_PL_IN, OB_PL_OUT,
	 * OB_PL_HUP, OB_PL_RDHUP, OB_PL_ERR); with w, w is put on the object's
	 * watchers (knl_obpoll_link) in the same take of the manager's lock,
	 * so that no change after the look goes untold. A manager that has
	 * it never names another object by a handle once its object is gone
	 * (a generation in the handle), tells its watchers of every change
	 * that may make the object ready (knl_obpoll_tell), and lets them go
	 * when the object goes (knl_obpoll_gone), both under that lock.
	 * NULL: its objects cannot be watched (E_NOSPT).
	 */
	INT	(*pol)( INT h, OBPWAIT *w );

	/*
	 * the object's name, as ob_ref_nam puts it: at most size bytes in
	 * buf, cut at a whole letter, the whole size in *p_asize; NULL: the
	 * name ref gives is the whole name
	 */
	ER	(*refnam)( CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize );
} T_OBMGR;

/* The name manager (obname.c) */
IMPORT INT  knl_ob_regist( CONST T_OBMGR *mgr );	/* the manager's number */
IMPORT CONST T_OBMGR *knl_ob_mgr_of( CONST TS_UUID *uuid );

/*
 * A number that moves on whenever the object -- its records or its
 * metadata -- is changed through the name manager (a channel's sending
 * is not a change). One who keeps what it read of an object reads the
 * number before reading the object, and reads the object again once the
 * number differs. Objects may share a number: it moves on for any of
 * them, so it can differ without a change, never stay without one.
 */
IMPORT UW   knl_ob_chg_serial( CONST TS_UUID *uuid );

/*
 * A name of the manager's own put as ob_ref_nam puts one: at most size
 * bytes of s in buf, cut at a whole letter, with its nought; the whole
 * size in *p_asize. E_PAR for size below 0.
 */
IMPORT ER   knl_ob_name_put( CONST UB *s, SZ len, UB *buf, SZ size, SZ *p_asize );
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
IMPORT ER knl_obstrm_init( void );		/* obstrm.c: the pairs of streams */
IMPORT ER knl_obpoll_init( void );		/* obpoll.c: the poll sets */
IMPORT ER knl_gpbo_init( void );		/* device/gpu/gpu_obj.c: the BOs of the GPU as objects */

/*
 * The ends of streams and the BO objects a process made and never
 * opened go when it ends (knl_ob_prc_end, after its keys are closed)
 */
IMPORT void knl_obstrm_prc_end( ID pid );
IMPORT void knl_obpoll_prc_end( ID pid );
IMPORT void knl_gpbo_prc_end( ID pid );

/*
 * A key carried by a stream (obstrm.c): held, it keeps the object it
 * names open as the key did, with the key's operations, until it is
 * given to a process as a key of its own or let go. knl_ob_key_hold
 * takes the calling process's key (it stays the caller's); E_ID: not a
 * key, E_OACV: another's. knl_ob_key_give makes the new key, for pid
 * (0: the calling task of the kernel), and the hold is spent either
 * way; knl_ob_key_drop lets it go. Neither of the last two may be called
 * holding a lock a manager's close takes.
 */
typedef struct {
	INT	hnd;			/* the opened object held; -1 none */
	UINT	ops;
	BOOL	nowait;
	UINT	type, sub;		/* its manager's */
} T_OBKHOLD;

IMPORT ER   knl_ob_key_hold( ID key, T_OBKHOLD *h, TS_UUID *p_uuid );
IMPORT ID   knl_ob_key_give( T_OBKHOLD *h, ID pid );
IMPORT void knl_ob_key_drop( T_OBKHOLD *h );

/*
 * Poll sets (obpoll.c). knl_ob_key_watch takes a key of the caller's
 * that may read or write: the manager and handle of the object it opens
 * and what holds for it now (its manager's pol, w put on its watchers
 * unless NULL), or an error. The object's manager calls the other three
 * under its own lock.
 */
IMPORT INT  knl_ob_key_watch( ID key, OBPWAIT *w, CONST T_OBMGR **p_mgr, INT *p_h );
IMPORT void knl_obpoll_link( OBPLIST *l, OBPWAIT *w );
IMPORT void knl_obpoll_tell( OBPLIST *l );	/* the object changed: its watchers made ready */
IMPORT void knl_obpoll_gone( OBPLIST *l, BOOL tell );	/* the object goes: its watchers let go, told an error when keys to it are left */

/* The objects of a manager's kind process pid holds keys to: their UUIDs, at most max; how many */
IMPORT INT  knl_ob_key_objs( ID pid, UINT type, UINT sub, TS_UUID *buf, INT max );

/*
 * Where a USB device is plugged in, as its objects are named (obusb.c
 * knl_obusb_path, "0-4.3"), with its nought: the controller (UINT, 10
 * digits), the root port (xHCI MaxPorts is 8 bits, 3 digits) and a port
 * of 4 bits (the xHCI route string) for each of the 5 hubs there may be
 * (USB 2.0 4.1.1). The names made of it: "usb" and it; "hid", it and
 * ".i" with the interface number (8 bits).
 */
#define OB_USB_PATH_MAX		( 10 + 1 + 3 + 5 * 3 + 1 )
#define OB_USB_NAME_MAX		( 3 + OB_USB_PATH_MAX )
/* A device's product string, UTF-8, whole: as long as a string descriptor can be (USB_STR_MAX, usbdev.h) */
#define OB_USB_PRODUCT_MAX	( 126 * 3 + 1 )
IMPORT INT  knl_usb_dev_product( INT dev, UB *buf, INT max );	/* device/usb/usbman.c */
#define OB_HID_NAME_MAX		( 3 + OB_USB_PATH_MAX - 1 + 2 + 3 + 1 )

/* Volumes of the file manager */
IMPORT ER   knl_obfile_attach( CONST char *path, UINT flags );
IMPORT ER   knl_obfile_detach( CONST char *path );
IMPORT ER   knl_obfile_trx( CONST TS_UUID *on, BOOL begin, BOOL commit );
IMPORT INT  knl_obfile_revoke( CONST UB *devnm );	/* a device's medium went: its volumes are gone */
IMPORT void knl_ob_watch_keys_drop_of( CONST TS_UUID *uuids, INT n );	/* their keys only for notices closed */
IMPORT INT  knl_obfile_shut( BOOL finish );		/* the machine going off */

/* Protection domains (18.9): the one an object is under, E_NOEXS when none */
IMPORT T_OBPRT *knl_obfile_domain( CONST TS_UUID *uuid );	/* Kfree it; NULL none */
IMPORT T_OBPRT *knl_obfile_get_dom( CONST char *path );
IMPORT ER   knl_obfile_set_dom( CONST char *path, CONST T_OBPRT *prt );

/* What one manager asks of another */
IMPORT ID   knl_obfile_vol_of( CONST TS_UUID *uuid );	/* the TSFS volume, or an error */
IMPORT ID   knl_obfile_first_vol( void );
IMPORT ID   knl_obfile_vol_path( CONST char *path );	/* the TSFS volume attached with a path */
IMPORT void knl_obdev_volume_up( void );	/* a volume came: the device box can be kept */
IMPORT BOOL knl_obmem_name_taken( CONST UB *name );
IMPORT BOOL knl_obchan_name_taken( CONST UB *name );
IMPORT BOOL knl_obchan_merge_move( CONST TS_UUID *port, CONST T_OBNTM *msg );	/* a waiting move brought up to date */

/* The devices outside the device management, each serving its own (obdev.c knl_obdev_start) */
IMPORT void knl_obrand_start( void );		/* obrand.c */
IMPORT void knl_obdisp_start( void );		/* obdisp.c */
IMPORT void knl_obinput_start( void );		/* obinput.c */
IMPORT void knl_obgpio_start( void );		/* obgpio.c */
IMPORT void knl_gpu_obj_start( void );		/* device/gpu/gpu_obj.c: the GPU's device object */
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

/*
 * A text made as large as it needs (a record made when it is read, a
 * list of any length): fn writes into t of max bytes with the calls
 * above and answers the length, -1 when it did not fit -- it is then
 * called again with a buffer twice as large -- or another error. No
 * list is cut to fit a buffer.
 */
typedef INT (*OJ_MAKE)( void *ctx, UB *t, INT max );
IMPORT INT  knl_oj_make( OJ_MAKE fn, void *ctx, UB **p_buf );	/* the length; Kfree *p_buf */
IMPORT ER   knl_oj_make_rea( OJ_MAKE fn, void *ctx, D off, void *buf, SZ size, SZ *p_asize );
IMPORT UD   knl_oj_make_len( OJ_MAKE fn, void *ctx );

/*
 * A record read whole, however long, into a buffer with room bytes
 * more and a nought after the text: the length, the buffer in *p_buf
 * (Kfree it) and the bytes it holds in *p_cap, or an error (obname.c)
 */
IMPORT INT  knl_ob_rea_all( ID key, INT recno, INT room, UB **p_buf, INT *p_cap );

/*
 * Metadata read whole, however long (objson.c): get copies at most size
 * bytes from the start and says how many; the length, the buffer in
 * *p_buf (Kfree it, room bytes more and a nought after the text) and the
 * bytes it holds in *p_cap, or an error.
 */
typedef ER (*FP_META_GET)( void *ctx, UB *buf, SZ size, SZ *p_asize );
IMPORT INT  knl_meta_all( FP_META_GET get, void *ctx, INT room, UB **p_buf, INT *p_cap );
/* the same of a volume's object: knl_tsfs_meta_all (ts/tsfs.h) */

/* ---------------------------------------------------------------- protection (obprot.c) */

IMPORT void knl_ob_prt_legacy( T_OBPRT *prt, BOOL readable, BOOL editable,
			       BOOL deletable );
IMPORT void knl_ob_prt_default( T_OBPRT *prt, CONST T_OBCRD *crd );
/* the protection the metadata keeps, in memory of its own (*p_prt, Kfree it) */
IMPORT ER   knl_ob_prt_parse( CONST UB *json, INT len, T_OBPRT **p_prt );
/*
 * The protection written into the metadata *p_json of len bytes, which
 * holds *p_cap: made larger (a new buffer in *p_json and *p_cap, the old
 * one freed) when the text does not fit. The new length, or -1.
 */
IMPORT INT  knl_ob_prt_store( UB **p_json, INT len, INT *p_cap, CONST T_OBPRT *prt );
IMPORT INT  knl_ob_ops_text( UB *out, INT pos, INT max, UINT ops );
IMPORT BOOL knl_ob_uuid_zero( CONST TS_UUID *u );

/* ---------------------------------------------------------------- hashing (obhash.c) */

#define OB_SHA256_LEN	32
IMPORT void knl_ob_sha256( CONST UB *data, SZ len, UB out[OB_SHA256_LEN] );
IMPORT void knl_ob_pbkdf2( CONST UB *pw, SZ pwlen, CONST UB *salt, SZ saltlen,
			   INT iter, UB out[OB_SHA256_LEN] );

#endif /* __OBJ_H__ */
