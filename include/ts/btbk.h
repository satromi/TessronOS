/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	btbk.h
 *	BTRON backup archives (design 11.19)
 *
 *	A backup volume is one TAD record: a TS_INFO segment, the volume
 *	head in a 0xFFFD segment, and a 0xFFFD large segment for each
 *	object, or for the piece of an object the volume holds. An object
 *	segment keeps the object's name and F_STATE as they are, and its
 *	records as a stream compressed with LZSS: a 16 byte head for each
 *	record (or piece of one) followed by its bytes, a link record being
 *	written as a 146 byte descriptor instead of its LINK.
 *
 *	The library is plain C over callbacks: it makes no system call and
 *	allocates nothing, so the kernel and a program link the same files.
 *	The big state (the LZSS rings and hash chains) is in structures the
 *	caller places; none of it goes on the stack.
 *
 *	Everything in an archive is little-endian, TC strings included.
 */

#ifndef __TS_BTBK_H__
#define __TS_BTBK_H__

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* LZSS
 *
 * Tokens, by the first byte b0:
 *   0x00-0x1F  b0+1 literal bytes follow
 *   0x20-0xEF  b0 b1: w = b0<<8|b1, copy (w>>12)+1 bytes from w&0xFFF back
 *   0xF0-0xFF  b0 b1 b2: w = b1<<8|b2, copy ((b0&15)<<4|w>>12)+1 bytes
 *              from w&0xFFF back
 * There is no head and no end mark: the reader must know how much to
 * take. The encoder's output is fixed by its input alone (a window of
 * 4094 bytes, matches of 3 to 256 bytes, of two equal matches the
 * farther one), so the same bytes always compress to the same stream.
 */

#define BK_ENC_RING	0x2000
#define BK_DEC_RING	0x1000
#define BK_DEC_INBUF	0x400

/* Where output goes: answers 0, or an error that stops the encoder */
typedef INT (*BK_SINK)( void *arg, CONST UB *buf, INT len );
/* Where input comes from: answers the bytes given (0 at the end), or an error */
typedef INT (*BK_SRC)( void *arg, UB *buf, INT len );

typedef struct {
	BK_SINK	sink;
	void	*arg;
	INT	oldest;			/* position leaving the window */
	INT	cur;			/* position being encoded */
	INT	wp;			/* position input goes to */
	INT	outn;			/* literals in outbuf */
	INT	delay;			/* positions not yet in the chains */
	UB	outbuf[0x24];		/* one block: count, literals, match */
	UB	ring[BK_ENC_RING];
	UH	head[0x400];		/* oldest position + 1 of each chain */
	UH	tail[0x400];		/* newest position + 1 */
	UH	chain[BK_ENC_RING];	/* next newer position + 1 */
} BK_ENC;

typedef struct {
	BK_SRC	src;
	void	*arg;
	INT	rest;			/* input still to be taken from src */
	INT	rp, wp;			/* ring: drained up to, filled up to */
	INT	ip, iend;		/* in[] */
	UB	ring[BK_DEC_RING];
	UB	in[BK_DEC_INBUF];
} BK_DEC;

/*
 * The encoder. bk_enc_put takes input in any pieces (len 0 flushes);
 * bk_enc_flush sends what is held. A new stream starts with bk_enc_init.
 */
IMPORT void bk_enc_init( BK_ENC *enc, BK_SINK sink, void *arg );
IMPORT ER   bk_enc_put( BK_ENC *enc, CONST UB *buf, INT len );
IMPORT ER   bk_enc_flush( BK_ENC *enc );

/*
 * The decoder, over `inlen` bytes of input. bk_dec_read answers the
 * bytes it gave, fewer than asked only when the input ran out, or an
 * error of the source. bk_dec_done: all the input is used and nothing
 * expanded is waiting.
 */
IMPORT void bk_dec_init( BK_DEC *dec, BK_SRC src, void *arg, INT inlen );
IMPORT INT  bk_dec_read( BK_DEC *dec, UB *buf, INT len );
IMPORT BOOL bk_dec_done( CONST BK_DEC *dec );

