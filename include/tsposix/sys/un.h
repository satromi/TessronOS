/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/un.h
 *	Local sockets' addresses; such a socket cannot be made (EAFNOSUPPORT).
 */

#ifndef __TSPOSIX_SYS_UN_H__
#define __TSPOSIX_SYS_UN_H__

#include <sys/socket.h>

struct sockaddr_un {
	sa_family_t	sun_family;
	char		sun_path[108];
};

#endif /* __TSPOSIX_SYS_UN_H__ */
