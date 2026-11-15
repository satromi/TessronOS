/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	dttagent.c
 *	The test agent: the lines of dtuitest.c taken over the network, for
 *	machines with no serial port (make DESKTOP=1 TESTAGENT=1; tools/mcp,
 *	design 12.4.1)
 *
 *	It is in no other build. In this one it opens nothing unless the
 *	start-up settings (cmdline.txt) say testagent=<key>, the key 64 to
 *	128 hex digits (32 to 64 bytes). The key is taken out of the settings
 *	in memory once read, and is then no longer anywhere the console or a
 *	record could show it.
 *
 *	One client at a time, on TCP port TA_PORT, IPv4, from a private
 *	address only. Everything goes over TLS with the key as the shared key:
 *	one without it fails in the handshake, and no line is read before
 *	the handshake has ended. After a failure the next is not looked at
 *	for a second. A connection silent for TA_IDLE_MS is closed.
 *
 *	A client that has gone away without a word (its network changed, the
 *	machine slept) is let go soon, so that the next is taken at once:
 *	  - TCP's keepalive probes an idle connection and drops it when they
 *	    go unanswered (TA_KA_*), as sshd's ClientAliveInterval would;
 *	  - what is sent must make progress within TA_SEND_MS, as Linux's
 *	    TCP_USER_TIMEOUT bounds data left unacknowledged: a send never
 *	    waits on the stack's retransmissions, which give up only after
 *	    minutes;
 *	  - a session whose own address is no longer the interface's (another
 *	    network joined) ends;
 *	  - while a session waits for a line, a new client is taken in as soon
 *	    as it asks, and once its handshake has proved the key, the silent
 *	    session gives way to it. A client without the key never ends one.
 *	A connection the client gave up on while it waited in the listening
 *	queue is dropped without a handshake and without the wait after a
 *	failure, which is for a key that failed.
 *
 *	A line is taken as dtuitest.c takes it, and its answer lines come
 *	back without the mark "@ut ". These few are the agent's own, the
 *	bytes they name following the answer or the line:
 *
 *	  hello			"ok tessronos 1 W H": the protocol and the screen
 *	  shot			"ok W H BYTES", then the pixels, R G B a pixel from
 *				the top left, pointer included
 *	  log POS [MAX]		what the console has said from POS: "ok N NEXT END",
 *				then N bytes (at most TA_LOG_MAX). A POS since
 *				overwritten moves on to the oldest byte kept
 *	  put BYTES		followed by the bytes: an object holding them as
 *				record 1, as a package taken in has them: "ok UUID"
 *	  rmobj UUID		that object taken away
 *	  reboot, poweroff	"ok", and the session ends as the person ending it
 *				would end it, windows saved
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/so.h>
#include <ts/wm.h>
#include <ts/ob.h>
#include <ts/uuid.h>
#include "desktop.h"

#ifdef USE_TESTAGENT
#include "ta_tls.h"

IMPORT UD tm_log_at( void );			/* lib/libtm: bytes ever written to the console */
IMPORT INT knl_so_tcp_stats( UD *v, INT n ) __attribute__((weak));	/* the NetBSD stack's TCP counters */

/* TCP_STAT_* of the NetBSD stack said after a put: whether what came again had come before */
#define TA_TCPST	46
#define TCPST_DELACK		8
#define TCPST_SNDACKS		23
#define TCPST_SNDWINUP		26
#define TCPST_RCVTOTAL		28
#define TCPST_RCVPACK		29
#define TCPST_RCVDUPPACK	35
#define TCPST_RCVPARTDUPPACK	37
#define TCPST_RCVOOPACK		39
#define TCPST_RCVAFTERWIN	41

#define TA_PORT		7878
#define TA_LINE		512
#define TA_CHUNK	( 4 * 1024 * 1024 )	/* bytes of a put written at a time (design 7.3) */
#define TA_LOG_MAX	(60 * 1024)
#define TA_KEY_MIN	32		/* bytes */
#define TA_KEY_MAX	64
#define TA_HANDSHAKE_MS	10000
#define TA_IDLE_MS	600000
#define TA_ADDR_LOOK_MS	2000		/* how often the address shown is looked at again */
#define TA_DATA_MS	60000
#define TA_SEND_MS	30000
#define TA_REFUSED_MS	1000
#define TA_STACK	( 64 * 1024 )
#define TA_SHOW_MS	3000
#define TA_SHOW_PRI	30
#define TA_LOOK_MS	1000		/* how often a session waiting for a line looks about it */
#define TA_KA_IDLE_S	15		/* keepalive: idle before the first probe, s */
#define TA_KA_INTVL_S	5		/* between probes, s */
#define TA_KA_CNT	3		/* probes unanswered before the connection is dropped */
#define TA_TLS_CONN_EOF	(-0x7280)	/* mbed TLS: the other side closed in the middle */
#define TA_TLS_RECORD	16384		/* the most a TLS record carries */

