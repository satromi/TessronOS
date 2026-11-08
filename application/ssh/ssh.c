/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ssh.c
 *	The command ssh (make SSH=1): a terminal on another machine, or one
 *	command run there, from the console.
 *
 *	    ssh [-p port] [-l user] [user@]host [command]
 *
 *	The server's key is looked for in the file known_hosts next to the
 *	user's other settings ($HOME/.ssh, an object as any other), as
 *	host:port and the SHA-256 of the key. A key not seen before is asked
 *	about and kept if accepted; one that differs from the key kept ends
 *	the connection before anything is sent, and the user must decide what
 *	to do by editing that file. The password is read from the terminal
 *	without being shown.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/sftp.h>

#define WAIT_SHORT_MS	20		/* how long a look at the socket lasts while the terminal is watched */

/* ---------------------------------------------------------------- the key of the server */

static void kh_path( char *p, size_t max, BOOL make )
{
	CONST char	*h = getenv("HOME");

	snprintf(p, max, "%s/.ssh", ( h != NULL ) ? h : "");
	if ( make ) (void)mkdir(p, 0700);
	snprintf(p, max, "%s/.ssh/known_hosts", ( h != NULL ) ? h : "");
}

/* 1 the key known, 0 no key kept for the host, -1 another key is kept */
static INT key_check( CONST char *id, CONST char *hex )
{
	char	p[512], line[512];
	FILE	*f;
	size_t	il = strlen(id);
	INT	r = 0;

	kh_path(p, sizeof(p), FALSE);
	f = fopen(p, "r");
	if ( f == NULL ) return 0;
	while ( fgets(line, sizeof(line), f) != NULL ) {
		if ( strncmp(line, id, il) == 0 && line[il] == ' ' ) {
			line[strcspn(line, "\r\n")] = 0;
			r = ( strcmp(line + il + 1, hex) == 0 ) ? 1 : -1;
			if ( r > 0 ) break;
		}
	}
	fclose(f);
	return r;
}

static void key_keep( CONST char *id, CONST char *hex )
{
	char	p[512];
	FILE	*f;

	kh_path(p, sizeof(p), TRUE);
	f = fopen(p, "a");
	if ( f == NULL ) {
		fprintf(stderr, "ssh: the key could not be kept in %s\n", p);
		return;
	}
	fprintf(f, "%s %s\n", id, hex);
	fclose(f);
}

/* ---------------------------------------------------------------- the terminal */

static struct termios	tt_was;
static BOOL		tt_raw = FALSE;

static void tty_restore( void )
{
	if ( tt_raw ) {
		(void)tcsetattr(0, TCSAFLUSH, &tt_was);
		tt_raw = FALSE;
	}
}

static void tty_make_raw( void )
{
	struct termios	t;

	if ( tcgetattr(0, &tt_was) != 0 ) return;
	t = tt_was;
	cfmakeraw(&t);
	if ( tcsetattr(0, TCSAFLUSH, &t) == 0 ) tt_raw = TRUE;
}

/* A line without its being shown */
static void read_secret( CONST char *prompt, char *buf, size_t max )
{
	struct termios	t, was;
	BOOL		noecho = FALSE;
	size_t		n = 0;

	/* the prompt after the change, which drops what was typed before it */
	if ( tcgetattr(0, &was) == 0 ) {
		t = was;
		t.c_lflag &= ~(tcflag_t)ECHO;
		noecho = ( tcsetattr(0, TCSAFLUSH, &t) == 0 );
	}
	fprintf(stderr, "%s", prompt);
	fflush(stderr);
	for ( ;; ) {
		char	c;

		if ( read(0, &c, 1) != 1 || c == '\n' || c == '\r' ) break;
		if ( n + 1 < max ) buf[n++] = c;
	}
	buf[n] = 0;
	if ( noecho ) (void)tcsetattr(0, TCSAFLUSH, &was);
	fprintf(stderr, "\n");
}

static BOOL ask_yes( CONST char *q )
{
	char	b[16];
	size_t	n = 0;

	fprintf(stderr, "%s", q);
	fflush(stderr);
	for ( ;; ) {
		char	c;

		if ( read(0, &c, 1) != 1 || c == '\n' || c == '\r' ) break;
		if ( n + 1 < sizeof(b) ) b[n++] = c;
	}
	b[n] = 0;
	return (BOOL)( strcmp(b, "yes") == 0 );
}

