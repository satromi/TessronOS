/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obdev.c
 *	The device manager: devices as objects (design 18.7)
 *
 *	Every device the device management knows, and each subunit of one,
 *	is an object; so is the clock, a device this manager makes itself.
 *	Record 0 describes the device (xmlTAD made when it is read); record
 *	1 is its data -- blocks of a disk at byte positions, the bytes of a
 *	stream, the time of the clock. A serial port has two more: record 2
 *	its speed (UW, bits per second) and record 3 what it is (T_SERINFO).
 *
 *	A device's UUID is made (v7) the first time the device is seen and
 *	kept in the device box, an object of fixed UUID on the first volume,
 *	so that a virtual object of a device still finds it after the next
 *	start. Until a volume is there the names and UUIDs are kept here
 *	only, and written out when one comes.
 *
 *	A disk's volume is mounted through its object (fs_attach_dev): the
 *	key's rights on the blocks are the permission, the mount point is
 *	/media and the device's name, and the mount shows in the object's
 *	attributes (tessronos.mount) from the file layer's table, told with
 *	OB_E_CHANGE when it comes and goes. When a medium or a device goes,
 *	the mounts on it go with it.
 *
 *	A key that may only read the attributes -- enough to be told of
 *	what happens to the device -- does not open the device: only one
 *	that may read or write its records does. The device management
 *	has room for few devices open at a time (CNF_MAX_OPNDEV), and a
 *	program watching every disk would take it all.
 *
 *	The system itself is a virtual device of this manager as well
 *	(ob_uuid_system): its records are made and taken in obsys.c.
 *
 *	The list of the devices is an object too, virtual like the clock
 *	(ob_uuid_devlist). Its record 0 holds a link to each device there
 *	is, and whatever is told of any device -- that it came or went,
 *	its medium was put in or taken out, a volume on it was mounted or
 *	taken off -- is told to those who watch the list as well, the
 *	notice naming the device. One request watches them all, those
 *	that come later included.
 *
 *	Some devices are not in the device management: the keyboards and
 *	pointers and the input they feed, the screen, the banks of pins,
 *	each device on the USB bus, the random source. Whoever serves one
 *	adds it here by name with the calls that make its records
 *	(knl_obdev_add, T_OBDVOPS); it is then an object like the others,
 *	kept in the device box under its name, listed, protected by the
 *	mode it gives, and told of when it comes and goes.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/dt.h>
#include <ts/tsfs.h>
#include <ts/ser.h>
#include <ts/blk.h>
#include <ts/fs.h>
#include "obj.h"

#define OBD_FIRST	64		/* entries the table of the devices starts with; it doubles when full */
#define OBD_HND_FIRST	64		/* and the table of the devices open through their objects */
#define ENT_CLOCK	( -1 )		/* OBDHND.ent: the clock */
#define ENT_LIST	( -2 )		/* the list of the devices */
#define ENT_SYSTEM	( -3 )		/* the system (obsys.c) */
#define OBD_NAME	OB_DV_NAME

typedef struct {
	BOOL	used;
	UB	name[OBD_NAME];
	TS_UUID	uuid;
	BOOL	kept;			/* written in the device box */
	BOOL	seen;			/* the device management has it now, or it was added */
	CONST T_OBDVOPS *ops;		/* added by name: its records; NULL a registered device */
	void	*ctx;
	BOOL	fixed;			/* its UUID is a fixed one, not the box's */
	UH	irqcpu;			/* the processor its interrupts go to, 0 the system's choice */
} OBDENT;

typedef struct {
	BOOL	used;
	INT	ent;			/* ENT_CLOCK, ENT_LIST, or the device */
	ID	dd;			/* 0: not opened, the key reads no record */
	W	blksz;
	BOOL	sound;			/* a sound device: records are its data numbers */
	BOOL	camera;			/* a camera: records are its data numbers */
	BOOL	serial;			/* a serial port: records 2 and 3 are its attributes */
	CONST T_OBDVOPS *vops;		/* a device added by name */
	void	*vctx;
	UD	pos;			/* the key's own place in a stream */
} OBDHND;

/* A serial port's records and the data numbers they are */
#define OB_SER_NREC	4
LOCAL CONST W ser_dn[OB_SER_NREC] = { 0, 0, TDN_SER_SPEED, TDN_SER_INFO };

/*
 * A sound device's records and the data numbers they are (OB_SND_*):
 * record 1 is the PCM, the rest its attributes.
 */
LOCAL CONST W snd_dn[OB_SND_NREC] = {
	0,				/* record 0 is the description, not the device's */
	0,				/* PCM */
	-110, -111, -112, -113, -114, -115,	/* info, mode, control, count, end count, buffer */
	-140,				/* status */
	-120, -121,			/* input and output selectors */
	-122, -123, -124, -125, -126, -127, -128, -129, -130, -131, -132, -133, -134,
	-141, -142,			/* the outputs, and the one chosen */
	-143, -144			/* the mixer, and a volume set */
};

/* A camera's records and the data numbers they are (OB_CAM_*, include/ts/cam.h) */
LOCAL CONST W cam_dn[OB_CAM_NREC] = { 0, 0, -1, -2, -3, -4, -5 };

#define TDK_KIND(atr)	( (atr) & 0x00FFU )
#define TDK_SOUND_KIND	0x0080U		/* include/ts/snd.h: TDK_SOUND */
#define TDK_CAMERA_KIND	0x00A0U		/* include/ts/cam.h: TDK_CAMERA */

/* Whether a device is a sound device, from the kind it registered as */
LOCAL BOOL dev_is_sound( CONST UB *name )
{
	T_RDEV	rd;

	return (BOOL)( tk_ref_dev(name, &rd) >= E_OK && TDK_KIND(rd.devatr) == TDK_SOUND_KIND );
}

LOCAL BOOL dev_is_camera( CONST UB *name )
{
	T_RDEV	rd;

	return (BOOL)( tk_ref_dev(name, &rd) >= E_OK && TDK_KIND(rd.devatr) == TDK_CAMERA_KIND );
}

LOCAL BOOL dev_is_serial( CONST UB *name )
{
	T_RDEV	rd;

	return (BOOL)( tk_ref_dev(name, &rd) >= E_OK && TDK_KIND(rd.devatr) == TDK_SERIAL );
}

/*
 * The devices and subunits known (every disk's partitions among them),
 * and the devices open through their objects: as many as there are. An
 * entry is allocated once and never freed, so a key goes on with its
 * entry while another grows the table; a larger table is made whole
 * before it is put in place, and the one before is kept.
 */
typedef struct {
	INT	n;
	OBDENT	*e[];
} OBDTBL;

typedef struct {
	INT	n;
	OBDHND	*e[];
} OBDHTBL;

LOCAL OBDTBL	*obd_tbl = NULL;
LOCAL OBDHTBL	*obd_htbl = NULL;
LOCAL INT	obd_hi = 0;		/* one past the last entry ever used: the loops go no further */

#define OBD(i)		( *obd_tbl->e[(i)] )
#define OBD_N		( ( obd_tbl != NULL ) ? obd_tbl->n : 0 )
#define OBDH(i)		( *obd_htbl->e[(i)] )
#define OBDH_N		( ( obd_htbl != NULL ) ? obd_htbl->n : 0 )

/* Twice as many entries (first) in a table of n, with the lock held; the table, NULL when no memory */
LOCAL void *tbl_grow( void *old, INT first, SZ esz )
{
	OBDTBL	*t = (OBDTBL *)old, *nt;
	INT	n = ( t != NULL ) ? t->n : 0, cap = ( n > 0 ) ? n * 2 : first, i;

	nt = (OBDTBL *)Kcalloc(1, sizeof(OBDTBL) + sizeof(void *) * (SZ)cap);
	if ( nt == NULL ) {
		return NULL;
	}
	for ( i = 0; i < cap; i++ ) {
		nt->e[i] = ( i < n ) ? t->e[i] : (OBDENT *)Kcalloc(1, esz);
		if ( nt->e[i] == NULL ) {
			while ( --i >= n ) Kfree(nt->e[i]);
			Kfree(nt);
			return NULL;
		}
	}
	nt->n = cap;
	__sync_synchronize();
	return nt;
}
LOCAL ID	obd_mtx = 0;
LOCAL BOOL	obd_loaded = FALSE;	/* the device box has been read */
LOCAL UW	obd_serial = 0;		/* each time a device or a medium came or went */

IMPORT T_OBEDGE	knl_tm_log_edge;		/* lib/libtm */

#define LOCK()		tk_loc_mtx(obd_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(obd_mtx)

LOCAL BOOL same_name( CONST UB *a, CONST UB *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == b[i] );
}

LOCAL BOOL is_clock( CONST TS_UUID *u )
{
	return (BOOL)( ts_uuid_cmp(u, &ob_uuid_clock) == 0 );
}

LOCAL BOOL is_list( CONST TS_UUID *u )
{
	return (BOOL)( ts_uuid_cmp(u, &ob_uuid_devlist) == 0 );
}

LOCAL BOOL is_system( CONST TS_UUID *u )
{
	return (BOOL)( ts_uuid_cmp(u, &ob_uuid_system) == 0 );
}

/* Something happened on a device: told on it, and on the list of the devices */
LOCAL void dev_post( CONST TS_UUID *u, UINT event )
{
	knl_ob_post(u, -1, event, NULL);
	knl_ob_post_as(&ob_uuid_devlist, u, -1, event, NULL);
}

LOCAL INT ent_by_name( CONST UB *name )
{
	INT	i;

	for ( i = 0; i < obd_hi; i++ ) {
		if ( OBD(i).used && same_name(OBD(i).name, name) ) return i;
	}
	return -1;
}

LOCAL INT ent_by_uuid( CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < obd_hi; i++ ) {
		if ( OBD(i).used && OBD(i).seen && ts_uuid_cmp(&OBD(i).uuid, u) == 0 ) return i;
	}
	return -1;
}

