/*
 * What the build of wpa_supplicant's TLS layer is given in its own makefile
 * (tools/wpa/proc.sh forces this in): the ciphers OpenSSL may choose from.
 */
#define TLS_DEFAULT_CIPHERS "DEFAULT:!EXP:!LOW"

/* no OCSP in this OpenSSL (tools/wpa/proc.sh): the layer leaves it out with this */
#define OPENSSL_NO_TLSEXT 1