/* Memory to memory: the length made, or E_LIMIT when out is too small */
IMPORT INT  bk_compress( BK_ENC *work, CONST UB *in, INT inlen, UB *out, INT outmax );
IMPORT INT  bk_decompress( BK_DEC *work, CONST UB *in, INT inlen, UB *out, INT outmax );

/* ------------------------------------------------------------------ */
/* The archive */

#define BK_SEG_INFO	0xFFE0		/* TS_INFO */
#define BK_SEG		0xFFFD		/* the segment the archive is made of */
#define BK_KIND_VOL	0xF0		/* volume head */
#define BK_KIND_VOL2	0xF2		/* volume head, totals past BK_BIG */
#define BK_KIND_OBJ	0xF1		/* object */
#define BK_MORE		0x8000		/* vol, seg: goes on in the next volume */
#define BK_BIG		0x60000000
#define BK_TADVER	0x0121		/* TAD version the head's TS_INFO gives */

#define BK_HEAD_FIXED	42		/* TS_INFO 10 + segment head 4 + 28 */
#define BK_MEMO_TC	80
#define BK_NAME_TC	20
#define BK_FSTATE_SIZE	96
#define BK_META_SIZE	136		/* name 40 + F_STATE 96 */
#define BK_OBJ_META	0x90		/* object segment after its length word */
#define BK_REC_HEAD	16
#define BK_LINK_SIZE	0x92		/* link descriptor */
#define BK_FLINK_SIZE	132

#define BK_FULL		1		/* bk_wr_object: the volume is full */

/* F_STATE as the archive keeps it */
typedef struct {
	UH	f_type;
	UH	f_atype;
	UH	f_owner[14];
	UH	f_group[14];
	UH	f_grpacc;
	UH	f_pubacc;
	H	f_nlink;
	H	f_index;
	INT	f_size;			/* bytes of the records other than links */
	INT	f_nblk;
	INT	f_nrec;
	UINT	f_ltime;
	UINT	f_atime;
	UINT	f_mtime;
	UINT	f_ctime;
} BK_FSTATE;

typedef struct {
	UB	kind;
	UH	vol;			/* index in the set, from 0 */
	BOOL	more;			/* the set goes on in the next volume */
	UD	total;			/* estimate of the whole set (bk_estimate) */
	UINT	nobj;			/* objects of the whole set */
	UD	src;			/* bytes the objects take on their disk */
	UH	memo[BK_MEMO_TC];
} BK_VOLHEAD;

typedef struct {
	UINT	objid;			/* f_id << 16 | index of its file system */
	UH	seg;			/* piece number, BK_MORE when it goes on */
	UB	cmp;			/* 1: the records are compressed */
	UINT	llen;
	UH	name[BK_NAME_TC];
	BK_FSTATE st;			/* of the whole object, in every piece */
	UB	meta[BK_META_SIZE];	/* name and F_STATE as stored */
} BK_OBJHEAD;

typedef struct {
	INT	recno;
	UH	type;
	UH	subtype;
	INT	offset;			/* where in the record this piece starts */
	INT	len;			/* bytes of this piece */
} BK_RECHEAD;

/* The descriptor a link record is written as */
typedef struct {
	UH	atr[5];			/* LINK atr1..atr5 */
	UINT	objid;			/* the target when it is in the set, else 0 */
	UINT	f_ctime;		/* F_LINK */
	UH	f_atype;
	UH	f_name[20];
	UH	f_id;
	UINT	rf_ctime;
	UH	fs_name[20];
	UH	fs_locat[20];
} BK_LINKDESC;

/*
 * What is known of an object while its pieces are read: kept by the
 * reader in a table the caller gives, one entry for each object of the
 * set, so that a piece in a later volume finds where the one before
 * stopped. `user` is the caller's.
 */
typedef struct {
	UINT	objid;
	INT	nrec;			/* f_nrec */
	INT	nrec_done;		/* records begun */
	INT	recoff;			/* bytes of the last record so far */
	BOOL	done;			/* its last piece has been read */
	void	*user;
} BK_FRAG;