LOCAL INT ent_new( CONST UB *name )
{
	INT	i, k;

	for ( i = 0; ; i++ ) {
		if ( i == OBD_N ) {
			OBDTBL	*t = (OBDTBL *)tbl_grow(obd_tbl, OBD_FIRST, sizeof(OBDENT));

			if ( t == NULL ) {
				return -1;
			}
			obd_tbl = t;		/* the table before is not freed */
		}
		if ( !OBD(i).used ) {
			knl_memset(&OBD(i), 0, sizeof(OBDENT));
			if ( i >= obd_hi ) obd_hi = i + 1;
			for ( k = 0; name[k] != 0 && k < OBD_NAME - 1; k++ ) {
				OBD(i).name[k] = name[k];
			}
			OBD(i).used = TRUE;
			return i;
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- the device box */

/* The names and UUIDs the box holds, taken in (the box's win over ours) */
LOCAL void box_load( ID vol )
{
	UB	*j = NULL;
	INT	len, arr, pos, e;

	len = knl_tsfs_meta_all(vol, &ob_uuid_devbox, 0, &j, NULL);
	if ( len >= 0 ) {
		arr = knl_oj_path(j, len, "tessronos", "devices");
		pos = arr;
		while ( arr >= 0 && ( e = knl_oj_next(j, len, arr, &pos) ) >= 0 ) {
			UB	name[OBD_NAME], us[TS_UUID_STRLEN + 1];
			TS_UUID	u;
			INT	i;

			if ( j[e] != '{'
			  || knl_oj_str(j, len, knl_oj_member(j, len, e, "name"), name, sizeof(name)) < 0
			  || knl_oj_str(j, len, knl_oj_member(j, len, e, "uuid"), us, sizeof(us)) < 0
			  || ts_str_to_uuid((CONST char *)us, &u) < E_OK ) {
				continue;
			}
			i = ent_by_name(name);
			if ( i < 0 ) {
				i = ent_new(name);
			}
			if ( i >= 0 && !OBD(i).fixed ) {
				OBD(i).uuid = u;
				OBD(i).kept = TRUE;
			}
			if ( i >= 0 ) {
				D	v = 0;

				if ( knl_oj_num(j, len, knl_oj_member(j, len, e, "irqcpu"), &v)
				  && v > 0 && v <= CNF_MAX_PRCID ) {
					OBD(i).irqcpu = (UH)v;
					(void)knl_int_dev_target(name, (ID)v);
				}
			}
		}
	}
	if ( j != NULL ) Kfree(j);
}

/* Whether metadata read from the store says nothing refers to the object */
LOCAL BOOL refs_none( CONST UB *j, INT len )
{
	LOCAL CONST char	key[] = "\"refCount\":0";
	INT			i, k;

	for ( i = 0; i + (INT)sizeof(key) - 1 <= len; i++ ) {
		for ( k = 0; key[k] != 0 && j[i + k] == (UB)key[k]; k++ ) ;
		if ( key[k] == 0 ) {
			return (BOOL)( i + k >= len || j[i + k] < '0' || j[i + k] > '9' );
		}
	}
	return FALSE;
}

/* The list of every device known, the ones there now first (knl_oj_make) */
LOCAL INT box_list( void *ctx, UB *t, INT max )
{
	INT	n = 0, i, k;
	BOOL	first = TRUE;

	(void)ctx;
	n = knl_oj_put(t, n, max, "[");
	for ( k = 0; k < 2; k++ ) {
		for ( i = 0; i < obd_hi && n >= 0; i++ ) {
			if ( !OBD(i).used || OBD(i).fixed || OBD(i).seen != ( k == 0 ) ) continue;
			n = knl_oj_put(t, n, max, first ? "{\"name\":" : ",{\"name\":");
			n = knl_oj_put_str(t, n, max, OBD(i).name);
			n = knl_oj_put(t, n, max, ",\"uuid\":");
			n = knl_oj_put_uuid(t, n, max, &OBD(i).uuid);
			if ( OBD(i).irqcpu != 0 ) {
				n = knl_oj_put(t, n, max, ",\"irqcpu\":");
				n = knl_oj_put_num(t, n, max, OBD(i).irqcpu);
			}
			n = knl_oj_put(t, n, max, "}");
			first = FALSE;
		}
	}
	return knl_oj_put(t, n, max, "]");
}

/* The box written again with every name and UUID known, however many */
LOCAL void box_save( ID vol )
{
	UB	*j = NULL, *t = NULL;
	INT	len, n, i, cap = 0;
	BOOL	fresh = FALSE;
	ER	er;
	CONST char *base = "{\"name\":\"デバイス箱\",\"refCount\":1,\"recordCount\":0,"
			   "\"editable\":true,\"deletable\":false,\"readable\":true}";

	n = knl_oj_make(box_list, NULL, &t);
	if ( n < 0 ) {
		return;
	}
	len = knl_tsfs_meta_all(vol, &ob_uuid_devbox, n + 64, &j, &cap);
	if ( len < 0 ) {
		/* a new box: referred to by the system, so never garbage */
		for ( len = 0; base[len] != 0; len++ ) ;
		cap = len + n + 64;
		j = (UB *)Kmalloc((SZ)cap + 1);
		if ( j == NULL ) {
			Kfree(t);
			return;
		}
		len = knl_oj_put(j, 0, cap, base);
		fresh = TRUE;
	}
	if ( len > 0 && n > 0 ) {
		len = knl_oj_set_path(j, len, cap, "tessronos", "devices", t, n);
		/* the native store makes no object by writing metadata: a new box is made under its UUID */
		er = ( len <= 0 ) ? E_PAR
		   : fresh ? knl_tsfs_cre_as(vol, &ob_uuid_devbox, j, len)
		   : knl_tsfs_set_meta(vol, &ob_uuid_devbox, j, len);
		if ( er >= E_OK ) {
			for ( i = 0; i < obd_hi; i++ ) {
				if ( OBD(i).used ) OBD(i).kept = TRUE;
			}
			/*
			 * The store keeps the count of references itself and takes
			 * nothing from the metadata's; no link reaches the box, so
			 * the system holds it once (a box made before it did is
			 * held when it is next written)
			 */
			if ( fresh || refs_none(j, len) ) {
				(void)ts_lnk_obj(vol, &ob_uuid_devbox);
			}
		}
	}
	if ( j != NULL ) Kfree(j);
	Kfree(t);
}

EXPORT void knl_obdev_volume_up( void )
{
	ID	vol = knl_obfile_first_vol();

	if ( vol <= 0 || obd_mtx <= 0 ) {
		return;
	}
	LOCK();
	if ( !obd_loaded ) {
		box_load(vol);
		obd_loaded = TRUE;
	}
	box_save(vol);
	UNLOCK();
}

/* ---------------------------------------------------------------- the devices there are */

/* The device management asked again: new devices get a UUID, gone ones are not seen */
LOCAL void refresh( void )
{
	T_LDEV	ld[8];
	INT	start = 0, i, k, s;
	BOOL	fresh = FALSE;
	ID	vol;

	for ( i = 0; i < obd_hi; i++ ) {
		if ( OBD(i).ops == NULL ) OBD(i).seen = FALSE;	/* one added by name stays */
	}
	for (;;) {
		INT	n = tk_lst_dev(ld, start, 8);
		INT	got = ( n > 8 ) ? 8 : n;

		if ( n <= 0 ) break;
		for ( k = 0; k < got; k++ ) {
			for ( s = -1; s < ld[k].nsub; s++ ) {
				UB	name[OBD_NAME];
				INT	m = 0, e;

				while ( ld[k].devnm[m] != 0 && m < L_DEVNM ) {
					name[m] = ld[k].devnm[m];
					m++;
				}
				if ( s >= 0 ) {		/* a subunit: the name and its number */
					if ( s >= 100 ) name[m++] = (UB)( '0' + s / 100 );
					if ( s >= 10 ) name[m++] = (UB)( '0' + s / 10 % 10 );
					name[m++] = (UB)( '0' + s % 10 );
				}
				name[m] = 0;
				e = ent_by_name(name);
				if ( e < 0 ) {
					e = ent_new(name);
					if ( e < 0 ) continue;
					if ( ts_gen_uuid(&OBD(e).uuid) < E_OK ) {
						OBD(e).used = FALSE;	/* no clock: no UUID yet */
						continue;
					}
					fresh = TRUE;
				}
				OBD(e).seen = TRUE;
			}
		}
		start += got;
		if ( n <= 8 ) break;
	}
	if ( fresh && obd_loaded && ( vol = knl_obfile_first_vol() ) > 0 ) {
		box_save(vol);
	}
}

/* ---------------------------------------------------------------- what drivers say */

/* Whether a name is the device's own or one of its units' ("uda", "uda0") */
LOCAL BOOL name_under( CONST UB *name, CONST UB *devnm )
{
	INT	i;

	for ( i = 0; devnm[i] != 0; i++ ) {
		if ( name[i] != devnm[i] ) return FALSE;
	}
	for ( ; name[i] != 0; i++ ) {
		if ( name[i] < '0' || name[i] > '9' ) return FALSE;
	}
	return TRUE;
}

/*
 * The medium or the device went: the mounts of it and of its units go
 * too (design 12.2.3), each told on its object as the change of its
 * attributes. Outside the lock: taking a mount down reaches the device.
 */
LOCAL void revoke( CONST UB *devnm )
{
	UB	name[OBD_NAME];
	TS_UUID	u;
	INT	i;
	BOOL	hit;

	for ( i = 0; i < obd_hi; i++ ) {
		LOCK();
		hit = ( OBD(i).used && name_under(OBD(i).name, devnm) );
		if ( hit ) {
			knl_memcpy(name, OBD(i).name, OBD_NAME);
			u = OBD(i).uuid;
		}
		UNLOCK();
		if ( hit && knl_fs_revoke((CONST char *)name) > 0 ) {
			dev_post(&u, OB_E_CHANGE);
		}
		if ( hit ) {
			(void)knl_obfile_revoke(name);	/* the volumes of objects on it */
		}
	}
}

EXPORT void knl_obdev_media( CONST UB *devnm, BOOL in )
{
	TS_UUID	u;
	INT	e;

	if ( obd_mtx <= 0 || devnm == NULL ) {
		return;
	}
	LOCK();
	e = ent_by_name(devnm);
	if ( e < 0 ) {
		refresh();			/* one this manager has not met yet */
		e = ent_by_name(devnm);
	}
	if ( e >= 0 ) {
		u = OBD(e).uuid;
	}
	obd_serial++;
	UNLOCK();
	if ( e >= 0 ) {
		dev_post(&u, in ? OB_E_ATTACH : OB_E_DETACH);
	}
	if ( !in ) {
		revoke(devnm);
	}
}

EXPORT void knl_obdev_changed( void )
{
	typedef struct {
		TS_UUID	u;
		UINT	ev;
		INT	ent;
	} CHG;
	BOOL	*was;
	CHG	*c;
	INT	i, k, n = 0;

	if ( obd_mtx <= 0 ) {
		return;
	}
	LOCK();
	/* as many as there are before, and after (refresh makes new entries) */
	was = (BOOL *)Kmalloc(sizeof(BOOL) * (SZ)( obd_hi + 1 ));
	if ( was == NULL ) {
		UNLOCK();
		return;
	}
	k = obd_hi;
	for ( i = 0; i < obd_hi; i++ ) {
		was[i] = ( OBD(i).used && OBD(i).seen );
	}
	refresh();
	c = (CHG *)Kmalloc(sizeof(CHG) * (SZ)( obd_hi + 1 ));
	if ( c == NULL ) {
		UNLOCK();
		Kfree(was);
		return;
	}
	for ( i = 0; i < obd_hi; i++ ) {
		BOOL	now = ( OBD(i).used && OBD(i).seen );

		if ( now != ( i < k && was[i] ) ) {
			c[n].u = OBD(i).uuid;
			c[n].ev = now ? OB_E_ATTACH : OB_E_DETACH;
			c[n].ent = i;
			n++;
		}
	}
	obd_serial++;
	UNLOCK();
	for ( i = 0; i < n; i++ ) {
		dev_post(&c[i].u, c[i].ev);
		if ( c[i].ev == OB_E_DETACH ) {
			revoke(OBD(c[i].ent).name);	/* an entry keeps its name */
		}
	}
	Kfree(was);
	Kfree(c);
}

/* ---------------------------------------------------------------- the volumes kept attached */

/*
 * The volumes of objects to be attached again at the next start, by
 * their own UUIDs with their top objects, kept in the device box's
 * metadata (tessronos.volumes): [{"volume": UUID, "top": UUID}, ...],
 * as many as there are. vols_read takes the metadata whole into *p_j
 * (with room bytes more, *p_cap; Kfree it) and every volume into lists
 * of its own (*p_vu and *p_top, one more place than there are; Kfree
 * *p_vu, *p_top is in it). How many, or an error.
 */
LOCAL INT vols_read( ID vol, INT room, UB **p_j, INT *p_len, INT *p_cap, TS_UUID **p_vu, TS_UUID **p_top )
{
	UB	*j = NULL;
	TS_UUID	*vu, *top;
	INT	len, arr, pos, e, n = 0, cnt = 0;

	*p_j = NULL;
	*p_vu = *p_top = NULL;
	len = knl_tsfs_meta_all(vol, &ob_uuid_devbox, room, &j, p_cap);
	if ( len < 0 ) {
		return len;
	}
	arr = knl_oj_path(j, len, "tessronos", "volumes");
	pos = arr;
	while ( arr >= 0 && ( e = knl_oj_next(j, len, arr, &pos) ) >= 0 ) {
		cnt++;
	}
	vu = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)( cnt + 1 ) * 2);
	if ( vu == NULL ) {
		Kfree(j);
		return E_NOMEM;
	}
	top = vu + cnt + 1;
	pos = arr;
	while ( arr >= 0 && n < cnt && ( e = knl_oj_next(j, len, arr, &pos) ) >= 0 ) {
		UB	a[TS_UUID_STRLEN + 1], b[TS_UUID_STRLEN + 1];

		if ( j[e] != '{'
		  || knl_oj_str(j, len, knl_oj_member(j, len, e, "volume"), a, sizeof(a)) < 0
		  || ts_str_to_uuid((CONST char *)a, &vu[n]) < E_OK ) {
			continue;
		}
		if ( knl_oj_str(j, len, knl_oj_member(j, len, e, "top"), b, sizeof(b)) < 0
		  || ts_str_to_uuid((CONST char *)b, &top[n]) < E_OK ) {
			knl_memset(&top[n], 0, sizeof(top[n]));
		}
		n++;
	}
	*p_j = j;
	*p_len = len;
	*p_vu = vu;
	*p_top = top;
	return n;
}

EXPORT INT knl_obdev_vols_get( TS_UUID *vu, TS_UUID *top, INT max )
{
	UB	*j;
	TS_UUID	*av, *at;
	INT	len = 0, n, i;
	ID	vol = knl_obfile_first_vol();

	if ( vol <= 0 || obd_mtx <= 0 ) {
		return 0;
	}
	LOCK();
	n = vols_read(vol, 0, &j, &len, NULL, &av, &at);
	UNLOCK();
	if ( n < 0 ) {
		return 0;
	}
	for ( i = 0; i < n && i < max && vu != NULL && top != NULL; i++ ) {
		vu[i] = av[i];
		top[i] = at[i];
	}
	Kfree(j);
	Kfree(av);
	return n;
}

/* The list of volumes written as the device box keeps it (knl_oj_make) */
typedef struct {
	CONST TS_UUID	*vu, *top;
	INT		n;
} VOLLIST;

LOCAL INT vols_text( void *ctx, UB *t, INT max )
{
	VOLLIST	*l = (VOLLIST *)ctx;
	INT	m, i;

	m = knl_oj_put(t, 0, max, "[");
	for ( i = 0; i < l->n; i++ ) {
		m = knl_oj_put(t, m, max, ( i > 0 ) ? ",{\"volume\":" : "{\"volume\":");
		m = knl_oj_put_uuid(t, m, max, &l->vu[i]);
		m = knl_oj_put(t, m, max, ",\"top\":");
		m = knl_oj_put_uuid(t, m, max, &l->top[i]);
		m = knl_oj_put(t, m, max, "}");
	}
	return knl_oj_put(t, m, max, "]");
}

EXPORT ER knl_obdev_vols_put( CONST TS_UUID *vuuid, CONST TS_UUID *top, BOOL keep )
{
	TS_UUID	*vu = NULL, *tp = NULL;
	UB	*j = NULL, *t = NULL;
	INT	len = 0, n, i, k = 0, cap = 0;
	ID	vol = knl_obfile_first_vol();
	ER	er = E_OK;
	VOLLIST	l;

	if ( vol <= 0 || obd_mtx <= 0 || vuuid == NULL ) {
		return E_NOEXS;
	}
	LOCK();
	if ( !obd_loaded ) {
		box_load(vol);
		obd_loaded = TRUE;
	}
	box_save(vol);				/* the box is there to keep it in */
	/* every one kept, and room for this one more, and for the list written again */
	n = vols_read(vol, 0, &j, &len, &cap, &vu, &tp);
	if ( n < 0 ) {
		UNLOCK();
		return ( n == E_NOMEM ) ? E_NOMEM : E_NOEXS;
	}
	for ( i = 0; i < n; i++ ) {
		if ( ts_uuid_cmp(&vu[i], vuuid) != 0 ) {
			vu[k] = vu[i];
			tp[k] = tp[i];
			k++;
		}
	}
	if ( keep ) {
		vu[k] = *vuuid;
		if ( top != NULL ) tp[k] = *top;
		else knl_memset(&tp[k], 0, sizeof(tp[k]));
		k++;
	}
	if ( len <= 0 ) {
		er = E_NOEXS;
	} else {
		INT	m;

		l.vu = vu;
		l.top = tp;
		l.n = k;
		m = knl_oj_make(vols_text, &l, &t);
		if ( m > 0 && cap < len + m + 64 ) {
			UB	*nj = (UB *)Kmalloc((SZ)( len + m + 64 ) + 1);

			if ( nj != NULL ) {
				knl_memcpy(nj, j, len);
				Kfree(j);
				j = nj;
				cap = len + m + 64;
			} else {
				m = -1;
			}
		}
		len = ( m > 0 ) ? knl_oj_set_path(j, len, cap, "tessronos", "volumes", t, m) : -1;
		er = ( len <= 0 ) ? E_NOMEM : knl_tsfs_set_meta(vol, &ob_uuid_devbox, j, len);
	}
	UNLOCK();
	if ( j != NULL ) Kfree(j);
	if ( t != NULL ) Kfree(t);
	if ( vu != NULL ) Kfree(vu);
	return er;
}

EXPORT UW knl_obdev_serial( void )
{
	return __atomic_load_n(&obd_serial, __ATOMIC_ACQUIRE);
}

/* The names of the disks and partitions there are now, OBD_NAME bytes each */
EXPORT INT knl_obdev_disks( UB *buf, INT max )
{
	T_RDEV	rd;
	UB	name[OBD_NAME];
	INT	i, n = 0;

	if ( obd_mtx <= 0 ) {
		return 0;
	}
	/* buf NULL: how many there are, room for all of them */
	for ( i = 0; i < obd_hi && ( buf == NULL || n < max ); i++ ) {
		BOOL	take;

		LOCK();
		if ( i == 0 ) refresh();
		take = (BOOL)( OBD(i).used && OBD(i).seen && OBD(i).ops == NULL );
		if ( take ) knl_memcpy(name, OBD(i).name, OBD_NAME);
		UNLOCK();
		if ( take && tk_ref_dev(name, &rd) >= E_OK && rd.blksz > 1 && !dev_is_sound(name) ) {
			if ( buf != NULL ) knl_memcpy(buf + n * OBD_NAME, name, OBD_NAME);
			n++;
		}
	}
	return n;
}

EXPORT ER knl_obdev_uuid( CONST UB *devnm, TS_UUID *p_uuid )
{
	INT	e;

	if ( obd_mtx <= 0 || devnm == NULL || p_uuid == NULL ) {
		return E_NOEXS;
	}
	LOCK();
	e = ent_by_name(devnm);
	if ( e < 0 ) {
		refresh();
		e = ent_by_name(devnm);
	}
	if ( e >= 0 ) {
		*p_uuid = OBD(e).uuid;
	}
	UNLOCK();

	return ( e >= 0 ) ? E_OK : E_NOEXS;
}

/* ---------------------------------------------------------------- devices added by name */

/* A UUID without the clock (version 4): the device box keeps it all the same */
LOCAL ER random_uuid( TS_UUID *u )
{
	ER	er = ts_get_random(u->b, sizeof(u->b));

	u->b[6] = (UB)( 0x40 | ( u->b[6] & 0x0F ) );
	u->b[8] = (UB)( 0x80 | ( u->b[8] & 0x3F ) );
	return er;
}

EXPORT ER knl_obdev_add( CONST UB *name, CONST TS_UUID *fixed, CONST T_OBDVOPS *ops, void *ctx,
			 TS_UUID *p_uuid )
{
	TS_UUID	u;
	BOOL	fresh = FALSE, was;
	INT	e;
	ID	vol;

	if ( obd_mtx <= 0 || name == NULL || name[0] == 0 || ops == NULL ) {
		return E_PAR;
	}
	LOCK();
	e = ent_by_name(name);
	if ( e >= 0 && OBD(e).ops == NULL && OBD(e).seen ) {
		UNLOCK();
		return E_OBJ;			/* a registered device has the name */
	}
	if ( e < 0 ) {
		e = ent_new(name);
		if ( e < 0 ) {
			UNLOCK();
			return E_LIMIT;
		}
		fresh = TRUE;
	}
	if ( fixed != NULL ) {
		OBD(e).uuid = *fixed;
		OBD(e).fixed = TRUE;
	} else if ( fresh && ts_gen_uuid(&OBD(e).uuid) < E_OK && random_uuid(&OBD(e).uuid) < E_OK ) {
		OBD(e).used = FALSE;
		UNLOCK();
		return E_OBJ;
	}
	was = OBD(e).seen;
	OBD(e).ops = ops;
	OBD(e).ctx = ctx;
	OBD(e).seen = TRUE;
	u = OBD(e).uuid;
	if ( fresh && fixed == NULL && obd_loaded && ( vol = knl_obfile_first_vol() ) > 0 ) {
		box_save(vol);
	}
	UNLOCK();
	if ( p_uuid != NULL ) *p_uuid = u;
	if ( !was ) {
		dev_post(&u, OB_E_ATTACH);
	}
	return E_OK;
}

EXPORT ER knl_obdev_del( CONST UB *name )
{
	TS_UUID	u;
	BOOL	was = FALSE;
	INT	e;

	if ( obd_mtx <= 0 || name == NULL ) {
		return E_PAR;
	}
	LOCK();
	e = ent_by_name(name);
	if ( e >= 0 && OBD(e).ops != NULL ) {
		was = OBD(e).seen;
		OBD(e).seen = FALSE;		/* the entry and its UUID stay for its return */
		u = OBD(e).uuid;
	}
	UNLOCK();
	if ( e < 0 ) {
		return E_NOEXS;
	}
	if ( was ) {
		dev_post(&u, OB_E_DETACH);
	}
	return E_OK;
}

EXPORT void knl_obdev_post( CONST TS_UUID *uuid, INT recno, UINT event, CONST T_OBNTM *n )
{
	knl_ob_post(uuid, recno, event, n);
	knl_ob_post_as(&ob_uuid_devlist, uuid, recno, event, n);
}

/* ---------------------------------------------------------------- edges told by a task */

/*
 * The edges whose notices are posted here (knl_ob_edge_add), by a task
 * of their own woken through an event flag, which an interrupt handler
 * may set: what comes in is told as soon as it comes, whether or not
 * anything else happens. The flag is set wherever the kick is made --
 * in an interrupt handler, with dispatching disabled, inside the big
 * kernel lock, which the processor that holds it takes again -- except
 * where the processor holds one of the kernel's finer locks
 * (CNF_KLOCK_MODE 1 and above): setting the flag takes the flag's lock
 * and the scheduler's, which may not come under a lock of the same or a
 * later rank (klk.h). There the wake-up is owed, and given by the
 * processor as it lets go of the last lock it holds (knl_klk_kick_owed).
 */
#define OBE_MAX		16
#define OBE_PRI		8		/* above the programs: told as it comes */
#define OBE_LOOK_MS	20		/* a device looked at again, while a reader waits on it */

typedef struct {
	T_OBEDGE	*e;
	TS_UUID		uuid;		/* the object, unless by name */
	UB		devnm[L_DEVNM + 1];	/* the device, "" for the object */
	INT		recno;
	BOOL		(*look)( void );
} OBEDGE_ENT;

LOCAL OBEDGE_ENT	obe[OBE_MAX];
LOCAL volatile INT	obe_n = 0;
LOCAL T_SPLOCK		obe_lk;
LOCAL ID		obe_flg = 0;

EXPORT ER knl_ob_edge_add( T_OBEDGE *e, CONST TS_UUID *uuid, CONST UB *devnm, INT recno,
			   BOOL (*look)( void ) )
{
	UINT	s;
	INT	k;
	ER	er = E_LIMIT;

	if ( e == NULL || ( uuid == NULL && devnm == NULL ) ) {
		return E_PAR;
	}
	s = disint();
	SpinLock(&obe_lk);
	if ( obe_n < OBE_MAX ) {
		OBEDGE_ENT	*o = &obe[obe_n];

		knl_memset(o, 0, sizeof(*o));
		o->e = e;
		if ( uuid != NULL ) {
			o->uuid = *uuid;
		}
		for ( k = 0; devnm != NULL && devnm[k] != 0 && k < L_DEVNM; k++ ) {
			o->devnm[k] = devnm[k];
		}
		o->recno = recno;
		o->look = look;
		__atomic_store_n(&obe_n, obe_n + 1, __ATOMIC_RELEASE);
		er = E_OK;
	}
	SpinUnlock(&obe_lk);
	enaint(s);
	return er;
}

/* The task that tells woken (with no lock of the kernel held) */
LOCAL void obe_wake( void )
{
	if ( obe_flg > 0 ) {
		(void)tk_set_flg(obe_flg, 1);
	}
}

EXPORT void knl_ob_edge_kick( T_OBEDGE *e )
{
#if KLK_FINE
	UINT	s = disint();
	BOOL	held = (BOOL)( knl_klk_nheld() > 0 );

	if ( held ) {
		/* owed, and given as this processor lets go of its last lock */
		knl_klk_kick_hook = obe_wake;
		__atomic_store_n(&knl_klk_kick_owed, 1, __ATOMIC_SEQ_CST);
	}
	enaint(s);
	if ( held ) {
		return;
	}
#endif
	(void)e;
	obe_wake();
}

LOCAL void obe_task( INT stacd, void *exinf )
{
	OBEDGE_ENT	o;
	UINT		ptn;
	TS_UUID		u;
	TMO		tmo;
	INT		i, n;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		tmo = TMO_FEVR;
		n = __atomic_load_n(&obe_n, __ATOMIC_ACQUIRE);
		for ( i = 0; i < n; i++ ) {
			if ( obe[i].look != NULL && (*obe[i].look)() ) {
				tmo = OBE_LOOK_MS;
			}
		}
		for ( i = 0; i < n; i++ ) {
			if ( !knl_obedge_take(obe[i].e) ) {
				continue;
			}
			o = obe[i];
			if ( o.devnm[0] != 0 ) {
				if ( knl_obdev_uuid(o.devnm, &u) >= E_OK ) {
					knl_obdev_post(&u, o.recno, OB_E_CHANGE, NULL);
				}
			} else {
				knl_ob_post(&o.uuid, o.recno, OB_E_CHANGE, NULL);
			}
		}
		(void)tk_wai_flg(obe_flg, 1, TWF_ORW | TWF_BITCLR, &ptn, tmo);
	}
}

