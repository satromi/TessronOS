/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	net/if.h
 *	Network interfaces by name and index. The sockets of TessronOS
 *	(include/ts/soapp.h) do not name their interfaces: no name has an
 *	index and no index a name (lib/libcxxrt/ts_posix.c).
 */

#ifndef __TSPOSIX_NET_IF_H__
#define __TSPOSIX_NET_IF_H__

#ifdef __cplusplus
extern "C" {
#endif

#define IF_NAMESIZE	16
#define IFNAMSIZ	IF_NAMESIZE

#define IFF_UP		0x1
#define IFF_BROADCAST	0x2
#define IFF_LOOPBACK	0x8
#define IFF_RUNNING	0x40
#define IFF_MULTICAST	0x1000

unsigned int	if_nametoindex( const char *name );
char		*if_indextoname( unsigned int index, char *name );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_NET_IF_H__ */
