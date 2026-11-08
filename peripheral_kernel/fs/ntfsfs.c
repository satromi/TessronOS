/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ntfsfs.c
 *	NTFS file system implementation (fimp), design 7.6.
 *
 *	Reading: names, directories (the index of file names, in its record
 *	and in its blocks), and the data of a file, kept in its record,
 *	spread over runs of clusters, sparse or compressed (LZNT1). A
 *	record's attributes may continue in other records (an attribute
 *	list). An encrypted file is listed, and its data not given.
 *
 *	Writing is limited to what cannot leave the volume inconsistent:
 *	bytes of a file that already has room for them are replaced where
 *	they are, and the time of the file with them. NTFS does not put its
 *	files' data in the journal, so this leaves the journal as it is and
 *	changes no structure: no record or cluster is taken or given back,
 *	no index changes. Everything that would have to go through the
 *	journal -- making, removing or renaming a file, changing its size --
 *	answers EX_NOTSUP, and so does a write to a run that is compressed,
 *	sparse or encrypted, or past the part of the file that was
 *	initialized.
 *
 *	Even that is done only on a volume that is safe: NTFS 3.x, not
 *	marked dirty, its journal clean (nothing Windows would play back over
 *	what is written here) and not hibernated (hiberfil.sys in use: a
 *	Windows that sleeps, fast start-up included, holds the volume's state
 *	in memory and would write it back). Any other volume is served for
 *	reading alone.
 *
 *	What is on the medium is not trusted: every length and offset read
 *	from it is checked before it is followed.
 *
 *	One volume is served at a time through a mutex.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/proc.h>
#include "fsname.h"

#define SECT			512

#define INO_MFT			0
#define INO_LOGFILE		2
#define INO_VOLUME		3
#define INO_ROOT		5
#define INO_UPCASE		10
#define INO_FIRST_USER		24

#define AT_STDINFO		0x10
#define AT_LIST			0x20
#define AT_NAME			0x30
#define AT_VOLINFO		0x70
#define AT_DATA			0x80
#define AT_IROOT		0x90
#define AT_IALLOC		0xA0
#define AT_BITMAP		0xB0
#define AT_END			0xFFFFFFFFU

#define AF_COMPRESSED		0x00FF
#define AF_ENCRYPTED		0x4000
#define AF_SPARSE		0x8000

#define REC_INUSE		0x0001
#define REC_DIR			0x0002

#define IE_SUB			0x0001
#define IE_LAST			0x0002

#define FN_DIRFLAG		0x10000000U
#define NS_DOS			2

#define VOL_DIRTY		0x0001

#define RUN_SPARSE		( (D)-1 )
#define NAME_MAX16		255
#define XFER_MAX		2048		/* sectors of one request */
#define DEPTH_MAX		32		/* of an index */
#define CU_MAX			( 64 * 1024 * 16 )	/* the largest compression unit */

/* Seconds from 1601, where the times of NTFS count from, to 1985 */
#define FT_TO_TRON		12117859200ULL

typedef struct {
	UD	vcn;
	UD	len;
	D	lcn;		/* RUN_SPARSE: a hole */
} NRUN;

/* An attribute's value, wherever it is */
typedef struct {
	BOOL	resident;
	UB	*value;		/* resident: a copy */
	UD	size, init, alloc;
	UINT	flags;
	UINT	cu_shift;	/* clusters in a compression unit, as a shift */
	NRUN	*runs;
	INT	nruns, cap;
	UD	rec;		/* resident: the record it is in */
	UINT	val_off;	/* and where in it */
} NATTR;

typedef struct {
	ID	dd, mtxid;
	UD	csize, recsize, nclus;
	UD	idxsize;
	BOOL	ro;
	NATTR	mft;
	UH	*upcase;
	UB	*rec, *rec2;	/* records being looked at */
	UB	sb[SECT];
	UH	nm16[NAME_MAX16 + 1];
} NVOL;

typedef struct {
	NATTR	a;
	UD	ino;
	BOOL	isdir;
	BOOL	touched;
	/* a directory's listing */
	NATTR	iroot, ialloc, ibm;
	BOOL	has_alloc, has_bm;
	UB	*ib;
	UD	ibs;
	/* the last compression unit made */
	UB	*cu;
	D	cu_at;
} NFILE;

LOCAL UH rd16( CONST UB *p ) { return (UH)( p[0] | ( p[1] << 8 ) ); }
LOCAL UW rd32( CONST UB *p ) { return (UW)p[0] | ( (UW)p[1] << 8 ) | ( (UW)p[2] << 16 ) | ( (UW)p[3] << 24 ); }
LOCAL UD rd64( CONST UB *p ) { return (UD)rd32(p) | ( (UD)rd32(p + 4) << 32 ); }
LOCAL void wr16( UB *p, UH v ) { p[0] = (UB)v; p[1] = (UB)( v >> 8 ); }
LOCAL void wr64( UB *p, UD v )
{
	INT	i;

	for ( i = 0; i < 8; i++ ) p[i] = (UB)( v >> ( i * 8 ) );
}

/* ---------------------------------------------------------------- the medium */

LOCAL ER dev_read( NVOL *v, UD sect, void *buf, UD n )
{
	SZ	asz;

	return ( tk_srea_dev_d(v->dd, (D)sect, buf, (SZ)n * SECT, &asz) < E_OK ) ? EX_IO : EX_OK;
}

LOCAL ER dev_write( NVOL *v, UD sect, CONST void *buf, UD n )
{
	SZ	asz;

	return ( tk_swri_dev_d(v->dd, (D)sect, buf, (SZ)n * SECT, &asz) < E_OK ) ? EX_IO : EX_OK;
}

/* ---------------------------------------------------------------- runs and values */

LOCAL void attr_free( NATTR *a )
{
	if ( a->value != NULL ) Kfree(a->value);
	if ( a->runs != NULL ) Kfree(a->runs);
	knl_memset(a, 0, sizeof(*a));
}

LOCAL ER run_add( NATTR *a, UD vcn, UD len, D lcn )
{
	if ( a->nruns == a->cap ) {
		INT	cap = ( a->cap == 0 ) ? 16 : a->cap * 2;
		NRUN	*r = (NRUN *)Kmalloc(sizeof(NRUN) * (SZ)cap);

		if ( r == NULL ) return EX_NOMEM;
		if ( a->nruns > 0 ) knl_memcpy(r, a->runs, sizeof(NRUN) * (SZ)a->nruns);
		if ( a->runs != NULL ) Kfree(a->runs);
		a->runs = r;
		a->cap = cap;
	}
	a->runs[a->nruns].vcn = vcn;
	a->runs[a->nruns].len = len;
	a->runs[a->nruns].lcn = lcn;
	a->nruns++;
	return EX_OK;
}

/* The mapping pairs of one extent of an attribute, [p, end) */
LOCAL ER runs_decode( NVOL *v, NATTR *a, CONST UB *p, CONST UB *end, UD vcn )
{
	D	lcn = 0;
	ER	er;

	while ( p < end && *p != 0 ) {
		UINT	nl = *p & 0xF, no = *p >> 4, i;
		UD	len = 0;
		D	off = 0;

		p++;
		if ( nl == 0 || nl > 8 || no > 8 || p + nl + no > end ) return EX_IO;
		for ( i = 0; i < nl; i++ ) len |= (UD)p[i] << ( i * 8 );
		p += nl;
		if ( no > 0 ) {
			for ( i = 0; i < no; i++ ) off |= (D)( (UD)p[i] << ( i * 8 ) );
			if ( no < 8 && ( p[no - 1] & 0x80 ) != 0 ) off |= (D)( ~(UD)0 << ( no * 8 ) );
			p += no;
			lcn += off;
			if ( lcn < 0 || (UD)lcn + len > v->nclus || len > v->nclus ) return EX_IO;
			er = run_add(a, vcn, len, lcn);
		} else {
			if ( len > ( (UD)1 << 48 ) ) return EX_IO;
			er = run_add(a, vcn, len, RUN_SPARSE);
		}
		if ( er < EX_OK ) return er;
		vcn += len;
	}
	return EX_OK;
}

