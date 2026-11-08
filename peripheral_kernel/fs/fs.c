/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fs.c
 *	File management, common layer (design 12.2).
 *
 *	Keeps the fimp registry, the mount table and the descriptors, and
 *	routes each call to the fimp of the mount whose mount point is the
 *	longest prefix of the path. The path handed to the fimp is the rest
 *	of the path below that mount point, always starting with '/'.
 *
 *	A mount is not taken down while a descriptor is open on it or a
 *	call is at work on it through a path (its busy count), so a
 *	process that detaches cannot pull a volume from under another.
 *
 *	When its device or medium goes, a mount is marked gone instead:
 *	paths no longer find it, its descriptors answer EX_IO without
 *	reaching the fimp, and it is let go -- the fimp's state freed, the
 *	device closed, the mount point free again -- once the last of
 *	them is closed and no call is at work on it.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/ob.h>
#include <ts/psx.h>
#include <ts/time.h>

IMPORT INT knl_obfile_shut( BOOL finish );	/* obfile.c: the volumes of objects, the machine going off */

#define FS_MAX_FIMP	16

LOCAL CONST T_FIMP	*fs_fimp[FS_MAX_FIMP];
LOCAL INT		fs_nfimp = 0;

/*
 * The mount table: an entry is made the first time one more mount is
 * wanted than there are entries, and is kept, free or in use, for the
 * life of the system. A descriptor points at its mount, and a pointer
 * that stays good after the mount is let go is what makes a descriptor
 * on a mount whose device went safe to look at. fs_nmount is how many
 * entries have been made; both are looked at under fs_mtxid.
 */
LOCAL T_MOUNT		*fs_mount[FS_MAX_MOUNT];
LOCAL INT		fs_nmount = 0;
LOCAL ID		fs_mtxid = 0;		/* mount table and descriptors */

/*
 * Descriptors and open files (design 5.8.2). A descriptor is a slot
 * with its owner that names an open file; the open file holds the
 * offset and the flags, as POSIX's open file description, and is shared
 * by the descriptors fs_dup makes and those handed to a child
 * (knl_fs_give). An open file goes -- the fimp's close -- once no
 * descriptor names it and no call is at work on it. Both tables are
 * looked at under fs_mtxid.
 */
typedef struct {
	T_FILE	*f;
	ID	owner;			/* the process, 0 for the kernel */
	BOOL	used;
} FSLOT;

LOCAL FSLOT		fs_slot[FS_MAX_FILE];
LOCAL T_FILE		fs_file[FS_MAX_FILE];
LOCAL ID		fs_flgid = 0;		/* knl_fs_wake: set and cleared at once */

/* Pipes are open files of a mount that no path reaches (pipefs.c) */
LOCAL T_MOUNT		fs_pipe_mount;
IMPORT ER		knl_pipe_init( void );

/* ---------------------------------------------------------------- strings */

LOCAL INT str_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != '\0' && n < FS_PATH_MAX ) n++;
	return n;
}

LOCAL BOOL str_same( CONST char *a, CONST char *b )
{
	while ( *a != '\0' && *a == *b ) { a++; b++; }
	return ( *a == *b );
}

/*
 * Whether two device names are one disk, or a disk and a unit of it:
 * "uda" and "uda0". A whole disk's name ends in a letter, its units add
 * the number.
 */
LOCAL BOOL dev_overlap( CONST char *a, CONST char *b )
{
	CONST char	*a0 = a, *b0 = b, *rest;

	while ( *a != '\0' && *a == *b ) { a++; b++; }
	if ( *a == *b ) {
		return TRUE;
	}
	if ( *a == '\0' && a > a0 && !( a[-1] >= '0' && a[-1] <= '9' ) ) {
		rest = b;
	} else if ( *b == '\0' && b > b0 && !( b[-1] >= '0' && b[-1] <= '9' ) ) {
		rest = a;
	} else {
		return FALSE;
	}
	for ( ; *rest != '\0'; rest++ ) {
		if ( *rest < '0' || *rest > '9' ) return FALSE;
	}
	return TRUE;
}

LOCAL void str_ncpy( char *dst, CONST char *src, INT max )
{
	INT	i;

	for ( i = 0; i < max - 1 && src[i] != '\0'; i++ ) {
		dst[i] = src[i];
	}
	dst[i] = '\0';
}

/* ---------------------------------------------------------------- tables */

