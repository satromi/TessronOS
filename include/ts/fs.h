/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	fs.h
 *	File management (T2EX fs_, design 12.2)
 *
 *	The interface follows T-Kernel 2.0 Extension chapter 4: a small
 *	common layer keeps the mount table and the descriptors, and each
 *	file system is a fimp (file system implementation) behind the
 *	function table below.
 *
 *	This is the first stage: the calls the process loader and the
 *	tests need. Offsets and sizes are 64 bit throughout.
 */

#ifndef __TS_FS_H__
#define __TS_FS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <ts/uuid.h>

/*
 * A name of FAT may be 255 UTF-16 units long, which is at most 765 bytes
 * of UTF-8; a path holds a few of them.
 */
#define FS_PATH_MAX		1024		/* bytes of a path, NUL included */
#define FS_NAME_MAX		768		/* bytes of one name component, NUL included */
#define FS_DEVNM_MAX		16		/* bytes of a device name, NUL included */
#define FS_FIMPNM_MAX		16		/* bytes of a fimp name, NUL included */

#define FS_MAX_MOUNT		8		/* mounts at a time */
#define FS_MAX_FILE		4096		/* descriptors, the whole system's */
#define FS_PRC_FILE		1024		/* of them one process may hold at a time */

/*
 * Where the volume of a disk device object is mounted: this directory,
 * a slash and the device's name ("/media/uda0"). The kernel mounts
 * anywhere by device name (fs_attach); a program mounts only through
 * the device object (fs_attach_dev).
 */
#define FS_MEDIA_DIR		"/media"

/*
 * Error codes (T2EX uses negated errno values)
 */
#define EX_OK			0
#define EX_PERM			(-1)
#define EX_NOENT		(-2)
#define EX_IO			(-5)
#define EX_BADF			(-9)
#define EX_NOMEM		(-12)
#define EX_ACCES		(-13)
#define EX_FAULT		(-14)		/* a buffer of the caller's it may not use */
#define EX_NOTBLK		(-15)		/* not the object of a disk */
#define EX_BUSY			(-16)
#define EX_EXIST		(-17)
#define EX_NOTDIR		(-20)
#define EX_ISDIR		(-21)
#define EX_INVAL		(-22)
#define EX_MFILE		(-24)
#define EX_FBIG			(-27)
#define EX_NOSPC		(-28)
#define EX_ROFS			(-30)
#define EX_NAMETOOLONG		(-36)
#define EX_NOTEMPTY		(-39)
#define EX_NOTSUP		(-95)
#define EX_INTR			(-4)

/*
 * Open flags (fs_open)
 */
#define O_RDONLY		0x0000
#define O_WRONLY		0x0001
#define O_RDWR			0x0002
#define O_ACCMODE		0x0003
#define O_CREAT			0x0100
#define O_EXCL			0x0200
#define O_TRUNC			0x0400
#define O_APPEND		0x0800
#define O_DIRECTORY		0x1000

/*
 * Mount flags (fs_attach, fs_attach_dev)
 */
#define FS_MNT_RDONLY		0x0001		/* nothing is written: the device is opened for reading */

/*
 * Whence (fs_lseek)
 */
#define SEEK_SET_		0
#define SEEK_CUR_		1
#define SEEK_END_		2

/*
 * File kinds and mode bits
 */
#define FS_IFMT			0xF000
#define FS_IFREG		0x8000
#define FS_IFDIR		0x4000
#define FS_IRWXU		0x01C0

typedef struct {
	UINT	mode;			/* FS_IFREG / FS_IFDIR plus permissions */
	UD	size;			/* bytes */
	UD	mtime;			/* seconds since 1985-01-01 UTC (TS_TIME) */
	UD	ino;			/* first cluster, or another fimp key */
	TS_UUID	dev;			/* the device object of the volume, zero for none */
} T_FSTAT;

typedef struct {
	UD	ino;
	UINT	mode;			/* FS_IFREG / FS_IFDIR */
	UD	size;
	UB	name[FS_NAME_MAX];
} T_DIRENT;

typedef struct {
	UD	blocks;			/* total blocks of the volume */
	UD	bfree;			/* free blocks */
	UD	bsize;			/* bytes in a block */
} T_FSSTAT;

/* One mount, as fs_mounts tells it */
typedef struct {
	UB	path[FS_PATH_MAX];	/* mount point, "/media/uda0" */
	UB	fimp[FS_FIMPNM_MAX];	/* "fatfs" */
	UB	dev[FS_DEVNM_MAX];	/* "uda0", empty for none */
	UINT	flags;			/* FS_MNT_* */
	TS_UUID	obj;			/* the device object, zero for none */
} T_FSMNT;

