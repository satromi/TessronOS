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
IMPORT int	rump_tsnet_defroute4( UW gw );
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

/* ---------------------------------------------------------------- so_dhcp.c */

IMPORT ER	knl_so_dhcp_run( void );	/* start asking, or ask again */
IMPORT void	knl_so_dhcp_stop( void );	/* stop asking; the address stays */

/* ---------------------------------------------------------------- so_dns.c */

/* The name server to ask (network byte order), 0 for none */
IMPORT UW	knl_so_dns_server( INT n );
IMPORT ER	knl_so_dns_query( CONST char *name, INT family, UW *addrs, INT max, INT *p_n );

#endif /* __SO_NB_H__ */
