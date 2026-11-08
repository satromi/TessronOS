#!/bin/bash
# lib/libsftp on the host against a real OpenSSH, run as root so that sshd can
# check a password. /etc/passwd and /etc/shadow are bound over with copies in a
# mount namespace of this script's own: nothing of the system's changes.
# The server comes from openssh-server unpacked under the user's ~/.local/sshd.
# Usage (as root): HOME_USER=name tools/ssh/test_host.sh
set -u
U=${HOME_USER:-satromi}
UH=$(getent passwd "$U" | cut -d: -f6)
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
W=$(mktemp -d /tmp/sshtest.XXXXXX)
chmod 755 "$W"
trap 'kill $(cat "$W/pid" 2>/dev/null) 2>/dev/null; rm -rf "$W"' EXIT
LB=$UH/tessronos-ssh-host

# the library and Mbed TLS for the host, once
su "$U" -c "ARCH=host bash $here/build.sh all" | tail -1
if [ ! -f "$LB/libmbed.a" ]; then
	mkdir -p "$LB/m"
	ls "$top"/lib/mbedtls/library/*.c | xargs -P 8 -I{} sh -c 'gcc -g -O1 "-DMBEDTLS_CONFIG_FILE=\"ts_tls_config.h\"" -I'"$top/lib/libtls"' -I'"$top/lib/mbedtls/include"' -c -o '"$LB/m"'/$(basename {} .c).o {}' || exit 1
	ar rcs "$LB/libmbed.a" "$LB"/m/*.o
fi
gcc -g -fsanitize=address,undefined -std=gnu11 -I"$top/tools/tests/hostfs/shim" -I"$top/include" -I"$UH/libssh2/libssh2-1.11.1/include" -I"$top/lib/libtls" -I"$top/lib/mbedtls/include" "-DMBEDTLS_CONFIG_FILE=\"ts_tls_config.h\"" \
	-o "$W/t" "$here/test_host.c" "$top/lib/libsftp/sftp.c" "$LB/libssh2.a" "$LB/libmbed.a" 2>&1 | grep -E "error|undefined" | head -20
[ -x "$W/t" ] || { echo "RESULT build failed"; exit 1; }

ssh-keygen -q -t rsa -b 3072 -N "" -f "$W/hk_rsa"
ssh-keygen -q -t ecdsa -N "" -f "$W/hk_ecdsa"
mkdir -p "$W/auth" "$W/data"; chmod 755 "$W/auth"; chown "$U" "$W/data"
# the keys of the user: the form OpenSSH writes today, the PEM form, a key of another
# kind, and one the server does not list
ssh-keygen -q -t rsa -b 2048 -N "" -f "$W/k_openssh"
ssh-keygen -q -t rsa -b 2048 -N "" -m PEM -f "$W/k_pem"
ssh-keygen -q -t rsa -b 2048 -N "" -m PKCS8 -f "$W/k_pkcs8"
ssh-keygen -q -t ed25519 -N "" -f "$W/k_ed"
ssh-keygen -q -t rsa -b 2048 -N "" -m PEM -f "$W/k_other"
cat "$W/k_openssh.pub" "$W/k_pem.pub" "$W/k_pkcs8.pub" "$W/k_ed.pub" > "$W/auth/keys"
S=$UH/.local/sshd/usr
cat > "$W/conf" <<EOC
Port 2222
ListenAddress 127.0.0.1
HostKey $W/hk_rsa
HostKey $W/hk_ecdsa
UsePAM no
PasswordAuthentication yes
PidFile $W/pid
StrictModes no
PubkeyAuthentication yes
AuthorizedKeysFile $W/auth/keys
LogLevel DEBUG1
Subsystem sftp $S/lib/openssh/sftp-server
EOC
mkdir -p /run/sshd
LD_P=$W/libs
mkdir -p "$LD_P"
[ -e /lib/x86_64-linux-gnu/libwrap.so.0 ] || { (cd "$W" && apt-get download -q libwrap0 >/dev/null 2>&1; dpkg -x libwrap0*.deb "$W/w"; cp "$W"/w/lib/x86_64-linux-gnu/libwrap.so.0* "$LD_P"/ 2>/dev/null; cp "$W"/w/usr/lib/x86_64-linux-gnu/libwrap.so.0* "$LD_P"/ 2>/dev/null); }

cat > "$W/run" <<EOR
#!/bin/bash
cp /etc/passwd $W/passwd; cp /etc/shadow $W/shadow
grep -q '^sshd:' $W/passwd || echo 'sshd:x:74:65534::/run/sshd:/usr/sbin/nologin' >> $W/passwd
H=\$(python3 -c 'import crypt; print(crypt.crypt("s3cret pass", crypt.mksalt(crypt.METHOD_SHA512)))')
grep -v '^$U:' $W/shadow > $W/shadow2; echo "$U:\$H:19000:0:99999:7:::" >> $W/shadow2
mount --bind $W/passwd /etc/passwd
mount --bind $W/shadow2 /etc/shadow
LD_LIBRARY_PATH=$LD_P $S/sbin/sshd -f $W/conf -E $W/log
sleep 1
echo "== password"; timeout 40 $W/t 2222 $U "s3cret pass" $W/data; r1=\$?
echo "== key"; r2=0
for k in "k_pem yes" "k_pkcs8 yes" "k_other no" "k_openssh \${EXPECT_OPENSSH:-no}" "k_ed no"; do
	set -- \$k
	timeout 40 $W/t 2222 $U key $W/\$1 \$2 || r2=1
done
r1=\$(( r1 + r2 ))
kill \$(cat $W/pid)
grep -E "Accepted|Failed|userauth|signature|error" $W/log | tail -14
echo "RESULT rc=\$r1"
EOR
unshare -m bash "$W/run"
