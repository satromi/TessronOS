/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netlog.h
 *	The network's lines on the console with the time since the start
 *	(design 9.6.6): included by the Wi-Fi manager (wifi.c), the
 *	adapters' manager (netadp.c) and DHCP (so_dhcp.c), it turns their
 *	tm_printf and tm_putstring into ones that put the milliseconds since
 *	the start before a line that begins with "wifi:" or "net:"
 *	("12345.678 wifi: ..."), so that the records of a machine show
 *	how far apart the steps of a join, the scans and DHCP's messages
 *	were. The time and the line go out under the console's lock in one
 *	piece. Other lines are as they were.
 */

#ifndef NETLOG_H
#define NETLOG_H

#include <tm/tmonitor.h>

IMPORT void	knl_net_stamp( CONST void *line );	/* so_api.c */
IMPORT INT	knl_net_stamp_end( void );

#define tm_printf(f, ...)	( knl_net_stamp((f)), (void)tm_printf((f), ##__VA_ARGS__), knl_net_stamp_end() )
#define tm_putstring(s)		( knl_net_stamp((s)), (void)tm_putstring((s)), knl_net_stamp_end() )

#endif /* NETLOG_H */
