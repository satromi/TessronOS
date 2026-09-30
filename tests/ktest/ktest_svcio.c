/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_svcio.c
 *	What a process reaches files, mounts and the network through
 *	(design 12.2, 12.6): mounting through a disk's device object at
 *	/media/<its name> -- the key's rights, the mount on the object and
 *	its notices, the mount going with the medium -- the owners of
 *	descriptors and sockets and their closing when a process ends, the
 *	limit on a process's descriptors, a process ended while it waits
 *	in the network stack, a process ended again and again in the
 *	middle of its file and object calls -- made by its main task or by
 *	another task of its own -- and the volume it leaves, a
 *	process that hands the kernel an address that is not mapped, the
 *	disk moving a process's pages itself, a process under a user who is not an
 *	administrator, and the object, mount and socket calls through the
 *	SVC gateway from EL0 with pointers good and bad (the program
 *	tests/uprog/svcprog.c, /boot/SVCPROG.ELF).
 *
 *	The last test takes the USB disk out through the QEMU monitor and
 *	it does not come back: nothing after it may want the disk.
 *
 *	Built with NET=user, the machine is on QEMU's user mode network and
 *	the host runs tools/ftpd.py; the last test then reaches that server.
 *	Otherwise it is skipped.
 *
 *	On the Raspberry Pi 5 a USB disk is the user's: the tests that mount
 *	and write one are skipped, and so is taking a device out, which
 *	only the QEMU monitor does.
 */

#include "ktest.h"
#include "tstdlib.h"
#include <ts/fs.h>
#include <ts/so.h>
#include <ts/proc.h>
#include <ts/usb.h>
#include "../../peripheral_kernel/obj/obj.h"

IMPORT UD	knl_pf_free_count( INT zone );
IMPORT UW	knl_prc_ter_inside;

#define SVCPROG		"/boot/SVCPROG.ELF"
#define HELLO		"/boot/HELLO.ELF"
#define SP_HOLD		1		/* as tests/uprog/svcprog.c */
#define SP_NET		2
#define SP_MOUNT	3
#define SP_BLOCK	4
#define SP_USER		5
#define SP_LIMITS	6
#define SP_OBPTR	7
#define SP_BIGIO	8
#define SP_WATCH	9
#define SP_KILLIO	10
#define SP_KFAULT	11
#define SP_OBBIG	12
#define SP_KILLOB	13
#define SP_KILLSUB	17
#define NET_PORT	17100
#define LOOPBACK	0x7f000001UL
#define SO_NUM		SO_MAX

LOCAL BOOL	have_prog = FALSE;
LOCAL BOOL	have_stack = FALSE;

LOCAL void addr_set( struct sockaddr_in *sa, UW addr, UH port )
{
	knl_memset(sa, 0, sizeof(*sa));
	sa->sin_len = sizeof(*sa);
	sa->sin_family = AF_INET;
	sa->sin_port = lwip_htons(port);
	sa->sin_addr.s_addr = lwip_htonl(addr);
}

LOCAL void rcv_timeout( INT s, INT sec )
{
	struct timeval	tv;

	tv.tv_sec = sec;
	tv.tv_usec = 0;
	(void)so_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

LOCAL BOOL str_is( CONST UB *a, CONST char *b )
{
	return ( knl_strcmp((CONST char *)a, b) == 0 );
}

LOCAL INT owned_files( ID pid )
{
	INT	fd, n = 0;

	for ( fd = 0; fd < FS_MAX_FILE; fd++ ) {
		if ( knl_fs_mine(fd, pid) ) n++;
	}
	return n;
}

LOCAL INT owned_socks( ID pid )
{
	INT	s, n = 0;

	for ( s = 0; s < SO_NUM; s++ ) {
		if ( knl_so_mine(s, pid) ) n++;
	}
	return n;
}

/* ---------------------------------------------------------------- the rules of /media */

LOCAL void test_media_path( void )
{
	KT_ASSERT(knl_fs_media_path("/media/uda0"));
	KT_ASSERT(knl_fs_media_path("/media/a"));
	KT_ASSERT(knl_fs_media_path("/media/..."));
	KT_ASSERT(!knl_fs_media_path("/media"));
	KT_ASSERT(!knl_fs_media_path("/media/"));
	KT_ASSERT(!knl_fs_media_path("/media/a/"));
	KT_ASSERT(!knl_fs_media_path("/media/a/b"));
	KT_ASSERT(!knl_fs_media_path("/media/."));
	KT_ASSERT(!knl_fs_media_path("/media/.."));
	KT_ASSERT(!knl_fs_media_path("/mediax/a"));
	KT_ASSERT(!knl_fs_media_path("/boot"));
	KT_ASSERT(!knl_fs_media_path("/usb"));
	KT_ASSERT(!knl_fs_media_path("media/a"));
	KT_ASSERT(!knl_fs_media_path(NULL));
	KT_ASSERT(!knl_fs_media_path("/media/0123456789012345678901234567890123456789012345678901234567890123"));
}

/*
 * The FAT partition of the USB disk the tests plug in: the first
 * partition of a USB disk whose boot sector says FAT32. The disk may
 * still be coming up when this suite runs on its own.
 */
LOCAL CONST char *usb_fat( void )
{
#ifdef RPI5
	tm_printf((UB *)"  no USB disk is written on the Raspberry Pi 5: one there is the user's\n");
	return NULL;
#else
	static CONST char	*names[] = { "uda0", "udb0", "udc0", "udd0" };
	UB			sec[512];
	SZ			asz;
	INT			i, t;
	ID			dd;

	for ( t = 0; t < 20; t++ ) {
		for ( i = 0; i < (INT)( sizeof(names) / sizeof(names[0]) ); i++ ) {
			dd = tk_opn_dev((CONST UB *)names[i], TD_READ);
			if ( dd <= 0 ) continue;
			asz = 0;
			if ( tk_srea_dev(dd, 0, sec, 512, &asz) >= E_OK && sec[510] == 0x55
			  && sec[82] == 'F' && sec[83] == 'A' && sec[84] == 'T' && sec[86] == '2' ) {
				tk_cls_dev(dd, 0);
				return names[i];
			}
			tk_cls_dev(dd, 0);
		}
		tk_dly_tsk(500);
	}
	return NULL;
#endif
}

/* ---------------------------------------------------------------- mounting through device objects */

LOCAL CONST char	*mdev = NULL;		/* the FAT partition, "uda0" */
LOCAL char		mwhole[8];		/* its disk, "uda" */
LOCAL char		mpath[32];		/* where it is mounted, "/media/uda0" */
LOCAL TS_UUID		mobj;			/* its device object */

LOCAL void str_cat( char *d, CONST char *a, CONST char *b )
{
	while ( *a != '\0' ) *d++ = *a++;
	while ( *b != '\0' ) *d++ = *b++;
	*d = '\0';
}

LOCAL BOOL has( CONST UB *j, SZ len, CONST char *s )
{
	SZ	n = (SZ)knl_strlen(s), i, k;

	for ( i = 0; i + n <= len; i++ ) {
		for ( k = 0; k < n && j[i + k] == (UB)s[k]; k++ ) ;
		if ( k == n ) return TRUE;
	}
	return FALSE;
}

/* The device object of that name */
LOCAL ER dev_obj( CONST char *name, TS_UUID *p_uuid )
{
	TS_UUID	*list;
	T_OBREF	r;
	INT	cnt = 0, i;
	ER	er = E_NOEXS;

	list = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 128);
	if ( list == NULL ) {
		return E_NOMEM;
	}
	if ( ob_lst_obj(OB_T_DEVICE, 0, NULL, list, 128, &cnt) >= E_OK ) {
		for ( i = 0; i < cnt; i++ ) {
			if ( ob_ref_obj(&list[i], &r) >= E_OK && str_is(r.name, name) ) {
				*p_uuid = list[i];
				er = E_OK;
				break;
			}
		}
	}
	Kfree(list);
	return er;
}

LOCAL ID port_open( TS_UUID *u )
{
	T_OBCRE	c;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	c.name = (CONST UB *)"ktest.mount";
	if ( ob_cre_obj(&c, u) < E_OK ) {
		return E_SYS;
	}
	return ob_opn_obj(u, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
}

LOCAL void port_close( ID port, CONST TS_UUID *u )
{
	if ( port > 0 ) ob_cls_obj(port);
	(void)ob_del_obj(u);
}

/* A notice of that event on that object, among those waiting, within ms */
LOCAL BOOL notice( ID port, UINT event, CONST TS_UUID *u, INT ms )
{
	T_OBNTM	m;
	SZ	asz;

	for ( ;; ) {
		asz = 0;
		while ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz == (SZ)sizeof(m) ) {
			if ( m.event == event && ts_uuid_cmp(&m.uuid, u) == 0 ) {
				return TRUE;
			}
		}
		if ( ms <= 0 ) {
			return FALSE;
		}
		tk_dly_tsk(100);
		ms -= 100;
	}
}

