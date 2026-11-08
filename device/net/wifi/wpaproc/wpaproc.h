/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	wpaproc.h (x64 PC, Raspberry Pi 5)
 *	The messages between the kernel's Wi-Fi manager and the process
 *	that runs wpa_supplicant (design 9.6.11), through record 1 of the
 *	device object "wpasup": the manager's calls of knl_wpa_* (CALL)
 *	and their answers (RET), the process's calls back into the kernel
 *	while it answers (CB) and theirs (CBRET).
 *
 *	A message is a header and its arguments one after another: an
 *	integer (4 bytes, little endian), a run of bytes with its length,
 *	or a null pointer. Both ends know the order of each operation's
 *	arguments; a reader that finds another kind, or runs off the end,
 *	takes the message as broken. Each CALL has its number, which the
 *	messages of its answer (RET, CB, CBRET) carry: one of a call that
 *	has given up is told from the next call's.
 *
 *	Plain C without the kernel's headers: the kernel, the process and
 *	the host's test all build it.
 */

#ifndef __WPAPROC_H__
#define __WPAPROC_H__

#define WPP_MAGIC	0x31505057U	/* "WPP1" */
#define WPP_VERSION	1
#define WPP_MAX		4096		/* bytes of a message, its header included */

/* Kinds */
#define WPP_HELLO	1		/* process to kernel: ready (ret: the version) */
#define WPP_CALL	2		/* kernel to process */
#define WPP_RET		3		/* process to kernel */
#define WPP_CB		4		/* process to kernel */
#define WPP_CBRET	5		/* kernel to process */

/* Operations of CALL: the functions of wpa_glue.h */
#define WPP_CLASSIFY	1
#define WPP_PSK		2
#define WPP_START	3
#define WPP_CHOOSE	4
#define WPP_START2	5
#define WPP_SAE_PT	6
#define WPP_SAE_START	7
#define WPP_SAE_RX	8
#define WPP_SAE_RESEND	9
#define WPP_SAE_PMK	10
#define WPP_SAE_ABORT	11
#define WPP_ASSOC	12
#define WPP_RX		13
#define WPP_STATE	14
#define WPP_DISASSOC	15
#define WPP_TICK	16
#define WPP_EAP_BLOB	17		/* a piece of a certificate or key of knl_wpa_eap_set */
#define WPP_EAP_SET	18
#define WPP_EAP_RESULT	19
#define WPP_EAP_END	20
#define WPP_EAP_PIECE	3072		/* bytes of a certificate in one message */
#define WPP_EAP_BLOB_MAX 16384		/* bytes of one certificate or key */

/* Operations of CB: the kernel's functions the glue calls */
#define WPP_CB_EAPOL_TX	1
#define WPP_CB_SETKEY	2
#define WPP_CB_DELKEY	3
#define WPP_CB_DEAUTH	4
#define WPP_CB_SAY	5

/* The most an answer carries of each output (the callers' buffers are this large) */
#define WPP_START_IE_MAX	64	/* knl_wpa_start's element */
#define WPP_PMK_LEN		32
#define WPP_PMKID_LEN		16
#define WPP_ADDR_LEN		6

typedef struct {
	unsigned int	magic;
	unsigned short	kind;
	unsigned short	op;
	int		ret;
	unsigned int	seq;		/* the CALL's number, which the messages of its answer carry */
	unsigned int	len;		/* bytes of the arguments after the header */
} WPP_HDR;

/* The start-up argument of the process: the object of the way, its UUID's 16 bytes */
typedef struct {
	unsigned int	magic;		/* WPP_MAGIC */
	unsigned int	version;	/* WPP_VERSION */
	unsigned char	chan[16];
} WPP_ARG;

#define WPP_ARGS_MAX	( WPP_MAX - (int)sizeof(WPP_HDR) )

#define WPP_T_INT	1
#define WPP_T_BYTES	2
#define WPP_T_NULL	3

/* A message being written */
typedef struct {
	unsigned char	*b;		/* the whole message, header first */
	int		n;		/* bytes written, header included */
	int		bad;		/* it did not fit */
} WPP_W;

/* A message being read */
typedef struct {
	const unsigned char *b;
	int		n;		/* bytes of the message */
	int		at;
	int		bad;		/* broken or short */
} WPP_R;

static inline void wpp_w_begin( WPP_W *w, unsigned char *buf, int kind, int op, int ret )
{
	WPP_HDR	*h = (WPP_HDR *)(void *)buf;

	h->magic = WPP_MAGIC;
	h->kind = (unsigned short)kind;
	h->op = (unsigned short)op;
	h->ret = ret;
	h->seq = 0;
	h->len = 0;
	w->b = buf;
	w->n = (int)sizeof(WPP_HDR);
	w->bad = 0;
}