/* How many bytes from byte offset 'off' lie together: *plcn_byte the place, -1 for a hole */
LOCAL ER run_find( NVOL *v, NATTR *a, UD off, UD *pcontig, D *pbyte )
{
	UD	vcn = off / v->csize;
	INT	lo = 0, hi = a->nruns - 1;

	while ( lo <= hi ) {
		INT	m = ( lo + hi ) / 2;
		NRUN	*r = &a->runs[m];

		if ( vcn < r->vcn ) {
			hi = m - 1;
		} else if ( vcn >= r->vcn + r->len ) {
			lo = m + 1;
		} else {
			UD	in = off - r->vcn * v->csize;

			*pcontig = r->len * v->csize - in;
			*pbyte = ( r->lcn == RUN_SPARSE ) ? (D)-1 : (D)( (UD)r->lcn * v->csize + in );
			return EX_OK;
		}
	}
	return EX_IO;
}

/* Bytes of a value as it lies, not decompressed; holes read as zeros. Any offset and length */
LOCAL ER raw_read( NVOL *v, NATTR *a, UD off, UD len, UB *buf )
{
	if ( a->resident ) {
		if ( off + len > a->size ) return EX_IO;
		knl_memcpy(buf, a->value + off, (SZ)len);
		return EX_OK;
	}
	while ( len > 0 ) {
		UD	cont, n;
		D	at;
		ER	er;

		er = run_find(v, a, off, &cont, &at);
		if ( er < EX_OK ) return er;
		n = ( cont < len ) ? cont : len;
		if ( at < 0 ) {
			knl_memset(buf, 0, (SZ)n);
		} else if ( ( (UD)at & ( SECT - 1 ) ) == 0 && n >= SECT ) {
			n &= ~(UD)( SECT - 1 );
			if ( n > (UD)XFER_MAX * SECT ) n = (UD)XFER_MAX * SECT;
			er = dev_read(v, (UD)at / SECT, buf, n / SECT);
			if ( er < EX_OK ) return er;
		} else {
			UD	in = (UD)at & ( SECT - 1 );

			if ( n > SECT - in ) n = SECT - in;
			er = dev_read(v, (UD)at / SECT, v->sb, 1);
			if ( er < EX_OK ) return er;
			knl_memcpy(buf, v->sb + in, (SZ)n);
		}
		buf += n;
		off += n;
		len -= n;
	}
	return EX_OK;
}

/* The update sequence put back: FALSE when the block was torn */
LOCAL BOOL fixup_apply( UB *b, UD size )
{
	UINT	uo = rd16(b + 4), uc = rd16(b + 6), i;

	if ( uc == 0 || uo + uc * 2 > size || (UD)( uc - 1 ) * SECT > size ) {
		return FALSE;
	}
	for ( i = 1; i < uc; i++ ) {
		UB	*e = b + i * SECT - 2;

		if ( rd16(e) != rd16(b + uo) ) return FALSE;
		wr16(e, rd16(b + uo + i * 2));
	}
	return TRUE;
}

/* And taken out again, for writing it */
LOCAL void fixup_undo( UB *b, UD size )
{
	UINT	uo = rd16(b + 4), uc = rd16(b + 6), i;

	(void)size;
	for ( i = 1; i < uc; i++ ) {
		UB	*e = b + i * SECT - 2;

		wr16(b + uo + i * 2, rd16(e));
		wr16(e, rd16(b + uo));
	}
}

LOCAL ER rec_read( NVOL *v, UD ino, UB *buf )
{
	UD	off = ino * v->recsize;
	ER	er;

	if ( off + v->recsize > v->mft.size ) return EX_NOENT;
	er = raw_read(v, &v->mft, off, v->recsize, buf);
	if ( er < EX_OK ) return er;
	if ( buf[0] != 'F' || buf[1] != 'I' || buf[2] != 'L' || buf[3] != 'E' || !fixup_apply(buf, v->recsize) ) {
		return EX_IO;
	}
	return EX_OK;
}

/*
 * The next attribute of a record after *pp (NULL: the first); NULL at
 * the end, or when its length does not stay inside the record.
 */
LOCAL UB *attr_next( NVOL *v, UB *rec, UB *at )
{
	UB	*end = rec + v->recsize;
	UD	used = rd32(rec + 24);

	if ( used <= v->recsize ) end = rec + used;
	if ( at == NULL ) {
		UINT	off = rd16(rec + 20);

		if ( off + 16 > v->recsize ) return NULL;
		at = rec + off;
	} else {
		UW	l = rd32(at + 4);

		if ( l < 16 || (UD)l > (UD)( end - at ) ) return NULL;
		at += l;
	}
	if ( at + 8 > end || rd32(at) == AT_END ) return NULL;
	{
		UW	l = rd32(at + 4);

		if ( l < 16 || (UD)l > (UD)( end - at ) ) return NULL;
	}
	return at;
}

LOCAL BOOL attr_name_is( CONST UB *at, CONST UH *name, INT nlen )
{
	INT	i;

	if ( at[9] != nlen ) return FALSE;
	if ( nlen == 0 ) return TRUE;
	if ( (UD)rd16(at + 10) + (UD)nlen * 2 > (UD)rd32(at + 4) ) return FALSE;
	for ( i = 0; i < nlen; i++ ) {
		if ( rd16(at + rd16(at + 10) + i * 2) != name[i] ) return FALSE;
	}
	return TRUE;
}

/* One extent of an attribute taken in */
LOCAL ER attr_take( NVOL *v, UB *at, UD rec, NATTR *a, BOOL first )
{
	UW	alen = rd32(at + 4);

	if ( at[8] == 0 ) {
		UW	vl = rd32(at + 16);
		UINT	vo = rd16(at + 20);

		if ( alen < 24 || (UD)vo + vl > alen ) return EX_IO;
		a->resident = TRUE;
		a->size = a->init = a->alloc = vl;
		a->flags = rd16(at + 12);
		a->rec = rec;
		a->val_off = (UINT)( ( at - v->rec2 ) + vo );
		if ( a->value != NULL ) Kfree(a->value);
		a->value = (UB *)Kmalloc((SZ)vl + 1);
		if ( a->value == NULL ) return EX_NOMEM;
		knl_memcpy(a->value, at + vo, vl);
		return EX_OK;
	}
	if ( alen < 64 ) return EX_IO;
	if ( first ) {
		a->size = rd64(at + 48);
		a->alloc = rd64(at + 40);
		a->init = rd64(at + 56);
		a->flags = rd16(at + 12);
		a->cu_shift = rd16(at + 34);
		if ( a->init > a->size ) a->init = a->size;
	}
	{
		UINT	ro = rd16(at + 32);

		if ( ro >= alen ) return EX_IO;
		return runs_decode(v, a, at + ro, at + alen, rd64(at + 16));
	}
}

/*
 * An attribute of a file, with its extents in whatever records they
 * are. EX_NOENT when it has none.
 */
