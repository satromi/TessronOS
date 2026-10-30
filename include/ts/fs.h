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

/*
 * Mounts at a time (CNF_MAX_VOL). A mount's entry is made the first time
 * it is wanted and kept for the next, so what is held is what has been
 * used at once, not this many. A caller that lists the mounts asks how
 * many there are first (fs_mounts(NULL, 0)).
 */
#define FS_MAX_MOUNT		4096
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
#define EX_AGAIN		(-11)		/* FS_O_NONBLOCK: nothing to read, no room to write */
#define EX_NOTTY		(-25)		/* not a terminal */
#define EX_PIPE			(-32)		/* a pipe no one reads any more */
#define EX_NXIO			(-6)		/* a terminal whose other side is gone */
#define EX_NOEXEC		(-8)		/* not a program that can be run */

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
#define FS_O_NONBLOCK		0x2000		/* a pipe or a terminal: EX_AGAIN instead of waiting */
#define FS_O_NOCTTY		0x4000		/* a terminal opened does not become the controlling one */

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
#define FS_IFCHR		0x2000		/* a device of characters: /dev, a terminal */
#define FS_IFIFO		0x1000		/* a pipe */
#define FS_IRWXU		0x01C0

typedef struct {
	UINT	mode;			/* FS_IFREG / FS_IFDIR plus permissions */
	UD	size;			/* bytes */
	UD	mtime;			/* seconds since 1985-01-01 UTC (TS_TIME) */
	UD	ino;			/* first cluster, or another fimp key */
	TS_UUID	dev;			/* the device object of the volume, zero for none */
} T_FSTAT;

/*
 * The longer answer of fs_xstat and fs_fxstat, for the POSIX layer
 * (lib/libposix): T_FSTAT and what a fimp may say beyond it -- the
 * links to it, its owner's numbers (knl_fs_uid_of), which device of
 * characters it is. What a fimp leaves 0 the layer fills in: nlink 1,
 * the owner the system (0). T_FSTAT itself keeps its size: programs
 * built before hand it to fs_stat.
 */
typedef struct {
	T_FSTAT	st;
	UINT	nlink;
	UINT	uid, gid;
	UINT	rdev;			/* FS_IFCHR: which one (devfs) */
	UD	atime, ctime;		/* TS_TIME, 0: as mtime */
	UINT	blksize;
	UINT	resv[7];
} T_FSXSTAT;

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

	/*
	 * What the POSIX layer needs besides (NULL where a fimp has none of
	 * it): the longer stat, of a path and of an open file; a request of
	 * a device (FS_IOC_*, arg in the kernel's memory); which of
	 * FS_POLL_* an open file is ready for, without waiting; a second
	 * name for a file (link); and the permission bits set (chmod).
	 */
	ER	(*xstat)( T_MOUNT *m, CONST char *path, T_FSXSTAT *st );
	ER	(*fxstat)( T_FILE *f, T_FSXSTAT *st );
	ER	(*ioctl)( T_FILE *f, UINT cmd, void *arg );
	UINT	(*poll)( T_FILE *f, UINT events );
	ER	(*link)( T_MOUNT *m, CONST char *from, CONST char *to );
	ER	(*chmod)( T_MOUNT *m, CONST char *path, UINT mode );

	/* what was written to the open file on the medium (fs_fsync); NULL: the mount's sync */
	ER	(*fsync)( T_FILE *f );
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

	/*
	 * An open file is shared: every descriptor made from it -- by
	 * fs_dup, or handed to a child (knl_fs_give) -- names this one, with
	 * its offset and flags, as POSIX's open file description. refs
	 * counts the descriptors, busy the calls at work on it; the fimp's
	 * close comes when both are gone. obj is the fimp's own.
	 */
	INT	refs;
	INT	busy;
	void	*obj;
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
 * What was written to one open file made sure to be on the medium:
 * POSIX's fsync and fdatasync. The file's own when its fimp can tell
 * (an object of the object world), else its mount's (FAT); fs_sync is
 * every mount's.
 */
IMPORT ER  fs_fsync( INT fd );

/*
 * The POSIX layer's (design 5.8): a second descriptor of the same open
 * file, a pipe, the longer stat, a request of a device, the flags of an
 * open file, waiting for several at once, a second name and the
 * permission bits.
 */
IMPORT INT fs_dup( INT fd );
IMPORT ER  fs_pipe( INT fd[2] );
IMPORT ER  fs_xstat( CONST char *path, T_FSXSTAT *st );
IMPORT ER  fs_fxstat( INT fd, T_FSXSTAT *st );
IMPORT ER  fs_ioctl( INT fd, UINT cmd, void *arg );
IMPORT INT fs_fcntl( INT fd, INT cmd, INT arg );
IMPORT ER  fs_link( CONST char *from, CONST char *to );
IMPORT ER  fs_chmod( CONST char *path, UINT mode );