/*
 * A connection: the socket, how long a receive may wait, whether it is
 * given up (nothing more is sent on it, and it is closed with a reset),
 * and the two addresses
 */
typedef struct {
	INT	s;
	INT	tmo;
	BOOL	dead;
	UB	self[4];
	UB	peer[4];
} TAIO;

LOCAL TAIO	ta_iob[2];		/* the session's, and one coming in while it waits */
LOCAL TAIO	*ta_io = &ta_iob[0];
LOCAL void	*ta_conn = NULL;
/*
 * A whole record read at a time: mbed TLS then holds nothing of the
 * client's between reads, and a session waiting on the socket for a
 * line misses nothing it already has.
 */
LOCAL UB	ta_in[TA_TLS_RECORD];
LOCAL BOOL	ta_ending = FALSE;	/* reboot or poweroff asked: no one else is let in */
LOCAL INT	ta_in_at = 0, ta_in_n = 0;

/*
 * What the agent's task is doing, for the message's task to tell when it
 * has not moved on for longer than any of its waits may last
 */
#define TA_STUCK_MS	120000
LOCAL CONST char	TA_ACCEPT[] = "accept";
LOCAL CONST char	TA_LINE_WAIT[] = "line";
LOCAL ID		ta_tid = 0;
LOCAL CONST char	*volatile ta_step = TA_ACCEPT;
LOCAL volatile UW	ta_beat = 0;

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
}

LOCAL BOOL connected( INT s )
{
	struct sockaddr_in	sa;
	socklen_t		len = sizeof(sa);

	return (BOOL)( so_getpeername(s, (struct sockaddr *)&sa, &len) >= E_OK );
}

/*
 * As much as the socket takes now, with no wait inside the stack: the
 * wait is here, for room in the sending buffer, and it ends when none has
 * come for TA_SEND_MS, as the client then takes in nothing
 */
LOCAL int io_send( void *ctx, const unsigned char *buf, size_t n )
{
	TAIO		*io = (TAIO *)ctx;
	struct pollfd	p;
	UW		t0 = now_ms();
	INT		k = 0, left;

	if ( io->dead ) {
		return -1;
	}
	for ( ;; ) {
		left = TA_SEND_MS - (INT)( now_ms() - t0 );
		p.fd = io->s;
		p.events = POLLOUT;
		p.revents = 0;
		if ( left <= 0 || so_poll(&p, 1, left) <= 0 || ( p.revents & ( POLLERR | POLLHUP ) ) != 0 ) {
			break;
		}
		k = so_send(io->s, buf, (SZ)n, MSG_DONTWAIT);
		if ( k > 0 ) {
			return k;
		}
		/*
		 * E_TMOUT is both no room after all and the stack's own giving
		 * up (ETIMEDOUT): only a connection still connected is waited on
		 */
		if ( k != E_TMOUT || !connected(io->s) ) {
			break;
		}
		tk_dly_tsk(10);
	}
	tm_printf((UB *)"testagent: sending given up after %d ms (%d): the client takes nothing in\n",
		  (INT)( now_ms() - t0 ), k);
	io->dead = TRUE;
	return -1;
}

LOCAL int io_recv( void *ctx, unsigned char *buf, size_t n )
{
	TAIO		*io = (TAIO *)ctx;
	struct pollfd	p;
	INT		k;

	p.fd = io->s;
	p.events = POLLIN;
	p.revents = 0;
	if ( io->dead || so_poll(&p, 1, io->tmo) <= 0 ) {
		io->dead = TRUE;
		return -1;
	}
	k = so_recv(io->s, buf, (SZ)n, 0);
	if ( k < 0 ) {
		io->dead = TRUE;
	}
	return k;
}

/* Bytes to the client: FALSE when the connection is given up */
LOCAL BOOL send_out( CONST void *buf, size_t n )
{
	if ( ta_conn == NULL || ta_io->dead ) {
		return FALSE;
	}
	if ( ta_tls_write(ta_conn, buf, n) != 0 ) {
		ta_io->dead = TRUE;
		return FALSE;
	}
	return TRUE;
}

/* An answer line of dtuitest.c's: it is whole, new line and all */
LOCAL void to_client( CONST char *s )
{
	(void)send_out(s, (size_t)knl_strlen(s));
}

LOCAL void answer( CONST char *s )
{
	to_client(s);
}

