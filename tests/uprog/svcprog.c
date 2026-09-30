/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	svcprog.c
 *	A program the kernel tests start as a process (tests/ktest/ktest_svcio.c)
 *	to reach files, mounts and sockets through the SVC gateway from EL0,
 *	built with lib/libts like any program.
 *
 *	The start-up argument is eight words: what to do, and what the test
 *	hands it for that. The exit code is 0 when every step went as it
 *	should, or the number of the first step that did not.
 *
 *	SP_HOLD	open two files and a socket, then wait for a message; its
 *		first byte says whether to end without closing them (0) or
 *		by a fault (1). The test looks at who owns what meanwhile.
 *		arg[1] is a descriptor of the kernel's, not to be reachable.
 *	SP_NET	talk to the test over TCP on 127.0.0.1:arg[2], also through
 *		the socket's object, and try the calls around it that a
 *		process may and may not make. arg[3] is a socket of the
 *		kernel's, not to be reachable.
 *	SP_MOUNT mount the volume of the disk device named in arg[1..3]
 *		(its bytes, "uda0") through its device object: found by
 *		listing the device objects, opened with the rights the mount
 *		needs, watched for the change, written to, taken off again.
 *	SP_BLOCK wait in the stack until the test ends the process: in
 *		so_recv on a connection to 127.0.0.1:arg[2] (arg[1] 0), or in
 *		so_accept on a socket listening there (arg[1] 1).
 *	SP_USER	log in as the user whose UUID is arg[1..4] (password
 *		"ktest-pw") and try, as someone who is not an administrator,
 *		what only one may do; then mount the disk named in arg[5..7]
 *		through a key the test hands over in a message, as far as
 *		that key's rights go.
 *	SP_LIMITS open as many files as a process may, and one more; wait
 *		for a message while holding them.
 *	SP_OBPTR hand the object calls pointers that are not the
 *		process's to hand, or not writable by it.
 *	SP_BIGIO write a file of BIG_SZ bytes in the directory named in
 *		arg[1..7] and read it back, from and into buffers of the
 *		process's that start on a page and that do not, in one
 *		call and in pieces that begin and end inside sectors.
 *	SP_WATCH watch every device through the list of the devices with
 *		one request; hold a file open to say it is ready, and end
 *		when a disk has been told to go and then to come back.
 *	SP_KILLIO write, cut back, rename and make anew a file in the
 *		directory named in arg[1..7] without end, in writes larger
 *		than the pieces the gateway makes; the test ends it.
 *	SP_KFAULT wait for a message into a page of shared memory that
 *		another of its tasks takes away meanwhile: the kernel meets
 *		the address gone while it works for the process.
 *	SP_OBBIG write a record of BIG_SZ bytes of an object on the store
 *		attached at arg[1..7] and read it back, from and into
 *		buffers on a page and off it.
 *	SP_KILLOB write records of an object on that store without end;
 *		the test ends it.
 *	SP_TKOWN make objects of the core of its own -- a semaphore, an
 *		event flag, a mutex, a message buffer, tasks -- and use them;
 *		try what it may not ask for; fill its room for semaphores and
 *		tasks; then wait for a message: its first byte says whether to
 *		return (0), fault (1) or wait on until the test ends it (2).
 *	SP_TKHOLD make a semaphore and wait for messages until one of type
 *		2: 0 when exactly one other came and the semaphore is as it
 *		was made.
 *	SP_TKATK take in a message the numbers of another process, of its
 *		main task and its semaphore, of a semaphore of the kernel's and
 *		of a third process, and try the core's calls on them; as the
 *		administrator it is by its parent, end the third, look at the
 *		other and send it a message; then log in as the user whose
 *		UUID is arg[1..4] and try to end it, send to it, collect it,
 *		hand it a key.
 *	SP_KILLSUB as SP_KILLIO, but in a task of its own that the main task
 *		made, the main task only sleeping; the test ends it.
 *	SP_DROP	log in as the user whose UUID is arg[1..4], make a window
 *		that takes drops, and use the object dropped on it with the
 *		rights the drop granted: refuse the first drop and be unable
 *		to open it, take the second and read and write it but not
 *		delete it or change its protection; wait for a message.
 *	SP_KILLDRAW make a window and draw in it without end -- rectangles,
 *		lines, letters, a figure -- showing each round; the test ends
 *		it.
 *	SP_KILLMENU make a window and show on it the menu of the definition
 *		whose UUID is arg[1..4], waiting for an answer that does not
 *		come; the test ends it.
 *	SP_DTPTR hand the calendar and the console pointers that are not
 *		the process's, or not writable by it; then a line longer than
 *		the pieces the kernel takes it in.
 *	SP_CLOCK set the time (dt_settime, tk_set_tim, tk_set_utc) and the
 *		zone (dt_setsystz, to arg[5] minutes): logged in as the user
 *		in arg[1..4], who is refused each; with none, as the
 *		administrator, who is not.
 *	SP_PARALLEL start four tasks of its own that each keep a processor
 *		busy for a while, and answer 300 and how many of them were
 *		running at once: on a machine of several processors they are
 *		spread over them, and more than one runs at the same time.
 *	SP_KILLWIN make a window and a sub window of it, and then fault
 *		without closing either.
 *	SP_PAUSEWIN make a window "ktpause" and wait without end, outside
 *		any call that holds it; the test ends it from the keyboard.
 *	SP_DEVOB the device objects as a program sees them (devob): how
 *		long random bytes take, and what a user who is not an
 *		administrator is given and refused.
 *	SP_FAULT fault, after arg[2] milliseconds: arg[1] 0 writes to an
 *		address of its half that is not mapped (FAULT_VA), 1 runs an
 *		instruction that is none (UDF), 2 jumps to that address.
 */

#include <config.h>		/* first: the core's types as wide as the kernel's */
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/syscall.h>
#include <ts/uapp.h>
#include <ts/proc.h>
#include <ts/dt.h>
#include <ts/hid.h>
#include <ts/svc.h>

IMPORT ER ts_get_mono( UD *p_ns );

#define SP_HOLD		1
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
#define SP_TKOWN	14
#define SP_TKHOLD	15
#define SP_TKATK	16
#define SP_KILLSUB	17
#define SP_DROP		18
#define SP_KILLDRAW	19
#define SP_KILLMENU	20
#define SP_DTPTR	21
#define SP_CLOCK	22
#define SP_PARALLEL	23
#define SP_KILLWIN	24
#define SP_PAUSEWIN	25
#define SP_FAULT	26
#define SP_DEVOB	27
#define LIST_MAX	128
#define BIG_SZ		( 160 * 1024 )
#define KILL_SZ		( 640 * 1024 )		/* more than the gateway's pieces */

#define PROG		"/boot/HELLO.ELF"
#define KERNEL_PTR	((void *)0xFFFF000000100000UL)	/* not the process's */

LOCAL UINT	arg[8];
LOCAL T_FSMNT	mnt[FS_MAX_MOUNT];
LOCAL T_TSMSG	msg;
LOCAL TS_UUID	list[LIST_MAX];
LOCAL T_OBNTM	ntm;
LOCAL UB	atr[1024];
LOCAL T_OBCRD	crd;
LOCAL INT	fds[FS_PRC_FILE + 1];
LOCAL UB	wbuf[BIG_SZ + 64] __attribute__((aligned(4096)));
LOCAL UB	rbuf[BIG_SZ + 64] __attribute__((aligned(4096)));
LOCAL UB	kbuf[KILL_SZ];

LOCAL BOOL same( CONST UB *a, CONST char *b )
{
	while ( *b != '\0' && *a == (UB)*b ) {
		a++;
		b++;
	}
	return ( *a == 0 && *b == '\0' );
}

LOCAL INT hold( void )
{
	UB	b[4];
	INT	fd, fd2, s;

	fd = fs_open(PROG, O_RDONLY);
	if ( fd < 0 ) return 1;
	if ( fs_read(fd, b, 4) != 4 || b[0] != 0x7f || b[1] != 'E' ) return 2;
	if ( fs_read((INT)arg[1], b, 4) != EX_BADF ) return 3;
	if ( fs_close((INT)arg[1]) != EX_BADF ) return 4;
	if ( fs_read(fd, KERNEL_PTR, 4) != EX_FAULT ) return 5;
	s = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) return 6;
	fd2 = fs_open(PROG, O_RDONLY);
	if ( fd2 < 0 ) return 7;

	if ( ts_rcv_msg(&msg, 10000) != E_OK ) return 8;
	if ( msg.body[0] == 1 ) {
		*(volatile UINT *)0 = 1;		/* a fault ends it here */
	}
	return 0;				/* and the files and the socket stay open */
}

/* Whether the text of len bytes has 'want' in it */
LOCAL BOOL has_text( CONST UB *t, SZ len, CONST char *want )
{
	SZ	i, k;

	for ( i = 0; i < len; i++ ) {
		for ( k = 0; want[k] != 0 && i + k < len && t[i + k] == (UB)want[k]; k++ ) ;
		if ( want[k] == 0 ) return TRUE;
	}
	return FALSE;
}

