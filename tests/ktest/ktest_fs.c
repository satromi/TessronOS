/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_fs.c
 *	File management: the FAT volume mounted on /boot. Reading the files
 *	the image was built with, directories, writing, creating and
 *	deleting, and detach/attach.
 *	Skipped when no FAT volume is mounted.
 */

#include "ktest.h"
#include <ts/fs.h>

#define BOOT		"/boot"
#define HELLO		"/boot/HELLO.TXT"
#define DATA		"/boot/DATA.BIN"
#define INNER		"/boot/SUB/INNER.TXT"

LOCAL CONST char	hello_text[] = "TessronOS FAT test file.\n";
LOCAL BOOL		have_fs = FALSE;

LOCAL INT str_len( CONST char *s )
{
	INT	n = 0;
	while ( s[n] != '\0' ) n++;
	return n;
}

LOCAL BOOL name_is( CONST UB *a, CONST char *b )
{
	INT	i;
	for ( i = 0; a[i] != '\0' && b[i] != '\0'; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return ( a[i] == '\0' && b[i] == '\0' );
}

/* the volume mounted at start-up answers statvfs and stat */
LOCAL void test_mounted( void )
{
	T_FSSTAT	vfs;
	T_FSTAT		st;

	if ( fs_statvfs(BOOT, &vfs) < EX_OK ) {
		KT_SKIP("no FAT volume on /boot");
	}
	have_fs = TRUE;
	tm_printf((UB*)"  /boot: %d blocks of %d bytes, %d free\n",
		(INT)vfs.blocks, (INT)vfs.bsize, (INT)vfs.bfree);
	KT_ASSERT(vfs.blocks > 65525);			/* FAT32 */
	KT_ASSERT(vfs.bfree > 0 && vfs.bfree < vfs.blocks);

	KT_ASSERT_ER(fs_stat(BOOT, &st), EX_OK);
	KT_ASSERT_EQ(st.mode & FS_IFMT, FS_IFDIR);

	KT_ASSERT_ER(fs_stat(HELLO, &st), EX_OK);
	KT_ASSERT_EQ(st.mode & FS_IFMT, FS_IFREG);
	KT_ASSERT_EQ(st.size, str_len(hello_text));
	KT_ASSERT_ER(fs_stat("/boot/NOSUCH.TXT", &st), EX_NOENT);
}

/* a small file reads back exactly */
LOCAL void test_read( void )
{
	UB	buf[64];
	INT	fd, n, i;

	if ( !have_fs ) KT_SKIP("no FAT volume");

	fd = fs_open(HELLO, O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;

	n = fs_read(fd, buf, sizeof(buf));
	KT_ASSERT_EQ(n, str_len(hello_text));
	for ( i = 0; i < n; i++ ) {
		if ( buf[i] != (UB)hello_text[i] ) {
			KT_ASSERT_EQ(buf[i], (UB)hello_text[i]);
			break;
		}
	}
	KT_ASSERT_EQ(fs_read(fd, buf, sizeof(buf)), 0);		/* at the end */
	KT_ASSERT_ER(fs_close(fd), EX_OK);
	KT_ASSERT_ER(fs_close(fd), EX_BADF);			/* closed twice */
}

/* a file of several clusters: each sector carries its own number */
LOCAL void test_read_multi( void )
{
	UB	*buf;
	INT	fd, n, s;
	D	pos;

	if ( !have_fs ) KT_SKIP("no FAT volume");

	buf = (UB *)Kmalloc(4608 + 16);
	KT_ASSERT(buf != NULL);
	if ( buf == NULL ) return;

	fd = fs_open(DATA, O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) { Kfree(buf); return; }

	n = fs_read(fd, buf, 4608 + 16);
	KT_ASSERT_EQ(n, 4608);
	for ( s = 0; s < 9; s++ ) {
		UB	*p = buf + s * 512;
		KT_ASSERT(p[0] == 'S' && p[1] == 'E');
		KT_ASSERT_EQ(p[10], (UB)('0' + s));		/* "SECTOR 000s " */
	}

	/* read one sector from the middle after a seek */
	pos = fs_lseek(fd, 512 * 5, SEEK_SET_);
	KT_ASSERT_EQ(pos, 512 * 5);
	n = fs_read(fd, buf, 512);
	KT_ASSERT_EQ(n, 512);
	KT_ASSERT_EQ(buf[10], (UB)('0' + 5));

	/* an unaligned span across a cluster boundary */
	pos = fs_lseek(fd, 512 * 2 - 4, SEEK_SET_);
	KT_ASSERT_EQ(pos, 512 * 2 - 4);
	n = fs_read(fd, buf, 8);
	KT_ASSERT_EQ(n, 8);
	KT_ASSERT_EQ(buf[4], (UB)'S');
	KT_ASSERT_EQ(buf[14 - 10 + 10], (UB)buf[14 - 10 + 10]);

	KT_ASSERT_EQ(fs_lseek(fd, 0, SEEK_END_), 4608);
	KT_ASSERT_ER(fs_close(fd), EX_OK);
	Kfree(buf);
}

/* the root of the volume lists the files, and a subdirectory is reachable */
LOCAL void test_dir( void )
{
	T_DIRENT	de[8];
	UB		buf[32];
	INT		fd, n, i;
	BOOL		saw_hello = FALSE, saw_data = FALSE, saw_sub = FALSE;

	if ( !have_fs ) KT_SKIP("no FAT volume");

	fd = fs_open(BOOT, O_RDONLY | O_DIRECTORY);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) return;

	n = fs_getdents(fd, de, 8);
	KT_ASSERT(n > 0);
	for ( i = 0; i < n; i++ ) {
		tm_printf((UB*)"  /boot/%s %s %d\n", de[i].name,
			(de[i].mode == FS_IFDIR) ? "dir " : "file", (INT)de[i].size);
		if ( name_is(de[i].name, "HELLO.TXT") ) {
			saw_hello = TRUE;
			KT_ASSERT_EQ(de[i].mode, FS_IFREG);
			KT_ASSERT_EQ(de[i].size, str_len(hello_text));
		}
		if ( name_is(de[i].name, "DATA.BIN") ) {
			saw_data = TRUE;
			KT_ASSERT_EQ(de[i].size, 4608);
		}
		if ( name_is(de[i].name, "SUB") ) {
			saw_sub = TRUE;
			KT_ASSERT_EQ(de[i].mode, FS_IFDIR);
		}
	}
	KT_ASSERT(saw_hello && saw_data && saw_sub);
	KT_ASSERT_ER(fs_close(fd), EX_OK);

	/* a file inside the subdirectory */
	fd = fs_open(INNER, O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		n = fs_read(fd, buf, sizeof(buf));
		KT_ASSERT_EQ(n, 11);
		KT_ASSERT_EQ(buf[0], (UB)'i');
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}

	/* a file is not a directory */
	KT_ASSERT_EQ(fs_open(HELLO, O_RDONLY | O_DIRECTORY), EX_NOTDIR);
	KT_ASSERT_EQ(fs_open("/boot/SUB/NOSUCH", O_RDONLY), EX_NOENT);
}

/* create, write across clusters, read back, truncate, delete */
LOCAL void test_write( void )
{
	UB	*wbuf, *rbuf;
	INT	fd, n, i;
	T_FSTAT	st;
	CONST INT len = 2000;

	if ( !have_fs ) KT_SKIP("no FAT volume");

	wbuf = (UB *)Kmalloc(len);
	rbuf = (UB *)Kmalloc(len);
	KT_ASSERT(wbuf != NULL && rbuf != NULL);
	if ( wbuf == NULL || rbuf == NULL ) return;
	for ( i = 0; i < len; i++ ) wbuf[i] = (UB)(i * 31 + 7);

	fs_unlink("/boot/NEW.TXT");			/* from an earlier run */

	fd = fs_open("/boot/NEW.TXT", O_RDWR | O_CREAT | O_EXCL);
	KT_ASSERT(fd >= 0);
	if ( fd < 0 ) { Kfree(wbuf); Kfree(rbuf); return; }

	n = fs_write(fd, wbuf, len);
	KT_ASSERT_EQ(n, len);
	KT_ASSERT_ER(fs_fstat(fd, &st), EX_OK);
	KT_ASSERT_EQ(st.size, len);
	KT_ASSERT_ER(fs_close(fd), EX_OK);

	/* the same file, opened again */
	KT_ASSERT_ER(fs_stat("/boot/NEW.TXT", &st), EX_OK);
	KT_ASSERT_EQ(st.size, len);

	fd = fs_open("/boot/NEW.TXT", O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		n = fs_read(fd, rbuf, len);
		KT_ASSERT_EQ(n, len);
		for ( i = 0; i < len; i++ ) {
			if ( rbuf[i] != wbuf[i] ) {
				KT_ASSERT_EQ(rbuf[i], wbuf[i]);
				break;
			}
		}
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}

	/* O_EXCL refuses an existing name */
	KT_ASSERT_EQ(fs_open("/boot/NEW.TXT", O_WRONLY | O_CREAT | O_EXCL), EX_EXIST);

	/* overwrite in place, then shorten */
	fd = fs_open("/boot/NEW.TXT", O_RDWR);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		KT_ASSERT_EQ(fs_lseek(fd, 100, SEEK_SET_), 100);
		KT_ASSERT_EQ(fs_write(fd, (UB*)"PATCHED", 7), 7);
		KT_ASSERT_EQ(fs_lseek(fd, 100, SEEK_SET_), 100);
		KT_ASSERT_EQ(fs_read(fd, rbuf, 7), 7);
		KT_ASSERT_EQ(rbuf[0], (UB)'P');
		KT_ASSERT_EQ(rbuf[6], (UB)'D');
		KT_ASSERT_ER(fs_ftruncate(fd, 128), EX_OK);
		KT_ASSERT_ER(fs_fstat(fd, &st), EX_OK);
		KT_ASSERT_EQ(st.size, 128);
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}
	KT_ASSERT_ER(fs_stat("/boot/NEW.TXT", &st), EX_OK);
	KT_ASSERT_EQ(st.size, 128);

	/* O_TRUNC empties it */
	fd = fs_open("/boot/NEW.TXT", O_WRONLY | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		KT_ASSERT_ER(fs_fstat(fd, &st), EX_OK);
		KT_ASSERT_EQ(st.size, 0);
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}

	KT_ASSERT_ER(fs_unlink("/boot/NEW.TXT"), EX_OK);
	KT_ASSERT_ER(fs_stat("/boot/NEW.TXT", &st), EX_NOENT);
	KT_ASSERT_ER(fs_unlink("/boot/NEW.TXT"), EX_NOENT);

	Kfree(wbuf);
	Kfree(rbuf);
}