LOCAL void obe_start( void )
{
	T_CFLG	cf;
	T_CTSK	ct;
	ID	tid;

	cf.exinf = NULL;
	cf.flgatr = TA_TFIFO | TA_WMUL;
	cf.iflgptn = 0;
	obe_flg = tk_cre_flg(&cf);
	if ( obe_flg <= 0 ) {
		obe_flg = 0;
		return;
	}
	ct.exinf = NULL;
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)obe_task;
	ct.itskpri = OBE_PRI;
	ct.stksz = 8 * 1024;
	tid = tk_cre_tsk(&ct);
	if ( tid <= 0 || tk_sta_tsk(tid, 0) < E_OK ) {
		tm_printf((UB *)"TessronOS: no task to tell what comes in (%d)\n", tid);
	}
	/* the console's log, told on the system object (obsys.c) */
	(void)knl_ob_edge_add(&knl_tm_log_edge, &ob_uuid_system, NULL, OB_SYS_LOG, NULL);
}

EXPORT INT knl_obdev_link( UB *t, INT n, INT max, CONST TS_UUID *u, CONST UB *name )
{
	char	us[TS_UUID_STRLEN + 1];

	(void)ts_uuid_to_str(u, us, sizeof(us));
	n = knl_oj_put(t, n, max, "<p><link id=\"");
	n = knl_oj_put(t, n, max, us);
	n = knl_oj_put(t, n, max, "_0.xtad\"/>");
	n = knl_oj_put(t, n, max, (CONST char *)name);
	return knl_oj_put(t, n, max, "</p>");
}

