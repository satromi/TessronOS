#!/usr/bin/env python3
"""A small FTP server for the tests, on the Python standard library alone.

It serves one directory of the host to a machine that reaches the host
through QEMU's user mode network (the host is 10.0.2.2 from inside), so
that the network calls of TessronOS and the programs built on them can be
tried against a real server without anything installed.

    ftpd.py [--host 127.0.0.1] [--port 2121] [--user NAME --password PW]
            [--pasv-address A.B.C.D] [--active-via HOST:PORT]
            [--exit-on-eof] [--quiet] <directory>

Without --user any name is let in (anonymous), with any password. The
commands carried are USER, PASS, SYST, TYPE (A or I), PWD, CWD, CDUP,
PASV, EPSV, PORT, EPRT, LIST, NLST, SIZE, MDTM, RETR, STOR, DELE, MKD,
RMD, NOOP, FEAT, OPTS and QUIT; each connection has a thread of its own.

PASV names the address a client is to connect to for data. The server
listens on 127.0.0.1, which inside the guest is the guest itself, so
--pasv-address gives the address to name instead (10.0.2.2); EPSV names
only the port and needs none.

PORT and EPRT (the active way) name where the client listens, and the
server connects there. The guest's address (10.0.2.15) cannot be reached
from the host through QEMU's user mode network; a port forwarded into
the guest (hostfwd) can, so --active-via gives the host's end of that
forward, which the server connects to instead of what was named.

With --exit-on-eof the server ends when its standard input reaches its
end. Whoever started it stops it by closing that pipe, which also
reaches a server run by the Windows Python from WSL, where a signal
to the WSL side does not.
"""

import argparse
import os
import socket
import socketserver
import sys
import threading
import time

DATA_TIMEOUT = 15		# seconds to wait for the data connection
CTRL_TIMEOUT = 300		# seconds a control connection may sit idle


