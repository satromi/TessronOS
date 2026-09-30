/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	arpa/inet.h
 *	Internet addresses as text.
 */

#ifndef __TSPOSIX_ARPA_INET_H__
#define __TSPOSIX_ARPA_INET_H__

#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

in_addr_t	inet_addr( const char *cp );
int		inet_aton( const char *cp, struct in_addr *in );
char		*inet_ntoa( struct in_addr in );
int		inet_pton( int af, const char *src, void *dst );
const char	*inet_ntop( int af, const void *src, char *dst, socklen_t size );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_ARPA_INET_H__ */
