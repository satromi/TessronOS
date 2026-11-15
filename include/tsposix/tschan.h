/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tschan.h
 *	Local sockets between processes, and descriptors as keys to real
 *	objects (lib/libcxxrt/ts_chan.c, design 18.8, 17.19).
 *
 *	socketpair(AF_UNIX, SOCK_STREAM) is a pair of stream objects
 *	(OB_S_STREAM of include/ts/ob.h). read, write, send, recv, sendmsg,
 *	recvmsg, shutdown, close, fcntl (O_NONBLOCK, FD_CLOEXEC), poll and
 *	epoll reach its descriptors as Linux's; SCM_RIGHTS carries
 *	descriptors that are keys to objects -- the ends, memory objects of
 *	memfd_create, PRIME descriptors of the GPU, and any descriptor made
 *	from a key -- and the one who receives gets keys of its own, with
 *	no more rights than the sender had. A descriptor of a file or a
 *	network socket cannot be carried (EBADF).
 *
 *	What Linux does by fork and inheritance is done here by hand: a
 *	process hands a child the descriptors with ts_chan_give once it has
 *	started it, and the child takes them with ts_chan_take; or the
 *	parent names an end by its UUID (ts_chan_uuid, in the child's
 *	start-up argument) and the child opens it (ts_chan_open), keeping it
 *	open until the child has.
 */

#ifndef __TSPOSIX_TSCHAN_H__
#define __TSPOSIX_TSCHAN_H__

#ifdef __cplusplus
extern "C" {
#endif

/* A descriptor's key to its object (> 0), or -1; the key stays the descriptor's */
int	ts_fd_key( int fd );

/* A descriptor made from a key of the process's, of the kind (OB_T_*, OB_S_*) it names; 0, 0: any */
int	ts_fd_from_key( int key, unsigned type, unsigned sub );

/* The descriptors handed to child pid (keys of its own), and taken by it, waiting up to ms (-1 no end) */
int	ts_chan_give( int pid, const int *fds, int n );
int	ts_chan_take( int *fds, int max, int ms );

/* The UUID of the end a descriptor holds, and an end opened by its UUID */
int	ts_chan_uuid( int fd, unsigned char uuid[16] );
int	ts_chan_open( const unsigned char uuid[16] );

/* Whether fd is an end of a stream or a key of this library's */
int	ts_chan_is( int fd );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_TSCHAN_H__ */