LOCAL INT net( void )
{
	struct sockaddr_in	sa, peer;
	struct pollfd		p;
	T_SOTIMEVAL		tv;
	socklen_t		len;
	TS_UUID			u, so;
	T_OBREF			ref;
	T_FSTAT			st;
	UB			b[16];
	static UB		meta[1024];
	SZ			asz;
	UINT			a = 0, m = 0, g = 0;
	INT			s, n, i;
	ID			key;
	BOOL			boot = FALSE;

	/* the connection to the test, and the bytes both ways */
	s = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) return 1;
	tv.sec = 5;
	tv.usec = 0;
	if ( so_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != E_OK ) return 2;
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = so_htons((UH)arg[2]);
	sa.sin_addr.s_addr = SO_IPADDR(127, 0, 0, 1);
	if ( so_connect(s, (struct sockaddr *)&sa, sizeof(sa)) != E_OK ) return 3;
	len = sizeof(peer);
	if ( so_getpeername(s, (struct sockaddr *)&peer, &len) != E_OK ) return 4;
	if ( so_ntohs(peer.sin_port) != (UH)arg[2] ) return 5;
	if ( so_send(s, "ping", 4, 0) != 4 ) return 6;
	p.fd = s;
	p.events = POLLIN;
	p.revents = 0;
	if ( so_poll(&p, 1, 5000) != 1 || ( p.revents & POLLIN ) == 0 ) return 7;
	n = so_recv(s, b, sizeof(b), 0);
	if ( n != 4 || b[0] != 'P' || b[3] != 'G' ) return 8;

	/* the socket as an object: its own, connected, and data through record 1 */
	if ( so_getobj(s, &so) != E_OK ) return 32;
	if ( so_getobj((INT)arg[3], &u) != E_ID ) return 33;
	if ( so_getobj(s, KERNEL_PTR) != E_MACV ) return 34;
	if ( ob_ref_obj(&so, &ref) != E_OK || ref.type != OB_T_CHANNEL || ref.sub != OB_S_SOCKET ) return 35;
	key = ob_opn_obj(&so, OB_OP_R | OB_OP_W);
	if ( key <= 0 ) return 36;
	asz = 0;
	if ( ob_get_atr(key, meta, sizeof(meta), &asz) != E_OK || !has_text(meta, asz, "\"state\":\"connected\"") ) return 37;
	if ( ob_wri_rec(key, OB_SK_DATA, 0, "obj!", 4, &asz) != E_OK || asz != 4 ) return 38;
	asz = 0;
	if ( ob_rea_rec(key, OB_SK_DATA, 0, b, 4, &asz) != E_OK || asz != 4 || b[0] != 'P' || b[1] != 'O' ) return 39;
	ob_cls_obj(key);

	/* what is not its own is refused */
	if ( so_send(s, KERNEL_PTR, 4, 0) != E_MACV ) return 9;
	if ( so_close((INT)arg[3]) != E_ID ) return 10;
	if ( so_close(99) != E_ID ) return 11;
	if ( so_shutdown(s, SHUT_RDWR) != E_OK ) return 12;
	if ( so_close(s) != E_OK ) return 13;
	if ( so_close(s) != E_ID ) return 14;
	if ( ob_ref_obj(&so, &ref) != E_NOEXS ) return 40;	/* the object went with it */

	/* names and the interface */
	if ( so_resolve("127.0.0.1", &a) != E_OK || a != SO_IPADDR(127, 0, 0, 1) ) return 15;
	if ( so_getifaddr(&a, &m, &g) != E_OK ) return 16;
	if ( so_getifaddr(KERNEL_PTR, NULL, NULL) != E_MACV ) return 17;

	/* numbers of its own */
	if ( ts_gen_uuid(&u) != E_OK || ( u.b[6] >> 4 ) != 7 ) return 18;
	if ( ts_get_random(b, sizeof(b)) != E_OK ) return 19;
	if ( ts_get_random(KERNEL_PTR, 16) != E_MACV ) return 20;

	/* the mounts, listed */
	n = fs_mounts(mnt, FS_MAX_MOUNT);
	if ( n < 1 ) return 21;
	for ( i = 0; i < n && i < FS_MAX_MOUNT; i++ ) {
		if ( same(mnt[i].path, "/boot") && same(mnt[i].fimp, "fatfs") ) boot = TRUE;
	}
	if ( !boot ) return 22;
	if ( fs_mounts(KERNEL_PTR, 1) != EX_FAULT ) return 28;

	/* files by path */
	if ( fs_stat(PROG, &st) != EX_OK || st.size == 0 ) return 29;
	if ( fs_open("/boot/../boot/HELLO.ELF", O_RDONLY) != EX_ACCES ) return 30;
	if ( fs_stat(KERNEL_PTR, &st) != EX_FAULT ) return 31;
	return 0;
}

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	INT	i;

	for ( i = 0; i < 16; i++ ) {
		if ( a->b[i] != b->b[i] ) return FALSE;
	}
	return TRUE;
}

/* Whether s is somewhere in the n bytes at j */
LOCAL BOOL has( CONST UB *j, SZ n, CONST char *s )
{
	SZ	len = 0, i, k;

	while ( s[len] != '\0' ) len++;
	for ( i = 0; i + len <= n; i++ ) {
		for ( k = 0; k < len && j[i + k] == (UB)s[k]; k++ ) ;
		if ( k == len ) return TRUE;
	}
	return FALSE;
}

LOCAL void cat( char *d, CONST char *a, CONST char *b )
{
	while ( *a != '\0' ) *d++ = *a++;
	while ( *b != '\0' ) *d++ = *b++;
	*d = '\0';
}

/* A notice of a change on u, among those waiting at the port */
LOCAL BOOL changed( ID port, CONST TS_UUID *u )
{
	SZ	asz = 0;

	while ( ob_rea_rec(port, 0, 0, &ntm, sizeof(ntm), &asz) == E_OK && asz == (SZ)sizeof(ntm) ) {
		if ( ntm.event == OB_E_CHANGE && same_uuid(&ntm.uuid, u) ) return TRUE;
	}
	return FALSE;
}

LOCAL INT mount( void )
{
	T_OBREF	r;
	T_OBCRE	c;
	T_OBNTF	req;
	TS_UUID	u, clk, ch, boot;
	char	name[16], path[48], f[64], want[64];
	UB	b[4];
	SZ	asz = 0;
	INT	cnt = 0, i, n, fd;
	ID	kr, krw, kc, kb, port;
	BOOL	found = FALSE, fclk = FALSE, fboot = FALSE;

	memcpy(name, &arg[1], 12);
	name[12] = '\0';

	/* the device object of that name, and the clock's, among the devices */
	if ( ob_lst_obj(OB_T_DEVICE, 0, NULL, list, LIST_MAX, &cnt) != E_OK ) return 40;
	for ( i = 0; i < cnt && i < LIST_MAX; i++ ) {
		if ( ob_ref_obj(&list[i], &r) != E_OK ) continue;
		if ( same(r.name, name) && r.sub == OB_S_DISK ) {
			u = list[i];
			found = TRUE;
		}
		if ( r.sub == OB_S_CLOCK ) {
			clk = list[i];
			fclk = TRUE;
		}
	}
	if ( !found ) return 41;
	if ( !fclk ) return 42;
	cat(path, FS_MEDIA_DIR "/", name);

	kr = ob_opn_obj(&u, OB_OP_R);
	krw = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE);
	kc = ob_opn_obj(&clk, OB_OP_R);
	if ( kr <= 0 || krw <= 0 || kc <= 0 ) return 43;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) != E_OK ) return 44;
	port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	if ( port <= 0 ) return 45;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	if ( ob_ntf_evt(kr, OB_REC_ANY, &req, port) <= 0 ) return 46;

	/* the key's rights are the permission; a clock is no disk */
	if ( fs_attach_dev(kr, "fatfs", 0) != EX_ACCES ) return 47;
	if ( fs_attach_dev(kc, "fatfs", FS_MNT_RDONLY) != EX_NOTBLK ) return 48;
	if ( fs_attach_dev(krw, KERNEL_PTR, 0) != EX_FAULT ) return 49;
	if ( fs_attach_dev(krw, "fatfs", 0) != EX_OK ) return 50;
	if ( fs_attach_dev(krw, "fatfs", 0) != EX_BUSY ) return 51;

	/* told, and shown on the object */
	if ( !changed(port, &u) ) return 52;
	cat(want, "\"path\":\"", path);
	if ( ob_get_atr(kr, atr, sizeof(atr), &asz) != E_OK || !has(atr, asz, want) ) return 53;

	/* a file written and read back */
	cat(f, path, "/SVCPROG.TXT");
	fd = fs_open(f, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) return 54;
	if ( fs_write(fd, "el0", 3) != 3 ) return 55;
	if ( fs_close(fd) != EX_OK ) return 56;
	fd = fs_open(f, O_RDONLY);
	if ( fd < 0 ) return 57;
	if ( fs_read(fd, b, 3) != 3 || b[0] != 'e' || b[2] != '0' ) return 58;

	/* taken off: not while the file is open, not with a key that only reads */
	if ( fs_detach_dev(krw) != EX_BUSY ) return 59;
	if ( fs_close(fd) != EX_OK ) return 60;
	if ( fs_unlink(f) != EX_OK ) return 61;
	if ( fs_detach_dev(kr) != EX_ACCES ) return 62;
	if ( fs_detach_dev(krw) != EX_OK ) return 63;
	if ( !changed(port, &u) ) return 64;
	asz = 0;
	if ( ob_get_atr(kr, atr, sizeof(atr), &asz) != E_OK || has(atr, asz, "\"mount\"") ) return 65;
	if ( fs_detach_dev(krw) != EX_NOENT ) return 66;

	/* what the system mounted stays: /boot, through its object from the list */
	n = fs_mounts(mnt, FS_MAX_MOUNT);
	for ( i = 0; i < n && i < FS_MAX_MOUNT; i++ ) {
		if ( same(mnt[i].path, "/boot") ) {
			boot = mnt[i].obj;
			fboot = TRUE;
		}
	}
	if ( !fboot ) return 67;
	kb = ob_opn_obj(&boot, OB_OP_R | OB_OP_WRITE);
	if ( kb <= 0 ) return 68;
	if ( fs_detach_dev(kb) != EX_ACCES ) return 69;

	ob_cls_obj(kb);
	ob_cls_obj(kc);
	ob_cls_obj(krw);
	ob_cls_obj(kr);
	ob_cls_obj(port);
	if ( ob_del_obj(&ch) != E_OK ) return 70;
	return 0;
}

/* A socket address of 127.0.0.1 at that port */
LOCAL void local( struct sockaddr_in *sa, UINT port )
{
	memset(sa, 0, sizeof(*sa));
	sa->sin_len = sizeof(*sa);
	sa->sin_family = AF_INET;
	sa->sin_port = so_htons((UH)port);
	sa->sin_addr.s_addr = SO_IPADDR(127, 0, 0, 1);
}

LOCAL INT block( void )
{
	struct sockaddr_in	sa;
	UB			b[4];
	INT			s;

	s = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) return 300;
	local(&sa, arg[2]);
	if ( arg[1] == 0 ) {
		if ( so_connect(s, (struct sockaddr *)&sa, sizeof(sa)) != E_OK ) return 301;
		if ( so_send(s, "r", 1, 0) != 1 ) return 302;
		(void)so_recv(s, b, sizeof(b), 0);	/* nothing comes: it is ended here */
		return 303;
	}
	if ( so_bind(s, (struct sockaddr *)&sa, sizeof(sa)) != E_OK ) return 304;
	if ( so_listen(s, 1) != E_OK ) return 305;
	(void)so_accept(s, NULL, NULL);		/* nobody comes: it is ended here */
	return 306;
}