/* The next byte from the client, -1 when there is no more */
LOCAL INT in_byte( void )
{
	if ( ta_in_at >= ta_in_n ) {
		ta_in_n = ta_tls_read(ta_conn, ta_in, sizeof(ta_in));
		ta_in_at = 0;
		if ( ta_in_n <= 0 ) {
			tm_printf((UB *)"testagent: the connection ended in reading (-0x%x)\n", -ta_in_n);
			ta_in_n = 0;
			return -1;
		}
	}
	return ta_in[ta_in_at++];
}

LOCAL void step( CONST char *what )
{
	ta_step = what;
	ta_beat = now_ms();
}

/* Up to max bytes from the client, those left over from the lines first; -1 when there is no more */
LOCAL INT in_bytes( UB *buf, INT max )
{
	INT	n = ta_in_n - ta_in_at;

	if ( n > 0 ) {
		n = ( n > max ) ? max : n;
		knl_memcpy(buf, ta_in + ta_in_at, (SZ)n);
		ta_in_at += n;
		return n;
	}
	n = ta_tls_read(ta_conn, buf, max);
	return ( n > 0 ) ? n : -1;
}

LOCAL BOOL is_word( CONST char *s, CONST char *w, CONST char **rest )
{
	while ( *w != 0 ) {
		if ( *s++ != *w++ ) {
			return FALSE;
		}
	}
	if ( *s != 0 && *s != ' ' ) {
		return FALSE;
	}
	while ( *s == ' ' ) s++;
	*rest = s;
	return TRUE;
}

LOCAL UD dec( CONST char **p )
{
	CONST char	*s = *p;
	UD		v = 0;

	while ( *s == ' ' ) s++;
	for ( ; *s >= '0' && *s <= '9'; s++ ) {
		v = v * 10 + (UD)( *s - '0' );
	}
	*p = s;
	return v;
}

LOCAL void do_hello( void )
{
	char	t[64];
	INT	w = 0, h = 0;

	wm_screen(&w, &h);
	(void)tm_sprintf((UB *)t, (UB *)"ok tessronos 1 %d %d\n", w, h);
	answer(t);
}

LOCAL void do_shot( void )
{
	UW		*px = NULL;
	T_DPRECT	at;
	UB		*row;
	char		t[64];
	INT		w, h, i, k = 0;
	SZ		n;
	ER		er;

	er = dt_scr_grab(DT_SCR_SCREEN, TRUE, NULL, &px, &at);
	row = ( er >= E_OK ) ? (UB *)Kmalloc(TA_CHUNK) : NULL;
	if ( row == NULL ) {
		if ( px != NULL ) Kfree(px);
		(void)tm_sprintf((UB *)t, (UB *)"err shot %d\n", (INT)er);
		answer(t);
		return;
	}
	w = at.right - at.left;
	h = at.bottom - at.top;
	(void)tm_sprintf((UB *)t, (UB *)"ok %d %d %d\n", w, h, w * h * 3);
	answer(t);
	n = (SZ)w * h;
	for ( i = 0; i < n; i++ ) {
		row[k++] = (UB)( px[i] >> 16 );
		row[k++] = (UB)( px[i] >> 8 );
		row[k++] = (UB)px[i];
		if ( k > TA_CHUNK - 3 || i == n - 1 ) {
			if ( !send_out(row, (size_t)k) ) {
				break;
			}
			k = 0;
		}
	}
	Kfree(row);
	Kfree(px);
}

LOCAL void do_log( CONST char *s )
{
	UD	pos = dec(&s), max = dec(&s);
	UB	*buf;
	INT	n;
	char	t[80];

	if ( max == 0 || max > TA_LOG_MAX ) max = TA_LOG_MAX;
	buf = (UB *)Kmalloc((SZ)max);
	if ( buf == NULL ) {
		answer("err no memory\n");
		return;
	}
	n = tm_log_read(&pos, buf, (INT)max);
	(void)tm_sprintf((UB *)t, (UB *)"ok %d %d %d\n", n, (INT)pos, (INT)tm_log_at());
	answer(t);
	if ( n > 0 ) {
		(void)send_out(buf, (size_t)n);
	}
	Kfree(buf);
}

/*
 * The bytes taken into an object, as a package taken in by ファイル変換 is
 * one: a word on what it is as record 0, the file as record 1. In pieces, as
 * a package of the whole system does not fit in memory at once. FALSE when
 * fewer bytes came than were said: what follows on the connection can no
 * longer be told from them, and the session ends. An object left half
 * filled is taken away.
 */