/* directories can be made, filled, emptied and removed */
LOCAL void test_mkdir( void )
{
	T_DIRENT	de[8];
	T_FSTAT		st;
	INT		fd, n, i;
	BOOL		saw = FALSE;

	if ( !have_fs ) KT_SKIP("no FAT volume");

	fs_unlink("/boot/TESTDIR/F.TXT");
	fs_rmdir("/boot/TESTDIR");

	KT_ASSERT_ER(fs_mkdir("/boot/TESTDIR"), EX_OK);
	KT_ASSERT_ER(fs_mkdir("/boot/TESTDIR"), EX_EXIST);
	KT_ASSERT_ER(fs_stat("/boot/TESTDIR", &st), EX_OK);
	KT_ASSERT_EQ(st.mode & FS_IFMT, FS_IFDIR);

	fd = fs_open("/boot/TESTDIR/F.TXT", O_WRONLY | O_CREAT);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		KT_ASSERT_EQ(fs_write(fd, (UB*)"x", 1), 1);
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}

	/* a directory with something in it is kept */
	KT_ASSERT_ER(fs_rmdir("/boot/TESTDIR"), EX_NOTEMPTY);

	fd = fs_open("/boot/TESTDIR", O_RDONLY | O_DIRECTORY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		n = fs_getdents(fd, de, 8);
		KT_ASSERT(n >= 3);			/* ".", "..", "F.TXT" */
		for ( i = 0; i < n; i++ ) {
			if ( name_is(de[i].name, "F.TXT") ) { saw = TRUE; KT_ASSERT_EQ(de[i].size, 1); }
		}
		KT_ASSERT(saw);
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}

	KT_ASSERT_ER(fs_unlink("/boot/TESTDIR/F.TXT"), EX_OK);
	KT_ASSERT_ER(fs_rmdir("/boot/TESTDIR"), EX_OK);
	KT_ASSERT_ER(fs_stat("/boot/TESTDIR", &st), EX_NOENT);
}