/* The disk device object of that name */
LOCAL BOOL disk_named( CONST char *name, TS_UUID *u )
{
	T_OBREF	r;
	INT	cnt = 0, i;

	if ( ob_lst_obj(OB_T_DEVICE, 0, NULL, list, LIST_MAX, &cnt) != E_OK ) return FALSE;
	for ( i = 0; i < cnt && i < LIST_MAX; i++ ) {
		if ( ob_ref_obj(&list[i], &r) == E_OK && same(r.name, name) && r.sub == OB_S_DISK ) {
			*u = list[i];
			return TRUE;
		}
	}
	return FALSE;
}

LOCAL INT user( void )
{
	TS_UUID	me, dev;
	char	name[16], f[64], g[64];
	UB	b[4];
	INT	fd;
	ID	key;

	memcpy(&me, &arg[1], sizeof(me));
	memcpy(name, &arg[5], 12);
	name[12] = '\0';

	/* who it is now */
	if ( ob_login(&me, (CONST UB *)"wrong") != E_OACV ) return 400;
	if ( ob_login(&me, (CONST UB *)"ktest-pw") != E_OK ) return 401;
	if ( ob_get_crd(&crd) != E_OK || !same_uuid(&crd.user, &me) ) return 402;

	/* what only an administrator may do */
	if ( so_setifaddr(SO_IPADDR(10, 9, 8, 7), SO_IPADDR(255, 0, 0, 0), 0, 0) != E_OACV ) return 403;
	if ( so_dhcp_start() != E_OACV ) return 404;
	if ( !disk_named(name, &dev) ) return 405;
	if ( ob_opn_obj(&dev, OB_OP_R) != E_OACV ) return 406;	/* the disk's protection */

	/* a key an administrator gives it: as far as that key goes */
	if ( ts_rcv_msg(&msg, 10000) != E_OK ) return 407;
	memcpy(&key, msg.body, sizeof(key));
	if ( fs_attach_dev(key, "fatfs", 0) != EX_ACCES ) return 408;
	if ( fs_attach_dev(key, "fatfs", FS_MNT_RDONLY) != EX_OK ) return 409;
	cat(g, FS_MEDIA_DIR "/", name);
	cat(f, g, "/HELLO.TXT");
	fd = fs_open(f, O_RDONLY);
	if ( fd < 0 ) return 410;
	if ( fs_read(fd, b, 1) != 1 || b[0] != 'T' ) return 411;
	if ( fs_close(fd) != EX_OK ) return 412;
	if ( fs_detach_dev(key) != EX_OK ) return 413;
	return 0;
}

LOCAL INT limits( void )
{
	T_FSTAT	st, st2;
	TS_UUID	boot;
	INT	i, n;
	BOOL	fboot = FALSE;

	for ( i = 0; i < FS_PRC_FILE; i++ ) {
		fds[i] = fs_open(PROG, O_RDONLY);
		if ( fds[i] < 0 ) return 500 + i;
	}
	if ( fs_open(PROG, O_RDONLY) != EX_MFILE ) return 520;

	/* each is an alias of a file on the volume of a device object */
	n = fs_mounts(mnt, FS_MAX_MOUNT);
	for ( i = 0; i < n && i < FS_MAX_MOUNT; i++ ) {
		if ( same(mnt[i].path, "/boot") ) {
			boot = mnt[i].obj;
			fboot = TRUE;
		}
	}
	if ( !fboot ) return 521;
	if ( fs_fstat(fds[0], &st) != EX_OK || !same_uuid(&st.dev, &boot) ) return 522;
	if ( fs_stat(PROG, &st2) != EX_OK || !same_uuid(&st2.dev, &boot) ) return 523;
	for ( i = 0; i < 16 && st.dev.b[i] == 0; i++ ) ;
	if ( i == 16 ) return 524;

	if ( ts_rcv_msg(&msg, 10000) != E_OK ) return 525;
	for ( i = 0; i < FS_PRC_FILE; i++ ) {
		if ( fs_close(fds[i]) != EX_OK ) return 530;
	}
	return 0;
}

LOCAL INT obptr( void )
{
	T_OBREF	r;
	T_OBCRE	c;
	T_OBNTF	req;
	TS_UUID	clk, ch, u;
	SZ	asz = 0;
	INT	cnt = 0, i;
	ID	kc, port;
	BOOL	fclk = FALSE;
	void	*ro = (void *)same;		/* the program's text: read only */

	if ( ob_lst_obj(OB_T_DEVICE, 0, NULL, list, LIST_MAX, &cnt) != E_OK ) return 600;
	for ( i = 0; i < cnt && i < LIST_MAX; i++ ) {
		if ( ob_ref_obj(&list[i], &r) == E_OK && r.sub == OB_S_CLOCK ) {
			clk = list[i];
			fclk = TRUE;
		}
	}
	if ( !fclk ) return 601;
	kc = ob_opn_obj(&clk, OB_OP_R);
	if ( kc <= 0 ) return 602;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) != E_OK ) return 603;
	port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	if ( port <= 0 ) return 604;

	/* what the kernel would read */
	if ( ob_opn_obj(KERNEL_PTR, OB_OP_R) != E_MACV ) return 610;
	if ( ob_ref_obj(KERNEL_PTR, &r) != E_MACV ) return 611;
	if ( ob_fnd_nam(KERNEL_PTR, &u) != E_MACV ) return 612;
	if ( ob_cre_obj(KERNEL_PTR, &u) != E_MACV ) return 613;
	c.name = KERNEL_PTR;
	if ( ob_cre_obj(&c, &u) != E_MACV ) return 614;
	c.name = NULL;
	c.json = KERNEL_PTR;
	c.jsonsz = 16;
	if ( ob_cre_obj(&c, &u) != E_MACV ) return 615;
	c.json = NULL;
	c.jsonsz = 0;
	c.prt = KERNEL_PTR;
	if ( ob_cre_obj(&c, &u) != E_MACV ) return 616;
	c.prt = NULL;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_CHANGE;
	if ( ob_ntf_evt(kc, OB_REC_ANY, KERNEL_PTR, port) != E_MACV ) return 617;
	if ( ob_set_atr(kc, KERNEL_PTR, 16) != E_MACV ) return 618;
	if ( ob_att_vol(KERNEL_PTR, 0) != E_MACV ) return 619;
	if ( ob_login(KERNEL_PTR, (CONST UB *)"x") != E_MACV ) return 620;
	if ( ob_login(&clk, KERNEL_PTR) != E_MACV ) return 621;
	if ( ob_wri_rec(port, 0, 0, KERNEL_PTR, 16, NULL) != E_MACV ) return 622;

	/* where the kernel would write: not the process's, or read only */
	if ( ob_ref_obj(&clk, KERNEL_PTR) != E_MACV ) return 630;
	if ( ob_ref_obj(&clk, ro) != E_MACV ) return 631;
	if ( ob_lst_obj(OB_T_DEVICE, 0, NULL, KERNEL_PTR, 4, &cnt) != E_MACV ) return 632;
	if ( ob_lst_obj(OB_T_DEVICE, 0, NULL, list, 4, ro) != E_MACV ) return 633;
	if ( ob_rea_rec(kc, 1, 0, KERNEL_PTR, 16, NULL) != E_MACV ) return 634;
	if ( ob_rea_rec(kc, 1, 0, atr, 16, KERNEL_PTR) != E_MACV ) return 635;
	if ( ob_get_atr(kc, ro, 64, &asz) != E_MACV ) return 636;
	if ( ob_get_crd(KERNEL_PTR) != E_MACV ) return 637;
	if ( ob_get_prt(&clk, ro) != E_MACV ) return 638;
	if ( ob_map_rec(kc, 0, 0, KERNEL_PTR) != E_MACV ) return 639;
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, KERNEL_PTR) != E_MACV ) return 640;

	/* and the calls still work with pointers that are the process's */
	if ( ob_ref_obj(&clk, &r) != E_OK || r.sub != OB_S_CLOCK ) return 650;
	if ( ob_rea_rec(kc, 1, 0, atr, 16, &asz) != E_OK || asz <= 0 ) return 651;
	if ( ob_ntf_evt(kc, OB_REC_ANY, &req, port) <= 0 ) return 652;

	ob_cls_obj(kc);
	ob_cls_obj(port);
	if ( ob_del_obj(&ch) != E_OK ) return 653;
	return 0;
}

/* ---------------------------------------------------------------- large transfers */

LOCAL UB pattern( INT i, INT seed )
{
	return (UB)( i * 7 + ( i >> 9 ) + seed );
}

/* Moves n bytes in the pieces 'cut' gives (0 ends them), or in one */
LOCAL INT io_pieces( INT fd, UB *p, INT n, CONST INT *cut, BOOL wr )
{
	INT	done = 0, want, got;

	while ( done < n ) {
		want = ( cut != NULL && *cut > 0 ) ? *cut++ : n - done;
		if ( want > n - done ) want = n - done;
		got = wr ? fs_write(fd, p + done, want) : fs_read(fd, p + done, want);
		if ( got != want ) return -1;
		done += got;
	}
	return done;
}

