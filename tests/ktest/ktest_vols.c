/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_vols.c
 *	Many disks and many volumes at once (design 7.6, 8.3)
 *
 *	Four USB disks that tools/tests/mkvoldisk.py makes (KT_VOL_DIR) are
 *	plugged into the hub through the QEMU monitor: seventeen volumes
 *	together, five of them TSFS volumes of objects, ten FAT volumes of
 *	the three kinds, one of them in an extended partition's chain of
 *	logical ones, an exFAT one and a blank partition. Then:
 *
 *	  - what is on each is told from its first blocks;
 *	  - every TSFS volume is attached and every FAT one mounted at
 *	    once, more than the tables of four and eight there were held;
 *	    the others are refused with EX_NOTSUP;
 *	  - objects and files are written and read back on each;
 *	  - a link from an object of one volume to an object of another
 *	    finds it; it is not counted on its own volume, and is counted
 *	    on the volume the object is on through the record that volume
 *	    keeps of links from other volumes;
 *	  - that record follows links made and given up, is put right when
 *	    the volume comes back after links changed while it was away,
 *	    keeps an object only another volume links to from being
 *	    garbage, and is checked and put right by the disk repair;
 *	  - a UUID is one object's on every volume: an object is not made
 *	    with the UUID of one on another volume, and a copy of a volume
 *	    is not attached beside it;
 *	  - a volume with an object open is not detached;
 *	  - a disk pulled out with an object and a file open on it: the
 *	    volume goes, the key and the descriptor answer errors, and all
 *	    is let go when they are closed; put back, it is attached again
 *	    with its objects as they were;
 *	  - attaching, using, detaching, pulling out and putting back again
 *	    and again leaves the tables, the open devices and the memory as
 *	    they were;
 *	  - four tasks on four processors attach, write, read and detach
 *	    volumes together, and every volume checks whole afterwards;
 *	  - more than twenty-six disks behind three hubs are named on past
 *	    "udz", and the fortieth partition of a disk is found and
 *	    mounted.
 *
 *	The disks are taken out at the end, so that the suites after see
 *	the machine as it was.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <sys/queue.h>
#include <ts/fs.h>
#include <ts/blk.h>
#include <ts/ob.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsck.h>
#include <ts/uuid.h>
#include <ts/tad.h>
#include <ts/om.h>

#ifndef KT_VOL_DIR
#define KT_VOL_DIR	""		/* no images: nothing is plugged in */
#endif

#define NDISK		4
#define NAME_MAX	OB_DV_NAME
#define LIST_MAX	2048
#define WAIT_MS		30000
#define VOL_STACK	( 64 * 1024 )

IMPORT QUEUE	knl_FreeOpnCB;		/* the device management's free open blocks */
IMPORT SZ	knl_Iused( INT *p_areas );
IMPORT INT	knl_ob_hnd_made( SZ *p_each );	/* the object layer's table of opened objects */

/* The disks, as mkvoldisk.py makes them */
typedef struct {
	CONST char	*id;		/* the device's id in QEMU */
	CONST char	*drive;
	CONST char	*file;
	CONST char	*port;		/* on the hub */
	UD		bytes;
	INT		parts;		/* partitions it has */
	UB		name[NAME_MAX];	/* "ude", once it has come */
	BOOL		in;
} DISK;

LOCAL DISK	dk[NDISK] = {
	{ "vola", "vdka", "vola.img", "4.5", 50ULL << 20, 5, "", FALSE },
	{ "volb", "vdkb", "volb.img", "4.6", 44ULL << 20, 8, "", FALSE },
	{ "volc", "vdkc", "volc.img", "4.7", 26ULL << 20, 3, "", FALSE },
	{ "vold", "vdkd", "vold.img", "4.8", 18ULL << 20, 0, "", FALSE },
};

/* What each partition holds: disk, partition (-1 the whole disk), kind, label */
typedef struct {
	INT		disk, part;
	UINT		kind;
	CONST char	*label;
} VOLDEF;

LOCAL CONST VOLDEF vd[] = {
	{ 0, 0, FS_KIND_TSFS, "VOLA1" }, { 0, 1, FS_KIND_FAT16, NULL }, { 0, 2, FS_KIND_TSFS, "VOLA3" },
	{ 0, 3, FS_KIND_EXFAT, NULL }, { 0, 4, FS_KIND_NTFS, NULL },
	{ 1, 0, FS_KIND_FAT16, NULL }, { 1, 1, FS_KIND_FAT12, NULL }, { 1, 2, FS_KIND_FAT12, NULL },
	{ 1, 3, FS_KIND_FAT12, NULL }, { 1, 4, FS_KIND_FAT12, NULL }, { 1, 5, FS_KIND_FAT12, NULL },
	{ 1, 6, FS_KIND_FAT12, NULL }, { 1, 7, FS_KIND_TSFS, "VOLB" },
	{ 2, 0, FS_KIND_TSFS, "VOLC1" }, { 2, 1, FS_KIND_BLANK, NULL }, { 2, 2, FS_KIND_FAT16, NULL },
	{ 3, -1, FS_KIND_FAT16, NULL },
};

#define NVOL	( (INT)( sizeof(vd) / sizeof(vd[0]) ) )
#define V_COPY	14			/* the blank partition a volume is copied into */

LOCAL BOOL	have_disks = FALSE;
LOCAL UB	*names;			/* knl_obdev_disks, NAME_MAX each */

/* ---------------------------------------------------------------- the disks */

LOCAL INT differ( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != b[i] ) return 1;
	}
	return 0;
}

LOCAL void vol_dev( INT v, UB *out )
{
	CONST DISK	*d = &dk[vd[v].disk];
	INT		k = (INT)knl_strlen((CONST char *)d->name);

	knl_memcpy(out, d->name, k);
	if ( vd[v].part >= 0 ) {
		if ( vd[v].part >= 10 ) out[k++] = (UB)( '0' + vd[v].part / 10 );
		out[k++] = (UB)( '0' + vd[v].part % 10 );
	}
	out[k] = 0;
}

LOCAL void media_path( INT v, char *out )
{
	UB	dev[NAME_MAX];

	vol_dev(v, dev);
	knl_strcpy(out, FS_MEDIA_DIR "/");
	knl_strcat(out, (CONST char *)dev);
}

LOCAL BOOL is_whole( CONST UB *n )
{
	INT	k = (INT)knl_strlen((CONST char *)n);

	return (BOOL)( k > 0 && !( n[k - 1] >= '0' && n[k - 1] <= '9' ) );
}

LOCAL BOOL listed( CONST UB *name )
{
	INT	n = knl_obdev_disks(names, LIST_MAX), i;

	for ( i = 0; i < n; i++ ) {
		if ( knl_strcmp((CONST char *)( names + i * NAME_MAX ), (CONST char *)name) == 0 ) return TRUE;
	}
	return FALSE;
}

/* The whole disk of a size that is not one of the others, once it is there */
LOCAL BOOL find_disk( DISK *d )
{
	T_FSPROBE	pr;
	INT		n = knl_obdev_disks(names, LIST_MAX), i, k;

	for ( i = 0; i < n; i++ ) {
		UB	*nm = names + i * NAME_MAX;
		BOOL	taken = FALSE;

		if ( !is_whole(nm) ) continue;
		for ( k = 0; k < NDISK; k++ ) {
			if ( &dk[k] != d && dk[k].in && knl_strcmp((CONST char *)dk[k].name, (CONST char *)nm) == 0 ) {
				taken = TRUE;
			}
		}
		if ( taken || knl_fs_probe((CONST char *)nm, &pr) < E_OK || pr.bytes != d->bytes ) continue;
		knl_memcpy(d->name, nm, NAME_MAX);
		return TRUE;
	}
	return FALSE;
}

LOCAL void plug( DISK *d )
{
	tm_printf((UB *)"KTEST QMP {\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":"
		  "\"drive_add 0 if=none,id=%s,file=%s/%s,format=raw\"}}\n", d->drive, KT_VOL_DIR, d->file);
	tk_dly_tsk(300);
	tm_printf((UB *)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":{\"driver\":\"usb-storage\","
		  "\"id\":\"%s\",\"bus\":\"xhci.0\",\"port\":\"%s\",\"drive\":\"%s\",\"removable\":true}}\n",
		  d->id, d->port, d->drive);
}