LOCAL ER attr_load( NVOL *v, UD ino, UINT type, CONST UH *name, INT nlen, NATTR *a )
{
	UB	*at;
	UB	*list = NULL;
	UD	list_len = 0;
	BOOL	got = FALSE;
	ER	er;

	knl_memset(a, 0, sizeof(*a));
	er = rec_read(v, ino, v->rec2);
	if ( er < EX_OK ) return er;
	if ( ( rd16(v->rec2 + 22) & REC_INUSE ) == 0 ) return EX_NOENT;

	/* a list, when there is one, says where every attribute is */
	for ( at = NULL; ( at = attr_next(v, v->rec2, at) ) != NULL; ) {
		if ( rd32(at) == AT_LIST ) {
			NATTR	l;

			knl_memset(&l, 0, sizeof(l));
			er = attr_take(v, at, ino, &l, TRUE);
			if ( er >= EX_OK ) {
				list_len = l.size;
				if ( list_len > 16 * 1024 * 1024 ) {
					er = EX_IO;
				} else {
					list = (UB *)Kmalloc((SZ)list_len + 1);
					er = ( list == NULL ) ? EX_NOMEM : raw_read(v, &l, 0, list_len, list);
				}
			}
			attr_free(&l);
			if ( er < EX_OK ) {
				if ( list != NULL ) Kfree(list);
				return er;
			}
			break;
		}
	}
	if ( list == NULL ) {
		for ( at = NULL; ( at = attr_next(v, v->rec2, at) ) != NULL; ) {
			if ( rd32(at) == type && attr_name_is(at, name, nlen) ) {
				er = attr_take(v, at, ino, a, !got);
				if ( er < EX_OK ) {
					attr_free(a);
					return er;
				}
				got = TRUE;
			}
		}
		if ( !got ) return EX_NOENT;
		return EX_OK;
	}
	{
		UD	o = 0;

		while ( o + 26 <= list_len ) {
			UB	*e = list + o;
			UINT	el = rd16(e + 4);

			if ( el < 26 || o + el > list_len ) break;
			if ( rd32(e) == type && e[6] == nlen && (UD)e[7] + (UD)nlen * 2 <= el ) {
				INT	i;
				BOOL	same = TRUE;

				for ( i = 0; i < nlen; i++ ) {
					if ( rd16(e + e[7] + i * 2) != name[i] ) same = FALSE;
				}
				if ( same ) {
					UD	r = rd64(e + 16) & 0xFFFFFFFFFFFFULL;
					UINT	id = rd16(e + 24);

					er = rec_read(v, r, v->rec2);
					if ( er < EX_OK ) goto fail;
					for ( at = NULL; ( at = attr_next(v, v->rec2, at) ) != NULL; ) {
						if ( rd32(at) == type && rd16(at + 14) == id ) {
							er = attr_take(v, at, r, a, !got);
							if ( er < EX_OK ) goto fail;
							got = TRUE;
							break;
						}
					}
				}
			}
			o += el;
		}
	}
	Kfree(list);
	if ( !got ) return EX_NOENT;
	return EX_OK;

    fail:
	Kfree(list);
	attr_free(a);
	return er;
}

/* ---------------------------------------------------------------- names and indexes */

LOCAL INT name_cmp( NVOL *v, CONST UH *a, INT an, CONST UB *b, INT bn )
{
	INT	i, n = ( an < bn ) ? an : bn;

	for ( i = 0; i < n; i++ ) {
		UH	x = v->upcase[a[i]], y = v->upcase[rd16(b + i * 2)];

		if ( x != y ) return ( x < y ) ? -1 : 1;
	}
	return ( an == bn ) ? 0 : ( an < bn ) ? -1 : 1;
}

/*
 * Whether an entry of an index stays within [p, end). Its file name, if
 * it has one, is left for the caller to check with entry_name.
 */
LOCAL BOOL entry_ok( CONST UB *p, CONST UB *end )
{
	UINT	l;

	if ( p + 16 > end ) return FALSE;
	l = rd16(p + 8);
	return (BOOL)( l >= 16 && (UD)l <= (UD)( end - p ) );
}

LOCAL BOOL entry_name( CONST UB *p )
{
	UINT	kl = rd16(p + 10);

	return (BOOL)( kl >= 66 && (UD)p[16 + 64] * 2 + 66 <= kl && (UD)kl + 16 <= rd16(p + 8) );
}

/* The entry of a name in a list of entries; else *psub the block to go down to, or -1 */
LOCAL BOOL entries_find( NVOL *v, UB *p, UB *end, CONST UH *nm, INT n, UD *pino, D *psub )
{
	*psub = -1;
	while ( entry_ok(p, end) ) {
		UINT	fl = rd16(p + 12);
		UINT	el = rd16(p + 8);

		if ( ( fl & IE_LAST ) == 0 && entry_name(p) ) {
			INT	c = name_cmp(v, nm, n, p + 16 + 66, p[16 + 64]);

			if ( c == 0 ) {
				*pino = rd64(p) & 0xFFFFFFFFFFFFULL;
				return TRUE;
			}
			if ( c < 0 ) {
				if ( ( fl & IE_SUB ) != 0 && el >= 24 ) *psub = (D)rd64(p + el - 8);
				return FALSE;
			}
		} else if ( ( fl & IE_LAST ) != 0 ) {
			if ( ( fl & IE_SUB ) != 0 && el >= 24 ) *psub = (D)rd64(p + el - 8);
			return FALSE;
		}
		p += el;
	}
	return FALSE;
}

/* Where a block of an index is: VCNs count clusters, or sectors in a block smaller than a cluster */
LOCAL UD ib_off( NVOL *v, UD ibs, UD vcn )
{
	return vcn * ( ( ibs >= v->csize ) ? v->csize : SECT );
}

/* The entries of a block read: [*pp, *pend) */
LOCAL ER ib_read( NVOL *v, NATTR *ia, UD ibs, UD off, UB *ib, UB **pp, UB **pend )
{
	UD	eo, sz;
	ER	er;

	er = raw_read(v, ia, off, ibs, ib);
	if ( er < EX_OK ) return er;
	if ( ib[0] != 'I' || ib[1] != 'N' || ib[2] != 'D' || ib[3] != 'X' || !fixup_apply(ib, ibs) ) {
		return EX_IO;
	}
	eo = rd32(ib + 24);
	sz = rd32(ib + 28);
	if ( 24 + eo >= ibs || sz > ibs - 24 || eo >= sz ) return EX_IO;
	*pp = ib + 24 + eo;
	*pend = ib + 24 + sz;
	return EX_OK;
}

LOCAL CONST UH I30[4] = { '$', 'I', '3', '0' };

/* The file of a name in a directory */
LOCAL ER dir_find( NVOL *v, UD dir, CONST UH *nm, INT n, UD *pino )
{
	NATTR	ir, ia;
	UB	*ib = NULL;
	UB	*p, *end;
	D	sub;
	UD	ibs, eo, sz;
	INT	depth;
	ER	er;

	er = attr_load(v, dir, AT_IROOT, I30, 4, &ir);
	if ( er < EX_OK ) return er;
	knl_memset(&ia, 0, sizeof(ia));
	if ( !ir.resident || ir.size < 32 ) {
		er = EX_IO;
		goto out;
	}
	ibs = rd32(ir.value + 8);
	eo = rd32(ir.value + 16);
	sz = rd32(ir.value + 20);
	if ( 16 + eo >= ir.size || sz > ir.size - 16 ) {
		er = EX_IO;
		goto out;
	}
	p = ir.value + 16 + eo;
	end = ir.value + 16 + sz;
	if ( entries_find(v, p, end, nm, n, pino, &sub) ) {
		er = EX_OK;
		goto out;
	}
	if ( sub < 0 ) {
		er = EX_NOENT;
		goto out;
	}
	if ( ibs < SECT || ibs > 1024 * 1024 || ( ibs & ( SECT - 1 ) ) != 0 ) {
		er = EX_IO;
		goto out;
	}
	er = attr_load(v, dir, AT_IALLOC, I30, 4, &ia);
	if ( er < EX_OK ) goto out;
	ib = (UB *)Kmalloc((SZ)ibs);
	if ( ib == NULL ) {
		er = EX_NOMEM;
		goto out;
	}
	for ( depth = 0; depth < DEPTH_MAX; depth++ ) {
		er = ib_read(v, &ia, ibs, ib_off(v, ibs, (UD)sub), ib, &p, &end);
		if ( er < EX_OK ) goto out;
		if ( entries_find(v, p, end, nm, n, pino, &sub) ) {
			er = EX_OK;
			goto out;
		}
		if ( sub < 0 ) {
			er = EX_NOENT;
			goto out;
		}
	}
	er = EX_IO;

    out:
	if ( ib != NULL ) Kfree(ib);
	attr_free(&ia);
	attr_free(&ir);
	return er;
}

