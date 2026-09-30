/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	poll.h
 *	Waiting for descriptors: sockets through so_poll; a file is always
 *	ready.
 */

#ifndef __TSPOSIX_POLL_H__
#define __TSPOSIX_POLL_H__

#ifdef __cplusplus
extern "C" {
#endif

struct pollfd {
	int	fd;
	short	events;
	short	revents;
};

typedef unsigned long nfds_t;

#define POLLIN		0x001
#define POLLPRI		0x002
#define POLLOUT		0x004
#define POLLERR		0x008
#define POLLHUP		0x010
#define POLLNVAL	0x020
#define POLLRDNORM	0x040
#define POLLRDBAND	0x080
#define POLLWRNORM	0x100
#define POLLWRBAND	0x200
#define POLLRDHUP	0x2000

int	poll( struct pollfd *fds, nfds_t nfds, int timeout );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_POLL_H__ */