/* Whether the device object's attributes show a mount, and that one */
LOCAL BOOL shows_mount( ID key, CONST char *what )
{
	UB	*j = (UB *)Kmalloc(1024);
	SZ	asz = 0;
	BOOL	yes = FALSE;

	if ( j == NULL ) {
		return FALSE;
	}
	if ( ob_get_atr(key, j, 1024, &asz) >= E_OK ) {
		yes = has(j, asz, ( what != NULL ) ? what : "\"mount\"");
	}
	Kfree(j);
	return yes;
}

/* The USB partition and its disk, their names and object */
LOCAL BOOL mount_setup( void )
{
	INT	k;

	if ( mdev != NULL ) {
		return TRUE;
	}
	mdev = usb_fat();
	if ( mdev == NULL || dev_obj(mdev, &mobj) < E_OK ) {
		mdev = NULL;
		return FALSE;
	}
	for ( k = 0; mdev[k] != '\0' && !( mdev[k] >= '0' && mdev[k] <= '9' ) && k < 7; k++ ) {
		mwhole[k] = mdev[k];
	}
	mwhole[k] = '\0';
	str_cat(mpath, FS_MEDIA_DIR "/", mdev);
	tm_printf((UB*)"  the USB FAT partition is %s, on %s\n", mdev, mwhole);
	return TRUE;
}

/*
 * Mounting through a device object: the key's rights on the blocks are
 * the permission, the mount point is the device's own name under
 * /media, the object shows the mount and tells of each change.
 */
LOCAL void test_mount_dev( void )
{
	T_OBNTF		req;
	T_FSMNT		*mnt;
	T_FSTAT		st;
	TS_UUID		uw, ub, ch;
	UB		b[8];
	char		f[48], want[96];
	INT		fd, n, i;
	ID		kr, krw, ka, kw, kb, kc, port;
	BOOL		seen = FALSE, boot = FALSE;

	if ( !mount_setup() ) KT_SKIP("no FAT partition on a USB disk");
	KT_ASSERT_ER(dev_obj(mwhole, &uw), E_OK);
	KT_ASSERT_ER(dev_obj(KT_BOOTDEV, &ub), E_OK);
	mnt = (T_FSMNT *)Kmalloc(sizeof(T_FSMNT) * FS_MAX_MOUNT);
	KT_ASSERT(mnt != NULL);
	if ( mnt == NULL ) return;

	kr  = ob_opn_obj(&mobj, OB_OP_R);			/* reads the blocks */
	krw = ob_opn_obj(&mobj, OB_OP_R | OB_OP_WRITE);		/* reads and writes them */
	ka  = ob_opn_obj(&mobj, OB_OP_ATRRD);			/* only looks at it */
	kw  = ob_opn_obj(&uw, OB_OP_R | OB_OP_WRITE);		/* the whole disk */
	kb  = ob_opn_obj(&ub, OB_OP_R | OB_OP_WRITE);		/* /boot's */
	kc  = ob_opn_obj(&ob_uuid_clock, OB_OP_R);		/* not a disk */
	port = port_open(&ch);
	KT_ASSERT(kr > 0 && krw > 0 && ka > 0 && kw > 0 && kb > 0 && kc > 0 && port > 0);
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	KT_ASSERT(ob_ntf_evt(ka, OB_REC_ANY, &req, port) > 0);

	/* refused: not a key, flags not known, not a disk, rights short of the mount */
	KT_ASSERT_ER(fs_attach_dev(0, "fatfs", FS_MNT_RDONLY), EX_BADF);
	KT_ASSERT_ER(fs_attach_dev(kr, "fatfs", 0x10), EX_INVAL);
	KT_ASSERT_ER(fs_attach_dev(kr, NULL, FS_MNT_RDONLY), EX_INVAL);
	KT_ASSERT_ER(fs_attach_dev(kc, "fatfs", FS_MNT_RDONLY), EX_NOTBLK);
	KT_ASSERT_ER(fs_attach_dev(port, "fatfs", FS_MNT_RDONLY), EX_NOTBLK);
	KT_ASSERT_ER(fs_attach_dev(ka, "fatfs", FS_MNT_RDONLY), EX_ACCES);
	KT_ASSERT_ER(fs_attach_dev(kr, "fatfs", 0), EX_ACCES);	/* writing, with a key that reads */
	KT_ASSERT_ER(fs_attach_dev(kr, "nosuch", FS_MNT_RDONLY), EX_NOENT);
	KT_ASSERT_ER(fs_attach_dev(kb, "fatfs", 0), EX_BUSY);		/* /boot's device */
	KT_ASSERT_ER(fs_detach_dev(kb), EX_ACCES);			/* and not a process's to take */
	KT_ASSERT_ER(fs_detach_dev(kr), EX_NOENT);
	KT_ASSERT_ER(fs_detach_dev(kc), EX_NOTBLK);
	KT_ASSERT(!shows_mount(ka, NULL));
	KT_ASSERT(!notice(port, OB_E_CHANGE, &mobj, 0));
	KT_ASSERT(shows_mount(kb, "\"mount\":{\"path\":\"/boot\",\"fs\":\"fatfs\""));

	/* read only: mounted, shown on the object, told, listed, read, and nothing written */
	KT_ASSERT_ER(fs_attach_dev(kr, "fatfs", FS_MNT_RDONLY), EX_OK);
	KT_ASSERT(notice(port, OB_E_CHANGE, &mobj, 0));
	str_cat(want, "\"tessronos\":{\"mount\":{\"path\":\"", mpath);
	str_cat(want, want, "\",\"fs\":\"fatfs\",\"readonly\":true}}");
	KT_ASSERT(shows_mount(ka, want));
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_BUSY);		/* the device */
	KT_ASSERT_ER(fs_attach_dev(kw, "fatfs", FS_MNT_RDONLY), EX_BUSY);	/* the disk it is on */

	n = fs_mounts(mnt, FS_MAX_MOUNT);
	KT_ASSERT(n >= 2);
	for ( i = 0; i < n && i < FS_MAX_MOUNT; i++ ) {
		if ( str_is(mnt[i].path, mpath) ) {
			seen = TRUE;
			KT_ASSERT(str_is(mnt[i].dev, mdev));
			KT_ASSERT(str_is(mnt[i].fimp, "fatfs"));
			KT_ASSERT_EQ(mnt[i].flags, FS_MNT_RDONLY);
			KT_ASSERT_EQ(ts_uuid_cmp(&mnt[i].obj, &mobj), 0);
		}
		if ( str_is(mnt[i].path, "/boot") ) {
			boot = TRUE;
			KT_ASSERT_EQ(ts_uuid_cmp(&mnt[i].obj, &ub), 0);
		}
	}
	KT_ASSERT(seen);
	KT_ASSERT(boot);
	KT_ASSERT_EQ(fs_mounts(NULL, 0), n);

	str_cat(f, mpath, "/HELLO.TXT");
	fd = fs_open(f, O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		KT_ASSERT(fs_read(fd, b, sizeof(b)) >= 3);
		KT_ASSERT(b[0] == 'T' && b[1] == 'e' && b[2] == 's');
	}
	KT_ASSERT_ER(fs_open(f, O_RDWR), EX_ROFS);
	KT_ASSERT_ER(fs_open(f, O_RDONLY | O_TRUNC), EX_ROFS);
	KT_ASSERT_ER(fs_unlink(f), EX_ROFS);
	KT_ASSERT_ER(fs_stat(f, &st), EX_OK);

	/* taken off: not while a file is open, not with a key that only looks */
	KT_ASSERT_ER(fs_detach_dev(kr), EX_BUSY);
	KT_ASSERT_ER(fs_detach_dev(ka), EX_ACCES);
	if ( fd >= 0 ) KT_ASSERT_ER(fs_close(fd), EX_OK);
	KT_ASSERT_ER(fs_detach_dev(kr), EX_OK);
	KT_ASSERT(notice(port, OB_E_CHANGE, &mobj, 0));
	KT_ASSERT(!shows_mount(ka, NULL));
	KT_ASSERT_ER(fs_stat(f, &st), EX_NOENT);
	KT_ASSERT_ER(fs_detach_dev(kr), EX_NOENT);

	/* written to as well; then taken off only with a key that writes */
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_OK);
	KT_ASSERT(shows_mount(ka, "\"readonly\":false"));
	str_cat(f, mpath, "/KTMNT.TXT");
	fd = fs_open(f, O_WRONLY | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		KT_ASSERT_EQ(fs_write(fd, "mount", 5), 5);
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}
	KT_ASSERT_ER(fs_stat(f, &st), EX_OK);
	KT_ASSERT_EQ(st.size, 5);
	KT_ASSERT_ER(fs_unlink(f), EX_OK);
	KT_ASSERT_ER(fs_detach_dev(kr), EX_ACCES);
	KT_ASSERT_ER(fs_detach_dev(krw), EX_OK);

	/* the whole disk is refused by the fimp: it starts with the partition table */
	KT_ASSERT_ER(fs_attach_dev(kw, "fatfs", FS_MNT_RDONLY), EX_INVAL);
	KT_ASSERT(!knl_fs_mount_of(mwhole, NULL));

	ob_cls_obj(kr);
	ob_cls_obj(krw);
	ob_cls_obj(ka);
	ob_cls_obj(kw);
	ob_cls_obj(kb);
	ob_cls_obj(kc);
	port_close(port, &ch);
	Kfree(mnt);
}

