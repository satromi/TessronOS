/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	netdb.h
 *	Names of hosts: getaddrinfo asks the system's resolver (so_resolve)
 *	for the first IPv4 address of a name.
 */

#ifndef __TSPOSIX_NETDB_H__
#define __TSPOSIX_NETDB_H__

#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

struct addrinfo {
	int		ai_flags;
	int		ai_family;
	int		ai_socktype;
	int		ai_protocol;
	socklen_t	ai_addrlen;
	struct sockaddr	*ai_addr;
	char		*ai_canonname;
	struct addrinfo	*ai_next;
};

struct hostent {
	char	*h_name;
	char	**h_aliases;
	int	h_addrtype;
	int	h_length;
	char	**h_addr_list;
};
#define h_addr	h_addr_list[0]

#define AI_PASSIVE	0x0001
#define AI_CANONNAME	0x0002
#define AI_NUMERICHOST	0x0004
#define AI_V4MAPPED	0x0008
#define AI_ALL		0x0010
#define AI_ADDRCONFIG	0x0020
#define AI_NUMERICSERV	0x0400

#define NI_NUMERICHOST	0x01
#define NI_NUMERICSERV	0x02
#define NI_NOFQDN	0x04
#define NI_NAMEREQD	0x08
#define NI_DGRAM	0x10
#define NI_MAXHOST	1025
#define NI_MAXSERV	32

#define EAI_BADFLAGS	-1
#define EAI_NONAME	-2
#define EAI_AGAIN	-3
#define EAI_FAIL	-4
#define EAI_FAMILY	-6
#define EAI_SOCKTYPE	-7
#define EAI_SERVICE	-8
#define EAI_MEMORY	-10
#define EAI_SYSTEM	-11
#define EAI_OVERFLOW	-12
#define EAI_NODATA	-5
#define EAI_ADDRFAMILY	-9

int		getaddrinfo( const char *node, const char *service,
			     const struct addrinfo *hints, struct addrinfo **res );
void		freeaddrinfo( struct addrinfo *res );
const char	*gai_strerror( int err );
int		getnameinfo( const struct sockaddr *sa, socklen_t salen, char *host,
			     socklen_t hostlen, char *serv, socklen_t servlen, int flags );
struct hostent	*gethostbyname( const char *name );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_NETDB_H__ */
