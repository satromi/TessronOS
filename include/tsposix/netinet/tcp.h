/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netinet/tcp.h
 *	The options of a TCP socket.
 */

#ifndef __TSPOSIX_NETINET_TCP_H__
#define __TSPOSIX_NETINET_TCP_H__

#define TCP_NODELAY	1
#define TCP_MAXSEG	2
#define TCP_KEEPIDLE	4
#define TCP_KEEPINTVL	5
#define TCP_KEEPCNT	6
#define TCP_INFO	11
#define TCP_QUICKACK	12
#define TCP_USER_TIMEOUT 18
#define TCP_FASTOPEN	23

#endif /* __TSPOSIX_NETINET_TCP_H__ */