/*
 * The medium goes while a file is open on the volume, as the driver
 * tells it: the mount goes at once, the file answers EX_IO without
 * reaching the device, and the mount point is free once it is closed.
 */
LOCAL void test_mount_gone( void )
{
	T_OBNTF		req;
	T_FSMNT		*mnt;
	T_FSTAT		st;
	TS_UUID		ch;
	UB		b[8];
	char		f[48];
	INT		fd, n, i;
	ID		krw, port;

	if ( !mount_setup() ) KT_SKIP("no FAT partition on a USB disk");
	mnt = (T_FSMNT *)Kmalloc(sizeof(T_FSMNT) * FS_MAX_MOUNT);
	krw = ob_opn_obj(&mobj, OB_OP_R | OB_OP_WRITE);
	port = port_open(&ch);
	KT_ASSERT(mnt != NULL && krw > 0 && port > 0);
	if ( mnt == NULL || krw <= 0 || port <= 0 ) goto done;
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	KT_ASSERT(ob_ntf_evt(krw, OB_REC_ANY, &req, port) > 0);

	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_OK);
	KT_ASSERT(notice(port, OB_E_CHANGE, &mobj, 0));
	str_cat(f, mpath, "/HELLO.TXT");
	fd = fs_open(f, O_RDONLY);
	KT_ASSERT(fd >= 0);

	knl_obdev_media((CONST UB *)mwhole, FALSE);
	KT_ASSERT(notice(port, OB_E_CHANGE, &mobj, 0));
	KT_ASSERT(!shows_mount(krw, NULL));
	KT_ASSERT_ER(fs_read(fd, b, sizeof(b)), EX_IO);
	KT_ASSERT_ER(fs_lseek(fd, 0, SEEK_SET_), EX_IO);
	KT_ASSERT_ER(fs_fstat(fd, &st), EX_IO);
	KT_ASSERT_ER(fs_stat(f, &st), EX_NOENT);
	KT_ASSERT_ER(fs_open(f, O_RDONLY), EX_NOENT);
	n = fs_mounts(mnt, FS_MAX_MOUNT);
	for ( i = 0; i < n && i < FS_MAX_MOUNT; i++ ) {
		KT_ASSERT(!str_is(mnt[i].path, mpath));
	}
	KT_ASSERT_ER(fs_detach_dev(krw), EX_NOENT);
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_BUSY);		/* still held by the file */
	if ( fd >= 0 ) {
		KT_ASSERT_ER(fs_close(fd), EX_IO);
		KT_ASSERT_ER(fs_close(fd), EX_BADF);
	}

	/* free again; and with nothing open, the medium's going takes it at once */
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_OK);
	knl_obdev_media((CONST UB *)mwhole, FALSE);
	KT_ASSERT(!knl_fs_mount_of(mdev, NULL));
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", FS_MNT_RDONLY), EX_OK);
	fd = fs_open(f, O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) {
		KT_ASSERT(fs_read(fd, b, sizeof(b)) >= 3);
		KT_ASSERT_ER(fs_close(fd), EX_OK);
	}
	KT_ASSERT_ER(fs_detach_dev(krw), EX_OK);
	knl_obdev_media((CONST UB *)mwhole, TRUE);

    done:
	if ( krw > 0 ) ob_cls_obj(krw);
	if ( port > 0 ) port_close(port, &ch);
	if ( mnt != NULL ) Kfree(mnt);
}

/* ---------------------------------------------------------------- owners */

/* The bookkeeping itself, with a process number no process has */
LOCAL void test_owner( void )
{
	struct sockaddr_in	me;
	ID			fake = 9999;
	INT			fd, fd0, s;
	UW			a = 0;

	fd = knl_fs_open_as(HELLO, O_RDONLY, fake);
	fd0 = fs_open(HELLO, O_RDONLY);
	KT_ASSERT(fd >= 0 && fd0 >= 0);
	KT_ASSERT(knl_fs_mine(fd, fake));
	KT_ASSERT(!knl_fs_mine(fd, 0));
	KT_ASSERT(knl_fs_mine(fd0, 0));
	KT_ASSERT(!knl_fs_mine(fd0, fake));
	KT_ASSERT(!knl_fs_mine(-1, 0));
	KT_ASSERT(!knl_fs_mine(FS_MAX_FILE, 0));
	KT_ASSERT_EQ(owned_files(fake), 1);
	knl_fs_prc_end(fake);
	KT_ASSERT_EQ(owned_files(fake), 0);
	KT_ASSERT_ER(fs_close(fd), EX_BADF);		/* closed for it */
	KT_ASSERT_ER(fs_close(fd0), EX_OK);		/* the kernel's stayed */

	if ( so_getifaddr(&a, NULL, NULL) < E_OK ) KT_SKIP("no stack");
	have_stack = TRUE;
	s = knl_so_socket_as(AF_INET, SOCK_DGRAM, 0, fake);
	KT_ASSERT(s >= 0);
	KT_ASSERT(knl_so_mine(s, fake));
	KT_ASSERT(!knl_so_mine(s, 0));
	addr_set(&me, LOOPBACK, NET_PORT + 1);
	KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);
	knl_so_prc_end(fake);
	KT_ASSERT(!knl_so_mine(s, fake));
	KT_ASSERT_EQ(owned_socks(fake), 0);
	KT_ASSERT(so_close(s) < E_OK);			/* closed for it */

	/* the port is free again: the socket really went */
	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	KT_ASSERT(s >= 0);
	KT_ASSERT(knl_so_mine(s, 0));
	KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_close(s), E_OK);
}

/* ---------------------------------------------------------------- a process */

LOCAL void test_program( void )
{
	T_FSTAT	st;

	if ( fs_stat(SVCPROG, &st) < EX_OK ) KT_SKIP("no " SVCPROG);
	KT_ASSERT(st.size > 0);
	have_prog = TRUE;
}

/* The test program started with eight words: what to do, and what for */
LOCAL ID start_args( UW *arg )
{
	T_CPRC	cprc;

	cprc.pri    = KT_PRI_HIGH;
	cprc.prcatr = 0;
	cprc.stksz  = 0;
	cprc.arg    = arg;
	cprc.argsz  = sizeof(UW) * 8;
	return ts_cre_prc(SVCPROG, &cprc);
}

LOCAL ID start_prog( UW mode, UW a1, UW a2, UW a3 )
{
	UW	arg[8];

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = mode;
	arg[1] = a1;
	arg[2] = a2;
	arg[3] = a3;
	return start_args(arg);
}

/*
 * The process opens two files and a socket and keeps them; they are its
 * own while it runs, and gone when it has ended -- by returning, or by
 * a fault. A descriptor of the kernel's stays out of its reach.
 */
LOCAL void hold_run( UB how )
{
	T_TSMSG	*m;
	T_PSTS	psts;
	INT	kfd, t;
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");
	if ( !have_stack ) KT_SKIP("no stack");
	m = (T_TSMSG *)Kmalloc(sizeof(T_TSMSG));
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	kfd = fs_open(HELLO, O_RDONLY);
	KT_ASSERT(kfd >= 0);
	pid = start_prog(SP_HOLD, (UW)kfd, 0, 0);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) goto done;

	for ( t = 0; t < 50 && ( owned_files(pid) < 2 || owned_socks(pid) < 1 ); t++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT_EQ(owned_files(pid), 2);
	KT_ASSERT_EQ(owned_socks(pid), 1);
	KT_ASSERT(knl_fs_mine(kfd, 0));

	knl_memset(m, 0, sizeof(T_TSMSG));
	m->type = 1;
	m->size = 1;
	m->body[0] = how;
	KT_ASSERT_ER(ts_snd_msg(pid, m, 1000), E_OK);
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	tm_printf((UB*)"  the program ended with %d\n", psts.exitcd);
	if ( how == 0 ) {
		KT_ASSERT_EQ(psts.exitcd, 0);
	} else {
		KT_ASSERT_EQ(psts.exitcd, TS_ABORT_FAULT);
	}
	KT_ASSERT_EQ(owned_files(pid), 0);
	KT_ASSERT_EQ(owned_socks(pid), 0);

    done:
	if ( kfd >= 0 ) KT_ASSERT_ER(fs_close(kfd), EX_OK);
	Kfree(m);
}