LOCAL INT bigio( void )
{
	static CONST INT wcut[] = { 700, 65536, 3, 4096, 0 };
	static CONST INT rcut[] = { 1000, 70001, 512, 1, 0 };
	char	dir[32], f[48];
	UB	*src, *dst;
	T_FSTAT	st;
	INT	k, i, fd;

	memcpy(dir, &arg[1], 28);
	dir[28] = '\0';
	cat(f, dir, "/BIGIO.BIN");

	for ( k = 0; k < 2; k++ ) {
		/* k 0: both on a page; k 1: neither on a sector */
		src = wbuf + ( ( k == 0 ) ? 0 : 1 );
		dst = rbuf + ( ( k == 0 ) ? 0 : 3 );
		for ( i = 0; i < BIG_SZ; i++ ) {
			src[i] = pattern(i, k);
		}
		fd = fs_open(f, O_WRONLY | O_CREAT | O_TRUNC);
		if ( fd < 0 ) return 800 + k * 20;
		if ( io_pieces(fd, src, BIG_SZ, ( k == 0 ) ? NULL : wcut, TRUE) != BIG_SZ ) return 801 + k * 20;
		if ( fs_close(fd) != EX_OK ) return 802 + k * 20;
		if ( fs_stat(f, &st) != EX_OK || st.size != BIG_SZ ) return 803 + k * 20;

		fd = fs_open(f, O_RDONLY);
		if ( fd < 0 ) return 804 + k * 20;
		memset(dst, 0, BIG_SZ);
		if ( io_pieces(fd, dst, BIG_SZ, ( k == 0 ) ? NULL : rcut, FALSE) != BIG_SZ ) return 805 + k * 20;
		if ( fs_read(fd, dst, 16) != 0 ) return 806 + k * 20;
		for ( i = 0; i < BIG_SZ; i++ ) {
			if ( dst[i] != pattern(i, k) ) return 807 + k * 20;
		}

		/* from the middle: a sector boundary and not, into an odd place */
		memset(dst, 0, 20000);
		if ( fs_lseek(fd, 3 * 512, SEEK_SET_) != 3 * 512 ) return 808 + k * 20;
		if ( fs_read(fd, dst + 5, 16384) != 16384 ) return 809 + k * 20;
		for ( i = 0; i < 16384; i++ ) {
			if ( dst[5 + i] != pattern(3 * 512 + i, k) ) return 810 + k * 20;
		}
		if ( fs_lseek(fd, 12345, SEEK_SET_) != 12345 ) return 811 + k * 20;
		if ( fs_read(fd, dst, 9000) != 9000 ) return 812 + k * 20;
		for ( i = 0; i < 9000; i++ ) {
			if ( dst[i] != pattern(12345 + i, k) ) return 813 + k * 20;
		}
		if ( fs_close(fd) != EX_OK ) return 814 + k * 20;
	}
	if ( fs_unlink(f) != EX_OK ) return 850;
	return 0;
}

/* ---------------------------------------------------------------- ended in the middle */

/* The directory, or the store's path, the test hands over in arg[1..7] */
LOCAL void arg_path( char *d )
{
	memcpy(d, &arg[1], 28);
	d[28] = '\0';
}

LOCAL INT killio( void )
{
	char	dir[32], f[48], g[48];
	INT	fd, k;

	arg_path(dir);
	cat(f, dir, "/KILLIO.BIN");
	cat(g, dir, "/KILLIO.OLD");
	for ( k = 0; k < KILL_SZ; k++ ) {
		kbuf[k] = pattern(k, 9);
	}
	fd = fs_open(f, O_RDWR | O_CREAT | O_TRUNC);	/* ready */
	if ( fd < 0 ) return 1000;
	for ( k = 0; ; k++ ) {
		if ( fs_write(fd, kbuf, KILL_SZ) != KILL_SZ ) return 1001;
		if ( fs_write(fd, kbuf + 1, 777) != 777 ) return 1002;
		switch ( k % 4 ) {
		  case 1:			/* cut back into a cluster */
			if ( fs_ftruncate(fd, 5000) != EX_OK ) return 1003;
			if ( fs_lseek(fd, 0, SEEK_END_) != 5000 ) return 1004;
			break;
		  case 3:			/* away under another name, and made anew */
			if ( fs_close(fd) != EX_OK ) return 1005;
			if ( fs_rename(f, g) != EX_OK ) return 1006;
			fd = fs_open(f, O_RDWR | O_CREAT | O_TRUNC);
			if ( fd < 0 ) return 1007;
			if ( fs_unlink(g) != EX_OK ) return 1008;
			break;
		  default:
			break;
		}
	}
}

/* A new object on the store at the path in arg[1..7], and a record of it opened */
LOCAL INT ob_new( TS_UUID *u, ID *p_key, INT *p_rec )
{
	char	vol[32];
	T_OBCRE	c;

	arg_path(vol);
	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_FILE;
	c.vol = vol;
	if ( ob_cre_obj(&c, u) != E_OK ) return 1;
	*p_key = ob_opn_obj(u, OB_OP_R | OB_OP_W);
	if ( *p_key <= 0 ) return 2;
	if ( ob_apd_rec(*p_key, OB_RT_SYSDATA, 0, p_rec) != E_OK ) return 3;
	return 0;
}

LOCAL INT obbig( void )
{
	TS_UUID	u;
	UB	*src, *dst;
	SZ	asz;
	INT	k, i, rec = 0;
	ID	key = 0;

	if ( ob_new(&u, &key, &rec) != 0 ) return 1200;
	for ( k = 0; k < 2; k++ ) {
		/* k 0: both on a page; k 1: neither on a sector */
		src = wbuf + ( ( k == 0 ) ? 0 : 1 );
		dst = rbuf + ( ( k == 0 ) ? 0 : 3 );
		for ( i = 0; i < BIG_SZ; i++ ) {
			src[i] = pattern(i, k + 4);
		}
		asz = 0;
		if ( ob_wri_rec(key, rec, 0, src, BIG_SZ, &asz) != E_OK || asz != BIG_SZ ) return 1201 + k * 10;
		memset(dst, 0, BIG_SZ);
		asz = 0;
		if ( ob_rea_rec(key, rec, 0, dst, BIG_SZ, &asz) != E_OK || asz != BIG_SZ ) return 1202 + k * 10;
		for ( i = 0; i < BIG_SZ; i++ ) {
			if ( dst[i] != pattern(i, k + 4) ) return 1203 + k * 10;
		}
	}
	ob_cls_obj(key);
	if ( ob_del_obj(&u) != E_OK ) return 1250;
	return 0;
}

LOCAL INT killob( void )
{
	TS_UUID	u;
	SZ	asz;
	INT	k, rec = 0;
	ID	key = 0;

	for ( k = 0; k < BIG_SZ; k++ ) {
		wbuf[k] = pattern(k, 7);
	}
	if ( ob_new(&u, &key, &rec) != 0 ) return 1300;
	if ( fs_open(PROG, O_RDONLY) < 0 ) return 1301;		/* ready */
	for ( k = 0; ; k++ ) {
		asz = 0;
		if ( ob_wri_rec(key, rec, ( k % 3 ) * 1000, wbuf, BIG_SZ, &asz) != E_OK ) return 1302;
		if ( k % 5 == 4 && ob_trn_rec(key, rec, 3000) != E_OK ) return 1303;
		if ( ob_rea_rec(key, rec, 0, rbuf, BIG_SZ, &asz) != E_OK ) return 1304;
	}
}

/* ---------------------------------------------------------------- watching the devices */

LOCAL INT watch( void )
{
	CONST TS_UUID	devlist = OB_UUID_DEVLIST_INIT;
	T_OBREF		r;
	T_OBCRE		c;
	T_OBNTF		req;
	TS_UUID		ch, gone[8];
	SZ		asz = 0;
	INT		ngone = 0, t, i, fd;
	ID		key, port;

	if ( ob_ref_obj(&devlist, &r) != E_OK || r.sub != OB_S_DEVLIST ) return 900;
	key = ob_opn_obj(&devlist, OB_OP_R);
	if ( key <= 0 ) return 901;
	if ( ob_rea_rec(key, 0, 0, atr, sizeof(atr), &asz) != E_OK || !has(atr, asz, "<link id=") ) return 902;
	if ( ob_wri_rec(key, 0, 0, atr, 4, &asz) != E_OACV ) return 903;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) != E_OK ) return 904;
	port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	if ( port <= 0 ) return 905;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_ATTACH | OB_E_DETACH | OB_E_CHANGE;
	if ( ob_ntf_evt(key, OB_REC_ANY, &req, port) <= 0 ) return 906;

	fd = fs_open(PROG, O_RDONLY);		/* ready */
	if ( fd < 0 ) return 907;

	for ( t = 0; t < 400; t++ ) {		/* 20 seconds */
		while ( ob_rea_rec(port, 0, 0, &ntm, sizeof(ntm), &asz) == E_OK && asz == (SZ)sizeof(ntm) ) {
			if ( ntm.event == OB_E_DETACH && ngone < 8 ) {
				gone[ngone++] = ntm.uuid;
			}
			if ( ntm.event != OB_E_ATTACH ) continue;
			for ( i = 0; i < ngone; i++ ) {
				if ( same_uuid(&gone[i], &ntm.uuid) ) break;
			}
			if ( i < ngone && ob_ref_obj(&ntm.uuid, &r) == E_OK && r.sub == OB_S_DISK ) {
				fs_close(fd);
				ob_cls_obj(port);
				ob_cls_obj(key);
				if ( ob_del_obj(&ch) != E_OK ) return 908;
				return 0;
			}
		}
		tk_dly_tsk(50);
	}
	return ( ngone == 0 ) ? 910 : 911;
}

/* ---------------------------------------------------------------- tasks side by side */

#define PAR_N		4
#define PAR_BUSY_NS	300000000ULL	/* how long each keeps its processor */

LOCAL volatile UD	par_from[PAR_N], par_to[PAR_N];

/* Busy, without waiting or calling anything that waits, then says so */
LOCAL void par_task( INT stacd, void *exinf )
{
	INT	me = stacd & 0xFF;
	ID	sem = (ID)( stacd >> 8 );
	UD	t0 = 0, t = 0;

	(void)ts_get_mono(&t0);
	par_from[me] = t0;
	do {
		(void)ts_get_mono(&t);
	} while ( t - t0 < PAR_BUSY_NS );
	par_to[me] = t;
	(void)tk_sig_sem(sem, 1);
	tk_exd_tsk();
}

/*
 * Four tasks of the same priority. On one processor they cannot overlap:
 * a task is not preempted by another of its own priority, so each runs
 * to its end before the next starts. On several they run side by side,
 * and the moment the last of them started, the others still run.
 */
LOCAL INT parallel( void )
{
	T_CSEM	cs;
	T_CTSK	c;
	T_RTSK	rt;
	ID	sem, t;
	UD	last = 0;
	INT	i, n = 0;

	if ( tk_ref_tsk(TSK_SELF, &rt) != E_OK ) return 1;
	memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = PAR_N;
	sem = tk_cre_sem(&cs);
	if ( sem <= 0 ) return 2;
	for ( i = 0; i < PAR_N; i++ ) {
		memset(&c, 0, sizeof(c));
		c.tskatr = TA_HLNG | TA_RNG3;
		c.task = (FP)par_task;
		c.itskpri = rt.tskbpri;
		c.stksz = 16 * 1024;
		t = tk_cre_tsk(&c);
		if ( t <= 0 ) return 3;
		if ( tk_sta_tsk(t, (INT)( ( sem << 8 ) | i )) != E_OK ) return 4;
	}
	for ( i = 0; i < PAR_N; i++ ) {
		if ( tk_wai_sem(sem, 1, 10000) != E_OK ) return 5;
	}
	for ( i = 0; i < PAR_N; i++ ) {
		if ( par_from[i] > last ) last = par_from[i];
	}
	for ( i = 0; i < PAR_N; i++ ) {
		if ( par_from[i] <= last && par_to[i] > last ) n++;
	}
	return 300 + n;
}

