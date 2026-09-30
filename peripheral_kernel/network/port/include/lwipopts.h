/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	lwipopts.h
 *	How lwIP is built for TessronOS (design 12.6).
 *
 *	The stack runs with its own thread (NO_SYS is 0), so the sockets and
 *	the netconn layers are available and `so_` maps onto them. IPv4 only
 *	for now, over Ethernet, with the loopback interface turned on so
 *	that the stack can be exercised on a machine with nothing plugged
 *	in.
 *
 *	There is no C library underneath, so the pieces lwIP would take from
 *	one are switched off here and provided by the port instead.
 */

#ifndef __TS_LWIPOPTS_H__
#define __TS_LWIPOPTS_H__

/* ------------------------------------------------ what the platform has */
#define NO_SYS				0
#define SYS_LIGHTWEIGHT_PROT		1
#define LWIP_PROVIDE_ERRNO		1	/* no errno.h here */
#define LWIP_NO_INTTYPES_H		1	/* cc.h gives the formatters */
#define LWIP_NO_UNISTD_H		1
#define LWIP_NO_CTYPE_H			1
#define MEM_LIBC_MALLOC			0	/* lwIP keeps its own heap */
#define MEMP_MEM_MALLOC			0
#define LWIP_TIMEVAL_PRIVATE		1	/* lwIP defines struct timeval */

/* ------------------------------------------------ memory */
/*
 * Sockets and TCP connections: 64 each (the socket numbers of libcxxrt's
 * POSIX layer run 0 .. 63 too). A browser opens many connections at once;
 * closed ones in TIME_WAIT hold a TCP control block until lwIP reuses it.
 */
#define MEM_ALIGNMENT			8	/* LP64 */
#define MEM_SIZE			(512 * 1024)
#define MEMP_NUM_PBUF			64
#define MEMP_NUM_UDP_PCB		16
#define MEMP_NUM_TCP_PCB		64
#define MEMP_NUM_TCP_PCB_LISTEN		4
#define MEMP_NUM_TCP_SEG		256
#define MEMP_NUM_NETBUF			32
#define MEMP_NUM_NETCONN		64
#define MEMP_NUM_TCPIP_MSG_API		32
#define MEMP_NUM_TCPIP_MSG_INPKT	32
#define MEMP_NUM_SYS_TIMEOUT		12
#define PBUF_POOL_SIZE			64
#define PBUF_POOL_BUFSIZE		1536	/* a whole frame */

/* ------------------------------------------------ protocols */
#define LWIP_IPV4			1
#define LWIP_IPV6			0
#define LWIP_ARP			1
#define LWIP_ETHERNET			1
#define LWIP_ICMP			1
#define LWIP_RAW			1
#define LWIP_UDP			1
#define LWIP_TCP			1
#define LWIP_DHCP			1
#define LWIP_DNS			1
#define LWIP_IGMP			0
#define LWIP_AUTOIP			0

#define TCP_MSS				1460
#define TCP_SND_BUF			(4 * TCP_MSS)
#define TCP_WND				(4 * TCP_MSS)
#define TCP_SND_QUEUELEN		((4 * TCP_SND_BUF) / TCP_MSS)

/* ------------------------------------------------ interfaces */
#define LWIP_NETIF_LOOPBACK		1
#define LWIP_HAVE_LOOPIF		1
#define LWIP_LOOPBACK_MAX_PBUFS		8
#define LWIP_NETIF_HOSTNAME		1
#define LWIP_NETIF_STATUS_CALLBACK	1
#define LWIP_NETIF_LINK_CALLBACK	1
#define LWIP_SINGLE_NETIF		0

/* ------------------------------------------------ the API */
#define LWIP_NETCONN			1
#define LWIP_NETIF_API			1	/* naming the interfaces by index */
#define LWIP_SOCKET			1
#define LWIP_COMPAT_SOCKETS		0	/* the names stay lwip_* */
#define LWIP_POSIX_SOCKETS_IO_NAMES	0
#define LWIP_SOCKET_OFFSET		0
#define LWIP_TCP_KEEPALIVE		1
#define LWIP_SO_RCVTIMEO		1
#define LWIP_SO_SNDTIMEO		1
#define LWIP_SO_RCVBUF			1
#define SO_REUSE			1
#define LWIP_NETBUF_RECVINFO		0

/* ------------------------------------------------ threads */
#define TCPIP_THREAD_NAME		"tcpip"
#define TCPIP_THREAD_STACKSIZE		(16 * 1024)
#define TCPIP_THREAD_PRIO		10
#define TCPIP_MBOX_SIZE			16
#define DEFAULT_THREAD_STACKSIZE	(8 * 1024)
#define DEFAULT_THREAD_PRIO		12
#define DEFAULT_UDP_RECVMBOX_SIZE	8
#define DEFAULT_TCP_RECVMBOX_SIZE	8
#define DEFAULT_RAW_RECVMBOX_SIZE	8
#define DEFAULT_ACCEPTMBOX_SIZE		8

/* ------------------------------------------------ checksums and statistics */
#define CHECKSUM_GEN_IP			1
#define CHECKSUM_GEN_UDP		1
#define CHECKSUM_GEN_TCP		1
#define CHECKSUM_CHECK_IP		1
#define CHECKSUM_CHECK_UDP		1
#define CHECKSUM_CHECK_TCP		1
#define LWIP_STATS			1
#define LWIP_STATS_DISPLAY		0

/* ------------------------------------------------ randomness */
extern unsigned int ts_lwip_rand( void );
#define LWIP_RAND()			((u32_t)ts_lwip_rand())

/* ------------------------------------------------ what to print */
#define LWIP_DEBUG			0

#endif /* __TS_LWIPOPTS_H__ */