/* The entry of a key's device added by name, while it is there (under the lock) */
LOCAL BOOL added_there( OBDHND *o )
{
	return (BOOL)( o->vops != NULL && o->ent >= 0 && OBD(o->ent).seen && OBD(o->ent).ops == o->vops );
}

/* ---------------------------------------------------------------- the manager */

LOCAL ER obd_find( CONST TS_UUID *uuid )
{
	INT	i;

	if ( is_clock(uuid) || is_list(uuid) || is_system(uuid) ) {
		return E_OK;
	}
	LOCK();
	refresh();
	i = ent_by_uuid(uuid);
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obd_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	T_RDEV	rd;
	INT	i;

	if ( is_clock(uuid) ) {
		(void)knl_oj_put(r->name, 0, OB_NAME_MAX, "clock");
		r->sub = OB_S_CLOCK;
		r->flags = OB_F_VIRTUAL;
		r->nrec = 2;
		return E_OK;
	}
	if ( is_list(uuid) ) {
		(void)knl_oj_put(r->name, 0, OB_NAME_MAX, "devices");
		r->sub = OB_S_DEVLIST;
		r->flags = OB_F_VIRTUAL;
		r->nrec = 1;
		return E_OK;
	}
	if ( is_system(uuid) ) {
		(void)knl_oj_put(r->name, 0, OB_NAME_MAX, "system");
		r->sub = OB_S_SYSTEM;
		r->flags = OB_F_VIRTUAL;
		r->nrec = OB_SYS_NREC;
		return E_OK;
	}
	LOCK();
	i = ent_by_uuid(uuid);
	if ( i >= 0 && OBD(i).ops != NULL ) {
		knl_memcpy(r->name, OBD(i).name, OBD_NAME);
		r->sub = OBD(i).ops->sub;
		r->flags = OB_F_VIRTUAL;
		r->nrec = OBD(i).ops->nrec;
	} else if ( i >= 0 ) {
		knl_memcpy(r->name, OBD(i).name, OBD_NAME);
		if ( dev_is_sound(OBD(i).name) ) {
			r->sub = OB_S_SOUND;
			r->nrec = OB_SND_NREC;
		} else if ( dev_is_camera(OBD(i).name) ) {
			r->sub = OB_S_CAMERA;
			r->nrec = OB_CAM_NREC;
		} else {
			r->sub = ( tk_ref_dev(OBD(i).name, &rd) >= E_OK && rd.blksz > 1 )
			       ? OB_S_DISK : OB_S_CHAR;
			r->nrec = 2;
		}
	}
	UNLOCK();

	return ( i >= 0 ) ? E_OK : E_NOEXS;
}

/*
 * Devices belong to the system. A disk's blocks are the administrators'
 * alone (rw-r----- with their group); the clock is read by everyone;
 * sound is everyone's to play and record, each opening a channel of
 * its own. The system object is written by the administrators and by
 * the ordinary users' group (its records of the colour scheme, the
 * wallpaper and the power are the only ones that take writing), and so
 * is a medium of the person's (medium_free).
 */

/* Whether name begins with the base, the disk's name without its partition's number */
LOCAL BOOL on_disk( CONST UB *name, CONST UB *base, INT nb )
{
	INT	k;

	for ( k = 0; k < nb; k++ ) {
		if ( name[k] != base[k] ) return FALSE;
	}
	return TRUE;
}

/*
 * A medium the person at the machine may read and write as a whole: a
 * disk of the USB mass storage ("uda", "uda0", a CD-ROM as well), of
 * which the system holds nothing -- no native volume attached (its
 * objects keep their own protection, which the blocks would get round)
 * and nothing mounted but where a program mounts a medium, under
 * FS_MEDIA_DIR. A disk the machine started from is never one.
 */
