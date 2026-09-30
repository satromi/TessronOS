/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_fat.c
 *	The FAT fimp on its three layouts: FAT12 (partition 5 of the test
 *	disk, vblka4, mounted on /fat12), FAT16 (partition 4, vblka3, on
 *	/fat16) and FAT32 (/boot, where the tests work in /boot/FATTEST),
 *	and on two USB disks without a partition table, mounted whole: a
 *	FAT32 volume (/usbsf, the tests work in /usbsf/FATTEST) and a
 *	1.44MB floppy image (/usbfd). tools/mkfat.py made all but /boot.
 *
 *	Every test runs on each volume in turn: the files the image came
 *	with, writing and reading back, truncating, directories, long and
 *	Japanese names, listing, a file over many clusters (on FAT12 across
 *	the entries of the table that straddle two sectors), the fixed root
 *	of FAT12 and FAT16 filled up, a full volume, renaming files and
 *	directories, and the free space before and after. On the volumes
 *	that can be unmounted: the mark of being in use, FSInfo, the look
 *	over at mount of a volume left marked, and renames cut off after
 *	each sector they write.
 *
 *	What the last test leaves on the volumes is for a check of the disk
 *	image from the host (tools/mkfat.py check) after the run. A run
 *	starts by taking away what an earlier one left.
 *
 *	On the Raspberry Pi 5 only /boot is there: the card has no FAT12 or
 *	FAT16 partition, and a USB disk of the sizes looked for would be one
 *	of the user's, which the tests do not write on.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/fs.h>
#include <ts/blk.h>

typedef struct {
	CONST char	*label;
	CONST char	*dev;		/* partition to mount; NULL: mounted at start-up */
	UD		usb_blocks;	/* a whole USB disk of this many blocks; 0: none */
	CONST char	*mnt;		/* mount point */
	CONST char	*dir;		/* where the tests work */
	UINT		bits;
	INT		root_free;	/* free entries of the fixed root as made; 0: none */
	CONST char	*hello;		/* the text of HELLO.TXT */
	BOOL		premade_jp;	/* the image holds the file with a Japanese name */
	BOOL		up;
} VOL;

#define NVOL		5
#define SF32_BLOCKS	69632		/* tools/mkfat.py make 32, 34MB */
#define FD_BLOCKS	2880		/* tools/mkfat.py make floppy */

LOCAL VOL	vol[NVOL] = {
#ifdef RPI5
	{ "FAT12", "", 0, "/fat12", "/fat12", 12, 25, "TessronOS FAT12 test file.\n", TRUE, FALSE },
	{ "FAT16", "", 0, "/fat16", "/fat16", 16, 58, "TessronOS FAT16 test file.\n", TRUE, FALSE },
#else
	{ "FAT12", "vblka4", 0, "/fat12", "/fat12", 12, 25, "TessronOS FAT12 test file.\n", TRUE, FALSE },
	{ "FAT16", "vblka3", 0, "/fat16", "/fat16", 16, 58, "TessronOS FAT16 test file.\n", TRUE, FALSE },
#endif
	{ "FAT32", NULL, 0, "/boot", "/boot/FATTEST", 32, 0, "TessronOS FAT test file.\n", FALSE, FALSE },
	{ "FAT32 USB", NULL, SF32_BLOCKS, "/usbsf", "/usbsf/FATTEST", 32, 0, "TessronOS FAT test file.\n",
	  FALSE, FALSE },
	{ "FAT12 USB floppy", NULL, FD_BLOCKS, "/usbfd", "/usbfd", 12, 217, "TessronOS FAT12 test file.\n",
	  TRUE, FALSE },
};

/* the names of the USB disks found, "udc" and the like */
LOCAL char	usb_name[NVOL][8];

/* the names the images come with, kept when the root is cleared */
#define JP_PREMADE	"日本語のファイル.txt"
LOCAL CONST char	*premade[] = { "HELLO.TXT", "DATA.BIN", "SUB", "BIG.BIN", JP_PREMADE, NULL };

LOCAL INT	nup = 0;

/* ---------------------------------------------------------------- helpers */

LOCAL INT str_len( CONST char *s )
{
	INT	n = 0;

	while ( s[n] != '\0' ) n++;
	return n;
}

LOCAL BOOL same_bytes( CONST UB *a, CONST char *b, INT n )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return TRUE;
}

LOCAL BOOL str_eq( CONST char *a, CONST char *b )
{
	while ( *a != '\0' && *a == *b ) { a++; b++; }
	return ( *a == *b );
}

/* out = a "/" b */
LOCAL char *pj( char *out, CONST char *a, CONST char *b )
{
	INT	i = 0, j;

	for ( j = 0; a[j] != '\0' && i < FS_PATH_MAX - 2; j++ ) out[i++] = a[j];
	out[i++] = '/';
	for ( j = 0; b[j] != '\0' && i < FS_PATH_MAX - 1; j++ ) out[i++] = b[j];
	out[i] = '\0';

	return out;
}

/* a name with a number in it: "R" 7 ".TXT" -> "R007.TXT" */
LOCAL char *numname( char *out, CONST char *pre, INT n, CONST char *post )
{
	INT	i = 0, j;

	for ( j = 0; pre[j] != '\0'; j++ ) out[i++] = pre[j];
	out[i++] = (char)('0' + (n / 100) % 10);
	out[i++] = (char)('0' + (n / 10) % 10);
	out[i++] = (char)('0' + n % 10);
	for ( j = 0; post[j] != '\0'; j++ ) out[i++] = post[j];
	out[i] = '\0';

	return out;
}

LOCAL UB pat( UB seed, UD i )
{
	return (UB)(seed + i * 7 + (i >> 9));
}

LOCAL UD bfree( VOL *v )
{
	T_FSSTAT	st;

	if ( fs_statvfs(v->mnt, &st) < EX_OK ) return ~0ULL;
	return st.bfree;
}

LOCAL UD bsize( VOL *v )
{
	T_FSSTAT	st;

	if ( fs_statvfs(v->mnt, &st) < EX_OK ) return 1;
	return st.bsize;
}

/* clusters a file of 'len' bytes takes */
LOCAL UD nclus( VOL *v, UD len )
{
	UD	b = bsize(v);

	return ( len + b - 1 ) / b;
}

/* bytes [from, from + len) of the pattern, written in pieces of 'piece' */
LOCAL INT put_pat( INT fd, UB seed, UD from, UD len, UD piece )
{
	UB	*buf;
	UD	done = 0, i, n;
	INT	w = 0;

	buf = (UB *)Kmalloc((SZ)piece);
	if ( buf == NULL ) return EX_NOMEM;
	while ( done < len ) {
		n = ( len - done < piece ) ? len - done : piece;
		for ( i = 0; i < n; i++ ) buf[i] = pat(seed, from + done + i);
		w = fs_write(fd, buf, (SZ)n);
		if ( w <= 0 ) break;
		done += (UD)w;
		if ( (UD)w < n ) break;
	}
	Kfree(buf);

	return ( done == 0 && w < 0 ) ? w : (INT)done;
}

LOCAL INT write_pat( CONST char *path, UB seed, UD len, UD piece )
{
	INT	fd, n;

	fd = fs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) return fd;
	n = put_pat(fd, seed, 0, len, piece);
	fs_close(fd);

	return n;
}

/* the file holds the pattern from 0 to len and ends there */
LOCAL BOOL check_pat( CONST char *path, UB seed, UD len )
{
	UB	*buf;
	UD	done = 0, i;
	INT	fd, n;
	BOOL	ok = TRUE;

	fd = fs_open(path, O_RDONLY);
	if ( fd < 0 ) return FALSE;
	buf = (UB *)Kmalloc(4096);
	if ( buf == NULL ) { fs_close(fd); return FALSE; }
	while ( ok ) {
		n = fs_read(fd, buf, 4096);
		if ( n <= 0 ) break;
		for ( i = 0; i < (UD)n && ok; i++ ) {
			if ( buf[i] != pat(seed, done + i) ) {
				tm_printf((UB *)"  %s: byte %d is %d\n", path, (INT)(done + i), buf[i]);
				ok = FALSE;
			}
		}
		done += (UD)n;
	}
	if ( done != len ) {
		tm_printf((UB *)"  %s: %d bytes, not %d\n", path, (INT)done, (INT)len);
		ok = FALSE;
	}
	Kfree(buf);
	fs_close(fd);

	return ok;
}

LOCAL UD size_of( CONST char *path )
{
	T_FSTAT	st;

	if ( fs_stat(path, &st) < EX_OK ) return ~0ULL;
	return st.size;
}

LOCAL BOOL exists( CONST char *path )
{
	T_FSTAT	st;

	return ( fs_stat(path, &st) >= EX_OK );
}

LOCAL BOOL dot( CONST UB *n )
{
	return ( n[0] == '.' && ( n[1] == 0 || ( n[1] == '.' && n[2] == 0 ) ) );
}

LOCAL BOOL is_premade( CONST UB *n )
{
	INT	i;

	for ( i = 0; premade[i] != NULL; i++ ) {
		if ( str_eq((CONST char *)n, premade[i]) ) return TRUE;
	}
	return FALSE;
}

/* The first name in a directory other than ".", ".." and, if asked, the premade ones */
LOCAL BOOL first_name( CONST char *dir, BOOL keep_premade, UB *name )
{
	T_DIRENT	de[4];
	INT		fd, n, i;
	BOOL		found = FALSE;

	fd = fs_open(dir, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) return FALSE;
	while ( !found && ( n = fs_getdents(fd, de, 4) ) > 0 ) {
		for ( i = 0; i < n && !found; i++ ) {
			if ( dot(de[i].name) ) continue;
			if ( keep_premade && is_premade(de[i].name) ) continue;
			knl_memcpy(name, de[i].name, FS_NAME_MAX);
			found = TRUE;
		}
	}
	fs_close(fd);

	return found;
}