LOCAL ER path_walk( NVOL *v, CONST char *path, UD *pino )
{
	CONST char *p = path;
	UD	cur = INO_ROOT;

	if ( *p != '/' ) return EX_INVAL;
	for ( ;; ) {
		CONST char *c;
		INT	len, n;
		ER	er;

		while ( *p == '/' ) p++;
		if ( *p == 0 ) break;
		c = p;
		while ( *p != 0 && *p != '/' ) p++;
		len = (INT)( p - c );
		if ( len == 1 && c[0] == '.' ) continue;
		if ( len == 2 && c[0] == '.' && c[1] == '.' ) return EX_INVAL;
		n = knl_fs_utf8_to_16((CONST UB *)c, len, v->nm16, NAME_MAX16);
		if ( n == -2 ) return EX_NAMETOOLONG;
		if ( n <= 0 ) return EX_INVAL;
		er = dir_find(v, cur, v->nm16, n, &cur);
		if ( er < EX_OK ) return er;
	}
	*pino = cur;
	return EX_OK;
}

/* ---------------------------------------------------------------- what a file says of itself */

LOCAL ER file_info( NVOL *v, UD ino, BOOL *pisdir, UD *pmtime )
{
	UB	*at;
	ER	er;

	er = rec_read(v, ino, v->rec);
	if ( er < EX_OK ) return er;
	if ( ( rd16(v->rec + 22) & REC_INUSE ) == 0 ) return EX_NOENT;
	*pisdir = (BOOL)( ( rd16(v->rec + 22) & REC_DIR ) != 0 );
	*pmtime = 0;
	for ( at = NULL; ( at = attr_next(v, v->rec, at) ) != NULL; ) {
		if ( rd32(at) == AT_STDINFO && at[8] == 0 ) {
			UINT	vo = rd16(at + 20);

			if ( rd32(at + 16) >= 16 && (UD)vo + 16 <= rd32(at + 4) ) {
				UD	ft = rd64(at + vo + 8) / 10000000ULL;

				*pmtime = ( ft > FT_TO_TRON ) ? ft - FT_TO_TRON : 0;
			}
			break;
		}
	}
	return EX_OK;
}

/* ---------------------------------------------------------------- mounting */

/* Whether the journal holds nothing to play back. All 0xFF is one that was emptied */
LOCAL ER log_clean( NVOL *v, BOOL *pclean )
{
	NATTR	l;
	UB	*pg;
	UD	best_lsn = 0, off, psize = 4096;
	BOOL	any = FALSE, clean = TRUE, blank = TRUE;
	static CONST UH noname[1] = { 0 };
	ER	er;

	*pclean = FALSE;
	er = attr_load(v, INO_LOGFILE, AT_DATA, noname, 0, &l);
	if ( er < EX_OK ) return er;
	pg = (UB *)Kmalloc((SZ)64 * 1024);
	if ( pg == NULL || l.size < 2 * psize ) {
		attr_free(&l);
		if ( pg != NULL ) Kfree(pg);
		return ( pg == NULL ) ? EX_NOMEM : EX_IO;
	}
	for ( off = 0; off < 2 * psize && er >= EX_OK; off += psize ) {
		UD	i;

		er = raw_read(v, &l, off, psize, pg);
		if ( er < EX_OK ) break;
		for ( i = 0; i < psize && pg[i] == 0xFF; i++ ) ;
		if ( i == psize ) continue;
		blank = FALSE;
		if ( pg[0] != 'R' || pg[1] != 'S' || pg[2] != 'T' || pg[3] != 'R' ) continue;
		{
			UD	sys = rd32(pg + 16);
			UINT	ra = rd16(pg + 24);

			if ( sys >= 512 && sys <= 64 * 1024 && ( sys & ( sys - 1 ) ) == 0 && sys != psize ) {
				psize = sys;		/* the journal's own page: read it whole and start over */
				if ( l.size < 2 * psize ) {
					er = EX_IO;
					break;
				}
				off = (UD)0 - psize;
				any = FALSE;
				clean = TRUE;
				blank = TRUE;
				best_lsn = 0;
				continue;
			}
			if ( !fixup_apply(pg, psize) || (UD)ra + 16 > psize ) continue;
			{
				UD	lsn = rd64(pg + ra);
				UINT	inuse = rd16(pg + ra + 12), fl = rd16(pg + ra + 14);

				if ( !any || lsn >= best_lsn ) {
					best_lsn = lsn;
					clean = (BOOL)( inuse == 0xFFFF || ( fl & 0x0002 ) != 0 );
				}
				any = TRUE;
			}
		}
	}
	attr_free(&l);
	Kfree(pg);
	if ( er < EX_OK ) return er;
	*pclean = (BOOL)( blank || ( any && clean ) );
	return EX_OK;
}

/* Whether hiberfil.sys holds a machine asleep */
LOCAL ER hibernated( NVOL *v, BOOL *phib )
{
	static CONST UH nm[12] = { 'h','i','b','e','r','f','i','l','.','s','y','s' };
	static CONST UH noname[1] = { 0 };
	NATTR	a;
	UD	ino;
	UB	h[8];
	ER	er;

	*phib = FALSE;
	er = dir_find(v, INO_ROOT, nm, 12, &ino);
	if ( er == EX_NOENT ) return EX_OK;
	if ( er < EX_OK ) return er;
	er = attr_load(v, ino, AT_DATA, noname, 0, &a);
	if ( er == EX_NOENT ) return EX_OK;
	if ( er < EX_OK ) return er;
	if ( a.size >= 4 ) {
		er = raw_read(v, &a, 0, 4, h);
		if ( er >= EX_OK ) {
			UINT	i;
			BOOL	zero = TRUE;

			for ( i = 0; i < 4; i++ ) {
				if ( h[i] != 0 ) zero = FALSE;
			}
			/* a file left with nothing in it says nothing is asleep; anything else is taken to */
			*phib = (BOOL)!zero;
		}
	}
	attr_free(&a);
	return er;
}

LOCAL ER safe_to_write( NVOL *v, T_MOUNT *m )
{
	static CONST UH noname[1] = { 0 };
	NATTR	a;
	BOOL	clean, hib;
	UINT	major = 0, flags = 0;
	ER	er;

	er = attr_load(v, INO_VOLUME, AT_VOLINFO, noname, 0, &a);
	if ( er < EX_OK ) return er;
	if ( a.resident && a.size >= 12 ) {
		major = a.value[8];
		flags = rd16(a.value + 10);
	}
	attr_free(&a);
	if ( major != 3 || ( flags & VOL_DIRTY ) != 0 ) {
		tm_printf((UB *)"ntfsfs: %s: version %d, flags %x: read only\n", m->dev, major, flags);
		return EX_ROFS;
	}
	er = log_clean(v, &clean);
	if ( er < EX_OK ) return er;
	if ( !clean ) {
		tm_printf((UB *)"ntfsfs: %s: its journal has not been played back: read only\n", m->dev);
		return EX_ROFS;
	}
	er = hibernated(v, &hib);
	if ( er < EX_OK ) return er;
	if ( hib ) {
		tm_printf((UB *)"ntfsfs: %s: Windows is hibernated on it (or fast start-up): read only\n", m->dev);
		return EX_ROFS;
	}
	return EX_OK;
}