/* The open file a descriptor names, held busy until fd_put; NULL when none */
LOCAL T_FILE *fd_get( INT fd )
{
	T_FILE	*f = NULL;

	if ( fd < 0 || fd >= FS_MAX_FILE ) {
		return NULL;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	if ( fs_slot[fd].used && fs_slot[fd].f != NULL ) {
		f = fs_slot[fd].f;
		f->busy++;
	}
	tk_unl_mtx(fs_mtxid);

	return f;
}

LOCAL void mount_reap( T_MOUNT *m );

/*
 * An open file no descriptor names and no call works on any more: the
 * fimp's close, then the entry free. Called without the lock.
 */
LOCAL ER file_last( T_FILE *f )
{
	T_MOUNT	*m = f->mount;
	ER	er;

	if ( m->gone ) {
		er = EX_IO;
	} else {
		er = ( m->fimp->close != NULL ) ? m->fimp->close(f) : EX_OK;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	f->owner = 0;
	f->obj = NULL;
	f->used = FALSE;
	if ( m != &fs_pipe_mount ) {
		mount_reap(m);
	}
	tk_unl_mtx(fs_mtxid);
	knl_fs_wake();

	return er;
}

LOCAL void fd_put( T_FILE *f )
{
	BOOL	last;

	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	f->busy--;
	last = ( f->refs == 0 && f->busy == 0 );
	if ( last ) f->busy = 1;		/* this caller closes it */
	tk_unl_mtx(fs_mtxid);
	if ( last ) {
		(void)file_last(f);
	}
}

/* A free open file, taken; NULL when there is none. Under the lock */
LOCAL T_FILE *file_new( void )
{
	INT	i;

	for ( i = 0; i < FS_MAX_FILE; i++ ) {
		if ( !fs_file[i].used ) {
			T_FILE	*f = &fs_file[i];

			knl_memset(f, 0, sizeof(*f));
			f->used = TRUE;
			f->mode = FS_IFREG;
			return f;
		}
	}
	return NULL;
}

/*
 * A free descriptor for owner naming f, f's count of them up; -1 when
 * there is none, or the owner holds FS_PRC_FILE already. Under the lock.
 */
LOCAL INT slot_new( T_FILE *f, ID owner )
{
	INT	i, n = 0, fd = -1;

	for ( i = 0; i < FS_MAX_FILE; i++ ) {
		if ( !fs_slot[i].used ) {
			if ( fd < 0 ) fd = i;
		} else if ( owner > 0 && fs_slot[i].owner == owner ) {
			n++;
		}
	}
	if ( fd < 0 || n >= FS_PRC_FILE ) {
		return -1;
	}
	fs_slot[fd].used = TRUE;
	fs_slot[fd].owner = owner;
	fs_slot[fd].f = f;
	f->refs++;
	return fd;
}

/*
 * Mount whose mount point is the longest prefix of 'path'.
 *	"/boot/x" under mount "/boot" gives the remainder "/x"; the mount
 *	point itself gives "/".
 */
LOCAL T_MOUNT *find_mount( CONST char *path, CONST char **rest )
{
	T_MOUNT	*best = NULL;
	INT	bestlen = 0, i, n;

	if ( path[0] != '/' ) {
		return NULL;
	}
	for ( i = 0; i < fs_nmount; i++ ) {
		T_MOUNT *m = fs_mount[i];
		INT	j;

		if ( !m->used || m->gone ) continue;
		n = m->pathlen;
		if ( n == 1 ) {				/* "/" matches everything */
			if ( bestlen == 0 ) { best = m; bestlen = 1; }
			continue;
		}
		for ( j = 0; j < n; j++ ) {
			if ( path[j] != (char)m->path[j] ) break;
		}
		if ( j < n ) continue;
		if ( path[n] != '\0' && path[n] != '/' ) continue;	/* "/booty" */
		if ( n > bestlen ) { best = m; bestlen = n; }
	}
	if ( best == NULL ) {
		return NULL;
	}
	if ( rest != NULL ) {
		*rest = ( bestlen == 1 ) ? path : ( ( path[bestlen] == '\0' ) ? "/" : path + bestlen );
	}
	return best;
}

/* Whether nothing is using a mount: no call at work on it, no file open on it */
LOCAL BOOL mount_idle( CONST T_MOUNT *m )
{
	INT	i;

	if ( m->busy > 0 ) {
		return FALSE;
	}
	for ( i = 0; i < FS_MAX_FILE; i++ ) {
		if ( fs_file[i].used && fs_file[i].mount == m ) return FALSE;
	}
	return TRUE;
}

/* A mount taken down: the fimp lets go of it and the device is closed. Under the lock */
LOCAL void mount_free( T_MOUNT *m )
{
	SZ	asz;

	if ( m->fimp->unmount != NULL ) m->fimp->unmount(m);
	if ( m->dd > 0 ) {
		/* what the unmount wrote last, its mark, out of the device's cache as well */
		if ( !m->gone && ( m->flags & FS_MNT_RDONLY ) == 0 ) {
			(void)tk_swri_dev(m->dd, TDN_FLUSH, NULL, 0, &asz);
		}
		tk_cls_dev(m->dd, 0);
	}
	m->dd = 0;
	m->gone = FALSE;
	m->objok = FALSE;
	m->used = FALSE;
}

/*
 * The device object of a mount's volume, asked of the device manager
 * the first time and kept: a mount made at start-up comes before the
 * device objects have their UUIDs. Zero for a volume with no device.
 */
LOCAL void mount_obj( T_MOUNT *m, TS_UUID *p_uuid )
{
	if ( !m->objok && m->dev[0] != 0 && knl_obdev_uuid(m->dev, &m->obj) >= E_OK ) {
		m->objok = TRUE;
	}
	if ( m->objok ) {
		*p_uuid = m->obj;
	} else {
		knl_memset(p_uuid, 0, sizeof(*p_uuid));
	}
}

/*
 * A mount whose device went, or one no path reaches (made from a disk's
 * key), taken down once its last user is done. Under the lock
 */
LOCAL void mount_reap( T_MOUNT *m )
{
	if ( m->used && ( m->gone || m->pathlen == 0 ) && mount_idle(m) ) {
		mount_free(m);
	}
}

/* The mount of a path, held busy until mount_put */
LOCAL T_MOUNT *mount_get( CONST char *path, CONST char **rest )
{
	T_MOUNT	*m;

	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	m = find_mount(path, rest);
	if ( m != NULL ) m->busy++;
	tk_unl_mtx(fs_mtxid);

	return m;
}

LOCAL void mount_put( T_MOUNT *m )
{
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	m->busy--;
	mount_reap(m);
	tk_unl_mtx(fs_mtxid);
}

LOCAL BOOL mount_rdonly( CONST T_MOUNT *m )
{
	return ( (m->flags & FS_MNT_RDONLY) != 0 );
}

/* ---------------------------------------------------------------- start-up */

EXPORT ER fs_main( void )
{
	T_CMTX	cmtx;
	T_CFLG	cflg;
	INT	i;

	if ( fs_mtxid > 0 ) {
		return E_OK;
	}
	fs_nmount = 0;
	for ( i = 0; i < FS_MAX_FILE; i++ ) {
		fs_file[i].used = FALSE;
		fs_slot[i].used = FALSE;
	}
	fs_nfimp = 0;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	fs_mtxid = tk_cre_mtx(&cmtx);
	if ( fs_mtxid <= 0 ) {
		return (ER)fs_mtxid;
	}
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	fs_flgid = tk_cre_flg(&cflg);
	if ( fs_flgid <= 0 ) {
		return (ER)fs_flgid;
	}

	knl_memset(&fs_pipe_mount, 0, sizeof(fs_pipe_mount));
	fs_pipe_mount.fimp = &knl_fimp_pipe;
	fs_pipe_mount.used = TRUE;

	return knl_pipe_init();
}

EXPORT ER fs_regist( CONST T_FIMP *fimp )
{
	INT	i;

	if ( fimp == NULL || fimp->name == NULL ) {
		return EX_INVAL;
	}
	for ( i = 0; i < fs_nfimp; i++ ) {
		if ( str_same(fs_fimp[i]->name, fimp->name) ) return EX_EXIST;
	}
	if ( fs_nfimp >= FS_MAX_FIMP ) {
		return EX_NOSPC;
	}
	fs_fimp[fs_nfimp++] = fimp;

	return EX_OK;
}

/*
 * Mount: opens the block device and hands the mount to the fimp.
 *	devnm is a device name such as "vblka0"; a fimp that keeps its
 *	data in memory takes NULL. A mount point or a device already in
 *	use is refused (EX_BUSY): two mounts of one device would each
 *	keep their own idea of what is on it. With 'whole', a device that
 *	contains or lies in one mounted is refused too: a disk and its
 *	partition are the same blocks. With FS_MNT_RDONLY the device is
 *	opened for reading and nothing that writes is let through
 *	(EX_ROFS). A mount that is gone still holds its device and its
 *	mount point until it is let go.
 */
LOCAL ER attach( CONST char *fimpnm, CONST char *devnm, CONST char *path, UINT flags, BOOL whole )
{
	CONST T_FIMP	*fimp = NULL;
	T_MOUNT		*m = NULL;
	INT		i, len;
	ID		dd = 0;
	ER		er;

	/* "" for a mount no path reaches, made from a disk's key (knl_fs_open_vol) */
	if ( fimpnm == NULL || path == NULL || ( path[0] != '/' && path[0] != '\0' ) ) {
		return EX_INVAL;
	}
	len = str_len(path);
	if ( len >= FS_PATH_MAX ) {
		return EX_NAMETOOLONG;
	}
	if ( len > 1 && path[len - 1] == '/' ) {
		return EX_INVAL;			/* no trailing slash */
	}
	if ( devnm != NULL && str_len(devnm) >= FS_DEVNM_MAX ) {
		return EX_NAMETOOLONG;
	}
	for ( i = 0; i < fs_nfimp; i++ ) {
		if ( str_same(fs_fimp[i]->name, fimpnm) ) { fimp = fs_fimp[i]; break; }
	}
	if ( fimp == NULL ) {
		return EX_NOENT;
	}

	tk_loc_mtx(fs_mtxid, TMO_FEVR);

	for ( i = 0; i < fs_nmount; i++ ) {
		T_MOUNT		*e = fs_mount[i];
		CONST char	*d = (CONST char *)e->dev;

		if ( e->used && ( ( path[0] != '\0' && str_same((CONST char *)e->path, path) )
		  || ( devnm != NULL && ( str_same(d, devnm)
					|| ( whole && d[0] != '\0' && dev_overlap(d, devnm) ) ) ) ) ) {
			er = EX_BUSY;
			goto exit;
		}
		if ( !e->used && m == NULL ) m = e;
	}
	if ( m == NULL ) {
		/* every entry is in use: one more, kept from now on */
		if ( fs_nmount >= FS_MAX_MOUNT ) {
			er = EX_NOSPC;
			goto exit;
		}
		m = (T_MOUNT *)Kcalloc(1, sizeof(T_MOUNT));
		if ( m == NULL ) {
			er = EX_NOMEM;
			goto exit;
		}
		fs_mount[fs_nmount++] = m;
	}

	if ( devnm != NULL ) {
		dd = tk_opn_dev((UB *)devnm, ( (flags & FS_MNT_RDONLY) != 0 ) ? TD_READ : TD_UPDATE);
		if ( dd < E_OK ) {
			er = EX_NOENT;
			goto exit;
		}
	}

	m->fimp    = fimp;
	m->dd      = dd;
	m->flags   = flags;
	m->exinf   = NULL;
	m->busy    = 0;
	m->gone    = FALSE;
	m->objok   = FALSE;
	m->pathlen = len;
	str_ncpy((char *)m->path, path, FS_PATH_MAX);
	str_ncpy((char *)m->dev, ( devnm != NULL ) ? devnm : "", FS_DEVNM_MAX);
	m->used    = TRUE;

	er = ( fimp->mount != NULL ) ? fimp->mount(m) : EX_OK;
	if ( er < EX_OK ) {
		m->used = FALSE;
		if ( dd > 0 ) tk_cls_dev(dd, 0);
	}

    exit:
	tk_unl_mtx(fs_mtxid);

	return er;
}

EXPORT ER fs_attach( CONST char *fimpnm, CONST char *devnm, CONST char *path, UINT flags )
{
	return attach(fimpnm, devnm, path, flags, FALSE);
}

/* A live mount, by its mount point or by its device. Under the lock */
LOCAL T_MOUNT *mount_by( CONST char *path, CONST char *devnm )
{
	INT	i;

	for ( i = 0; i < fs_nmount; i++ ) {
		T_MOUNT	*m = fs_mount[i];

		if ( !m->used || m->gone ) continue;
		if ( path != NULL && str_same((CONST char *)m->path, path) ) return m;
		if ( devnm != NULL && str_same((CONST char *)m->dev, devnm) ) return m;
	}
	return NULL;
}

/* Taken down, unless a file of it is open or a call is at work on it (EX_BUSY). Under the lock */
LOCAL ER detach( T_MOUNT *m )
{
	if ( !mount_idle(m) ) {
		return EX_BUSY;
	}
	mount_free(m);

	return EX_OK;
}

EXPORT ER fs_detach( CONST char *path )
{
	T_MOUNT	*m;
	ER	er;

	if ( path == NULL ) {
		return EX_INVAL;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	m = mount_by(path, NULL);
	er = ( m != NULL ) ? detach(m) : EX_NOENT;
	tk_unl_mtx(fs_mtxid);

	return er;
}

/* ---------------------------------------------------------------- mounts of device objects */

/* FS_MEDIA_DIR "/" devnm into buf; FALSE when that is not one name there */
/* a descriptor that is a disk's root opened from its key (vd, knl_fs_open_vol) */
#define FS_O_VOLUME	0x8000

/* Whether a descriptor of the mount is a disk's root opened from its key (vd). Under the lock */
LOCAL BOOL vd_open( CONST T_MOUNT *m )
{
	INT	i;

	for ( i = 0; i < FS_MAX_FILE; i++ ) {
		if ( fs_file[i].used && fs_file[i].mount == m && ( fs_file[i].oflags & FS_O_VOLUME ) != 0 ) {
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL BOOL media_path( CONST char *devnm, char *buf )
{
	CONST char	*dir = FS_MEDIA_DIR;
	INT		n = 0, i;

	for ( i = 0; dir[i] != '\0'; i++ ) buf[n++] = dir[i];
	buf[n++] = '/';
	for ( i = 0; devnm[i] != '\0'; i++ ) {
		if ( n >= FS_PATH_MAX - 1 ) return FALSE;
		buf[n++] = devnm[i];
	}
	buf[n] = '\0';

	return knl_fs_media_path(buf);
}

EXPORT ER knl_fs_attach_media( CONST char *fimpnm, CONST char *devnm, UINT flags )
{
	char	path[FS_PATH_MAX];

	if ( devnm == NULL || !media_path(devnm, path) ) {
		return EX_INVAL;
	}
	/* opened already from its key, with no path: the same mount takes the path */
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	{
		T_MOUNT	*m = mount_by(NULL, devnm);

		if ( m != NULL && m->pathlen == 0
		  && ( ( flags & FS_MNT_RDONLY ) != 0 || !mount_rdonly(m) ) ) {
			str_ncpy((char *)m->path, path, FS_PATH_MAX);
			m->pathlen = str_len(path);
			tk_unl_mtx(fs_mtxid);
			return EX_OK;
		}
	}
	tk_unl_mtx(fs_mtxid);
	return attach(fimpnm, devnm, path, flags, TRUE);
}

EXPORT ER knl_fs_detach_media( CONST char *devnm )
{
	char	path[FS_PATH_MAX];
	T_MOUNT	*m;
	ER	er;

	if ( devnm == NULL || !media_path(devnm, path) ) {
		return EX_INVAL;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	m = mount_by(NULL, devnm);
	if ( m == NULL ) {
		er = EX_NOENT;
	} else if ( !str_same((CONST char *)m->path, path) ) {
		er = EX_ACCES;			/* one the system made elsewhere (/boot) */
	} else if ( vd_open(m) ) {
		/* still open from its key: only the path goes, the mount with the last of them */
		m->path[0] = '\0';
		m->pathlen = 0;
		er = EX_OK;
	} else {
		er = detach(m);
	}
	tk_unl_mtx(fs_mtxid);

	return er;
}

LOCAL void mnt_copy( T_FSMNT *d, CONST T_MOUNT *m )
{
	str_ncpy((char *)d->path, (CONST char *)m->path, FS_PATH_MAX);
	str_ncpy((char *)d->fimp, m->fimp->name, FS_FIMPNM_MAX);
	str_ncpy((char *)d->dev, (CONST char *)m->dev, FS_DEVNM_MAX);
	d->flags = m->flags;
	knl_memset(&d->obj, 0, sizeof(d->obj));
}

EXPORT BOOL knl_fs_mount_of( CONST char *devnm, T_FSMNT *p_mnt )
{
	T_MOUNT	*m;

	if ( devnm == NULL || devnm[0] == '\0' || fs_mtxid <= 0 ) {
		return FALSE;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	m = mount_by(NULL, devnm);
	if ( m != NULL && p_mnt != NULL ) {
		mnt_copy(p_mnt, m);
	}
	tk_unl_mtx(fs_mtxid);

	return ( m != NULL );
}

EXPORT INT knl_fs_revoke( CONST char *devnm )
{
	INT	i, n = 0;

	if ( devnm == NULL || devnm[0] == '\0' || fs_mtxid <= 0 ) {
		return 0;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	for ( i = 0; i < fs_nmount; i++ ) {
		T_MOUNT	*m = fs_mount[i];

		if ( !m->used || m->gone || !str_same((CONST char *)m->dev, devnm) ) continue;
		m->gone = TRUE;
		n++;
		mount_reap(m);
	}
	tk_unl_mtx(fs_mtxid);

	return n;
}

/* ---------------------------------------------------------------- files */

/*
 * Open for a process (owner, its pid) or for the kernel (0). The mount
 * is looked up and the descriptor reserved under one lock, so the mount
 * cannot be taken down in between.
 */
EXPORT INT knl_fs_open_as( CONST char *path, UINT oflags, ID owner )
{
	CONST char	*rest;
	T_MOUNT		*m;
	T_FILE		*f = NULL;
	INT		fd = -1;
	ER		er;

	if ( path == NULL || str_len(path) >= FS_PATH_MAX ) {
		return EX_NAMETOOLONG;
	}

	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	m = find_mount(path, &rest);
	if ( m == NULL ) {
		tk_unl_mtx(fs_mtxid);
		return EX_NOENT;
	}
	if ( mount_rdonly(m) && ( (oflags & O_ACCMODE) != O_RDONLY
				|| (oflags & (O_CREAT | O_TRUNC)) != 0 ) ) {
		tk_unl_mtx(fs_mtxid);
		return EX_ROFS;
	}
	f = file_new();
	if ( f != NULL ) {
		fd = slot_new(f, owner);
		if ( fd < 0 ) f->used = FALSE;
	}
	if ( fd < 0 ) {
		tk_unl_mtx(fs_mtxid);
		return EX_MFILE;
	}
	f->owner  = owner;
	f->mount  = m;
	f->oflags = oflags;
	f->busy   = 1;			/* the fimp works on it: not closed meanwhile */
	tk_unl_mtx(fs_mtxid);

	er = ( m->fimp->open != NULL ) ? m->fimp->open(m, rest, oflags, f) : EX_NOTSUP;
	if ( er < EX_OK ) {
		tk_loc_mtx(fs_mtxid, TMO_FEVR);
		fs_slot[fd].used = FALSE;
		fs_slot[fd].f = NULL;
		f->owner = 0;
		f->used = FALSE;
		tk_unl_mtx(fs_mtxid);
		return (INT)er;
	}
	if ( (oflags & O_APPEND) != 0 ) {
		f->offset = f->size;
	}
	fd_put(f);

	return fd;
}

EXPORT INT fs_open( CONST char *path, UINT oflags )
{
	return knl_fs_open_as(path, oflags, 0);
}

/*
 * The descriptor goes. When it was the last of its open file and no
 * call is at work on that, the file is closed: on a mount that is gone
 * without the fimp, whose volume is no longer there (EX_IO says that
 * what was written last may not have reached it). The last one lets the
 * mount go.
 */
EXPORT ER fs_close( INT fd )
{
	T_FILE	*f;
	BOOL	last;

	if ( fd < 0 || fd >= FS_MAX_FILE ) {
		return EX_BADF;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	if ( !fs_slot[fd].used || fs_slot[fd].f == NULL ) {
		tk_unl_mtx(fs_mtxid);
		return EX_BADF;
	}
	f = fs_slot[fd].f;
	fs_slot[fd].used = FALSE;
	fs_slot[fd].f = NULL;
	fs_slot[fd].owner = 0;
	f->refs--;
	last = ( f->refs == 0 && f->busy == 0 );
	if ( last ) f->busy = 1;
	tk_unl_mtx(fs_mtxid);

	if ( !last ) {
		knl_fs_wake();			/* a pipe's end may be one fewer */
		return EX_OK;
	}
	return file_last(f);
}

/* Whether a descriptor is open and belongs to that owner */
EXPORT BOOL knl_fs_mine( INT fd, ID owner )
{
	BOOL	yes;

	if ( fd < 0 || fd >= FS_MAX_FILE || fs_mtxid <= 0 ) {
		return FALSE;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	yes = ( fs_slot[fd].used && fs_slot[fd].owner == owner );
	tk_unl_mtx(fs_mtxid);
	return yes;
}

/*
 * A process is ending: whatever it left open is closed. The table is
 * looked through under one hold of the lock, a handful of the process's
 * descriptors at a time, and they are closed with the lock let go. Taking
 * the lock once per entry made every ending a few thousand turns of a
 * lock that the other endings and every file call also want, and with
 * many processes ending at once they queued behind each other for
 * minutes, the reaper among them.
 */
#define FS_END_BATCH	32

EXPORT void knl_fs_prc_end( ID pid )
{
	INT	fd = 0, n, i;
	INT	got[FS_END_BATCH];

	if ( pid <= 0 || fs_mtxid <= 0 ) {
		return;
	}
	while ( fd < FS_MAX_FILE ) {
		n = 0;
		tk_loc_mtx(fs_mtxid, TMO_FEVR);
		for ( ; fd < FS_MAX_FILE && n < FS_END_BATCH; fd++ ) {
			if ( fs_slot[fd].used && fs_slot[fd].owner == pid ) {
				got[n++] = fd;
			}
		}
		tk_unl_mtx(fs_mtxid);

		for ( i = 0; i < n; i++ ) {
			(void)fs_close(got[i]);
		}
	}
}

/* A second descriptor of the same open file, for owner */
EXPORT INT knl_fs_dup_as( INT fd, ID owner )
{
	INT	nfd = EX_BADF;

	if ( fd < 0 || fd >= FS_MAX_FILE ) {
		return EX_BADF;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	if ( fs_slot[fd].used && fs_slot[fd].f != NULL ) {
		nfd = slot_new(fs_slot[fd].f, owner);
		if ( nfd < 0 ) nfd = EX_MFILE;
	}
	tk_unl_mtx(fs_mtxid);

	return nfd;
}

EXPORT INT fs_dup( INT fd )
{
	return knl_fs_dup_as(fd, 0);
}

/*
 * Descriptors of 'from' given to 'to' as well: each of the open files
 * gets one more descriptor, owned by 'to'. All or none.
 */
EXPORT ER knl_fs_give( CONST INT *fds, INT n, ID from, ID to, INT *out )
{
	INT	i, j;
	ER	er = EX_OK;

	if ( n < 0 || ( n > 0 && ( fds == NULL || out == NULL ) ) ) {
		return EX_INVAL;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	for ( i = 0; i < n; i++ ) {
		INT	fd = fds[i];

		if ( fd < 0 || fd >= FS_MAX_FILE || !fs_slot[fd].used || fs_slot[fd].owner != from ) {
			er = EX_BADF;
			break;
		}
		out[i] = slot_new(fs_slot[fd].f, to);
		if ( out[i] < 0 ) {
			er = EX_MFILE;
			break;
		}
	}
	if ( er < EX_OK ) {
		for ( j = 0; j < i; j++ ) {
			fs_slot[out[j]].f->refs--;	/* the caller's still name them */
			fs_slot[out[j]].used = FALSE;
			fs_slot[out[j]].f = NULL;
		}
	}
	tk_unl_mtx(fs_mtxid);

	return er;
}

/*
 * A pipe: two open files of the pipe fimp, the reading end in fd[0], the
 * writing end in fd[1], owned by owner.
 */
IMPORT ER knl_pipe_make( T_FILE *rd, T_FILE *wr );

EXPORT ER knl_fs_pipe_as( INT fd[2], ID owner )
{
	T_FILE	*r = NULL, *w = NULL;
	INT	a = -1, b = -1;
	ER	er;

	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	r = file_new();
	w = ( r != NULL ) ? file_new() : NULL;
	if ( w != NULL ) {
		a = slot_new(r, owner);
		b = ( a >= 0 ) ? slot_new(w, owner) : -1;
	}
	if ( b < 0 ) {
		if ( a >= 0 ) { fs_slot[a].used = FALSE; fs_slot[a].f = NULL; }
		if ( r != NULL ) r->used = FALSE;
		if ( w != NULL ) w->used = FALSE;
		tk_unl_mtx(fs_mtxid);
		return EX_MFILE;
	}
	r->mount = w->mount = &fs_pipe_mount;
	r->owner = w->owner = owner;
	r->oflags = O_RDONLY;
	w->oflags = O_WRONLY;
	r->mode = w->mode = FS_IFIFO;
	tk_unl_mtx(fs_mtxid);

	er = knl_pipe_make(r, w);
	if ( er < EX_OK ) {
		tk_loc_mtx(fs_mtxid, TMO_FEVR);
		fs_slot[a].used = fs_slot[b].used = FALSE;
		fs_slot[a].f = fs_slot[b].f = NULL;
		r->used = w->used = FALSE;
		tk_unl_mtx(fs_mtxid);
		return er;
	}
	fd[0] = a;
	fd[1] = b;
	return EX_OK;
}

EXPORT ER fs_pipe( INT fd[2] )
{
	return knl_fs_pipe_as(fd, 0);
}

/* ---------------------------------------------------------------- waiting */

/* Everyone waiting in a slice is woken to look again */
EXPORT void knl_fs_wake( void )
{
	if ( fs_flgid > 0 ) {
		tk_set_flg(fs_flgid, 1);
		tk_clr_flg(fs_flgid, 0);
	}
}

/*
 * One slice of a wait for something knl_fs_wake tells: FALSE when the
 * caller is to stop waiting -- a signal to be handed over, or the end
 * of its process.
 */
EXPORT BOOL knl_fs_wait_slice( TMO ms )
{
	UINT	ptn;

	(void)tk_wai_flg(fs_flgid, 1, TWF_ORW, &ptn, ms);
	return !knl_psx_intr();
}

/* The descriptor's mount went with its device: nothing is asked of the fimp */
#define GONE(f)		( (f)->mount->gone )

/*
 * The most one read or write moves: what its INT answer can count,
 * whole pages of it. A larger request moves that much and answers so;
 * the caller goes on from there, as with any short read or write.
 */
#define FS_RW_MAX	( (SZ)0x7FFFF000 )

EXPORT INT fs_read( INT fd, void *buf, SZ len )
{
	T_FILE	*f = fd_get(fd);
	INT	r;

	if ( f == NULL || buf == NULL ) {
		if ( f != NULL ) fd_put(f);
		return EX_BADF;
	}
	if ( GONE(f) ) {
		r = EX_IO;
	} else if ( (f->oflags & O_ACCMODE) == O_WRONLY ) {
		r = EX_BADF;
	} else if ( f->mode == FS_IFDIR ) {
		r = EX_ISDIR;
	} else if ( len == 0 ) {
		r = 0;
	} else if ( len < 0 ) {
		r = EX_INVAL;
	} else {
		if ( len > FS_RW_MAX ) len = FS_RW_MAX;
		r = ( f->mount->fimp->read != NULL ) ? f->mount->fimp->read(f, buf, len) : EX_NOTSUP;
	}
	fd_put(f);
	return r;
}

EXPORT INT fs_write( INT fd, CONST void *buf, SZ len )
{
	T_FILE	*f = fd_get(fd);
	INT	r;

	if ( f == NULL || buf == NULL ) {
		if ( f != NULL ) fd_put(f);
		return EX_BADF;
	}
	if ( GONE(f) ) {
		r = EX_IO;
	} else if ( (f->oflags & O_ACCMODE) == O_RDONLY ) {
		r = EX_BADF;
	} else if ( f->mode == FS_IFDIR ) {
		r = EX_ISDIR;
	} else if ( len == 0 ) {
		r = 0;
	} else if ( len < 0 ) {
		r = EX_INVAL;
	} else {
		if ( len > FS_RW_MAX ) len = FS_RW_MAX;
		if ( (f->oflags & O_APPEND) != 0 && f->mode == FS_IFREG ) {
			f->offset = f->size;
		}
		r = ( f->mount->fimp->write != NULL ) ? f->mount->fimp->write(f, buf, len) : EX_NOTSUP;
	}
	fd_put(f);
	return r;
}

EXPORT D fs_lseek( INT fd, D offset, INT whence )
{
	T_FILE	*f = fd_get(fd);
	D	pos;

	if ( f == NULL ) {
		return EX_BADF;
	}
	if ( GONE(f) ) {
		fd_put(f);
		return EX_IO;
	}
	if ( f->mode == FS_IFIFO ) {
		fd_put(f);
		return EX_INVAL;		/* ESPIPE */
	}
	switch ( whence ) {
	  case SEEK_SET_:	pos = offset;				break;
	  case SEEK_CUR_:	pos = (D)f->offset + offset;		break;
	  case SEEK_END_:	pos = (D)f->size + offset;		break;
	  default:		pos = EX_INVAL;				break;
	}
	if ( pos >= 0 ) {
		f->offset = (UD)pos;
	} else if ( whence >= SEEK_SET_ && whence <= SEEK_END_ ) {
		pos = EX_INVAL;
	}
	fd_put(f);

	return pos;
}

EXPORT ER fs_stat( CONST char *path, T_FSTAT *st )
{
	CONST char	*rest;
	T_MOUNT		*m;
	ER		er;

	if ( path == NULL || st == NULL ) {
		return EX_INVAL;
	}
	m = mount_get(path, &rest);
	if ( m == NULL ) {
		return EX_NOENT;
	}
	if ( m->fimp->stat != NULL ) {
		er = m->fimp->stat(m, rest, st);
	} else if ( m->fimp->xstat != NULL ) {
		T_FSXSTAT	x;

		er = m->fimp->xstat(m, rest, &x);
		if ( er >= EX_OK ) *st = x.st;
	} else {
		er = EX_NOTSUP;
	}
	if ( er >= EX_OK ) {
		mount_obj(m, &st->dev);
	}
	mount_put(m);

	return er;
}

EXPORT ER fs_fstat( INT fd, T_FSTAT *st )
{
	T_FSXSTAT	x;
	ER		er;

	if ( st == NULL ) {
		return EX_BADF;
	}
	er = fs_fxstat(fd, &x);
	if ( er >= EX_OK ) {
		*st = x.st;
	}
	return er;
}

/* What the layer fills in where a fimp said nothing */
LOCAL void xstat_fill( T_FSXSTAT *x )
{
	if ( x->nlink == 0 ) x->nlink = 1;
	if ( x->atime == 0 ) x->atime = x->st.mtime;
	if ( x->ctime == 0 ) x->ctime = x->st.mtime;
	if ( x->blksize == 0 ) x->blksize = 4096;
}

EXPORT ER fs_fxstat( INT fd, T_FSXSTAT *st )
{
	T_FILE	*f = fd_get(fd);
	ER	er = EX_OK;

	if ( f == NULL || st == NULL ) {
		if ( f != NULL ) fd_put(f);
		return EX_BADF;
	}
	knl_memset(st, 0, sizeof(*st));
	if ( GONE(f) ) {
		er = EX_IO;
	} else if ( f->mount->fimp->fxstat != NULL ) {
		er = f->mount->fimp->fxstat(f, st);
	} else {
		st->st.mode  = f->mode | FS_IRWXU;
		st->st.size  = f->size;
		st->st.mtime = 0;
		st->st.ino   = f->ino;
	}
	if ( er >= EX_OK ) {
		if ( f->mount != &fs_pipe_mount ) {
			mount_obj(f->mount, &st->st.dev);
		}
		xstat_fill(st);
	}
	fd_put(f);

	return er;
}

EXPORT ER fs_xstat( CONST char *path, T_FSXSTAT *st )
{
	CONST char	*rest;
	T_MOUNT		*m;
	ER		er;

	if ( path == NULL || st == NULL ) {
		return EX_INVAL;
	}
	m = mount_get(path, &rest);
	if ( m == NULL ) {
		return EX_NOENT;
	}
	knl_memset(st, 0, sizeof(*st));
	if ( m->fimp->xstat != NULL ) {
		er = m->fimp->xstat(m, rest, st);
	} else if ( m->fimp->stat != NULL ) {
		er = m->fimp->stat(m, rest, &st->st);
		if ( er >= EX_OK ) {
			st->st.mode = ( st->st.mode & FS_IFMT ) | ( ( st->st.mode & FS_IFDIR ) ? 0755 : 0644 );
		}
	} else {
		er = EX_NOTSUP;
	}
	if ( er >= EX_OK ) {
		mount_obj(m, &st->st.dev);
		xstat_fill(st);
	}
	mount_put(m);

	return er;
}

EXPORT ER fs_ftruncate( INT fd, UD len )
{
	T_FILE	*f = fd_get(fd);
	ER	er;

	if ( f == NULL ) {
		return EX_BADF;
	}
	if ( (f->oflags & O_ACCMODE) == O_RDONLY ) {
		er = EX_BADF;
	} else if ( GONE(f) ) {
		er = EX_IO;
	} else {
		er = ( f->mount->fimp->truncate != NULL ) ? f->mount->fimp->truncate(f, len) : EX_NOTSUP;
	}
	fd_put(f);
	return er;
}

EXPORT ER fs_truncate( CONST char *path, UD len )
{
	INT	fd;
	ER	er;

	fd = fs_open(path, O_WRONLY);
	if ( fd < 0 ) {
		return (ER)fd;
	}
	er = fs_ftruncate(fd, len);
	fs_close(fd);

	return er;
}

EXPORT INT fs_getdents( INT fd, T_DIRENT *buf, INT nent )
{
	T_FILE	*f = fd_get(fd);
	INT	r;

	if ( f == NULL || buf == NULL || nent <= 0 ) {
		if ( f != NULL ) fd_put(f);
		return EX_BADF;
	}
	if ( GONE(f) ) {
		r = EX_IO;
	} else if ( f->mode != FS_IFDIR ) {
		r = EX_NOTDIR;
	} else {
		r = ( f->mount->fimp->getdents != NULL ) ? f->mount->fimp->getdents(f, buf, nent) : EX_NOTSUP;
	}
	fd_put(f);
	return r;
}

EXPORT ER fs_ioctl( INT fd, UINT cmd, void *arg )
{
	T_FILE	*f = fd_get(fd);
	ER	er;

	if ( f == NULL ) {
		return EX_BADF;
	}
	if ( GONE(f) ) {
		er = EX_IO;
	} else if ( f->mount->fimp->ioctl != NULL ) {
		er = f->mount->fimp->ioctl(f, cmd, arg);
	} else {
		er = EX_NOTTY;
	}
	fd_put(f);
	return er;
}

/* The flags of the open file: the access, O_APPEND and FS_O_NONBLOCK; only the last two are set */
EXPORT INT fs_fcntl( INT fd, INT cmd, INT arg )
{
	T_FILE	*f = fd_get(fd);
	INT	r = EX_INVAL;

	if ( f == NULL ) {
		return EX_BADF;
	}
	switch ( cmd ) {
	  case FS_F_GETFL:
		r = (INT)( f->oflags & ( O_ACCMODE | O_APPEND | FS_O_NONBLOCK ) );
		break;
	  case FS_F_SETFL:
		f->oflags = ( f->oflags & ~(UINT)( O_APPEND | FS_O_NONBLOCK ) )
			  | ( (UINT)arg & ( O_APPEND | FS_O_NONBLOCK ) );
		r = EX_OK;
		knl_fs_wake();
		break;
	  default:
		break;
	}
	fd_put(f);
	return r;
}

/*
 * Which of the descriptors are ready: a fimp without poll is always
 * ready for what was asked (a file on a volume never waits). Looked at
 * again whenever knl_fs_wake says something changed, and at least every
 * 100ms; a signal to be handed over stops the wait (EX_INTR).
 */
EXPORT INT fs_poll( T_FSPOLL *fds, INT n, TMO tmout )
{
	UD	start = 0, now;
	INT	i, ready;

	if ( n < 0 || ( n > 0 && fds == NULL ) ) {
		return EX_INVAL;
	}
	(void)ts_get_mono(&start);
	for ( ;; ) {
		ready = 0;
		for ( i = 0; i < n; i++ ) {
			T_FILE	*f;
			UINT	ev = fds[i].events | FS_POLL_ERR | FS_POLL_HUP;

			fds[i].revents = 0;
			if ( fds[i].fd < 0 ) continue;
			f = fd_get(fds[i].fd);
			if ( f == NULL ) {
				fds[i].revents = FS_POLL_NVAL;
			} else {
				if ( GONE(f) ) {
					fds[i].revents = FS_POLL_ERR;
				} else if ( f->mount->fimp->poll != NULL ) {
					fds[i].revents = f->mount->fimp->poll(f, ev) & ev;
				} else {
					fds[i].revents = fds[i].events & ( FS_POLL_IN | FS_POLL_OUT );
				}
				fd_put(f);
			}
			if ( fds[i].revents != 0 ) ready++;
		}
		if ( ready > 0 || tmout == TMO_POL ) {
			return ready;
		}
		if ( tmout != TMO_FEVR ) {
			(void)ts_get_mono(&now);
			if ( (D)( now - start ) / 1000000 >= (D)tmout ) {
				return 0;
			}
		}
		if ( !knl_fs_wait_slice(100) ) {
			return EX_INTR;
		}
	}
}

/* ---------------------------------------------------------------- names */

#define FS_PATH_OP(fn, member)						\
	CONST char *rest;						\
	T_MOUNT	*m;							\
	ER	er;							\
	if ( path == NULL ) return EX_INVAL;				\
	m = mount_get(path, &rest);					\
	if ( m == NULL ) return EX_NOENT;				\
	if ( mount_rdonly(m) ) {					\
		er = EX_ROFS;						\
	} else {							\
		er = ( m->fimp->member != NULL ) ? m->fimp->member(m, rest) : EX_NOTSUP; \
	}								\
	mount_put(m);							\
	return er;

EXPORT ER fs_mkdir( CONST char *path )
{
	FS_PATH_OP(fs_mkdir, mkdir)
}

EXPORT ER fs_rmdir( CONST char *path )
{
	FS_PATH_OP(fs_rmdir, rmdir)
}

EXPORT ER fs_unlink( CONST char *path )
{
	FS_PATH_OP(fs_unlink, unlink)
}

EXPORT ER fs_utime( CONST char *path, UD mtime )
{
	CONST char *rest;
	T_MOUNT	*m;
	ER	er;

	if ( path == NULL ) return EX_INVAL;
	m = mount_get(path, &rest);
	if ( m == NULL ) return EX_NOENT;
	if ( mount_rdonly(m) ) {
		er = EX_ROFS;
	} else {
		er = ( m->fimp->utime != NULL ) ? m->fimp->utime(m, rest, mtime) : EX_NOTSUP;
	}
	mount_put(m);
	return er;
}

EXPORT ER fs_rename( CONST char *from, CONST char *to )
{
	CONST char	*rfrom, *rto;
	T_MOUNT		*mf, *mt;
	ER		er;

	if ( from == NULL || to == NULL ) {
		return EX_INVAL;
	}
	mf = mount_get(from, &rfrom);
	if ( mf == NULL ) {
		return EX_NOENT;
	}
	mt = mount_get(to, &rto);
	if ( mt == NULL ) {
		er = EX_NOENT;
	} else if ( mf != mt ) {
		er = EX_NOTSUP;			/* across volumes: copy and delete */
	} else if ( mount_rdonly(mf) ) {
		er = EX_ROFS;
	} else {
		er = ( mf->fimp->rename != NULL ) ? mf->fimp->rename(mf, rfrom, rto) : EX_NOTSUP;
	}
	if ( mt != NULL ) mount_put(mt);
	mount_put(mf);

	return er;
}

/* A second name for a file, on the same mount */
EXPORT ER fs_link( CONST char *from, CONST char *to )
{
	CONST char	*rfrom, *rto;
	T_MOUNT		*mf, *mt;
	ER		er;

	if ( from == NULL || to == NULL ) {
		return EX_INVAL;
	}
	mf = mount_get(from, &rfrom);
	if ( mf == NULL ) {
		return EX_NOENT;
	}
	mt = mount_get(to, &rto);
	if ( mt == NULL ) {
		er = EX_NOENT;
	} else if ( mf != mt ) {
		er = EX_NOTSUP;			/* EXDEV */
	} else if ( mount_rdonly(mf) ) {
		er = EX_ROFS;
	} else {
		er = ( mf->fimp->link != NULL ) ? mf->fimp->link(mf, rfrom, rto) : EX_PERM;
	}
	if ( mt != NULL ) mount_put(mt);
	mount_put(mf);

	return er;
}

EXPORT ER fs_chmod( CONST char *path, UINT mode )
{
	CONST char	*rest;
	T_MOUNT		*m;
	ER		er;

	if ( path == NULL ) {
		return EX_INVAL;
	}
	m = mount_get(path, &rest);
	if ( m == NULL ) {
		return EX_NOENT;
	}
	if ( mount_rdonly(m) ) {
		er = EX_ROFS;
	} else {
		er = ( m->fimp->chmod != NULL ) ? m->fimp->chmod(m, rest, mode & 07777) : EX_OK;
	}
	mount_put(m);
	return er;
}

EXPORT ER fs_statvfs( CONST char *path, T_FSSTAT *st )
{
	CONST char	*rest;
	T_MOUNT		*m;
	ER		er;

	if ( path == NULL || st == NULL ) {
		return EX_INVAL;
	}
	m = mount_get(path, &rest);
	if ( m == NULL ) {
		return EX_NOENT;
	}
	er = ( m->fimp->statvfs != NULL ) ? m->fimp->statvfs(m, st) : EX_NOTSUP;
	mount_put(m);

	return er;
}

EXPORT ER fs_sync( void )
{
	INT	i;
	ER	er = EX_OK, e;

	for ( i = 0; ; i++ ) {
		T_MOUNT *m;
		BOOL	go;

		tk_loc_mtx(fs_mtxid, TMO_FEVR);
		if ( i >= fs_nmount ) {
			tk_unl_mtx(fs_mtxid);
			break;
		}
		m = fs_mount[i];
		go = ( m->used && !m->gone && m->fimp->sync != NULL && !mount_rdonly(m) );
		if ( go ) m->busy++;
		tk_unl_mtx(fs_mtxid);
		if ( !go ) continue;

		e = m->fimp->sync(m);
		if ( e < EX_OK ) er = e;
		mount_put(m);
	}

	return er;
}


/* ---------------------------------------------------------------- the machine going off */

/* What a mount is called on the console */
LOCAL CONST char *mount_said( CONST T_MOUNT *m )
{
	return ( m->pathlen > 0 ) ? (CONST char *)m->path : (CONST char *)m->dev;
}

/* One mount written, and out of the device's cache. A device with no cache says E_PAR, which is no failure */
LOCAL ER mount_write( T_MOUNT *m )
{
	SZ	asz;
	ER	er, e;

	er = m->fimp->sync(m);
	if ( er >= EX_OK && m->dd > 0 ) {
		e = tk_swri_dev(m->dd, TDN_FLUSH, NULL, 0, &asz);
		if ( e < E_OK && e != E_PAR && e != E_NOSPT ) {
			er = EX_IO;
		}
	}
	return er;
}

/*
 * fs_sync as the machine goes off, where it matters what became of each
 * volume: each is told on the console, also one whose disk has gone and
 * one that is only read, and a failure is tried once more. How many
 * could not be written.
 */
EXPORT INT knl_fs_sync_report( void )
{
	INT	i, bad = 0;

	for ( i = 0; ; i++ ) {
		T_MOUNT	*m;
		BOOL	go = FALSE;
		ER	er;

		tk_loc_mtx(fs_mtxid, TMO_FEVR);
		if ( i >= fs_nmount ) {
			tk_unl_mtx(fs_mtxid);
			break;
		}
		m = fs_mount[i];
		if ( m->used ) {
			if ( m->gone ) {
				tm_printf((UB *)"shutdown: %s (%s): its disk has gone, nothing written\n", mount_said(m), m->fimp->name);
			} else if ( m->fimp->sync != NULL && !mount_rdonly(m) ) {
				go = TRUE;
				m->busy++;
			}
		}
		tk_unl_mtx(fs_mtxid);
		if ( !go ) continue;

		er = mount_write(m);
		if ( er < EX_OK ) {
			tm_printf((UB *)"shutdown: %s (%s): not written (%d), once more\n", mount_said(m), m->fimp->name, (INT)er);
			er = mount_write(m);
		}
		tm_printf((UB *)"shutdown: %s (%s): %s (%d)\n", mount_said(m), m->fimp->name,
			  ( er >= EX_OK ) ? "written" : "COULD NOT BE WRITTEN", (INT)er);
		if ( er < EX_OK ) bad++;
		mount_put(m);
	}
	return bad + knl_obfile_shut(FALSE);
}

/*
 * The last of the file systems, every machine alike: the volumes of
 * objects left clean and read only (the system volume cannot be closed,
 * the kernel keeps objects of it open), then every other mount that can
 * be written taken down, which takes the in-use mark off it and leaves it
 * out of the device's cache. One with a file still open is written and
 * left, and told; the next start looks it over. 'keep1' and 'keep2' are
 * paths left for the caller, who has something still to write there; this
 * is called again for them. Called with the devices still working.
 */
EXPORT void knl_fs_finish( CONST char *keep1, CONST char *keep2 )
{
	INT	i, k, open, down = 0, left = 0;
	ER	er;

	if ( fs_mtxid <= 0 ) {
		return;
	}
	(void)knl_obfile_shut(TRUE);

	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	for ( i = 0; i < fs_nmount; i++ ) {
		T_MOUNT	*m = fs_mount[i];

		if ( !m->used || m->gone || m->dd <= 0 || mount_rdonly(m) ) {
			continue;
		}
		if ( ( keep1 != NULL && str_same((CONST char *)m->path, keep1) )
		  || ( keep2 != NULL && str_same((CONST char *)m->path, keep2) ) ) {
			continue;
		}
		if ( mount_idle(m) ) {
			tm_printf((UB *)"shutdown: %s (%s): taken down\n", mount_said(m), m->fimp->name);
			mount_free(m);
			down++;
			continue;
		}
		for ( k = 0, open = 0; k < FS_MAX_FILE; k++ ) {
			if ( fs_file[k].used && fs_file[k].mount == m ) open++;
		}
		er = ( m->fimp->sync != NULL ) ? mount_write(m) : EX_OK;
		tm_printf((UB *)"shutdown: %s (%s): %d files still open, written (%d) and left marked in use\n",
			  mount_said(m), m->fimp->name, open, (INT)er);
		left++;
	}
	tk_unl_mtx(fs_mtxid);
	tm_printf((UB *)"shutdown: %d mounts taken down, %d left\n", down, left);
}


/* ---------------------------------------------------------------- a disk reached through its key */

/*
 * The volume of a disk reached from its device object's key, with no
 * path of its own (fs_opn_vol, design 7.2). The mount has an empty mount
 * point, so no path reaches it; its root is a descriptor (vd), and the
 * paths of the *at calls go below that root only. The mount goes when
 * the last descriptor of it is closed.
 */

/*
 * A path below a volume's root made plain into buf ("/a/b"): "." taken
 * out, ".." taking out the name before it. A ".." that would leave the
 * root is refused (EX_ACCES): nothing above the vd's root is reached,
 * whatever the path says.
 */
LOCAL ER below( CONST char *path, char *buf )
{
	INT	n = 1, i = 0, k, j;

	buf[0] = '/';
	buf[1] = '\0';
	if ( path == NULL ) {
		return EX_INVAL;
	}
	while ( path[i] != '\0' ) {
		while ( path[i] == '/' ) i++;
		if ( path[i] == '\0' ) break;
		for ( k = 0; path[i + k] != '\0' && path[i + k] != '/'; k++ ) ;
		if ( k == 1 && path[i] == '.' ) {
			i += k;
			continue;
		}
		if ( k == 2 && path[i] == '.' && path[i + 1] == '.' ) {
			if ( n == 1 ) {
				return EX_ACCES;		/* above the volume's root */
			}
			while ( n > 1 && buf[n - 1] != '/' ) n--;
			if ( n > 1 ) n--;
			buf[n] = '\0';
			i += k;
			continue;
		}
		if ( n + 1 + k >= FS_PATH_MAX ) {
			return EX_NAMETOOLONG;
		}
		if ( n > 1 ) buf[n++] = '/';
		for ( j = 0; j < k; j++ ) buf[n++] = path[i + j];
		buf[n] = '\0';
		i += k;
	}
	return EX_OK;
}

/*
 * The mount of a vd, held busy until mount_put, and its path below the
 * root into rest. EX_BADF when vd is not one, EX_IO when the disk went,
 * EX_ROFS when it is to change something and the vd only reads.
 */
LOCAL T_MOUNT *vd_mount( INT vd, CONST char *path, BOOL change, char *rest, ER *p_er )
{
	T_FILE	*f = fd_get(vd);
	T_MOUNT	*m = NULL;
	ER	er;

	if ( f == NULL ) {
		*p_er = EX_BADF;
		return NULL;
	}
	er = ( ( f->oflags & FS_O_VOLUME ) == 0 ) ? EX_BADF : below(path, rest);
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	if ( er >= EX_OK ) {
		if ( f->mount->gone ) {
			er = EX_IO;
		} else if ( change && ( ( f->oflags & O_ACCMODE ) == O_RDONLY || mount_rdonly(f->mount) ) ) {
			er = EX_ROFS;
		} else {
			m = f->mount;
			m->busy++;
		}
	}
	tk_unl_mtx(fs_mtxid);
	fd_put(f);
	*p_er = er;
	return m;
}

/* A descriptor of a file opened on mount m at rest, for owner. Called without the lock, m held busy */
LOCAL INT open_on( T_MOUNT *m, CONST char *rest, UINT oflags, ID owner )
{
	T_FILE	*f = NULL;
	INT	fd = -1;
	ER	er;

	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	f = file_new();
	if ( f != NULL ) {
		fd = slot_new(f, owner);
		if ( fd < 0 ) f->used = FALSE;
	}
	if ( fd < 0 ) {
		tk_unl_mtx(fs_mtxid);
		return EX_MFILE;
	}
	f->owner  = owner;
	f->mount  = m;
	f->oflags = oflags & ~(UINT)FS_O_VOLUME;
	f->busy   = 1;
	tk_unl_mtx(fs_mtxid);

	er = ( m->fimp->open != NULL ) ? m->fimp->open(m, rest, f->oflags, f) : EX_NOTSUP;
	if ( er < EX_OK ) {
		tk_loc_mtx(fs_mtxid, TMO_FEVR);
		fs_slot[fd].used = FALSE;
		fs_slot[fd].f = NULL;
		f->owner = 0;
		f->used = FALSE;
		mount_reap(m);
		tk_unl_mtx(fs_mtxid);
		return (INT)er;
	}
	if ( (oflags & O_APPEND) != 0 ) {
		f->offset = f->size;
	}
	fd_put(f);
	return fd;
}

EXPORT INT knl_fs_open_vol( CONST char *fimpnm, CONST char *devnm, UINT flags, ID owner )
{
	T_MOUNT	*m;
	BOOL	write = (BOOL)( ( flags & FS_MNT_RDONLY ) == 0 );
	INT	fd;
	ER	er = EX_OK;

	if ( devnm == NULL || fimpnm == NULL ) {
		return EX_INVAL;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	m = mount_by(NULL, devnm);
	if ( m != NULL && write && mount_rdonly(m) ) {
		/* read only so far: made writable when nothing of it is open */
		if ( !mount_idle(m) ) {
			er = EX_BUSY;
		} else {
			char	path[FS_PATH_MAX];
			UINT	mf = m->flags & ~(UINT)FS_MNT_RDONLY;
			CONST char *fn = m->fimp->name;

			str_ncpy(path, (CONST char *)m->path, FS_PATH_MAX);
			mount_free(m);
			tk_unl_mtx(fs_mtxid);
			er = attach(fn, devnm, ( path[0] != '\0' ) ? path : "", mf, TRUE);
			tk_loc_mtx(fs_mtxid, TMO_FEVR);
			m = ( er >= EX_OK ) ? mount_by(NULL, devnm) : NULL;
		}
	}
	if ( m != NULL ) m->busy++;
	tk_unl_mtx(fs_mtxid);
	if ( er < EX_OK ) {
		return er;
	}
	if ( m == NULL ) {
		er = attach(fimpnm, devnm, "", flags, TRUE);
		if ( er < EX_OK ) {
			return er;
		}
		tk_loc_mtx(fs_mtxid, TMO_FEVR);
		m = mount_by(NULL, devnm);
		if ( m != NULL ) m->busy++;
		tk_unl_mtx(fs_mtxid);
		if ( m == NULL ) {
			return EX_IO;
		}
	}
	fd = open_on(m, "/", O_RDONLY | O_DIRECTORY, owner);
	if ( fd >= 0 ) {
		T_FILE	*f = fd_get(fd);

		if ( f != NULL ) {
			f->oflags = ( write ? O_RDWR : O_RDONLY ) | O_DIRECTORY | FS_O_VOLUME;
			fd_put(f);
		}
	}
	mount_put(m);
	return fd;
}

EXPORT INT knl_fs_openat_as( INT vd, CONST char *path, UINT oflags, ID owner )
{
	char	rest[FS_PATH_MAX];
	T_MOUNT	*m;
	INT	fd;
	ER	er;

	m = vd_mount(vd, path, (BOOL)( ( oflags & O_ACCMODE ) != O_RDONLY
				      || ( oflags & ( O_CREAT | O_TRUNC ) ) != 0 ), rest, &er);
	if ( m == NULL ) {
		return er;
	}
	fd = open_on(m, rest, oflags, owner);
	mount_put(m);
	return fd;
}

EXPORT INT fs_openat( INT vd, CONST char *path, UINT oflags )
{
	return knl_fs_openat_as(vd, path, oflags, 0);
}

EXPORT ER fs_statat( INT vd, CONST char *path, T_FSTAT *st )
{
	char	rest[FS_PATH_MAX];
	T_MOUNT	*m;
	ER	er = EX_OK;

	if ( st == NULL ) return EX_INVAL;
	m = vd_mount(vd, path, FALSE, rest, &er);
	if ( m == NULL ) return er;
	er = ( m->fimp->stat != NULL ) ? m->fimp->stat(m, rest, st) : EX_NOTSUP;
	if ( er >= EX_OK ) mount_obj(m, &st->dev);
	mount_put(m);
	return er;
}

/* A path operation of a vd's disk: the fimp's mkdir, rmdir or unlink on the path below the root */
#define VD_MKDIR	0
#define VD_RMDIR	1
#define VD_UNLINK	2

LOCAL ER vd_path_op( INT vd, CONST char *path, INT op )
{
	char	rest[FS_PATH_MAX];
	T_MOUNT	*m;
	ER	(*fn)( T_MOUNT *m, CONST char *path );
	ER	er;

	m = vd_mount(vd, path, TRUE, rest, &er);
	if ( m == NULL ) return er;
	fn = ( op == VD_MKDIR ) ? m->fimp->mkdir : ( op == VD_RMDIR ) ? m->fimp->rmdir : m->fimp->unlink;
	er = ( fn != NULL ) ? fn(m, rest) : EX_NOTSUP;
	mount_put(m);
	return er;
}

EXPORT ER fs_mkdirat( INT vd, CONST char *path )
{
	return vd_path_op(vd, path, VD_MKDIR);
}

EXPORT ER fs_rmdirat( INT vd, CONST char *path )
{
	return vd_path_op(vd, path, VD_RMDIR);
}

EXPORT ER fs_unlinkat( INT vd, CONST char *path )
{
	return vd_path_op(vd, path, VD_UNLINK);
}

EXPORT ER fs_utimeat( INT vd, CONST char *path, UD mtime )
{
	char	rest[FS_PATH_MAX];
	T_MOUNT	*m;
	ER	er;

	m = vd_mount(vd, path, TRUE, rest, &er);
	if ( m == NULL ) return er;
	er = ( m->fimp->utime != NULL ) ? m->fimp->utime(m, rest, mtime) : EX_NOTSUP;
	mount_put(m);
	return er;
}

/* A name changed within the vd's disk */
EXPORT ER fs_renameat( INT vd, CONST char *from, CONST char *to )
{
	char	rf[FS_PATH_MAX], rt[FS_PATH_MAX];
	T_MOUNT	*m;
	ER	er;

	m = vd_mount(vd, from, TRUE, rf, &er);
	if ( m == NULL ) return er;
	er = below(to, rt);
	if ( er >= EX_OK ) {
		er = ( m->fimp->rename != NULL ) ? m->fimp->rename(m, rf, rt) : EX_NOTSUP;
	}
	mount_put(m);
	return er;
}

EXPORT ER fs_fstatvfs( INT vd, T_FSSTAT *st )
{
	char	rest[FS_PATH_MAX];
	T_MOUNT	*m;
	ER	er;

	if ( st == NULL ) return EX_INVAL;
	m = vd_mount(vd, "", FALSE, rest, &er);
	if ( m == NULL ) return er;
	er = ( m->fimp->statvfs != NULL ) ? m->fimp->statvfs(m, st) : EX_NOTSUP;
	mount_put(m);
	return er;
}

EXPORT ER fs_fsync( INT fd )
{
	T_FILE	*f = fd_get(fd);
	T_MOUNT	*m;
	ER	er;

	if ( f == NULL ) {
		return EX_BADF;
	}
	m = f->mount;
	if ( GONE(f) ) {
		er = EX_IO;
	} else if ( m->fimp->fsync != NULL ) {
		er = m->fimp->fsync(f);
	} else if ( m->fimp->sync != NULL && !mount_rdonly(m) ) {
		er = m->fimp->sync(m);
	} else {
		er = EX_OK;
	}
	fd_put(f);
	return er;
}

/* ---------------------------------------------------------------- the mounts */

/*
 * The live mounts, a copy of the table. The device object of each is
 * asked of the device manager once the table's lock is let go.
 */
EXPORT INT fs_mounts( T_FSMNT *buf, INT max )
{
	INT	i, n = 0;

	if ( max < 0 || ( max > 0 && buf == NULL ) ) {
		return EX_INVAL;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	for ( i = 0; i < fs_nmount; i++ ) {
		T_MOUNT	*m = fs_mount[i];

		if ( !m->used || m->gone ) continue;
		if ( n < max ) {
			mnt_copy(&buf[n], m);
		}
		n++;
	}
	tk_unl_mtx(fs_mtxid);

	for ( i = 0; i < n && i < max; i++ ) {
		if ( buf[i].dev[0] != 0 ) {
			(void)knl_obdev_uuid(buf[i].dev, &buf[i].obj);
		}
	}
	return n;
}

/*
 * FS_MEDIA_DIR, a slash and one plain name: not empty, not "." or "..",
 * with no slash in it and short enough to be a device's name, which is
 * what the name there is.
 */
EXPORT BOOL knl_fs_media_path( CONST char *path )
{
	CONST char	*dir = FS_MEDIA_DIR;
	INT		i, n;

	if ( path == NULL ) {
		return FALSE;
	}
	for ( i = 0; dir[i] != '\0'; i++ ) {
		if ( path[i] != dir[i] ) return FALSE;
	}
	if ( path[i++] != '/' ) {
		return FALSE;
	}
	path += i;
	for ( n = 0; path[n] != '\0'; n++ ) {
		if ( path[n] == '/' || n >= FS_DEVNM_MAX - 1 ) return FALSE;
	}
	if ( n == 0 || ( n == 1 && path[0] == '.' ) || ( n == 2 && path[0] == '.' && path[1] == '.' ) ) {
		return FALSE;
	}
	return TRUE;
}

/* How many entries of the mount table have been made, and how many are in use now */
EXPORT INT knl_fs_slots( INT *p_used )
{
	INT	i, n, used = 0;

	if ( fs_mtxid <= 0 ) {
		if ( p_used != NULL ) *p_used = 0;
		return 0;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	n = fs_nmount;
	for ( i = 0; i < n; i++ ) {
		if ( fs_mount[i]->used ) used++;
	}
	tk_unl_mtx(fs_mtxid);
	if ( p_used != NULL ) *p_used = used;
	return n;
}

#ifdef USE_KTEST
/* For the tests: fn on the mount of path, held busy while it runs */
EXPORT ER knl_fs_on_mount( CONST char *path, ER (*fn)( T_MOUNT *m, void *arg ), void *arg )
{
	CONST char	*rest;
	T_MOUNT		*m;
	ER		er;

	if ( path == NULL || fn == NULL ) {
		return EX_INVAL;
	}
	m = mount_get(path, &rest);
	if ( m == NULL ) {
		return EX_NOENT;
	}
	er = m->gone ? EX_IO : fn(m, arg);
	mount_put(m);

	return er;
}
#endif