LOCAL BOOL do_put( CONST char *s )
{
	static CONST char	js[] = "{\"name\":\"試験用の受取り\",\"relationship\":[],\"linktype\":false,\"refCount\":1,\"recordCount\":2,\"editable\":true,\"deletable\":true,\"readable\":true,\"applist\":{},\"tessronos\":{\"records\":[{\"n\":0,\"rt\":1},{\"n\":1,\"rt\":15}]}}";
	static CONST char	note[] = "<tad version=\"1.0\" encoding=\"UTF-8\"><document><p>test</p></document></tad>";
	UD	left = dec(&s), at = 0;
	UB	*buf = (UB *)Kmalloc(TA_CHUNK);
	T_OBCRE	c;
	TS_UUID	u;
	ID	key = 0;
	INT	rec = -1;
	SZ	asz = 0;
	UW	t0, tw0, wr_ms = 0;
	UD	st0[TA_TCPST], st1[TA_TCPST];
	BOOL	made = FALSE;
	ER	er = ( buf != NULL ) ? E_OK : E_NOMEM;
	char	t[TS_UUID_STRLEN + 16];

	if ( er >= E_OK ) {
		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_STORAGE;
		c.sub = OB_S_FILE;
		c.json = (CONST UB *)js;
		c.jsonsz = (SZ)knl_strlen(js);
		knl_ob_make_for_person(TRUE);
		er = ob_cre_obj(&c, &u);
		knl_ob_make_for_person(FALSE);
		made = ( er >= E_OK );
	}
	if ( er >= E_OK ) {
		(void)ob_lnk_obj(&u);
		key = ob_opn_obj(&u, OB_OP_R | OB_OP_WRITE | OB_OP_RECORD | OB_O_EXCL);
		er = ( key > 0 ) ? E_OK : (ER)key;
	}
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_TAD, 0, &rec);
	if ( er >= E_OK ) er = ob_wri_rec(key, rec, 0, note, (SZ)( sizeof(note) - 1 ), &asz);
	if ( er >= E_OK ) er = ob_apd_rec(key, OB_RT_SYSDATA, 0, &rec);

	/* the bytes are taken off the connection whatever came of the object */
	ta_io->tmo = TA_DATA_MS;
	knl_memset(st0, 0, sizeof(st0));
	if ( knl_so_tcp_stats != NULL ) (void)knl_so_tcp_stats(st0, TA_TCPST);
	t0 = now_ms();
	while ( left > 0 ) {
		INT	n = 0, b;
		UW	tw;

		while ( n < TA_CHUNK && (UD)n < left && ( buf != NULL ) ) {
			b = (INT)( ( left - (UD)n < (UD)( TA_CHUNK - n ) ) ? left - (UD)n : (UD)( TA_CHUNK - n ) );
			step("put: reading");
			if ( ( b = in_bytes(buf + n, b) ) < 0 ) break;
			n += b;
		}
		if ( n == 0 ) {
			er = ( er >= E_OK ) ? E_IO : er;
			break;
		}
		step("put: writing");
		tw = now_ms();
		if ( er >= E_OK ) er = ob_wri_rec(key, rec, (D)at, buf, n, &asz);
		wr_ms += now_ms() - tw;
		at += (UD)n;
		left -= (UD)n;
	}
	step("put: closing");
	tw0 = now_ms();
	if ( key > 0 ) (void)ob_cls_obj(key);
	tm_printf((UB *)"testagent: put %d KB in %d ms: %d ms writing, %d ms closing\n",
		  (INT)( at >> 10 ), (INT)( tw0 - t0 ), (INT)wr_ms, (INT)( now_ms() - tw0 ));
	if ( knl_so_tcp_stats != NULL && knl_so_tcp_stats(st1, TA_TCPST) >= TA_TCPST ) {
#define D_(i)	(INT)( st1[i] - st0[i] )
		tm_printf((UB *)"testagent: TCP in the put: received %d (%d in order, %d only again, %d partly again,"
			  " %d out of order, %d past the window); sent %d ACKs (%d delayed), %d window updates\n",
			  D_(TCPST_RCVTOTAL), D_(TCPST_RCVPACK), D_(TCPST_RCVDUPPACK), D_(TCPST_RCVPARTDUPPACK),
			  D_(TCPST_RCVOOPACK), D_(TCPST_RCVAFTERWIN), D_(TCPST_SNDACKS), D_(TCPST_DELACK), D_(TCPST_SNDWINUP));
#undef D_
	}
	if ( buf != NULL ) Kfree(buf);
	if ( er < E_OK ) {
		if ( made ) {
			(void)ob_unl_obj(&u);
			(void)ob_del_obj(&u);
		}
		(void)tm_sprintf((UB *)t, (UB *)"err put %d\n", (INT)er);
		answer(t);
		if ( left > 0 ) {
			tm_printf((UB *)"testagent: put: %d KB not come: the session ends\n", (INT)( left >> 10 ));
		}
		return (BOOL)( left == 0 );
	}
	knl_memcpy(t, "ok ", 3);
	(void)ts_uuid_to_str(&u, t + 3, TS_UUID_STRLEN + 1);
	answer(t);
	answer("\n");
	return TRUE;
}