/* A file or a directory with all it holds taken away */
LOCAL void rm_tree( CONST char *path, BOOL keep_premade, BOOL top )
{
	T_FSTAT	st;
	UB	name[FS_NAME_MAX];
	char	*sub;
	INT	k;

	if ( fs_stat(path, &st) < EX_OK ) return;
	if ( (st.mode & FS_IFMT) != FS_IFDIR ) {
		fs_unlink(path);
		return;
	}
	sub = (char *)Kmalloc(FS_PATH_MAX);
	if ( sub == NULL ) return;
	for ( k = 0; k < 2000 && first_name(path, keep_premade, name); k++ ) {
		pj(sub, path, (CONST char *)name);
		rm_tree(sub, FALSE, FALSE);
		if ( exists(sub) ) break;
	}
	Kfree(sub);
	if ( !top ) fs_rmdir(path);
}

#define EACH_VOL(v)	for ( v = &vol[0]; v < &vol[NVOL]; v++ ) if ( v->up && \
			( tm_printf((UB *)"  %s\n", v->label), TRUE ) )

/* ---------------------------------------------------------------- mount */

/*
 * The USB disk of 'blocks' blocks, by the name of the whole unit. The
 * disks may still be coming up when this suite runs on its own.
 */
LOCAL CONST char *find_usb( UD blocks, char *name )
{
	DiskInfo	di;
	SZ		asz;
	ID		dd;
	INT		t, u;

	for ( t = 0; t < 20; t++ ) {
		for ( u = 0; u < 8; u++ ) {
			name[0] = 'u'; name[1] = 'd'; name[2] = (char)('a' + u); name[3] = '\0';
			dd = tk_opn_dev((UB *)name, TD_READ);
			if ( dd <= 0 ) continue;
			asz = 0;
			if ( tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz) >= E_OK
			  && (UD)di.blockcont == blocks ) {
				tk_cls_dev(dd, 0);
				return name;
			}
			tk_cls_dev(dd, 0);
		}
		tk_dly_tsk(500);
	}
	return NULL;
}

/* The FAT12 and FAT16 partitions mounted; what an earlier run left, gone */
LOCAL void test_mount( void )
{
	T_FSSTAT	st;
	T_FSTAT		fst;
	char		p[FS_PATH_MAX];
	VOL		*v;
	INT		i, fd;

	for ( i = 0; i < NVOL; i++ ) {
		v = &vol[i];
#ifdef RPI5
		if ( v->usb_blocks != 0 || ( v->dev != NULL && v->dev[0] == '\0' ) ) {
			tm_printf((UB *)"  %s: not on the Raspberry Pi 5\n", v->label);
			continue;
		}
#endif
		if ( v->usb_blocks != 0 ) {
			v->dev = find_usb(v->usb_blocks, usb_name[i]);
			if ( v->dev == NULL ) {
				tm_printf((UB *)"  %s: no USB disk of %d blocks\n", v->label, (INT)v->usb_blocks);
				continue;
			}
			tm_printf((UB *)"  %s is %s, mounted whole\n", v->label, v->dev);
		}
		if ( v->dev != NULL ) {
			fs_detach(v->mnt);			/* from a run that stopped */
			if ( fs_attach("fatfs", v->dev, v->mnt, 0) < EX_OK ) {
				tm_printf((UB *)"  %s: no %s\n", v->label, v->dev);
				continue;
			}
		}
		if ( fs_statvfs(v->mnt, &st) < EX_OK ) continue;
		v->up = TRUE;
		nup++;
		tm_printf((UB *)"  %s on %s: %d clusters of %d bytes, %d free\n", v->label, v->mnt,
			  (INT)st.blocks, (INT)st.bsize, (INT)st.bfree);

		/* the type follows from the number of clusters */
		switch ( v->bits ) {
		  case 12: KT_ASSERT(st.blocks < 4085); break;
		  case 16: KT_ASSERT(st.blocks >= 4085 && st.blocks < 65525); break;
		  default: KT_ASSERT(st.blocks >= 65525); break;
		}
		KT_ASSERT(st.bfree > 0 && st.bfree < st.blocks);
		KT_ASSERT_ER(fs_stat(v->mnt, &fst), EX_OK);
		KT_ASSERT_EQ(fst.mode & FS_IFMT, FS_IFDIR);

		if ( v->bits == 32 ) {
			char	g[FS_PATH_MAX];

			rm_tree(v->dir, FALSE, FALSE);
			rm_tree(pj(p, v->mnt, "FATTOPMV"), FALSE, FALSE);
			KT_ASSERT_ER(fs_mkdir(v->dir), EX_OK);
			/*
			 * Room made in the directory beforehand, so that it
			 * does not grow a cluster in the middle of a count
			 * of the free space
			 */
			pj(g, v->dir, "G");
			for ( fd = 0; fd < 100; fd++ ) {
				INT f = fs_open(numname(p, g, fd, ".TMP"), O_WRONLY | O_CREAT);
				if ( f >= 0 ) fs_close(f);
			}
			for ( fd = 0; fd < 100; fd++ ) {
				fs_unlink(numname(p, g, fd, ".TMP"));
			}
		} else {
			rm_tree(v->dir, TRUE, TRUE);
		}
	}
	if ( nup == 0 ) KT_SKIP("no FAT volume");

	/* the whole disk begins with a partition table, not a boot sector */
	KT_ASSERT_ER(fs_attach("fatfs", KT_DISK, "/fatwhole", 0), EX_INVAL);
}

/* ---------------------------------------------------------------- the files the image came with */

LOCAL void test_premade( void )
{
	char	p[FS_PATH_MAX];
	UB	buf[512];
	VOL	*v;
	INT	fd, n, s;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		fd = fs_open(pj(p, v->mnt, "HELLO.TXT"), O_RDONLY);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			n = fs_read(fd, buf, sizeof(buf));
			KT_ASSERT_EQ(n, str_len(v->hello));
			KT_ASSERT(same_bytes(buf, v->hello, n));
			fs_close(fd);
		}

		/* nine sectors over several clusters, each stamped with its number */
		fd = fs_open(pj(p, v->mnt, "DATA.BIN"), O_RDONLY);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			for ( s = 0; s < 9; s++ ) {
				n = fs_read(fd, buf, 512);
				KT_ASSERT_EQ(n, 512);
				KT_ASSERT(buf[0] == 'S' && buf[7] == '0' + s / 1000 % 10
					  && buf[10] == '0' + s % 10);
			}
			KT_ASSERT_EQ(fs_read(fd, buf, 512), 0);
			fs_close(fd);
		}

		fd = fs_open(pj(p, v->mnt, "SUB/INNER.TXT"), O_RDONLY);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			n = fs_read(fd, buf, sizeof(buf));
			KT_ASSERT_EQ(n, 11);
			KT_ASSERT(buf[0] == 'i' && buf[10] == '\n');
			fs_close(fd);
		}

		if ( v->premade_jp ) {
			fd = fs_open(pj(p, v->mnt, JP_PREMADE), O_RDONLY);
			KT_ASSERT(fd >= 0);
			if ( fd >= 0 ) {
				n = fs_read(fd, buf, sizeof(buf));
				KT_ASSERT_EQ(n, 10);
				KT_ASSERT(same_bytes(buf, "日本語\n", 10));
				fs_close(fd);
			}
			/* the short alias finds it too */
			KT_ASSERT_EQ(size_of(pj(p, v->mnt, "______~1.TXT")), 10);
		}

		if ( v->bits == 12 ) {
			/*
			 * 700 clusters taken every other one: following the
			 * chain crosses the entries whose two bytes lie in two
			 * sectors of the table
			 */
			fd = fs_open(pj(p, v->mnt, "BIG.BIN"), O_RDONLY);
			KT_ASSERT(fd >= 0);
			if ( fd >= 0 ) {
				BOOL ok = TRUE;

				for ( s = 0; s < 700 && ok; s++ ) {
					n = fs_read(fd, buf, 512);
					ok = ( n == 512 && buf[0] == 'B' && buf[4] == '0' + s / 10000 % 10
					    && buf[5] == '0' + s / 1000 % 10 && buf[6] == '0' + s / 100 % 10
					    && buf[7] == '0' + s / 10 % 10 && buf[8] == '0' + s % 10 );
					if ( !ok ) tm_printf((UB *)"  BIG.BIN sector %d\n", s);
				}
				KT_ASSERT(ok);
				KT_ASSERT_EQ(fs_read(fd, buf, 512), 0);
				/* backwards: from the start of the chain again */
				KT_ASSERT_EQ(fs_lseek(fd, 341 * 512, SEEK_SET_), 341 * 512);
				KT_ASSERT_EQ(fs_read(fd, buf, 512), 512);
				KT_ASSERT(buf[6] == '3' && buf[7] == '4' && buf[8] == '1');
				fs_close(fd);
			}
		}
	}
}

/* ---------------------------------------------------------------- files */