class Session(socketserver.StreamRequestHandler):
    """One control connection."""

    def setup(self):
        super().setup()
        self.request.settimeout(CTRL_TIMEOUT)
        self.cwd = '/'
        self.user = None
        self.logged_in = False
        self.binary = True
        self.pasv = None
        self.active = None

    # ------------------------------------------------------------ replies

    def reply(self, text):
        self.log('> ' + text)
        self.wfile.write((text + '\r\n').encode('utf-8'))
        self.wfile.flush()

    def log(self, text):
        if not self.server.quiet:
            sys.stderr.write('ftpd %s:%d %s\n' % (self.client_address[0],
                                                  self.client_address[1], text))

    # ------------------------------------------------------------ paths

    def virtual(self, arg):
        """The server's path for an argument, relative to the current directory."""
        path = arg if arg.startswith('/') else self.cwd.rstrip('/') + '/' + arg
        parts = []
        for p in path.split('/'):
            if p in ('', '.'):
                continue
            if p == '..':
                if parts:
                    parts.pop()
                continue
            parts.append(p)
        return '/' + '/'.join(parts)

    def real(self, vpath):
        """The host's path of a server path; never outside the served directory."""
        root = self.server.root
        full = os.path.realpath(os.path.join(root, vpath.lstrip('/')))
        if full != root and not full.startswith(root + os.sep):
            raise PermissionError(vpath)
        return full

    # ------------------------------------------------------------ data connection

    def open_pasv(self):
        if self.pasv is not None:
            self.pasv.close()
        self.active = None
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind((self.server.host, 0))
        s.listen(1)
        s.settimeout(DATA_TIMEOUT)
        self.pasv = s
        return s.getsockname()[1]

    def active_conn(self):
        """The active way: a connection to where PORT or EPRT said, or through the forward."""
        host, port = self.active
        self.active = None
        if self.server.active_via is not None:
            host, port = self.server.active_via
        try:
            conn = socket.create_connection((host, port), timeout=DATA_TIMEOUT)
        except OSError:
            self.reply("425 Can't open data connection.")
            return None
        self.log('data to %s:%d' % (host, port))
        return conn

    def data_conn(self):
        if self.active is not None:
            return self.active_conn()
        if self.pasv is None:
            self.reply('425 Use PASV, EPSV, PORT or EPRT first.')
            return None
        try:
            conn, _ = self.pasv.accept()
        except OSError:
            self.reply('425 No data connection came.')
            conn = None
        finally:
            self.pasv.close()
            self.pasv = None
        if conn is not None:
            conn.settimeout(DATA_TIMEOUT)
        return conn

    def send_data(self, data):
        conn = self.data_conn()
        if conn is None:
            return
        self.reply('150 Opening data connection.')
        try:
            conn.sendall(data)
        finally:
            conn.close()
        self.reply('226 Transfer complete.')

    # ------------------------------------------------------------ listings

    def list_line(self, name, full):
        st = os.stat(full)
        kind = 'd' if os.path.isdir(full) else '-'
        mode = kind + ('rwxr-xr-x' if kind == 'd' else 'rw-r--r--')
        when = time.gmtime(st.st_mtime)
        if time.time() - st.st_mtime > 180 * 24 * 3600:
            stamp = time.strftime('%b %d  %Y', when)
        else:
            stamp = time.strftime('%b %d %H:%M', when)
        return '%s 1 ftp ftp %12d %s %s' % (mode, st.st_size, stamp, name)

    def entries(self, arg):
        """(name, host path) of what a listing covers: a directory's entries, or one file."""
        vpath = self.virtual(arg) if arg and not arg.startswith('-') else self.cwd
        full = self.real(vpath)
        if os.path.isdir(full):
            return [(n, os.path.join(full, n)) for n in sorted(os.listdir(full))]
        if os.path.exists(full):
            return [(os.path.basename(full), full)]
        raise FileNotFoundError(vpath)

    # ------------------------------------------------------------ the loop

    def handle(self):
        self.reply('220 TessronOS test FTP server ready.')
        while True:
            try:
                line = self.rfile.readline()
            except OSError:
                break
            if not line:
                break
            line = line.decode('utf-8', 'replace').rstrip('\r\n')
            cmd, _, arg = line.partition(' ')
            cmd = cmd.upper()
            self.log('< ' + (cmd + ' ****' if cmd == 'PASS' else line))
            if cmd == 'QUIT':
                self.reply('221 Goodbye.')
                break
            fn = getattr(self, 'ftp_' + cmd, None)
            if fn is None:
                self.reply('502 Command not implemented.')
                continue
            if not self.logged_in and cmd not in ('USER', 'PASS', 'SYST', 'FEAT', 'NOOP', 'OPTS'):
                self.reply('530 Please log in with USER and PASS.')
                continue
            try:
                fn(arg)
            except PermissionError:
                self.reply('550 Permission denied.')
            except FileNotFoundError:
                self.reply('550 No such file or directory.')
            except OSError as e:
                self.reply('550 %s.' % (e.strerror or 'Failed'))
        if self.pasv is not None:
            self.pasv.close()

    # ------------------------------------------------------------ commands

    def ftp_USER(self, arg):
        self.user = arg
        self.logged_in = False
        if self.server.user is None:
            self.reply('331 Any password will do.')
        else:
            self.reply('331 Password required.')

    def ftp_PASS(self, arg):
        if self.user is None:
            self.reply('503 Send USER first.')
            return
        if self.server.user is None or (self.user == self.server.user
                                        and arg == self.server.password):
            self.logged_in = True
            self.reply('230 Logged in.')
        else:
            self.reply('530 Login incorrect.')

    def ftp_SYST(self, arg):
        self.reply('215 UNIX Type: L8')

    def ftp_FEAT(self, arg):
        self.wfile.write(b'211-Features:\r\n EPSV\r\n PASV\r\n SIZE\r\n MDTM\r\n UTF8\r\n')
        self.reply('211 End')

    def ftp_OPTS(self, arg):
        if arg.upper().startswith('UTF8'):
            self.reply('200 UTF8 is always on.')
        else:
            self.reply('501 Option not understood.')

    def ftp_NOOP(self, arg):
        self.reply('200 OK.')

    def ftp_TYPE(self, arg):
        t = arg.upper().split(' ')[0] if arg else ''
        if t == 'I':
            self.binary = True
            self.reply('200 Type set to I.')
        elif t == 'A':
            self.binary = False
            self.reply('200 Type set to A.')
        else:
            self.reply('504 Type not carried.')

    def ftp_PWD(self, arg):
        self.reply('257 "%s" is the current directory.' % self.cwd.replace('"', '""'))

    def ftp_CWD(self, arg):
        vpath = self.virtual(arg)
        if not os.path.isdir(self.real(vpath)):
            raise FileNotFoundError(vpath)
        self.cwd = vpath
        self.reply('250 Directory changed to %s.' % vpath)

    def ftp_CDUP(self, arg):
        self.ftp_CWD('..')

    def ftp_PASV(self, arg):
        port = self.open_pasv()
        addr = self.server.pasv_address or self.server.host
        self.reply('227 Entering Passive Mode (%s,%d,%d).'
                   % (addr.replace('.', ','), port >> 8, port & 0xff))

    def ftp_EPSV(self, arg):
        port = self.open_pasv()
        self.reply('229 Entering Extended Passive Mode (|||%d|).' % port)

    def set_active(self, host, port):
        if self.pasv is not None:
            self.pasv.close()
            self.pasv = None
        self.active = (host, port)

    def ftp_PORT(self, arg):
        try:
            n = [int(x) for x in arg.split(',')]
            if len(n) != 6 or any(v < 0 or v > 255 for v in n):
                raise ValueError(arg)
        except ValueError:
            self.reply('501 Syntax error in PORT.')
            return
        self.set_active('.'.join(str(v) for v in n[:4]), n[4] * 256 + n[5])
        self.reply('200 PORT command successful.')

    def ftp_EPRT(self, arg):
        parts = arg[1:].split(arg[:1]) if arg else []
        if len(parts) < 3:
            self.reply('501 Syntax error in EPRT.')
            return
        if parts[0] != '1':
            self.reply('522 Network protocol not supported, use (1).')
            return
        try:
            port = int(parts[2])
        except ValueError:
            self.reply('501 Syntax error in EPRT.')
            return
        self.set_active(parts[1], port)
        self.reply('200 EPRT command successful.')

    def ftp_LIST(self, arg):
        lines = [self.list_line(n, f) for n, f in self.entries(arg)]
        self.send_data(''.join(l + '\r\n' for l in lines).encode('utf-8'))

    def ftp_NLST(self, arg):
        names = [n for n, _ in self.entries(arg)]
        self.send_data(''.join(n + '\r\n' for n in names).encode('utf-8'))

    def ftp_SIZE(self, arg):
        full = self.real(self.virtual(arg))
        if not os.path.isfile(full):
            raise FileNotFoundError(arg)
        self.reply('213 %d' % os.path.getsize(full))

    def ftp_MDTM(self, arg):
        full = self.real(self.virtual(arg))
        if not os.path.exists(full):
            raise FileNotFoundError(arg)
        self.reply('213 ' + time.strftime('%Y%m%d%H%M%S', time.gmtime(os.path.getmtime(full))))

    def ftp_RETR(self, arg):
        full = self.real(self.virtual(arg))
        if not os.path.isfile(full):
            raise FileNotFoundError(arg)
        with open(full, 'rb') as f:
            data = f.read()
        if not self.binary:
            data = data.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
        self.send_data(data)

    def ftp_STOR(self, arg):
        full = self.real(self.virtual(arg))
        conn = self.data_conn()
        if conn is None:
            return
        self.reply('150 Ready to receive.')
        chunks = []
        try:
            while True:
                b = conn.recv(65536)
                if not b:
                    break
                chunks.append(b)
        finally:
            conn.close()
        data = b''.join(chunks)
        if not self.binary:
            data = data.replace(b'\r\n', b'\n')
        with open(full, 'wb') as f:
            f.write(data)
        self.reply('226 Transfer complete (%d bytes).' % len(data))

    def ftp_DELE(self, arg):
        full = self.real(self.virtual(arg))
        if not os.path.isfile(full):
            raise FileNotFoundError(arg)
        os.remove(full)
        self.reply('250 Deleted.')

    def ftp_MKD(self, arg):
        vpath = self.virtual(arg)
        os.mkdir(self.real(vpath))
        self.reply('257 "%s" created.' % vpath.replace('"', '""'))

    def ftp_RMD(self, arg):
        vpath = self.virtual(arg)
        full = self.real(vpath)
        if full == self.server.root:
            raise PermissionError(vpath)
        os.rmdir(full)
        self.reply('250 Removed.')


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description='A small FTP server for the tests.')
    ap.add_argument('directory', help='the directory to serve')
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=2121)
    ap.add_argument('--user', default=None, help='the one user let in (default: anyone)')
    ap.add_argument('--password', default='')
    ap.add_argument('--pasv-address', default=None,
                    help='the address PASV names (default: the one listened on)')
    ap.add_argument('--active-via', default=None,
                    help='HOST:PORT the active way connects to (a port forwarded into the client)')
    ap.add_argument('--exit-on-eof', action='store_true',
                    help='end when standard input ends')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--http', type=int, default=0,
                    help='also the test HTTP server (tools/httpd_test.py) on this port and the next')
    args = ap.parse_args()
    if args.http:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import httpd_test
        httpd_test.serve(args.host, args.http, args.quiet)

    root = os.path.realpath(args.directory)
    if not os.path.isdir(root):
        sys.exit('ftpd: not a directory: %s' % args.directory)
    srv = Server((args.host, args.port), Session)
    srv.root = root
    srv.host = args.host
    srv.user = args.user
    srv.password = args.password
    srv.pasv_address = args.pasv_address
    srv.active_via = None
    if args.active_via:
        h, _, p = args.active_via.rpartition(':')
        srv.active_via = (h, int(p))
    srv.quiet = args.quiet
    if not args.quiet:
        sys.stderr.write('ftpd: serving %s on %s:%d\n' % (root, args.host, args.port))
    if args.exit_on_eof:
        def watch():
            try:
                sys.stdin.read()
            except (OSError, ValueError):
                pass
            os._exit(0)
        threading.Thread(target=watch, daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


if __name__ == '__main__':
    main()