LOCAL void do_rmobj( CONST char *s )
{
	TS_UUID	u;
	char	t[32];
	ER	er = ts_str_to_uuid(s, &u);

	if ( er >= E_OK ) {
		(void)ob_unl_obj(&u);
		er = ob_del_obj(&u);
	}
	(void)tm_sprintf((UB *)t, (UB *)"ok %d\n", (INT)er);
	answer(t);
}

/* One line, which may be the agent's own. FALSE: the session is over */
LOCAL BOOL do_line( CONST char *s )
{
	CONST char	*r;

	if ( is_word(s, "hello", &r) ) {
		do_hello();
	} else if ( is_word(s, "shot", &r) ) {
		do_shot();
	} else if ( is_word(s, "log", &r) ) {
		do_log(r);
	} else if ( is_word(s, "put", &r) ) {
		if ( !do_put(r) ) return FALSE;
	} else if ( is_word(s, "rmobj", &r) ) {
		do_rmobj(r);
	} else if ( is_word(s, "reboot", &r) || is_word(s, "poweroff", &r) ) {
		tm_printf((UB *)"testagent: %s\n", s);
		answer("ok\n");
		ta_ending = TRUE;
		(void)dt_tagent_power(( s[0] == 'r' ) ? 1 : 0);
		return FALSE;
	} else {
		dt_uitest_line(s, to_client);
	}
	return TRUE;
}

/* From a private network only: this port is for the tests on one's own LAN */
LOCAL BOOL private_addr( CONST UB *a )
{
	return (BOOL)( a[0] == 10 || ( a[0] == 172 && ( a[1] & 0xf0 ) == 16 )
		    || ( a[0] == 192 && a[1] == 168 ) || ( a[0] == 169 && a[1] == 254 )
		    || a[0] == 127 );
}

LOCAL char	ta_shown[200];

LOCAL void show( void )
{
	(void)wm_msg_put(ta_shown);
	tm_printf((UB *)"testagent: %s\n", ta_shown);
}

/* This machine's address listened on, or what it waits for */
LOCAL void tell( CONST char *what, CONST UB *a )
{
	if ( a != NULL ) {
		(void)tm_sprintf((UB *)ta_shown, (UB *)"試験用の遠隔操作が有効: %s この機械 %d.%d.%d.%d:%d",
				 what, a[0], a[1], a[2], a[3], TA_PORT);
	} else {
		(void)tm_sprintf((UB *)ta_shown, (UB *)"試験用の遠隔操作が有効: %s", what);
	}
	show();
}

/* A connection: both ends, so that the client's address is not taken for this machine's */
LOCAL void tell_conn( CONST char *what, CONST TAIO *io )
{
	CONST UB	*a = io->self, *b = io->peer;

	(void)tm_sprintf((UB *)ta_shown, (UB *)"試験用の遠隔操作が有効: %s この機械 %d.%d.%d.%d:%d、相手 %d.%d.%d.%d",
			 what, a[0], a[1], a[2], a[3], TA_PORT, b[0], b[1], b[2], b[3]);
	show();
}

/*
 * The person is to see that this is on as long as it is. The message line
 * is the programs' too, and they clear it as the pointer moves: it is put back
 * whenever it stands empty, and what a program says there is left alone.
 */
LOCAL void ta_show_task( INT stacd, void *exinf )
{
	char		t[8];
	INT		n;
	UW		told = 0, beat;
	CONST char	*s;
	T_RTSK		r;

	(void)stacd;
	(void)exinf;
	for ( ;; ) {
		tk_dly_tsk(TA_SHOW_MS);
		n = wm_msg_text(t, sizeof(t));
		if ( n <= 0 ) {
			(void)wm_msg_put(ta_shown);
		}
		/* the agent's task gone quiet past its own limits: where, and on what it waits */
		s = ta_step;
		beat = ta_beat;
		if ( s != TA_ACCEPT && beat != told
		     && now_ms() - beat > ( ( s == TA_LINE_WAIT ) ? TA_IDLE_MS + TA_STUCK_MS : TA_STUCK_MS ) ) {
			told = beat;
			knl_memset(&r, 0, sizeof(r));
			n = ( ta_tid > 0 ) ? (INT)tk_ref_tsk(ta_tid, &r) : -1;
			tm_printf((UB *)"testagent: no progress for %d s at \"%s\": task %d (%d), state %x, waits %x on %d\n",
				  (INT)( ( now_ms() - beat ) / 1000 ), s, (INT)ta_tid, n, (UW)r.tskstat, (UW)r.tskwait, (INT)r.wid);
		}
	}
}