LOCAL void unplug( DISK *d )
{
	tm_printf((UB *)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"%s\"}}\n", d->id);
	tk_dly_tsk(300);
	tm_printf((UB *)"KTEST QMP {\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":"
		  "\"drive_del %s\"}}\n", d->drive);
}

/* A disk plugged in and waited for with its partitions */
LOCAL BOOL come( DISK *d )
{
	INT	t, p;
	UB	nm[NAME_MAX];

	plug(d);
	for ( t = 0; t < WAIT_MS / 100; t++ ) {
		tk_dly_tsk(100);
		if ( !find_disk(d) ) continue;
		for ( p = 0; p < d->parts; p++ ) {
			INT	k;

			knl_strcpy((char *)nm, (CONST char *)d->name);
			k = (INT)knl_strlen((char *)nm);
			if ( p >= 10 ) nm[k++] = (UB)( '0' + p / 10 );
			nm[k++] = (UB)( '0' + p % 10 );
			nm[k] = 0;
			if ( !listed(nm) ) break;
		}
		if ( p == d->parts ) {
			d->in = TRUE;
			tm_printf((UB *)"  %s is %s with %d partitions\n", d->file, d->name, d->parts);
			return TRUE;
		}
	}
	{
		INT	n = knl_obdev_disks(names, LIST_MAX), i;

		tm_printf((UB *)"  %s did not come (%s); the disks:", d->file, d->name);
		for ( i = 0; i < n; i++ ) tm_printf((UB *)" %s", names + i * NAME_MAX);
		tm_printf((UB *)"\n");
	}
	return FALSE;
}

/* A disk pulled out and waited for until its device is gone */
LOCAL BOOL go( DISK *d )
{
	INT	t;

	unplug(d);
	for ( t = 0; t < WAIT_MS / 100; t++ ) {
		tk_dly_tsk(100);
		if ( !listed(d->name) ) {
			d->in = FALSE;
			return TRUE;
		}
	}
	return FALSE;
}

/* ---------------------------------------------------------------- counting */

typedef struct {
	INT	fs_made, fs_used, obf_made, obf_used, ts_made, ts_used, blk_made, blk_used;
	INT	opn_free, areas;
	SZ	heap;
} COUNTS;

LOCAL INT que_len( QUEUE *q )
{
	QUEUE	*e;
	INT	n = 0;

	for ( e = q->next; e != q && n < 100000; e = e->next ) n++;
	return n;
}

LOCAL void counts( COUNTS *c )
{
	SZ	each = 0;
	INT	made;

	c->fs_made = knl_fs_slots(&c->fs_used);
	c->obf_made = knl_obfile_slots(&c->obf_used);
	c->ts_made = knl_tsfs_slots(&c->ts_used);
	c->blk_made = knl_tsfsblk_slots(&c->blk_used, NULL);
	c->opn_free = que_len(&knl_FreeOpnCB);
	c->heap = knl_Iused(&c->areas);
	/* less what the table of opened objects keeps as it grows to its size */
	made = knl_ob_hnd_made(&each);
	c->heap -= (SZ)made * ( ( each + 7 ) & ~(SZ)7 );
	c->areas -= made;
}

LOCAL void counts_say( CONST char *when, CONST COUNTS *c )
{
	tm_printf((UB *)"  %s: mounts %d/%d volumes %d/%d tsfs %d/%d blk %d/%d, open devices free %d,"
		  " heap %d bytes in %d\n", when, c->fs_used, c->fs_made, c->obf_used, c->obf_made,
		  c->ts_used, c->ts_made, c->blk_used, c->blk_made, c->opn_free, (INT)c->heap, c->areas);
}

/* ---------------------------------------------------------------- attaching and mounting */

/* A FAT volume mounted through its device object, the fimp chosen by what is on it */
LOCAL ER mount_auto( INT v, UINT flags )
{
	TS_UUID	u;
	UB	dev[NAME_MAX];
	ID	key;
	ER	er;

	vol_dev(v, dev);
	if ( knl_obdev_uuid(dev, &u) < E_OK ) return E_NOEXS;
	key = ob_opn_obj(&u, OB_OP_R | ( ( flags & FS_MNT_RDONLY ) ? 0 : OB_OP_WRITE ));
	if ( key <= 0 ) return ( key < 0 ) ? (ER)key : E_SYS;
	er = fs_attach_dev(key, NULL, flags);
	ob_cls_obj(key);
	return er;
}

LOCAL ER unmount( INT v )
{
	UB	dev[NAME_MAX];

	vol_dev(v, dev);
	return knl_fs_detach_media((CONST char *)dev);
}

LOCAL ER attach( INT v )
{
	UB	dev[NAME_MAX];

	vol_dev(v, dev);
	return ob_att_vol((CONST char *)dev, TSFS_STORE_BLK);
}

LOCAL ER detach( INT v )
{
	UB	dev[NAME_MAX];

	vol_dev(v, dev);
	return ob_det_vol((CONST char *)dev);
}

LOCAL BOOL is_fat( INT v )
{
	return (BOOL)( vd[v].kind == FS_KIND_FAT12 || vd[v].kind == FS_KIND_FAT16 || vd[v].kind == FS_KIND_FAT32 );
}

/* Every volume attached or mounted, as it is meant to be; how many there were */
LOCAL INT all_on( void )
{
	INT	v, n = 0;
	ER	er;

	for ( v = 0; v < NVOL; v++ ) {
		if ( vd[v].kind == FS_KIND_TSFS ) {
			er = attach(v);
		} else if ( is_fat(v) ) {
			er = mount_auto(v, 0);
		} else {
			continue;
		}
		if ( er >= E_OK ) {
			n++;
		} else {
			UB	dev[NAME_MAX];

			vol_dev(v, dev);
			tm_printf((UB *)"  %s did not go on: %d\n", dev, (INT)er);
		}
	}
	return n;
}

LOCAL INT all_off( void )
{
	INT	v, n = 0;

	for ( v = 0; v < NVOL; v++ ) {
		if ( vd[v].kind == FS_KIND_TSFS ) {
			if ( detach(v) >= E_OK ) n++;
		} else if ( is_fat(v) ) {
			if ( unmount(v) >= E_OK ) n++;
		}
	}
	(void)fs_sync();
	return n;
}

/* ---------------------------------------------------------------- objects and files */

/* An object made on a volume with a record of bytes; its UUID */
LOCAL ER obj_make( INT v, CONST UB *data, SZ size, TS_UUID *p_u )
{
	T_OBCRE	c;
	UB	dev[NAME_MAX];
	SZ	asz = 0;
	INT	recno = 0;
	ID	key;
	ER	er;

	vol_dev(v, dev);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.name = (CONST UB *)"vols";
	c.vol = (CONST char *)dev;
	er = ob_cre_obj(&c, p_u);
	if ( er < E_OK ) return er;
	key = ob_opn_obj(p_u, OB_OP_R | OB_OP_W);
	if ( key <= 0 ) return ( key < 0 ) ? (ER)key : E_SYS;
	er = ob_apd_rec(key, OB_RT_TAD, 0, &recno);
	if ( er >= E_OK ) er = ob_wri_rec(key, recno, 0, data, size, &asz);
	ob_cls_obj(key);
	return er;
}

/* The record read back and compared */
LOCAL BOOL obj_same( CONST TS_UUID *u, CONST UB *data, SZ size )
{
	UB	*b = (UB *)Kmalloc((SZ)size + 16);
	SZ	asz = 0;
	ID	key;
	BOOL	same = FALSE;

	key = ob_opn_obj(u, OB_OP_R);
	if ( key > 0 && b != NULL && ob_rea_rec(key, 0, 0, b, size + 16, &asz) >= E_OK && asz == size ) {
		same = (BOOL)( differ(b, data, (INT)size) == 0 );
	}
	if ( key > 0 ) ob_cls_obj(key);
	if ( b != NULL ) Kfree(b);
	return same;
}