typedef struct {
	BK_SRC	src;
	void	*arg;
	BK_FRAG	*frag;
	INT	nfrag, nused;
	INT	nvol;			/* volumes opened */
	BOOL	more;			/* the last volume opened goes on */
	BOOL	loose;			/* one volume looked at alone (below) */
	INT	state;
	BK_FRAG	*cur;			/* object being read */
	BOOL	cmp, last;
	INT	raw;			/* uncompressed stream left (cmp 0) */
	INT	body;			/* bytes of the record piece not read */
	UH	type;
	BK_DEC	dec;
} BK_RD;

/*
 * Reading a set of volumes, in order.
 *
 *   bk_rd_init	  the table of objects (see BK_FRAG)
 *   bk_rd_open	  the next volume: its head, checked to be the volume the
 *		  set wants now
 *   bk_rd_object the next object segment: 1, or 0 at the end of the
 *		  volume. *p_frag is its entry: a first piece makes one,
 *		  a later piece finds the one the earlier piece made
 *		  (E_NOEXS when there is none)
 *   bk_rd_record the next record piece of the object: 1, or 0 when the
 *		  object's stream has ended as its head said it would
 *   bk_rd_read	  bytes of a data record piece
 *   bk_rd_link	  the descriptor of a link record
 *   bk_rd_close  the set is whole: the last volume did not go on and
 *		  every object had its last piece
 *
 * Whatever the format does not allow is E_OBJ: a head of the wrong kind
 * or volume, records out of order, a piece of the wrong length, a stream
 * with bytes left over or too few.
 *
 * With `loose` set after bk_rd_init, the first volume may be any volume
 * of its set and a later piece whose earlier pieces were not read is
 * taken as it is, for looking at a volume without the rest of its set.
 * bk_rd_close still tells whether the set was whole.
 */
IMPORT void bk_rd_init( BK_RD *rd, BK_FRAG *frag, INT nfrag );
IMPORT ER   bk_rd_open( BK_RD *rd, BK_SRC src, void *arg, BK_VOLHEAD *vh );
IMPORT INT  bk_rd_object( BK_RD *rd, BK_OBJHEAD *oh, BK_FRAG **p_frag );
IMPORT INT  bk_rd_record( BK_RD *rd, BK_RECHEAD *rh );
IMPORT INT  bk_rd_read( BK_RD *rd, UB *buf, INT len );
IMPORT ER   bk_rd_link( BK_RD *rd, BK_LINKDESC *ld );
IMPORT ER   bk_rd_close( BK_RD *rd );

/* Where a volume's bytes go: `len` bytes at byte `off` of the volume */
typedef INT (*BK_OUT)( void *arg, UD off, CONST UB *buf, INT len );

/*
 * The records of one object, for the writer: how many, the type,
 * subtype and size of each, its bytes, and for a link record (type 0)
 * the descriptor to write.
 */
typedef struct {
	INT	nrec;
	ER	(*info)( void *arg, INT rec, UH *type, UH *subtype, INT *size );
	INT	(*read)( void *arg, INT rec, INT off, UB *buf, INT len );
	ER	(*link)( void *arg, INT rec, BK_LINKDESC *ld );
	void	*arg;
} BK_OBJSRC;

typedef struct {
	BK_OUT	out;
	void	*arg;
	UD	total, src;		/* the head's totals */
	UINT	nobj;
	UH	memo[BK_MEMO_TC];
	UH	vol;			/* volume being written */
	D	free;			/* what the volume still takes */
	UD	pos;			/* where the next byte goes */
	INT	objbytes;		/* the object segment so far */
	INT	resume_rec;		/* where the next piece starts */
	INT	resume_off;
	UH	segno;
	BOOL	wrote;			/* the volume has a record piece */
	UB	buf[0x800];
	BK_ENC	enc;
} BK_WR;

