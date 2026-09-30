/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_sntp.c
 *	The system clock from the network (SNTP, RFC 4330; design 12.6).
 *
 *	A board without a battery for its RTC (the Raspberry Pi 5 as sold)
 *	starts its clock at 1985 on every power up, and until the clock is
 *	set no UUID and so no object can be made (design 11.4). Once the
 *	interface has an address, a task of its own asks a time server
 *	(CNF_NTP_SERVER, looked up through the DNS server that came with the
 *	address) and sets the system clock from the answer, which also
 *	writes the RTC. It asks again every hour to stay close; a failure is
 *	tried again after a short wait.
 *
 *	The server is CNF_NTP_SERVER until the network's settings name
 *	another, or none (so_sntp_server, from so_conf.c); a server named
 *	anew is asked at once. CNF_NET_SNTP starts the task with the
 *	network; otherwise the settings start it.
 *
 *	One request, one answer: the server's transmit time, plus half the
 *	time the round trip took. Answers that are not a server's, that
 *	come from a server that has lost its own time (stratum 0, the alarm
 *	in the leap indicator) or that say a time before 2020 are refused.
 */

#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/so.h>

#define NTP_PORT		123
#define NTP_PACKET		48
#define NTP_UNIX_OFFSET		2208988800ULL	/* seconds from 1900 to 1970 */
#define UNIX_2020_S		1577836800ULL
#define TRON_UNIX_MS		473385600000LL	/* ms from 1970 to 1985 */

#define SNTP_TASK_PRI		30
#define SNTP_TASK_STKSZ		(8 * 1024)
#define SNTP_ANSWER_MS		3000		/* how long one answer is waited for */
#define SNTP_RETRY_MS		15000		/* after a failure */
#define SNTP_RESYNC_MS		3600000		/* after a success */
#define SNTP_ADDR_POLL_MS	1000		/* while the interface has no address */

#define SNTP_IDLE_MS		60000		/* with no server named, how often to look again */

LOCAL BOOL	sntp_synced = FALSE;
LOCAL D		sntp_last_ms = 0;		/* the Unix time of the last setting, ms */
LOCAL char	sntp_host[80] = CNF_NTP_SERVER;	/* "" asks none */
LOCAL ID	sntp_tskid = 0;

/* The system clock as Unix time in ms */
LOCAL D unix_now_ms( void )
{
	SYSTIM	t;

	if ( tk_get_tim(&t) < E_OK ) {
		return 0;
	}
	return ( ( (D)t.hi << 32 ) | t.lo ) + TRON_UNIX_MS;
}

LOCAL UW be32( CONST UB *p )
{
	return ( (UW)p[0] << 24 ) | ( (UW)p[1] << 16 ) | ( (UW)p[2] << 8 ) | p[3];
}

/*
 * Ask the server once. Answers the Unix time in ms at the moment the
 * answer arrived, or an error.
 */
LOCAL ER sntp_ask( CONST struct sockaddr_in *srv, D *p_ms )
{
	UB		pkt[NTP_PACKET];
	fd_set		rd;
	SYSTIM		t0, t1;
	INT		s, n, i;
	UD		sec, frac;
	D		ms, rtt;
	ER		er = E_TMOUT;

	s = so_socket(AF_INET, SOCK_DGRAM, 0);
	if ( s < 0 ) {
		return (ER)s;
	}
	for ( i = 0; i < NTP_PACKET; i++ ) pkt[i] = 0;
	pkt[0] = ( 0 << 6 ) | ( 4 << 3 ) | 3;	/* no leap warning, version 4, a client */

	(void)tk_get_otm(&t0);
	if ( so_sendto(s, pkt, NTP_PACKET, 0, (CONST struct sockaddr *)srv, sizeof(*srv)) != NTP_PACKET ) {
		so_close(s);
		return E_IO;
	}
	FD_ZERO(&rd);
	FD_SET(s, &rd);
	if ( so_select(s + 1, &rd, NULL, NULL, SNTP_ANSWER_MS) == 1 ) {
		n = so_recv(s, pkt, NTP_PACKET, 0);
		(void)tk_get_otm(&t1);
		if ( n < NTP_PACKET ) {
			er = E_IO;
		} else if ( ( pkt[0] & 7 ) != 4 || ( pkt[0] >> 6 ) == 3
			 || pkt[1] == 0 || pkt[1] > 15 ) {
			er = E_OBJ;		/* not a server's answer, or one without time */
		} else {
			sec  = be32(&pkt[40]);	/* the transmit time, from 1900 */
			frac = be32(&pkt[44]);
			if ( sec < NTP_UNIX_OFFSET + UNIX_2020_S ) {
				er = E_OBJ;
			} else {
				rtt = ( ( (D)t1.hi << 32 ) | t1.lo ) - ( ( (D)t0.hi << 32 ) | t0.lo );
				ms = (D)( sec - NTP_UNIX_OFFSET ) * 1000 + (D)( ( frac * 1000 ) >> 32 );
				*p_ms = ms + rtt / 2;
				er = E_OK;
			}
		}
	}
	so_close(s);
	return er;
}