static inline void wpp_w_byte( WPP_W *w, unsigned char c )
{
	if ( w->n >= WPP_MAX ) {
		w->bad = 1;
		return;
	}
	w->b[w->n++] = c;
}

static inline void wpp_w_u32( WPP_W *w, unsigned int v )
{
	wpp_w_byte(w, (unsigned char)v);
	wpp_w_byte(w, (unsigned char)( v >> 8 ));
	wpp_w_byte(w, (unsigned char)( v >> 16 ));
	wpp_w_byte(w, (unsigned char)( v >> 24 ));
}

static inline void wpp_w_int( WPP_W *w, int v )
{
	wpp_w_byte(w, WPP_T_INT);
	wpp_w_u32(w, (unsigned int)v);
}

/* Bytes, or a null pointer when p is NULL */
static inline void wpp_w_bytes( WPP_W *w, const void *p, int len )
{
	const unsigned char *s = (const unsigned char *)p;
	int	i;

	if ( p == 0 ) {
		wpp_w_byte(w, WPP_T_NULL);
		return;
	}
	if ( len < 0 || len > WPP_MAX - w->n - 5 ) {
		w->bad = 1;
		return;
	}
	wpp_w_byte(w, WPP_T_BYTES);
	wpp_w_u32(w, (unsigned int)len);
	for ( i = 0; i < len; i++ ) w->b[w->n++] = s[i];
}

/* The message's length into its header: the bytes to send, or 0 when it did not fit */
static inline int wpp_w_end( WPP_W *w )
{
	if ( w->bad ) return 0;
	((WPP_HDR *)(void *)w->b)->len = (unsigned int)( w->n - (int)sizeof(WPP_HDR) );
	return w->n;
}

/* A message checked whole: its header, of the kind wanted (0: any), and its length */
static inline const WPP_HDR *wpp_r_begin( WPP_R *r, const void *buf, int n, int kind )
{
	const WPP_HDR *h = (const WPP_HDR *)buf;

	r->b = (const unsigned char *)buf;
	r->n = n;
	r->at = (int)sizeof(WPP_HDR);
	r->bad = 1;
	if ( n < (int)sizeof(WPP_HDR) || n > WPP_MAX || h->magic != WPP_MAGIC
	  || h->len != (unsigned int)( n - (int)sizeof(WPP_HDR) ) ) return 0;
	if ( kind != 0 && h->kind != kind ) return 0;
	r->bad = 0;
	return h;
}

static inline unsigned int wpp_r_u32( WPP_R *r )
{
	unsigned int v;

	if ( r->bad || r->n - r->at < 4 ) {
		r->bad = 1;
		return 0;
	}
	v = (unsigned int)r->b[r->at] | (unsigned int)r->b[r->at + 1] << 8
	  | (unsigned int)r->b[r->at + 2] << 16 | (unsigned int)r->b[r->at + 3] << 24;
	r->at += 4;
	return v;
}

static inline int wpp_r_type( WPP_R *r )
{
	if ( r->bad || r->at >= r->n ) {
		r->bad = 1;
		return 0;
	}
	return r->b[r->at++];
}

static inline int wpp_r_int( WPP_R *r )
{
	if ( wpp_r_type(r) != WPP_T_INT ) {
		r->bad = 1;
		return 0;
	}
	return (int)wpp_r_u32(r);
}

/*
 * Bytes where they are in the message (NULL for a null pointer), with
 * their length; at most 'max' of them, or the message is broken
 */
static inline const unsigned char *wpp_r_bytes( WPP_R *r, int *p_len, int max )
{
	unsigned int len;
	const unsigned char *p;
	int	t = wpp_r_type(r);

	*p_len = 0;
	if ( t == WPP_T_NULL ) return 0;
	if ( t != WPP_T_BYTES ) {
		r->bad = 1;
		return 0;
	}
	len = wpp_r_u32(r);
	if ( r->bad || len > (unsigned int)max || len > (unsigned int)( r->n - r->at ) ) {
		r->bad = 1;
		return 0;
	}
	p = r->b + r->at;
	r->at += (int)len;
	*p_len = (int)len;
	return p;
}

/* The bytes of an output copied to where the caller wants them (when it wants them) */
static inline void wpp_r_copy( WPP_R *r, void *dst, int *p_len, int max )
{
	int	len, i;
	const unsigned char *p = wpp_r_bytes(r, &len, max);
	unsigned char *d = (unsigned char *)dst;

	if ( p != 0 && d != 0 ) {
		for ( i = 0; i < len; i++ ) d[i] = p[i];
	}
	if ( p_len != 0 ) *p_len = len;
}

/* Zeros over a message that held secrets: not to be taken away as a dead store */
static inline void wpp_wipe( void *p, int n )
{
	volatile unsigned char *v = (volatile unsigned char *)p;

	while ( n-- > 0 ) *v++ = 0;
}

#endif /* __WPAPROC_H__ */