/* ---------------------------------------------------------------- objects of the core */

#define TK_SEM_ROOM	TS_PRC_SEM_MAX
#define TK_TSK_ROOM	TS_PRC_TSK_MAX

LOCAL volatile ER	sub_er[4];
LOCAL ID		main_tid;

/* A task of the process's own: signals the semaphore it was started with, and goes */
LOCAL void sub_signal( INT stacd, void *exinf )
{
	sub_er[0] = tk_sig_sem((ID)stacd, 1);
	tk_exd_tsk();
}

/* One that waits on its semaphore until it is ended */
LOCAL void sub_wait( INT stacd, void *exinf )
{
	(void)tk_wai_sem((ID)stacd, 1, TMO_FEVR);
	tk_exd_tsk();
}

/* One that tries to end the main task, and keeps what it was told */
LOCAL void sub_main( INT stacd, void *exinf )
{
	sub_er[1] = tk_ter_tsk(main_tid);
	sub_er[2] = tk_del_tsk(main_tid);
	sub_er[3] = tk_sta_tsk(main_tid, 0);
	(void)tk_sig_sem((ID)stacd, 1);
	tk_exd_tsk();
}

/* Asked at protection level 0: it runs at level 3 in this space all the same */
LOCAL ID new_task( void (*entry)( INT, void * ), PRI pri )
{
	T_CTSK	c;

	memset(&c, 0, sizeof(c));
	c.tskatr = TA_HLNG | TA_RNG0;
	c.task = (FP)entry;
	c.itskpri = pri;
	c.stksz = 16 * 1024;
	return tk_cre_tsk(&c);
}

LOCAL BOOL tsk_gone( ID tid )
{
	T_RTSK	r;
	INT	i;

	for ( i = 0; i < 200; i++ ) {
		if ( tk_ref_tsk(tid, &r) == E_NOEXS ) return TRUE;
		tk_dly_tsk(1);
	}
	return FALSE;
}

LOCAL BOOL tsk_waits( ID tid )
{
	T_RTSK	r;
	INT	i;

	for ( i = 0; i < 200; i++ ) {
		if ( tk_ref_tsk(tid, &r) == E_OK && r.tskstat == TTS_WAI ) return TRUE;
		tk_dly_tsk(1);
	}
	return FALSE;
}

LOCAL INT tkown( void )
{
	T_CSEM	cs;
	T_CFLG	cf;
	T_CMTX	cm;
	T_CMBF	cb;
	T_CMBX	cx;
	T_CCYC	cc;
	T_RSEM	rs;
	T_RTSK	rt;
	T_RMTX	rm;
	UINT	ptn = 0;
	UB	mb[64];
	INT	n;
	PRI	pri;
	ID	s, s2, f, m, b, t, w;
	ER	er;

	main_tid = tk_get_tid();
	if ( tk_ref_tsk(TSK_SELF, &rt) != E_OK ) return 1400;
	pri = rt.tskbpri;
	if ( tk_ref_tsk(TSK_SELF, KERNEL_PTR) != E_MACV ) return 1401;

	/* a semaphore */
	memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.isemcnt = 0;
	cs.maxsem = 4;
	s = tk_cre_sem(&cs);
	if ( s <= 0 ) return 1402;
	if ( tk_cre_sem(KERNEL_PTR) != E_MACV ) return 1403;
	if ( tk_sig_sem(s, 1) != E_OK || tk_wai_sem(s, 1, TMO_POL) != E_OK ) return 1404;
	if ( tk_wai_sem(s, 1, TMO_POL) != E_TMOUT ) return 1405;
	if ( tk_ref_sem(s, &rs) != E_OK || rs.semcnt != 0 ) return 1406;
	if ( tk_ref_sem(s, KERNEL_PTR) != E_MACV ) return 1407;

	/* an event flag */
	memset(&cf, 0, sizeof(cf));
	cf.flgatr = TA_TFIFO | TA_WMUL;
	f = tk_cre_flg(&cf);
	if ( f <= 0 ) return 1410;
	if ( tk_set_flg(f, 5) != E_OK || tk_wai_flg(f, 1, TWF_ORW, &ptn, TMO_POL) != E_OK
	  || ptn != 5 ) return 1411;
	if ( tk_wai_flg(f, 1, TWF_ORW, KERNEL_PTR, TMO_POL) != E_MACV ) return 1412;
	if ( tk_clr_flg(f, 0) != E_OK || tk_wai_flg(f, 1, TWF_ORW, &ptn, TMO_POL) != E_TMOUT ) return 1413;

	/* a mutex; a ceiling above the process is not to be had */
	memset(&cm, 0, sizeof(cm));
	cm.mtxatr = TA_INHERIT;
	m = tk_cre_mtx(&cm);
	if ( m <= 0 ) return 1420;
	if ( tk_loc_mtx(m, TMO_POL) != E_OK || tk_unl_mtx(m) != E_OK ) return 1421;
	if ( tk_ref_mtx(m, &rm) != E_OK || rm.htsk != 0 ) return 1422;
	cm.mtxatr = TA_CEILING;
	cm.ceilpri = 1;
	if ( pri > 1 && tk_cre_mtx(&cm) != E_PAR ) return 1423;

	/* a message buffer, the kernel's memory and no other */
	memset(&cb, 0, sizeof(cb));
	cb.mbfatr = TA_TFIFO;
	cb.bufsz = 256;
	cb.maxmsz = 64;
	b = tk_cre_mbf(&cb);
	if ( b <= 0 ) return 1430;
	if ( tk_snd_mbf(b, "hello", 5, TMO_POL) != E_OK ) return 1431;
	if ( tk_rcv_mbf(b, mb, TMO_POL) != 5 || mb[0] != 'h' || mb[4] != 'o' ) return 1432;
	if ( tk_snd_mbf(b, KERNEL_PTR, 5, TMO_POL) != E_MACV ) return 1433;
	cb.mbfatr = TA_TFIFO | TA_USERBUF;
	cb.bufptr = mb;
	if ( tk_cre_mbf(&cb) != E_RSATR ) return 1434;

	/* what a process has none of */
	memset(&cx, 0, sizeof(cx));
	memset(&cc, 0, sizeof(cc));
	if ( tk_cre_mbx(&cx) != E_NOSPT ) return 1440;
	if ( tk_cre_cyc(&cc) != E_NOSPT ) return 1441;
	if ( tk_dis_dsp() != E_NOSPT ) return 1442;

	/* a task of its own runs in this space and goes by itself */
	t = new_task(sub_signal, pri);
	if ( t <= 0 ) return 1450;
	if ( tk_sta_tsk(t, s) != E_OK || tk_wai_sem(s, 1, 2000) != E_OK || sub_er[0] != E_OK ) return 1451;
	if ( !tsk_gone(t) ) return 1452;
	if ( pri > 1 && new_task(sub_signal, pri - 1) != E_PAR ) return 1453;

	/* the main task is not another task's to end */
	t = new_task(sub_main, pri);
	if ( t <= 0 || tk_sta_tsk(t, s) != E_OK || tk_wai_sem(s, 1, 2000) != E_OK ) return 1454;
	if ( sub_er[1] != E_OACV || sub_er[2] != E_OACV || sub_er[3] != E_OACV ) return 1455;
	if ( !tsk_gone(t) ) return 1456;

	/* a task of its own that waits may be ended, held and let go */
	s2 = tk_cre_sem(&cs);
	w = new_task(sub_wait, pri);
	if ( s2 <= 0 || w <= 0 || tk_sta_tsk(w, s2) != E_OK || !tsk_waits(w) ) return 1460;
	if ( tk_ter_tsk(w) != E_OK || tk_del_tsk(w) != E_OK ) return 1461;
	w = new_task(sub_wait, pri);
	if ( w <= 0 || tk_sta_tsk(w, s2) != E_OK || !tsk_waits(w) ) return 1462;
	if ( tk_sus_tsk(w) != E_OK || tk_ref_tsk(w, &rt) != E_OK || rt.tskstat != TTS_WAS ) return 1463;
	if ( tk_rsm_tsk(w) != E_OK || tk_ref_tsk(w, &rt) != E_OK || rt.tskstat != TTS_WAI ) return 1464;

	/* room for so many of each; w stays, waiting */
	for ( n = 0, er = E_OK; n < TK_SEM_ROOM + 8; n++ ) {
		er = tk_cre_sem(&cs);
		if ( er <= 0 ) break;
	}
	if ( er != E_LIMIT || n + 2 != TK_SEM_ROOM ) return 1470;
	for ( n = 0, er = E_OK; n < TK_TSK_ROOM + 8; n++ ) {
		er = new_task(sub_wait, pri);
		if ( er <= 0 ) break;
	}
	if ( er != E_LIMIT || n + 2 != TK_TSK_ROOM ) return 1471;

	/* ended as the test says, holding all of it and the mutex */
	if ( tk_loc_mtx(m, TMO_POL) != E_OK ) return 1480;
	if ( ts_rcv_msg(&msg, 20000) != E_OK ) return 1481;
	if ( msg.body[0] == 1 ) {
		*(volatile UINT *)0 = 1;		/* a fault ends it here */
	}
	if ( msg.body[0] == 2 ) {
		(void)ts_rcv_msg(&msg, 20000);		/* the test ends it meanwhile */
		return 1482;
	}
	return 0;
}

/* The files written by a task of the process's own; a step that went wrong ends the process */
LOCAL void sub_killio( INT stacd, void *exinf )
{
	ts_ext_prc(killio());
}

LOCAL INT killsub( void )
{
	T_RTSK	rt;
	ID	t;

	if ( tk_ref_tsk(TSK_SELF, &rt) != E_OK ) return 1700;
	t = new_task(sub_killio, rt.tskbpri);
	if ( t <= 0 || tk_sta_tsk(t, 0) != E_OK ) return 1701;
	(void)tk_slp_tsk(TMO_FEVR);
	return 1702;
}