LOCAL void test_prc_exit( void )
{
	hold_run(0);
}

LOCAL void test_prc_fault( void )
{
	hold_run(1);
}

/* The process's object links to the object of the one socket it has */
LOCAL void prc_links_socket( ID pid )
{
	TS_UUID	pu, su;
	UB	*t;
	char	us[TS_UUID_STRLEN + 1];
	SZ	asz = 0;
	INT	nums[2], i;
	ID	key;
	BOOL	found = FALSE;

	KT_ASSERT_EQ(knl_so_obj_list(pid, nums, &su, 1), 1);
	KT_ASSERT(knl_so_mine(nums[0], pid));
	KT_ASSERT_ER(knl_prc_uuid(pid, &pu, NULL), E_OK);
	t = (UB *)Kmalloc(8192);
	key = ob_opn_obj(&pu, OB_OP_R);
	KT_ASSERT(t != NULL && key > 0);
	if ( t != NULL && key > 0 ) {
		KT_ASSERT_ER(ob_rea_rec(key, 0, 0, t, 8191, &asz), E_OK);
		t[asz] = 0;
		(void)ts_uuid_to_str(&su, us, sizeof(us));
		for ( i = 0; i + TS_UUID_STRLEN <= (INT)asz && !found; i++ ) {
			INT	k;

			for ( k = 0; k < TS_UUID_STRLEN && t[i + k] == (UB)us[k]; k++ ) ;
			found = (BOOL)( k == TS_UUID_STRLEN );
		}
		KT_ASSERT(found);
	}
	if ( key > 0 ) ob_cls_obj(key);
	if ( t != NULL ) Kfree(t);
}

/*
 * The socket calls from EL0: the process connects to a socket of the
 * kernel's over the loopback interface, sends, waits and reads the
 * answer, and tries what it may not do. Its exit code is the first step
 * that went wrong.
 */
LOCAL void test_prc_net( void )
{
	struct sockaddr_in	me;
	T_PSTS			psts;
	UB			b[8];
	INT			srv, acc = -1, n;
	ID			pid;

	if ( !have_prog ) KT_SKIP("no program");
	if ( !have_stack ) KT_SKIP("no stack");

	srv = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(srv >= 0);
	if ( srv < 0 ) return;
	addr_set(&me, LOOPBACK, NET_PORT);
	KT_ASSERT_ER(so_bind(srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
	KT_ASSERT_ER(so_listen(srv, 1), E_OK);
	rcv_timeout(srv, 5);

	pid = start_prog(SP_NET, 0, NET_PORT, (UW)srv);
	KT_ASSERT(pid > 0);
	if ( pid > 0 ) {
		acc = so_accept(srv, NULL, NULL);
		KT_ASSERT(acc >= 0);
	}
	if ( acc >= 0 ) {
		rcv_timeout(acc, 5);
		KT_ASSERT(knl_so_mine(acc, 0));
		KT_ASSERT_EQ(owned_socks(pid), 1);
		n = so_recv(acc, b, 4, 0);
		KT_ASSERT_EQ(n, 4);
		KT_ASSERT(b[0] == 'p' && b[3] == 'g');
		prc_links_socket(pid);
		KT_ASSERT_EQ(so_send(acc, "PING", 4, 0), 4);
		/* and again, the process writing and reading its socket's object */
		n = so_recv(acc, b, 4, 0);
		KT_ASSERT_EQ(n, 4);
		KT_ASSERT(b[0] == 'o' && b[3] == '!');
		KT_ASSERT_EQ(so_send(acc, "PONG", 4, 0), 4);
	}
	if ( pid > 0 ) {
		psts.exitcd = -1;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
		if ( psts.exitcd != 0 ) {
			tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
		}
		KT_ASSERT_EQ(psts.exitcd, 0);
		KT_ASSERT_EQ(owned_socks(pid), 0);
	}
	if ( acc >= 0 ) KT_ASSERT_ER(so_close(acc), E_OK);
	KT_ASSERT_ER(so_close(srv), E_OK);
}

/*
 * Mounting from EL0: the process finds the device object of the USB
 * partition by its name, opens it with the rights it needs and mounts
 * through the key, and tries what it may not do (tests/uprog/svcprog.c,
 * SP_MOUNT). The name goes in the start-up argument.
 */
LOCAL void test_prc_mount( void )
{
	T_PSTS	psts;
	UW	nm[3];
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");
	if ( !mount_setup() ) KT_SKIP("no FAT partition on a USB disk");
	knl_memset(nm, 0, sizeof(nm));
	knl_memcpy(nm, mdev, knl_strlen(mdev));
	pid = start_prog(SP_MOUNT, nm[0], nm[1], nm[2]);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 20000), E_OK);
	if ( psts.exitcd != 0 ) {
		tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
	}
	KT_ASSERT_EQ(psts.exitcd, 0);
	KT_ASSERT(!knl_fs_mount_of(mdev, NULL));
	KT_ASSERT_EQ(owned_files(pid), 0);
}

LOCAL BOOL echo_once( UH port );

/* The program started, run to its end, and its exit code 0: the step it stopped at, if not */
LOCAL void run_to_end( ID pid, TMO tmo )
{
	T_PSTS	psts;

	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = -1;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, tmo), E_OK);
	if ( psts.exitcd != 0 ) {
		tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
	}
	KT_ASSERT_EQ(psts.exitcd, 0);
}

/* The object calls from EL0 refuse pointers that are not the process's, or not writable by it */
LOCAL void test_prc_obptr( void )
{
	if ( !have_prog ) KT_SKIP("no program");
	run_to_end(start_prog(SP_OBPTR, 0, 0, 0), 10000);
}

/* The program's large transfers in the directory dir */
LOCAL void bigio_in( CONST char *dir )
{
	UW	arg[8];
	SYSTIM	t0, t1;

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_BIGIO;
	knl_memcpy(&arg[1], dir, knl_strlen(dir));
	tk_get_tim(&t0);
	run_to_end(start_args(arg), 60000);
	tk_get_tim(&t1);
	tm_printf((UB*)"  %s: %d ms\n", dir, (INT)( t1.lo - t0.lo ));
}

/*
 * fs_read and fs_write from EL0 with buffers of the process's own, on
 * the page and off it, whole and in pieces, on the FAT volume of the
 * virtio disk (/boot) and on that of a USB disk mounted through its
 * device object. The disk moves the process's pages itself, a run of
 * pages that follow one another in memory at a time, and a sector that
 * falls across pages that do not goes through the FAT driver's own
 * buffer; every byte must come back where it was.
 */
LOCAL void test_prc_bigio( void )
{
	ID	krw;

	if ( !have_prog ) KT_SKIP("no program");
	bigio_in("/boot");
	if ( !mount_setup() ) {
		tm_printf((UB*)"  no FAT partition on a USB disk\n");
		return;
	}
	krw = ob_opn_obj(&mobj, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(krw > 0);
	if ( krw <= 0 ) return;
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_OK);
	bigio_in(mpath);
	KT_ASSERT_ER(fs_detach_dev(krw), EX_OK);
	ob_cls_obj(krw);
}

/*
 * A record of BIG_SZ bytes of an object on the store at vol, written
 * and read back by the program from and into its own buffers, on the
 * page and off it; the time it took.
 */
LOCAL void obbig_in( CONST char *vol )
{
	UW	arg[8];
	SYSTIM	t0, t1;

	KT_ASSERT_ER(ob_att_vol(vol, 0), E_OK);
	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_OBBIG;
	knl_memcpy(&arg[1], vol, knl_strlen(vol));
	tk_get_tim(&t0);
	run_to_end(start_args(arg), 60000);
	tk_get_tim(&t1);
	tm_printf((UB*)"  %s: %d ms\n", vol, (INT)( t1.lo - t0.lo ));
	KT_ASSERT_ER(ob_det_vol(vol), E_OK);
}