LOCAL ER nt_mount( T_MOUNT *m )
{
	static CONST UH noname[1] = { 0 };
	NVOL	*v;
	UB	*b;
	UB	*at;
	UINT	bps, spc;
	INT	cpr, cpi;
	UD	total, mftlcn, i;
	T_CMTX	cm;
	NATTR	full;
	ER	er;

	v = (NVOL *)Kmalloc(sizeof(NVOL));
	if ( v == NULL ) return EX_NOMEM;
	knl_memset(v, 0, sizeof(*v));
	v->dd = m->dd;
	v->mtxid = -1;
	b = v->sb;
	er = dev_read(v, 0, b, 1);
	if ( er < EX_OK ) goto err;
	if ( b[3] != 'N' || b[4] != 'T' || b[5] != 'F' || b[6] != 'S' || rd16(b + 510) != 0xAA55 ) {
		er = EX_INVAL;
		goto err;
	}
	bps = rd16(b + 11);
	spc = b[13];
	total = rd64(b + 40);
	mftlcn = rd64(b + 48);
	cpr = (B)b[64];
	cpi = (B)b[68];
	if ( bps != SECT ) {
		tm_printf((UB *)"ntfsfs: a sector of %d bytes is not handled\n", bps);
		er = EX_NOTSUP;
		goto err;
	}
	/* above 128 it is a power: the volumes of very large clusters */
	if ( spc > 0x80 ) spc = 1U << ( 256 - spc );
	if ( spc == 0 || ( spc & ( spc - 1 ) ) != 0 || total == 0 ) {
		er = EX_INVAL;
		goto err;
	}
	v->csize = (UD)SECT * spc;
	v->nclus = total / spc;
	v->recsize = ( cpr > 0 ) ? (UD)cpr * v->csize : ( cpr < 0 && cpr >= -31 ) ? (UD)1 << -cpr : 0;
	v->idxsize = ( cpi > 0 ) ? (UD)cpi * v->csize : ( cpi < 0 && cpi >= -31 ) ? (UD)1 << -cpi : 0;
	if ( v->recsize < SECT || v->recsize > 64 * 1024 || ( v->recsize & ( v->recsize - 1 ) ) != 0
	  || mftlcn >= v->nclus ) {
		er = EX_INVAL;
		goto err;
	}
	v->rec = (UB *)Kmalloc((SZ)v->recsize);
	v->rec2 = (UB *)Kmalloc((SZ)v->recsize);
	v->upcase = (UH *)Kmalloc(65536 * sizeof(UH));
	if ( v->rec == NULL || v->rec2 == NULL || v->upcase == NULL ) {
		er = EX_NOMEM;
		goto err;
	}
	for ( i = 0; i < 65536; i++ ) v->upcase[i] = (UH)i;

	er = dev_read(v, mftlcn * spc, v->rec2, v->recsize / SECT);
	if ( er < EX_OK ) goto err;
	if ( v->rec2[0] != 'F' || v->rec2[1] != 'I' || !fixup_apply(v->rec2, v->recsize) ) {
		er = EX_IO;
		goto err;
	}
	/* the $MFT's own first extent, enough to reach any extension of its list */
	for ( at = NULL; ( at = attr_next(v, v->rec2, at) ) != NULL; ) {
		if ( rd32(at) == AT_DATA && at[9] == 0 ) {
			er = attr_take(v, at, 0, &v->mft, TRUE);
			break;
		}
	}
	if ( er < EX_OK || v->mft.nruns == 0 ) {
		er = ( er < EX_OK ) ? er : EX_IO;
		goto err;
	}
	/* then all of it */
	er = attr_load(v, INO_MFT, AT_DATA, noname, 0, &full);
	if ( er < EX_OK ) goto err;
	attr_free(&v->mft);
	v->mft = full;

	/* names compare through the volume's own table */
	{
		NATTR	u;

		if ( attr_load(v, INO_UPCASE, AT_DATA, noname, 0, &u) >= EX_OK ) {
			if ( u.size == 65536 * 2 ) {
				UB	*t = (UB *)Kmalloc(65536 * 2);

				if ( t != NULL ) {
					if ( raw_read(v, &u, 0, 65536 * 2, t) >= EX_OK ) {
						for ( i = 0; i < 65536; i++ ) v->upcase[i] = rd16(t + i * 2);
					}
					Kfree(t);
				}
			}
			attr_free(&u);
		}
	}

	er = tk_cre_mtx(( cm.exinf = NULL, cm.mtxatr = TA_TFIFO, &cm ));
	if ( er < E_OK ) {
		er = EX_NOMEM;
		goto err;
	}
	v->mtxid = er;

	v->ro = TRUE;
	if ( ( m->flags & FS_MNT_RDONLY ) == 0 ) {
		er = safe_to_write(v, m);
		if ( er >= EX_OK ) {
			v->ro = FALSE;
		} else if ( er != EX_ROFS ) {
			tm_printf((UB *)"ntfsfs: %s: could not tell whether it is safe to write (%d): read only\n", m->dev, er);
		}
	}
	m->exinf = v;
	return EX_OK;

    err:
	if ( v->mtxid > 0 ) tk_del_mtx(v->mtxid);
	attr_free(&v->mft);
	if ( v->rec != NULL ) Kfree(v->rec);
	if ( v->rec2 != NULL ) Kfree(v->rec2);
	if ( v->upcase != NULL ) Kfree(v->upcase);
	Kfree(v);
	return er;
}

LOCAL ER nt_unmount( T_MOUNT *m )
{
	NVOL	*v = (NVOL *)m->exinf;

	tk_del_mtx(v->mtxid);
	attr_free(&v->mft);
	Kfree(v->rec);
	Kfree(v->rec2);
	Kfree(v->upcase);
	Kfree(v);
	m->exinf = NULL;
	return EX_OK;
}

/* ---------------------------------------------------------------- files */

LOCAL void nfile_free( NFILE *n )
{
	attr_free(&n->a);
	attr_free(&n->iroot);
	attr_free(&n->ialloc);
	attr_free(&n->ibm);
	if ( n->ib != NULL ) Kfree(n->ib);
	if ( n->cu != NULL ) Kfree(n->cu);
	Kfree(n);
}

LOCAL ER nt_open( T_MOUNT *m, CONST char *path, UINT oflags, T_FILE *f )
{
	static CONST UH noname[1] = { 0 };
	NVOL	*v = (NVOL *)m->exinf;
	NFILE	*n = NULL;
	UD	ino, mt;
	BOOL	isdir, wr = (BOOL)( ( oflags & O_ACCMODE ) != O_RDONLY );
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( wr && v->ro ) {
		er = EX_ROFS;
		goto exit;
	}
	er = path_walk(v, path, &ino);
	if ( er == EX_NOENT && ( oflags & O_CREAT ) != 0 ) {
		er = v->ro ? EX_ROFS : EX_NOTSUP;
		goto exit;
	}
	if ( er < EX_OK ) goto exit;
	if ( ( oflags & ( O_CREAT | O_EXCL ) ) == ( O_CREAT | O_EXCL ) ) {
		er = EX_EXIST;
		goto exit;
	}
	er = file_info(v, ino, &isdir, &mt);
	if ( er < EX_OK ) goto exit;
	if ( isdir && wr ) {
		er = EX_ISDIR;
		goto exit;
	}
	if ( !isdir && ( oflags & O_DIRECTORY ) != 0 ) {
		er = EX_NOTDIR;
		goto exit;
	}
	n = (NFILE *)Kmalloc(sizeof(NFILE));
	if ( n == NULL ) {
		er = EX_NOMEM;
		goto exit;
	}
	knl_memset(n, 0, sizeof(*n));
	n->ino = ino;
	n->isdir = isdir;
	n->cu_at = -1;
	if ( isdir ) {
		f->mode = FS_IFDIR;
		f->size = 0;
	} else {
		er = attr_load(v, ino, AT_DATA, noname, 0, &n->a);
		if ( er == EX_NOENT ) {
			er = EX_OK;		/* a file with no data of its own: a link of another kind */
		}
		if ( er < EX_OK ) goto exit;
		f->mode = FS_IFREG;
		f->size = n->a.size;
		if ( ( oflags & O_TRUNC ) != 0 && n->a.size > 0 ) {
			er = EX_NOTSUP;
			goto exit;
		}
	}
	f->ino = ino;
	f->priv = 0;
	f->obj = n;
	n = NULL;

    exit:
	if ( n != NULL ) nfile_free(n);
	tk_unl_mtx(v->mtxid);
	return er;
}

