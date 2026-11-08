/*
 * lib/libsftp on the host, against a real sshd (tools/ssh/test_host.sh):
 * the key exchange, a password, SFTP in both directions, one
 * command, a wrong password refused.
 *   test_host PORT USER PASS DIR
 */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/ftp.h>
#include <ts/sftp.h>

static int fails;
#define CK(c) do { if ( !(c) ) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* what the Mbed TLS of the programs asks of the system (lib/libtls/tls.c on the real thing) */
int mbedtls_hardware_poll(void *d, unsigned char *o, size_t l, size_t *n)
{
	FILE *f = fopen("/dev/urandom", "rb");
	(void)d;
	*n = fread(o, 1, l, f);
	fclose(f);
	return 0;
}
#include <time.h>
long long ts_tls_time(long long *t) { long long v = time(NULL); if ( t ) *t = v; return v; }
long long mbedtls_ms_time(void) { return (long long)time(NULL) * 1000; }

static INT n_open(void *ctx, const char *host, UINT port)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	(void)ctx;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr(host);
	return connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 ? s : -1;
}
static INT wb(INT r) { return ( r < 0 && ( errno == EAGAIN || errno == EWOULDBLOCK ) ) ? SSH_WOULDBLOCK : r; }
static INT n_send(void *ctx, INT c, const void *b, SZ n) { (void)ctx; return wb((INT)send(c, b, n, MSG_DONTWAIT)); }
static INT n_recv(void *ctx, INT c, void *b, SZ n) { (void)ctx; return wb((INT)recv(c, b, n, MSG_DONTWAIT)); }
static INT n_wait(void *ctx, INT c, BOOL rd, BOOL wr, INT ms)
{
	struct pollfd p = { c, (short)( ( rd ? POLLIN : 0 ) | ( wr ? POLLOUT : 0 ) ), 0 };
	(void)ctx;
	return poll(&p, 1, ms);
}
static void n_close(void *ctx, INT c) { (void)ctx; close(c); }
static const T_SSHNET net = { NULL, n_open, n_send, n_recv, n_close, n_wait };

static int seen_file, seen_dir;
static INT ls_cb(void *arg, const T_FTPENT *e)
{
	(void)arg;
	if ( strcmp((char *)e->name, "up.bin") == 0 && !e->dir && e->size == 3000001 ) seen_file = 1;
	if ( strcmp((char *)e->name, "newdir") == 0 && e->dir ) seen_dir = 1;
	return 0;
}

/* The key in the file against the server: 1 let in, and a command run; 0 not let in */
static int try_key(UINT port, const char *user, const char *file)
{
	static char pem[16384];
	static UB out[100];
	T_SSH *c;
	UB h[SSH_HASH_BYTES];
	FILE *f = fopen(file, "rb");
	size_t len;
	int in;
	SZ o = 0;
	INT n;

	if ( f == NULL ) {
		printf("key %s: no such file\n", file);
		fails++;
		return 0;
	}
	len = fread(pem, 1, sizeof(pem), f);
	fclose(f);
	CK(ssh_connect(&c, &net, "127.0.0.1", port, h) == E_OK);
	in = ssh_login_key(c, user, pem, (SZ)len) == E_OK;
	printf("key %s: %s (%s)\n", file, in ? "let in" : "not let in", in ? "" : ssh_last_error(c));
	if ( in ) {
		memset(out, 0, sizeof(out));
		CK(ssh_exec(c, "echo by key") == E_OK);
		while ( o < 90 && ( n = ssh_chan_read(c, out + o, 90 - o) ) > 0 ) o += n;
		CK(strcmp((char *)out, "by key\n") == 0);
	}
	ssh_close(c);
	return in;
}

int main(int ac, char **av)
{
	T_SSH *c;
	UB h[SSH_HASH_BYTES], h2[SSH_HASH_BYTES];
	UINT port = atoi(av[1]);
	char p[400];
	UB *b, *r;
	int i;
	INT n;
	SZ got = 0;

	/* t port user key <file> yes|no: only this */
	if ( ac == 6 && strcmp(av[3], "key") == 0 ) {
		CK(try_key(port, av[2], av[4]) == ( strcmp(av[5], "yes") == 0 ));
		printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
		return fails != 0;
	}
	b = malloc(3000001);
	r = malloc(3000001);

	/* the wrong password: nothing further */
	CK(ssh_connect(&c, &net, "127.0.0.1", port, h) == E_OK);
	CK(ssh_login(c, av[2], "wrong password") == E_OACV);
	ssh_close(c);

	CK(ssh_connect(&c, &net, "127.0.0.1", port, h2) == E_OK);
	CK(memcmp(h, h2, sizeof(h)) == 0);	/* the same server says the same key */
	printf("host key sha256:");
	for ( i = 0; i < SSH_HASH_BYTES; i++ ) printf("%02x", h[i]);
	printf("\n");
	setvbuf(stdout, NULL, _IONBF, 0);
	CK(ssh_login(c, av[2], av[3]) == E_OK);
	CK(sftp_start(c) == E_OK);

	for ( i = 0; i < 3000001; i++ ) b[i] = (UB)( i * 31 >> 3 );
	snprintf(p, sizeof(p), "%s/up.bin", av[4]);
	CK(sftp_put(c, (UB *)p) == E_OK);
	CK(sftp_write(c, b, 3000001) == 3000001);
	CK(sftp_end(c) == E_OK);
	CK(sftp_get(c, (UB *)p) == E_OK);
	while ( ( n = sftp_read(c, r + got, 3000001 - got) ) > 0 ) got += n;
	CK(n == 0);
	CK(got == 3000001 && memcmp(b, r, got) == 0);
	CK(sftp_end(c) == E_OK);
	CK(sftp_mfmt(c, (UB *)p, 1000000000) == E_OK);
	snprintf(p, sizeof(p), "%s/newdir", av[4]);
	CK(sftp_mkd(c, (UB *)p) == E_OK);
	CK(sftp_list(c, (UB *)av[4], ls_cb, NULL) == E_OK);
	CK(seen_file && seen_dir);
	snprintf(p, sizeof(p), "%s/no/such", av[4]);
	CK(sftp_get(c, (UB *)p) == E_NOEXS);
	{
		static UB out[100];
		SZ o = 0;
		CK(ssh_exec(c, "echo hi from the server") == E_OK);
		while ( o < 90 && ( n = ssh_chan_read(c, out + o, 90 - o) ) > 0 ) o += n;
		printf("command said: %s", out);
		CK(strcmp((char *)out, "hi from the server\n") == 0);
	}
	ssh_close(c);
	free(b);
	free(r);
	printf("%s (%d failed)\n", fails ? "FAILED" : "ALL OK", fails);
	return fails != 0;
}