LOCAL BOOL medium_free( CONST UB *name )
{
	T_FSMNT	*mt;
	UB	*vp;
	INT	nb = 0, n, i, k;
	BOOL	free = TRUE;
	CONST INT stride = 32;

	if ( name[0] != 'u' || name[1] != 'd' ) {
		return FALSE;
	}
	while ( nb < OBD_NAME && name[nb] != 0 && ( name[nb] < '0' || name[nb] > '9' ) ) nb++;

	n = fs_mounts(NULL, 0);
	if ( n > 0 ) {
		mt = (T_FSMNT *)Kmalloc(sizeof(T_FSMNT) * (SZ)( n + 4 ));
		if ( mt == NULL ) {
			return FALSE;
		}
		n = fs_mounts(mt, n + 4);
		for ( i = 0; i < n && free; i++ ) {
			if ( mt[i].dev[0] != 0 && on_disk(mt[i].dev, name, nb)
			  && !on_disk(mt[i].path, (CONST UB *)FS_MEDIA_DIR "/", (INT)sizeof(FS_MEDIA_DIR)) ) {
				free = FALSE;
			}
		}
		Kfree(mt);
	}
	/* every volume attached: room for more than there are now, so none is left out */
	k = knl_obfile_nvol() + 8;
	vp = (UB *)Kmalloc(stride * k);
	if ( vp == NULL ) {
		return FALSE;
	}
	n = knl_obfile_vols(vp, stride, k);
	if ( n >= k ) {
		free = FALSE;		/* more came meanwhile: taken as in use rather than looked past */
	}
	for ( i = 0; i < n && free; i++ ) {
		if ( on_disk(vp + i * stride, name, nb) ) {
			free = FALSE;
		}
	}
	Kfree(vp);
	return free;
}

LOCAL ER obd_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	CONST T_OBDVOPS	*ops = NULL;
	INT	i;
	BOOL	open = FALSE, medium = FALSE;
	UB	name[OBD_NAME];
	T_OBPRT	*prt;

	name[0] = 0;
	if ( !is_clock(uuid) && !is_list(uuid) && !is_system(uuid) ) {
		LOCK();
		i = ent_by_uuid(uuid);
		if ( i >= 0 ) {
			ops = OBD(i).ops;
			knl_memcpy(name, OBD(i).name, OBD_NAME);
		}
		UNLOCK();
		if ( ops != NULL ) {
			/* added by name: the mode it gives, and the record only x reaches */
			prt = knl_ob_prt_new(0, ( ops->xrec > 0 ) ? 1 : 0);
			if ( prt == NULL ) {
				return E_NOMEM;
			}
			prt->owner = ob_user_system;
			prt->group = ob_group_admin;
			prt->mode = ops->mode;
			prt->attr = OB_A_PERM;
			if ( ops->xrec > 0 ) {
				OB_PRT_RMASK(prt)[0].recno = ops->xrec;
				OB_PRT_RMASK(prt)[0].ops = ops->xops | OB_OP_EXEC;
			}
			*p_prt = prt;
			return E_OK;
		}
		/* what a program plays sound, talks on a line or takes pictures with is everyone's */
		open = ( i >= 0 && ( dev_is_sound(name) || dev_is_serial(name) || dev_is_camera(name) ) );
		/* a medium of the person's: read and written by the ordinary users too */
		medium = ( i >= 0 && !open && medium_free(name) );
	}
	prt = knl_ob_prt_new(1, 0);
	if ( prt == NULL ) {
		return E_NOMEM;
	}
	prt->owner = ob_user_system;
	prt->group = ob_group_admin;
	prt->mode = is_list(uuid) ? 0444 : is_clock(uuid) ? 0644 : is_system(uuid) ? 0664 : open ? 0666 : 0640;
	prt->attr = OB_A_PERM;
	*p_prt = prt;
	if ( is_system(uuid) ) {
		/* the colour scheme, the wallpaper and the power are the person's at the machine to set */
		prt->acl[0].kind = OB_ACE_GROUP;
		prt->acl[0].who = ob_group_users;
		prt->acl[0].ops = OB_OP_WRITE;
	} else {
		/*
		 * What a device is -- its size, where it is mounted -- is the
		 * ordinary users' to look at; a medium of the person's is
		 * theirs to read and write as well
		 */
		prt->acl[0].kind = OB_ACE_GROUP;
		prt->acl[0].who = ob_group_users;
		prt->acl[0].ops = medium ? ( OB_OP_R | OB_OP_WRITE ) : OB_OP_ATRRD;
	}

	return E_OK;
}

LOCAL INT obd_open( CONST TS_UUID *uuid, UINT ops )
{
	INT	e = ENT_CLOCK, i;
	ID	dd = 0;
	T_RDEV	rd;
	W	blksz = 1;
	BOOL	sound = FALSE, serial = FALSE, camera = FALSE;
	CONST T_OBDVOPS	*vops = NULL;
	void	*vctx = NULL;
	UD	pos = 0;

	if ( is_list(uuid) ) {
		e = ENT_LIST;
	} else if ( is_system(uuid) ) {
		e = ENT_SYSTEM;
	} else if ( !is_clock(uuid) ) {
		LOCK();
		e = ent_by_uuid(uuid);
		vops = ( e >= 0 ) ? OBD(e).ops : NULL;
		vctx = ( e >= 0 ) ? OBD(e).ctx : NULL;
		UNLOCK();
		if ( e < 0 ) {
			return E_NOEXS;
		}
		if ( vops != NULL ) {
			/* added by name: nothing of the device management to open */
		} else if ( ( ops & ( OB_OP_READ | OB_OP_WRITE ) ) != 0 ) {
			dd = tk_opn_dev(OBD(e).name, ( ops & OB_OP_WRITE ) ? TD_UPDATE : TD_READ);
			if ( dd < E_OK ) {
				return (INT)dd;
			}
		}
		if ( vops == NULL && tk_ref_dev(OBD(e).name, &rd) >= E_OK && rd.blksz > 0 ) {
			blksz = rd.blksz;
			sound = ( TDK_KIND(rd.devatr) == TDK_SOUND_KIND );
			serial = ( TDK_KIND(rd.devatr) == TDK_SERIAL );
			camera = ( TDK_KIND(rd.devatr) == TDK_CAMERA_KIND );
		}
	}
	if ( vops != NULL && vops->opened != NULL ) {
		vops->opened(vctx, &pos);
	}
	LOCK();
	for ( i = 0; ; i++ ) {
		if ( i == OBDH_N ) {
			OBDHTBL	*t = (OBDHTBL *)tbl_grow(obd_htbl, OBD_HND_FIRST, sizeof(OBDHND));

			if ( t == NULL ) {
				break;
			}
			obd_htbl = t;		/* the table before is not freed */
		}
		if ( !OBDH(i).used ) {
			OBDH(i).vops = vops;
			OBDH(i).vctx = vctx;
			OBDH(i).pos = pos;
			OBDH(i).used = TRUE;
			OBDH(i).ent = e;
			OBDH(i).dd = dd;
			OBDH(i).blksz = blksz;
			OBDH(i).sound = sound;
			OBDH(i).serial = serial;
			OBDH(i).camera = camera;
			UNLOCK();
			return i + 1;
		}
	}
	UNLOCK();
	if ( dd > 0 ) tk_cls_dev(dd, 0);

	return E_NOMEM;
}

LOCAL OBDHND *hnd_of( INT h )
{
	return ( h >= 1 && h <= OBDH_N && OBDH(h - 1).used ) ? &OBDH(h - 1) : NULL;
}

LOCAL ER obd_close( INT h )
{
	OBDHND	*o = hnd_of(h);

	if ( o == NULL ) {
		return E_ID;
	}
	if ( o->dd > 0 ) tk_cls_dev(o->dd, 0);
	o->used = FALSE;

	return E_OK;
}

/* Record 0: what the device is, as xmlTAD */
LOCAL INT describe( OBDHND *o, UB *t, INT max )
{
	CONST UB	*name = ( o->ent == ENT_SYSTEM ) ? (CONST UB *)"system"
			      : ( o->ent < 0 ) ? (CONST UB *)"clock" : OBD(o->ent).name;
	INT		n;

	n = knl_oj_put(t, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	n = knl_oj_put(t, n, max, (CONST char *)name);
	n = knl_oj_put(t, n, max, "\"><document><p>");
	n = knl_oj_put(t, n, max, (CONST char *)name);
	if ( o->ent == ENT_SYSTEM ) {
		n = knl_oj_put(t, n, max, ": the system (virtual). Records 1 to 9 are key and value "
				   "lines: the system, power (write OFF or RESTART), the screen (画面), the USB "
				   "devices, the colour scheme (write its number), the wallpaper (write its "
				   "object), the network, sound (write SELECT and an output's id), the hardware's state");
	} else if ( o->ent < 0 ) {
		n = knl_oj_put(t, n, max, ": clock (virtual)");
	} else if ( o->sound ) {
		n = knl_oj_put(t, n, max, ": sound. Record 1 is the PCM (written, it plays; read, "
				   "it gives what was recorded); records 2 on are its attributes");
	} else if ( o->camera ) {
		n = knl_oj_put(t, n, max, ": camera. Record 1 is its frames (read, the newest one "
				   "not had yet); record 2 what it is, 3 its modes, 4 the mode picked (written, "
				   "it is picked), 5 the stream (write 1 to start, 0 to stop), 6 its counts");
	} else if ( o->serial ) {
		n = knl_oj_put(t, n, max, ": serial port. Record 1 is the data (read, what has "
				   "come in; written, it is sent); record 2 its speed, record 3 what it is");
	} else if ( o->blksz > 1 ) {
		n = knl_oj_put(t, n, max, ": disk, block ");
		n = knl_oj_put_num(t, n, max, o->blksz);
		n = knl_oj_put(t, n, max, " bytes");
	} else {
		n = knl_oj_put(t, n, max, ": stream");
	}
	return knl_oj_put(t, n, max, "</p></document></tad>");
}

/* The list's record 0: a link to each device there is */
LOCAL INT list_text( UB *t, INT max )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n, i;

	n = knl_oj_put(t, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"devices\">"
			   "<document><p>devices</p>");
	LOCK();
	refresh();
	for ( i = 0; i < obd_hi; i++ ) {
		if ( !OBD(i).used || !OBD(i).seen ) continue;
		(void)ts_uuid_to_str(&OBD(i).uuid, us, sizeof(us));
		n = knl_oj_put(t, n, max, "<p><link id=\"");
		n = knl_oj_put(t, n, max, us);
		n = knl_oj_put(t, n, max, "_0.xtad\"/>");
		n = knl_oj_put(t, n, max, (CONST char *)OBD(i).name);
		n = knl_oj_put(t, n, max, "</p>");
	}
	UNLOCK();
	return knl_oj_put(t, n, max, "</document></tad>");
}

/* The devices there are now */
LOCAL INT list_count( void )
{
	INT	i, n = 0;

	LOCK();
	refresh();
	for ( i = 0; i < obd_hi; i++ ) {
		if ( OBD(i).used && OBD(i).seen ) n++;
	}
	UNLOCK();
	return n;
}

