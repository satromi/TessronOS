/* The one thing libssh2 wants of it, for the C library has none */
#ifndef TS_SSH_SYS_UIO_H
#define TS_SSH_SYS_UIO_H
#include <stddef.h>
struct iovec {
	void	*iov_base;
	size_t	iov_len;
};
#endif