LOCAL ER file_put( INT v, CONST char *name, CONST UB *data, SZ size )
{
	char	path[64];
	INT	fd, n;

	media_path(v, path);
	knl_strcat(path, "/");
	knl_strcat(path, name);
	fd = fs_open(path, O_CREAT | O_RDWR | O_TRUNC);
	if ( fd < 0 ) return (ER)fd;
	n = fs_write(fd, data, size);
	(void)fs_close(fd);
	return ( n == (INT)size ) ? E_OK : E_IO;
}

LOCAL BOOL file_same( INT v, CONST char *name, CONST UB *data, SZ size )
{
	char	path[64];
	UB	*b = (UB *)Kmalloc((SZ)size + 16);
	INT	fd, n = -1;

	media_path(v, path);
	knl_strcat(path, "/");
	knl_strcat(path, name);
	fd = fs_open(path, O_RDONLY);
	if ( fd >= 0 && b != NULL ) n = fs_read(fd, b, size + 16);
	if ( fd >= 0 ) (void)fs_close(fd);
	n = ( n == (INT)size && differ(b, data, (INT)size) == 0 );
	if ( b != NULL ) Kfree(b);
	return (BOOL)n;
}

LOCAL INT obj_refs( CONST TS_UUID *u )
{
	T_OBREF	r;

	return ( ob_ref_obj(u, &r) >= E_OK ) ? r.refcnt : -1;
}

/* A record of xmlTAD with a link to an object, or none */
LOCAL INT tad_with( UB *buf, INT max, CONST TS_UUID *to )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n;

	knl_strcpy((char *)buf, "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>links");
	if ( to != NULL ) {
		(void)ts_uuid_to_str(to, us, sizeof(us));
		knl_strcat((char *)buf, "<link id=\"");
		knl_strcat((char *)buf, us);
		knl_strcat((char *)buf, "_0.xtad\"/>");
	}
	knl_strcat((char *)buf, "</p></document></tad>");
	n = (INT)knl_strlen((char *)buf);
	return ( n < max ) ? n : max;
}

LOCAL ER rec_put( CONST TS_UUID *u, CONST UB *data, SZ size )
{
	SZ	asz = 0;
	ID	key = ob_opn_obj(u, OB_OP_R | OB_OP_W);
	ER	er;

	if ( key <= 0 ) return ( key < 0 ) ? (ER)key : E_SYS;
	er = ob_trn_rec(key, 0, 0);
	if ( er >= E_OK ) er = ob_wri_rec(key, 0, 0, data, size, &asz);
	ob_cls_obj(key);			/* the link table is made again as it closes */
	return er;
}

/* ---------------------------------------------------------------- the tests */

LOCAL TS_UUID	obj_of[NVOL];		/* an object written on each TSFS volume */
LOCAL COUNTS	base;

LOCAL void test_plug( void )
{
	INT	i;

	if ( KT_VOL_DIR[0] == 0 ) KT_SKIP("no disk images (make KTEST=1 builds them)");
	counts(&base);
	counts_say("before", &base);
	for ( i = 0; i < NDISK; i++ ) {
		if ( !come(&dk[i]) ) {
			if ( i == 0 ) KT_SKIP("no disk came (the QMP helper is not there)");
			KT_ASSERT(dk[i].in);
			return;
		}
	}
	have_disks = TRUE;
}

/* What each holds, from its first blocks */
LOCAL void test_probe( void )
{
	T_FSPROBE	pr;
	UB		dev[NAME_MAX];
	INT		v, i;

	if ( !have_disks ) KT_SKIP("no disks");
	/* the blank partition a copy went into last time is blank again */
	{
		UB	*z = (UB *)Kcalloc(1, 65536);
		SZ	asz;
		ID	dd;

		vol_dev(V_COPY, dev);
		dd = tk_opn_dev(dev, TD_UPDATE);
		KT_ASSERT(dd > 0 && z != NULL);
		if ( dd > 0 && z != NULL ) KT_ASSERT_ER(tk_swri_dev(dd, 0, z, 65536, &asz), E_OK);
		if ( dd > 0 ) tk_cls_dev(dd, 0);
		if ( z != NULL ) Kfree(z);
	}
	for ( v = 0; v < NVOL; v++ ) {
		vol_dev(v, dev);
		KT_ASSERT_ER(knl_fs_probe((CONST char *)dev, &pr), E_OK);
		tm_printf((UB *)"  %s: %s \"%s\" %d MB%s\n", dev, pr.name, pr.label, (INT)( pr.bytes >> 20 ),
			  pr.fimp[0] != 0 ? " (fimp)" : "");
		KT_ASSERT_EQ(pr.kind, vd[v].kind);
		if ( vd[v].label != NULL ) {
			KT_ASSERT_EQ(knl_strcmp((CONST char *)pr.label, vd[v].label), 0);
		}
		KT_ASSERT_EQ(( pr.fimp[0] != 0 ), is_fat(v) || vd[v].kind == FS_KIND_EXFAT || vd[v].kind == FS_KIND_NTFS);
	}
	/* a whole disk with a partition table says so */
	for ( i = 0; i < 3; i++ ) {
		KT_ASSERT_ER(knl_fs_probe((CONST char *)dk[i].name, &pr), E_OK);
		KT_ASSERT_EQ(pr.kind, FS_KIND_PARTED);
	}
}

/* Every TSFS volume attached and every FAT volume mounted, at once; the others refused */
LOCAL void test_all_on( void )
{
	COUNTS	c;
	INT	v, n;

	if ( !have_disks ) KT_SKIP("no disks");
	n = all_on();
	KT_ASSERT_EQ(n, 14);
	counts(&c);
	counts_say("all on", &c);
	KT_ASSERT(c.obf_used >= base.obf_used + 4);	/* and the system's: more than four */
	KT_ASSERT(c.fs_used >= base.fs_used + 10);		/* and /boot: more than eight */
	for ( v = 0; v < NVOL; v++ ) {
		if ( vd[v].kind == FS_KIND_BLANK || vd[v].kind == FS_KIND_TSFS ) {
			KT_ASSERT_ER(mount_auto(v, FS_MNT_RDONLY), EX_NOTSUP);
		}
	}
	/* the exFAT partition: changed through its device object, the fimp chosen by what is on it */
	for ( v = 0; v < NVOL; v++ ) {
		if ( vd[v].kind == FS_KIND_EXFAT ) {
			static UB	xd[3000];
			INT		i;

			for ( i = 0; i < (INT)sizeof(xd); i++ ) xd[i] = (UB)( i * 3 );
			KT_ASSERT_ER(mount_auto(v, 0), E_OK);
			KT_ASSERT_ER(file_put(v, "exfat test.bin", xd, sizeof(xd)), E_OK);
			KT_ASSERT(file_same(v, "exfat test.bin", xd, sizeof(xd)));
			KT_ASSERT_ER(unmount(v), E_OK);
			KT_ASSERT_ER(mount_auto(v, FS_MNT_RDONLY), E_OK);
			KT_ASSERT(file_same(v, "exfat test.bin", xd, sizeof(xd)));
			KT_ASSERT_ER(unmount(v), E_OK);
		}
	}
	/*
	 * The NTFS partition: its file read; on a volume that is safe, its
	 * bytes replaced where they are; what changes the structure refused
	 */
	for ( v = 0; v < NVOL; v++ ) {
		if ( vd[v].kind == FS_KIND_NTFS ) {
			static UB	nd[3000];
			char		path[64];
			INT		i, fd;

			for ( i = 0; i < (INT)sizeof(nd); i++ ) nd[i] = (UB)( i * 3 );
			KT_ASSERT_ER(mount_auto(v, FS_MNT_RDONLY), E_OK);
			KT_ASSERT(file_same(v, "VOLS.BIN", nd, sizeof(nd)));
			KT_ASSERT(file_same(v, "vols.bin", nd, sizeof(nd)));
			KT_ASSERT_ER(unmount(v), E_OK);

			KT_ASSERT_ER(mount_auto(v, 0), E_OK);
			media_path(v, path);
			knl_strcat(path, "/VOLS.BIN");
			fd = fs_open(path, O_RDWR);
			KT_ASSERT(fd >= 0);
			if ( fd >= 0 ) {
				for ( i = 0; i < 100; i++ ) nd[i + 700] = (UB)( 0xA5 ^ i );
				(void)fs_lseek(fd, 700, SEEK_SET_);
				KT_ASSERT_EQ(fs_write(fd, nd + 700, 100), 100);
				KT_ASSERT_EQ(fs_lseek(fd, 0, SEEK_END_), 3000);
				KT_ASSERT_ER(fs_write(fd, nd, 10), EX_NOTSUP);
				(void)fs_close(fd);
			}
			KT_ASSERT(file_same(v, "VOLS.BIN", nd, sizeof(nd)));
			media_path(v, path);
			knl_strcat(path, "/newdir");
			KT_ASSERT_ER(fs_mkdir(path), EX_NOTSUP);
			KT_ASSERT_ER(unmount(v), E_OK);
			KT_ASSERT_ER(mount_auto(v, FS_MNT_RDONLY), E_OK);
			KT_ASSERT(file_same(v, "VOLS.BIN", nd, sizeof(nd)));
			KT_ASSERT_ER(unmount(v), E_OK);

			/*
			 * The bytes put back as the image was made with them: the
			 * images are made once for the build's directory and plugged
			 * in again by every run after, which reads them as made
			 */
			for ( i = 0; i < 100; i++ ) nd[i + 700] = (UB)( ( i + 700 ) * 3 );
			KT_ASSERT_ER(mount_auto(v, 0), E_OK);
			media_path(v, path);
			knl_strcat(path, "/VOLS.BIN");
			fd = fs_open(path, O_RDWR);
			KT_ASSERT(fd >= 0);
			if ( fd >= 0 ) {
				(void)fs_lseek(fd, 700, SEEK_SET_);
				KT_ASSERT_EQ(fs_write(fd, nd + 700, 100), 100);
				(void)fs_close(fd);
			}
			KT_ASSERT_ER(unmount(v), E_OK);
			KT_ASSERT_ER(mount_auto(v, FS_MNT_RDONLY), E_OK);
			KT_ASSERT(file_same(v, "VOLS.BIN", nd, sizeof(nd)));
			KT_ASSERT_ER(unmount(v), E_OK);
		}
	}
	/* a FAT partition is no volume of objects; attached already, a volume is E_OK again */
	KT_ASSERT(attach(1) < E_OK);
	KT_ASSERT_ER(attach(0), E_OK);
}