/*
 * Writing a set of volumes.
 *
 *   bk_wr_init	  the totals of the head (bk_estimate) and the memo
 *   bk_wr_begin  the next volume, with what it can hold in bytes
 *   bk_wr_object an object: E_OK, or BK_FULL when the volume is full;
 *		  then bk_wr_end(TRUE) closes it, bk_wr_begin opens the
 *		  next one and the same object is given again, to go on
 *		  where it stopped. E_LIMIT when a whole volume held
 *		  nothing of it.
 *   bk_wr_end	  the volume done; `more` marks it as not the last
 *
 * The room left is what the capacity less the bytes written, compressed,
 * leaves; a record goes only as far as that room less a thirty-second
 * of it, so a record may be cut into pieces within one volume, and a
 * piece of less than 1 KB is not begun. The pieces fall where the
 * format puts them for that capacity.
 */
IMPORT void bk_wr_init( BK_WR *wr, UD total, UINT nobj, UD src, CONST UH *memo );
IMPORT ER   bk_wr_begin( BK_WR *wr, BK_OUT out, void *arg, UD capacity );
IMPORT ER   bk_wr_object( BK_WR *wr, UINT objid, CONST UB *meta, CONST BK_OBJSRC *os );
IMPORT ER   bk_wr_end( BK_WR *wr, BOOL more );

/*
 * What one object adds to the head's totals: to `total` its records and
 * heads as the archive holds them before compression, to `src` the
 * blocks it takes on a disk of `bsize` byte blocks.
 */
IMPORT void bk_estimate( CONST BK_FSTATE *st, UINT bsize, UD *total, UD *src );

/* The name and F_STATE of an object in the stored form, and back */
IMPORT void bk_meta_put( UB *meta, CONST UH *name, CONST BK_FSTATE *st );
IMPORT void bk_meta_get( CONST UB *meta, UH *name, BK_FSTATE *st );

/* A link descriptor in the stored form, and back */
IMPORT void bk_link_put( UB *buf, CONST BK_LINKDESC *ld );
IMPORT void bk_link_get( CONST UB *buf, BK_LINKDESC *ld );

/* ------------------------------------------------------------------ */
/* xmlTAD to binary TAD
 *
 * A document or figure of xmlTAD as a TAD record a BTRON application
 * opens: TS_INFO, then TS_TEXT ... TS_TEXTEND or TS_FIG ... TS_FIGEND,
 * each nested document and figure framed the same way. The elements are
 * the ones lib/libbpk writes for the segments of a binary TAD, turned
 * back into those segments, so that a record taken through xmlTAD and
 * back reads as the same xmlTAD again; elements of TessronOS's own
 * (colours of lines and fills, text in a rectangle, <br/>) are written
 * as the nearest segments.
 *
 * The characters are TRON code: JIS X 0208 where it has them (ASCII as
 * its row 3, a space as the full-width one), the Unicode planes for the
 * rest. Every <link> is a TS_VOBJ, and hk->link is told of each in the
 * order they are written: the n-th TS_VOBJ is to take the object's n-th
 * link record. A picture (<image>, <pixelmap>) is asked of hk->image by
 * its href and written as a TS_IMAGE of 24-bit RGB with a mask where
 * there are pixels to leave out; without the hook, or when it cannot
 * say, the picture is left out and counted.
 *
 * The text is read where it lies: nothing is allocated and the state is
 * in a BK_TADW the caller places.
 */

#define BK_TAD_DEPTH	32		/* nesting of elements */
#define BK_TAD_PATS	48		/* colours a figure is given as patterns */
#define BK_TAD_DECO	32		/* decorations open at once */
#define BK_TAD_VERSION	0x0122		/* the TS_INFO this writes */
#define BK_PX_CLEAR	0xFFFFFFFFU	/* a pixel the image hook says is not there */

typedef struct {
	INT	n;			/* its place among the links, from 0 */
	CONST UB *tag;			/* the start tag, for bk_tad_attr */
	INT	taglen;
	UB	target[40];		/* the UUID its id names, "" when none */
	UH	attr;			/* VLINK attr: what its frame leaves out */
} BK_TADLINK;