/* the volume can be unmounted and mounted again, and the data survives */
LOCAL void test_detach_attach( void )
{
	T_FSTAT	st;
	INT	fd;

	if ( !have_fs ) KT_SKIP("no FAT volume");

	fd = fs_open(HELLO, O_RDONLY);
	KT_ASSERT(fd >= 0);
	KT_ASSERT_ER(fs_detach(BOOT), EX_BUSY);		/* a file is open */
	KT_ASSERT_ER(fs_close(fd), EX_OK);

	KT_ASSERT_ER(fs_sync(), EX_OK);
	KT_ASSERT_ER(fs_detach(BOOT), EX_OK);
	KT_ASSERT_ER(fs_stat(HELLO, &st), EX_NOENT);
	KT_ASSERT_ER(fs_detach(BOOT), EX_NOENT);

	KT_ASSERT_ER(fs_attach("fatfs", KT_BOOTDEV, BOOT, 0), EX_OK);
	KT_ASSERT_ER(fs_stat(HELLO, &st), EX_OK);
	KT_ASSERT_EQ(st.size, str_len(hello_text));

	KT_ASSERT_ER(fs_attach("fatfs", KT_BOOTDEV, BOOT, 0), EX_BUSY);
	KT_ASSERT_ER(fs_attach("nosuch", KT_BOOTDEV, "/x", 0), EX_NOENT);
}

