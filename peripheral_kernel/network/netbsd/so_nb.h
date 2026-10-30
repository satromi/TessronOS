/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	so_nb.h
 *	Inside the so_ calls over the NetBSD stack (design 12.6)
 *
 *	so_api.c carries the calls, so_dhcp.c asks the network for an
 *	address and so_dns.c looks names up. This is what they share.
 */

#ifndef __SO_NB_H__
#define __SO_NB_H__

#include <ts/so.h>

#define SO_IFNAME	"neta0"		/* the card's interface in the stack */

/* ---------------------------------------------------------------- the rump kernel's helpers (rumpcomp/tsnet_conf.c) */

IMPORT int	rump_tsnet_setaddr4( CONST char *ifname, UW addr, UW mask, UW bcast );
IMPORT int	rump_tsnet_getaddr4( CONST char *ifname, UW *addr, UW *mask );
/* the address addr's check against the other machines: 0 usable, 1 checking, 2 another's, 3 link down, -1 none */
IMPORT int	rump_tsnet_addrstate4( CONST char *ifname, UW addr );
IMPORT int	rump_tsnet_defroute4( UW gw );

/* The IPv6 addresses of an interface (T_SOIF6 has the layout of the kernel's list) */
IMPORT int	rump_tsnet_addrs6( CONST char *ifname, T_SOIF6 *list, int max );
IMPORT int	rump_tsnet_setaddr6( CONST char *ifname, CONST UB *addr, int plen, UINT vltime, UINT pltime,
				     UINT flags );		/* SO_IF6_AUTOCONF, SO_IF6_TEMPORARY */
IMPORT int	rump_tsnet_ip6on( CONST char *ifname, int on );
IMPORT void	rump_tsnet_prefer_temp( int yes );
IMPORT int	rump_tsnet_deladdr6( CONST char *ifname, CONST UB *addr );
IMPORT int	rump_tsnet_defroute6( CONST char *ifname, CONST UB *gw );
IMPORT int	rump_tsnet_cmsg_hlim( void *ctl, UINT len );
IMPORT int	rump_tsnet_cmsg_tos( void *ctl, UINT len );
IMPORT int	rump_tsnet_ifindex( CONST char *ifname );
IMPORT int	rump_tsnet_ifname( int idx, char *buf );
IMPORT int	rump_tsnet_ifup( CONST char *ifname, int up );
IMPORT int	rump_tsnet_fionread( int fd, int *avail );
IMPORT void	*rump_tsnet_lwp_make( void );
IMPORT void	rump_lwproc_curlwp_set( void *l );
IMPORT void	rump_lwproc_curlwp_clear( void *l );
IMPORT void	rump_tsnet_host( void );
IMPORT void	rump_boot_setsigmodel( int model );

/* ---------------------------------------------------------------- so_api.c */

IMPORT BOOL	knl_so_nb_card;		/* the stack has the card's interface */
IMPORT ER	knl_so_nb_er( INT nberr );	/* a NetBSD errno value as an error code */

/*
 * The interface's address: addr, mask and gateway, all in network byte
 * order; an addr of 0 with 'dhcp' leaves it ready for DHCP's broadcasts.
 */
IMPORT ER	knl_so_nb_ifaddr( UW addr, UW mask, UW gw, BOOL dhcp );

/* The name servers the interface came with (DHCP, or a fixed address) */
IMPORT void	knl_so_nb_ifdns( UW dns0, UW dns1 );

/*
 * A receive on a socket that may not block, with the address it came
 * from and the hop limit it arrived with (-1 when the socket was not
 * asked for it, IPV6_RECVHOPLIMIT): the bytes, or an error code.
 */
IMPORT INT	knl_so_nb_recv_hlim( INT s, void *mem, SZ size, struct sockaddr *from,
				     socklen_t *fromlen, INT *p_hlim );

/* An IPv6 address as text, the longest run of zero groups shortened (RFC 5952) */
IMPORT void	knl_so_ip6_text( char *buf, SZ len, CONST UB *a );

/* ---------------------------------------------------------------- so_dhcp.c */

IMPORT ER	knl_so_dhcp_run( void );	/* start asking, or ask again */
IMPORT void	knl_so_dhcp_stop( void );	/* stop asking; the address stays */

/* ---------------------------------------------------------------- so_slaac.c */

IMPORT ER	knl_so_slaac_run( void );	/* listen to the routers, and ask them */
/*
 * Stop listening, and wait until it has. With 'forget' the addresses
 * made, the default route and the name servers the routers named go as
 * well; without, they stay until their lifetimes run out.
 */
IMPORT void	knl_so_slaac_stop( BOOL forget );
IMPORT void	knl_so_slaac_relink( void );	/* the link came up again: the routers asked again */
IMPORT UINT	knl_so_slaac_solicits( void );	/* the router solicitations sent so far */
IMPORT void	knl_so_slaac_temp( BOOL on );	/* temporary addresses as well (RFC 8981), or not */
/* The nth IPv6 name server the routers named, and the router the default route goes through */
IMPORT BOOL	knl_so_slaac_dns( INT n, UB *addr );
IMPORT BOOL	knl_so_slaac_gw( UB *addr );

/* The router the IPv6 default route goes through, as the settings make it (so_api.c) */
IMPORT BOOL	knl_so_gw6( UB *gw );

/* ---------------------------------------------------------------- so_dns.c */

/* The IPv4 name server to ask (network byte order), 0 for none */
IMPORT UW	knl_so_dns_server( INT n );
/*
 * The addresses of a name: family AF_INET (A records), AF_INET6
 * (AAAA) or AF_UNSPEC (both, asked at once); up to max of them, the
 * IPv4 ones first.
 */
IMPORT ER	knl_so_dns_query( CONST char *name, INT family, T_SOADDR *addrs, INT max, INT *p_n );

#endif /* __SO_NB_H__ */