/* ob_rea_rec and ob_wri_rec from EL0 on a store on /boot and on one on the USB disk */
LOCAL void test_prc_obbig( void )
{
	char	vol[48];
	ID	krw;

	if ( !have_prog ) KT_SKIP("no program");
	obbig_in("/boot/KFYSTORE");
	if ( !mount_setup() ) {
		tm_printf((UB*)"  no FAT partition on a USB disk\n");
		return;
	}
	krw = ob_opn_obj(&mobj, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(krw > 0);
	if ( krw <= 0 ) return;
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_OK);
	str_cat(vol, mpath, "/KFYSTORE");
	obbig_in(vol);
	KT_ASSERT_ER(fs_detach_dev(krw), EX_OK);
	ob_cls_obj(krw);
}

/*
 * The FAT volume mounted at path, looked over as it stands without
 * putting anything right: every chain as long as its file, no cluster
 * taken that no entry has, no rename mark left, the copies of the table
 * alike, the free count right, and the mark of being in use on the disk
 * as the volume has it: on when it has been written to since it was
 * mounted ('written'), off when not.
 */
LOCAL void volume_whole( CONST char *path, BOOL written )
{
	T_FATVFY	vf;

	KT_ASSERT_ER(knl_fs_on_mount(path, knl_fat_verify, &vf), EX_OK);
	if ( vf.bad != 0 || vf.lost != 0 || vf.marks != 0 || vf.copies != 0 || !vf.freeok
	  || vf.partial || vf.ioerr || vf.inuse != written || vf.mark != written ) {
		tm_printf((UB*)"  %s: bad %d lost %d marks %d copies %d freeok %d partial %d"
			   " ioerr %d inuse %d mark %d (%d files)\n", path, vf.bad, vf.lost,
			   vf.marks, vf.copies, vf.freeok, vf.partial, vf.ioerr, vf.inuse,
			   vf.mark, vf.files);
	}
	KT_ASSERT_EQ(vf.bad, 0);
	KT_ASSERT_EQ(vf.lost, 0);
	KT_ASSERT_EQ(vf.marks, 0);
	KT_ASSERT_EQ(vf.copies, 0);
	KT_ASSERT(vf.freeok);
	KT_ASSERT(!vf.partial);
	KT_ASSERT(!vf.ioerr);
	KT_ASSERT_EQ(vf.inuse, written);
	KT_ASSERT_EQ(vf.mark, written);
	KT_ASSERT(vf.files > 0);
}

/* The program started with mode and the path in arg[1..7] */
LOCAL ID start_path( UW mode, CONST char *path )
{
	UW	arg[8];

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = mode;
	knl_memcpy(&arg[1], path, knl_strlen(path));
	return start_args(arg);
}

/*
 * The program started with mode and path, ended by ts_ter_prc a while
 * after it is ready (it holds a file), again and again at moments that
 * differ; each time it is gone within a piece of its work, with its
 * files closed. 'after' looks at what it left.
 */
LOCAL void kill_runs( UW mode, CONST char *path, INT runs, void (*after)( void ) )
{
	T_PSTS	psts;
	SYSTIM	t0, t1;
	INT	r, t, ms, worst = 0;
	UW	inside = knl_prc_ter_inside;
	ID	pid;

	for ( r = 0; r < runs; r++ ) {
		pid = start_path(mode, path);
		KT_ASSERT(pid > 0);
		if ( pid <= 0 ) return;
		for ( t = 0; t < 100 && owned_files(pid) < 1; t++ ) {
			tk_dly_tsk(20);
		}
		tk_dly_tsk(30 + ( r * 137 ) % 500);
		tk_get_tim(&t0);
		KT_ASSERT_ER(ts_ter_prc(pid, -7), E_OK);
		psts.exitcd = 0;
		KT_ASSERT_ER(ts_wai_prc(pid, &psts, 20000), E_OK);
		tk_get_tim(&t1);
		ms = (INT)( t1.lo - t0.lo );
		if ( ms > worst ) worst = ms;
		if ( psts.exitcd != -7 ) {
			tm_printf((UB*)"  the program stopped at step %d\n", psts.exitcd);
		}
		KT_ASSERT_EQ(psts.exitcd, -7);
		KT_ASSERT_EQ(owned_files(pid), 0);
		if ( after != NULL ) {
			after();
		}
	}
	tm_printf((UB*)"  %s: ended %d times, %d of them inside a call, gone at most %d ms"
		   " after ts_ter_prc\n", path, runs, (INT)( knl_prc_ter_inside - inside ), worst);
}

LOCAL CONST char	*kill_dir;

LOCAL void kill_io_after( void )
{
	volume_whole(kill_dir, TRUE);
}

/* The file the program was at, its size the length of its chain (volume_whole), and gone */
LOCAL void kill_io_in( CONST char *dir, INT runs )
{
	char	f[48];
	INT	gone;

	kill_dir = dir;
	kill_runs(SP_KILLIO, dir, runs, kill_io_after);
	/*
	 * The file under one of its two names: an end that came between the
	 * rename and the making anew leaves only the old one.
	 */
	str_cat(f, dir, "/KILLIO.BIN");
	gone = ( fs_unlink(f) == EX_OK ) ? 1 : 0;
	str_cat(f, dir, "/KILLIO.OLD");
	if ( fs_unlink(f) == EX_OK ) gone++;
	KT_ASSERT(gone >= 1);
	volume_whole(dir, TRUE);
}

/*
 * A process ended while it writes, cuts back, renames and makes anew a
 * file on a FAT volume, in calls larger than the gateway's pieces: it
 * leaves the file layer before it goes, and the volume it leaves is
 * whole and marked in use as it should be, on /boot and on a USB disk
 * mounted through its device object. The USB volume then comes off
 * (nothing of the ended calls holds it) and goes on again whole.
 */
LOCAL void test_prc_kill_io( void )
{
	ID	krw;

	if ( !have_prog ) KT_SKIP("no program");
	kill_io_in("/boot", 8);
	if ( !mount_setup() ) {
		tm_printf((UB*)"  no FAT partition on a USB disk\n");
		return;
	}
	krw = ob_opn_obj(&mobj, OB_OP_R | OB_OP_WRITE);
	KT_ASSERT(krw > 0);
	if ( krw <= 0 ) return;
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_OK);
	kill_io_in(mpath, 8);
	KT_ASSERT_ER(fs_detach_dev(krw), EX_OK);
	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_OK);
	volume_whole(mpath, FALSE);
	KT_ASSERT_ER(fs_detach_dev(krw), EX_OK);
	ob_cls_obj(krw);
}

/*
 * The same with the files written by a task the process made, its main
 * task asleep: a task other than the main task inside a call keeps its
 * process from being taken down there as well (design 9.11, 9.15), and
 * at least one of the ends found it inside.
 */
LOCAL void test_prc_kill_sub( void )
{
	UW	inside = knl_prc_ter_inside;

	if ( !have_prog ) KT_SKIP("no program");
	kill_dir = "/boot";
	kill_runs(SP_KILLSUB, "/boot", 8, kill_io_after);
	KT_ASSERT(knl_prc_ter_inside - inside > 0);
	KT_ASSERT_ER(fs_unlink("/boot/KILLIO.BIN"), EX_OK);
	(void)fs_unlink("/boot/KILLIO.OLD");
	volume_whole("/boot", TRUE);
}

#define KILL_STORE	"/boot/KFYKILL"

/* The store takes a call at once after each end: nothing of the ended call holds it */
LOCAL void kill_ob_after( void )
{
	T_OBCRE	c;
	TS_UUID	u;

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.vol = KILL_STORE;
	KT_ASSERT_ER(ob_cre_obj(&c, &u), E_OK);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
	volume_whole("/boot", TRUE);
}

/*
 * A process ended while it writes, cuts short and reads a record of an
 * object on a store on /boot: it leaves the store's calls before it
 * goes, the store goes on taking calls, and the volume is whole. The
 * objects the ended processes made are deleted afterwards.
 */
LOCAL void test_prc_kill_ob( void )
{
	T_OBCRE	c;
	TS_UUID	mark, *list, last;
	INT	cnt = 0, i, n = 0;
	ID	vol;
	BOOL	first = TRUE;

	if ( !have_prog ) KT_SKIP("no program");
	KT_ASSERT_ER(ob_att_vol(KILL_STORE, 0), E_OK);
	kill_runs(SP_KILLOB, KILL_STORE, 6, kill_ob_after);

	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.vol = KILL_STORE;
	KT_ASSERT_ER(ob_cre_obj(&c, &mark), E_OK);
	vol = knl_obfile_vol_of(&mark);
	list = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 256);
	KT_ASSERT(list != NULL && vol > 0);
	/* page by page: with the other suites' objects about, those of this store
	   come after the first page, their UUIDs being the newest */
	while ( list != NULL && vol > 0
	     && ob_lst_obj(OB_T_STORAGE, OB_S_FILE, first ? NULL : &last, list, 256, &cnt) >= E_OK
	     && cnt > 0 ) {
		if ( cnt > 256 ) cnt = 256;
		last = list[cnt - 1];
		first = FALSE;
		for ( i = 0; i < cnt; i++ ) {
			if ( knl_obfile_vol_of(&list[i]) == vol && ob_del_obj(&list[i]) >= E_OK ) n++;
		}
		if ( cnt < 256 ) break;
	}
	if ( list != NULL ) Kfree(list);
	KT_ASSERT(n >= 7);			/* the mark and one of each process's */
	KT_ASSERT_ER(ob_det_vol(KILL_STORE), E_OK);
	volume_whole("/boot", TRUE);
}

