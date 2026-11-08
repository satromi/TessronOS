/*
 * The private keys the client uses come from memory, never from a file
 * (lib/libsftp), and the Mbed TLS of the programs is built without files.
 * libssh2 refers to the file loader all the same; it fails, should it
 * ever be reached.
 */
#ifndef TS_SSH_SHIM_H
#define TS_SSH_SHIM_H
#define mbedtls_pk_parse_keyfile(ctx, path, pwd, rng, rngctx) ( (void)(path), (void)(pwd), -1 )
#define mbedtls_pk_parse_public_keyfile(ctx, path) ( (void)(path), -1 )
/*
 * Where libssh2 waits (lib/libsftp/sftp.c): the wait of its connection, not
 * the select of the C library, which knows nothing of the system's sockets
 */
#define select	ts_ssh_select

/* the default send and receive of libssh2 are replaced by callbacks before any use */
#ifndef TS_SSH_HOST
#define recv(s, b, l, f)	( (void)(s), (void)(b), (void)(l), (void)(f), -1 )
#define send(s, b, l, f)	( (void)(s), (void)(b), (void)(l), (void)(f), -1 )
#endif
#endif
