#!/bin/bash
# An OpenSSH sshd on the host for the tests inside the machine (make SSH=1 NET=user, ktest_xfc,
# tools/tests/ui_ssh.steps), run as root in a mount namespace of its own: the user "kts"
# with the password "ktpass", whose home holds KTEST.TXT, exists only in copies of /etc/passwd
# and /etc/shadow bound over the real ones there. Nothing of the system's changes.
#   sudo-less from Windows: wsl -u root bash tools/ssh/sshd_host.sh start [PORT]; ... stop
set -u
U=${HOME_USER:-satromi}
UH=$(getent passwd "$U" | cut -d: -f6)
S=$UH/.local/sshd/usr
W=/tmp/sshd_kt
case "${1:-start}" in
stop)
	[ -f "$W/pid" ] && kill "$(cat "$W/pid")" 2>/dev/null
	rm -rf "$W"
	exit 0
	;;
esac
PORT=${2:-2222}
rm -rf "$W"; mkdir -p "$W/home" "$W/libs" /run/sshd; chmod 755 "$W"
printf 'TessronOS FTP test file\n' > "$W/home/KTEST.TXT"
chown -R 1000 "$W/home"
ssh-keygen -q -t rsa -b 3072 -N "" -f "$W/hk_rsa"
ssh-keygen -q -t ecdsa -N "" -f "$W/hk_ecdsa"
cat > "$W/conf" <<EOC
Port $PORT
ListenAddress 127.0.0.1
HostKey $W/hk_rsa
HostKey $W/hk_ecdsa
UsePAM no
PasswordAuthentication yes
PidFile $W/pid
StrictModes no
Subsystem sftp $S/lib/openssh/sftp-server
EOC
if [ ! -e /lib/x86_64-linux-gnu/libwrap.so.0 ]; then
	(cd "$W" && apt-get download -q libwrap0 >/dev/null 2>&1; dpkg -x libwrap0*.deb w; cp w/lib/x86_64-linux-gnu/libwrap.so.0* libs/ 2>/dev/null; cp w/usr/lib/x86_64-linux-gnu/libwrap.so.0* libs/ 2>/dev/null)
fi
cat > "$W/run" <<EOR
cp /etc/passwd $W/passwd; cp /etc/shadow $W/shadow
grep -q '^sshd:' $W/passwd || echo 'sshd:x:74:65534::/run/sshd:/usr/sbin/nologin' >> $W/passwd
echo "kts:x:1000:1000::$W/home:/bin/sh" >> $W/passwd
H=\$(python3 -c 'import crypt; print(crypt.crypt("ktpass", crypt.mksalt(crypt.METHOD_SHA512)))')
echo "kts:\$H:19000:0:99999:7:::" >> $W/shadow
mount --bind $W/passwd /etc/passwd
mount --bind $W/shadow /etc/shadow
LD_LIBRARY_PATH=$W/libs exec $S/sbin/sshd -f $W/conf -E $W/log
EOR
unshare -m bash "$W/run"
sleep 1
[ -f "$W/pid" ] && echo "sshd $(cat "$W/pid") on $PORT"