/* Forward declarations of the fimp interface */
typedef struct fs_mount	T_MOUNT;
typedef struct fs_file	T_FILE;

typedef struct fs_fimp {
	CONST char *name;
	ER	(*mount)( T_MOUNT *m );
	ER	(*unmount)( T_MOUNT *m );
	ER	(*open)( T_MOUNT *m, CONST char *path, UINT oflags, T_FILE *f );
	ER	(*close)( T_FILE *f );
	INT	(*read)( T_FILE *f, void *buf, SZ len );
	INT	(*write)( T_FILE *f, CONST void *buf, SZ len );
	ER	(*truncate)( T_FILE *f, UD len );
	ER	(*stat)( T_MOUNT *m, CONST char *path, T_FSTAT *st );
	INT	(*getdents)( T_FILE *f, T_DIRENT *buf, INT nent );
	ER	(*mkdir)( T_MOUNT *m, CONST char *path );
	ER	(*rmdir)( T_MOUNT *m, CONST char *path );
	ER	(*unlink)( T_MOUNT *m, CONST char *path );
	ER	(*rename)( T_MOUNT *m, CONST char *from, CONST char *to );
	ER	(*statvfs)( T_MOUNT *m, T_FSSTAT *st );
	ER	(*sync)( T_MOUNT *m );
	ER	(*utime)( T_MOUNT *m, CONST char *path, UD mtime );	/* NULL: times are not set */
} T_FIMP;

struct fs_mount {
	CONST T_FIMP *fimp;
	UB	path[FS_PATH_MAX];	/* mount point, "/boot" */
	INT	pathlen;
	ID	dd;			/* open descriptor of the block device */
	UB	dev[FS_DEVNM_MAX];	/* its name, empty for none */
	UINT	flags;			/* FS_MNT_* */
	void	*exinf;			/* private data of the fimp */
	INT	busy;			/* calls at work on it through a path */
	BOOL	gone;			/* its device or medium went: nothing reaches it,
					   and it is let go when the last user is done */
	TS_UUID	obj;			/* its device's object, once asked (objok) */
	BOOL	objok;
	BOOL	used;
};

struct fs_file {
	T_MOUNT	*mount;
	UINT	oflags;
	UD	offset;
	UINT	mode;			/* FS_IFREG / FS_IFDIR */
	UD	size;
	UD	ino;			/* first cluster */
	UD	dir_sect;		/* sector of the directory entry */
	UINT	dir_off;		/* offset of the entry in that sector */
	UD	priv;			/* fimp scratch (directory scan position) */

	/*
	 * Where this file got to in its own chain of clusters: the number
	 * of the cluster and which one along it is. Reading or writing
	 * carries on from there instead of walking from the first
	 * cluster again.
	 *
	 * Without it, reading a file of a few megabytes a sector at a
	 * time walks the whole chain for every sector: the work goes up
	 * with the square of the size, and writing a screenful of pixels
	 * to a file takes minutes. The hint costs two words.
	 */
	UD	hint_idx;		/* which cluster along, 0 for none */
	UD	hint_clus;
	ID	owner;			/* the process it was opened for, 0 for the kernel */
	BOOL	used;
};

/*
 * Common layer (kernel/fs/fs.c)
 */
IMPORT ER  fs_main( void );				/* start the file system layer */
IMPORT ER  fs_regist( CONST T_FIMP *fimp );		/* register a fimp */
IMPORT ER  fs_attach( CONST char *fimpnm, CONST char *devnm, CONST char *path, UINT flags );
IMPORT ER  fs_detach( CONST char *path );		/* both the kernel's only */
IMPORT INT fs_open( CONST char *path, UINT oflags );	/* returns a descriptor */
IMPORT ER  fs_close( INT fd );
IMPORT INT fs_read( INT fd, void *buf, SZ len );
IMPORT INT fs_write( INT fd, CONST void *buf, SZ len );
IMPORT D   fs_lseek( INT fd, D offset, INT whence );
IMPORT ER  fs_stat( CONST char *path, T_FSTAT *st );
IMPORT ER  fs_fstat( INT fd, T_FSTAT *st );
IMPORT ER  fs_truncate( CONST char *path, UD len );
IMPORT ER  fs_ftruncate( INT fd, UD len );
IMPORT INT fs_getdents( INT fd, T_DIRENT *buf, INT nent );
IMPORT ER  fs_mkdir( CONST char *path );
IMPORT ER  fs_rmdir( CONST char *path );
IMPORT ER  fs_unlink( CONST char *path );
IMPORT ER  fs_rename( CONST char *from, CONST char *to );