/*
 * A process waits for a message into a page of shared memory, made sure
 * of when the call began, and another of its tasks takes the page away
 * meanwhile (SP_KFAULT). The kernel meets the address gone as it hands
 * the message over: the call runs to its end on a page of nothing, the
 * process ends as it returns with TS_ABORT_FAULT, and the system goes
 * on, twice over the same page of nothing, which is not freed with the
 * space (the free pages before and after are shown).
 */
LOCAL void run_kfault( void )
{
	T_PSTS	psts;
	TS_UUID	u;
	ID	pid = start_prog(SP_KFAULT, 0, 0, 0);

	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 10000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, TS_ABORT_FAULT);
	/* the memory it waited into, and its page, are the object's until it goes */
	KT_ASSERT_ER(ob_fnd_nam((CONST UB *)"ktkfault", &u), E_OK);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
}

LOCAL void test_prc_kfault( void )
{
	UD	before, after;
	INT	t;

	if ( !have_prog ) KT_SKIP("no program");
	before = knl_pf_free_count(-1);
	run_kfault();
	for ( t = 0; t < 20; t++ ) {
		after = knl_pf_free_count(-1);
		if ( after == before ) break;
		tk_dly_tsk(50);
	}
	tm_printf((UB*)"  free pages %d before, %d after\n", (INT)before, (INT)after);
	KT_ASSERT_EQ(knl_prc_kfaults, 0);
	run_kfault();				/* and again, on the same page of nothing */
	KT_ASSERT_EQ(knl_prc_kfaults, 0);
}

/*
 * A process holds at most FS_PRC_FILE descriptors, and the kernel can
 * still open files while it does; each names its volume's device object.
 */
LOCAL void test_prc_limits( void )
{
	T_TSMSG	*m;
	INT	fd, t;
	ID	pid;

	if ( !have_prog ) KT_SKIP("no program");
	m = (T_TSMSG *)Kmalloc(sizeof(T_TSMSG));
	KT_ASSERT(m != NULL);
	if ( m == NULL ) return;
	pid = start_prog(SP_LIMITS, 0, 0, 0);
	KT_ASSERT(pid > 0);
	for ( t = 0; pid > 0 && t < 50 && owned_files(pid) < FS_PRC_FILE; t++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT_EQ(owned_files(pid), FS_PRC_FILE);
	fd = fs_open(HELLO, O_RDONLY);
	KT_ASSERT(fd >= 0);
	if ( fd >= 0 ) KT_ASSERT_ER(fs_close(fd), EX_OK);
	knl_memset(m, 0, sizeof(T_TSMSG));
	m->type = 1;
	m->size = 1;
	if ( pid > 0 ) KT_ASSERT_ER(ts_snd_msg(pid, m, 1000), E_OK);
	run_to_end(pid, 10000);
	KT_ASSERT_EQ(owned_files(pid), 0);
	Kfree(m);
}

/*
 * A process under a user that is not an administrator: it logs in as
 * one (ob_login) and is refused what only an administrator may do --
 * setting the interface, opening the disk -- and may mount the disk only
 * through a key an administrator hands it, as far as that key goes.
 */
LOCAL void test_prc_user( void )
{
	T_OBCRE	c;
	T_TSMSG	*m = NULL;
	TS_UUID	u;
	UW	arg[8];
	UB	meta[160];
	INT	n;
	ID	pid, key = 0, dk;
	CONST char *j = "{\"name\":\"ktuser\",\"tessronos\":{\"user\":{\"name\":\"ktuser\",\"groups\":[]}}}";

	if ( !have_prog ) KT_SKIP("no program");
	if ( !mount_setup() ) KT_SKIP("no FAT partition on a USB disk");
	n = (INT)knl_strlen(j);
	knl_memcpy(meta, j, n);
	knl_memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.json = meta;
	c.jsonsz = n;
	if ( ob_cre_obj(&c, &u) < E_OK ) KT_SKIP("no store for a user object");
	KT_ASSERT_ER(ob_set_pwd(&u, (CONST UB *)"ktest-pw"), E_OK);

	knl_memset(arg, 0, sizeof(arg));
	arg[0] = SP_USER;
	knl_memcpy(&arg[1], &u, sizeof(u));
	knl_memcpy(&arg[5], mdev, knl_strlen(mdev));
	pid = start_args(arg);
	KT_ASSERT(pid > 0);
	m = (T_TSMSG *)Kmalloc(sizeof(T_TSMSG));
	key = ob_opn_obj(&mobj, OB_OP_R);
	KT_ASSERT(m != NULL && key > 0);
	if ( pid > 0 && m != NULL && key > 0 ) {
		dk = ob_dup_key(key, OB_OP_R, pid);
		KT_ASSERT(dk > 0);
		knl_memset(m, 0, sizeof(T_TSMSG));
		m->type = 1;
		m->size = sizeof(dk);
		knl_memcpy(m->body, &dk, sizeof(dk));
		KT_ASSERT_ER(ts_snd_msg(pid, m, 1000), E_OK);
	}
	run_to_end(pid, 30000);
	KT_ASSERT(!knl_fs_mount_of(mdev, NULL));

	if ( key > 0 ) ob_cls_obj(key);
	if ( m != NULL ) Kfree(m);
	KT_ASSERT_ER(ob_del_obj(&u), E_OK);
}

/*
 * A process ended while it waits in the network stack -- in so_recv on
 * a connection, or in so_accept -- leaves the stack first and then ends:
 * its sockets are closed, the other end sees the connection close, the
 * port is free again, and the stack goes on working.
 */
LOCAL void kill_waiting( UW how )
{
	struct sockaddr_in	me;
	T_PSTS			psts;
	UB			b[4];
	INT			srv = -1, acc = -1, s, t;
	ID			pid;

	if ( !have_prog ) KT_SKIP("no program");
	if ( !have_stack ) KT_SKIP("no stack");
	addr_set(&me, LOOPBACK, NET_PORT + 2);
	if ( how == 0 ) {
		srv = so_socket(AF_INET, SOCK_STREAM, 0);
		KT_ASSERT(srv >= 0);
		if ( srv < 0 ) return;
		KT_ASSERT_ER(so_bind(srv, (struct sockaddr *)&me, sizeof(me)), E_OK);
		KT_ASSERT_ER(so_listen(srv, 1), E_OK);
		rcv_timeout(srv, 5);
	}
	pid = start_prog(SP_BLOCK, how, NET_PORT + 2, 0);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) goto done;
	if ( how == 0 ) {
		acc = so_accept(srv, NULL, NULL);
		KT_ASSERT(acc >= 0);
		if ( acc >= 0 ) {
			rcv_timeout(acc, 5);
			KT_ASSERT_EQ(so_recv(acc, b, 1, 0), 1);
		}
	}
	for ( t = 0; t < 50 && owned_socks(pid) < 1; t++ ) {
		tk_dly_tsk(100);
	}
	tk_dly_tsk(500);			/* well inside its wait by now */

	KT_ASSERT_ER(ts_ter_prc(pid, -5), E_OK);
	psts.exitcd = 0;
	KT_ASSERT_ER(ts_wai_prc(pid, &psts, 5000), E_OK);
	KT_ASSERT_EQ(psts.exitcd, -5);
	KT_ASSERT_EQ(owned_socks(pid), 0);
	if ( acc >= 0 ) {
		KT_ASSERT_EQ(so_recv(acc, b, sizeof(b), 0), 0);	/* closed from the other end */
	}

	/* the port it listened on is free, and a connection still goes both ways */
	s = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(s >= 0);
	if ( s >= 0 && how == 1 ) {
		KT_ASSERT_ER(so_bind(s, (struct sockaddr *)&me, sizeof(me)), E_OK);
	}
	if ( s >= 0 ) so_close(s);
	KT_ASSERT(echo_once((UH)( NET_PORT + 3 + how )));	/* a port of its own: the last one waits out its close */

    done:
	if ( acc >= 0 ) so_close(acc);
	if ( srv >= 0 ) so_close(srv);
}