LOCAL void test_files( void )
{
	char	p[FS_PATH_MAX];
	UB	buf[16];
	VOL	*v;
	UD	f0, f1;
	INT	fd, i;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		pj(p, v->dir, "T1.TXT");
		fd = fs_open(p, O_RDWR | O_CREAT | O_EXCL);
		KT_ASSERT(fd >= 0);
		if ( fd < 0 ) continue;
		f0 = bfree(v);
		KT_ASSERT_EQ(put_pat(fd, 0x21, 0, 3000, 3000), 3000);
		{
			T_FSTAT st;
			KT_ASSERT_ER(fs_fstat(fd, &st), EX_OK);
			KT_ASSERT_EQ(st.size, 3000);
		}
		KT_ASSERT_ER(fs_close(fd), EX_OK);
		KT_ASSERT_EQ(size_of(p), 3000);
		f1 = bfree(v);
		KT_ASSERT_EQ(f0 - f1, nclus(v, 3000));
		KT_ASSERT(check_pat(p, 0x21, 3000));
		KT_ASSERT_ER(fs_open(p, O_RDWR | O_CREAT | O_EXCL), EX_EXIST);

		/* shorter: the clusters past the end go back */
		KT_ASSERT_ER(fs_truncate(p, 100), EX_OK);
		KT_ASSERT_EQ(size_of(p), 100);
		KT_ASSERT_EQ(f0 - bfree(v), nclus(v, 100));
		KT_ASSERT(check_pat(p, 0x21, 100));

		/* emptied on opening */
		fd = fs_open(p, O_WRONLY | O_TRUNC);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) fs_close(fd);
		KT_ASSERT_EQ(size_of(p), 0);
		KT_ASSERT_EQ(bfree(v), f0);

		/* added to the end, a piece at a time */
		fd = fs_open(p, O_WRONLY | O_APPEND);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			KT_ASSERT_EQ(put_pat(fd, 0x44, 0, 700, 700), 700);
			KT_ASSERT_EQ(put_pat(fd, 0x44, 700, 700, 333), 700);
			fs_close(fd);
		}
		KT_ASSERT(check_pat(p, 0x44, 1400));

		/* written past the end: the gap reads as zeros */
		fd = fs_open(p, O_RDWR);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			KT_ASSERT_EQ(fs_lseek(fd, 5000, SEEK_SET_), 5000);
			KT_ASSERT_EQ(fs_write(fd, "0123456789", 10), 10);
			KT_ASSERT_EQ(fs_lseek(fd, 1400, SEEK_SET_), 1400);
			KT_ASSERT_EQ(fs_read(fd, buf, 16), 16);
			for ( i = 0; i < 16; i++ ) KT_ASSERT_EQ(buf[i], 0);
			KT_ASSERT_EQ(fs_lseek(fd, 4998, SEEK_SET_), 4998);
			KT_ASSERT_EQ(fs_read(fd, buf, 16), 12);
			KT_ASSERT(buf[0] == 0 && buf[2] == '0' && buf[11] == '9');
			fs_close(fd);
		}
		KT_ASSERT_EQ(size_of(p), 5010);
		KT_ASSERT_EQ(f0 - bfree(v), nclus(v, 5010));

		KT_ASSERT_ER(fs_unlink(p), EX_OK);
		KT_ASSERT(!exists(p));
		KT_ASSERT_EQ(bfree(v), f0);
		KT_ASSERT_ER(fs_unlink(p), EX_NOENT);
	}
}

/* ---------------------------------------------------------------- directories */

LOCAL void test_dirs( void )
{
	char	p[FS_PATH_MAX], q[FS_PATH_MAX];
	T_FSTAT	st, st2;
	VOL	*v;
	UD	f0;
	INT	fd;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		f0 = bfree(v);
		KT_ASSERT_ER(fs_mkdir(pj(p, v->dir, "D1")), EX_OK);
		KT_ASSERT_ER(fs_mkdir(p), EX_EXIST);
		KT_ASSERT_ER(fs_mkdir(pj(p, v->dir, "D1/D2")), EX_OK);
		KT_ASSERT_EQ(f0 - bfree(v), 2);
		fd = fs_open(pj(p, v->dir, "D1/D2/F.TXT"), O_WRONLY | O_CREAT);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			KT_ASSERT_EQ(fs_write(fd, "0123456789", 10), 10);
			fs_close(fd);
		}
		KT_ASSERT_ER(fs_rmdir(pj(p, v->dir, "D1")), EX_NOTEMPTY);
		KT_ASSERT_ER(fs_unlink(p), EX_ISDIR);
		KT_ASSERT_ER(fs_rmdir(pj(p, v->dir, "D1/D2/F.TXT")), EX_NOTDIR);
		KT_ASSERT_ER(fs_open(pj(p, v->dir, "D1/D2"), O_WRONLY), EX_ISDIR);
		KT_ASSERT_ER(fs_open(pj(p, v->dir, "D1/D2/F.TXT/X"), O_RDONLY), EX_NOTDIR);

		/* ".." leads back up */
		KT_ASSERT_ER(fs_stat(pj(p, v->dir, "D1/D2/.."), &st), EX_OK);
		KT_ASSERT_ER(fs_stat(pj(q, v->dir, "D1"), &st2), EX_OK);
		KT_ASSERT_EQ(st.mode & FS_IFMT, FS_IFDIR);
		KT_ASSERT_EQ(st.ino, st2.ino);
		KT_ASSERT_EQ(size_of(pj(p, v->dir, "D1/D2/../D2/./F.TXT")), 10);
		KT_ASSERT_ER(fs_rmdir(pj(p, v->dir, "D1/D2/.")), EX_INVAL);
		KT_ASSERT_ER(fs_rmdir(pj(p, v->dir, "D1/D2/..")), EX_INVAL);

		KT_ASSERT_ER(fs_unlink(pj(p, v->dir, "D1/D2/F.TXT")), EX_OK);
		KT_ASSERT_ER(fs_rmdir(pj(p, v->dir, "D1/D2")), EX_OK);
		KT_ASSERT_ER(fs_rmdir(pj(p, v->dir, "D1")), EX_OK);
		KT_ASSERT(!exists(p));
		KT_ASSERT_EQ(bfree(v), f0);
	}
}

/* ---------------------------------------------------------------- names */

LOCAL CONST char	*long_names[] = {
	"日本語の長い名前のファイル.txt",
	"漢字",
	"MixedCase.Txt",
	"lower.txt",
	"a long name with spaces.text",
	"\xF0\x9F\x98\x80" "face.txt",		/* outside the basic plane: a surrogate pair */
	NULL
};

/* Whether a directory lists 'name' exactly once, with 'size' bytes */
LOCAL INT listed( CONST char *dir, CONST char *name, UD size )
{
	T_DIRENT	de[5];
	INT		fd, n, i, cnt = 0;

	fd = fs_open(dir, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) return -1;
	while ( ( n = fs_getdents(fd, de, 5) ) > 0 ) {
		for ( i = 0; i < n; i++ ) {
			if ( str_eq((CONST char *)de[i].name, name) ) {
				cnt++;
				if ( de[i].size != size ) cnt += 100;
			}
		}
	}
	fs_close(fd);

	return cnt;
}

LOCAL void test_names( void )
{
	char	p[FS_PATH_MAX];
	VOL	*v;
	UD	f0;
	INT	i, len;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		f0 = bfree(v);
		for ( i = 0; long_names[i] != NULL; i++ ) {
			len = str_len(long_names[i]);
			KT_ASSERT_EQ(write_pat(pj(p, v->dir, long_names[i]), (UB)i, (UD)len, 64), len);
		}
		for ( i = 0; long_names[i] != NULL; i++ ) {
			len = str_len(long_names[i]);
			KT_ASSERT_EQ(listed(v->dir, long_names[i], (UD)len), 1);
			KT_ASSERT(check_pat(pj(p, v->dir, long_names[i]), (UB)i, (UD)len));
		}
		/* names are found whatever their case, and keep the case they were given */
		KT_ASSERT_EQ(size_of(pj(p, v->dir, "MIXEDCASE.TXT")), 13);
		KT_ASSERT_EQ(size_of(pj(p, v->dir, "LOWER.TXT")), 9);
		KT_ASSERT_EQ(size_of(pj(p, v->dir, "A LONG NAME WITH SPACES.TEXT")), 28);
		KT_ASSERT_EQ(listed(v->dir, "LOWER.TXT", 9), 0);
		KT_ASSERT_ER(fs_open(pj(p, v->dir, "MIXEDCASE.TXT"), O_WRONLY | O_CREAT | O_EXCL), EX_EXIST);

		for ( i = 0; long_names[i] != NULL; i++ ) {
			KT_ASSERT_ER(fs_unlink(pj(p, v->dir, long_names[i])), EX_OK);
			KT_ASSERT_EQ(listed(v->dir, long_names[i], 0), 0);
		}
		KT_ASSERT_EQ(bfree(v), f0);
	}
}

/* ---------------------------------------------------------------- listing */

#define NLIST		30

LOCAL void test_listing( void )
{
	char		p[FS_PATH_MAX], d[FS_PATH_MAX], nm[16];
	T_DIRENT	de[4];
	UB		seen[NLIST];
	VOL		*v;
	UD		f0;
	INT		fd, n, i, k, dots, other;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		f0 = bfree(v);
		pj(d, v->dir, "LS");
		KT_ASSERT_ER(fs_mkdir(d), EX_OK);
		for ( i = 0; i < NLIST; i++ ) {
			KT_ASSERT_EQ(write_pat(pj(p, d, numname(nm, "L", i, ".DAT")), 0, (UD)i * 10, 512),
				     i * 10);
			seen[i] = 0;
		}

		/* four at a time: the place in the directory is kept between calls */
		dots = other = 0;
		fd = fs_open(d, O_RDONLY | O_DIRECTORY);
		KT_ASSERT(fd >= 0);
		while ( fd >= 0 && ( n = fs_getdents(fd, de, 4) ) > 0 ) {
			for ( k = 0; k < n; k++ ) {
				if ( dot(de[k].name) ) {
					dots++;
					KT_ASSERT_EQ(de[k].mode, FS_IFDIR);
					continue;
				}
				i = (de[k].name[1] - '0') * 100 + (de[k].name[2] - '0') * 10 + (de[k].name[3] - '0');
				if ( de[k].name[0] != 'L' || i < 0 || i >= NLIST ) { other++; continue; }
				seen[i]++;
				KT_ASSERT_EQ(de[k].size, i * 10);
				KT_ASSERT_EQ(de[k].mode, FS_IFREG);
			}
		}
		if ( fd >= 0 ) {
			KT_ASSERT_EQ(fs_getdents(fd, de, 4), 0);	/* stays at the end */
			fs_close(fd);
		}
		KT_ASSERT_EQ(dots, 2);
		KT_ASSERT_EQ(other, 0);
		for ( i = 0; i < NLIST; i++ ) KT_ASSERT_EQ(seen[i], 1);

		fd = fs_open(pj(p, d, "L001.DAT"), O_RDONLY);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			KT_ASSERT_ER(fs_getdents(fd, de, 4), EX_NOTDIR);
			fs_close(fd);
		}
		fs_sync();
		rm_tree(d, FALSE, FALSE);
		KT_ASSERT(!exists(d));
		KT_ASSERT_EQ(bfree(v), f0);
	}
}