/* The time a file was last written set (TS_TIME, seconds since 1985-01-01 UTC) */
IMPORT ER  fs_utime( CONST char *path, UD mtime );
IMPORT ER  fs_statvfs( CONST char *path, T_FSSTAT *st );
IMPORT ER  fs_sync( void );

/*
 * Mounting through a device object (design 12.2.3): the volume of the
 * disk whose object devkey names, at FS_MEDIA_DIR "/" and the device's
 * name. The key is the caller's and must allow reading record 1 (the
 * blocks) for FS_MNT_RDONLY, reading and writing it otherwise, just as
 * reading and writing the blocks through the key would; that is all the
 * permission there is. The mount shows in the object's attributes
 * (tessronos.mount) and each change is told with OB_E_CHANGE on it. When
 * the device or its medium goes the mount goes with it: descriptors
 * still open answer EX_IO, and the mount point is free once they are
 * closed.
 */
IMPORT ER  fs_attach_dev( ID devkey, CONST char *fimpnm, UINT flags );
IMPORT ER  fs_detach_dev( ID devkey );

/*
 * The mounts there are, up to max of them into buf; answers how many
 * there are. A copy of the mount table, each with its device object.
 * T2EX has no call to list them.
 */
IMPORT INT fs_mounts( T_FSMNT *buf, INT max );

/*
 * Descriptors and their owners. One opened for a process is that
 * process's alone and is closed when the process ends; the kernel's own
 * (owner 0) are out of a process's reach. A process holds at most
 * FS_PRC_FILE at a time (EX_MFILE), so that one cannot take them all.
 * Each is an alias of a file on a volume: fs_fstat names the volume's
 * device object in T_FSTAT.dev.
 */
IMPORT INT  knl_fs_open_as( CONST char *path, UINT oflags, ID owner );
IMPORT BOOL knl_fs_mine( INT fd, ID owner );
IMPORT void knl_fs_prc_end( ID pid );

/* Whether a path is FS_MEDIA_DIR "/" and one name */
IMPORT BOOL knl_fs_media_path( CONST char *path );

/*
 * What the device manager uses for fs_attach_dev and fs_detach_dev, by
 * the device's name once the key has been looked at. Attaching mounts
 * at FS_MEDIA_DIR "/" devnm and refuses a device that is, contains or
 * lies in one already mounted (EX_BUSY). Detaching takes only such a
 * mount (EX_ACCES for another, /boot). knl_fs_mount_of tells the live
 * mount of a device; knl_fs_revoke marks the mounts of a device gone
 * and answers how many there were.
 */
IMPORT ER   knl_fs_attach_media( CONST char *fimpnm, CONST char *devnm, UINT flags );
IMPORT ER   knl_fs_detach_media( CONST char *devnm );
IMPORT BOOL knl_fs_mount_of( CONST char *devnm, T_FSMNT *p_mnt );
IMPORT INT  knl_fs_revoke( CONST char *devnm );

/*
 * FAT file system (kernel/fs/fatfs.c)
 */
IMPORT CONST T_FIMP knl_fimp_fat;

#ifdef USE_KTEST
/*
 * For the tests: fn run on the mount of path, held busy meanwhile
 * (EX_NOENT for none); and the look over of a FAT volume at mount, made
 * on the mounted volume without putting anything right.
 */
IMPORT ER knl_fs_on_mount( CONST char *path, ER (*fn)( T_MOUNT *m, void *arg ), void *arg );

typedef struct {
	BOOL	inuse;			/* the volume says it carries the mark of being in use */
	BOOL	mark;			/* the disk carries it */
	BOOL	ioerr;			/* a write failed */
	UINT	copies;			/* sectors in which a copy of the table differs */
	UINT	marks;			/* rename marks left */
	BOOL	partial;		/* the look over did not reach every directory */
	UINT	bad;			/* entries and chains the look over would put right */
	UINT	lost;			/* clusters taken that no entry has */
	BOOL	freeok;			/* the free count kept is the one counted */
	UINT	files;			/* files looked at */
} T_FATVFY;

IMPORT ER knl_fat_verify( T_MOUNT *m, void *arg );	/* arg: T_FATVFY */
#endif

#ifdef __cplusplus
}
#endif

#endif /* __TS_FS_H__ */