LOCAL INT two( UB *t, INT n, INT max, INT v )
{
	if ( v < 10 ) n = knl_oj_put(t, n, max, "0");
	return knl_oj_put_num(t, n, max, v);
}

/* The clock's record 1: the local time, ISO 8601 with the offset */
LOCAL INT clock_text( UB *t, INT max )
{
	TS_TIME	now;
	TS_TM	tm;
	INT	tz = 0, n, a;

	if ( dt_gettime(&now) < E_OK || dt_localtime(&now, &tm) < E_OK ) {
		return E_SYS;
	}
	(void)dt_getsystz(&tz);
	if ( tm.tm_isdst ) tz += 60;		/* summer time: an hour more */
	n = knl_oj_put_num(t, 0, max, tm.tm_year + 1900);
	n = knl_oj_put(t, n, max, "-");
	n = two(t, n, max, tm.tm_mon + 1);
	n = knl_oj_put(t, n, max, "-");
	n = two(t, n, max, tm.tm_mday);
	n = knl_oj_put(t, n, max, "T");
	n = two(t, n, max, tm.tm_hour);
	n = knl_oj_put(t, n, max, ":");
	n = two(t, n, max, tm.tm_min);
	n = knl_oj_put(t, n, max, ":");
	n = two(t, n, max, tm.tm_sec);
	a = ( tz < 0 ) ? -tz : tz;
	n = knl_oj_put(t, n, max, ( tz < 0 ) ? "-" : "+");
	n = two(t, n, max, a / 60);
	n = knl_oj_put(t, n, max, ":");
	return two(t, n, max, a % 60);
}

/* Numbers of an ISO 8601 time: "2026-09-25T11:17:00" (an offset is ignored) */
LOCAL ER clock_set( CONST UB *s, SZ len )
{
	INT	v[6], k = 0, p = 0;
	TS_TM	tm;
	TS_TIME	t;

	while ( k < 6 && p < len ) {
		INT	x = 0;
		BOOL	any = FALSE;

		while ( p < len && s[p] >= '0' && s[p] <= '9' ) {
			x = x * 10 + ( s[p++] - '0' );
			any = TRUE;
		}
		if ( !any ) return E_PAR;
		v[k++] = x;
		if ( p < len ) p++;		/* the - : or T between */
	}
	if ( k < 6 ) {
		return E_PAR;
	}
	knl_memset(&tm, 0, sizeof(tm));
	tm.tm_year = v[0] - 1900;
	tm.tm_mon = v[1] - 1;
	tm.tm_mday = v[2];
	tm.tm_hour = v[3];
	tm.tm_min = v[4];
	tm.tm_sec = v[5];
	if ( dt_mktime_local(&tm, &t) < E_OK ) {
		return E_PAR;
	}
	return knl_dt_set_clock(t);		/* the key said it may; the write is told */
}

/* ---------------------------------------------------------------- a device added by name */

/* What it is, its ops, and whether it is still there */
LOCAL BOOL vdev_take( OBDHND *o, UB *name, CONST T_OBDVOPS **p_ops, void **p_ctx )
{
	BOOL	there;

	if ( name == NULL ) {
		/* a record read or written: whether it is still there is enough, and it is a word */
		there = added_there(o);
	} else {
		LOCK();
		there = added_there(o);
		if ( there ) {
			knl_memcpy(name, OBD(o->ent).name, OBD_NAME);
		}
		UNLOCK();
	}
	*p_ops = o->vops;
	*p_ctx = o->vctx;
	return there;
}

/* Record 0: the name and what the one who serves it says, links included */
LOCAL INT vdev_describe( OBDHND *o, UB *t, INT max )
{
	CONST T_OBDVOPS	*ops;
	void		*ctx;
	UB		name[OBD_NAME];
	INT		n;

	if ( !vdev_take(o, name, &ops, &ctx) ) {
		return E_NOEXS;
	}
	n = knl_oj_put(t, 0, max, "<tad version=\"1.0\" encoding=\"UTF-8\" filename=\"");
	n = knl_oj_put(t, n, max, (CONST char *)name);
	n = knl_oj_put(t, n, max, "\"><document><p>");
	n = knl_oj_put(t, n, max, (CONST char *)name);
	n = knl_oj_put(t, n, max, ": ");
	n = knl_oj_put(t, n, max, ops->kind);
	n = knl_oj_put(t, n, max, "</p>");
	if ( ops->text != NULL ) {
		n = ops->text(ctx, t, n, max);
	}
	return knl_oj_put(t, n, max, "</document></tad>");
}

/*
 * The texts of the records made when they are read, each made as large
 * as it needs (knl_oj_make): the list, the system's records, record 0
 * and the clock
 */
LOCAL INT list_make( void *ctx, UB *t, INT max )
{
	(void)ctx;
	return list_text(t, max);
}

LOCAL INT sys_make( void *ctx, UB *t, INT max )
{
	return knl_obsys_text(*(INT *)ctx, t, max);
}

LOCAL INT desc_make( void *ctx, UB *t, INT max )
{
	return describe((OBDHND *)ctx, t, max);
}

LOCAL INT clock_make( void *ctx, UB *t, INT max )
{
	(void)ctx;
	return clock_text(t, max);
}

LOCAL INT vdesc_make( void *ctx, UB *t, INT max )
{
	return vdev_describe((OBDHND *)ctx, t, max);
}

LOCAL ER vdev_rea( OBDHND *o, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	CONST T_OBDVOPS	*ops;
	void		*ctx;

	if ( !vdev_take(o, NULL, &ops, &ctx) ) {
		return E_NOEXS;			/* the device went */
	}
	if ( recno < 0 || recno >= ops->nrec ) {
		return E_NOEXS;
	}
	if ( recno > 0 ) {
		return ( ops->rea != NULL ) ? ops->rea(ctx, &o->pos, recno, off, buf, size, p_asize )
					    : E_NOSPT;
	}
	return knl_oj_make_rea(vdesc_make, o, off, buf, size, p_asize);
}

LOCAL ER vdev_wri( OBDHND *o, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	CONST T_OBDVOPS	*ops;
	void		*ctx;

	if ( !vdev_take(o, NULL, &ops, &ctx) ) {
		return E_NOEXS;
	}
	if ( recno < 0 || recno >= ops->nrec ) {
		return E_NOEXS;
	}
	if ( recno == 0 || ops->wri == NULL ) {
		return E_RONLY;
	}
	return ops->wri(ctx, &o->pos, recno, off, buf, size, p_asize);
}

LOCAL ER vdev_lrc( OBDHND *o, T_OBREC *buf, INT n, INT *p_cnt )
{
	CONST T_OBDVOPS	*ops;
	void		*ctx;
	INT		k;

	if ( !vdev_take(o, NULL, &ops, &ctx) ) {
		return E_NOEXS;
	}
	for ( k = 0; k < ops->nrec && k < n; k++ ) {
		buf[k].recno = k;
		buf[k].rt = ( k == 0 ) ? OB_RT_TAD : OB_RT_SYSDATA;
		buf[k].sub = 0;
		if ( k == 0 ) {
			buf[k].size = knl_oj_make_len(vdesc_make, o);
		} else {
			buf[k].size = ( ops->size != NULL ) ? ops->size(ctx, k) : 0;
		}
	}
	*p_cnt = k;
	return E_OK;
}

LOCAL ER vdev_gat( OBDHND *o, UB *json, SZ size, SZ *p_asize )
{
	CONST T_OBDVOPS	*ops;
	void		*ctx;
	UB		name[OBD_NAME];
	INT		n;

	if ( !vdev_take(o, name, &ops, &ctx) ) {
		return E_NOEXS;
	}
	n = knl_oj_put(json, 0, (INT)size, "{\"name\":");
	n = knl_oj_put_str(json, n, (INT)size, name);
	n = knl_oj_put(json, n, (INT)size, ",\"device\":{\"kind\":");
	n = knl_oj_put_str(json, n, (INT)size, (CONST UB *)ops->kind);
	if ( ops->attr != NULL ) {
		n = ops->attr(ctx, json, n, (INT)size);
	}
	n = knl_oj_put(json, n, (INT)size, "}}");
	if ( n < 0 ) {
		return E_LIMIT;
	}
	if ( p_asize != NULL ) *p_asize = n;
	return E_OK;
}

LOCAL ER obd_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	OBDHND	*o = hnd_of(h);
	SZ	asz = 0;
	ER	er;

	(void)nowait;
	if ( o == NULL ) {
		return E_ID;
	}
	if ( o->vops != NULL ) {
		return vdev_rea(o, recno, off, buf, size, p_asize);
	}
	if ( off < 0 ) {
		return E_PAR;
	}
	if ( o->ent == ENT_LIST ) {
		if ( recno != 0 ) {
			return E_NOEXS;
		}
		return knl_oj_make_rea(list_make, NULL, off, buf, size, p_asize);
	}
	if ( o->ent == ENT_SYSTEM && recno > 0 ) {
		if ( recno >= OB_SYS_NREC ) {
			return E_NOEXS;
		}
		return knl_oj_make_rea(sys_make, &recno, off, buf, size, p_asize);
	}
	if ( o->dd <= 0 && o->ent >= 0 && recno > 0 ) {
		return E_OACV;			/* opened for the attributes only */
	}
	if ( recno == 0 || ( recno == 1 && o->ent < 0 ) ) {
		return knl_oj_make_rea(( recno == 0 ) ? desc_make : clock_make, o, off, buf, size, p_asize);
	}
	if ( o->sound ) {
		if ( recno < 1 || recno >= OB_SND_NREC ) {
			return E_NOEXS;
		}
		if ( recno > OB_SND_PCM && off != 0 ) {
			return E_PAR;		/* an attribute is read whole */
		}
		er = tk_srea_dev(o->dd, snd_dn[recno], buf, size, &asz);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;
		return ( er < E_OK ) ? er : E_OK;
	}
	if ( o->camera ) {
		if ( recno < 1 || recno >= OB_CAM_NREC ) {
			return E_NOEXS;
		}
		if ( off != 0 ) {
			return E_PAR;		/* a frame and an attribute are read whole */
		}
		er = tk_srea_dev(o->dd, cam_dn[recno], buf, size, &asz);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;
		return ( er < E_OK ) ? er : E_OK;
	}
	if ( o->serial && recno > 1 && recno < OB_SER_NREC ) {
		if ( off != 0 ) {
			return E_PAR;		/* an attribute is read whole */
		}
		er = tk_srea_dev(o->dd, ser_dn[recno], buf, size, &asz);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;
		return ( er < E_OK ) ? er : E_OK;
	}
	if ( recno != 1 ) {
		return E_NOEXS;
	}
	if ( o->blksz > 1 ) {
		if ( off % o->blksz != 0 || size % o->blksz != 0 ) {
			return E_PAR;		/* whole blocks only */
		}
		/* the block drivers take the start in blocks and the size in bytes */
		er = tk_srea_dev_d(o->dd, (D)( off / o->blksz ), buf, size, &asz);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;
		return ( er < E_OK ) ? er : E_OK;
	}
	er = tk_srea_dev(o->dd, 0, buf, size, &asz);
	if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;

	return ( er < E_OK ) ? er : E_OK;
}