/* ---------------------------------------------------------------- many names */

#define NMANY		120

/* A name like the ones an object store gives its files: a UUID, then an ending */
LOCAL char *uuid_name( char *out, INT n, CONST char *end )
{
	CONST char	*pre = "019a3b2c-7d4e-7f00-8a1b-000000000";
	INT		i = 0, j;

	for ( j = 0; pre[j] != '\0'; j++ ) out[i++] = pre[j];
	out[i++] = (char)( '0' + ( n / 100 ) % 10 );
	out[i++] = (char)( '0' + ( n / 10 ) % 10 );
	out[i++] = (char)( '0' + n % 10 );
	for ( j = 0; end[j] != '\0'; j++ ) out[i++] = end[j];
	out[i] = '\0';
	return out;
}

/*
 * Many long names that begin alike, in a directory of many clusters:
 * each has a short alias of its own, each is found and a name not there
 * is not, and they stay so as names go, come back into the holes left,
 * and are renamed; a directory made again where one was removed holds
 * none of the old names.
 */
LOCAL void test_many_names( void )
{
	char		p[FS_PATH_MAX], q[FS_PATH_MAX], d[FS_PATH_MAX], nm[64];
	T_DIRENT	de[5];
	VOL		*v;
	UD		f0;
	INT		fd, n, i, k, cnt;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		f0 = bfree(v);
		pj(d, v->dir, "MANY");
		KT_ASSERT_ER(fs_mkdir(d), EX_OK);
		for ( i = 0; i < NMANY; i++ ) {
			KT_ASSERT_EQ(write_pat(pj(p, d, uuid_name(nm, i, ".json")), (UB)i, (UD)( i % 7 ), 512),
				     i % 7);
		}
		for ( i = 0; i < NMANY; i++ ) {
			KT_ASSERT_EQ(size_of(pj(p, d, uuid_name(nm, i, ".json"))), i % 7);
		}
		KT_ASSERT(!exists(pj(p, d, uuid_name(nm, NMANY, ".json"))));
		KT_ASSERT(!exists(pj(p, d, uuid_name(nm, 1, ".jso"))));

		/* every other one goes; the rest stay found */
		for ( i = 0; i < NMANY; i += 2 ) {
			KT_ASSERT_ER(fs_unlink(pj(p, d, uuid_name(nm, i, ".json"))), EX_OK);
		}
		for ( i = 0; i < NMANY; i++ ) {
			KT_ASSERT_EQ(exists(pj(p, d, uuid_name(nm, i, ".json"))), ( i % 2 ) != 0);
		}

		/* back in, under another ending; some renamed */
		for ( i = 0; i < NMANY; i += 2 ) {
			KT_ASSERT_EQ(write_pat(pj(p, d, uuid_name(nm, i, "_0.xtad")), (UB)i, 3, 512), 3);
		}
		for ( i = 1; i < NMANY; i += 4 ) {
			KT_ASSERT_ER(fs_rename(pj(p, d, uuid_name(nm, i, ".json")),
					       pj(q, d, uuid_name(nm, i, ".ico"))), EX_OK);
		}
		cnt = 0;
		fd = fs_open(d, O_RDONLY | O_DIRECTORY);
		KT_ASSERT(fd >= 0);
		while ( fd >= 0 && ( n = fs_getdents(fd, de, 5) ) > 0 ) {
			for ( k = 0; k < n; k++ ) {
				if ( !dot(de[k].name) ) cnt++;
			}
		}
		if ( fd >= 0 ) fs_close(fd);
		KT_ASSERT_EQ(cnt, NMANY);
		for ( i = 0; i < NMANY; i++ ) {
			BOOL	ico = ( i % 4 ) == 1;

			KT_ASSERT_EQ(exists(pj(p, d, uuid_name(nm, i, ".json"))), ( i % 2 ) != 0 && !ico);
			KT_ASSERT_EQ(exists(pj(p, d, uuid_name(nm, i, ".ico"))), ico);
			KT_ASSERT_EQ(exists(pj(p, d, uuid_name(nm, i, "_0.xtad"))), ( i % 2 ) == 0);
		}

		/* gone and made again: nothing of the old names in it */
		fs_sync();
		rm_tree(d, FALSE, FALSE);
		KT_ASSERT(!exists(d));
		KT_ASSERT_ER(fs_mkdir(d), EX_OK);
		KT_ASSERT(!exists(pj(p, d, uuid_name(nm, 1, ".json"))));
		KT_ASSERT_EQ(write_pat(pj(p, d, uuid_name(nm, 1, ".json")), 1, 5, 512), 5);
		KT_ASSERT_EQ(size_of(pj(p, d, uuid_name(nm, 1, ".json"))), 5);
		rm_tree(d, FALSE, FALSE);
		KT_ASSERT(!exists(d));
		KT_ASSERT_EQ(bfree(v), f0);
	}
}

/* ---------------------------------------------------------------- many clusters */

LOCAL void test_big( void )
{
	char	p[FS_PATH_MAX];
	VOL	*v;
	UD	f0, len;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		len = ( v->bits == 16 ) ? 2000000 : 1000000;
		pj(p, v->dir, "BIGW.BIN");
		f0 = bfree(v);
		/* odd pieces: sectors are written in part as well as whole */
		KT_ASSERT_EQ(write_pat(p, 0x5A, len, 3001), (INT)len);
		KT_ASSERT_EQ(size_of(p), len);
		KT_ASSERT_EQ(f0 - bfree(v), nclus(v, len));
		KT_ASSERT(check_pat(p, 0x5A, len));

		KT_ASSERT_ER(fs_truncate(p, 123456), EX_OK);
		KT_ASSERT_EQ(f0 - bfree(v), nclus(v, 123456));
		KT_ASSERT(check_pat(p, 0x5A, 123456));

		KT_ASSERT_ER(fs_unlink(p), EX_OK);
		KT_ASSERT_EQ(bfree(v), f0);
	}
}

/* A volume with no cluster left: writing stops there, and all comes back */
LOCAL void test_disk_full( void )
{
	char	p[FS_PATH_MAX], q[FS_PATH_MAX];
	VOL	*v = &vol[0];
	UD	f0;
	INT	fd, n, total = 0;

	if ( !v->up ) KT_SKIP("no FAT12 volume");
	f0 = bfree(v);
	pj(p, v->dir, "FULL.BIN");
	fd = fs_open(p, O_WRONLY | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;
	for (;;) {
		n = put_pat(fd, 0x33, (UD)total, 8192, 8192);
		if ( n <= 0 ) break;
		total += n;
		if ( n < 8192 ) break;
	}
	KT_ASSERT_EQ(fs_write(fd, "x", 1), EX_NOSPC);
	fs_close(fd);
	tm_printf((UB *)"  %d bytes fill the FAT12 volume\n", total);
	KT_ASSERT_EQ(bfree(v), 0);
	KT_ASSERT_EQ((UD)total, f0 * bsize(v));
	KT_ASSERT_EQ(size_of(p), total);

	/* an entry can still be made, but nothing that needs a cluster */
	fd = fs_open(pj(q, v->dir, "EMPTY.TXT"), O_WRONLY | O_CREAT);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		KT_ASSERT_EQ(fs_write(fd, "x", 1), EX_NOSPC);
		fs_close(fd);
	}
	KT_ASSERT_EQ(size_of(q), 0);
	KT_ASSERT_ER(fs_mkdir(pj(q, v->dir, "NODIR")), EX_NOSPC);
	KT_ASSERT(!exists(q));

	KT_ASSERT(check_pat(p, 0x33, (UD)total));
	KT_ASSERT_ER(fs_unlink(p), EX_OK);
	KT_ASSERT_ER(fs_unlink(pj(q, v->dir, "EMPTY.TXT")), EX_OK);
	KT_ASSERT_EQ(bfree(v), f0);
}

/* ---------------------------------------------------------------- the fixed root */

LOCAL void test_root_full( void )
{
	char	p[FS_PATH_MAX], q[FS_PATH_MAX], nm[16];
	VOL	*v;
	UD	f0;
	INT	n, fd = 0, i;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		if ( v->root_free == 0 ) continue;	/* a FAT32 root grows */
		f0 = bfree(v);
		for ( n = 0; n < 600; n++ ) {
			fd = fs_open(pj(p, v->mnt, numname(nm, "R", n, ".TXT")), O_WRONLY | O_CREAT | O_EXCL);
			if ( fd < 0 ) break;
			fs_close(fd);
		}
		KT_ASSERT_ER(fd, EX_NOSPC);
		KT_ASSERT_EQ(n, v->root_free);
		tm_printf((UB *)"  the root is full after %d files\n", n);

		/* nothing more in the root, and no cluster lost trying */
		KT_ASSERT_ER(fs_mkdir(pj(p, v->mnt, "RX")), EX_NOSPC);
		KT_ASSERT_ER(fs_open(pj(p, v->mnt, "a long name.txt"), O_WRONLY | O_CREAT), EX_NOSPC);
		KT_ASSERT_EQ(bfree(v), f0);
		KT_ASSERT(!exists(pj(p, v->mnt, "RX")));

		/* a directory below still grows */
		fd = fs_open(pj(p, v->mnt, "SUB/ROOTFULL.TXT"), O_WRONLY | O_CREAT);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) fs_close(fd);
		/* and a file cannot be moved into the root */
		KT_ASSERT_ER(fs_rename(p, pj(q, v->mnt, "MOVED.TXT")), EX_NOSPC);
		KT_ASSERT(exists(p));
		KT_ASSERT(!exists(q));
		KT_ASSERT_ER(fs_unlink(p), EX_OK);

		/* one taken away makes room for one */
		KT_ASSERT_ER(fs_unlink(pj(p, v->mnt, numname(nm, "R", 0, ".TXT"))), EX_OK);
		fd = fs_open(pj(p, v->mnt, "ONEMORE.TXT"), O_WRONLY | O_CREAT);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) fs_close(fd);
		KT_ASSERT_ER(fs_open(pj(p, v->mnt, "TWOMORE.TXT"), O_WRONLY | O_CREAT), EX_NOSPC);
		KT_ASSERT_ER(fs_unlink(pj(p, v->mnt, "ONEMORE.TXT")), EX_OK);

		for ( i = 1; i < n; i++ ) {
			KT_ASSERT_ER(fs_unlink(pj(p, v->mnt, numname(nm, "R", i, ".TXT"))), EX_OK);
		}
		KT_ASSERT_EQ(bfree(v), f0);
	}
}