LOCAL INT listening( void )
{
	struct sockaddr_in	sa;
	INT			s, on = 1;

	s = so_socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) {
		return s;
	}
	(void)so_setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	knl_memset(&sa, 0, sizeof(sa));
	sa.sin_len = sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = so_htons(TA_PORT);
	sa.sin_addr.s_addr = INADDR_ANY;
	if ( so_bind(s, (struct sockaddr *)&sa, sizeof(sa)) < E_OK || so_listen(s, 1) < E_OK ) {
		(void)so_close(s);
		return -1;
	}
	return s;
}

/*
 * A connection ended: TLS's closing word sent while the connection still
 * carries, and one given up closed with a reset, so that the stack does
 * not go on sending to a client that is gone
 */
LOCAL void conn_end( TAIO *io, void *conn )
{
	struct linger	l;

	if ( conn != NULL ) {
		ta_tls_close(conn);		/* io_send sends nothing on one given up */
	}
	if ( io->dead ) {
		l.l_onoff = 1;
		l.l_linger = 0;
		(void)so_setsockopt(io->s, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
	}
	(void)so_close(io->s);
	io->s = -1;
}

/* TCP's keepalive: an idle connection whose client no longer answers is dropped by the stack */
LOCAL void keepalive( INT s )
{
	INT	on = 1, idle = TA_KA_IDLE_S, intvl = TA_KA_INTVL_S, cnt = TA_KA_CNT;

	(void)so_setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	(void)so_setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	(void)so_setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	(void)so_setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

/* The client closed or reset the connection before a byte of it was read */
LOCAL BOOL gone_already( INT s )
{
	struct pollfd	p;
	UB		b;
	INT		k;

	p.fd = s;
	p.events = POLLIN;
	p.revents = 0;
	if ( so_poll(&p, 1, 0) <= 0 ) {
		return FALSE;
	}
	k = so_recv(s, &b, 1, MSG_PEEK | MSG_DONTWAIT);
	return (BOOL)( k == 0 || ( k < 0 && k != E_TMOUT ) );
}

/*
 * The next connection of the listening queue into io, its handshake done:
 * the TLS connection, or NULL. Only a key that failed (or no address of
 * a private network) makes the next wait TA_REFUSED_MS; a connection the
 * client has already given up on is let go at once, so that a queue of
 * them does not keep the one still waiting behind them past its patience.
 */
LOCAL void *come_in( INT ls, TAIO *io )
{
	struct sockaddr_in	peer, self;
	socklen_t		len = sizeof(peer);
	CONST UB		*pa = io->peer;
	void			*conn;
	INT			c, err = 0;

	c = so_accept(ls, (struct sockaddr *)&peer, &len);
	if ( c < 0 ) {
		tk_dly_tsk(TA_REFUSED_MS);
		return NULL;
	}
	knl_memset(io, 0, sizeof(*io));
	io->s = c;
	knl_memcpy(io->peer, &peer.sin_addr, 4);
	len = sizeof(self);
	if ( so_getsockname(c, (struct sockaddr *)&self, &len) >= E_OK ) {
		knl_memcpy(io->self, &self.sin_addr, 4);
	}
	if ( !private_addr(pa) ) {
		tm_printf((UB *)"testagent: refused %d.%d.%d.%d: not a private address\n", pa[0], pa[1], pa[2], pa[3]);
		io->dead = TRUE;
		conn_end(io, NULL);
		tk_dly_tsk(TA_REFUSED_MS);
		return NULL;
	}
	if ( gone_already(c) ) {
		tm_printf((UB *)"testagent: %d.%d.%d.%d gave up while it waited to be taken: let go\n", pa[0], pa[1], pa[2], pa[3]);
		io->dead = TRUE;
		conn_end(io, NULL);
		return NULL;
	}
	keepalive(c);
	io->tmo = TA_HANDSHAKE_MS;
	conn = ta_tls_open(io_send, io_recv, io, &err);
	if ( conn == NULL ) {
		io->dead = TRUE;
		conn_end(io, NULL);
		if ( err == TA_TLS_CONN_EOF ) {
			tm_printf((UB *)"testagent: %d.%d.%d.%d closed in the handshake: let go\n", pa[0], pa[1], pa[2], pa[3]);
		} else {
			tm_printf((UB *)"testagent: refused %d.%d.%d.%d: handshake -0x%x\n", pa[0], pa[1], pa[2], pa[3], -err);
			tk_dly_tsk(TA_REFUSED_MS);
		}
		return NULL;
	}
	return conn;
}

/*
 * The session's own address no longer the interface's: another network
 * joined, nothing of this connection can come or go. No address at all
 * is left to the keepalive, as the same one may come back.
 */
LOCAL BOOL self_gone( void )
{
	UW	now = 0, mask, gw;
	UW	self;

	knl_memcpy(&self, ta_io->self, 4);
	if ( ta_io->self[0] == 127 || self == 0 ) {
		return FALSE;
	}
	return (BOOL)( so_getifaddr(&now, &mask, &gw) >= E_OK && now != 0 && now != self );
}

/*
 * A new client while the session is silent: it takes the session's
 * place once its handshake has proved the key, and not before, so that
 * one without the key cannot end a session. The silent one is reset: a
 * client that went away no longer reads, and one still there learns at
 * once that it was let go.
 */
LOCAL BOOL take_over( INT ls )
{
	TAIO	*io = ( ta_io == &ta_iob[0] ) ? &ta_iob[1] : &ta_iob[0];
	CONST UB *a = ta_io->peer, *b = io->peer;
	void	*conn;

	step("handshake");
	conn = come_in(ls, io);
	if ( conn == NULL ) {
		return FALSE;
	}
	tm_printf((UB *)"testagent: %d.%d.%d.%d came in with the key: the silent session of %d.%d.%d.%d gives way\n",
		  b[0], b[1], b[2], b[3], a[0], a[1], a[2], a[3]);
	ta_io->dead = TRUE;
	conn_end(ta_io, ta_conn);
	ta_io = io;
	ta_conn = conn;
	ta_in_at = ta_in_n = 0;
	tell_conn("接続中", ta_io);
	return TRUE;
}

/*
 * Until the client says something: TRUE when there is something to read
 * (or the connection's end, which the reading finds), FALSE when the
 * session is to end
 */
LOCAL BOOL wait_line( INT ls )
{
	struct pollfd	p[2];
	UW		t0 = now_ms();
	INT		k;

	step(TA_LINE_WAIT);
	for ( ;; ) {
		if ( (INT)( now_ms() - t0 ) >= TA_IDLE_MS ) {
			tm_printf((UB *)"testagent: silent for %d s: the session ends\n", TA_IDLE_MS / 1000);
			return FALSE;
		}
		p[0].fd = ta_io->s;
		p[0].events = POLLIN;
		p[0].revents = 0;
		p[1].fd = ls;
		p[1].events = POLLIN;
		p[1].revents = 0;
		k = so_poll(p, 2, TA_LOOK_MS);
		if ( k < 0 ) {
			ta_io->dead = TRUE;
			return FALSE;
		}
		if ( p[0].revents != 0 ) {
			return TRUE;
		}
		if ( ( p[1].revents & POLLIN ) != 0 ) {
			if ( take_over(ls) ) {
				t0 = now_ms();
			}
			step(TA_LINE_WAIT);
			continue;
		}
		if ( self_gone() ) {
			CONST UB	*a = ta_io->self;

			tm_printf((UB *)"testagent: %d.%d.%d.%d is no longer this machine's address: the session ends\n",
				  a[0], a[1], a[2], a[3]);
			ta_io->dead = TRUE;
			return FALSE;
		}
	}
}

LOCAL void session( INT ls )
{
	char	line[TA_LINE];
	INT	n = 0, c;

	ta_in_at = ta_in_n = 0;
	for ( ;; ) {
		if ( n == 0 && ta_in_at >= ta_in_n && !wait_line(ls) ) {
			break;
		}
		ta_io->tmo = TA_DATA_MS;	/* the rest of a line, once something has come */
		if ( ( c = in_byte() ) < 0 ) {
			break;
		}
		if ( c == '\r' ) continue;
		if ( c != '\n' ) {
			if ( n < TA_LINE - 1 ) line[n++] = (char)c;
			continue;
		}
		line[n] = 0;
		n = 0;
		step("a line");
		if ( !do_line(line) || ta_io->dead ) {
			break;
		}
	}
}

LOCAL void ta_task( INT stacd, void *exinf )
{
	INT	ls;
	UW	addr = 0, mask = 0, gw = 0;
	void	*conn;

	(void)stacd;
	(void)exinf;
	while ( ( ls = listening() ) < 0 ) {
		tk_dly_tsk(2000);
	}
	tell("アドレスの割当てを待っています", NULL);
	while ( so_getifaddr(&addr, &mask, &gw) < E_OK || addr == 0 ) {
		tk_dly_tsk(1000);
	}
	tell("待受け", (CONST UB *)&addr);
	for ( ;; ) {
		step(TA_ACCEPT);
		/*
		 * Listening on every address, a new one (another network
		 * joined) is answered on at once; what the message line shows
		 * follows it, looked at while no one asks to connect.
		 */
		{
			struct pollfd	lp;
			UW		now = 0;

			lp.fd = ls;
			lp.events = POLLIN;
			lp.revents = 0;
			if ( so_poll(&lp, 1, TA_ADDR_LOOK_MS) <= 0 ) {
				if ( so_getifaddr(&now, &mask, &gw) >= E_OK && now != 0 && now != addr ) {
					addr = now;
					tell("待受け", (CONST UB *)&addr);
				}
				continue;
			}
		}
		step("handshake");
		conn = come_in(ls, &ta_iob[0]);
		if ( conn == NULL ) {
			continue;
		}
		ta_io = &ta_iob[0];
		ta_conn = conn;
		tell_conn("接続中", ta_io);
		session(ls);
		step("closing");
		conn_end(ta_io, ta_conn);
		ta_conn = NULL;
		tell_conn("切断", ta_io);
		if ( ta_ending ) {
			/* a client waiting for the machine to come back must not find this one on its way out */
			(void)so_close(ls);
			(void)tk_slp_tsk(TMO_FEVR);
		}
		if ( so_getifaddr(&addr, &mask, &gw) >= E_OK && addr != 0 ) {
			tell("待受け", (CONST UB *)&addr);
		}
	}
}

/* The value of testagent= made into the key, and wiped from where it was read */
LOCAL INT take_key( UB *key )
{
	UB	*c = knl_bootargs, *v = NULL;
	INT	k = 0, i;
	static CONST char	w[] = "testagent=";

	for ( ; *c != 0; c++ ) {
		if ( c != knl_bootargs && c[-1] != ' ' ) continue;
		for ( i = 0; w[i] != 0 && c[i] == (UB)w[i]; i++ ) ;
		if ( w[i] == 0 ) {
			v = c + i;
			break;
		}
	}
	if ( v == NULL ) {
		return 0;
	}
	for ( i = 0; v[i] != 0 && v[i] != ' ' && v[i] != '\n' && v[i] != '\r'; i++ ) {
		UB	d = v[i];
		INT	x = ( d >= '0' && d <= '9' ) ? d - '0' : ( d >= 'a' && d <= 'f' ) ? d - 'a' + 10
			  : ( d >= 'A' && d <= 'F' ) ? d - 'A' + 10 : -1;

		if ( x < 0 || k >= TA_KEY_MAX * 2 ) {
			k = -1;
			break;
		}
		if ( ( k & 1 ) == 0 ) {
			key[k / 2] = (UB)( x << 4 );
		} else {
			key[k / 2] |= (UB)x;
		}
		k++;
	}
	for ( i = 0; v[i] != 0 && v[i] != ' ' && v[i] != '\n' && v[i] != '\r'; i++ ) {
		v[i] = 'x';
	}
#ifdef _X64_
	{
		char	*b = ( (T_BOOTINFO *)knl_bootinfo )->cmdline;

		for ( ; *b != 0; b++ ) {
			if ( ( b == ( (T_BOOTINFO *)knl_bootinfo )->cmdline || b[-1] == ' ' ) ) {
				for ( i = 0; w[i] != 0 && b[i] == w[i]; i++ ) ;
				if ( w[i] == 0 ) {
					for ( b += i; *b != 0 && *b != ' '; b++ ) *b = 'x';
					break;
				}
			}
		}
	}
#endif
	if ( k < TA_KEY_MIN * 2 || ( k & 1 ) != 0 ) {
		return -1;
	}
	return k / 2;
}

EXPORT void dt_tagent_start( void )
{
	static UB	key[TA_KEY_MAX];
	T_CTSK		ct;
	ID		tid;
	INT		n = take_key(key);
	INT		er;

	if ( n == 0 ) {
		tm_printf((UB *)"testagent: off (no testagent= in the settings): the port is not opened\n");
		return;
	}
	if ( n < 0 ) {
		tm_printf((UB *)"testagent: the key must be 64 to 128 hex digits: the port is not opened\n");
		return;
	}
	er = ta_tls_setup(key, n);
	knl_memset(key, 0, sizeof(key));
	if ( er != 0 ) {
		tm_printf((UB *)"testagent: TLS -0x%x: the port is not opened\n", -er);
		return;
	}
	knl_memset(&ct, 0, sizeof(ct));
	ct.tskatr = TA_HLNG | TA_RNG0;
	ct.task = (FP)ta_task;
	ct.itskpri = 6;		/* as the serial port's: the desktop may be busy for a while */
	ct.stksz = TA_STACK;
	tid = tk_cre_tsk(&ct);
	if ( tid > 0 ) {
		ta_tid = tid;
		(void)tk_sta_tsk(tid, 0);
	}
	ct.task = (FP)ta_show_task;
	ct.itskpri = TA_SHOW_PRI;
	ct.stksz = TA_STACK;		/* the line draws its letters on this stack */
	tid = tk_cre_tsk(&ct);
	if ( tid > 0 ) {
		(void)tk_sta_tsk(tid, 0);
	} else {
		tm_printf((UB *)"testagent: the message's task %d\n", (INT)tid);
	}
}

#endif /* USE_TESTAGENT */