/* long names are created, found again, listed and deleted */
LOCAL void test_longname( void )
{
	CONST char	*names[] = {
		"/boot/019a1132-762b-7b02-ba2a-a918a9b37c39_0.xtad",
		"/boot/019a1132-762b-7b02-ba2a-a918a9b37c39.json",
		"/boot/MixedCase Name.Txt",
		"/boot/short.txt"
	};
	T_DIRENT	de[16];
	T_FSTAT		st;
	INT		fd, i, k, n;
	INT		found = 0;

	if ( !have_fs ) KT_SKIP("no FAT volume");

	for ( i = 0; i < 4; i++ ) fs_unlink(names[i]);

	for ( i = 0; i < 4; i++ ) {
		fd = fs_open(names[i], O_RDWR | O_CREAT | O_EXCL);
		KT_ASSERT(fd >= 0);
		if ( fd < 0 ) continue;
		KT_ASSERT_EQ(fs_write(fd, (UB*)names[i], 20), 20);
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}

	/* each one is found again under the name it was made with */
	for ( i = 0; i < 4; i++ ) {
		UB	buf[24];

		KT_ASSERT_ER(fs_stat(names[i], &st), EX_OK);
		KT_ASSERT_EQ(st.size, 20);
		fd = fs_open(names[i], O_RDONLY);
		KT_ASSERT(fd >= 0);
		if ( fd < 0 ) continue;
		KT_ASSERT_EQ(fs_read(fd, buf, 20), 20);
		KT_ASSERT_EQ(buf[0], (UB)'/');
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}

	/* a name differing only in case is the same file */
	KT_ASSERT_ER(fs_stat("/boot/mixedcase name.txt", &st), EX_OK);
	KT_ASSERT_EQ(st.size, 20);

	/* the directory shows them in full */
	fd = fs_open(BOOT, O_RDONLY | O_DIRECTORY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		while ( (n = fs_getdents(fd, de, 16)) > 0 ) {
			for ( k = 0; k < n; k++ ) {
				for ( i = 0; i < 4; i++ ) {
					if ( name_is(de[k].name, names[i] + 6) ) {
						found++;
						KT_ASSERT_EQ(de[k].size, 20);
					}
				}
			}
		}
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}
	KT_ASSERT_EQ(found, 4);

	/* deleting takes the long name entries with it */
	for ( i = 0; i < 4; i++ ) {
		KT_ASSERT_ER(fs_unlink(names[i]), EX_OK);
		KT_ASSERT_ER(fs_stat(names[i], &st), EX_NOENT);
	}

	fd = fs_open(BOOT, O_RDONLY | O_DIRECTORY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		found = 0;
		while ( (n = fs_getdents(fd, de, 16)) > 0 ) {
			for ( k = 0; k < n; k++ ) {
				for ( i = 0; i < 4; i++ ) {
					if ( name_is(de[k].name, names[i] + 6) ) found++;
				}
			}
		}
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}
	KT_ASSERT_EQ(found, 0);
}

/* a directory with a long name holds files that are found through it */
LOCAL void test_longname_dir( void )
{
	T_FSTAT	st;
	INT	fd;

	if ( !have_fs ) KT_SKIP("no FAT volume");

	fs_unlink("/boot/Long Directory Name/inside file.txt");
	fs_rmdir("/boot/Long Directory Name");

	KT_ASSERT_ER(fs_mkdir("/boot/Long Directory Name"), EX_OK);
	KT_ASSERT_ER(fs_stat("/boot/Long Directory Name", &st), EX_OK);
	KT_ASSERT_EQ(st.mode & FS_IFMT, FS_IFDIR);

	fd = fs_open("/boot/Long Directory Name/inside file.txt", O_WRONLY | O_CREAT);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		KT_ASSERT_EQ(fs_write(fd, (UB*)"x", 1), 1);
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}
	KT_ASSERT_ER(fs_stat("/boot/Long Directory Name/inside file.txt", &st), EX_OK);
	KT_ASSERT_EQ(st.size, 1);

	KT_ASSERT_ER(fs_rmdir("/boot/Long Directory Name"), EX_NOTEMPTY);
	KT_ASSERT_ER(fs_unlink("/boot/Long Directory Name/inside file.txt"), EX_OK);
	KT_ASSERT_ER(fs_rmdir("/boot/Long Directory Name"), EX_OK);
}

EXPORT void ktest_fs( void )
{
	KT_RUN(test_mounted);
	KT_RUN(test_read);
	KT_RUN(test_read_multi);
	KT_RUN(test_dir);
	KT_RUN(test_write);
	KT_RUN(test_mkdir);
	KT_RUN(test_longname);
	KT_RUN(test_longname_dir);
	KT_RUN(test_detach_attach);
}