/*
 * A page the kernel is to write that goes while it waits: the main task
 * waits for a message into a record of memory mapped into the process,
 * made sure of when the call began; another task takes the record off
 * the process and sends the message. The kernel meets the address
 * gone as it hands the message over.
 */
LOCAL ID	kf_key;
LOCAL INT	kf_rec;
LOCAL T_TSMSG	kf_msg;

LOCAL void kf_unmap( INT stacd, void *exinf )
{
	(void)stacd;
	(void)exinf;
	tk_dly_tsk(200);
	(void)ob_unm_rec(kf_key, kf_rec, 0);
	memset(&kf_msg, 0, sizeof(kf_msg));
	kf_msg.type = 1;
	kf_msg.size = 1;
	(void)ts_snd_msg(ts_get_pid(), &kf_msg, 1000);
	tk_exd_tsk();
}

LOCAL INT kfault( void )
{
	T_OBCRE	c;
	T_OBMAP	m;
	T_RTSK	rt;
	TS_UUID	u;
	INT	rec = -1;
	ID	t;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_STORAGE;
	c.sub = OB_S_MEMORY;
	c.flags = OB_F_GLOBAL;			/* the test takes it away by its name */
	c.name = (CONST UB *)"ktkfault";
	if ( ob_cre_obj(&c, &u) != E_OK ) return 1101;
	kf_key = ob_opn_obj(&u, OB_OP_R | OB_OP_W);
	if ( kf_key <= 0 ) return 1102;
	if ( ob_apd_rec(kf_key, OB_RT_SYSDATA, 0, &rec) != E_OK ) return 1103;
	memset(&m, 0, sizeof(m));
	m.flags = OB_M_WRITE;
	m.size = sizeof(T_TSMSG);
	kf_rec = rec;
	if ( ob_map_rec(kf_key, rec, 0, &m) != E_OK || m.addr == NULL ) return 1104;
	if ( tk_ref_tsk(TSK_SELF, &rt) != E_OK ) return 1105;
	t = new_task(kf_unmap, rt.tskbpri);
	if ( t <= 0 || tk_sta_tsk(t, 0) != E_OK ) return 1106;
	(void)ts_rcv_msg((T_TSMSG *)m.addr, 5000);	/* the process ends as it returns */
	return 1107;
}

LOCAL INT tkhold( void )
{
	T_CSEM	cs;
	T_RSEM	rs;
	INT	got = 0;
	ID	s;

	memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.maxsem = 4;
	s = tk_cre_sem(&cs);
	if ( s <= 0 ) return 1500;
	for ( ;; ) {
		if ( ts_rcv_msg(&msg, 30000) != E_OK ) return 1501;
		if ( msg.type == 2 ) break;
		got++;
	}
	if ( tk_ref_sem(s, &rs) != E_OK || rs.semcnt != 0 ) return 1502;
	return ( got == 1 ) ? 0 : 1510 + got;
}

LOCAL INT tkatk( void )
{
	CONST TS_UUID	clock = OB_UUID_CLOCK_INIT;
	TS_UUID		u;
	T_RTSK		rt;
	T_RSEM		rs;
	T_RPRC		rp;
	T_PSTS		ps;
	UINT		id[5];
	INT		k;
	ID		key, k2, s;

	memcpy(&u, &arg[1], sizeof(u));
	if ( ts_rcv_msg(&msg, 20000) != E_OK || msg.size < (SZ)sizeof(id) ) return 1600;
	memcpy(id, msg.body, sizeof(id));

	/* the other process's main task */
	if ( tk_ter_tsk(id[1]) != E_ID ) return 1601;
	if ( tk_del_tsk(id[1]) != E_ID ) return 1602;
	if ( tk_sus_tsk(id[1]) != E_ID ) return 1603;
	if ( tk_chg_pri(id[1], TPRI_INI) != E_ID ) return 1604;
	if ( tk_rel_wai(id[1]) != E_ID ) return 1605;
	if ( tk_wup_tsk(id[1]) != E_ID ) return 1606;
	if ( tk_ref_tsk(id[1], &rt) != E_ID ) return 1607;
	if ( tk_rsm_tsk(id[1]) != E_ID ) return 1608;
	if ( tk_sta_tsk(id[1], 0) != E_ID ) return 1609;

	/* its semaphore, and one of the kernel's */
	for ( k = 0; k < 2; k++ ) {
		s = (ID)id[2 + k];
		if ( tk_sig_sem(s, 1) != E_ID ) return 1610 + k * 10;
		if ( tk_del_sem(s) != E_ID ) return 1611 + k * 10;
		if ( tk_wai_sem(s, 1, TMO_POL) != E_ID ) return 1612 + k * 10;
		if ( tk_ref_sem(s, &rs) != E_ID ) return 1613 + k * 10;
	}

	/* an administrator by its parent: it may end a process, look at one, send to one */
	if ( ts_ref_prc(id[0], &rp) != E_OK || rp.maintsk != (ID)id[1] ) return 1630;
	memset(&msg, 0, sizeof(msg));
	msg.type = 1;
	if ( ts_snd_msg(id[0], &msg, TMO_POL) != E_OK ) return 1631;
	if ( ts_ter_prc(id[4], -9) != E_OK ) return 1632;
	if ( ts_wai_prc(id[0], &ps, TMO_POL) != E_OACV ) return 1633;	/* not its child */

	/* a user who is not an administrator */
	if ( ob_login(&u, (CONST UB *)"ktest-pw") != E_OK ) return 1640;
	if ( ts_ter_prc(id[0], -9) != E_OACV ) return 1641;
	if ( ts_snd_msg(id[0], &msg, TMO_POL) != E_OACV ) return 1642;
	if ( ts_ref_prc(id[0], &rp) != E_OK ) return 1643;		/* anyone may look */
	if ( ts_wai_prc(id[0], &ps, TMO_POL) != E_OACV ) return 1644;
	key = ob_opn_obj(&clock, OB_OP_READ);
	if ( key <= 0 ) return 1645;
	if ( ob_dup_key(key, OB_OP_READ, id[0]) != E_OACV ) return 1646;
	k2 = ob_dup_key(key, OB_OP_READ, ts_get_pid());
	if ( k2 <= 0 ) return 1647;
	(void)ob_cls_obj(k2);
	(void)ob_cls_obj(key);
	if ( tk_ter_tsk(id[1]) != E_ID ) return 1648;
	return 0;
}

LOCAL T_OBDROP	drop;

/* The next drop on the window, read from its record; FALSE none within 10 s */
LOCAL BOOL next_drop( ID port, ID kw )
{
	SZ	asz = 0;
	INT	t;

	for ( t = 0; t < 200; t++ ) {
		while ( ob_rea_rec(port, 0, 0, &ntm, sizeof(ntm), &asz) == E_OK && asz == (SZ)sizeof(ntm) ) {
			if ( ntm.event != OB_E_DROP ) continue;
			memset(&drop, 0, sizeof(drop));
			return (BOOL)( ob_rea_rec(kw, OB_WR_DROP, 0, &drop, sizeof(drop), &asz) == E_OK
				       && drop.n == 1 );
		}
		tk_dly_tsk(50);
	}
	return FALSE;
}

LOCAL ER drop_reply( ID kw, UINT answer )
{
	T_OBDRANS	a;
	SZ		asz = 0;

	memset(&a, 0, sizeof(a));
	a.seq = drop.seq;
	a.answer = answer;
	return ob_wri_rec(kw, OB_WR_DROP, 0, &a, sizeof(a), &asz);
}

