/*
 * What libssh2 would have found out by configure, for the programs of
 * TessronOS. The library is given no socket of the system to work on:
 * all its bytes go through the send and receive callbacks (lib/libsftp),
 * which wait until the bytes have gone, so nothing here asks about
 * blocking or select.
 */
#ifndef TS_LIBSSH2_CONFIG_H
#define TS_LIBSSH2_CONFIG_H

#define HAVE_INTTYPES_H 1
#define HAVE_STDLIB_H 1
#define HAVE_UNISTD_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_STRTOLL 1
#define HAVE_SNPRINTF 1
#define HAVE_LONGLONG 1
#define HAVE_SYS_UIO_H 1
#define LIBSSH2_DH_GEX_NEW 1

#ifdef TS_SSH_HOST
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SELECT 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_ARPA_INET_H 1
#endif

#endif
