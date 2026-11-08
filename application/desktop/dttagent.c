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
#define TA_DATA_MS	60000
#define TA_SEND_MS	30000
#define TA_REFUSED_MS	1000
#define TA_STACK	( 64 * 1024 )
#define TA_SHOW_MS	3000
#define TA_SHOW_PRI	30

/* What the transport needs: the socket and how long a receive may wait */
typedef struct {
	INT	s;
	INT	tmo;
} TAIO;

LOCAL TAIO	ta_io;
LOCAL void	*ta_conn = NULL;
LOCAL UB	ta_in[1024];
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

LOCAL int io_send( void *ctx, const unsigned char *buf, size_t n )
{
	TAIO		*io = (TAIO *)ctx;
	struct pollfd	p;

	p.fd = io->s;
	p.events = POLLOUT;
	p.revents = 0;
	if ( so_poll(&p, 1, TA_SEND_MS) <= 0 ) {
		return -1;
	}
	return so_send(io->s, buf, (SZ)n, 0);
}

LOCAL int io_recv( void *ctx, unsigned char *buf, size_t n )
{
	TAIO		*io = (TAIO *)ctx;
	struct pollfd	p;

	p.fd = io->s;
	p.events = POLLIN;
	p.revents = 0;
	if ( so_poll(&p, 1, io->tmo) <= 0 ) {
		return -1;
	}
	return so_recv(io->s, buf, (SZ)n, 0);
}

/* An answer line of dtuitest.c's: it is whole, new line and all */
LOCAL void to_client( CONST char *s )
{
	if ( ta_conn != NULL ) {
		(void)ta_tls_write(ta_conn, s, (size_t)knl_strlen(s));
	}
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
			ta_in_n = 0;
			return -1;
		}
	}
	return ta_in[ta_in_at++];
}

LOCAL UW now_ms( void )
{
	SYSTIM	t;

	(void)tk_get_otm(&t);
	return (UW)t.lo;
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
			if ( ta_tls_write(ta_conn, row, (size_t)k) != 0 ) {
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
		(void)ta_tls_write(ta_conn, buf, (size_t)n);
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
	ta_io.tmo = TA_DATA_MS;
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
	ta_io.tmo = TA_IDLE_MS;
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

LOCAL void session( void )
{
	char	line[TA_LINE];
	INT	n = 0, c;

	ta_in_at = ta_in_n = 0;
	ta_io.tmo = TA_IDLE_MS;
	step(TA_LINE_WAIT);
	while ( ( c = in_byte() ) >= 0 ) {
		if ( c == '\r' ) continue;
		if ( c != '\n' ) {
			if ( n < TA_LINE - 1 ) line[n++] = (char)c;
			continue;
		}
		line[n] = 0;
		n = 0;
		step("a line");
		if ( !do_line(line) ) {
			break;
		}
		step(TA_LINE_WAIT);
	}
}

/* From a private network only: this port is for the tests on one's own LAN */
LOCAL BOOL private_addr( CONST UB *a )
{
	return (BOOL)( a[0] == 10 || ( a[0] == 172 && ( a[1] & 0xf0 ) == 16 )
		    || ( a[0] == 192 && a[1] == 168 ) || ( a[0] == 169 && a[1] == 254 )
		    || a[0] == 127 );
}

LOCAL char	ta_shown[160];

LOCAL void tell( CONST char *what, CONST UB *a )
{
	if ( a != NULL ) {
		(void)tm_sprintf((UB *)ta_shown, (UB *)"試験用の遠隔操作が有効: %s %d.%d.%d.%d:%d", what, a[0], a[1], a[2], a[3], TA_PORT);
	} else {
		(void)tm_sprintf((UB *)ta_shown, (UB *)"試験用の遠隔操作が有効: %s", what);
	}
	(void)wm_msg_put(ta_shown);
	tm_printf((UB *)"testagent: %s\n", ta_shown);
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

LOCAL void ta_task( INT stacd, void *exinf )
{
	INT	ls, c, err = 0;
	UW	addr = 0, mask = 0, gw = 0;

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
		struct sockaddr_in	peer;
		socklen_t		plen = sizeof(peer);
		CONST UB		*pa = (CONST UB *)&peer.sin_addr;

		step(TA_ACCEPT);
		c = so_accept(ls, (struct sockaddr *)&peer, &plen);
		if ( c < 0 ) {
			tk_dly_tsk(TA_REFUSED_MS);
			continue;
		}
		if ( !private_addr(pa) ) {
			tm_printf((UB *)"testagent: refused %d.%d.%d.%d: not a private address\n", pa[0], pa[1], pa[2], pa[3]);
			(void)so_close(c);
			tk_dly_tsk(TA_REFUSED_MS);
			continue;
		}
		ta_io.s = c;
		ta_io.tmo = TA_HANDSHAKE_MS;
		step("handshake");
		ta_conn = ta_tls_open(io_send, io_recv, &ta_io, &err);
		if ( ta_conn == NULL ) {
			tm_printf((UB *)"testagent: refused %d.%d.%d.%d: handshake -0x%x\n", pa[0], pa[1], pa[2], pa[3], -err);
			(void)so_close(c);
			tk_dly_tsk(TA_REFUSED_MS);
			continue;
		}
		tell("接続", pa);
		session();
		step("closing");
		ta_tls_close(ta_conn);
		ta_conn = NULL;
		(void)so_close(c);
		tell("切断", pa);
		if ( ta_ending ) {
			/* a client waiting for the machine to come back must not find this one on its way out */
			(void)so_close(ls);
			(void)tk_slp_tsk(TMO_FEVR);
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
