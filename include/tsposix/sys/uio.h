/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	sys/uio.h
 *	Scattered reads and writes (readv, writev).
 */

#ifndef __TSPOSIX_SYS_UIO_H__
#define __TSPOSIX_SYS_UIO_H__

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct iovec {
	void	*iov_base;
	size_t	iov_len;
};

#define IOV_MAX	1024

ssize_t	readv( int fd, const struct iovec *iov, int n );
ssize_t	writev( int fd, const struct iovec *iov, int n );

#ifdef __cplusplus
}
#endif

#endif /* __TSPOSIX_SYS_UIO_H__ */