/* fs_fcntl: the flags of the open file (O_APPEND and FS_O_NONBLOCK may be set) */
#define FS_F_GETFL		3
#define FS_F_SETFL		4

/* fs_poll: as POSIX's poll */
#define FS_POLL_IN		0x0001
#define FS_POLL_PRI		0x0002
#define FS_POLL_OUT		0x0004
#define FS_POLL_ERR		0x0008
#define FS_POLL_HUP		0x0010
#define FS_POLL_NVAL		0x0020

typedef struct {
	INT	fd;
	UINT	events;
	UINT	revents;		/* out */
} T_FSPOLL;

/* How many are ready (0 when the time ran out), EX_INTR on a signal; tmout in ms, TMO_FEVR */
IMPORT INT fs_poll( T_FSPOLL *fds, INT n, TMO tmout );

/*
 * Requests of a device (fs_ioctl). A terminal (devfs, design 5.8.4)
 * takes the termios ones, its size, its foreground group and becoming
 * the session's terminal; a pipe and a terminal say what can be read.
 */
#define FS_IOC_TCGETS		1	/* T_FSTERMIOS * */
#define FS_IOC_TCSETS		2	/* now */
#define FS_IOC_TCSETSW		3	/* once what was written has gone out */
#define FS_IOC_TCSETSF		4	/* and what came in thrown away */
#define FS_IOC_GWINSZ		5	/* T_FSWINSZ * */
#define FS_IOC_SWINSZ		6	/* the foreground group is told SIGWINCH */
#define FS_IOC_GPGRP		7	/* INT *: the foreground group */
#define FS_IOC_SPGRP		8	/* INT *: one of the caller's session */
#define FS_IOC_SCTTY		9	/* the caller's session's terminal from now on */
#define FS_IOC_NOTTY		10	/* and no longer */
#define FS_IOC_GPTN		11	/* INT *: the number of the pty (/dev/pts/N) */
#define FS_IOC_NREAD		12	/* INT *: bytes that can be read now */
#define FS_IOC_FLUSH		13	/* INT *: 0 what came in, 1 what goes out, 2 both */
#define FS_IOC_DRAIN		14	/* wait until what was written has been read */
#define FS_IOC_GSID		15	/* INT *: the session whose terminal it is */
#define FS_IOC_ISTTY		16	/* E_OK for a terminal, EX_NOTTY otherwise */

/* The termios of a terminal: NetBSD's flag values and indexes of c_cc */
#define FS_NCCS			20
typedef struct {
	UINT	c_iflag;
	UINT	c_oflag;
	UINT	c_cflag;
	UINT	c_lflag;
	UB	c_cc[FS_NCCS];
	INT	c_ispeed;
	INT	c_ospeed;
} T_FSTERMIOS;

typedef struct {
	UH	ws_row, ws_col;
	UH	ws_xpixel, ws_ypixel;
} T_FSWINSZ;

/*
 * Descriptors handed to another process (the child ts_spn_prc starts):
 * a new descriptor of each open file, owned by 'to', in out[] in the
 * same order. EX_BADF when one is not the owner's 'from'.
 */
IMPORT ER   knl_fs_give( CONST INT *fds, INT n, ID from, ID to, INT *out );
IMPORT INT  knl_fs_dup_as( INT fd, ID owner );
IMPORT ER   knl_fs_pipe_as( INT fd[2], ID owner );

/*
 * Something a waiter of fs_poll or of a pipe or terminal may want
 * changed: they look again. Called holding none of the layer's locks.
 */
IMPORT void knl_fs_wake( void );

/* Waiting a slice for knl_fs_wake: FALSE when the caller is to stop (a signal, its end) */
IMPORT BOOL knl_fs_wait_slice( TMO ms );

/* The kinds of file the layer keeps besides the volumes (peripheral_kernel/fs) */
IMPORT CONST T_FIMP knl_fimp_pipe;
IMPORT CONST T_FIMP knl_fimp_ttydev;	/* null, tty, ptmx ... opened (devfs.c) */
IMPORT CONST T_FIMP knl_fimp_obj;	/* the object world at "/" */

/* Mounted at the start (devinit): the object world at "/" */
IMPORT void knl_fs_posix_mounts( void );

/*
 * The devices of characters of the POSIX programs as device objects
 * (devfs.c): added at the start; whether an object is one of them; one
 * opened as a file, the open file's calls then knl_fimp_ttydev's.
 */
IMPORT void knl_ttydev_start( void );
IMPORT BOOL knl_ttydev_is( CONST TS_UUID *u );
IMPORT ER   knl_ttydev_open( CONST TS_UUID *u, UINT oflags, T_FILE *f );