/* The time of the file brought up to now, in its record */
LOCAL ER touch_put( NVOL *v, UD ino )
{
	SYSTIM	t;
	UB	*at;
	UD	ft;
	UD	off = ino * v->recsize, done = 0;
	ER	er;

	if ( tk_get_tim(&t) < E_OK ) return EX_OK;
	ft = ( ( ( (UD)(UW)t.hi << 32 ) | t.lo ) / 1000 + FT_TO_TRON ) * 10000000ULL;
	er = rec_read(v, ino, v->rec);
	if ( er < EX_OK ) return er;
	for ( at = NULL; ( at = attr_next(v, v->rec, at) ) != NULL; ) {
		if ( rd32(at) == AT_STDINFO && at[8] == 0 ) {
			UINT	vo = rd16(at + 20);

			if ( rd32(at + 16) >= 32 && (UD)vo + 32 <= rd32(at + 4) ) {
				wr64(at + vo + 8, ft);		/* changed */
				wr64(at + vo + 16, ft);		/* the record changed */
				wr64(at + vo + 24, ft);		/* read */
			}
			break;
		}
	}
	fixup_undo(v->rec, v->recsize);
	while ( done < v->recsize ) {
		UD	cont;
		D	byte;

		er = run_find(v, &v->mft, off + done, &cont, &byte);
		if ( er < EX_OK ) return er;
		if ( byte < 0 || ( (UD)byte & ( SECT - 1 ) ) != 0 ) return EX_IO;
		if ( cont > v->recsize - done ) cont = v->recsize - done;
		er = dev_write(v, (UD)byte / SECT, v->rec + done, cont / SECT);
		if ( er < EX_OK ) return er;
		done += cont;
	}
	return EX_OK;
}

LOCAL ER nt_close( T_FILE *f )
{
	NVOL	*v = (NVOL *)f->mount->exinf;
	NFILE	*n = (NFILE *)f->obj;
	ER	er = EX_OK;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( n->touched ) {
		er = touch_put(v, n->ino);
	}
	nfile_free(n);
	f->obj = NULL;
	tk_unl_mtx(v->mtxid);
	return er;
}

/* LZNT1: one compression unit into out; how many bytes came, the rest are zeros to the caller */
LOCAL INT lznt1( CONST UB *in, UD inlen, UB *out, UD outmax )
{
	UD	ip = 0, op = 0;

	while ( ip + 2 <= inlen && op < outmax ) {
		UW	h = rd16(in + ip);
		UD	clen, cend, start = op;

		if ( h == 0 ) break;
		clen = ( h & 0xFFF ) + 1;
		ip += 2;
		cend = ip + clen;
		if ( cend > inlen ) return -1;
		if ( ( h & 0x8000 ) == 0 ) {
			if ( op + clen > outmax ) return -1;
			knl_memcpy(out + op, in + ip, (SZ)clen);
			op += clen;
			ip = cend;
			continue;
		}
		while ( ip < cend ) {
			UB	tags = in[ip++];
			INT	b;

			for ( b = 0; b < 8 && ip < cend; b++, tags >>= 1 ) {
				if ( ( tags & 1 ) == 0 ) {
					if ( op >= outmax ) return -1;
					out[op++] = in[ip++];
				} else {
					UD	pos = op - start, disp, l, k;
					UINT	lb = 12, tok;

					if ( ip + 2 > cend || pos == 0 ) return -1;
					tok = rd16(in + ip);
					ip += 2;
					{
						UD	q;

						for ( q = pos - 1; q >= 0x10; q >>= 1 ) lb--;
					}
					disp = ( tok >> lb ) + 1;
					l = ( tok & ( ( 1U << lb ) - 1 ) ) + 3;
					if ( disp > pos || op + l > outmax ) return -1;
					for ( k = 0; k < l; k++, op++ ) out[op] = out[op - disp];
				}
			}
		}
		/* a chunk stands for 4KB: what it did not make is zeros */
		if ( op - start < 4096 ) {
			UD	to = start + 4096;

			if ( to > outmax ) to = outmax;
			/* only when more follows; the last one's length is the caller's zeros */
			if ( ip + 2 <= inlen && rd16(in + ip) != 0 ) {
				knl_memset(out + op, 0, (SZ)( to - op ));
				op = to;
			}
		}
	}
	return (INT)op;
}

/* The compression unit at 'at' made, in n->cu */
LOCAL ER cu_make( NVOL *v, NFILE *n, UD at, UD cus )
{
	NATTR	*a = &n->a;
	UB	*raw;
	UD	got = 0, off, cont;
	D	byte;
	BOOL	hole = FALSE, allhole = TRUE;
	ER	er = EX_OK;

	if ( n->cu_at == (D)at ) return EX_OK;
	if ( n->cu == NULL ) {
		n->cu = (UB *)Kmalloc((SZ)cus);
		if ( n->cu == NULL ) return EX_NOMEM;
	}
	n->cu_at = -1;

	/* the unit's clusters: all there, stored as they are; some holes behind, compressed; none, zeros */
	for ( off = at; off < at + cus; off += v->csize ) {
		er = run_find(v, a, off, &cont, &byte);
		if ( er < EX_OK ) {
			/* past the runs is the same as a hole */
			hole = TRUE;
			er = EX_OK;
			continue;
		}
		if ( byte < 0 ) {
			hole = TRUE;
		} else {
			if ( hole ) return EX_IO;	/* a hole before data is not what a unit looks like */
			got += v->csize;
			allhole = FALSE;
		}
	}
	if ( allhole ) {
		knl_memset(n->cu, 0, (SZ)cus);
	} else if ( !hole ) {
		er = raw_read(v, a, at, cus, n->cu);
		if ( er < EX_OK ) return er;
	} else {
		INT	r;

		raw = (UB *)Kmalloc((SZ)got);
		if ( raw == NULL ) return EX_NOMEM;
		er = raw_read(v, a, at, got, raw);
		if ( er >= EX_OK ) {
			r = lznt1(raw, got, n->cu, cus);
			if ( r < 0 ) {
				er = EX_IO;
			} else if ( (UD)r < cus ) {
				knl_memset(n->cu + r, 0, (SZ)( cus - (UD)r ));
			}
		}
		Kfree(raw);
		if ( er < EX_OK ) return er;
	}
	n->cu_at = (D)at;
	return EX_OK;
}

LOCAL UB *dma_at( UB *p, UD n, BOOL into, UD *pn )
{
	UD	at = 0, want = n * SECT;
	void	*pa, *k = NULL;
	INT	r;

	*pn = 0;
	if ( want > (UD)XFER_MAX * SECT ) want = (UD)XFER_MAX * SECT;
	while ( at < want ) {
		pa = NULL;
		r = ConvPhysicalAddress(p + at, (INT)( want - at ), &pa);
		if ( r <= 0 || pa == NULL ) break;
		at += (UD)r;
	}
	if ( at >= SECT ) {
		*pn = at / SECT;
		return p;
	}
	r = knl_prc_dma(p, (INT)want, into, &k);
	if ( r < SECT || k == NULL ) {
		return NULL;
	}
	*pn = (UD)r / SECT;
	return (UB *)k;
}