/* One connection over the loopback interface, a few bytes each way */
LOCAL BOOL echo_once( UH port )
{
	struct sockaddr_in	me;
	UB			b[8];
	INT			srv, cli, acc = -1;
	BOOL			ok = FALSE;

	srv = so_socket(AF_INET, SOCK_STREAM, 0);
	cli = so_socket(AF_INET, SOCK_STREAM, 0);
	addr_set(&me, LOOPBACK, port);
	if ( srv >= 0 && cli >= 0
	  && so_bind(srv, (struct sockaddr *)&me, sizeof(me)) >= E_OK
	  && so_listen(srv, 1) >= E_OK
	  && so_connect(cli, (struct sockaddr *)&me, sizeof(me)) >= E_OK
	  && ( acc = so_accept(srv, NULL, NULL) ) >= 0 ) {
		rcv_timeout(acc, 5);
		rcv_timeout(cli, 5);
		ok = ( so_send(cli, "echo", 4, 0) == 4 && so_recv(acc, b, 4, 0) == 4
		    && so_send(acc, b, 4, 0) == 4 && so_recv(cli, b, 4, 0) == 4 && b[0] == 'e' );
	}
	if ( acc >= 0 ) so_close(acc);
	if ( cli >= 0 ) so_close(cli);
	if ( srv >= 0 ) so_close(srv);
	return ok;
}

LOCAL void test_prc_kill_recv( void )
{
	kill_waiting(0);
}

LOCAL void test_prc_kill_accept( void )
{
	kill_waiting(1);
}

/* The USB device list changed from gen, within ms */
LOCAL BOOL usb_changed( UW gen, INT ms )
{
	T_USBSTAT	st;

	for ( ; ms > 0; ms -= 50 ) {
		if ( ts_usb_stat(&st) >= E_OK && st.gen != gen ) {
			return TRUE;
		}
		tk_dly_tsk(50);
	}
	return FALSE;
}

/* The USB disk behind the hub, taken out and put back through the QEMU monitor */
#define QMP_USBFS_DEL	"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"usbfs\"}}\n"
#define QMP_USBFS_ADD	"KTEST QMP {\"execute\":\"device_add\",\"arguments\":" \
			"{\"driver\":\"usb-storage\",\"bus\":\"xhci.0\",\"port\":\"4.2\"," \
			"\"drive\":\"usbdisk3\",\"removable\":true,\"id\":\"usbfs\"}}\n"

/*
 * A process watches every device with one request on the list of the
 * devices, holding no device open, and is told of a disk that goes and
 * comes back (tests/uprog/svcprog.c, SP_WATCH). The list's object and
 * its record 0 are there for everyone to read.
 */