LOCAL ER obd_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize,
		  BOOL nowait )
{
	OBDHND	*o = hnd_of(h);
	SZ	asz = 0;
	ER	er;

	(void)nowait;
	if ( o == NULL ) {
		return E_ID;
	}
	if ( o->vops != NULL ) {
		return vdev_wri(o, recno, off, buf, size, p_asize);
	}
	if ( o->ent == ENT_LIST ) {
		return E_RONLY;
	}
	if ( o->ent == ENT_SYSTEM ) {
		if ( recno <= 0 || recno >= OB_SYS_NREC ) {
			return ( recno == 0 ) ? E_RONLY : E_NOEXS;
		}
		if ( off != 0 ) {
			return E_PAR;		/* what is asked is written whole */
		}
		er = knl_obsys_write(recno, (CONST UB *)buf, size);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
		return er;
	}
	if ( o->dd <= 0 && o->ent >= 0 && recno > 0 ) {
		return E_OACV;			/* opened for the attributes only */
	}
	if ( o->sound && recno >= 1 && recno < OB_SND_NREC ) {
		if ( off != 0 ) {
			return E_PAR;		/* sound is written as it comes, an attribute whole */
		}
		if ( recno == OB_SND_OUTS || recno == OB_SND_OUTSEL ) {
			/* the output is the system's: chosen through its record 8, which is protected */
			return E_RONLY;
		}
		er = tk_swri_dev(o->dd, snd_dn[recno], buf, size, &asz);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;
		return ( er < E_OK ) ? er : E_OK;
	}
	if ( o->camera && ( recno == OB_CAM_MODE || recno == OB_CAM_STREAM ) ) {
		if ( off != 0 ) {
			return E_PAR;
		}
		er = tk_swri_dev(o->dd, cam_dn[recno], buf, size, &asz);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;
		return ( er < E_OK ) ? er : E_OK;
	}
	if ( o->serial && recno == 2 ) {
		if ( off != 0 ) {
			return E_PAR;
		}
		er = tk_swri_dev(o->dd, TDN_SER_SPEED, buf, size, &asz);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;
		return ( er < E_OK ) ? er : E_OK;
	}
	if ( recno != 1 || off < 0 ) {
		return ( recno == 0 || ( o->serial && recno == 3 ) ) ? E_RONLY : E_PAR;
	}
	if ( o->ent < 0 ) {
		er = clock_set((CONST UB *)buf, size);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = size;
		return er;
	}
	if ( o->blksz > 1 ) {
		if ( off % o->blksz != 0 || size % o->blksz != 0 ) {
			return E_PAR;
		}
		er = tk_swri_dev_d(o->dd, (D)( off / o->blksz ), buf, size, &asz);
		if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;
		return ( er < E_OK ) ? er : E_OK;
	}
	er = tk_swri_dev(o->dd, 0, buf, size, &asz);
	if ( er >= E_OK && p_asize != NULL ) *p_asize = asz;

	return ( er < E_OK ) ? er : E_OK;
}

LOCAL ER obd_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	OBDHND	*o = hnd_of(h);
	ID	dd;
	INT	k;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( o->vops != NULL ) {
		return vdev_lrc(o, buf, n, p_cnt);
	}
	if ( o->ent == ENT_LIST ) {
		if ( n > 0 ) {
			buf[0].recno = 0;
			buf[0].rt = OB_RT_TAD;
			buf[0].sub = 0;
			buf[0].size = knl_oj_make_len(list_make, NULL);
		}
		*p_cnt = ( n > 0 ) ? 1 : 0;
		return E_OK;
	}
	if ( o->ent == ENT_SYSTEM ) {
		for ( k = 0; k < OB_SYS_NREC && k < n; k++ ) {
			buf[k].recno = k;
			buf[k].rt = ( k == 0 ) ? OB_RT_TAD : OB_RT_SYSDATA;
			buf[k].sub = 0;
			buf[k].size = ( k == 0 ) ? knl_oj_make_len(desc_make, o) : knl_oj_make_len(sys_make, &k);
		}
		*p_cnt = k;
		return E_OK;
	}
	/* a key for the attributes has the device asked with it opened for now */
	dd = o->dd;
	if ( dd <= 0 && o->ent >= 0 && ( o->sound || o->camera || o->blksz > 1 ) ) {
		dd = tk_opn_dev(OBD(o->ent).name, TD_READ);
	}
	for ( k = 0; k < ( o->sound ? OB_SND_NREC : o->camera ? OB_CAM_NREC : o->serial ? OB_SER_NREC : 2 ) && k < n; k++ ) {
		buf[k].recno = k;
		buf[k].rt = ( k == 0 ) ? OB_RT_TAD : OB_RT_SYSDATA;
		buf[k].sub = 0;
		buf[k].size = ( k == 0 ) ? knl_oj_make_len(desc_make, o) : 0;
		if ( o->serial && k > 1 ) {
			buf[k].size = ( k == 2 ) ? sizeof(UW) : sizeof(T_SERINFO);
		}
		if ( o->camera && k > OB_CAM_FRAME ) {
			SZ	asz = 0;

			if ( dd > 0 && tk_srea_dev(dd, cam_dn[k], NULL, 0, &asz) >= E_OK ) {
				buf[k].size = (UD)asz;
			}
		}
		if ( o->sound && k > OB_SND_PCM ) {
			SZ	asz = 0;

			/* an attribute read with no room says how large it is */
			if ( dd > 0 && tk_srea_dev(dd, snd_dn[k], NULL, 0, &asz) >= E_OK ) {
				buf[k].size = (UD)asz;
			}
		}
		if ( k == 1 && o->blksz > 1 && !o->sound && !o->serial && o->ent >= 0 ) {
			DiskInfo	di;
			SZ		asz = 0;

			/* a disk's blocks: record 1 is as long as the unit */
			if ( dd > 0 && tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz) >= E_OK
			  && di.blockcont_d > 0 ) {
				buf[k].size = (UD)di.blockcont_d * (UD)o->blksz;
			}
		}
	}
	if ( dd > 0 && dd != o->dd ) {
		tk_cls_dev(dd, 0);
	}
	*p_cnt = k;

	return E_OK;
}

/*
 * The device's interrupts in its attributes (design 4.5):
 * "irq":{"cpu":the processor asked for, 0 none,"intno":[...],"on":[...]}
 * with the processor each interrupt goes to now, and "count":[...] how
 * many of each have arrived when the driver counts them all (NVMe);
 * nothing when the device's driver named none of its interrupts
 */
#define IRQ_SHOW	8

IMPORT BOOL	knl_int_count_of( UINT intno, UW *p_n );	/* kernel/tkernel/inttarget.c */

LOCAL INT irq_text( UB *json, INT n, INT size, CONST UB *name )
{
	UINT	intno[IRQ_SHOW];
	UW	cnt[IRQ_SHOW];
	ID	prc[IRQ_SHOW], want = 0;
	INT	k, i;

	k = knl_int_dev_info(name, &want, intno, prc, IRQ_SHOW);
	if ( k <= 0 && want == 0 ) {
		return n;
	}
	if ( k > IRQ_SHOW ) k = IRQ_SHOW;
	n = knl_oj_put(json, n, size, ",\"irq\":{\"cpu\":");
	n = knl_oj_put_num(json, n, size, want);
	n = knl_oj_put(json, n, size, ",\"intno\":[");
	for ( i = 0; i < k; i++ ) {
		if ( i > 0 ) n = knl_oj_put(json, n, size, ",");
		n = knl_oj_put_num(json, n, size, intno[i]);
	}
	n = knl_oj_put(json, n, size, "],\"on\":[");
	for ( i = 0; i < k; i++ ) {
		if ( i > 0 ) n = knl_oj_put(json, n, size, ",");
		n = knl_oj_put_num(json, n, size, prc[i]);
	}
	for ( i = 0; i < k && knl_int_count_of(intno[i], &cnt[i]); i++ ) ;
	if ( k > 0 && i == k ) {
		n = knl_oj_put(json, n, size, "],\"count\":[");
		for ( i = 0; i < k; i++ ) {
			if ( i > 0 ) n = knl_oj_put(json, n, size, ",");
			n = knl_oj_put_num(json, n, size, cnt[i]);
		}
	}
	return knl_oj_put(json, n, size, "]}");
}

LOCAL ER obd_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	OBDHND	*o = hnd_of(h);
	T_FSMNT	mnt;
	INT	n;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( o->vops != NULL ) {
		return vdev_gat(o, json, size, p_asize);
	}
	if ( o->ent == ENT_LIST ) {
		n = knl_oj_put(json, 0, (INT)size, "{\"name\":\"devices\",\"device\":{\"kind\":\"list\","
			       "\"virtual\":true,\"count\":");
		n = knl_oj_put_num(json, n, (INT)size, list_count());
		n = knl_oj_put(json, n, (INT)size, "}}");
		if ( n < 0 ) {
			return E_LIMIT;
		}
		if ( p_asize != NULL ) *p_asize = n;
		return E_OK;
	}
	if ( o->ent == ENT_SYSTEM ) {
		n = knl_oj_put(json, 0, (INT)size, "{\"name\":\"system\",\"device\":{\"kind\":\"system\","
			       "\"virtual\":true}}");
		if ( n < 0 ) {
			return E_LIMIT;
		}
		if ( p_asize != NULL ) *p_asize = n;
		return E_OK;
	}
	n = knl_oj_put(json, 0, (INT)size, "{\"name\":");
	n = knl_oj_put_str(json, n, (INT)size,
			   ( o->ent < 0 ) ? (CONST UB *)"clock" : OBD(o->ent).name);
	n = knl_oj_put(json, n, (INT)size, ",\"device\":{\"kind\":");
	n = knl_oj_put(json, n, (INT)size, ( o->ent < 0 ) ? "\"clock\",\"virtual\":true"
				   : ( o->blksz > 1 ) ? "\"disk\",\"virtual\":false"
						      : "\"stream\",\"virtual\":false");
	n = knl_oj_put(json, n, (INT)size, ",\"blksz\":");
	n = knl_oj_put_num(json, n, (INT)size, o->blksz);
	n = knl_oj_put(json, n, (INT)size, "}");
	if ( o->ent >= 0 ) {
		n = irq_text(json, n, (INT)size, OBD(o->ent).name);
	}
	/*
	 * A disk: what is on it (design 7.6), and the volume mounted from
	 * it, where, with what and how (design 12.2.3)
	 */
	if ( o->ent >= 0 && o->blksz > 1 && !o->sound && !o->serial ) {
		T_FSPROBE	*pr = (T_FSPROBE *)Kmalloc(sizeof(T_FSPROBE));

		BOOL		mounted = knl_fs_mount_of((CONST char *)OBD(o->ent).name, &mnt);

		n = knl_oj_put(json, n, (INT)size, ",\"tessronos\":{");
		if ( mounted ) {
			n = knl_oj_put(json, n, (INT)size, "\"mount\":{\"path\":");
			n = knl_oj_put_str(json, n, (INT)size, mnt.path);
			n = knl_oj_put(json, n, (INT)size, ",\"fs\":");
			n = knl_oj_put_str(json, n, (INT)size, mnt.fimp);
			n = knl_oj_put(json, n, (INT)size, ( mnt.flags & FS_MNT_RDONLY ) ? ",\"readonly\":true}"
											 : ",\"readonly\":false}");
		}
		if ( pr != NULL && knl_fs_probe((CONST char *)OBD(o->ent).name, pr) >= E_OK ) {
			n = knl_oj_put(json, n, (INT)size, mounted ? ",\"format\":{\"kind\":" : "\"format\":{\"kind\":");
			n = knl_oj_put_str(json, n, (INT)size, pr->name);
			n = knl_oj_put(json, n, (INT)size, ",\"label\":");
			n = knl_oj_put_str(json, n, (INT)size, pr->label);
			n = knl_oj_put(json, n, (INT)size, ",\"fs\":");
			n = knl_oj_put_str(json, n, (INT)size, pr->fimp);
			n = knl_oj_put(json, n, (INT)size, "}");
		}
		if ( pr != NULL ) Kfree(pr);
		n = knl_oj_put(json, n, (INT)size, "}");
	}
	n = knl_oj_put(json, n, (INT)size, "}");
	if ( n < 0 ) {
		return E_LIMIT;
	}
	if ( p_asize != NULL ) *p_asize = n;

	return E_OK;
}