/* What comes from either side passed to the other until the channel ends */
static INT session( T_SSH *c )
{
	static UB	b[8192];
	struct winsize	ws, was;
	BOOL		have_ws = ( ioctl(0, TIOCGWINSZ, &was) == 0 );
	INT		sock = ssh_socket(c);

	ssh_nonblock(c);
	for ( ;; ) {
		struct pollfd	p;
		INT		n;

		while ( ( n = ssh_chan_read(c, b, sizeof(b)) ) > 0 ) {
			(void)write(1, b, (size_t)n);
		}
		if ( n < 0 ) return 0;

		/* a little wait on the socket: the terminal is looked at between */
		(void)ssh_net_so.wait(NULL, sock, TRUE, FALSE, WAIT_SHORT_MS);

		p.fd = 0;
		p.events = POLLIN;
		p.revents = 0;
		if ( poll(&p, 1, 0) > 0 && ( p.revents & POLLIN ) != 0 ) {
			ssize_t	k = read(0, b, sizeof(b));
			SZ	done = 0;

			if ( k <= 0 ) return 0;
			while ( done < k ) {
				n = ssh_chan_write(c, b + done, k - done);
				if ( n < 0 ) return 1;
				done += n;
			}
		}
		if ( have_ws && ioctl(0, TIOCGWINSZ, &ws) == 0
		  && ( ws.ws_row != was.ws_row || ws.ws_col != was.ws_col ) ) {
			was = ws;
			(void)ssh_resize(c, ws.ws_col, ws.ws_row);
		}
	}
}

/* What is done with the person's key, in place of connecting. The exit code */
static int key_command( CONST char *what, CONST char *arg, INT bits, BOOL force )
{
	static char	pem[SSH_KEY_MAX + 1], line[SSH_PUB_MAX];
	INT		n;
	ER		er;

	n = ssh_key_load(pem, SSH_KEY_MAX);
	if ( strcmp(what, "-Y") == 0 ) {
		if ( n < E_OK ) {
			fprintf(stderr, "ssh: no key to show (%d); make one with -K or take one in with -I\n", n);
			return 1;
		}
	} else {
		if ( n >= E_OK && !force ) {
			fprintf(stderr, "ssh: there is a key already; -f replaces it\n");
			return 1;
		}
		if ( strcmp(what, "-K") == 0 ) {
			fprintf(stderr, "Making an RSA key of %d bits ...\n", bits);
			n = ssh_key_generate(bits, pem, SSH_KEY_MAX);
		} else {
			FILE	*f = fopen(arg, "rb");

			if ( f == NULL ) {
				fprintf(stderr, "ssh: cannot read %s\n", arg);
				return 1;
			}
			n = (INT)fread(pem, 1, SSH_KEY_MAX, f);
			fclose(f);
		}
		if ( n <= 0 ) {
			fprintf(stderr, "ssh: no key (%d)\n", n);
			return 1;
		}
		er = ssh_key_store(pem, n);
		if ( er == E_OBJ ) {
			fprintf(stderr, "ssh: not a key this can use: RSA in PEM or PKCS#8, not encrypted. A key OpenSSH wrote\n"
				"  in its own form is changed with: ssh-keygen -p -m PEM -f file\n");
			return 1;
		}
		if ( er < E_OK ) {
			fprintf(stderr, "ssh: the key could not be kept (%d)\n", (INT)er);
			return 1;
		}
	}
	n = ssh_key_public(pem, n, line, sizeof(line));
	memset(pem, 0, sizeof(pem));
	if ( n < E_OK ) {
		fprintf(stderr, "ssh: the key is not usable (%d)\n", n);
		return 1;
	}
	printf("%s\n", line);
	if ( strcmp(what, "-Y") != 0 ) printf("The key is kept. Put the line above in the server's authorized_keys.\n");
	fflush(stdout);
	return 0;
}

/* The person's key offered, when there is one: whether it was let in */
static BOOL login_by_key( T_SSH *c, CONST char *user )
{
	static char	pem[SSH_KEY_MAX + 1];
	INT		n = ssh_key_load(pem, SSH_KEY_MAX);
	BOOL		in = FALSE;

	if ( n == E_OACV ) {
		fprintf(stderr, "ssh: the objects of your key have been opened to others: it is not used\n");
	}
	if ( n > 0 ) {
		in = ssh_login_key(c, user, pem, n) >= E_OK;
		memset(pem, 0, sizeof(pem));
	}
	return in;
}

