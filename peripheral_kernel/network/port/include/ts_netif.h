/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_netif.h
 *	The interface between the stack and the card (design 12.6).
 */

#ifndef __TS_NETIF_H__
#define __TS_NETIF_H__

#include "lwip/err.h"

struct netif;

IMPORT err_t ts_netif_init( struct netif *netif );
IMPORT BOOL  ts_netif_link_up( void );

#endif /* __TS_NETIF_H__ */