/* ---------------------------------------------------------------- rename */

LOCAL void test_rename_file( void )
{
	char	a[FS_PATH_MAX], b[FS_PATH_MAX];
	VOL	*v;
	UD	f0, f1;
	INT	fd;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		f0 = bfree(v);
		KT_ASSERT_EQ(write_pat(pj(a, v->dir, "A.TXT"), 0x61, 2000, 2000), 2000);

		/* in one directory */
		KT_ASSERT_ER(fs_rename(a, pj(b, v->dir, "B.TXT")), EX_OK);
		KT_ASSERT(!exists(a));
		KT_ASSERT(check_pat(b, 0x61, 2000));

		/* to a long Japanese name and back to a short one */
		KT_ASSERT_ER(fs_rename(b, pj(a, v->dir, "名前を変えたファイル.dat")), EX_OK);
		KT_ASSERT_EQ(listed(v->dir, "名前を変えたファイル.dat", 2000), 1);
		KT_ASSERT_EQ(listed(v->dir, "B.TXT", 2000), 0);
		KT_ASSERT_ER(fs_rename(a, pj(b, v->dir, "c.txt")), EX_OK);
		KT_ASSERT_EQ(listed(v->dir, "名前を変えたファイル.dat", 2000), 0);

		/* only the case changes; the same name is no change at all */
		KT_ASSERT_ER(fs_rename(b, pj(a, v->dir, "C.TXT")), EX_OK);
		KT_ASSERT_EQ(listed(v->dir, "C.TXT", 2000), 1);
		KT_ASSERT_EQ(listed(v->dir, "c.txt", 2000), 0);
		KT_ASSERT_ER(fs_rename(a, a), EX_OK);
		KT_ASSERT(check_pat(a, 0x61, 2000));

		/* into another directory */
		KT_ASSERT_ER(fs_mkdir(pj(b, v->dir, "RD1")), EX_OK);
		KT_ASSERT_ER(fs_rename(a, pj(b, v->dir, "RD1/C.TXT")), EX_OK);
		KT_ASSERT(!exists(a));
		KT_ASSERT(check_pat(b, 0x61, 2000));
		f1 = bfree(v);

		/* over a file that has the name: that file's clusters go back */
		KT_ASSERT_EQ(write_pat(pj(a, v->dir, "RD1/D.TXT"), 0x62, 5000, 5000), 5000);
		KT_ASSERT_EQ(f1 - bfree(v), nclus(v, 5000));
		KT_ASSERT_ER(fs_rename(b, a), EX_OK);
		KT_ASSERT(!exists(b));
		KT_ASSERT(check_pat(a, 0x61, 2000));
		KT_ASSERT_EQ(bfree(v), f1);

		/* not over a directory; not from what is not there */
		KT_ASSERT_ER(fs_mkdir(pj(b, v->dir, "RD1/SUBD")), EX_OK);
		KT_ASSERT_ER(fs_rename(a, b), EX_ISDIR);
		KT_ASSERT_ER(fs_rename(b, a), EX_NOTDIR);
		KT_ASSERT_ER(fs_rename(pj(b, v->dir, "NOSUCH.TXT"), pj(a, v->dir, "X.TXT")), EX_NOENT);
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD1/D.TXT"), pj(b, v->dir, "NODIR/D.TXT")), EX_NOENT);
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD1/D.TXT"), pj(b, v->dir, "RD1/D.TXT/E")), EX_NOTDIR);
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD1/."), pj(b, v->dir, "DOT")), EX_INVAL);
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD1/D.TXT"), pj(b, v->dir, "RD1/..")), EX_INVAL);
		KT_ASSERT(check_pat(pj(a, v->dir, "RD1/D.TXT"), 0x61, 2000));

		/* a file that is open follows its entry: what is written after lands in the new one */
		fd = fs_open(pj(a, v->dir, "RD1/D.TXT"), O_WRONLY | O_APPEND);
		KT_ASSERT(fd >= 0);
		if ( fd >= 0 ) {
			KT_ASSERT_ER(fs_rename(a, pj(b, v->dir, "RD1/SUBD/長い名前に移したファイル.txt")), EX_OK);
			KT_ASSERT_EQ(put_pat(fd, 0x61, 2000, 3000, 1000), 3000);
			/* an open file is not replaced */
			fs_close(fs_open(a, O_WRONLY | O_CREAT));
			KT_ASSERT_ER(fs_rename(a, b), EX_BUSY);
			KT_ASSERT_ER(fs_close(fd), EX_OK);
			KT_ASSERT_ER(fs_unlink(a), EX_OK);
		}
		KT_ASSERT_EQ(size_of(b), 5000);
		KT_ASSERT(check_pat(b, 0x61, 5000));
		KT_ASSERT(!exists(a));

		rm_tree(pj(a, v->dir, "RD1"), FALSE, FALSE);
		KT_ASSERT(!exists(a));
		KT_ASSERT_EQ(bfree(v), f0);
	}
}

LOCAL void test_rename_dir( void )
{
	char	a[FS_PATH_MAX], b[FS_PATH_MAX], top[FS_PATH_MAX];
	T_FSTAT	st, st2;
	VOL	*v;
	UD	f0;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		f0 = bfree(v);
		KT_ASSERT_ER(fs_mkdir(pj(a, v->dir, "RD2")), EX_OK);
		KT_ASSERT_ER(fs_mkdir(pj(a, v->dir, "RD3")), EX_OK);
		KT_ASSERT_ER(fs_mkdir(pj(a, v->dir, "RD2/SUBD")), EX_OK);
		KT_ASSERT_EQ(write_pat(pj(a, v->dir, "RD2/SUBD/F.TXT"), 0x70, 777, 777), 777);
		KT_ASSERT_ER(fs_mkdir(pj(a, v->dir, "RD2/SUBD/DEEP")), EX_OK);

		/* in one directory: ".." stays */
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD2/SUBD"), pj(b, v->dir, "RD2/サブ")), EX_OK);
		KT_ASSERT_EQ(size_of(pj(a, v->dir, "RD2/サブ/F.TXT")), 777);

		/* to another directory: ".." follows */
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD2/サブ"), pj(b, v->dir, "RD3/MOVED")), EX_OK);
		KT_ASSERT(!exists(a));
		KT_ASSERT(check_pat(pj(a, v->dir, "RD3/MOVED/F.TXT"), 0x70, 777));
		KT_ASSERT_ER(fs_stat(pj(a, v->dir, "RD3/MOVED/.."), &st), EX_OK);
		KT_ASSERT_ER(fs_stat(pj(b, v->dir, "RD3"), &st2), EX_OK);
		KT_ASSERT_EQ(st.ino, st2.ino);
		KT_ASSERT_EQ(size_of(pj(a, v->dir, "RD3/MOVED/DEEP/../../MOVED/F.TXT")), 777);

		/* not into itself, nor below itself */
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD3"), pj(b, v->dir, "RD3/MOVED/IN")), EX_INVAL);
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD3"), pj(b, v->dir, "RD3/IN")), EX_INVAL);
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD3/MOVED"), pj(b, v->dir, "RD3/MOVED/DEEP/IN")), EX_INVAL);
		KT_ASSERT(exists(pj(a, v->dir, "RD3/MOVED/DEEP")));

		/* over a directory: only an empty one */
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD2"), pj(b, v->dir, "RD3")), EX_NOTEMPTY);
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD3"), pj(b, v->dir, "RD2")), EX_OK);
		KT_ASSERT(!exists(a));
		KT_ASSERT_EQ(size_of(pj(a, v->dir, "RD2/MOVED/F.TXT")), 777);

		/* to the root of the volume: ".." names the root */
		pj(top, v->mnt, ( v->bits == 32 ) ? "FATTOPMV" : "TOPMV");
		KT_ASSERT_ER(fs_rename(pj(a, v->dir, "RD2/MOVED"), top), EX_OK);
		KT_ASSERT_ER(fs_stat(pj(a, top, ".."), &st), EX_OK);
		KT_ASSERT_ER(fs_stat(v->mnt, &st2), EX_OK);
		KT_ASSERT_EQ(st.ino, st2.ino);
		KT_ASSERT_EQ(size_of(pj(a, top, "DEEP/../F.TXT")), 777);
		/* and back below */
		KT_ASSERT_ER(fs_rename(top, pj(b, v->dir, "RD2/BACK")), EX_OK);
		KT_ASSERT_ER(fs_stat(pj(a, v->dir, "RD2/BACK/.."), &st), EX_OK);
		KT_ASSERT_ER(fs_stat(pj(a, v->dir, "RD2"), &st2), EX_OK);
		KT_ASSERT_EQ(st.ino, st2.ino);

		/* the root itself does not move */
		KT_ASSERT_ER(fs_rename(v->mnt, pj(a, v->dir, "ROOT")), EX_BUSY);

		rm_tree(pj(a, v->dir, "RD2"), FALSE, FALSE);
		KT_ASSERT(!exists(a));
		KT_ASSERT_EQ(bfree(v), f0);
	}
}