/* An object on each volume of objects and a file on each FAT volume, read back */
LOCAL void test_rw( void )
{
	UB	data[300];
	INT	v, i;
	char	nm[16];

	if ( !have_disks ) KT_SKIP("no disks");
	for ( v = 0; v < NVOL; v++ ) {
		for ( i = 0; i < (INT)sizeof(data); i++ ) data[i] = (UB)( v * 7 + i );
		if ( vd[v].kind == FS_KIND_TSFS ) {
			KT_ASSERT_ER(obj_make(v, data, sizeof(data), &obj_of[v]), E_OK);
			KT_ASSERT(obj_same(&obj_of[v], data, sizeof(data)));
		} else if ( is_fat(v) ) {
			knl_strcpy(nm, "VOLS.BIN");
			KT_ASSERT_ER(file_put(v, nm, data, sizeof(data)), E_OK);
			KT_ASSERT(file_same(v, nm, data, sizeof(data)));
		}
	}
	(void)fs_sync();
}

/*
 * A link across volumes (design 8.3): it finds its object; its id names
 * the object's volume before the object, and only that of a link on
 * another volume; the object's count is the links on its own volume.
 */
LOCAL BOOL has_text( CONST UB *b, SZ n, CONST char *t )
{
	SZ	i, k;

	for ( i = 0; i < n; i++ ) {
		for ( k = 0; t[k] != 0 && i + k < n && b[i + k] == (UB)t[k]; k++ ) ;
		if ( t[k] == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL void test_across( void )
{
	UB	*tad = (UB *)Kmalloc(512), *buf;
	UB	path[32], dev[NAME_MAX], data[16];
	char	vs[TS_UUID_STRLEN + 1], ms[TS_UUID_STRLEN + 1], bs[TS_UUID_STRLEN + 1];
	TS_UUID	a, b, a2, vb, mine;
	T_TAD	*doc = NULL;
	T_VOBJ	*v = (T_VOBJ *)Kmalloc(sizeof(T_VOBJ));
	SZ	len;
	INT	r0, r1, n;

	if ( !have_disks ) KT_SKIP("no disks");
	if ( tad == NULL || v == NULL ) KT_SKIP("no memory");
	a = obj_of[0];				/* VOLA1 */
	b = obj_of[2];				/* VOLA3 */
	KT_ASSERT(knl_obfile_vol_of(&a) > 0 && knl_obfile_vol_of(&b) > 0);
	KT_ASSERT(knl_obfile_vol_of(&a) != knl_obfile_vol_of(&b));
	KT_ASSERT_ER(knl_obfile_path_of(&b, path, sizeof(path)), E_OK);
	vol_dev(2, dev);
	KT_ASSERT_EQ(knl_strcmp((CONST char *)path, (CONST char *)dev), 0);
	KT_ASSERT_ER(knl_obfile_vuuid_of(&b, &vb), E_OK);
	(void)ts_uuid_to_str(&vb, vs, sizeof(vs));
	(void)ts_uuid_to_str(&b, bs, sizeof(bs));

	/* across: not counted on the volume of the link; counted on b's, by the record it keeps of it */
	r0 = obj_refs(&b);
	n = tad_with(tad, 512, &b);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(obj_refs(&b), r0 + 1);

	/* the id made to name the volume, once; and bare again seen from that volume */
	buf = (UB *)Kmalloc((SZ)n + 1);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;
	knl_memcpy(buf, tad, n);
	len = (SZ)n;
	KT_ASSERT_EQ(om_link_fix(&buf, &len, &a), 1);
	KT_ASSERT(has_text(buf, len, vs));
	KT_ASSERT_EQ(om_link_fix(&buf, &len, &a), 0);
	KT_ASSERT_ER(tad_parse(buf, len, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_ER(tad_lnk_get(doc, 0, v), E_OK);
		KT_ASSERT_EQ(ts_uuid_cmp(&v->target, &b), 0);
		KT_ASSERT_EQ(ts_uuid_cmp(&v->tvol, &vb), 0);
		tad_free(doc);
		doc = NULL;
	}
	/* written so: the native store still takes it for a link to b, the same one */
	KT_ASSERT_ER(rec_put(&a, buf, len), E_OK);
	KT_ASSERT_EQ(obj_refs(&b), r0 + 1);
	KT_ASSERT_EQ(om_link_fix(&buf, &len, &b), 1);
	KT_ASSERT(!has_text(buf, len, vs));
	Kfree(buf);

	/* the machine before the volume: this machine's is taken for none */
	KT_ASSERT_ER(knl_obfile_machine(&mine), E_OK);
	(void)ts_uuid_to_str(&mine, ms, sizeof(ms));
	knl_strcpy((char *)tad, "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p><link id=\"");
	knl_strcat((char *)tad, ms);
	knl_strcat((char *)tad, "/");
	knl_strcat((char *)tad, vs);
	knl_strcat((char *)tad, "/");
	knl_strcat((char *)tad, bs);
	knl_strcat((char *)tad, "_0.xtad\"/></p></document></tad>");
	len = knl_strlen((char *)tad);
	KT_ASSERT_ER(tad_parse(tad, len, NULL, &doc), E_OK);
	if ( doc != NULL ) {
		KT_ASSERT_ER(tad_lnk_get(doc, 0, v), E_OK);
		KT_ASSERT_EQ(ts_uuid_cmp(&v->target, &b), 0);
		KT_ASSERT_EQ(ts_uuid_cmp(&v->tvol, &vb), 0);
		KT_ASSERT_EQ(ts_uuid_cmp(&v->tmach, &mine), 0);
		tad_free(doc);
	}
	buf = (UB *)Kmalloc(len + 1);
	if ( buf != NULL ) {
		knl_memcpy(buf, tad, (INT)len);
		KT_ASSERT_EQ(om_link_fix(&buf, &len, &b), 1);	/* the same volume: bare */
		KT_ASSERT(!has_text(buf, len, ms) && !has_text(buf, len, vs) && has_text(buf, len, bs));
		Kfree(buf);
	}

	/* on the same volume: counted there, the link taken away and the object thrown away */
	r1 = r0;
	knl_memcpy(data, "a2a2a2a2a2a2a2a2", 16);
	KT_ASSERT_ER(obj_make(0, data, 16, &a2), E_OK);
	r0 = obj_refs(&a2);
	n = tad_with(tad, 512, &a2);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(obj_refs(&a2), r0 + 1);
	KT_ASSERT_EQ(obj_refs(&b), r1);		/* the link to b went: its record with it */
	n = tad_with(tad, 512, NULL);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(obj_refs(&a2), r0);
	n = tad_with(tad, 512, &a2);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(obj_refs(&a2), r0 + 1);
	KT_ASSERT_ER(ob_del_obj(&a), E_OK);
	KT_ASSERT_EQ(obj_refs(&a2), r0);
	(void)ob_del_obj(&a2);
	{
		UB	d2[300];
		INT	i;

		for ( i = 0; i < (INT)sizeof(d2); i++ ) d2[i] = (UB)i;
		KT_ASSERT_ER(obj_make(0, d2, sizeof(d2), &obj_of[0]), E_OK);
	}
	Kfree(tad);
	Kfree(v);
}

/*
 * The record of links from other volumes (design 8.3): kept as links
 * come and go; put right at attaching for what changed while the volume
 * was away; an object only another volume links to is no garbage; the
 * check finds an entry of an object that is not there and the repair
 * takes it away.
 */
LOCAL INT xref_count( ID vol, CONST TS_UUID *from, CONST TS_UUID *to )
{
	T_OBXREF	*x = (T_OBXREF *)Kmalloc(sizeof(T_OBXREF) * 64);
	INT		n, i, c = 0;

	if ( x == NULL ) return -1;
	n = knl_obxref_list(vol, x, 64);
	for ( i = 0; i < n; i++ ) {
		if ( ts_uuid_cmp(&x[i].from_obj, from) == 0 && ts_uuid_cmp(&x[i].target, to) == 0 ) {
			c += x[i].count;
		}
	}
	Kfree(x);
	return c;
}

LOCAL BOOL in_gc( ID vol, CONST TS_UUID *u )
{
	TS_UUID	buf[32], from;
	INT	got, i;
	BOOL	first = TRUE;

	for (;;) {
		got = 0;
		if ( ts_lst_gc(vol, first ? NULL : &from, buf, 32, &got) < E_OK || got <= 0 ) {
			return FALSE;
		}
		for ( i = 0; i < got; i++ ) {
			if ( ts_uuid_cmp(&buf[i], u) == 0 ) return TRUE;
		}
		first = FALSE;
		from = buf[got - 1];
		if ( got < 32 ) return FALSE;
	}
}

LOCAL void test_xref( void )
{
	UB		*tad = (UB *)Kmalloc(512), dev[NAME_MAX], data[16];
	TS_UUID		a, b, c, va, fake;
	T_TSFSCK	*res = (T_TSFSCK *)Kmalloc(sizeof(T_TSFSCK));
	ID		vc;
	INT		r0, rc, n;

	if ( !have_disks ) KT_SKIP("no disks");
	if ( tad == NULL || res == NULL ) KT_SKIP("no memory");
	a = obj_of[0];				/* VOLA1 */
	b = obj_of[13];				/* VOLC1 */
	vc = knl_obfile_vol_of(&b);
	KT_ASSERT(knl_obfile_vol_of(&a) > 0 && vc > 0);
	KT_ASSERT_ER(knl_obfile_vuuid_of(&a, &va), E_OK);
	r0 = obj_refs(&b);

	/* a link made and given up: the record and the count follow */
	n = tad_with(tad, 512, &b);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(xref_count(vc, &a, &b), 1);
	KT_ASSERT_EQ(obj_refs(&b), r0 + 1);
	n = tad_with(tad, 512, NULL);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(xref_count(vc, &a, &b), 0);
	KT_ASSERT_EQ(obj_refs(&b), r0);

	/* given up while VOLC1 is away, made while it is away: put right as it comes back */
	n = tad_with(tad, 512, &b);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(obj_refs(&b), r0 + 1);
	KT_ASSERT_ER(detach(13), E_OK);
	n = tad_with(tad, 512, NULL);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_ER(attach(13), E_OK);
	vc = knl_obfile_vol_of(&b);
	KT_ASSERT_EQ(xref_count(vc, &a, &b), 0);
	KT_ASSERT_EQ(obj_refs(&b), r0);
	KT_ASSERT_ER(detach(13), E_OK);
	n = tad_with(tad, 512, &b);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_ER(attach(13), E_OK);
	vc = knl_obfile_vol_of(&b);
	KT_ASSERT_EQ(xref_count(vc, &a, &b), 1);
	KT_ASSERT_EQ(obj_refs(&b), r0 + 1);

	/* an object only another volume links to is not garbage */
	knl_memcpy(data, "cccccccccccccccc", 16);
	KT_ASSERT_ER(obj_make(13, data, 16, &c), E_OK);
	rc = obj_refs(&c);
	KT_ASSERT_EQ(rc, 0);
	n = tad_with(tad, 512, &c);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(obj_refs(&c), 1);
	KT_ASSERT(!in_gc(vc, &c));
	KT_ASSERT_EQ(obj_refs(&b), r0);		/* a links to c now, not to b */

	/* the check: nothing wrong; then an entry of an object VOLA1 does not have */
	vol_dev(13, dev);
	knl_memset(res, 0, sizeof(*res));
	KT_ASSERT_ER(ob_chk_run((CONST char *)dev, TSFSCK_QUICK, res, NULL, NULL), E_OK);
	KT_ASSERT_EQ(res->found[TSFSCK_K_XREF], 0);
	KT_ASSERT_EQ(res->found[TSFSCK_K_HIDDEN], 0);
	KT_ASSERT_ER(ts_gen_uuid(&fake), E_OK);
	KT_ASSERT_ER(knl_obxref_move(&va, &fake, &b, 1), E_OK);
	KT_ASSERT_EQ(obj_refs(&b), r0 + 1);
	knl_memset(res, 0, sizeof(*res));
	KT_ASSERT_ER(ob_chk_run((CONST char *)dev, TSFSCK_QUICK, res, NULL, NULL), E_OK);
	KT_ASSERT_EQ(res->found[TSFSCK_K_XREF], 1);
	knl_memset(res, 0, sizeof(*res));
	KT_ASSERT_ER(ob_chk_run((CONST char *)dev, TSFSCK_QUICK | TSFSCK_REPAIR, res, NULL, NULL), E_OK);
	KT_ASSERT_EQ(res->fixed[TSFSCK_K_XREF], 1);
	vc = knl_obfile_vol_of(&b);
	KT_ASSERT(vc > 0);
	KT_ASSERT_EQ(xref_count(vc, &fake, &b), 0);
	KT_ASSERT_EQ(obj_refs(&b), r0);
	KT_ASSERT_EQ(xref_count(vc, &a, &c), 1);
	knl_memset(res, 0, sizeof(*res));
	KT_ASSERT_ER(ob_chk_run((CONST char *)dev, TSFSCK_QUICK, res, NULL, NULL), E_OK);
	KT_ASSERT_EQ(res->found[TSFSCK_K_XREF], 0);

	/* the link given up: c is garbage again */
	n = tad_with(tad, 512, NULL);
	KT_ASSERT_ER(rec_put(&a, tad, n), E_OK);
	KT_ASSERT_EQ(obj_refs(&c), 0);
	KT_ASSERT(in_gc(vc, &c));
	(void)ob_del_obj(&c);
	Kfree(tad);
	Kfree(res);
}

/* A UUID is one object's on every volume attached */
LOCAL void test_unique( void )
{
	T_OBCRE		c;
	T_OBFVREF	ref, ref2;
	TS_UUID		u;
	UB		dev[NAME_MAX], src[NAME_MAX], *buf;
	SZ		asz;
	ID		di, dd;
	INT		off;
	ER		er = E_OK;

	if ( !have_disks ) KT_SKIP("no disks");
	vol_dev(0, dev);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.vol = (CONST char *)dev;
	c.uuid = obj_of[2];			/* the UUID of one on VOLA3 */
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OBJ);

	/* a copy of VOLC1 made into the blank partition beside it is not attached with it */
	vol_dev(13, src);
	KT_ASSERT_ER(detach(13), E_OK);
	buf = (UB *)Kmalloc(65536);
	di = tk_opn_dev(src, TD_READ);
	vol_dev(V_COPY, dev);
	dd = tk_opn_dev(dev, TD_UPDATE);
	KT_ASSERT(buf != NULL && di > 0 && dd > 0);
	for ( off = 0; buf != NULL && di > 0 && dd > 0 && off < 6 * 2048 && er >= E_OK; off += 128 ) {
		er = tk_srea_dev(di, off, buf, 65536, &asz);
		if ( er >= E_OK ) er = tk_swri_dev(dd, off, buf, 65536, &asz);
	}
	KT_ASSERT(er >= E_OK);
	if ( di > 0 ) tk_cls_dev(di, 0);
	if ( dd > 0 ) tk_cls_dev(dd, 0);
	if ( buf != NULL ) Kfree(buf);
	KT_ASSERT_ER(attach(13), E_OK);
	KT_ASSERT_ER(attach(V_COPY), E_OBJ);
	KT_ASSERT_ER(knl_obfile_vol_ref((CONST char *)src, &ref), E_OK);
	KT_ASSERT(ref.native && !ref.system);
	KT_ASSERT(knl_obfile_vol_ref((CONST char *)dev, &ref2) < E_OK);
	/* VOLC1 detached, the copy goes in its place, and the original is refused */
	KT_ASSERT_ER(detach(13), E_OK);
	KT_ASSERT_ER(attach(V_COPY), E_OK);
	KT_ASSERT_ER(attach(13), E_OBJ);
	KT_ASSERT(knl_obfile_vol_of(&obj_of[13]) == knl_obfile_vol_path((CONST char *)dev));
	KT_ASSERT_ER(detach(V_COPY), E_OK);
	KT_ASSERT_ER(attach(13), E_OK);
}

/* A volume with an object open is not detached */
LOCAL void test_busy( void )
{
	ID	key;

	if ( !have_disks ) KT_SKIP("no disks");
	key = ob_opn_obj(&obj_of[0], OB_OP_R);
	KT_ASSERT(key > 0);
	KT_ASSERT_ER(detach(0), E_BUSY);
	KT_ASSERT(knl_obfile_vol_of(&obj_of[0]) > 0);
	if ( key > 0 ) ob_cls_obj(key);
	KT_ASSERT_ER(detach(0), E_OK);
	KT_ASSERT(knl_obfile_vol_of(&obj_of[0]) < 0);
	KT_ASSERT_ER(attach(0), E_OK);
	KT_ASSERT(knl_obfile_vol_of(&obj_of[0]) > 0);
}

/* The disk pulled out with an object and a file open on it, and put back */
LOCAL void test_yank( void )
{
	COUNTS	c0, c1;
	UB	data[300], b[64];
	SZ	asz = 0;
	char	path[64];
	INT	fd, i;
	ID	key;

	if ( !have_disks ) KT_SKIP("no disks");
	counts(&c0);
	key = ob_opn_obj(&obj_of[12], OB_OP_R);		/* VOLB, in the chain of logical partitions */
	media_path(5, path);
	knl_strcat(path, "/VOLS.BIN");
	fd = fs_open(path, O_RDONLY);
	KT_ASSERT(key > 0 && fd >= 0);
	unplug(&dk[1]);
	for ( i = 0; i < WAIT_MS / 100 && knl_obfile_vol_of(&obj_of[12]) > 0; i++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(knl_obfile_vol_of(&obj_of[12]) < 0);	/* its objects are found no more */
	if ( key > 0 ) {
		KT_ASSERT(ob_rea_rec(key, 0, 0, b, sizeof(b), &asz) < E_OK);
		ob_cls_obj(key);
	}
	if ( fd >= 0 ) {
		KT_ASSERT_ER(fs_read(fd, b, sizeof(b)), EX_IO);
		KT_ASSERT_ER(fs_close(fd), EX_IO);
	}
	/* closed, the device leaves */
	for ( i = 0; i < WAIT_MS / 100 && listed(dk[1].name); i++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(!listed(dk[1].name));
	dk[1].in = FALSE;
	counts(&c1);
	counts_say("pulled out", &c1);
	KT_ASSERT_EQ(c1.obf_used, c0.obf_used - 1);
	KT_ASSERT_EQ(c1.ts_used, c0.ts_used - 1);
	KT_ASSERT_EQ(c1.blk_used, c0.blk_used - 1);
	KT_ASSERT_EQ(c1.fs_used, c0.fs_used - 7);
	/* put back: attached again, its object as it was */
	KT_ASSERT(come(&dk[1]));
	KT_ASSERT_ER(attach(12), E_OK);
	{
		T_OBREF	r;

		tm_printf((UB *)"  back: the object %d\n", (INT)ob_ref_obj(&obj_of[12], &r));
	}
	for ( i = 0; i < (INT)sizeof(data); i++ ) data[i] = (UB)( 12 * 7 + i );
	KT_ASSERT(obj_same(&obj_of[12], data, sizeof(data)));
	for ( i = 5; i <= 11; i++ ) {
		KT_ASSERT_ER(mount_auto(i, 0), E_OK);
	}
}

/* Again and again: nothing is left behind */
LOCAL void test_cycles( void )
{
	COUNTS	c0, c1;
	UB	data[300];
	INT	round, i, v;

	if ( !have_disks ) KT_SKIP("no disks");
	for ( round = 0; round < 4; round++ ) {
		KT_ASSERT_EQ(all_off(), 14);
		for ( i = 0; i < NDISK; i++ ) KT_ASSERT(go(&dk[i]));
		tk_dly_tsk(500);
		if ( round == 1 ) {
			counts(&c0);			/* after one round, what stays made is made */
			counts_say("out, round 1", &c0);
		}
		if ( round == 3 ) {
			counts(&c1);
			counts_say("out, round 3", &c1);
		}
		for ( i = 0; i < NDISK; i++ ) KT_ASSERT(come(&dk[i]));
		KT_ASSERT_EQ(all_on(), 14);
		for ( v = 0; v < NVOL; v++ ) {
			for ( i = 0; i < (INT)sizeof(data); i++ ) data[i] = (UB)( v * 7 + i + round );
			if ( vd[v].kind == FS_KIND_TSFS && v != V_COPY ) {
				KT_ASSERT_ER(rec_put(&obj_of[v], data, sizeof(data)), E_OK);
				KT_ASSERT(obj_same(&obj_of[v], data, sizeof(data)));
			} else if ( is_fat(v) ) {
				KT_ASSERT_ER(file_put(v, "VOLS.BIN", data, sizeof(data)), E_OK);
			}
		}
	}
	KT_ASSERT_EQ(c1.fs_made, c0.fs_made);
	KT_ASSERT_EQ(c1.obf_made, c0.obf_made);
	KT_ASSERT_EQ(c1.ts_made, c0.ts_made);
	KT_ASSERT_EQ(c1.blk_made, c0.blk_made);
	KT_ASSERT_EQ(c1.fs_used, base.fs_used);
	KT_ASSERT_EQ(c1.obf_used, base.obf_used);
	KT_ASSERT_EQ(c1.opn_free, c0.opn_free);
	KT_ASSERT_EQ(c1.opn_free, base.opn_free);
	/* the heap: what two rounds more left over, a few blocks at the most */
	tm_printf((UB *)"  heap grew %d bytes in %d areas over two rounds\n", (INT)( c1.heap - c0.heap ),
		  c1.areas - c0.areas);
	KT_ASSERT(c1.heap - c0.heap < 64 * 1024);

	/* attached and detached alone, the disks staying in: nothing at all is left over */
	for ( round = 0; round < 3; round++ ) {
		if ( round == 1 ) counts(&c0);
		KT_ASSERT_EQ(all_off(), 14);
		KT_ASSERT_EQ(all_on(), 14);
	}
	counts(&c1);
	tm_printf((UB *)"  heap grew %d bytes in %d areas over two rounds of attaching alone\n",
		  (INT)( c1.heap - c0.heap ), c1.areas - c0.areas);
	KT_ASSERT(c1.heap - c0.heap < 4096);
	{
		INT	kind, v;

		for ( kind = 0; kind < 2; kind++ ) {
			for ( round = 0; round < 3; round++ ) {
				if ( round == 1 ) counts(&c0);
				for ( v = 0; v < NVOL; v++ ) {
					if ( kind == 0 && vd[v].kind == FS_KIND_TSFS ) {
						(void)detach(v);
						(void)attach(v);
					} else if ( kind == 1 && is_fat(v) ) {
						(void)unmount(v);
						(void)mount_auto(v, 0);
					}
				}
			}
			counts(&c1);
			tm_printf((UB *)"  %s alone: heap grew %d bytes in %d areas over two rounds\n",
				  kind == 0 ? "tsfs" : "fat", (INT)( c1.heap - c0.heap ), c1.areas - c0.areas);
		}
	}
}

/* Each step of attaching and mounting alone, ten times: no memory stays behind */
IMPORT void knl_dt_zone_load( void );
IMPORT void knl_obdev_volume_up( void );
IMPORT ER   knl_obfile_attach( CONST char *path, UINT flags );
IMPORT ER   knl_obfile_detach( CONST char *path );

/*
 * The heap counted is the whole kernel's: in a run of every suite, tasks
 * the suites before this one left (the stores' writing back, the network,
 * the programs' threads) take and give back memory of their own while a
 * step is repeated. Each count is therefore made once the heap has stayed
 * the same for a while, and the ten repeats are measured afresh up to
 * three times: memory a step itself keeps is left over every time, ten
 * times what one repeat keeps, while what another task held at one count
 * is gone at the next.
 */
#define LEAK_TRIES	3
#define LEAK_QUIET_MS	500		/* the heap unchanged this long */
#define LEAK_QUIET_MAX	3000		/* waited for at the most */

LOCAL void counts_quiet( COUNTS *c )
{
	COUNTS	prev;
	INT	t, still = 0;

	counts(&prev);
	for ( t = 0; t < LEAK_QUIET_MAX; t += 50 ) {
		tk_dly_tsk(50);
		counts(c);
		if ( c->heap == prev.heap && c->areas == prev.areas ) {
			still += 50;
			if ( still >= LEAK_QUIET_MS ) return;
		} else {
			still = 0;
			prev = *c;
		}
	}
	tm_printf((UB *)"  (the heap did not stay still for %d ms in %d ms)\n", LEAK_QUIET_MS, LEAK_QUIET_MAX);
}

LOCAL void leak_step( CONST char *what, INT step )
{
	COUNTS		c0, c1;
	T_FSPROBE	pr;
	TS_UUID		u;
	UB		fat[NAME_MAX], ts[NAME_MAX];
	INT		i, try;
	SZ		left = 0;
	ID		key;

	vol_dev(1, fat);
	vol_dev(0, ts);
	(void)knl_obdev_uuid(fat, &u);
	for ( try = 0; try < LEAK_TRIES; try++ ) {
		for ( i = 0; i < 12; i++ ) {
			if ( i == 2 ) counts_quiet(&c0);	/* the first two make what stays made */
			switch ( step ) {
			case 0:	(void)knl_fs_probe((CONST char *)fat, &pr); break;
			case 1:	key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
				if ( key > 0 ) ob_cls_obj(key);
				break;
			case 2:	(void)knl_fs_detach_media((CONST char *)fat);
				(void)knl_fs_attach_media("fatfs", (CONST char *)fat, 0);
				break;
			case 3:	(void)knl_obfile_detach((CONST char *)ts);
				(void)knl_obfile_attach((CONST char *)ts, TSFS_STORE_BLK);
				break;
			case 4:	knl_dt_zone_load(); break;
			case 5:	knl_obdev_volume_up(); break;
			default: (void)unmount(1); (void)mount_auto(1, 0); break;
			}
		}
		counts_quiet(&c1);
		left = c1.heap - c0.heap;
		tm_printf((UB *)"  %s, ten times: %d bytes in %d areas left over%s\n", what, (INT)left,
			  c1.areas - c0.areas, ( left < 1024 || try + 1 == LEAK_TRIES ) ? "" : "; again");
		if ( left < 1024 ) break;
	}
	KT_ASSERT(left < 1024);
}

LOCAL void test_leak( void )
{
	if ( !have_disks ) KT_SKIP("no disks");
	leak_step("probe", 0);
	leak_step("open", 1);
	leak_step("fs mount", 2);
	leak_step("obfile attach", 3);
	leak_step("zone", 4);
	leak_step("volume up", 5);
	leak_step("mount auto", 6);
}

/* ---------------------------------------------------------------- four at once */

typedef struct {
	INT	v;			/* the volume it works on */
	INT	rounds;
	INT	errors;
	ID	done;			/* a flag set when it is through */
} WORK;

LOCAL WORK	work[4];

LOCAL void worker( INT stacd, void *exinf )
{
	WORK	*w = (WORK *)exinf;
	UB	*data = (UB *)Kmalloc(8192);
	TS_UUID	u;
	INT	r, i;

	(void)stacd;
	for ( r = 0; data != NULL && r < w->rounds; r++ ) {
		for ( i = 0; i < 8192; i++ ) data[i] = (UB)( i * 3 + r + w->v );
		if ( vd[w->v].kind == FS_KIND_TSFS ) {
			if ( obj_make(w->v, data, 8192, &u) < E_OK || !obj_same(&u, data, 8192) ) w->errors++;
			if ( ob_del_obj(&u) < E_OK ) w->errors++;
			if ( r % 4 == 3 && ( detach(w->v) < E_OK || attach(w->v) < E_OK ) ) w->errors++;
		} else {
			if ( file_put(w->v, "SMP.BIN", data, 8192) < E_OK || !file_same(w->v, "SMP.BIN", data, 8192) ) {
				w->errors++;
			}
			if ( r % 4 == 3 && ( unmount(w->v) < E_OK || mount_auto(w->v, 0) < E_OK ) ) w->errors++;
		}
	}
	if ( data == NULL ) w->errors++;
	else Kfree(data);
	tk_set_flg(w->done, 1);
	tk_exd_tsk();
}

LOCAL void test_smp( void )
{
	CONST INT	vols[4] = { 0, 2, 12, 15 };	/* three volumes of objects on two disks, a FAT one */
	T_CFLG		cf;
	T_CTSK		ct;
	T_TSFSCK	*res;
	UINT		ptn;
	INT		i, errors = 0;
	ID		tid;

	if ( !have_disks ) KT_SKIP("no disks");
	cf.exinf = NULL;
	cf.flgatr = TA_TFIFO | TA_WMUL;
	cf.iflgptn = 0;
	for ( i = 0; i < 4; i++ ) {
		work[i].v = vols[i];
		work[i].rounds = 16;
		work[i].errors = 0;
		work[i].done = tk_cre_flg(&cf);
		ct.exinf = &work[i];
		ct.tskatr = TA_HLNG | TA_RNG0;
		ct.task = (FP)worker;
		ct.itskpri = KT_PRI_MAIN + 1;
		ct.stksz = VOL_STACK;
		ct.assprc = 0;
		tid = tk_cre_tsk(&ct);
		KT_ASSERT(tid > 0 && work[i].done > 0);
		if ( tid > 0 ) tk_sta_tsk(tid, 0);
	}
	for ( i = 0; i < 4; i++ ) {
		if ( work[i].done > 0 ) {
			KT_ASSERT_ER(tk_wai_flg(work[i].done, 1, TWF_ORW, &ptn, 300000), E_OK);
			tk_del_flg(work[i].done);
		}
		errors += work[i].errors;
	}
	tm_printf((UB *)"  four at once: %d errors\n", errors);
	KT_ASSERT_EQ(errors, 0);
	/* every volume of objects whole afterwards */
	res = (T_TSFSCK *)Kmalloc(sizeof(T_TSFSCK));
	for ( i = 0; res != NULL && i < NVOL; i++ ) {
		UB	dev[NAME_MAX];

		if ( vd[i].kind != FS_KIND_TSFS || i == V_COPY ) continue;
		vol_dev(i, dev);
		KT_ASSERT_ER(ob_chk_run((CONST char *)dev, TSFSCK_FULL, res, NULL, NULL), E_OK);
		tm_printf((UB *)"  %s checked: %d problems\n", dev, res->problems);
		KT_ASSERT_EQ(res->problems, 0);
	}
	if ( res != NULL ) Kfree(res);
}

/* Everything off and the disks out: the machine as the other suites know it */
LOCAL void test_out( void )
{
	COUNTS	c;
	INT	i;

	if ( !have_disks ) KT_SKIP("no disks");
	(void)all_off();
	for ( i = 0; i < NDISK; i++ ) {
		if ( dk[i].in ) KT_ASSERT(go(&dk[i]));
	}
	counts(&c);
	counts_say("after", &c);
	KT_ASSERT_EQ(c.fs_used, base.fs_used);
	KT_ASSERT_EQ(c.obf_used, base.obf_used);
	KT_ASSERT_EQ(c.opn_free, base.opn_free);
}

/* ---------------------------------------------------------------- more than 26 disks, more than 16 partitions */

LOCAL INT disk_count( void )
{
	INT	n = knl_obdev_disks(names, LIST_MAX), i, k = 0;

	for ( i = 0; i < n; i++ ) {
		if ( is_whole(names + i * NAME_MAX) ) k++;
	}
	return k;
}

/* A hub plugged in on a port of another, its ports where the disks go */
LOCAL void hub_plug( CONST char *id, CONST char *port )
{
	tm_printf((UB *)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":{\"driver\":\"usb-hub\","
		  "\"id\":\"%s\",\"bus\":\"xhci.0\",\"port\":\"%s\"}}\n", id, port);
	tk_dly_tsk(500);
}

LOCAL void any_plug( CONST char *id, CONST char *file, CONST char *port )
{
	tm_printf((UB *)"KTEST QMP {\"execute\":\"human-monitor-command\",\"arguments\":{\"command-line\":"
		  "\"drive_add 0 if=none,id=d%s,file=%s/%s,format=raw\"}}\n", id, KT_VOL_DIR, file);
	tk_dly_tsk(200);
	tm_printf((UB *)"KTEST QMP {\"execute\":\"device_add\",\"arguments\":{\"driver\":\"usb-storage\","
		  "\"id\":\"%s\",\"bus\":\"xhci.0\",\"port\":\"%s\",\"drive\":\"d%s\",\"removable\":true}}\n",
		  id, port, id);
}

LOCAL void any_unplug( CONST char *id )
{
	tm_printf((UB *)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"%s\"}}\n", id);
	tk_dly_tsk(150);
}

/*
 * Twenty-two small disks on hubs below hubs, with the machine's own more
 * than 26: the names go on past "udz" with two letters. A disk of forty
 * partitions has every one as a subunit, the fortieth a FAT volume that
 * mounts.
 */
LOCAL void test_many( void )
{
	T_FSPROBE	pr;
	DISK		p = { "volp", "vdkp", "volp.img", "4.4.7", 30ULL << 20, 40, "", FALSE };
	char		id[8], file[16], port[16];
	UB		dev[NAME_MAX];
	INT		before, i, t, two = 0;
	CONST char	*hubport[3] = { "4.4", "4.4.8", "4.4.8.8" };

	if ( !have_disks ) KT_SKIP("no disks");
	before = disk_count();
	hub_plug("hubv1", hubport[0]);
	hub_plug("hubv2", hubport[1]);
	hub_plug("hubv3", hubport[2]);
	for ( i = 0; i < 22; i++ ) {
		knl_strcpy(id, "vx");
		id[2] = (char)( '0' + i / 10 );
		id[3] = (char)( '0' + i % 10 );
		id[4] = 0;
		knl_strcpy(file, "volx");
		file[4] = id[2];
		file[5] = id[3];
		file[6] = 0;
		knl_strcat(file, ".img");
		/* seven on each of the first two hubs, eight on the last */
		knl_strcpy(port, hubport[( i < 7 ) ? 0 : ( i < 14 ) ? 1 : 2]);
		knl_strcat(port, ".");
		{
			INT	k = (INT)knl_strlen(port), q = ( i < 14 ) ? i % 7 + 1 : i - 14 + 1;

			port[k] = (char)( '0' + q );
			port[k + 1] = 0;
		}
		if ( i == 6 ) continue;			/* the place of the disk of many partitions */
		any_plug(id, file, port);
	}
	KT_ASSERT(come(&p));
	for ( t = 0; t < WAIT_MS / 100 && disk_count() < before + 22; t++ ) {
		tk_dly_tsk(100);
	}
	tm_printf((UB *)"  disks: %d before, %d now\n", before, disk_count());
	KT_ASSERT(disk_count() >= before + 22);
	KT_ASSERT(disk_count() > 26);
	{
		INT	n = knl_obdev_disks(names, LIST_MAX);

		for ( i = 0; i < n; i++ ) {
			UB	*nm = names + i * NAME_MAX;

			if ( is_whole(nm) && knl_strlen((CONST char *)nm) == 4 ) {
				if ( two == 0 ) tm_printf((UB *)"  a name of two letters: %s\n", nm);
				two++;
			}
		}
	}
	KT_ASSERT(two > 0);

	/* the fortieth partition: there, a FAT volume, mounted and read */
	knl_strcpy((char *)dev, (CONST char *)p.name);
	knl_strcat((char *)dev, "39");
	KT_ASSERT(listed(dev));
	KT_ASSERT_ER(knl_fs_probe((CONST char *)dev, &pr), E_OK);
	KT_ASSERT_EQ(pr.kind, FS_KIND_FAT12);
	{
		TS_UUID	u;
		ID	key;
		T_FSTAT	st;
		char	path[40];

		KT_ASSERT_ER(knl_obdev_uuid(dev, &u), E_OK);
		key = ob_opn_obj(&u, OB_OP_R);
		KT_ASSERT(key > 0);
		if ( key > 0 ) {
			KT_ASSERT_ER(fs_attach_dev(key, NULL, FS_MNT_RDONLY), E_OK);
			knl_strcpy(path, FS_MEDIA_DIR "/");
			knl_strcat(path, (CONST char *)dev);
			KT_ASSERT_ER(fs_stat(path, &st), E_OK);
			KT_ASSERT_ER(fs_detach_dev(key), E_OK);
			ob_cls_obj(key);
		}
	}

	/* all out again */
	for ( i = 0; i < 22; i++ ) {
		if ( i == 6 ) continue;
		knl_strcpy(id, "vx");
		id[2] = (char)( '0' + i / 10 );
		id[3] = (char)( '0' + i % 10 );
		id[4] = 0;
		any_unplug(id);
	}
	KT_ASSERT(go(&p));
	any_unplug("hubv3");
	any_unplug("hubv2");
	any_unplug("hubv1");
	for ( t = 0; t < WAIT_MS / 100 && disk_count() > before; t++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT_EQ(disk_count(), before);
}

EXPORT void ktest_vols( void )
{
	names = (UB *)Kmalloc((SZ)NAME_MAX * LIST_MAX);
	if ( names == NULL ) return;
	KT_RUN(test_plug);
	KT_RUN(test_probe);
	KT_RUN(test_all_on);
	KT_RUN(test_rw);
	KT_RUN(test_across);
	KT_RUN(test_xref);
	KT_RUN(test_unique);
	KT_RUN(test_busy);
	KT_RUN(test_yank);
	KT_RUN(test_leak);
	KT_RUN(test_cycles);
	KT_RUN(test_smp);
	KT_RUN(test_out);
	KT_RUN(test_many);
	Kfree(names);
}