LOCAL void test_prc_watch( void )
{
	T_USBSTAT	st;
	T_PSTS		psts;
	T_OBREF		r;
	UB		*t;
	SZ		asz = 0;
	INT		i;
	ID		pid, key;
	UW		gen;

	if ( !have_prog ) KT_SKIP("no program");
	KT_ASSERT_ER(ob_ref_obj(&ob_uuid_devlist, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_DEVLIST);
	key = ob_opn_obj(&ob_uuid_devlist, OB_OP_R);
	t = (UB *)Kmalloc(8192);
	KT_ASSERT(key > 0 && t != NULL);
	if ( key > 0 && t != NULL ) {
		KT_ASSERT_ER(ob_rea_rec(key, 0, 0, t, 8192, &asz), E_OK);
		KT_ASSERT(has(t, asz, KT_BOOTDEV "</p>"));
		KT_ASSERT_ER(ob_get_atr(key, t, 8192, &asz), E_OK);
		KT_ASSERT(has(t, asz, "\"kind\":\"list\""));
	}
	if ( key > 0 ) ob_cls_obj(key);
	if ( t != NULL ) Kfree(t);
#ifdef RPI5
	KT_SKIP("the rest takes a disk out (Raspberry Pi 5: only the QEMU monitor does that)");
#endif
	if ( ts_usb_stat(&st) < E_OK ) KT_SKIP("no USB");

	pid = start_prog(SP_WATCH, 0, 0, 0);
	KT_ASSERT(pid > 0);
	if ( pid <= 0 ) return;
	for ( i = 0; i < 50 && owned_files(pid) < 1; i++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT_EQ(owned_files(pid), 1);

	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;
	tm_printf((UB*)QMP_USBFS_DEL);
	if ( !usb_changed(gen, 5000) ) {
		(void)ts_ter_prc(pid, -1);
		(void)ts_wai_prc(pid, &psts, 5000);
		KT_SKIP("no QEMU monitor to take a device out with");
	}
	tk_dly_tsk(500);
	KT_ASSERT_ER(ts_usb_stat(&st), E_OK);
	gen = st.gen;
	tm_printf((UB*)QMP_USBFS_ADD);
	KT_ASSERT(usb_changed(gen, 5000));
	run_to_end(pid, 20000);
}

/*
 * The disk pulled out for real while a file is open on its volume (the
 * QEMU monitor takes the device away): the disk's object hears that it
 * went, the mount goes and its object is told, the file answers EX_IO,
 * and once the file and the keys are closed the device leaves the
 * kernel's list. The disk does not come back, so this runs last.
 */
LOCAL void test_mount_unplug( void )
{
	T_OBNTF		req;
	T_USBSTAT	st;
	T_RDEV		rd;
	TS_UUID		ch, uw;
	char		f[48];
	INT		fd = -1, t;
	ID		krw, kw, port;

	if ( !mount_setup() ) KT_SKIP("no FAT partition on a USB disk");
	if ( ts_usb_stat(&st) < E_OK ) KT_SKIP("no USB");
	KT_ASSERT_ER(dev_obj(mwhole, &uw), E_OK);
	krw = ob_opn_obj(&mobj, OB_OP_R | OB_OP_WRITE);
	kw = ob_opn_obj(&uw, OB_OP_ATRRD);
	port = port_open(&ch);
	KT_ASSERT(krw > 0 && kw > 0 && port > 0);
	if ( krw <= 0 || kw <= 0 || port <= 0 ) goto done;
	knl_memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	KT_ASSERT(ob_ntf_evt(krw, OB_REC_ANY, &req, port) > 0);
	req.events = OB_E_DETACH;
	KT_ASSERT(ob_ntf_evt(kw, OB_REC_ANY, &req, port) > 0);

	KT_ASSERT_ER(fs_attach_dev(krw, "fatfs", 0), EX_OK);
	KT_ASSERT(notice(port, OB_E_CHANGE, &mobj, 0));
	str_cat(f, mpath, "/UNPLUG.TXT");
	fd = fs_open(f, O_WRONLY | O_CREAT | O_TRUNC);
	KT_ASSERT(fd >= 0);

	tm_printf((UB*)"KTEST QMP {\"execute\":\"device_del\",\"arguments\":{\"id\":\"usbss\"}}\n");
	if ( !usb_changed(st.gen, 5000) ) {
		if ( fd >= 0 ) fs_close(fd);
		(void)fs_detach_dev(krw);
		ob_cls_obj(krw);
		ob_cls_obj(kw);
		port_close(port, &ch);
		KT_SKIP("no QEMU monitor to take a device out with");
	}
	KT_ASSERT(notice(port, OB_E_DETACH, &uw, 5000));
	KT_ASSERT(notice(port, OB_E_CHANGE, &mobj, 5000));
	KT_ASSERT(!knl_fs_mount_of(mdev, NULL));
	if ( fd >= 0 ) {
		KT_ASSERT_ER(fs_write(fd, "gone", 4), EX_IO);
		KT_ASSERT_ER(fs_close(fd), EX_IO);
	}
	KT_ASSERT_ER(fs_open(f, O_RDONLY), EX_NOENT);

	/* nothing holds the disk any more: its name goes */
	ob_cls_obj(krw);
	ob_cls_obj(kw);
	krw = kw = 0;
	for ( t = 0; t < 150 && tk_ref_dev((UB *)mwhole, &rd) >= E_OK; t++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT(tk_ref_dev((UB *)mwhole, &rd) < E_OK);
	KT_ASSERT(tk_ref_dev((UB *)mdev, &rd) < E_OK);
	mdev = NULL;

    done:
	if ( krw > 0 ) ob_cls_obj(krw);
	if ( kw > 0 ) ob_cls_obj(kw);
	if ( port > 0 ) port_close(port, &ch);
}

/* ---------------------------------------------------------------- the host's FTP server */

#ifdef KT_FTP_PORT

#define FTP_HOST	0x0a000202UL		/* 10.0.2.2, the host seen from inside */

/* One reply line; its code, or -1 */
LOCAL INT ftp_reply( INT s, char *line, INT max )
{
	INT	n = 0;
	char	c;

	while ( n < max - 1 ) {
		if ( so_recv(s, &c, 1, 0) != 1 ) {
			break;
		}
		if ( c == '\n' ) {
			line[n] = '\0';
			tm_printf((UB*)"  ftp< %s\n", line);
			if ( n >= 3 ) {
				return ( line[0] - '0' ) * 100 + ( line[1] - '0' ) * 10 + ( line[2] - '0' );
			}
			return -1;
		}
		if ( c != '\r' ) line[n++] = c;
	}
	return -1;
}

/* A command and its line end in one piece: the server answers a whole line */
LOCAL INT ftp_cmd( INT s, CONST char *cmd, char *line, INT max )
{
	char	b[64];
	INT	n = (INT)knl_strlen(cmd);

	if ( n > (INT)sizeof(b) - 2 ) {
		return -1;
	}
	knl_memcpy(b, cmd, n);
	b[n++] = '\r';
	b[n++] = '\n';
	if ( so_send(s, b, n, 0) != n ) {
		return -1;
	}
	return ftp_reply(s, line, max);
}

/*
 * How fast TCP carries bytes between this machine and the host: 16 MB
 * over an FTP data connection, from the host (RETR, the makefile's
 * RATE.BIN) or to it (STOR). Measured, not judged; it only has to
 * arrive whole.
 */
#define RATE_BYTES	(16 * 1024 * 1024)
#define RATE_CHUNK	(32 * 1024)

LOCAL void ftp_rate( INT s, BOOL from_host )
{
	struct sockaddr_in	sa;
	char			line[128];
	CONST char		*p;
	UB			*buf;
	UD			t0 = 0, t1 = 0;
	INT			d, n, port = 0, done = 0, ms;

	KT_ASSERT_EQ(ftp_cmd(s, "EPSV", line, sizeof(line)), 229);
	for ( p = line; *p != '\0' && !( p[0] == '|' && p[1] == '|' && p[2] == '|' ); p++ ) ;
	if ( *p != '\0' ) {
		for ( p += 3; *p >= '0' && *p <= '9'; p++ ) port = port * 10 + ( *p - '0' );
	}
	buf = (UB *)Kmalloc(RATE_CHUNK);
	d = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(port > 0 && buf != NULL && d >= 0);
	if ( port <= 0 || buf == NULL || d < 0 ) {
		if ( buf != NULL ) Kfree(buf);
		if ( d >= 0 ) so_close(d);
		return;
	}
	knl_memset(buf, 0, RATE_CHUNK);
	rcv_timeout(d, 30);
	addr_set(&sa, FTP_HOST, (UH)port);
	KT_ASSERT_ER(so_connect(d, (struct sockaddr *)&sa, sizeof(sa)), E_OK);
	KT_ASSERT_EQ(ftp_cmd(s, from_host ? "RETR RATE.BIN" : "STOR RATE_UP.BIN", line, sizeof(line)), 150);
	(void)ts_get_mono(&t0);
	if ( from_host ) {
		while ( ( n = so_recv(d, buf, RATE_CHUNK, 0) ) > 0 ) {
			done += n;
		}
	} else {
		while ( done < RATE_BYTES && ( n = so_send(d, buf, RATE_CHUNK, 0) ) > 0 ) {
			done += n;
		}
	}
	so_close(d);
	KT_ASSERT_EQ(ftp_reply(s, line, sizeof(line)), 226);
	(void)ts_get_mono(&t1);
	ms = (INT)( ( t1 - t0 ) / 1000000ULL );
	tm_printf((UB *)"  TCP %s the host: %d bytes in %d ms, %d KB/s\n", from_host ? "from" : "to",
		  done, ms, ( ms > 0 ) ? (INT)( (D)done * 1000 / 1024 / ms ) : 0);
	KT_ASSERT_EQ(done, RATE_BYTES);
	Kfree(buf);
}

LOCAL void test_ftp( void )
{
	struct sockaddr_in	sa;
	char			line[128];
	UB			data[64];
	UW			addr = 0, dns = 0;
	INT			s, d, n, t, port = 0, got = 0;
	CONST char		*p;

	if ( !have_stack ) KT_SKIP("no stack");
	KT_ASSERT_ER(so_dhcp_start(), E_OK);
	for ( t = 0; t < 150 && ( so_getifaddr(&addr, NULL, NULL) < E_OK || addr == 0 ); t++ ) {
		tk_dly_tsk(100);
	}
	KT_ASSERT_EQ(lwip_ntohl(addr), 0x0a00020fUL);		/* 10.0.2.15 */
	KT_ASSERT_ER(so_getdns(&dns), E_OK);
	tm_printf((UB*)"  address %08x, name server %08x\n", lwip_ntohl(addr), lwip_ntohl(dns));
	if ( addr == 0 ) return;

	s = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(s >= 0);
	if ( s < 0 ) return;
	rcv_timeout(s, 10);
	addr_set(&sa, FTP_HOST, KT_FTP_PORT);
	KT_ASSERT_ER(so_connect(s, (struct sockaddr *)&sa, sizeof(sa)), E_OK);
	KT_ASSERT_EQ(ftp_reply(s, line, sizeof(line)), 220);
	KT_ASSERT_EQ(ftp_cmd(s, "USER anonymous", line, sizeof(line)), 331);
	KT_ASSERT_EQ(ftp_cmd(s, "PASS tessronos", line, sizeof(line)), 230);
	KT_ASSERT_EQ(ftp_cmd(s, "TYPE I", line, sizeof(line)), 200);
	KT_ASSERT_EQ(ftp_cmd(s, "SIZE KTEST.TXT", line, sizeof(line)), 213);

	/* a file over a data connection: "229 ... (|||port|)" */
	KT_ASSERT_EQ(ftp_cmd(s, "EPSV", line, sizeof(line)), 229);
	for ( p = line; *p != '\0' && !( p[0] == '|' && p[1] == '|' && p[2] == '|' ); p++ ) ;
	if ( *p != '\0' ) {
		for ( p += 3; *p >= '0' && *p <= '9'; p++ ) port = port * 10 + ( *p - '0' );
	}
	KT_ASSERT(port > 0);
	d = so_socket(AF_INET, SOCK_STREAM, 0);
	KT_ASSERT(d >= 0);
	if ( d >= 0 && port > 0 ) {
		rcv_timeout(d, 10);
		addr_set(&sa, FTP_HOST, (UH)port);
		KT_ASSERT_ER(so_connect(d, (struct sockaddr *)&sa, sizeof(sa)), E_OK);
		KT_ASSERT_EQ(ftp_cmd(s, "RETR KTEST.TXT", line, sizeof(line)), 150);
		while ( got < (INT)sizeof(data) && ( n = so_recv(d, data + got, sizeof(data) - got, 0) ) > 0 ) {
			got += n;
		}
		KT_ASSERT_EQ(got, 24);		/* "TessronOS FTP test file
" */
		KT_ASSERT(data[0] == 'T' && data[1] == 'e' && data[2] == 's');
		KT_ASSERT_EQ(ftp_reply(s, line, sizeof(line)), 226);
	}
	if ( d >= 0 ) so_close(d);
	ftp_rate(s, TRUE);
	ftp_rate(s, FALSE);
	KT_ASSERT_EQ(ftp_cmd(s, "QUIT", line, sizeof(line)), 221);
	so_close(s);
}

#else

LOCAL void test_ftp( void )
{
	KT_SKIP("not on the user mode network (NET=user)");
}

#endif /* KT_FTP_PORT */

EXPORT void ktest_svcio( void )
{
	KT_RUN(test_media_path);
	KT_RUN(test_mount_dev);
	KT_RUN(test_mount_gone);
	KT_RUN(test_owner);
	KT_RUN(test_program);
	KT_RUN(test_prc_exit);
	KT_RUN(test_prc_fault);
	KT_RUN(test_prc_net);
	KT_RUN(test_prc_mount);
	KT_RUN(test_prc_obptr);
	KT_RUN(test_prc_bigio);
	KT_RUN(test_prc_obbig);
	KT_RUN(test_prc_kill_io);
	KT_RUN(test_prc_kill_sub);
	KT_RUN(test_prc_kill_ob);
	KT_RUN(test_prc_kfault);
	KT_RUN(test_prc_limits);
	KT_RUN(test_prc_user);
	KT_RUN(test_prc_kill_recv);
	KT_RUN(test_prc_kill_accept);
	KT_RUN(test_prc_watch);
	KT_RUN(test_ftp);
	KT_RUN(test_mount_unplug);	/* last: the disk stays out */
}