/*
 * A device's attributes written: of what is written, only the processor
 * the device's interrupts go to is taken (irq.cpu, 0 the
 * system's choice, design 4.5), and kept in the device box with the
 * device's name, so that it is taken again at the next start
 */
LOCAL ER obd_sat( INT h, CONST UB *json, SZ size )
{
	OBDHND	*o = hnd_of(h);
	INT	p, len = (INT)size;
	D	v = 0;
	ID	vol;
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	if ( o->ent < 0 ) {
		return E_NOSPT;
	}
	p = knl_oj_path(json, len, "irq", "cpu");
	if ( p < 0 ) {
		return E_OK;			/* nothing this takes */
	}
	if ( !knl_oj_num(json, len, p, &v) || v < 0 || v > CNF_MAX_PRCID ) {
		return E_PAR;
	}
	er = knl_int_dev_target(OBD(o->ent).name, (ID)v);
	if ( er < E_OK ) {
		return er;
	}
	LOCK();
	OBD(o->ent).irqcpu = (UH)v;
	if ( obd_loaded && ( vol = knl_obfile_first_vol() ) > 0 ) {
		box_save(vol);
	}
	UNLOCK();
	dev_post(&OBD(o->ent).uuid, OB_E_CHANGE);
	return E_OK;
}

LOCAL ER obd_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	INT	cnt = 0, i;

	CONST TS_UUID	*virt[3];
	INT		k;

	virt[0] = &ob_uuid_clock;
	virt[1] = &ob_uuid_devlist;
	virt[2] = &ob_uuid_system;
	LOCK();
	refresh();
	while ( cnt < n ) {
		CONST TS_UUID	*best = NULL;

		/* the clock, the list and the system among them */
		for ( k = 0; k < 3; k++ ) {
			if ( cnt > 0 && ts_uuid_cmp(virt[k], &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(virt[k], from) <= 0 ) continue;
			if ( best == NULL || ts_uuid_cmp(virt[k], best) < 0 ) {
				best = virt[k];
			}
		}
		for ( i = 0; i < obd_hi; i++ ) {
			if ( !OBD(i).used || !OBD(i).seen ) continue;
			if ( cnt > 0 && ts_uuid_cmp(&OBD(i).uuid, &buf[cnt - 1]) <= 0 ) continue;
			if ( cnt == 0 && from != NULL && ts_uuid_cmp(&OBD(i).uuid, from) <= 0 ) continue;
			if ( best == NULL || ts_uuid_cmp(&OBD(i).uuid, best) < 0 ) {
				best = &OBD(i).uuid;
			}
		}
		if ( best == NULL ) break;
		buf[cnt++] = *best;
	}
	UNLOCK();
	*p_cnt = cnt;

	return E_OK;
}

/* ---------------------------------------------------------------- mounting a disk's volume */

/*
 * The disk a key names: the key must be the caller's (EX_BADF), the
 * object a disk device (EX_NOTBLK), and the key allow 'ops' on record
 * 1, the blocks, as reading and writing them would (EX_ACCES). Its
 * name and object come back.
 */
LOCAL ER disk_of_key( ID key, UINT ops, UB *name, TS_UUID *p_uuid )
{
	T_RDEV	rd;
	INT	e;

	if ( knl_ob_key_rec(key, 0, -1, p_uuid) < E_OK ) {
		return EX_BADF;
	}
	if ( obd_mtx <= 0 || is_clock(p_uuid) || is_list(p_uuid) ) {
		return EX_NOTBLK;
	}
	LOCK();
	e = ent_by_uuid(p_uuid);
	if ( e >= 0 ) {
		knl_memcpy(name, OBD(e).name, OBD_NAME);
	}
	UNLOCK();
	if ( e < 0 || tk_ref_dev(name, &rd) < E_OK || rd.blksz <= 1 || dev_is_sound(name) ) {
		return EX_NOTBLK;
	}
	if ( ops != 0 && knl_ob_key_rec(key, ops, 1, p_uuid) < E_OK ) {
		return EX_ACCES;
	}
	return EX_OK;
}

/* What a mount with these flags needs of the blocks */
LOCAL UINT mount_ops( UINT flags )
{
	return ( ( flags & FS_MNT_RDONLY ) != 0 ) ? OB_OP_READ : ( OB_OP_READ | OB_OP_WRITE );
}

EXPORT ER fs_attach_dev( ID devkey, CONST char *fimpnm, UINT flags )
{
	UB	name[OBD_NAME];
	TS_UUID	u;
	ER	er;

	if ( ( flags & ~(UINT)FS_MNT_RDONLY ) != 0 ) {
		return EX_INVAL;
	}
	er = disk_of_key(devkey, mount_ops(flags), name, &u);
	if ( er < EX_OK ) {
		return er;
	}
	if ( fimpnm == NULL ) {
		/* the fimp of what is on it */
		T_FSPROBE	*pr = (T_FSPROBE *)Kmalloc(sizeof(T_FSPROBE));

		if ( pr == NULL ) {
			return EX_NOMEM;
		}
		er = knl_fs_probe((CONST char *)name, pr);
		fimpnm = ( er >= E_OK ) ? knl_fs_kind_fimp(pr->kind) : "";
		Kfree(pr);
		if ( er < E_OK ) {
			return EX_IO;
		}
		if ( fimpnm[0] == 0 ) {
			return EX_NOTSUP;	/* no fimp mounts this kind */
		}
	}
	er = knl_fs_attach_media(fimpnm, (CONST char *)name, flags);
	if ( er >= EX_OK ) {
		dev_post(&u, OB_E_CHANGE);
	}
	return er;
}

/* A disk's file system from its key, with no path (design 7.2), for owner */
EXPORT INT knl_fs_opn_vol_as( ID devkey, CONST char *fimpnm, UINT flags, ID owner )
{
	UB	name[OBD_NAME];
	TS_UUID	u;
	ER	er;

	if ( ( flags & ~(UINT)FS_MNT_RDONLY ) != 0 ) {
		return EX_INVAL;
	}
	er = disk_of_key(devkey, mount_ops(flags), name, &u);
	if ( er < EX_OK ) {
		return er;
	}
	if ( fimpnm == NULL ) {
		T_FSPROBE	*pr = (T_FSPROBE *)Kmalloc(sizeof(T_FSPROBE));

		if ( pr == NULL ) {
			return EX_NOMEM;
		}
		er = knl_fs_probe((CONST char *)name, pr);
		fimpnm = ( er >= E_OK ) ? knl_fs_kind_fimp(pr->kind) : "";
		Kfree(pr);
		if ( er < E_OK ) {
			return EX_IO;
		}
		if ( fimpnm[0] == 0 ) {
			return EX_NOTSUP;
		}
	}
	return knl_fs_open_vol(fimpnm, (CONST char *)name, flags, owner);
}

EXPORT INT fs_opn_vol( ID devkey, CONST char *fimpnm, UINT flags )
{
	return knl_fs_opn_vol_as(devkey, fimpnm, flags, 0);
}

/* Undone by one who could have done it: the same rights on the blocks */
EXPORT ER fs_detach_dev( ID devkey )
{
	UB	name[OBD_NAME];
	T_FSMNT	mnt;
	TS_UUID	u;
	ER	er;

	er = disk_of_key(devkey, 0, name, &u);
	if ( er < EX_OK ) {
		return er;
	}
	if ( !knl_fs_mount_of((CONST char *)name, &mnt) ) {
		return EX_NOENT;
	}
	if ( knl_ob_key_rec(devkey, mount_ops(mnt.flags), 1, &u) < E_OK ) {
		return EX_ACCES;
	}
	er = knl_fs_detach_media((CONST char *)name);
	if ( er >= EX_OK ) {
		dev_post(&u, OB_E_CHANGE);
	}
	return er;
}

LOCAL CONST T_OBMGR obd_mgr = {
	OB_T_DEVICE, 0, "device",
	obd_find, obd_ref, obd_prot, NULL, NULL, NULL,
	obd_open, obd_close, obd_rea, obd_wri, NULL, NULL, NULL, obd_lrc,
	obd_gat, obd_sat, NULL, obd_lst, NULL,
	NULL, NULL, NULL, NULL,
	NULL, NULL,
	/* no icon of their own */
	NULL, NULL,
	NULL,			/* records replaced by writing over them */
	NULL,			/* nothing to wait for */
	NULL			/* nothing comes in to wait for (ob_rea_tmo) */
};

EXPORT ER knl_obdev_init( void )
{
	T_CMTX	cmtx;
	INT	no;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	obd_mtx = tk_cre_mtx(&cmtx);
	if ( obd_mtx <= 0 ) {
		return (ER)obd_mtx;
	}
	no = knl_ob_regist(&obd_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}

/*
 * The devices outside the device management, made objects once the
 * object layer is up: the volume the device box is on is attached by
 * now, so they get the UUIDs they had before.
 */
EXPORT void knl_obdev_start( void )
{
	if ( obd_mtx <= 0 ) {
		return;
	}
	obe_start();
	knl_obrand_start();
	knl_obdisp_start();
	knl_obinput_start();
	knl_obgpio_start();
	knl_obusb_start();
}