LOCAL INT nt_read( T_FILE *f, void *buf, SZ len )
{
	NVOL	*v = (NVOL *)f->mount->exinf;
	NFILE	*n = (NFILE *)f->obj;
	NATTR	*a = &n->a;
	UB	*dst = (UB *)buf;
	SZ	done = 0;
	ER	er = EX_OK;

	if ( n->isdir ) return EX_ISDIR;
	if ( f->offset >= a->size ) return 0;
	if ( ( a->flags & AF_ENCRYPTED ) != 0 ) return EX_NOTSUP;
	if ( len > (SZ)( a->size - f->offset ) ) len = (SZ)( a->size - f->offset );
	if ( len > 0x40000000 ) len = 0x40000000;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	while ( done < len ) {
		UD	off = f->offset;
		SZ	w = len - done;

		if ( a->resident ) {
			knl_memcpy(dst + done, a->value + off, w);
		} else if ( off >= a->init ) {
			knl_memset(dst + done, 0, w);
		} else if ( ( a->flags & AF_COMPRESSED ) != 0 ) {
			UD	cus = v->csize << a->cu_shift, in, at;

			if ( cus > CU_MAX || a->cu_shift == 0 ) {
				er = EX_IO;
				break;
			}
			in = off % cus;
			at = off - in;
			er = cu_make(v, n, at, cus);
			if ( er < EX_OK ) break;
			if ( (UD)w > cus - in ) w = (SZ)( cus - in );
			if ( (UD)w > a->init - off ) w = (SZ)( a->init - off );
			knl_memcpy(dst + done, n->cu + in, w);
		} else {
			UD	cont, reach;
			D	byte;
			UB	*x;

			er = run_find(v, a, off, &cont, &byte);
			if ( er < EX_OK ) break;
			if ( (UD)w > cont ) w = (SZ)cont;
			if ( (UD)w > a->init - off ) w = (SZ)( a->init - off );
			if ( byte < 0 ) {
				knl_memset(dst + done, 0, w);
			} else if ( ( (UD)byte & ( SECT - 1 ) ) == 0 && w >= SECT
				 && ( x = dma_at(dst + done, (UD)w / SECT, TRUE, &reach) ) != NULL && reach > 0 ) {
				if ( dev_read(v, (UD)byte / SECT, x, reach) < EX_OK ) {
					er = EX_IO;
					break;
				}
				w = (SZ)reach * SECT;
			} else {
				UD	s = (UD)byte & ( SECT - 1 );

				if ( (UD)w > SECT - s ) w = (SZ)( SECT - s );
				if ( dev_read(v, (UD)byte / SECT, v->sb, 1) < EX_OK ) {
					er = EX_IO;
					break;
				}
				knl_memcpy(dst + done, v->sb + s, w);
			}
		}
		done += w;
		f->offset += (UD)w;
	}
	tk_unl_mtx(v->mtxid);
	if ( done == 0 && er < EX_OK ) return (INT)er;
	return (INT)done;
}

/* A resident value's bytes replaced, in its record */
LOCAL ER resident_put( NVOL *v, NFILE *n, UD off, CONST UB *src, SZ len )
{
	UB	*at;
	UD	roff = n->a.rec * v->recsize, done = 0;
	ER	er;

	er = rec_read(v, n->a.rec, v->rec);
	if ( er < EX_OK ) return er;
	for ( at = NULL; ( at = attr_next(v, v->rec, at) ) != NULL; ) {
		if ( rd32(at) == AT_DATA && at[9] == 0 && at[8] == 0 ) {
			UINT	vo = rd16(at + 20);

			if ( off + (UD)len > rd32(at + 16) || (UD)vo + rd32(at + 16) > rd32(at + 4) ) return EX_IO;
			knl_memcpy(at + vo + off, src, len);
			break;
		}
	}
	if ( at == NULL ) return EX_IO;
	fixup_undo(v->rec, v->recsize);
	while ( done < v->recsize ) {
		UD	cont;
		D	byte;

		er = run_find(v, &v->mft, roff + done, &cont, &byte);
		if ( er < EX_OK ) return er;
		if ( byte < 0 || ( (UD)byte & ( SECT - 1 ) ) != 0 ) return EX_IO;
		if ( cont > v->recsize - done ) cont = v->recsize - done;
		er = dev_write(v, (UD)byte / SECT, v->rec + done, cont / SECT);
		if ( er < EX_OK ) return er;
		done += cont;
	}
	knl_memcpy(n->a.value + off, src, len);
	return EX_OK;
}

LOCAL INT nt_write( T_FILE *f, CONST void *buf, SZ len )
{
	NVOL	*v = (NVOL *)f->mount->exinf;
	NFILE	*n = (NFILE *)f->obj;
	NATTR	*a = &n->a;
	CONST UB *src = (CONST UB *)buf;
	SZ	done = 0;
	ER	er = EX_OK;

	if ( len <= 0 ) return 0;
	if ( v->ro ) return EX_ROFS;
	if ( ( f->oflags & O_APPEND ) != 0 ) return EX_NOTSUP;
	if ( ( a->flags & ( AF_ENCRYPTED | AF_COMPRESSED ) ) != 0 ) return EX_NOTSUP;
	if ( f->offset + (UD)len > a->init ) return EX_NOTSUP;	/* the size or what is initialized would change */

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	if ( a->resident ) {
		er = resident_put(v, n, f->offset, src, len);
		if ( er >= EX_OK ) {
			f->offset += (UD)len;
			done = len;
		}
	} else {
		/* every byte has its place, or none is written */
		{
			UD	o = f->offset, e = f->offset + (UD)len;

			while ( o < e ) {
				UD	cont;
				D	byte;

				er = run_find(v, a, o, &cont, &byte);
				if ( er < EX_OK ) goto exit;
				if ( byte < 0 ) {
					er = EX_NOTSUP;
					goto exit;
				}
				o += cont;
			}
		}
		while ( done < len ) {
			UD	off = f->offset, cont, reach;
			D	byte;
			SZ	w = len - done;
			UB	*x;

			er = run_find(v, a, off, &cont, &byte);
			if ( er < EX_OK ) break;
			if ( (UD)w > cont ) w = (SZ)cont;
			if ( ( (UD)byte & ( SECT - 1 ) ) == 0 && w >= SECT
			  && ( x = dma_at((UB *)src + done, (UD)w / SECT, FALSE, &reach) ) != NULL && reach > 0 ) {
				if ( dev_write(v, (UD)byte / SECT, x, reach) < EX_OK ) {
					er = EX_IO;
					break;
				}
				w = (SZ)reach * SECT;
			} else {
				UD	s = (UD)byte & ( SECT - 1 );

				if ( (UD)w > SECT - s ) w = (SZ)( SECT - s );
				if ( dev_read(v, (UD)byte / SECT, v->sb, 1) < EX_OK ) {
					er = EX_IO;
					break;
				}
				knl_memcpy(v->sb + s, src + done, w);
				if ( dev_write(v, (UD)byte / SECT, v->sb, 1) < EX_OK ) {
					er = EX_IO;
					break;
				}
			}
			done += w;
			f->offset += (UD)w;
		}
	}
	if ( done > 0 ) {
		n->touched = TRUE;
		n->cu_at = -1;
	}

    exit:
	tk_unl_mtx(v->mtxid);
	if ( done == 0 && er < EX_OK ) return (INT)er;
	return (INT)done;
}

