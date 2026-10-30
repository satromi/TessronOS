/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wps_main.c
 *	wpasupp: the process that runs wpa_supplicant's handshakes, SAE and
 *	their cryptography (OpenSSL's libcrypto) for the kernel's Wi-Fi
 *	manager (design 9.6.11). The kernel's watcher starts it as a process
 *	of the system and gives it the object of their way ("wpasup"); it
 *	says HELLO there, and then answers each CALL it reads (wps_serve.c)
 *	until the way is gone. It keeps no file and opens no other object
 *	but the system's random source.
 *
 *	What the glue asks of the system besides the kernel's callbacks is
 *	here: the time in milliseconds (the system's clock since it
 *	started), random bytes (the object 乱数), and its stop, which ends
 *	the process for the watcher to start again. OpenSSL's seed comes
 *	from the same random bytes (getentropy).
 */

#include <config.h>
#include <stddef.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <tk/syscall.h>
#include <ts/uapp.h>
#include <ts/ob.h>
#include <string.h>
#include <errno.h>
#include "wpaproc.h"

extern int	wps_serve( const unsigned char *in, int n, unsigned char *out );
extern ER	ts_get_random( void *buf, SZ len );
extern void	wps_libc_init( void );

#define CB_WAIT_READS	5	/* reads of a second each a CBRET is waited for */

static ID		chan;
static unsigned char	in[WPP_MAX] __attribute__((aligned(8)));
static unsigned char	out[WPP_MAX] __attribute__((aligned(8)));
static unsigned int	cur_seq;	/* the number of the CALL being answered */

unsigned long long knl_wifi_ms( void )
{
	SYSTIM	t;

	if ( tk_get_otm(&t) < E_OK ) return 0;
	return ( (unsigned long long)(UW)t.hi << 32 ) | (UW)t.lo;
}

void knl_wifi_random( void *buf, int len )
{
	if ( ts_get_random(buf, (SZ)len) < E_OK ) {
		/* no randomness, no keys: ended, for the watcher to start again */
		tm_putstring((const UB *)"wpasupp: the random source cannot be read\n");
		ts_ext_prc(2);
	}
}

void knl_wifi_halt( void )
{
	ts_ext_prc(3);
}

int getentropy( void *buf, size_t len )
{
	if ( len > 256 ) {
		errno = EIO;
		return -1;
	}
	return ( ts_get_random(buf, (SZ)len) >= E_OK ) ? 0 : ( errno = EIO, -1 );
}

static ER put( const unsigned char *b, int n )
{
	SZ	asz = 0;

	((WPP_HDR *)(void *)b)->seq = cur_seq;
	return ob_wri_rec(chan, 1, 0, b, (SZ)n, &asz);
}

/* A CB handed to the kernel and its CBRET read back (its length; -1: none) */
int wps_p_xchg( unsigned char *buf, int len )
{
	WPP_R	r;
	SZ	asz;
	ER	er;
	int	i;

	if ( put(buf, len) < E_OK ) return -1;
	for ( i = 0; i < CB_WAIT_READS; i++ ) {
		asz = 0;
		er = ob_rea_rec(chan, 1, 0, buf, WPP_MAX, &asz);
		if ( er == E_TMOUT ) continue;
		if ( er < E_OK ) return -1;
		if ( wpp_r_begin(&r, buf, (int)asz, WPP_CBRET) != NULL ) return (int)asz;
		return -1;
	}
	return -1;
}

int main( void )
{
	WPP_ARG	arg;
	WPP_W	w;
	WPP_R	r;
	const WPP_HDR *h;
	SZ	n, asz;
	ER	er;
	int	m;

	wps_libc_init();
	memset(&arg, 0, sizeof(arg));
	n = ts_get_arg(&arg, sizeof(arg));
	if ( n < (SZ)sizeof(arg) || arg.magic != WPP_MAGIC || arg.version != WPP_VERSION ) {
		tm_putstring((const UB *)"wpasupp: started without the kernel's way\n");
		return 1;
	}
	chan = ob_opn_obj((const TS_UUID *)(const void *)arg.chan, OB_OP_READ | OB_OP_WRITE);
	if ( chan < E_OK ) {
		tm_putstring((const UB *)"wpasupp: the kernel's way cannot be opened\n");
		return 1;
	}
	wpp_w_begin(&w, out, WPP_HELLO, 0, WPP_VERSION);
	m = wpp_w_end(&w);
	cur_seq = 0;
	if ( put(out, m) < E_OK ) return 1;
	for ( ;; ) {
		asz = 0;
		er = ob_rea_rec(chan, 1, 0, in, WPP_MAX, &asz);
		if ( er == E_TMOUT ) continue;
		if ( er < E_OK ) break;			/* the way is gone */
		h = wpp_r_begin(&r, in, (int)asz, WPP_CALL);
		if ( h == NULL ) continue;		/* a CBRET of a CB that gave up */
		cur_seq = h->seq;
		m = wps_serve(in, (int)asz, out);
		if ( m <= 0 ) {
			/* a CALL that cannot be taken apart: failed, so that the kernel does not wait */
			wpp_w_begin(&w, out, WPP_RET, h->op, -1);
			m = wpp_w_end(&w);
		}
		wpp_wipe(in, sizeof(in));
		(void)put(out, m);
		wpp_wipe(out, sizeof(out));
	}
	return 0;
}
