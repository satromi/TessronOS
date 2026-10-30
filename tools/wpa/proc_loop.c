/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	proc_loop.c
 *	The way between the kernel's side and the process's side of the
 *	messages of wpaproc.h for the host's test (tools/wpa/proc.sh test):
 *	both sides in one program. A CALL the kernel's side hands over is
 *	answered there and then by the process's side (wps_serve); a CB the
 *	process's side makes while it answers is carried out by the
 *	kernel's side (wpp_cb_run) and its CBRET given back. Every message
 *	goes through the same making and taking apart as in the system,
 *	and each is checked as it passes.
 */

#include <stdio.h>
#include <string.h>
#include "wpaproc.h"

extern int	wps_serve( const unsigned char *in, int n, unsigned char *out );
extern int	wpp_cb_run( const unsigned char *in, int n, unsigned char *out );

static unsigned char	up[WPP_MAX], cbret[WPP_MAX];
static int		in_call;
long			wpp_loop_calls, wpp_loop_cbs;

int wpp_k_begin( void )
{
	if ( in_call ) {
		printf("test: a call inside a call\n");
		return -1;
	}
	in_call = 1;
	return 0;
}

void wpp_k_end( void )
{
	in_call = 0;
}

int wpp_k_xchg( unsigned char *buf, int len )
{
	WPP_R	r;
	int	n;

	if ( wpp_r_begin(&r, buf, len, WPP_CALL) == NULL ) {
		printf("test: a CALL that is not one\n");
		return -1;
	}
	wpp_loop_calls++;
	n = wps_serve(buf, len, up);
	if ( n <= 0 || wpp_r_begin(&r, up, n, WPP_RET) == NULL ) {
		printf("test: no RET for operation %d\n", ((WPP_HDR *)(void *)buf)->op);
		return -1;
	}
	memcpy(buf, up, n);
	return n;
}

int wps_p_xchg( unsigned char *buf, int len )
{
	WPP_R	r;
	int	n;

	if ( wpp_r_begin(&r, buf, len, WPP_CB) == NULL ) {
		printf("test: a CB that is not one\n");
		return -1;
	}
	wpp_loop_cbs++;
	n = wpp_cb_run(buf, len, cbret);
	if ( n <= 0 || wpp_r_begin(&r, cbret, n, WPP_CBRET) == NULL ) return -1;
	memcpy(buf, cbret, n);
	return n;
}

/* How many messages went through, said when the test ends */
__attribute__((destructor)) static void said( void )
{
	printf("test: %ld calls and %ld callbacks went through the messages\n", wpp_loop_calls, wpp_loop_cbs);
}