LOCAL INT dropped( void )
{
	CONST char	*j = "{\"rect\":[40,40,240,160],\"attr\":7}";
	T_OBCRE		c;
	T_OBNTF		req;
	T_OBPRT		p;
	TS_UUID		me, win, ch, t;
	UB		b[8];
	SZ		asz = 0;
	ID		kw, port, k;

	memcpy(&me, &arg[1], sizeof(me));
	if ( ob_login(&me, (CONST UB *)"ktest-pw") != E_OK ) return 1800;

	/* a window of its own that takes drops */
	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"ktdrop";
	c.json = (CONST UB *)j;
	for ( c.jsonsz = 0; j[c.jsonsz] != '\0'; c.jsonsz++ ) ;
	if ( ob_cre_obj(&c, &win) != E_OK ) return 1801;
	kw = ob_opn_obj(&win, OB_OP_R | OB_OP_W);
	if ( kw <= 0 ) return 1802;
	memset(&c, 0, sizeof(c));
	c.type = OB_T_CHANNEL;
	if ( ob_cre_obj(&c, &ch) != E_OK ) return 1803;
	port = ob_opn_obj(&ch, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
	if ( port <= 0 ) return 1804;
	memset(&req, 0, sizeof(req));
	req.events = OB_E_DROP;
	if ( ob_ntf_evt(kw, OB_REC_ANY, &req, port) <= 0 ) return 1805;

	/* the first drop: what the one who dropped it may do, until refused */
	if ( !next_drop(port, kw) ) return 1806;
	t = drop.v[0].target;
	if ( ( drop.v[0].ops & ( OB_OP_R | OB_OP_WRITE ) ) != ( OB_OP_R | OB_OP_WRITE ) ) return 1807;
	if ( ( drop.v[0].ops & ( OB_OP_DELETE | OB_OP_PROT | OB_OP_EXEC ) ) != 0 ) return 1808;
	k = ob_opn_obj(&t, OB_OP_R | OB_OP_WRITE);
	if ( k <= 0 ) return 1809;
	(void)ob_cls_obj(k);
	if ( drop_reply(kw, OB_DR_REFUSE) != E_OK ) return 1810;
	if ( ob_opn_obj(&t, OB_OP_R) != E_OACV ) return 1811;	/* its own rights give nothing */

	/* the second: taken, read and written, but not deleted nor its protection changed */
	if ( !next_drop(port, kw) || !same_uuid(&drop.v[0].target, &t) ) return 1812;
	k = ob_opn_obj(&t, OB_OP_R | OB_OP_WRITE);
	if ( k <= 0 ) return 1813;
	if ( ob_rea_rec(k, 0, 0, b, 6, &asz) != E_OK || asz != 6 || !same(b, "secret") ) return 1814;
	if ( ob_opn_obj(&t, OB_OP_DELETE) != E_OACV ) return 1815;
	if ( ob_opn_obj(&t, OB_OP_PROT) != E_OACV ) return 1816;
	if ( ob_del_obj(&t) != E_OACV ) return 1817;
	if ( ob_get_prt(&t, &p) != E_OK ) return 1818;
	p.mode = 0666;
	if ( ob_set_prt(&t, &p) != E_OACV ) return 1819;
	if ( drop_reply(kw, OB_DR_ACCEPT) != E_OK ) return 1820;
	if ( ob_wri_rec(k, 0, 0, "opened", 6, &asz) != E_OK ) return 1821;	/* the test waits for this */
	(void)ob_cls_obj(k);

	/* taken, it stays granted as long as the process runs */
	if ( ts_rcv_msg(&msg, 20000) != E_OK ) return 1822;
	k = ob_opn_obj(&t, OB_OP_R);
	if ( k <= 0 ) return 1823;
	(void)ob_cls_obj(k);
	(void)ob_cls_obj(kw);
	if ( ob_del_obj(&win) != E_OK ) return 1824;
	(void)ob_cls_obj(port);
	(void)ob_del_obj(&ch);
	return 0;
}

/* A window of the process's named "ktkill", its key, and its drawing environment */
LOCAL INT kill_window( ID *p_kw )
{
	CONST char	*j = "{\"rect\":[300,200,560,380],\"attr\":7}";
	T_OBCRE		c;
	TS_UUID		win;
	ID		kw;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"ktkill";
	c.json = (CONST UB *)j;
	for ( c.jsonsz = 0; j[c.jsonsz] != '\0'; c.jsonsz++ ) ;
	if ( ob_cre_obj(&c, &win) != E_OK ) return -1;
	kw = ob_opn_obj(&win, OB_OP_R | OB_OP_W);
	if ( kw <= 0 ) return -2;
	*p_kw = kw;
	return wm_obj_gid(kw);
}

/* It is ready: a semaphore of its own the test counts */
LOCAL BOOL ready( void )
{
	T_CSEM	cs;

	memset(&cs, 0, sizeof(cs));
	cs.sematr = TA_TFIFO;
	cs.isemcnt = 0;
	cs.maxsem = 1;
	return (BOOL)( tk_cre_sem(&cs) > 0 );
}

LOCAL CONST char	kill_fig[] =
	"<tad version=\"1.0\" encoding=\"UTF-8\"><figure>"
	"<rect left=\"4\" top=\"4\" right=\"120\" bottom=\"60\" fillColor=\"#80a0ff\" strokeColor=\"#000000\"/>"
	"<ellipse left=\"20\" top=\"20\" right=\"200\" bottom=\"140\" fillColor=\"#ff8080\" strokeColor=\"#000000\"/>"
	"<polygon points=\"10,150 100,90 190,150 150,170 50,170\" fillColor=\"#80ff80\" strokeColor=\"#000000\"/>"
	"</figure></tad>";

/* Drawing without end, every kind of drawing a process may do; the test ends it */
LOCAL INT killdraw( void )
{
	T_DPRECT	r;
	UINT		n = 0;
	INT		gid, len;
	ID		kw = 0;

	gid = kill_window(&kw);
	if ( gid < 0 ) return 1900 - gid;
	for ( len = 0; kill_fig[len] != '\0'; len++ ) ;
	if ( !ready() ) return 1903;
	for ( ;; n++ ) {
		r.left = (INT)( n % 40 );
		r.top = (INT)( n % 30 );
		r.right = r.left + 200;
		r.bottom = r.top + 140;
		(void)dp_fill_rect(gid, &r, 0x00FFFFFF ^ ( n * 0x010305 ));
		(void)dp_line(gid, 0, 0, 250, (INT)( n % 170 ), 0x000000);
		(void)dp_text(gid, 8, 150, (CONST UB *)"描画を止める試験 ABC", n * 0x030201, 14 + (INT)( n % 10 ));
		r.left = 0;
		r.top = 0;
		r.right = 250;
		r.bottom = 170;
		(void)dp_draw_tad(gid, &r, (INT)( n % 20 ), 0, (CONST UB *)kill_fig, len);
		(void)wm_obj_flush(kw, NULL);
	}
	return 0;
}

/* A menu shown on its window and waited for, with nobody to answer it; the test ends it */
LOCAL INT killmenu( void )
{
	T_DPRECT	r;
	T_MNSEL		sel;
	TS_UUID		def;
	INT		gid;
	ID		kw = 0, mid;

	memcpy(&def, &arg[1], sizeof(def));
	gid = kill_window(&kw);
	if ( gid < 0 ) return 2000 - gid;
	r.left = 0;
	r.top = 0;
	r.right = 250;
	r.bottom = 170;
	if ( dp_fill_rect(gid, &r, 0x00C0E0FF) != E_OK ) return 2003;
	if ( wm_obj_flush(kw, NULL) != E_OK ) return 2004;
	if ( mn_cre_men(&def, &mid) != E_OK ) return 2005;
	if ( !ready() ) return 2006;
	for ( ;; ) {
		(void)mn_pop_men(mid, kw, 20, 20, 0, &sel);
	}
	return 0;
}

#define UNMAPPED	((void *)0x0000200000000000UL)	/* in its half, but not mapped */

/*
 * A window ("ktkill") and a sub window of it ("ktsub"), and then a fault:
 * a write where nothing is mapped. The process never closes them.
 */
LOCAL INT killwin( void )
{
	static CONST char	hex[] = "0123456789abcdef";
	static CONST char	head[] = "{\"rect\":[320,160,600,200],\"attr\":1,\"sub\":\"";
	char			j[sizeof(head) + TS_UUID_STRLEN + 4];
	T_OBCRE			c;
	TS_UUID			win, sub;
	INT			n, i;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"ktkill";
	c.json = (CONST UB *)"{\"rect\":[300,220,560,380],\"attr\":7}";
	for ( c.jsonsz = 0; c.json[c.jsonsz] != '\0'; c.jsonsz++ ) ;
	if ( ob_cre_obj(&c, &win) != E_OK ) return 2400;

	/* the sub window names its window by UUID, 8-4-4-4-12 */
	for ( n = 0; head[n] != '\0'; n++ ) j[n] = head[n];
	for ( i = 0; i < 16; i++ ) {
		if ( i == 4 || i == 6 || i == 8 || i == 10 ) j[n++] = '-';
		j[n++] = hex[win.b[i] >> 4];
		j[n++] = hex[win.b[i] & 0x0F];
	}
	j[n++] = '"';
	j[n++] = '}';
	j[n] = '\0';
	c.name = (CONST UB *)"ktsub";
	c.json = (CONST UB *)j;
	c.jsonsz = (SZ)n;
	if ( ob_cre_obj(&c, &sub) != E_OK ) return 2401;
	if ( !ready() ) return 2402;
	tk_dly_tsk(300);
	*(volatile UW *)UNMAPPED = 1;		/* the fault that ends it */
	return 2403;
}

/* A window of its own, and a wait without end in it: Shift+Pause ends it */
LOCAL INT pausewin( void )
{
	CONST char	*j = "{\"rect\":[320,240,620,420],\"attr\":7}";
	T_OBCRE		c;
	TS_UUID		win;

	memset(&c, 0, sizeof(c));
	c.type = OB_T_WINDOW;
	c.sub = OB_S_WINDOW;
	c.name = (CONST UB *)"ktpause";
	c.json = (CONST UB *)j;
	for ( c.jsonsz = 0; j[c.jsonsz] != '\0'; c.jsonsz++ ) ;
	if ( ob_cre_obj(&c, &win) != E_OK ) return 2500;
	if ( !ready() ) return 2501;
	for ( ;; ) {
		(void)tk_slp_tsk(TMO_FEVR);
	}
	return 2502;
}

/* The exceptions the desktop tells of (design 5.5): the address and the three kinds */
#define FAULT_VA	0x0000200000001230UL

LOCAL INT faults( void )
{
	if ( arg[2] > 0 ) tk_dly_tsk(arg[2]);
	switch ( arg[1] ) {
	case 0:
		*(volatile UW *)FAULT_VA = 1;			/* データアボート */
		break;
	case 1:
		__asm__ volatile ( "udf #0" );			/* 未定義命令 */
		break;
	case 2:
		( (void (*)( void ))( FAULT_VA & ~3UL ) )();	/* 命令アボート */
		break;
	default:
		return 2601;
	}
	return 2600;
}

LOCAL char	longline[700];

/* The calendar and the console refuse pointers that are not the process's */
LOCAL INT dtptr( void )
{
	TS_TIME	t = 0;
	TS_TM	tm;
	INT	z = 0, i;
	void	*ro = (void *)same;		/* the program's text: read only */

	/* what the kernel would write */
	if ( dt_gettime(KERNEL_PTR) != E_MACV ) return 2100;
	if ( dt_gettime(UNMAPPED) != E_MACV ) return 2101;
	if ( dt_gettime(ro) != E_MACV ) return 2102;
	if ( dt_getsystz(KERNEL_PTR) != E_MACV ) return 2103;
	if ( dt_getsystz(ro) != E_MACV ) return 2104;
	if ( dt_gettime(&t) != E_OK || t <= 0 ) return 2105;
	if ( dt_gmtime(&t, KERNEL_PTR) != E_MACV ) return 2106;
	if ( dt_gmtime(&t, ro) != E_MACV ) return 2107;
	if ( dt_localtime(&t, UNMAPPED) != E_MACV ) return 2108;
	if ( dt_mktime(&tm, KERNEL_PTR) != E_MACV ) return 2109;
	if ( dt_mktime(&tm, ro) != E_MACV ) return 2110;

	/* what the kernel would read */
	if ( dt_gmtime(KERNEL_PTR, &tm) != E_MACV ) return 2111;
	if ( dt_localtime(UNMAPPED, &tm) != E_MACV ) return 2112;
	if ( dt_mktime(KERNEL_PTR, &t) != E_MACV ) return 2113;
	if ( tm_putstring(KERNEL_PTR) != E_MACV ) return 2114;
	if ( tm_putstring(UNMAPPED) != E_MACV ) return 2115;

	/* and what is the process's, as before */
	if ( dt_gmtime(&t, &tm) != E_OK || tm.tm_year < 85 ) return 2116;
	if ( dt_localtime(&t, &tm) != E_OK ) return 2117;
	if ( dt_mktime(&tm, &t) != E_OK ) return 2118;
	if ( dt_getsystz(&z) != E_OK ) return 2119;
	for ( i = 0; i < (INT)sizeof(longline) - 1; i++ ) longline[i] = (char)( 'a' + i % 26 );
	memcpy(&longline[sizeof(longline) - 12], "dtptr end\n", 11);
	if ( tm_putstring((CONST UB *)longline) != E_OK ) return 2120;
	return 0;
}

/*
 * Setting the time and the zone. With a user in arg[1..4] it logs in as
 * that one, who may not: each way of setting them is refused and
 * nothing changes. Without, it is the administrator its parent is, and
 * each way goes, the zone becoming arg[5].
 */
LOCAL INT clockset( void )
{
	CONST TS_UUID	clock = OB_UUID_CLOCK_INIT;
	TS_UUID		me, none;
	SYSTIM		s, u;
	TS_TIME		t = 0;
	INT		z0 = 0, z = 0, want = (INT)arg[5];
	ID		k;
	BOOL		user;

	memcpy(&me, &arg[1], sizeof(me));
	memset(&none, 0, sizeof(none));
	user = !same_uuid(&me, &none);
	if ( user && ob_login(&me, (CONST UB *)"ktest-pw") != E_OK ) return 2200;
	if ( dt_getsystz(&z0) != E_OK ) return 2201;
	if ( dt_gettime(&t) != E_OK || tk_get_tim(&s) != E_OK || tk_get_utc(&u) != E_OK ) return 2202;
	if ( user ) {
		if ( dt_settime(t) != E_OACV ) return 2210;
		if ( tk_set_tim(&s) != E_OACV ) return 2211;
		if ( tk_set_utc(&u) != E_OACV ) return 2212;
		if ( dt_setsystz(want) != E_OACV ) return 2213;
		if ( ob_opn_obj(&clock, OB_OP_WRITE) != E_OACV ) return 2214;
		if ( dt_getsystz(&z) != E_OK || z != z0 ) return 2215;
		k = ob_opn_obj(&clock, OB_OP_R);		/* reading it is everyone's */
		if ( k <= 0 ) return 2216;
		(void)ob_cls_obj(k);
		return 0;
	}
	if ( dt_settime(t) != E_OK ) return 2220;
	if ( tk_set_tim(&s) != E_OK ) return 2221;
	if ( tk_set_utc(&u) != E_OK ) return 2222;
	if ( dt_setsystz(want) != E_OK ) return 2223;
	if ( dt_getsystz(&z) != E_OK || z != want ) return 2224;
	if ( dt_setsystz(15 * 60) != E_PAR ) return 2225;
	return 0;
}

/*
 * The random bytes as programs built before asked for them: the system
 * call itself, which the kernel keeps as a read of 乱数
 */
LOCAL ER old_get_random( void *buf, SZ len )
{
	register UD	x0 __asm__("x0") = (UD)buf;
	register UD	x1 __asm__("x1") = (UD)len;
	register UD	x8 __asm__("x8") = TSN_TS_GET_RANDOM;

	__asm__ volatile("svc #0" : "+r"(x0), "+r"(x1) : "r"(x8)
			 : "x2", "x3", "x4", "x5", "x6", "x7", "x9", "x10", "x11", "x12", "x13",
			   "x14", "x15", "x16", "x17", "x30", "memory", "cc");
	return (ER)x0;
}

/* A record of an object read into rbuf: its length, or an error */
LOCAL INT rec_text( CONST TS_UUID *u, UINT ops, INT recno )
{
	SZ	asz = 0;
	ER	er;
	ID	k = ob_opn_obj(u, ops);

	if ( k <= 0 ) return (INT)k;
	er = ob_rea_rec(k, recno, 0, rbuf, 4096, &asz);
	(void)ob_cls_obj(k);
	return ( er < E_OK ) ? (INT)er : (INT)asz;
}

/*
 * The device objects as a program sees them. arg[1]:
 *   1	how long ts_get_random (the library, reading 乱数) takes for
 *	arg[2] bytes: the exit code is nanoseconds a call
 *   2	the same for the old system call
 *   3	as the user arg[2..5], not an administrator: what is everyone's is
 *	given, the rest refused
 *   4	as the administrator its parent is: what needs x is given too
 */
LOCAL INT devob( void )
{
	CONST TS_UUID	rnd = OB_UUID_RANDOM_INIT;
	CONST TS_UUID	disp = OB_UUID_DISPLAY_INIT;
	CONST TS_UUID	in = OB_UUID_INPUT_INIT;
	TS_UUID		me;
	T_HIDEV		ev;
	UD		t0 = 0, t1 = 0;
	INT		i, n = 2000, cnt = 0;
	SZ		len = (SZ)arg[2], asz = 0;
	ID		k;

	if ( arg[1] == 1 || arg[1] == 2 ) {
		ER	(*fn)( void *, SZ ) = ( arg[1] == 1 ) ? ts_get_random : old_get_random;

		if ( len <= 0 || len > (SZ)sizeof(rbuf) ) return 2701;
		if ( fn(rbuf, len) != E_OK ) return 2702;
		(void)ts_get_mono(&t0);
		for ( i = 0; i < n; i++ ) {
			if ( fn(rbuf, len) != E_OK ) return 2703;
		}
		(void)ts_get_mono(&t1);
		return (INT)( ( t1 - t0 ) / (UD)n );
	}
	if ( arg[1] == 3 ) {
		memcpy(&me, &arg[2], sizeof(me));
		if ( ob_login(&me, (CONST UB *)"ktest-pw") != E_OK ) return 2710;
		/* random bytes are everyone's; stirring them is not */
		if ( ts_get_random(rbuf, 64) != E_OK ) return 2711;
		if ( old_get_random(rbuf, 64) != E_OK ) return 2712;
		if ( ob_opn_obj(&rnd, OB_OP_WRITE) != E_OACV ) return 2713;
		/* the screen's mode is everyone's to read, not to set; its pixels are not */
		if ( rec_text(&disp, OB_OP_R, OB_DSP_MODE) <= 0 ) return 2714;
		if ( rec_text(&disp, OB_OP_R, OB_DSP_PIXELS) != E_OACV ) return 2715;
		if ( ob_opn_obj(&disp, OB_OP_WRITE) != E_OACV ) return 2716;
		if ( ob_opn_obj(&disp, OB_OP_READ | OB_OP_EXEC) != E_OACV ) return 2717;
		/* what the input devices are is everyone's; what is typed is not, nor putting in */
		if ( rec_text(&in, OB_OP_R, OB_IN_STATE) <= 0 ) return 2718;
		if ( rec_text(&in, OB_OP_R, OB_IN_EVENTS) != E_OACV ) return 2719;
		if ( ob_opn_obj(&in, OB_OP_WRITE | OB_OP_EXEC) != E_OACV ) return 2720;
		k = ob_opn_obj(&in, OB_OP_R);
		if ( k <= 0 ) return 2721;
		memset(&ev, 0, sizeof(ev));
		ev.type = HID_EV_KEY_DOWN;
		if ( ob_wri_rec(k, OB_IN_EVENTS, 0, &ev, sizeof(ev), &asz) != E_OACV ) return 2722;
		(void)ob_cls_obj(k);
		/* the USB devices: listed, read, never written */
		if ( ob_lst_obj(OB_T_DEVICE, OB_S_USB, NULL, list, LIST_MAX, &cnt) != E_OK || cnt < 1 ) {
			return 2723;
		}
		if ( rec_text(&list[0], OB_OP_R, OB_USB_INFO) <= 0 ) return 2724;
		if ( ob_opn_obj(&list[0], OB_OP_WRITE) != E_OACV ) return 2725;
		return 0;
	}
	if ( arg[1] == 4 ) {
		if ( rec_text(&disp, OB_OP_R | OB_OP_EXEC, OB_DSP_PIXELS) != 4096 ) return 2730;
		if ( rec_text(&disp, OB_OP_R, OB_DSP_PIXELS) != E_OACV ) return 2731;	/* x asked for */
		k = ob_opn_obj(&in, OB_OP_R | OB_OP_EXEC);
		if ( k <= 0 ) return 2732;
		if ( ob_rea_rec(k, OB_IN_EVENTS, 0, rbuf, sizeof(T_HIDEV) * 4, &asz) != E_OK ) return 2733;
		(void)ob_cls_obj(k);
		k = ob_opn_obj(&rnd, OB_OP_WRITE);
		if ( k <= 0 ) return 2734;
		if ( ob_wri_rec(k, OB_RND_DATA, 0, rbuf, 32, &asz) != E_OK || asz != 32 ) return 2735;
		(void)ob_cls_obj(k);
		return 0;
	}
	return 2700;
}

int main( void )
{
	SZ	n = ts_get_arg(arg, sizeof(arg));

	/* no argument: 100 and the error, or 200 and the size there was */
	if ( n < (SZ)sizeof(arg) ) {
		return ( n < 0 ) ? 100 - (INT)n : 200 + (INT)n;
	}
	switch ( arg[0] ) {
	  case SP_HOLD:	return hold();
	  case SP_NET:	return net();
	  case SP_MOUNT: return mount();
	  case SP_BLOCK: return block();
	  case SP_USER:	return user();
	  case SP_LIMITS: return limits();
	  case SP_OBPTR: return obptr();
	  case SP_BIGIO: return bigio();
	  case SP_WATCH: return watch();
	  case SP_KILLIO: return killio();
	  case SP_KFAULT: return kfault();
	  case SP_OBBIG: return obbig();
	  case SP_KILLOB: return killob();
	  case SP_TKOWN: return tkown();
	  case SP_TKHOLD: return tkhold();
	  case SP_TKATK: return tkatk();
	  case SP_KILLSUB: return killsub();
	  case SP_DROP:	return dropped();
	  case SP_KILLDRAW: return killdraw();
	  case SP_KILLMENU: return killmenu();
	  case SP_DTPTR: return dtptr();
	  case SP_CLOCK: return clockset();
	  case SP_PARALLEL: return parallel();
	  case SP_KILLWIN: return killwin();
	  case SP_PAUSEWIN: return pausewin();
	  case SP_FAULT: return faults();
	  case SP_DEVOB: return devob();
	  default:	return 101;
	}
}