/* ---------------------------------------------------------------- media without a partition table */

LOCAL ER sf_read( void *exinf, UD start, UD nsect, void *buf )
{
	SZ	asz;

	return tk_srea_dev(*(ID *)exinf, (W)start, buf, (SZ)nsect * BLK_SECTOR_SIZE, &asz) < E_OK
		? E_IO : E_OK;
}

/*
 * A unit whose sector 0 is the boot sector of the volume, as a whole
 * medium formatted without partitions has it: the scan of partitions
 * finds none, and the volume mounts from the unit. The two USB disks
 * are such media, and the FAT partitions of the test disk look the
 * same from inside; the boot sector of a FAT12 volume has code that
 * looks like a partition entry but for its first byte.
 */
LOCAL void test_superfloppy( void )
{
	T_PARTTBL	tbl;
	DiskInfo	di;
	UB		sec[BLK_SECTOR_SIZE];
	SZ		asz;
	ID		dd;
	INT		i;

	for ( i = 0; i < NVOL; i++ ) {
		VOL *v = &vol[i];

		if ( !v->up || v->dev == NULL ) continue;
		dd = tk_opn_dev((UB *)v->dev, TD_READ);
		KT_ASSERT(dd > 0);
		if ( dd <= 0 ) continue;
		KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
		KT_ASSERT_ER(tk_srea_dev(dd, 0, sec, BLK_SECTOR_SIZE, &asz), E_OK);
		KT_ASSERT(sec[510] == 0x55 && sec[511] == 0xAA);
		if ( v->bits == 12 ) {
			KT_ASSERT(sec[446] == 0x54 && sec[446 + 4] == 0x06);
		}
		KT_ASSERT_EQ(knl_read_parttbl(sf_read, &dd, (UD)di.blockcont, &tbl), 0);
		KT_ASSERT_EQ(tbl.n, 0);
		tk_cls_dev(dd, 0);
	}

	/* a real table is still read */
	dd = tk_opn_dev((UB *)KT_DISK, TD_READ);
	KT_ASSERT(dd > 0);
	if ( dd > 0 ) {
		KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
		KT_ASSERT(knl_read_parttbl(sf_read, &dd, (UD)di.blockcont, &tbl) >= KT_DISK_PARTS);
		KT_ASSERT(tbl.gpt);
		tk_cls_dev(dd, 0);
	}
}

/* ---------------------------------------------------------------- the disk as it is */

IMPORT UB	knl_fat_cut_dev[FS_DEVNM_MAX];
IMPORT INT	knl_fat_cut_left;
IMPORT BOOL	knl_fat_cut_hit;
IMPORT UW	knl_fat_checks;

/* Where things are on a volume, from its boot sector */
typedef struct {
	ID	dd;
	UINT	bits;
	UD	fat_sect, fat_sects, nfats, rsvd, root_sect, nclus, fsi;
} GEOM;

LOCAL ER raw_rw( GEOM *g, UD sect, UB *buf, BOOL write )
{
	SZ	asz;

	return ( write ) ? tk_swri_dev(g->dd, (W)sect, buf, BLK_SECTOR_SIZE, &asz)
			 : tk_srea_dev(g->dd, (W)sect, buf, BLK_SECTOR_SIZE, &asz);
}

/* The device of an unmounted volume opened, and its layout read */
LOCAL BOOL geom_open( VOL *v, GEOM *g )
{
	UB	b[BLK_SECTOR_SIZE];
	UD	tot, rootents, spc, data;

	g->dd = tk_opn_dev((UB *)v->dev, TD_UPDATE);
	if ( g->dd <= 0 ) return FALSE;
	if ( raw_rw(g, 0, b, FALSE) < E_OK ) { tk_cls_dev(g->dd, 0); return FALSE; }
	g->bits      = v->bits;
	spc          = b[13];
	g->rsvd      = (UD)b[14] | ((UD)b[15] << 8);
	g->nfats     = b[16];
	rootents     = (UD)b[17] | ((UD)b[18] << 8);
	tot          = (UD)b[19] | ((UD)b[20] << 8);
	if ( tot == 0 ) tot = (UD)b[32] | ((UD)b[33] << 8) | ((UD)b[34] << 16) | ((UD)b[35] << 24);
	g->fat_sects = (UD)b[22] | ((UD)b[23] << 8);
	if ( g->fat_sects == 0 ) {
		g->fat_sects = (UD)b[36] | ((UD)b[37] << 8) | ((UD)b[38] << 16) | ((UD)b[39] << 24);
	}
	g->fat_sect  = g->rsvd;
	g->root_sect = g->rsvd + g->nfats * g->fat_sects;
	data         = g->root_sect + ( rootents * 32 + BLK_SECTOR_SIZE - 1 ) / BLK_SECTOR_SIZE;
	g->nclus     = ( tot - data ) / spc;
	g->fsi       = ( v->bits == 32 ) ? ( (UD)b[48] | ((UD)b[49] << 8) ) : 0;
	if ( v->bits == 32 ) {
		/* the first sector of the root: cluster 2 in the images the tests use */
		g->root_sect = data;
	}
	return TRUE;
}

LOCAL UW rd32( CONST UB *p )
{
	return (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
}

LOCAL void wr32( UB *p, UW v )
{
	p[0] = (UB)v; p[1] = (UB)(v >> 8); p[2] = (UB)(v >> 16); p[3] = (UB)(v >> 24);
}

/* Byte 'off' of copy 'copy' of the table, read and, with 'set', written */
LOCAL UB fat_byte( GEOM *g, UD copy, UD off, BOOL set, UB val, UB mask )
{
	UB	b[BLK_SECTOR_SIZE];
	UD	s = g->fat_sect + copy * g->fat_sects + off / BLK_SECTOR_SIZE;

	raw_rw(g, s, b, FALSE);
	if ( set ) {
		b[off % BLK_SECTOR_SIZE] = (UB)(( b[off % BLK_SECTOR_SIZE] & ~mask ) | ( val & mask ));
		raw_rw(g, s, b, TRUE);
	}
	return b[off % BLK_SECTOR_SIZE];
}

/* Entry 'c' of copy 'copy' of the table */
LOCAL UD fat_ent( GEOM *g, UD copy, UD c )
{
	UD	w, i;

	switch ( g->bits ) {
	  case 12:
		w = fat_byte(g, copy, c + c / 2, FALSE, 0, 0)
		  | ((UD)fat_byte(g, copy, c + c / 2 + 1, FALSE, 0, 0) << 8);
		return ( c & 1 ) ? ( w >> 4 ) : ( w & 0xFFF );
	  case 16:
		return fat_byte(g, copy, c * 2, FALSE, 0, 0) | ((UD)fat_byte(g, copy, c * 2 + 1, FALSE, 0, 0) << 8);
	  default:
		for ( w = 0, i = 0; i < 4; i++ ) w |= (UD)fat_byte(g, copy, c * 4 + i, FALSE, 0, 0) << (i * 8);
		return w & 0x0FFFFFFF;
	}
}

LOCAL void fat_ent_set( GEOM *g, UD copy, UD c, UD val )
{
	UD	i;

	switch ( g->bits ) {
	  case 12:
		if ( c & 1 ) {
			fat_byte(g, copy, c + c / 2, TRUE, (UB)(val << 4), 0xF0);
			fat_byte(g, copy, c + c / 2 + 1, TRUE, (UB)(val >> 4), 0xFF);
		} else {
			fat_byte(g, copy, c + c / 2, TRUE, (UB)val, 0xFF);
			fat_byte(g, copy, c + c / 2 + 1, TRUE, (UB)(val >> 8), 0x0F);
		}
		break;
	  case 16:
		fat_byte(g, copy, c * 2, TRUE, (UB)val, 0xFF);
		fat_byte(g, copy, c * 2 + 1, TRUE, (UB)(val >> 8), 0xFF);
		break;
	  default:
		for ( i = 0; i < 4; i++ ) fat_byte(g, copy, c * 4 + i, TRUE, (UB)(val >> (i * 8)), 0xFF);
		break;
	}
}

/* The mark of being in use: entry 1 of the table, or the boot sector of FAT12 */
LOCAL BOOL raw_inuse( GEOM *g )
{
	UB	b[BLK_SECTOR_SIZE];
	UD	w;

	switch ( g->bits ) {
	  case 12:
		raw_rw(g, 0, b, FALSE);
		return ( b[37] & 1 ) != 0;
	  case 16:
		w = fat_byte(g, 0, 2, FALSE, 0, 0) | ((UD)fat_byte(g, 0, 3, FALSE, 0, 0) << 8);
		return ( w & 0x8000 ) == 0;
	  default:
		w = fat_byte(g, 0, 7, FALSE, 0, 0);
		return ( w & 0x08 ) == 0;
	}
}

LOCAL void raw_set_inuse( GEOM *g )
{
	UB	b[BLK_SECTOR_SIZE];
	UD	k;

	for ( k = 0; k < g->nfats; k++ ) {
		switch ( g->bits ) {
		  case 12:
			raw_rw(g, 0, b, FALSE);
			b[37] |= 1;
			raw_rw(g, 0, b, TRUE);
			return;
		  case 16:
			fat_byte(g, k, 3, TRUE, 0, 0x80);
			break;
		  default:
			fat_byte(g, k, 7, TRUE, 0, 0x08);
			break;
		}
	}
}

/* A free cluster from the end of the table, below 'below' */
LOCAL UD raw_free( GEOM *g, UD below )
{
	UD	c;

	for ( c = below - 1; c >= 2; c-- ) {
		if ( fat_ent(g, 0, c) == 0 && fat_ent(g, 1, c) == 0 ) return c;
	}
	return 0;
}

LOCAL VOL *find_vol( UINT bits, BOOL usb )
{
	INT	i;

	for ( i = 0; i < NVOL; i++ ) {
		if ( vol[i].up && vol[i].dev != NULL && vol[i].bits == bits
		  && ( vol[i].usb_blocks != 0 ) == usb ) return &vol[i];
	}
	return NULL;
}

#define EACH_UNMOUNTABLE(v)	for ( v = &vol[0]; v < &vol[NVOL]; v++ ) if ( v->up && v->dev != NULL && \
				( tm_printf((UB *)"  %s\n", v->label), TRUE ) )