/* dev (objfs.c) made to list the devices there are now; waits up to tmo ms (TMO_POL: not) */
IMPORT void knl_objfs_dev_sync( TMO tmo );

/* The program object a path of the object world names (E_NOEXS: none, EX_ISDIR...) */
IMPORT ER   knl_objfs_lookup( CONST char *path, TS_UUID *p_uuid );

/* The number POSIX shows for a user or a group object (0 the system) */
IMPORT UINT knl_fs_uid_of( CONST TS_UUID *who );

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
 * A disk's file system reached from its device object's key with no
 * path in the tree (design 7.2): the root of the disk as a descriptor
 * (vd), from which its files are opened and its names changed by paths
 * below that root only ("." and ".." are folded, and one that would
 * leave the root is refused, EX_ACCES). The key's rights are looked at
 * as for fs_attach_dev; a vd opened FS_MNT_RDONLY changes nothing
 * (EX_ROFS), whatever others opened the disk for. Those who open the
 * same disk share its mount, with a path of FS_MEDIA_DIR as well; the
 * mount goes with the last descriptor of it. When the disk goes, the
 * vd and what was opened from it answer EX_IO and close.
 */
IMPORT INT fs_opn_vol( ID devkey, CONST char *fimpnm, UINT flags );
IMPORT INT fs_openat( INT vd, CONST char *path, UINT oflags );
IMPORT ER  fs_statat( INT vd, CONST char *path, T_FSTAT *st );
IMPORT ER  fs_mkdirat( INT vd, CONST char *path );
IMPORT ER  fs_rmdirat( INT vd, CONST char *path );
IMPORT ER  fs_unlinkat( INT vd, CONST char *path );
IMPORT ER  fs_utimeat( INT vd, CONST char *path, UD mtime );
IMPORT ER  fs_renameat( INT vd, CONST char *from, CONST char *to );
IMPORT ER  fs_fstatvfs( INT vd, T_FSSTAT *st );

/* The same for the device manager and the gate: by the device's name, for owner */
IMPORT INT knl_fs_open_vol( CONST char *fimpnm, CONST char *devnm, UINT flags, ID owner );
IMPORT INT knl_fs_openat_as( INT vd, CONST char *path, UINT oflags, ID owner );
IMPORT INT knl_fs_opn_vol_as( ID devkey, CONST char *fimpnm, UINT flags, ID owner );

/*
 * What is on a disk or a partition (design 7.6), as its first blocks
 * say. fs_attach_dev with no fimp (NULL) mounts with the fimp of the
 * kind found, and answers EX_NOTSUP for a kind no fimp mounts: TSFS,
 * which is attached as a volume of objects instead (ob_att_vol), exFAT,
 * NTFS, ext2/3/4, ISO 9660 and the BTRON file system, which file
 * conversion reads without mounting. The device object of a disk tells
 * the kind among its attributes (tessronos.format).
 */
#define FS_KIND_NONE		0		/* not looked at */
#define FS_KIND_FAT12		1
#define FS_KIND_FAT16		2
#define FS_KIND_FAT32		3
#define FS_KIND_EXFAT		4
#define FS_KIND_NTFS		5
#define FS_KIND_EXT		6		/* ext2, ext3, ext4 */
#define FS_KIND_TSFS		7		/* the native TSFS */
#define FS_KIND_BTRON		8		/* the BTRON file system, or a CD-ROM carrying one */
#define FS_KIND_ISO9660		9
#define FS_KIND_PARTED		10		/* a whole disk: the volumes are in its partitions */
#define FS_KIND_BLANK		11		/* nothing written at its start */
#define FS_KIND_UNKNOWN		12

typedef struct {
	UINT	kind;			/* FS_KIND_* */
	UB	name[16];		/* "FAT32", "TSFS", ... */
	UB	label[64];		/* the volume's own name, UTF-8; empty for none */
	UB	fimp[FS_FIMPNM_MAX];	/* the fimp that mounts it, empty for none */
	UD	bytes;			/* how large the disk or partition is */
	BOOL	rdonly;			/* the medium is protected from writing */
	TS_UUID	vuuid;			/* TSFS: the volume's own UUID */
	TS_UUID	top;			/* TSFS: its top object, zero for none */
} T_FSPROBE;

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

/* Entries of the mount table made so far, and (p_used) how many are mounts now */
IMPORT INT  knl_fs_slots( INT *p_used );

/* What is on a device, by its name (kernel/fs/fsprobe.c); the fimp of a kind, "" for none */
IMPORT ER   knl_fs_probe( CONST char *devnm, T_FSPROBE *p );
IMPORT CONST char *knl_fs_kind_fimp( UINT kind );

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