int main( int ac, char **av )
{
	CONST char	*user = NULL, *host = NULL, *cmd = NULL;
	UINT		port = 22;
	static char	spec[256], pass[256], id[300], hex[SSH_HASH_BYTES * 2 + 1], cmdline[4096];
	UB		hash[SSH_HASH_BYTES];
	T_SSH		*c;
	INT		i, k, rc, bits = 3072;
	ER		er;
	BOOL		force = FALSE, in;
	CONST char	*keycmd = NULL, *keyarg = NULL;

	for ( i = 1; i < ac && av[i][0] == '-'; i++ ) {
		if ( strcmp(av[i], "-K") == 0 || strcmp(av[i], "-Y") == 0 ) {
			keycmd = av[i];
		} else if ( strcmp(av[i], "-I") == 0 && i + 1 < ac ) {
			keycmd = av[i];
			keyarg = av[++i];
		} else if ( strcmp(av[i], "-b") == 0 && i + 1 < ac ) {
			bits = atoi(av[++i]);
		} else if ( strcmp(av[i], "-f") == 0 ) {
			force = TRUE;
		} else if ( strcmp(av[i], "-p") == 0 && i + 1 < ac ) {
			port = (UINT)atoi(av[++i]);
		} else if ( strcmp(av[i], "-l") == 0 && i + 1 < ac ) {
			user = av[++i];
		} else {
			break;
		}
	}
	if ( keycmd != NULL ) {
		if ( bits < 2048 || bits > 8192 ) {
			fprintf(stderr, "ssh: -b is from 2048 to 8192\n");
			return 2;
		}
		return key_command(keycmd, keyarg, bits, force);
	}
	if ( i >= ac || port == 0 || port > 65535 ) {
		fprintf(stderr, "usage: ssh [-p port] [-l user] [user@]host [command]\n"
			"       ssh -K [-b bits] [-f]    make your key\n"
			"       ssh -I file [-f]         take your key in from a file\n"
			"       ssh -Y                   show your public key\n");
		return 2;
	}
	snprintf(spec, sizeof(spec), "%s", av[i++]);
	{
		char	*at = strchr(spec, '@');

		if ( at != NULL ) {
			*at = 0;
			user = spec;
			host = at + 1;
		} else {
			host = spec;
		}
	}
	if ( user == NULL ) user = getenv("USER");
	if ( user == NULL ) {
		fprintf(stderr, "ssh: whose name? -l user\n");
		return 2;
	}
	if ( i < ac ) {
		size_t	w = 0;

		for ( ; i < ac; i++ ) {
			w += (size_t)snprintf(cmdline + w, sizeof(cmdline) - w, "%s%s", w > 0 ? " " : "", av[i]);
			if ( w >= sizeof(cmdline) ) {
				fprintf(stderr, "ssh: the command is too long\n");
				return 2;
			}
		}
		cmd = cmdline;
	}

	er = ssh_connect(&c, &ssh_net_so, host, port, hash);
	if ( er < E_OK ) {
		fprintf(stderr, "ssh: %s: could not connect (%d)\n", host, (INT)er);
		return 1;
	}
	snprintf(id, sizeof(id), "%s:%u", host, port);
	for ( k = 0; k < SSH_HASH_BYTES; k++ ) snprintf(hex + k * 2, 3, "%02x", hash[k]);
	switch ( key_check(id, hex) ) {
	case 1:
		break;
	case 0:
		fprintf(stderr, "The key of %s is new here.\n  SHA-256 %s\n", id, hex);
		if ( !ask_yes("Is this the right key? Type yes to go on and keep it: ") ) {
			ssh_close(c);
			return 1;
		}
		key_keep(id, hex);
		break;
	default:
		fprintf(stderr, "ssh: THE KEY OF %s HAS CHANGED.\n  It is now SHA-256 %s\n"
			"  The server may be another one pretending. Nothing was sent.\n"
			"  If the change is right, take the old line out of known_hosts.\n", id, hex);
		ssh_close(c);
		return 1;
	}

	/* the key is offered once; a server that does not take it is asked by password */
	in = login_by_key(c, user);
	for ( k = 0; !in; k++ ) {
		read_secret("Password: ", pass, sizeof(pass));
		er = ssh_login(c, user, pass);
		memset(pass, 0, sizeof(pass));
		if ( er >= E_OK ) break;
		if ( k == 2 ) {
			fprintf(stderr, "ssh: not let in\n");
			ssh_close(c);
			return 1;
		}
		fprintf(stderr, "Not right. ");
	}

	if ( cmd != NULL ) {
		er = ssh_exec(c, cmd);
	} else {
		struct winsize	ws;
		CONST char	*term = getenv("TERM");
		INT		cols = 80, rows = 24;

		if ( ioctl(0, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ) {
			cols = ws.ws_col;
			rows = ws.ws_row;
		}
		er = ssh_shell(c, ( term != NULL ) ? term : "vt100", cols, rows);
	}
	if ( er < E_OK ) {
		fprintf(stderr, "ssh: the server did not start it (%d)\n", (INT)er);
		ssh_close(c);
		return 1;
	}

	if ( cmd == NULL ) tty_make_raw();
	rc = session(c);
	tty_restore();
	ssh_close(c);
	return rc;
}