/* ---------------------------------------------------------------- the mark of being in use */

/*
 * Mounted and not changed, the volume stays unmarked; the first change
 * marks it, unmounting takes the mark off, and the next mount of a
 * volume without the mark does not look it over.
 */
LOCAL void test_inuse( void )
{
	char	p[FS_PATH_MAX];
	GEOM	g;
	VOL	*v;
	UW	n;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_UNMOUNTABLE(v) {
		KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
		KT_ASSERT(geom_open(v, &g));
		KT_ASSERT(!raw_inuse(&g));
		n = knl_fat_checks;
		KT_ASSERT_ER(fs_attach("fatfs", v->dev, v->mnt, 0), EX_OK);
		KT_ASSERT_EQ(knl_fat_checks, n);
		KT_ASSERT(!raw_inuse(&g));

		KT_ASSERT_EQ(write_pat(pj(p, v->dir, "MARK.TXT"), 0x55, 100, 100), 100);
		KT_ASSERT(raw_inuse(&g));
		KT_ASSERT_ER(fs_unlink(p), EX_OK);
		KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
		KT_ASSERT(!raw_inuse(&g));

		/* read-only: the mark is left as it is */
		KT_ASSERT_ER(fs_attach("fatfs", v->dev, v->mnt, FS_MNT_RDONLY), EX_OK);
		KT_ASSERT(exists(pj(p, v->mnt, "HELLO.TXT")));
		KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
		KT_ASSERT(!raw_inuse(&g));
		KT_ASSERT_EQ(knl_fat_checks, n);
		tk_cls_dev(g.dd, 0);
		KT_ASSERT_ER(fs_attach("fatfs", v->dev, v->mnt, 0), EX_OK);
	}
}

/* ---------------------------------------------------------------- FSInfo */

LOCAL UW fsi_free( GEOM *g, UW *p_next )
{
	UB	b[BLK_SECTOR_SIZE];

	raw_rw(g, g->fsi, b, FALSE);
	if ( p_next != NULL ) *p_next = rd32(b + 492);
	return rd32(b + 488);
}

LOCAL void fsi_put( GEOM *g, UW cnt )
{
	UB	b[BLK_SECTOR_SIZE];

	raw_rw(g, g->fsi, b, FALSE);
	wr32(b + 488, cnt);
	raw_rw(g, g->fsi, b, TRUE);
}

/*
 * FSInfo on the disk follows the count after a sync; a count it does
 * not know, or one out of range, is counted again.
 */
LOCAL void test_fsinfo( void )
{
	GEOM	g;
	VOL	*v;
	char	p[FS_PATH_MAX];
	UD	f0, want;
	UW	next;
	INT	k;

	/* /boot: written back on sync */
	if ( vol[2].up ) {
		VOL	boot = vol[2];

		boot.dev = KT_BOOTDEV;
		KT_ASSERT_ER(fs_sync(), EX_OK);
		if ( geom_open(&boot, &g) ) {
			KT_ASSERT_EQ((UD)fsi_free(&g, &next), bfree(&vol[2]));
			KT_ASSERT(next >= 2 && next < g.nclus + 2);
			tk_cls_dev(g.dd, 0);
		}
	}

	v = find_vol(32, TRUE);
	if ( v == NULL ) KT_SKIP("no FAT32 USB disk");
	f0 = bfree(v);
	KT_ASSERT_EQ(write_pat(pj(p, v->dir, "FSI.BIN"), 0x66, 40000, 4096), 40000);
	want = f0 - nclus(v, 40000);		/* while the volume is there to ask */
	KT_ASSERT_ER(fs_sync(), EX_OK);
	KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
	KT_ASSERT(geom_open(v, &g));
	KT_ASSERT_EQ((UD)fsi_free(&g, &next), want);
	for ( k = 0; k < 2; k++ ) {
		/* unknown, then out of range */
		fsi_put(&g, ( k == 0 ) ? 0xFFFFFFFF : (UW)(g.nclus + 5));
		KT_ASSERT_ER(fs_attach("fatfs", v->dev, v->mnt, 0), EX_OK);
		KT_ASSERT_EQ(bfree(v), want);
		KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
		KT_ASSERT_EQ((UD)fsi_free(&g, NULL), want);
	}
	tk_cls_dev(g.dd, 0);
	KT_ASSERT_ER(fs_attach("fatfs", v->dev, v->mnt, 0), EX_OK);
	KT_ASSERT_ER(fs_unlink(p), EX_OK);
	KT_ASSERT_EQ(bfree(v), f0);
}

/* ---------------------------------------------------------------- the look over at mount */

/*
 * A volume left marked, with what a cut can leave behind put on it from
 * outside: a cluster taken that no file has (in both copies of the
 * table), one taken in the second copy only, a long name entry that
 * belongs to nothing, a rename mark left on HELLO.TXT, and on FAT32 a
 * wrong count in FSInfo. The mount puts all of it right.
 */
LOCAL void test_check( void )
{
	UB	b[BLK_SECTOR_SIZE];
	char	p[FS_PATH_MAX];
	GEOM	g;
	VOL	*v;
	UD	f0, x, y, hello_off, lfn_off;
	UW	n;
	INT	i;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_UNMOUNTABLE(v) {
		f0 = bfree(v);
		KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
		KT_ASSERT(geom_open(v, &g));
		raw_set_inuse(&g);
		x = raw_free(&g, g.nclus + 2);
		y = raw_free(&g, x);
		KT_ASSERT(x > 2 && y > 2);
		fat_ent_set(&g, 0, x, 0x0FFFFFFF);
		fat_ent_set(&g, 1, x, 0x0FFFFFFF);
		fat_ent_set(&g, 1, y, 0x0FFFFFFF);

		/* in the first sector of the root: HELLO.TXT, and a free entry
		   with no entry after it that a long name could belong to */
		raw_rw(&g, g.root_sect, b, FALSE);
		hello_off = lfn_off = 0;
		for ( i = 0; i < 15; i++ ) {
			UB *e = b + i * 32;

			if ( e[0] == 'H' && e[1] == 'E' && e[8] == 'T' ) hello_off = (UD)i * 32 + 1;
			if ( ( e[0] == 0xE5 || e[0] == 0 ) && ( e[32] == 0xE5 || e[32] == 0 )
			  && lfn_off == 0 ) lfn_off = (UD)i * 32 + 1;
		}
		KT_ASSERT(hello_off != 0 && lfn_off != 0);
		if ( hello_off != 0 ) b[hello_off - 1 + 12] |= 0x40;
		if ( lfn_off != 0 ) {
			UB *e = b + lfn_off - 1;

			knl_memset(e, 0xFF, 32);
			e[0] = 0x41; e[11] = 0x0F; e[12] = 0; e[13] = 0x5A; e[26] = 0; e[27] = 0;
		}
		raw_rw(&g, g.root_sect, b, TRUE);
		if ( v->bits == 32 ) fsi_put(&g, 5);

		n = knl_fat_checks;
		KT_ASSERT_ER(fs_attach("fatfs", v->dev, v->mnt, 0), EX_OK);
		KT_ASSERT_EQ(knl_fat_checks, n + 1);
		KT_ASSERT_EQ(bfree(v), f0);
		KT_ASSERT_EQ(size_of(pj(p, v->mnt, "HELLO.TXT")), str_len(v->hello));
		KT_ASSERT_ER(fs_sync(), EX_OK);
		KT_ASSERT_EQ(fat_ent(&g, 0, x), 0);
		KT_ASSERT_EQ(fat_ent(&g, 1, x), 0);
		KT_ASSERT_EQ(fat_ent(&g, 1, y), 0);
		raw_rw(&g, g.root_sect, b, FALSE);
		if ( hello_off != 0 ) KT_ASSERT_EQ(b[hello_off - 1 + 12] & 0xC0, 0);
		if ( lfn_off != 0 ) KT_ASSERT(b[lfn_off - 1] == 0xE5 || b[lfn_off - 1] == 0);
		if ( v->bits == 32 ) KT_ASSERT_EQ((UD)fsi_free(&g, NULL), f0);
		tk_cls_dev(g.dd, 0);
	}
}

/* ---------------------------------------------------------------- renames cut off */

/* The last name of a path, and the directory it is in copied to 'dir' */
LOCAL CONST char *split_path( CONST char *path, char *dir )
{
	INT	i, slash = 0;

	for ( i = 0; path[i] != '\0'; i++ ) {
		if ( path[i] == '/' ) slash = i;
	}
	for ( i = 0; i < slash; i++ ) dir[i] = path[i];
	dir[slash] = '\0';

	return path + slash + 1;
}

/* How many times a directory lists a name */
LOCAL INT count_name( CONST char *dir, CONST char *name )
{
	T_DIRENT	de[5];
	INT		fd, n, i, cnt = 0;

	fd = fs_open(dir, O_RDONLY | O_DIRECTORY);
	if ( fd < 0 ) return -1;
	while ( ( n = fs_getdents(fd, de, 5) ) > 0 ) {
		for ( i = 0; i < n; i++ ) {
			if ( str_eq((CONST char *)de[i].name, name) ) cnt++;
		}
	}
	fs_close(fd);

	return cnt;
}

/*
 * A rename stopped after each number of sectors it writes, as a power
 * cut would stop it, and the volume mounted again: the rename has
 * happened or not, never half, and no cluster is lost or shared. Each
 * kind is cut until it runs through; both outcomes must have been seen.
 */
#define CUT_KINDS	6