/* The server by name, through DNS */
LOCAL ER sntp_server( struct sockaddr_in *srv )
{
	struct addrinfo	hints, *res = NULL;
	INT		i;
	ER		er;

	for ( i = 0; i < (INT)sizeof(hints); i++ ) ((UB *)&hints)[i] = 0;
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if ( sntp_host[0] == '\0' ) {
		return E_NOEXS;
	}
	er = so_getaddrinfo(sntp_host, "123", &hints, &res);
	if ( er < E_OK || res == NULL ) {
		return ( er < E_OK ) ? er : E_NOEXS;
	}
	*srv = *(struct sockaddr_in *)res->ai_addr;
	srv->sin_port = lwip_htons(NTP_PORT);
	so_freeaddrinfo(res);
	return E_OK;
}

/* Set the system clock (and through it the RTC) to Unix time 'ms' */
LOCAL void sntp_set( D ms, CONST struct sockaddr_in *srv )
{
	SYSTIM	t;
	D	was = unix_now_ms();
	D	tron = ms - TRON_UNIX_MS;
	UW	a = lwip_ntohl(srv->sin_addr.s_addr);

	t.hi = (W)( tron >> 32 );
	t.lo = (UW)tron;
	if ( tk_set_tim(&t) < E_OK ) {
		return;
	}
	sntp_synced = TRUE;
	sntp_last_ms = ms;
	tm_printf((UB *)"sntp: the clock set from %d.%d.%d.%d: Unix %d s, it was %d ms off\n",
		  (INT)( a >> 24 ), (INT)( ( a >> 16 ) & 0xff ), (INT)( ( a >> 8 ) & 0xff ),
		  (INT)( a & 0xff ), (INT)( ms / 1000 ), (INT)( ms - was ));
}

LOCAL void sntp_task( INT stacd, void *exinf )
{
	struct sockaddr_in	srv;
	UW			addr;
	D			ms;
	ER			er;

	for (;;) {
		if ( sntp_host[0] == '\0' ) {
			tk_dly_tsk(SNTP_IDLE_MS);	/* none named; woken when one is */
			continue;
		}
		/* an address first, and with it the DNS server */
		addr = 0;
		if ( so_getifaddr(&addr, NULL, NULL) < E_OK || addr == 0 ) {
			tk_dly_tsk(SNTP_ADDR_POLL_MS);
			continue;
		}
		er = sntp_server(&srv);
		if ( er >= E_OK ) {
			er = sntp_ask(&srv, &ms);
		}
		if ( er >= E_OK ) {
			sntp_set(ms, &srv);
			tk_dly_tsk(SNTP_RESYNC_MS);
		} else {
			tk_dly_tsk(SNTP_RETRY_MS);
		}
	}
}

EXPORT ER so_sntp_start( void )
{
	T_CTSK	ctsk;
	ID	tskid;

	if ( sntp_tskid > 0 ) {
		return E_OK;
	}
	ctsk.exinf   = NULL;
	ctsk.tskatr  = TA_HLNG | TA_RNG0;
	ctsk.task    = (FP)sntp_task;
	ctsk.itskpri = SNTP_TASK_PRI;
	ctsk.stksz   = SNTP_TASK_STKSZ;
	tskid = tk_cre_tsk(&ctsk);
	if ( tskid <= 0 ) {
		return (ER)tskid;
	}
	sntp_tskid = tskid;
	return tk_sta_tsk(tskid, 0);
}

/*
 * The time server by name, "" for none. A server named is asked at once:
 * the task is started, or woken from its wait.
 */
EXPORT ER so_sntp_server( CONST char *name )
{
	INT	i;

	for ( i = 0; name != NULL && name[i] != '\0' && i < (INT)sizeof(sntp_host) - 1; i++ ) {
		sntp_host[i] = name[i];
	}
	sntp_host[i] = '\0';
	if ( sntp_host[0] == '\0' ) {
		return E_OK;
	}
	if ( sntp_tskid <= 0 ) {
		return so_sntp_start();
	}
	(void)tk_rel_wai(sntp_tskid);
	return E_OK;
}

EXPORT ER so_sntp_state( D *p_unix_ms )
{
	if ( !sntp_synced ) {
		return E_OBJ;
	}
	if ( p_unix_ms != NULL ) {
		*p_unix_ms = sntp_last_ms;
	}
	return E_OK;
}