typedef struct {
	/* each <link>, in order; an error stops the writing */
	ER	(*link)( void *arg, CONST BK_TADLINK *lk );
	/*
	 * The pixels of the picture href names: 0x00rrggbb, BK_PX_CLEAR where
	 * there is none, row after row, w by h. They are the hook's and need
	 * stay only until it is called again. E_NOEXS leaves the picture out.
	 */
	ER	(*image)( void *arg, CONST UB *href, CONST UINT **p_px, INT *p_w, INT *p_h );
	void	*arg;
} BK_TADHOOK;

typedef struct {
	INT	nlink;			/* TS_VOBJ written */
	INT	nimage;			/* TS_IMAGE written */
	INT	noimage;		/* pictures left out */
	INT	nskip;			/* elements left out */
	INT	nchar;			/* characters written */
	INT	nuni;			/* of them, in a Unicode plane */
} BK_TADSTAT;

/* A decoration open in the text: a TS_TSTYLE or TS_TATTR range */
typedef struct {
	UH	seg;
	UB	sub;			/* the start's sub id */
	UB	attr;
	UH	w1;			/* its word after the first, when it has one */
	UINT	hash;			/* of the ruby's text */
	CONST UB *tag;			/* the start tag it came from */
	INT	taglen;
	INT	serial;			/* which element opened it */
	BOOL	real;			/* a range of its own, not one opened again */
} BK_TADDECO;

/* One open element */
typedef struct {
	CONST UB *name;			/* in the text, for its end tag */
	INT	nlen;
	UB	kind;
	INT	serial;			/* the decoration it opened */
	UH	npara;			/* paragraphs begun in it (a text) */
	INT	plane;			/* the plane before it (a text) */
	UH	size, fatr, fatr_out, hr, wr;	/* the letters before it */
	UINT	color;
	INT	deco_base;		/* where its decorations begin (a text) */
	INT	npat;			/* the patterns before it (a figure) */
	INT	hunit, vunit;
	INT	segat;			/* where its segment begins (an overlay's text) */
} BK_TADLV;

typedef struct {
	UB	*out;
	INT	max, len;
	INT	plane;
	UH	size, fatr, fatr_out, hr, wr;
	UINT	color;
	INT	depth;
	BK_TADLV lv[BK_TAD_DEPTH];
	BK_TADDECO want[BK_TAD_DECO];	/* the decorations the text is in */
	BK_TADDECO have[BK_TAD_DECO];	/* the ones the record has opened */
	BK_TADDECO closed[BK_TAD_DECO];	/* the ones closed just now */
	INT	nwant, nhave, nclosed, deco_base, serial;
	BOOL	flushing;
	INT	npat, patnext;
	UINT	patcol[BK_TAD_PATS];
	UH	patid[BK_TAD_PATS];
	BOOL	fmod;			/* arrows were given by <figmodifier> */
	INT	pend;			/* a new paragraph or a link waiting ... */
	CONST UB *pend_p;		/* ... for the decorations after it */
	INT	pend_n;
	CONST BK_TADHOOK *hk;
	BK_TADSTAT st;
	INT	absorb;			/* elements after a table's cell that it says itself */
	ER	er;
} BK_TADW;

/*
 * xml[0..len) as binary TAD into out[0..max). Answers E_OK and the length
 * in *p_len; with out NULL only the length is found (E_NOMEM when out was
 * given too small). E_PAR for text that is not xmlTAD, or an error a hook
 * gave. hk may be NULL.
 */
IMPORT ER   bk_tad_from_xml( BK_TADW *w, CONST UB *xml, INT len, UB *out, INT max,
			     INT *p_len, CONST BK_TADHOOK *hk, BK_TADSTAT *st );

/* An attribute of a start tag, entities undone: its length, or -1 when it is not there */
IMPORT INT  bk_tad_attr( CONST UB *tag, INT taglen, CONST char *name, UB *buf, INT max );

/*
 * A TAD record walked segment by segment: every length must land on a
 * segment or the record's end, TS_TEXT and TS_FIG must close in order,
 * and TS_INFO must come first. E_OK and how many segments and characters
 * it holds, or E_OBJ.
 */
IMPORT ER   bk_tad_check( CONST UB *tad, INT len, INT *p_nseg, INT *p_nchar );

#ifdef __cplusplus
}
#endif

#endif /* __TS_BTBK_H__ */