LOCAL ER nt_stat( T_MOUNT *m, CONST char *path, T_FSTAT *st )
{
	static CONST UH noname[1] = { 0 };
	NVOL	*v = (NVOL *)m->exinf;
	UD	ino, mt;
	BOOL	isdir;
	ER	er;

	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = path_walk(v, path, &ino);
	if ( er >= EX_OK ) er = file_info(v, ino, &isdir, &mt);
	if ( er >= EX_OK ) {
		st->mode = ( isdir ? FS_IFDIR : FS_IFREG ) | FS_IRWXU;
		st->size = 0;
		st->mtime = mt;
		st->ino = ino;
		if ( !isdir ) {
			NATTR	a;

			er = attr_load(v, ino, AT_DATA, noname, 0, &a);
			if ( er >= EX_OK ) {
				st->size = a.size;
				attr_free(&a);
			} else if ( er == EX_NOENT ) {
				er = EX_OK;
			}
		}
	}
	tk_unl_mtx(v->mtxid);
	return er;
}

/* The index of a directory ready for listing; one with none is empty */
LOCAL ER list_ready( NVOL *v, NFILE *n )
{
	ER	er;

	if ( n->iroot.size != 0 || n->iroot.value != NULL ) {
		return EX_OK;
	}
	er = attr_load(v, n->ino, AT_IROOT, I30, 4, &n->iroot);
	if ( er == EX_NOENT ) return EX_OK;
	if ( er < EX_OK ) return er;
	if ( !n->iroot.resident || n->iroot.size < 32 ) return EX_IO;
	n->ibs = rd32(n->iroot.value + 8);
	er = attr_load(v, n->ino, AT_IALLOC, I30, 4, &n->ialloc);
	if ( er == EX_NOENT ) return EX_OK;
	if ( er < EX_OK ) return er;
	if ( n->ibs < SECT || n->ibs > 1024 * 1024 || ( n->ibs & ( SECT - 1 ) ) != 0 ) return EX_IO;
	n->has_alloc = TRUE;
	n->ib = (UB *)Kmalloc((SZ)n->ibs);
	if ( n->ib == NULL ) return EX_NOMEM;
	if ( attr_load(v, n->ino, AT_BITMAP, I30, 4, &n->ibm) >= EX_OK ) {
		n->has_bm = TRUE;
	}
	return EX_OK;
}

/*
 * The listing keeps its place as (block + 1, entry); block 0 stands for
 * the root. The blocks are gone through as they lie, not in the order of
 * the names; only those the index's bitmap says are in use.
 */
LOCAL INT nt_getdents( T_FILE *f, T_DIRENT *buf, INT nent )
{
	NVOL	*v = (NVOL *)f->mount->exinf;
	NFILE	*n = (NFILE *)f->obj;
	UD	blk = f->priv >> 24, ent = f->priv & 0xFFFFFF;
	INT	cnt = 0;
	ER	er;

	if ( !n->isdir ) return EX_NOTDIR;
	tk_loc_mtx(v->mtxid, TMO_FEVR);
	er = list_ready(v, n);
	if ( er < EX_OK ) goto exit;
	if ( n->iroot.value == NULL ) goto exit;

	while ( cnt < nent ) {
		UB	*p, *end;
		UD	k;

		if ( blk == 0 ) {
			UD	eo = rd32(n->iroot.value + 16), sz = rd32(n->iroot.value + 20);

			if ( 16 + eo >= n->iroot.size || sz > n->iroot.size - 16 ) {
				er = EX_IO;
				break;
			}
			p = n->iroot.value + 16 + eo;
			end = n->iroot.value + 16 + sz;
		} else {
			UD	bi = blk - 1, off = bi * n->ibs;

			if ( !n->has_alloc || off + n->ibs > n->ialloc.size ) break;
			if ( n->has_bm ) {
				UB	bit;

				if ( bi / 8 >= n->ibm.size ) break;
				er = raw_read(v, &n->ibm, bi / 8, 1, &bit);
				if ( er < EX_OK ) break;
				if ( ( bit & ( 1 << ( bi & 7 ) ) ) == 0 ) {
					blk++;
					ent = 0;
					continue;
				}
			}
			er = ib_read(v, &n->ialloc, n->ibs, off, n->ib, &p, &end);
			if ( er < EX_OK ) break;
		}
		for ( k = 0; entry_ok(p, end) && cnt < nent; k++ ) {
			UINT	fl = rd16(p + 12), el = rd16(p + 8);

			if ( ( fl & IE_LAST ) != 0 ) break;
			if ( k >= ent ) {
				ent = k + 1;
				if ( entry_name(p) && p[16 + 65] != NS_DOS ) {
					UD	ino = rd64(p) & 0xFFFFFFFFFFFFULL;
					INT	nl = p[16 + 64], i, b;

					/* the volume's own files are not the user's */
					if ( ino >= INO_FIRST_USER || rd16(p + 16 + 66) != '$' ) {
						if ( !( ino == INO_ROOT ) ) {
							for ( i = 0; i < nl; i++ ) v->nm16[i] = rd16(p + 16 + 66 + i * 2);
							b = knl_fs_utf16_to_8(v->nm16, nl, buf[cnt].name, FS_NAME_MAX - 1);
							if ( b >= 0 ) {
								buf[cnt].name[b] = 0;
								buf[cnt].mode = ( rd32(p + 16 + 56) & FN_DIRFLAG ) != 0 ? FS_IFDIR : FS_IFREG;
								buf[cnt].size = ( buf[cnt].mode == FS_IFDIR ) ? 0 : rd64(p + 16 + 48);
								buf[cnt].ino = ino;
								cnt++;
							}
						}
					}
				}
			}
			p += el;
		}
		if ( cnt >= nent ) break;
		blk++;
		ent = 0;
	}

    exit:
	f->priv = ( blk << 24 ) | ( ent & 0xFFFFFF );
	tk_unl_mtx(v->mtxid);
	if ( cnt == 0 && er < EX_OK ) return (INT)er;
	return cnt;
}

LOCAL ER nt_statvfs( T_MOUNT *m, T_FSSTAT *st )
{
	NVOL	*v = (NVOL *)m->exinf;

	/* how much is free would take reading $Bitmap whole: not needed to serve the volume */
	st->blocks = v->nclus;
	st->bfree = 0;
	st->bsize = v->csize;
	return EX_OK;
}

LOCAL ER nt_sync( T_MOUNT *m )
{
	(void)m;
	return EX_OK;		/* nothing is held back */
}

/* What changes the volume's structure: see the head of this file */
LOCAL ER nt_truncate( T_FILE *f, UD len )
{
	NFILE	*n = (NFILE *)f->obj;

	return ( !n->isdir && len == n->a.size ) ? EX_OK : EX_NOTSUP;
}

LOCAL ER nt_mkdir( T_MOUNT *m, CONST char *path )
{
	(void)path;
	return ( (NVOL *)m->exinf )->ro ? EX_ROFS : EX_NOTSUP;
}

LOCAL ER nt_rename( T_MOUNT *m, CONST char *from, CONST char *to )
{
	(void)from;
	(void)to;
	return ( (NVOL *)m->exinf )->ro ? EX_ROFS : EX_NOTSUP;
}

EXPORT CONST T_FIMP knl_fimp_ntfs = {
	.name     = "ntfsfs",
	.mount    = nt_mount,
	.unmount  = nt_unmount,
	.open     = nt_open,
	.close    = nt_close,
	.read     = nt_read,
	.write    = nt_write,
	.truncate = nt_truncate,
	.stat     = nt_stat,
	.getdents = nt_getdents,
	.mkdir    = nt_mkdir,
	.rmdir    = nt_mkdir,
	.unlink   = nt_mkdir,
	.rename   = nt_rename,
	.statvfs  = nt_statvfs,
	.sync     = nt_sync,
};
