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

#define FS_MAX_FIMP	4

LOCAL CONST T_FIMP	*fs_fimp[FS_MAX_FIMP];
LOCAL INT		fs_nfimp = 0;
LOCAL T_MOUNT		fs_mount[FS_MAX_MOUNT];
LOCAL T_FILE		fs_file[FS_MAX_FILE];
LOCAL ID		fs_mtxid = 0;		/* mount table and descriptors */

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

LOCAL T_FILE *fd_to_file( INT fd )
{
	if ( fd < 0 || fd >= FS_MAX_FILE || !fs_file[fd].used ) {
		return NULL;
	}
	return &fs_file[fd];
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
	for ( i = 0; i < FS_MAX_MOUNT; i++ ) {
		T_MOUNT *m = &fs_mount[i];
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
	if ( m->fimp->unmount != NULL ) m->fimp->unmount(m);
	if ( m->dd > 0 ) tk_cls_dev(m->dd, 0);
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

/* A mount whose device went, taken down once its last user is done. Under the lock */
LOCAL void mount_reap( T_MOUNT *m )
{
	if ( m->used && m->gone && mount_idle(m) ) {
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
	INT	i;

	if ( fs_mtxid > 0 ) {
		return E_OK;
	}
	for ( i = 0; i < FS_MAX_MOUNT; i++ ) {
		fs_mount[i].used = FALSE;
		fs_mount[i].gone = FALSE;
	}
	for ( i = 0; i < FS_MAX_FILE; i++ ) fs_file[i].used = FALSE;
	fs_nfimp = 0;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	fs_mtxid = tk_cre_mtx(&cmtx);
	if ( fs_mtxid <= 0 ) {
		return (ER)fs_mtxid;
	}

	return E_OK;
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

	if ( fimpnm == NULL || path == NULL || path[0] != '/' ) {
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

	for ( i = 0; i < FS_MAX_MOUNT; i++ ) {
		CONST char	*d = (CONST char *)fs_mount[i].dev;

		if ( fs_mount[i].used && ( str_same((CONST char *)fs_mount[i].path, path)
		  || ( devnm != NULL && ( str_same(d, devnm)
					|| ( whole && d[0] != '\0' && dev_overlap(d, devnm) ) ) ) ) ) {
			er = EX_BUSY;
			goto exit;
		}
		if ( !fs_mount[i].used && m == NULL ) m = &fs_mount[i];
	}
	if ( m == NULL ) {
		er = EX_NOSPC;
		goto exit;
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

	for ( i = 0; i < FS_MAX_MOUNT; i++ ) {
		T_MOUNT	*m = &fs_mount[i];

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
	for ( i = 0; i < FS_MAX_MOUNT; i++ ) {
		T_MOUNT	*m = &fs_mount[i];

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
	INT		i, n, fd = -1;
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
	for ( i = 0, n = 0; i < FS_MAX_FILE; i++ ) {
		if ( !fs_file[i].used ) {
			if ( f == NULL ) { f = &fs_file[i]; fd = i; }
		} else if ( owner > 0 && fs_file[i].owner == owner ) {
			n++;
		}
	}
	if ( f == NULL || n >= FS_PRC_FILE ) {
		tk_unl_mtx(fs_mtxid);
		return EX_MFILE;
	}
	f->used   = TRUE;		/* reserved while the fimp works */
	f->owner  = owner;
	f->mount  = m;
	f->oflags = oflags;
	f->offset = 0;
	f->size   = 0;
	f->mode   = FS_IFREG;
	f->ino    = 0;
	f->priv   = 0;
	f->hint_idx  = 0;		/* it has not been read along yet */
	f->hint_clus = 0;
	tk_unl_mtx(fs_mtxid);

	er = ( m->fimp->open != NULL ) ? m->fimp->open(m, rest, oflags, f) : EX_NOTSUP;
	if ( er < EX_OK ) {
		f->owner = 0;
		f->used = FALSE;
		return (INT)er;
	}
	if ( (oflags & O_APPEND) != 0 ) {
		f->offset = f->size;
	}

	return fd;
}

EXPORT INT fs_open( CONST char *path, UINT oflags )
{
	return knl_fs_open_as(path, oflags, 0);
}

/*
 * A descriptor on a mount that is gone is let go without the fimp,
 * whose volume is no longer there; EX_IO says that what was written
 * last may not have reached it. The last one lets the mount go.
 */
EXPORT ER fs_close( INT fd )
{
	T_FILE	*f = fd_to_file(fd);
	T_MOUNT	*m;
	ER	er;

	if ( f == NULL ) {
		return EX_BADF;
	}
	m = f->mount;
	if ( m->gone ) {
		er = EX_IO;
	} else {
		er = ( m->fimp->close != NULL ) ? m->fimp->close(f) : EX_OK;
	}
	tk_loc_mtx(fs_mtxid, TMO_FEVR);
	f->owner = 0;
	f->used = FALSE;
	mount_reap(m);
	tk_unl_mtx(fs_mtxid);

	return er;
}

/* Whether a descriptor is open and belongs to that owner */
EXPORT BOOL knl_fs_mine( INT fd, ID owner )
{
	T_FILE	*f = fd_to_file(fd);

	return ( f != NULL && f->owner == owner );
}

/* A process is ending: whatever it left open is closed */
EXPORT void knl_fs_prc_end( ID pid )
{
	INT	fd;

	if ( pid <= 0 || fs_mtxid <= 0 ) {
		return;
	}
	for ( fd = 0; fd < FS_MAX_FILE; fd++ ) {
		if ( knl_fs_mine(fd, pid) ) {
			(void)fs_close(fd);
		}
	}
}

/* The descriptor's mount went with its device: nothing is asked of the fimp */
#define GONE(f)		( (f)->mount->gone )

EXPORT INT fs_read( INT fd, void *buf, SZ len )
{
	T_FILE	*f = fd_to_file(fd);

	if ( f == NULL || buf == NULL ) {
		return EX_BADF;
	}
	if ( GONE(f) ) {
		return EX_IO;
	}
	if ( (f->oflags & O_ACCMODE) == O_WRONLY ) {
		return EX_BADF;
	}
	if ( f->mode == FS_IFDIR ) {
		return EX_ISDIR;
	}
	if ( len == 0 ) {
		return 0;
	}
	return ( f->mount->fimp->read != NULL ) ? f->mount->fimp->read(f, buf, len) : EX_NOTSUP;
}

EXPORT INT fs_write( INT fd, CONST void *buf, SZ len )
{
	T_FILE	*f = fd_to_file(fd);

	if ( f == NULL || buf == NULL ) {
		return EX_BADF;
	}
	if ( GONE(f) ) {
		return EX_IO;
	}
	if ( (f->oflags & O_ACCMODE) == O_RDONLY ) {
		return EX_BADF;
	}
	if ( f->mode == FS_IFDIR ) {
		return EX_ISDIR;
	}
	if ( len == 0 ) {
		return 0;
	}
	return ( f->mount->fimp->write != NULL ) ? f->mount->fimp->write(f, buf, len) : EX_NOTSUP;
}

EXPORT D fs_lseek( INT fd, D offset, INT whence )
{
	T_FILE	*f = fd_to_file(fd);
	D	pos;

	if ( f == NULL ) {
		return EX_BADF;
	}
	if ( GONE(f) ) {
		return EX_IO;
	}
	switch ( whence ) {
	  case SEEK_SET_:	pos = offset;				break;
	  case SEEK_CUR_:	pos = (D)f->offset + offset;		break;
	  case SEEK_END_:	pos = (D)f->size + offset;		break;
	  default:		return EX_INVAL;
	}
	if ( pos < 0 ) {
		return EX_INVAL;
	}
	f->offset = (UD)pos;

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
	er = ( m->fimp->stat != NULL ) ? m->fimp->stat(m, rest, st) : EX_NOTSUP;
	if ( er >= EX_OK ) {
		mount_obj(m, &st->dev);
	}
	mount_put(m);

	return er;
}

EXPORT ER fs_fstat( INT fd, T_FSTAT *st )
{
	T_FILE	*f = fd_to_file(fd);

	if ( f == NULL || st == NULL ) {
		return EX_BADF;
	}
	if ( GONE(f) ) {
		return EX_IO;
	}
	st->mode  = f->mode | FS_IRWXU;
	st->size  = f->size;
	st->mtime = 0;
	st->ino   = f->ino;
	mount_obj(f->mount, &st->dev);

	return EX_OK;
}

EXPORT ER fs_ftruncate( INT fd, UD len )
{
	T_FILE	*f = fd_to_file(fd);

	if ( f == NULL ) {
		return EX_BADF;
	}
	if ( (f->oflags & O_ACCMODE) == O_RDONLY ) {
		return EX_BADF;
	}
	if ( GONE(f) ) {
		return EX_IO;
	}
	return ( f->mount->fimp->truncate != NULL ) ? f->mount->fimp->truncate(f, len) : EX_NOTSUP;
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
	T_FILE	*f = fd_to_file(fd);

	if ( f == NULL || buf == NULL || nent <= 0 ) {
		return EX_BADF;
	}
	if ( GONE(f) ) {
		return EX_IO;
	}
	if ( f->mode != FS_IFDIR ) {
		return EX_NOTDIR;
	}
	return ( f->mount->fimp->getdents != NULL ) ? f->mount->fimp->getdents(f, buf, nent) : EX_NOTSUP;
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

	for ( i = 0; i < FS_MAX_MOUNT; i++ ) {
		T_MOUNT *m = &fs_mount[i];
		BOOL	go;

		tk_loc_mtx(fs_mtxid, TMO_FEVR);
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
	for ( i = 0; i < FS_MAX_MOUNT; i++ ) {
		T_MOUNT	*m = &fs_mount[i];

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