LOCAL CONST char	*cut_to[CUT_KINDS] = {
	"CUT/名前を替えた長いファイル.txt",		/* in one directory, to a long name */
	"CUT/SUB/移したファイル.txt",			/* to another directory */
	"CUT/SUB/移したディレクトリ",			/* a directory, to another parent */
	"CUT/SUB/T.TXT",				/* over a file */
	"CUT/空のファイルの長い名前.txt",		/* an empty file: it has no cluster */
	"CUT/名前を替えて二つのセクタにまたがる.txt",	/* the new group across two sectors */
};

LOCAL void cut_setup( VOL *v, INT kind, char *from )
{
	char	p[FS_PATH_MAX], q[FS_PATH_MAX];
	INT	fd;

	pj(p, v->dir, "CUT");
	rm_tree(p, FALSE, FALSE);
	fs_mkdir(p);
	fs_mkdir(pj(q, p, "SUB"));
	switch ( kind ) {
	  case 2:
		fs_mkdir(pj(from, p, "D1"));
		write_pat(pj(q, from, "F.TXT"), 0x32, 700, 700);
		break;
	  case 4:
		fd = fs_open(pj(from, p, "E.TXT"), O_WRONLY | O_CREAT);
		if ( fd >= 0 ) fs_close(fd);
		break;
	  case 5:
		/*
		 * Entries 4 to 23 taken and 14 to 23 given back: the new
		 * group of three goes to 14, 15 and 16, the last two in the
		 * directory's second sector, which it already has
		 */
		for ( fd = 0; fd < 20; fd++ ) {
			fs_close(fs_open(pj(q, p, numname(from, "P", fd, ".TXT")), O_WRONLY | O_CREAT));
		}
		for ( fd = 10; fd < 20; fd++ ) {
			fs_unlink(pj(q, p, numname(from, "P", fd, ".TXT")));
		}
		write_pat(pj(from, p, "A.TXT"), 0x31, 2000, 2000);
		break;
	  case 3:
		write_pat(pj(q, p, "SUB/T.TXT"), 0x33, 5000, 5000);
		/* fall through */
	  default:
		write_pat(pj(from, p, "A.TXT"), 0x31, 2000, 2000);
		break;
	}
	fs_sync();
}

/* The state after the rename (done) or before it, as it should be */
LOCAL BOOL cut_state( VOL *v, INT kind, CONST char *from, CONST char *to, BOOL done )
{
	char	p[FS_PATH_MAX], q[FS_PATH_MAX];
	T_FSTAT	st, st2;
	BOOL	ok = TRUE;

	switch ( kind ) {
	  case 2: {
		CONST char *d = ( done ) ? to : from;

		ok = check_pat(pj(p, d, "F.TXT"), 0x32, 700);
		/* ".." names the directory it is in */
		if ( fs_stat(pj(p, d, ".."), &st) < EX_OK
		  || fs_stat(pj(q, v->dir, ( done ) ? "CUT/SUB" : "CUT"), &st2) < EX_OK
		  || st.ino != st2.ino ) {
			tm_printf((UB *)"  %s/.. is not its parent\n", d);
			ok = FALSE;
		}
		break;
	  }
	  case 3:
		ok = check_pat(to, ( done ) ? 0x31 : 0x33, ( done ) ? 2000 : 5000);
		if ( !done ) ok = ok && check_pat(from, 0x31, 2000);
		break;
	  case 4:
		ok = ( size_of( ( done ) ? to : from ) == 0 );
		break;
	  default:
		ok = check_pat(( done ) ? to : from, 0x31, 2000);
		break;
	}
	return ok;
}

LOCAL void test_rename_cut( void )
{
	char	from[FS_PATH_MAX], to[FS_PATH_MAX], dir[FS_PATH_MAX];
	CONST char *nm;
	VOL	*v;
	UD	f0, want;
	INT	kind, n, i, fwd, back;
	BOOL	done;
	ER	er;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_UNMOUNTABLE(v) {
		for ( kind = 0; kind < CUT_KINDS; kind++ ) {
			fwd = back = 0;
			for ( n = 0; n < 60; n++ ) {
				cut_setup(v, kind, from);
				pj(to, v->dir, cut_to[kind]);
				f0 = bfree(v);

				for ( i = 0; i < FS_DEVNM_MAX && v->dev[i] != '\0'; i++ ) {
					knl_fat_cut_dev[i] = (UB)v->dev[i];
				}
				knl_fat_cut_dev[i] = 0;
				knl_fat_cut_hit = FALSE;
				knl_fat_cut_left = n;
				er = fs_rename(from, to);

				if ( !knl_fat_cut_hit ) {
					knl_fat_cut_left = -1;
					KT_ASSERT_ER(er, EX_OK);
					KT_ASSERT(cut_state(v, kind, from, to, TRUE));
					break;
				}
				/* the power stays off while the volume goes */
				KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
				knl_fat_cut_left = -1;
				KT_ASSERT_ER(fs_attach("fatfs", v->dev, v->mnt, 0), EX_OK);

				done = exists(to) && !exists(from);
				if ( kind == 3 ) {
					KT_ASSERT(exists(to));
				} else {
					KT_ASSERT(exists(to) != exists(from));
				}
				if ( done ) fwd++;
				else back++;
				if ( !cut_state(v, kind, from, to, done) ) {
					tm_printf((UB *)"  kind %d cut after %d sectors\n", kind, n);
					KT_ASSERT(FALSE);
				}
				/* the name the entry has now, once in its directory */
				nm = split_path(( done ) ? to : from, dir);
				KT_ASSERT_EQ(count_name(dir, nm), 1);
				want = f0 + ( ( kind == 3 && done ) ? nclus(v, 5000) : 0 );
				KT_ASSERT_EQ(bfree(v), want);
			}
			tm_printf((UB *)"  kind %d: %d cuts done after, %d before, through at %d\n",
				  kind, fwd, back, n);
			KT_ASSERT(n < 60);
			KT_ASSERT(back > 0);
			KT_ASSERT(fwd > 0);
		}
		rm_tree(pj(from, v->dir, "CUT"), FALSE, FALSE);
	}
}

/* ---------------------------------------------------------------- left for the host */

LOCAL void test_keep( void )
{
	char	a[FS_PATH_MAX], b[FS_PATH_MAX], k[FS_PATH_MAX];
	VOL	*v;
	INT	i;

	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		pj(k, v->dir, "KEEP");
		KT_ASSERT_ER(fs_mkdir(k), EX_OK);
		KT_ASSERT_EQ(write_pat(pj(a, k, "日本語の名前.txt"), 0x11, 3000, 1000), 3000);
		KT_ASSERT_ER(fs_mkdir(pj(a, k, "内側")), EX_OK);
		for ( i = 0; i < 20; i++ ) {
			char nm[16];
			KT_ASSERT_EQ(write_pat(pj(b, a, numname(nm, "K", i, ".TXT")), (UB)i, (UD)i * 100, 512),
				     i * 100);
		}
		KT_ASSERT_ER(fs_mkdir(pj(a, k, "移動先")), EX_OK);
		KT_ASSERT_ER(fs_rename(pj(a, k, "内側"), pj(b, k, "移動先/中へ移した")), EX_OK);
		KT_ASSERT_EQ(write_pat(pj(a, k, "移動先/BIGKEEP.BIN"), 0x22, 300000, 4096), 300000);
		KT_ASSERT_EQ(write_pat(pj(a, k, "消す.txt"), 0x23, 100, 100), 100);
		KT_ASSERT_ER(fs_rename(a, pj(b, k, "名前を変えた.txt")), EX_OK);
		KT_ASSERT_ER(fs_unlink(pj(a, k, "K.TXT")), EX_NOENT);
	}
	KT_ASSERT_ER(fs_sync(), EX_OK);
}

/* Unmounted and mounted again: what was written is on the disk */
LOCAL void test_remount( void )
{
	char	a[FS_PATH_MAX], k[FS_PATH_MAX];
	VOL	*v;
	INT	i;

	for ( i = 0; i < NVOL; i++ ) {
		v = &vol[i];
		if ( !v->up || v->dev == NULL ) continue;
		KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
		KT_ASSERT_ER(fs_attach("fatfs", v->dev, v->mnt, 0), EX_OK);
	}
	if ( nup == 0 ) KT_SKIP("no FAT volume");
	EACH_VOL(v) {
		pj(k, v->dir, "KEEP");
		KT_ASSERT(check_pat(pj(a, k, "日本語の名前.txt"), 0x11, 3000));
		KT_ASSERT(check_pat(pj(a, k, "移動先/BIGKEEP.BIN"), 0x22, 300000));
		KT_ASSERT(check_pat(pj(a, k, "名前を変えた.txt"), 0x23, 100));
		KT_ASSERT(check_pat(pj(a, k, "移動先/中へ移した/K019.TXT"), 19, 1900));
		KT_ASSERT_EQ(listed(k, "名前を変えた.txt", 100), 1);
	}
	for ( i = 0; i < NVOL; i++ ) {
		v = &vol[i];
		if ( !v->up || v->dev == NULL ) continue;
		KT_ASSERT_ER(fs_detach(v->mnt), EX_OK);
		v->up = FALSE;
	}
}

EXPORT void ktest_fat( void )
{
	nup = 0;
	KT_RUN(test_mount);
	KT_RUN(test_premade);
	KT_RUN(test_files);
	KT_RUN(test_dirs);
	KT_RUN(test_names);
	KT_RUN(test_listing);
	KT_RUN(test_many_names);
	KT_RUN(test_big);
	KT_RUN(test_disk_full);
	KT_RUN(test_root_full);
	KT_RUN(test_rename_file);
	KT_RUN(test_rename_dir);
	KT_RUN(test_superfloppy);
	KT_RUN(test_inuse);
	KT_RUN(test_fsinfo);
	KT_RUN(test_check);
	KT_RUN(test_rename_cut);
	KT_RUN(test_keep);
	KT_RUN(test_remount);
}
