/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obfile.c
 *	The file manager: storage objects kept on volumes (design 18.6)
 *
 *	The objects are TSFS's (design 11): this manager keeps the volumes
 *	that are attached, finds which one an object is on, and turns the
 *	basic operations into TSFS calls. What TSFS does not know -- the
 *	record types beyond xmlTAD and bytes, and the protection -- is kept
 *	in the metadata under "tessronos".
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/tsfsobj.h>
#include <ts/tsfsck.h>
#include <ts/om.h>
#include <ts/sysdef.h>
#include "obj.h"

#define OBF_VOL_MAX	CNF_MAX_VOL	/* volumes at once: the disks times their partitions */
#define OBF_HND_MAX	4096

/*
 * A volume attached. Its entry is made the first time one more volume
 * is attached than there are entries, and is kept, free or in use, from
 * then on, so that what is looked at between two takings of the lock is
 * never memory given back. The first entry made is the system's volume,
 * attached at the start; the order of the entries is the order objects
 * are looked for in.
 *
 * A volume whose medium went is 'gone': no object of it is found any
 * more, and it is closed once the last of them that were open is
 * closed. A volume of the same UUID may be attached meanwhile.
 */
typedef struct {
	BOOL	used;
	BOOL	gone;
	ID	vol;
	UB	path[FS_PATH_MAX];
	T_OBPRT	*dom;			/* its protection domain, NULL for none */
	BOOL	native;
	TS_UUID	vuuid;			/* the native volume's own UUID */
	BOOL	again;			/* attached before since the start: its disk taken out and put back */
	BOOL	finished;		/* the machine going off: left clean (knl_obfile_finish) */
	BOOL	foreign;		/* another machine's system volume: looked for after the others */
	TS_UUID	*al;			/* its aliases (al_build), al_n of each of the four tables */
	INT	al_n;
} OBFVOL;

typedef struct {
	BOOL	used;
	ID	vol;
	ID	od;
} OBFHND;

/* A record's type as the metadata declares it */
typedef struct {
	INT	n;
	UINT	rt, sub;
} OBFDECL;

LOCAL OBFVOL	*obf_vol[OBF_VOL_MAX];
LOCAL INT	obf_nvol = 0;		/* entries made */
LOCAL UW	obf_serial = 0;		/* each change of the volumes attached */
LOCAL OBFHND	obf_hnd[OBF_HND_MAX];
LOCAL ID	obf_mtx = 0;

/*
 * This machine's system volume: the first attached since the start. Its
 * UUID (zero for the FAT store) tells it from another machine's system
 * volume, which holds objects of the same UUIDs (the system's, made of
 * etc/def) and is attached from a disk the person put in.
 */
LOCAL BOOL	obf_sys_known = FALSE;
LOCAL TS_UUID	obf_sysvu;

#define LOCK()		tk_loc_mtx(obf_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(obf_mtx)

LOCAL CONST char obf_default_json[] =
	"{\"name\":\"\",\"refCount\":0,\"recordCount\":0,"
	"\"editable\":true,\"deletable\":true,\"readable\":true}";

/* ---------------------------------------------------------------- volumes */

/* The entry at a place, NULL past those made */
LOCAL OBFVOL *vent_at( INT i )
{
	return ( i >= 0 && i < __atomic_load_n(&obf_nvol, __ATOMIC_ACQUIRE) )
	       ? __atomic_load_n(&obf_vol[i], __ATOMIC_ACQUIRE) : NULL;
}

/* The volume at a place, if it is one objects are looked for on (under the lock) */
LOCAL ID live_at( INT i )
{
	OBFVOL	*v = vent_at(i);

	return ( v != NULL && v->used && !v->gone ) ? v->vol : 0;
}

/* The same, of the volumes that are another machine's system volume (foreign) or of the others */
LOCAL ID live_kind_at( INT i, BOOL foreign )
{
	OBFVOL	*v = vent_at(i);

	return ( v != NULL && v->used && !v->gone && v->foreign == foreign ) ? v->vol : 0;
}

/*
 * An object is looked for on this machine's volumes first, in the order
 * they were attached, and on another machine's system volumes after: a
 * UUID of the system's names this system's object, and the other
 * machine's object of that UUID is reached through the volume (design
 * 8.3).
 */
LOCAL BOOL al_is( CONST TS_UUID *u );
LOCAL ID obf_real( CONST TS_UUID *in, TS_UUID *real );
LOCAL CONST TS_UUID *al_of( CONST OBFVOL *v, CONST TS_UUID *orig );

EXPORT ID knl_obfile_vol_of( CONST TS_UUID *uuid )
{
	INT	i, pass, n = __atomic_load_n(&obf_nvol, __ATOMIC_ACQUIRE);
	ID	vol;
	TS_UUID	r;
	BOOL	aliased;

	if ( ts_uuid_cmp(uuid, &ob_uuid_domain) == 0 ) {
		return E_NOEXS;			/* a volume's domain is not an object to use */
	}
	if ( al_is(uuid) ) {
		return obf_real(uuid, &r);	/* another machine's object, by its alias */
	}
	for ( pass = 0; pass < 2; pass++ ) {
		for ( i = 0; i < n; i++ ) {
			LOCK();
			vol = live_kind_at(i, (BOOL)( pass == 1 ));
			/* another machine's object of this system's UUID is reached by its alias alone */
			aliased = (BOOL)( vol > 0 && pass == 1 && al_of(vent_at(i), uuid) != NULL );
			UNLOCK();
			if ( vol > 0 && !aliased && knl_tsfs_exists(vol, uuid) >= E_OK ) {
				return vol;
			}
		}
	}
	return E_NOEXS;
}

EXPORT INT knl_obfile_natives( ID *ids, TS_UUID *vu, INT max )
{
	INT	i, n = 0;

	LOCK();
	for ( i = 0; i < obf_nvol && n < max; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && v->native ) {
			if ( ids != NULL ) ids[n] = v->vol;
			if ( vu != NULL ) vu[n] = v->vuuid;
			n++;
		}
	}
	UNLOCK();
	return n;
}

EXPORT ID knl_obfile_first_vol( void )
{
	INT	i, n = __atomic_load_n(&obf_nvol, __ATOMIC_ACQUIRE);
	ID	vol = 0;

	LOCK();
	for ( i = 0; i < n && vol <= 0; i++ ) {
		vol = live_kind_at(i, FALSE);	/* never another machine's */
	}
	UNLOCK();
	return ( vol > 0 ) ? vol : E_NOEXS;
}

/* ---------------------------------------------------------------- another machine's objects */

/*
 * The objects of another machine's system volume that have the UUIDs of
 * this system's objects (all that came from etc/def) are reached by
 * aliases (design 8.3): UUIDs of version 8 (RFC 9562) whose first six
 * bytes are OBF_ALIAS_MARK and whose rest comes from SHA-256 of the
 * marker's name, the volume's UUID and the object's, so that the same
 * disk put in again gives the same aliases. No object of TessronOS has
 * such a UUID -- the system makes version 7 and version 4 ones -- and
 * none is made with one (obf_create). The records and the metadata of
 * those objects are read with every UUID written in them that has an
 * alias on that volume turned into the alias, and written with the
 * aliases turned back, in place: a UUID and its alias are written in
 * the same 36 letters. What a program, the desktop or a window does
 * with an object of the other machine is then done to an object of its
 * own UUID, never to this system's object of the same UUID, which an
 * alias never reaches; an alias of a volume taken away is found on no
 * volume at all.
 */
LOCAL CONST UB	OBF_ALIAS_MARK[6] = { 0x7e, 0x55, 0x0a, 0x11, 0xa5, 0x00 };
LOCAL CONST char OBF_ALIAS_NS[] = "tessronos.foreign-alias";

/* Whether a UUID is an alias */
LOCAL BOOL al_is( CONST TS_UUID *u )
{
	INT	i;

	for ( i = 0; i < 6; i++ ) {
		if ( u->b[i] != OBF_ALIAS_MARK[i] ) return FALSE;
	}
	return (BOOL)( ( u->b[6] & 0xF0 ) == 0x80 );
}

/* The alias of an object of a volume */
LOCAL void al_make( CONST TS_UUID *vuuid, CONST TS_UUID *orig, TS_UUID *out )
{
	UB	in[sizeof(OBF_ALIAS_NS) - 1 + 32], h[OB_SHA256_LEN];
	INT	n = (INT)sizeof(OBF_ALIAS_NS) - 1;

	knl_memcpy(in, OBF_ALIAS_NS, n);
	knl_memcpy(in + n, vuuid->b, 16);
	knl_memcpy(in + n + 16, orig->b, 16);
	knl_ob_sha256(in, (SZ)( n + 32 ), h);
	knl_memcpy(out->b, OBF_ALIAS_MARK, 6);
	out->b[6] = (UB)( 0x80 | ( h[0] & 0x0F ) );	/* version 8 */
	out->b[7] = h[1];
	out->b[8] = (UB)( 0x80 | ( h[2] & 0x3F ) );	/* variant 10 */
	knl_memcpy(out->b + 9, h + 3, 7);
}

/* The place of u in a table sorted by the UUIDs, or -1 */
LOCAL INT al_find( CONST TS_UUID *t, INT n, CONST TS_UUID *u )
{
	INT	lo = 0, hi = n - 1, m, c;

	while ( lo <= hi ) {
		m = ( lo + hi ) / 2;
		c = ts_uuid_cmp(&t[m], u);
		if ( c == 0 ) return m;
		if ( c < 0 ) lo = m + 1; else hi = m - 1;
	}
	return -1;
}

/* Two tables sorted together by the first (a shell sort: the tables are made once, at the attach) */
LOCAL void al_sort( TS_UUID *key, TS_UUID *val, INT n )
{
	INT	gap, i, j;
	TS_UUID	k, v;

	for ( gap = n / 2; gap > 0; gap /= 2 ) {
		for ( i = gap; i < n; i++ ) {
			k = key[i];
			v = val[i];
			for ( j = i; j >= gap && ts_uuid_cmp(&key[j - gap], &k) > 0; j -= gap ) {
				key[j] = key[j - gap];
				val[j] = val[j - gap];
			}
			key[j] = k;
			val[j] = v;
		}
	}
}

/* Whether an object of the UUID is on a volume of this machine (not another machine's) */
LOCAL BOOL al_here( CONST TS_UUID *u )
{
	INT	i, n = __atomic_load_n(&obf_nvol, __ATOMIC_ACQUIRE);
	ID	vol;

	for ( i = 0; i < n; i++ ) {
		LOCK();
		vol = live_kind_at(i, FALSE);
		UNLOCK();
		if ( vol > 0 && knl_tsfs_exists(vol, u) >= E_OK ) return TRUE;
	}
	return FALSE;
}

/*
 * The aliases of another machine's system volume, made as it is
 * attached: each of its objects that has the UUID of one of this
 * machine's. Four tables of n: the originals and their aliases sorted by
 * the originals, the aliases and their originals sorted by the aliases.
 * FALSE for want of memory (the volume is then not attached).
 */
LOCAL BOOL al_build( ID vol, CONST TS_UUID *vuuid, OBFVOL *into )
{
	TS_UUID	*page, from, *t = NULL, *nt;
	INT	got, i, n = 0, cap = 0;
	BOOL	first = TRUE;

	into->al_n = 0;
	into->al = NULL;
	page = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * 64);
	if ( page == NULL ) {
		return FALSE;
	}
	knl_memset(&from, 0, sizeof(from));
	for ( ;; ) {
		got = 0;
		if ( ts_lst_obj(vol, first ? NULL : &from, page, 64, &got) < E_OK || got <= 0 ) {
			break;
		}
		for ( i = 0; i < got; i++ ) {
			if ( ts_uuid_cmp(&page[i], &ob_uuid_domain) == 0 || !al_here(&page[i]) ) {
				continue;
			}
			if ( n >= cap ) {
				cap = ( cap == 0 ) ? 256 : cap * 2;
				nt = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)cap * 2);
				if ( nt == NULL ) {
					if ( t != NULL ) Kfree(t);
					Kfree(page);
					return FALSE;
				}
				if ( t != NULL ) {
					knl_memcpy(nt, t, (INT)( sizeof(TS_UUID) * (SZ)n ));
					knl_memcpy(nt + cap, t + cap / 2, (INT)( sizeof(TS_UUID) * (SZ)n ));
					Kfree(t);
				}
				t = nt;
			}
			t[n] = page[i];
			al_make(vuuid, &page[i], &t[cap + n]);
			n++;
		}
		from = page[got - 1];
		first = FALSE;
		if ( got < 64 ) {
			break;
		}
	}
	Kfree(page);
	if ( n > 0 ) {
		/* the four tables, side by side in one piece of memory */
		nt = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n * 4);
		if ( nt == NULL ) {
			Kfree(t);
			return FALSE;
		}
		knl_memcpy(nt, t, (INT)( sizeof(TS_UUID) * (SZ)n ));
		knl_memcpy(nt + n, t + cap, (INT)( sizeof(TS_UUID) * (SZ)n ));
		knl_memcpy(nt + 2 * n, t + cap, (INT)( sizeof(TS_UUID) * (SZ)n ));
		knl_memcpy(nt + 3 * n, t, (INT)( sizeof(TS_UUID) * (SZ)n ));
		Kfree(t);
		al_sort(nt, nt + n, n);
		al_sort(nt + 2 * n, nt + 3 * n, n);
		into->al = nt;
		into->al_n = n;
	}
	return TRUE;
}

/* The alias of an original on a volume's entry, or NULL (under the lock) */
LOCAL CONST TS_UUID *al_of( CONST OBFVOL *v, CONST TS_UUID *orig )
{
	INT	k = ( v->al_n > 0 ) ? al_find(v->al, v->al_n, orig) : -1;

	return ( k >= 0 ) ? &v->al[v->al_n + k] : NULL;
}

/* The original of an alias on a volume's entry, or NULL (under the lock) */
LOCAL CONST TS_UUID *al_orig( CONST OBFVOL *v, CONST TS_UUID *alias )
{
	INT	k = ( v->al_n > 0 ) ? al_find(v->al + 2 * v->al_n, v->al_n, alias) : -1;

	return ( k >= 0 ) ? &v->al[3 * v->al_n + k] : NULL;
}

/*
 * An object as the layers below know it: the volume and the UUID it has
 * there. An alias is the object of another machine's volume it stands
 * for; another UUID is itself, on the volume knl_obfile_vol_of finds.
 */
LOCAL ID obf_real( CONST TS_UUID *in, TS_UUID *real )
{
	CONST TS_UUID	*o = NULL;
	INT		i;
	ID		vol = 0;

	if ( !al_is(in) ) {
		*real = *in;
		return knl_obfile_vol_of(in);
	}
	LOCK();
	for ( i = 0; i < obf_nvol && o == NULL; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && v->foreign && ( o = al_orig(v, in) ) != NULL ) {
			*real = *o;
			vol = v->vol;
		}
	}
	UNLOCK();
	return ( vol > 0 ) ? vol : E_NOEXS;
}

EXPORT ER knl_obfile_real( CONST TS_UUID *in, ID *p_vol, TS_UUID *p_real )
{
	TS_UUID	r;
	ID	vol;

	if ( in == NULL ) {
		return E_PAR;
	}
	vol = obf_real(in, &r);
	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	if ( p_vol != NULL ) *p_vol = vol;
	if ( p_real != NULL ) *p_real = r;
	return E_OK;
}

/* A hexadecimal digit's value, or -1 */
LOCAL INT al_hex( UB c )
{
	return ( c >= '0' && c <= '9' ) ? c - '0' : ( c >= 'a' && c <= 'f' ) ? c - 'a' + 10
	     : ( c >= 'A' && c <= 'F' ) ? c - 'A' + 10 : -1;
}

/* A UUID written at p (36 letters, 8-4-4-4-12), or FALSE */
LOCAL BOOL al_text( CONST UB *p, TS_UUID *u )
{
	INT	i, k = 0, hi, lo;

	for ( i = 0; i < 36; ) {
		if ( i == 8 || i == 13 || i == 18 || i == 23 ) {
			if ( p[i] != '-' ) return FALSE;
			i++;
			continue;
		}
		hi = al_hex(p[i]);
		lo = al_hex(p[i + 1]);
		if ( hi < 0 || lo < 0 ) return FALSE;
		u->b[k++] = (UB)( ( hi << 4 ) | lo );
		i += 2;
	}
	return TRUE;
}

LOCAL void al_put( UB *p, CONST TS_UUID *u )
{
	static CONST char	hx[] = "0123456789abcdef";
	INT			i, k = 0;

	for ( i = 0; i < 36; ) {
		if ( i == 8 || i == 13 || i == 18 || i == 23 ) {
			p[i++] = '-';
			continue;
		}
		p[i++] = (UB)hx[u->b[k] >> 4];
		p[i++] = (UB)hx[u->b[k] & 15];
		k++;
	}
}

/*
 * The UUIDs written in bytes of a record or the metadata of an object of
 * the volume entry v, turned into their aliases (to TRUE) or back, in
 * place. A UUID is taken as one only whole and alone: not a part of a
 * longer run of hexadecimal digits. Answers how many were turned.
 */
LOCAL INT al_swap( OBFVOL *v, UB *b, SZ n, BOOL to )
{
	TS_UUID		u;
	CONST TS_UUID	*w;
	SZ		i;
	INT		k = 0;

	if ( v == NULL || v->al_n <= 0 || b == NULL ) {
		return 0;
	}
	for ( i = 0; i + 36 <= n; i++ ) {
		if ( al_hex(b[i]) < 0 || ( i > 0 && ( al_hex(b[i - 1]) >= 0 || b[i - 1] == '-' ) )
		  || ( i + 36 < n && ( al_hex(b[i + 36]) >= 0 || b[i + 36] == '-' ) ) || !al_text(b + i, &u) ) {
			continue;
		}
		LOCK();
		w = to ? al_of(v, &u) : al_orig(v, &u);
		if ( w != NULL ) {
			u = *w;
		}
		UNLOCK();
		if ( w != NULL ) {
			al_put(b + i, &u);
			k++;
		}
		i += 35;
	}
	return k;
}

/* The volume entry of a handle when it is another machine's with aliases, else NULL */
LOCAL OBFVOL *al_vol_of_hnd( CONST OBFHND *o )
{
	OBFVOL	*r = NULL;
	INT	i;

	if ( o == NULL ) {
		return NULL;
	}
	LOCK();
	for ( i = 0; i < obf_nvol && r == NULL; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && v->vol == o->vol && v->foreign && v->al_n > 0 ) {
			r = v;
		}
	}
	UNLOCK();
	return r;
}

LOCAL BOOL same_path( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

/* ---------------------------------------------------------------- protection domains */

/*
 * A volume's domain is the protection in the metadata of the object of
 * fixed identity ob_uuid_domain on it, which is on every volume that
 * has one and is never an object to anyone else. It is read when the
 * volume is attached and kept here, since every opening asks for it.
 */
LOCAL void dom_load( OBFVOL *v )
{
	T_OBPRT	*d = NULL;
	UB	*j = NULL;
	INT	len = knl_tsfs_meta_all(v->vol, &ob_uuid_domain, 0, &j, NULL);

	if ( len >= 0 && knl_oj_path(j, len, "tessronos", "access") >= 0 ) {
		if ( knl_ob_prt_parse(j, len, &d) < E_OK ) {
			d = NULL;
		}
	}
	if ( j != NULL ) Kfree(j);
	LOCK();
	if ( v->dom != NULL ) Kfree(v->dom);
	v->dom = d;
	UNLOCK();
}

LOCAL OBFVOL *vent_by_path( CONST char *path );
LOCAL void known_note( CONST TS_UUID *vuuid, CONST UB *label, CONST char *path );
LOCAL BOOL known_has( CONST TS_UUID *vuuid );

EXPORT T_OBPRT *knl_obfile_domain( CONST TS_UUID *uuid )
{
	ID	vol = knl_obfile_vol_of(uuid);
	T_OBPRT	*d = NULL;
	INT	i;

	if ( vol <= 0 ) {
		return NULL;
	}
	LOCK();
	for ( i = 0; i < obf_nvol && d == NULL; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && v->vol == vol && v->dom != NULL ) {
			d = knl_ob_prt_dup(v->dom);
		}
	}
	UNLOCK();

	return d;
}

EXPORT T_OBPRT *knl_obfile_get_dom( CONST char *path )
{
	OBFVOL	*v;
	T_OBPRT	*d = NULL;

	LOCK();
	v = vent_by_path(path);
	if ( v != NULL && v->dom != NULL ) {
		d = knl_ob_prt_dup(v->dom);
	}
	UNLOCK();

	return d;
}

EXPORT ER knl_obfile_set_dom( CONST char *path, CONST T_OBPRT *prt )
{
	OBFVOL	*v;
	UB	*j;
	INT	len, cap;
	ER	er;
	CONST char *base = "{\"name\":\"保護区画\",\"refCount\":1,\"recordCount\":0}";

	LOCK();
	v = vent_by_path(path);
	UNLOCK();
	if ( v == NULL ) {
		return E_NOEXS;
	}
	if ( prt == NULL ) {
		/* it is counted once so that nothing collects it; let go of that first */
		er = E_OK;
		if ( knl_tsfs_exists(v->vol, &ob_uuid_domain) >= E_OK ) {
			(void)ts_unl_obj(v->vol, &ob_uuid_domain);
			er = ts_del_obj(v->vol, &ob_uuid_domain);
		}
		if ( er >= E_OK ) dom_load(v);
		return er;
	}
	for ( len = 0; base[len] != 0; len++ ) ;
	cap = len;
	j = (UB *)Kmalloc((SZ)cap + 1);
	if ( j == NULL ) {
		return E_NOMEM;
	}
	for ( len = 0; base[len] != 0; len++ ) j[len] = (UB)base[len];
	j[len] = 0;
	len = knl_ob_prt_store(&j, len, &cap, prt);
	if ( len < 0 ) {
		Kfree(j);
		return E_PAR;
	}
	er = ( knl_tsfs_exists(v->vol, &ob_uuid_domain) >= E_OK )
	   ? knl_tsfs_set_meta(v->vol, &ob_uuid_domain, j, len)
	   : knl_tsfs_cre_as(v->vol, &ob_uuid_domain, j, len);
	Kfree(j);
	if ( er >= E_OK ) {
		dom_load(v);
	}
	return er;
}

/* ---------------------------------------------------------------- labels */

/*
 * The whole label of a native volume (design 7.6), in memory of its own:
 * the size asked first, and asked again should the name have grown
 * meanwhile. NULL when it could not be read or there was no memory.
 */
LOCAL UB *label_dup( ID vol )
{
	UB	*b = NULL;
	SZ	size = 0, asz = 0;
	ER	er;

	for ( ;; ) {
		er = knl_tsfs_vol_label(vol, b, size, &asz);
		if ( er < E_OK ) {
			tm_printf((UB *)"ob: the label of volume %d not read (%d)\n", (INT)vol, (INT)er);
			break;
		}
		if ( b != NULL && asz <= size ) {
			return b;
		}
		if ( b != NULL ) Kfree(b);
		size = asz;
		b = (UB *)Kmalloc(size);
		if ( b == NULL ) {
			tm_printf((UB *)"ob: the label of volume %d not read: no memory\n", (INT)vol);
			return NULL;
		}
	}
	if ( b != NULL ) Kfree(b);
	return NULL;
}

/*
 * The superblock's copy of the label written again from the label, the
 * name of the top object, when they differ: a system before that
 * renamed the top, or a copy cut in the middle of a letter. A volume
 * that does not write keeps its copy; reading the label still gives the
 * top's name.
 */
LOCAL void label_copy_again( ID vol, CONST char *path, CONST UB *label )
{
	ER	er = knl_tsfs_label_copy(vol, label);

	if ( er < E_OK && er != E_RONLY && er != E_NOSPT ) {
		tm_printf((UB *)"ob: %s: the label's copy in the superblock not written (%d)\n", path, (INT)er);
	}
}

/*
 * Attaching. The volume is opened outside the lock: opening a native
 * one reads its log back and looks it over, which takes time and the
 * device, and several disks are attached at once on several
 * processors. A path already attached is E_OK; a volume of the same
 * UUID as one attached (a copy of it) is refused with E_OBJ, since the
 * same objects would be found on both.
 */
LOCAL ER attach_vol( CONST char *path, UINT flags )
{
	OBFVOL		*v = NULL;
	T_TSFSVINF	*inf;
	UB		*lab = NULL;
	ID		vol;
	INT		i;
	BOOL		sysbox, foreign = FALSE;
	OBFVOL		alt;			/* the aliases made, until the entry takes them */
	ER		er = E_OK;

	LOCK();
	v = vent_by_path(path);
	UNLOCK();
	if ( v != NULL ) {
		return E_OK;			/* already there */
	}
	vol = ts_opn_vol(path, flags);
	if ( vol <= 0 ) {
		return ( vol < 0 ) ? (ER)vol : E_SYS;
	}
	inf = (T_TSFSVINF *)Kmalloc(sizeof(T_TSFSVINF));
	if ( inf == NULL || knl_tsfs_vol_info(vol, inf) < E_OK ) {
		if ( inf != NULL ) Kfree(inf);
		(void)ts_cls_vol(vol);
		return E_NOMEM;
	}
	if ( inf->native ) {
		lab = label_dup(vol);		/* NULL for want of memory: the copy names it meanwhile */
	}
	{
		TS_UUID	sb;

		/* a system volume: the system box on it (mktsfs --sysbox, the system's registration) */
		sysbox = (BOOL)( inf->native && ts_str_to_uuid(SYSDEF_SYSTEM_BOX, &sb) >= E_OK
				 && knl_tsfs_exists(vol, &sb) >= E_OK );
	}
	knl_memset(&alt, 0, sizeof(alt));
	if ( sysbox ) {
		LOCK();
		foreign = (BOOL)( obf_sys_known && ( knl_ob_uuid_zero(&obf_sysvu)
						     || ts_uuid_cmp(&inf->vuuid, &obf_sysvu) != 0 ) );
		UNLOCK();
		/* another machine's: the aliases of its objects that have this system's UUIDs */
		if ( foreign && !al_build(vol, &inf->vuuid, &alt) ) {
			if ( lab != NULL ) Kfree(lab);
			Kfree(inf);
			(void)ts_cls_vol(vol);
			return E_NOMEM;
		}
	}

	LOCK();
	for ( i = 0; i < obf_nvol && er >= E_OK; i++ ) {
		OBFVOL	*e = obf_vol[i];

		if ( !e->used || e->gone ) {
			if ( !e->used && v == NULL ) v = e;
			continue;
		}
		if ( same_path(e->path, path) ) {
			er = E_OK + 1;		/* attached meanwhile */
		} else if ( !knl_ob_uuid_zero(&inf->vuuid) && ts_uuid_cmp(&e->vuuid, &inf->vuuid) == 0 ) {
			er = E_OBJ;		/* the same volume, through another device */
		}
	}
	if ( er == E_OK && v == NULL ) {
		if ( obf_nvol >= OBF_VOL_MAX ) {
			er = E_LIMIT;
		} else if ( ( v = (OBFVOL *)Kcalloc(1, sizeof(OBFVOL)) ) == NULL ) {
			er = E_NOMEM;
		} else {
			__atomic_store_n(&obf_vol[obf_nvol], v, __ATOMIC_RELEASE);
			__atomic_store_n(&obf_nvol, obf_nvol + 1, __ATOMIC_RELEASE);
		}
	}
	if ( er == E_OK ) {
		v->vol = vol;
		for ( i = 0; path[i] != 0 && i < FS_PATH_MAX - 1; i++ ) {
			v->path[i] = (UB)path[i];
		}
		v->path[i] = 0;
		v->dom = NULL;
		v->native = inf->native;
		v->vuuid = inf->vuuid;
		v->again = known_has(&inf->vuuid);
		v->gone = FALSE;
		v->finished = FALSE;
		if ( !obf_sys_known ) {
			obf_sys_known = TRUE;	/* the first, this machine's */
			obf_sysvu = inf->vuuid;
			v->foreign = FALSE;
		} else {
			v->foreign = foreign;
		}
		v->al = v->foreign ? alt.al : NULL;
		v->al_n = v->foreign ? alt.al_n : 0;
		if ( v->foreign ) alt.al = NULL;
		v->used = TRUE;
		obf_serial++;
		known_note(&inf->vuuid, ( lab != NULL ) ? lab : inf->label, path);
	}
	UNLOCK();
	Kfree(inf);
	if ( alt.al != NULL ) Kfree(alt.al);	/* not taken */
	if ( er != E_OK ) {
		if ( lab != NULL ) Kfree(lab);
		(void)ts_cls_vol(vol);
		return ( er > E_OK ) ? E_OK : er;
	}
	if ( lab != NULL ) {
		label_copy_again(vol, path, lab);
		Kfree(lab);
	}
	if ( v->foreign ) {
		tm_printf((UB *)"ob: %s is another machine's system volume, %d aliases\n", path, v->al_n);
	}

	dom_load(v);
	knl_obdev_volume_up();			/* the device box has somewhere to live */
	if ( v->native ) {
		/* the records of links between it and the volumes there made to agree */
		(void)knl_obxref_sync(vol);
	}

	return E_OK;
}

/* Not while the volume is being checked (ts/tsfsck.h), nor is it checked while it is being attached */
EXPORT ER knl_obfile_attach( CONST char *path, UINT flags )
{
	ER	er;

	if ( path == NULL ) {
		return E_PAR;
	}
	if ( !knl_ob_chk_hold(path, TRUE) ) {
		return E_BUSY;
	}
	er = attach_vol(path, flags);
	(void)knl_ob_chk_hold(path, FALSE);
	return er;
}

/* The entry of the volume attached with a path, looked at under the lock */
LOCAL OBFVOL *vent_by_path( CONST char *path )
{
	INT	i;

	for ( i = 0; path != NULL && i < obf_nvol; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && same_path(v->path, path) ) {
			return v;
		}
	}
	return NULL;
}

/* The volume attached with a path */
LOCAL ID vol_by_path( CONST char *path )
{
	OBFVOL	*v = vent_by_path(path);

	return ( v != NULL ) ? v->vol : E_NOEXS;
}

EXPORT INT knl_obfile_nvol( void )
{
	INT	i, n = 0;

	LOCK();
	for ( i = 0; i < obf_nvol; i++ ) {
		if ( obf_vol[i]->used && !obf_vol[i]->gone ) {
			n++;
		}
	}
	UNLOCK();
	return n;
}

EXPORT INT knl_obfile_vols( UB *buf, INT stride, INT max )
{
	INT	i, k, n = 0;

	if ( buf == NULL || stride <= 0 || max <= 0 ) {
		return 0;
	}
	LOCK();
	for ( i = 0; i < obf_nvol && n < max; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( !v->used || v->gone ) {
			continue;
		}
		for ( k = 0; v->path[k] != 0 && k < stride - 1; k++ ) {
			buf[n * stride + k] = v->path[k];
		}
		buf[n * stride + k] = 0;
		n++;
	}
	UNLOCK();
	return n;
}

EXPORT BOOL knl_obfile_on( CONST TS_UUID *uuid, CONST char *path )
{
	ID	vol;

	if ( uuid == NULL || path == NULL ) {
		return FALSE;
	}
	LOCK();
	vol = vol_by_path(path);
	UNLOCK();
	return (BOOL)( vol > 0 && knl_obfile_vol_of(uuid) == vol );
}

EXPORT ER knl_obfile_alias( CONST char *path, CONST TS_UUID *orig, TS_UUID *p_alias )
{
	OBFVOL		*v;
	CONST TS_UUID	*a = NULL;

	if ( path == NULL || orig == NULL || p_alias == NULL ) {
		return E_PAR;
	}
	LOCK();
	v = vent_by_path(path);
	if ( v != NULL && v->foreign ) {
		a = al_of(v, orig);
		if ( a != NULL ) *p_alias = *a;
	}
	UNLOCK();
	return ( a != NULL ) ? E_OK : E_NOEXS;
}

EXPORT BOOL knl_obfile_foreign( CONST char *path )
{
	OBFVOL	*v;
	BOOL	foreign;

	if ( path == NULL ) {
		return FALSE;
	}
	LOCK();
	v = vent_by_path(path);
	foreign = (BOOL)( v != NULL && v->foreign );
	UNLOCK();
	return foreign;
}

/* The TSFS volume attached with a path, or E_NOEXS */
EXPORT ID knl_obfile_vol_path( CONST char *path )
{
	ID	vol;

	if ( path == NULL ) {
		return E_PAR;
	}
	LOCK();
	vol = vol_by_path(path);
	UNLOCK();
	return vol;
}

/* The path the volume an object is on was attached with */
EXPORT ER knl_obfile_path_of( CONST TS_UUID *uuid, UB *buf, INT max )
{
	ID	vol = knl_obfile_vol_of(uuid);
	INT	i, k;
	ER	er = E_NOEXS;

	if ( vol <= 0 || buf == NULL || max <= 0 ) {
		return E_NOEXS;
	}
	LOCK();
	for ( i = 0; i < obf_nvol && er < E_OK; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && v->vol == vol ) {
			for ( k = 0; v->path[k] != 0 && k < max - 1; k++ ) {
				buf[k] = v->path[k];
			}
			buf[k] = 0;
			er = E_OK;
		}
	}
	UNLOCK();
	return er;
}

/* A transaction of the volume an object is on, begun or ended */
EXPORT ER knl_obfile_trx( CONST TS_UUID *on, BOOL begin, BOOL commit )
{
	ID	vol = knl_obfile_vol_of(on);

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	return begin ? ts_beg_trx(vol) : ts_end_trx(vol, commit);
}

/* An entry let go of: its volume is closed already (under the lock) */
LOCAL void vent_free( OBFVOL *v )
{
	v->used = FALSE;
	v->gone = FALSE;
	if ( v->dom != NULL ) {
		Kfree(v->dom);
		v->dom = NULL;
	}
	if ( v->al != NULL ) {
		Kfree(v->al);
	}
	v->al = NULL;
	v->al_n = 0;
	v->foreign = FALSE;
	obf_serial++;
}

/*
 * Detaching: E_BUSY while an object of the volume is open (ts_cls_vol
 * says so with E_OBJ), and then nothing changes.
 */
EXPORT ER knl_obfile_detach( CONST char *path )
{
	OBFVOL	*v;
	ER	er = E_NOEXS;

	LOCK();
	v = vent_by_path(path);
	if ( v != NULL ) {
		/* refused only while an object of it is open; any other answer comes after it is closed */
		er = ts_cls_vol(v->vol);
		if ( er == E_OBJ ) {
			er = E_BUSY;
		} else {
			vent_free(v);
		}
	}
	UNLOCK();

	return er;
}

/*
 * The machine going off: each volume of objects, one by one, told on the
 * console. The volume's lock is not held over the writing, which takes
 * time and the device. 'finish' FALSE: what is committed made sure to be
 * on the medium, once more when that fails; how many failed. TRUE: each
 * left clean and read only; how many could not be.
 */
EXPORT INT knl_obfile_shut( BOOL finish )
{
	INT	i, n, bad = 0;

	if ( obf_mtx <= 0 ) {
		return 0;
	}
	n = __atomic_load_n(&obf_nvol, __ATOMIC_ACQUIRE);
	for ( i = 0; i < n; i++ ) {
		OBFVOL	*v = obf_vol[i];
		UB	path[FS_PATH_MAX];
		BOOL	used, gone, native, done;
		ID	vol;
		ER	er;

		LOCK();
		used = v->used;
		gone = v->gone;
		native = v->native;
		done = v->finished;
		vol = v->vol;
		knl_strcpy((char *)path, (CONST char *)v->path);
		UNLOCK();
		if ( !used || done ) {
			continue;
		}
		if ( gone ) {
			tm_printf((UB *)"shutdown: volume %s: its disk has gone, nothing written\n", path);
			continue;
		}
		if ( !finish ) {
			er = ts_syn_vol(vol);
			if ( er < E_OK ) {
				tm_printf((UB *)"shutdown: volume %s: not written (%d), once more\n", path, (INT)er);
				er = ts_syn_vol(vol);
			}
			tm_printf((UB *)"shutdown: volume %s: %s (%d)\n", path, ( er >= E_OK ) ? "written" : "COULD NOT BE WRITTEN", (INT)er);
			if ( er < E_OK ) bad++;
			continue;
		}
		if ( !native ) {
			continue;
		}
		er = ts_finish_vol(vol);
		if ( er == E_RONLY ) {
			tm_printf((UB *)"shutdown: volume %s: read only, left as it was\n", path);
		} else {
			tm_printf((UB *)"shutdown: volume %s: %s (%d)\n", path, ( er >= E_OK ) ? "marked clean" : "NOT MARKED CLEAN", (INT)er);
			if ( er < E_OK ) bad++;
		}
		LOCK();
		v->finished = TRUE;
		UNLOCK();
	}
	return bad;
}

/* Whether a volume's path is the device's own, or a store on its mount ("/media/uda0/...") */
LOCAL BOOL path_on_dev( CONST UB *path, CONST UB *devnm )
{
	CONST char	*dir = FS_MEDIA_DIR;
	INT		i, k;

	if ( same_path(path, (CONST char *)devnm) ) {
		return TRUE;
	}
	for ( i = 0; dir[i] != 0; i++ ) {
		if ( path[i] != (UB)dir[i] ) return FALSE;
	}
	if ( path[i++] != '/' ) {
		return FALSE;
	}
	for ( k = 0; devnm[k] != 0; k++ ) {
		if ( path[i + k] != devnm[k] ) return FALSE;
	}
	return (BOOL)( path[i + k] == 0 || path[i + k] == '/' );
}

/* Gone volumes closed if nothing of them is open now. Under the lock */
LOCAL void reap( void )
{
	INT	i;

	for ( i = 0; i < obf_nvol; i++ ) {
		OBFVOL	*v = obf_vol[i];

		/*
		 * Closed unless an object of it is open (E_OBJ). The writes that
		 * fail on the device that is not there make the answer an error,
		 * but the volume is closed all the same, and its number may be
		 * another's soon: the entry goes now.
		 */
		if ( v->used && v->gone && ts_cls_vol(v->vol) != E_OBJ ) {
			vent_free(v);
		}
	}
}

/*
 * The medium of a device went, or the device: every volume on it is
 * gone. One with nothing open is closed now; the writes still waiting
 * fail on the device that is not there, which is what they would do
 * whenever it went. Answers how many there were.
 *
 * One that stays open is held by the objects of it that are open. Their
 * watchers are told OB_E_DETACH, as of a medium that went, and the keys
 * that read nothing but the attributes (held only for notices, libtv's
 * ta_watch) are closed, as a detach closes them (ob_det_vol); a watcher
 * that reads more lets its key go when told. The volume closes with the
 * last key (obf_close), letting the device's name go. The UUIDs are
 * gathered under the lock and told after it, so that nothing is sent and
 * no key is closed while the volumes' lock is held.
 */
#define OBF_TELL_MAX	128

EXPORT INT knl_obfile_revoke( CONST UB *devnm )
{
	TS_UUID	*tell;
	INT	i, k, n = 0, nt = 0;

	if ( devnm == NULL || devnm[0] == 0 || obf_mtx <= 0 ) {
		return 0;
	}
	tell = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * OBF_TELL_MAX);
	LOCK();
	for ( i = 0; i < obf_nvol; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && path_on_dev(v->path, devnm) ) {
			v->gone = TRUE;
			obf_serial++;
			n++;
		}
	}
	if ( n > 0 ) {
		reap();
		for ( i = 0; i < obf_nvol && tell != NULL; i++ ) {
			OBFVOL	*v = obf_vol[i];

			if ( v->used && v->gone && path_on_dev(v->path, devnm) && nt < OBF_TELL_MAX ) {
				INT	j;

				k = knl_tsfs_vol_opened(v->vol, tell + nt, OBF_TELL_MAX - nt);
				k = ( k < OBF_TELL_MAX - nt ) ? k : OBF_TELL_MAX - nt;
				/* another machine's objects are known by their aliases: this system's are not told */
				for ( j = nt; j < nt + k; j++ ) {
					CONST TS_UUID	*a = al_of(v, &tell[j]);

					if ( a != NULL ) tell[j] = *a;
				}
				nt += k;
			}
		}
	}
	UNLOCK();
	if ( n > 0 ) {
		tm_printf((UB *)"ob: %d volume(s) on %s gone, %d object(s) of them open\n", n, (CONST char *)devnm, nt);
	}
	for ( i = 0; i < nt; i++ ) {
		knl_ob_post(&tell[i], -1, OB_E_DETACH, NULL);
	}
	if ( nt > 0 ) {
		knl_ob_watch_keys_drop_of(tell, nt);	/* as a detach lets them go (ob_det_vol) */
	}
	if ( tell != NULL ) {
		Kfree(tell);
	}
	return n;
}

/* ---------------------------------------------------------------- volumes by their UUIDs */

/*
 * The volumes attached since the start, by their own UUIDs, with the
 * names they were known by: a link to an object of one that is not
 * attached now says which to attach (design 8.3). The list grows, twice
 * as large each time it is full; the name is the whole label.
 */
#define OBF_KNOWN_FIRST	16

typedef struct {
	TS_UUID	vuuid;
	UB	*name;			/* its label, or the path it was attached with (Kmalloc) */
} OBFKNOWN;

LOCAL OBFKNOWN	*obf_known = NULL;
LOCAL INT	obf_nknown = 0;
LOCAL INT	obf_known_cap = 0;

/* A copy of a text in memory of its own (NULL for want of it) */
LOCAL UB *text_dup( CONST UB *t )
{
	UB	*d;
	SZ	n;

	for ( n = 0; t[n] != 0; n++ ) ;
	d = (UB *)Kmalloc(n + 1);
	if ( d != NULL ) {
		knl_memcpy(d, t, (INT)n + 1);
	}
	return d;
}

/*
 * A volume noted with its name: its label, else the path it was attached
 * with (path NULL: the name it had stays). Under the lock. Memory that
 * runs out leaves it as it was and says so on the console.
 */
LOCAL void known_note( CONST TS_UUID *vuuid, CONST UB *label, CONST char *path )
{
	OBFKNOWN	*k = NULL;
	UB		*nm;
	INT		i;

	if ( knl_ob_uuid_zero(vuuid) ) {
		return;
	}
	if ( label != NULL && label[0] != 0 ) {
		nm = text_dup(label);
	} else if ( path != NULL ) {
		nm = text_dup((CONST UB *)path);
	} else {
		return;
	}
	if ( nm == NULL ) {
		tm_printf((UB *)"ob: the name of a volume not noted: no memory\n");
		return;
	}
	for ( i = 0; i < obf_nknown && k == NULL; i++ ) {
		if ( ts_uuid_cmp(&obf_known[i].vuuid, vuuid) == 0 ) k = &obf_known[i];
	}
	if ( k == NULL ) {
		if ( obf_nknown >= obf_known_cap ) {
			INT		cap = ( obf_known_cap > 0 ) ? obf_known_cap * 2 : OBF_KNOWN_FIRST;
			OBFKNOWN	*t = (OBFKNOWN *)Kmalloc(sizeof(OBFKNOWN) * (SZ)cap);

			if ( t == NULL ) {
				tm_printf((UB *)"ob: the volume \"%s\" not noted: no memory\n", (CONST char *)nm);
				Kfree(nm);
				return;
			}
			if ( obf_known != NULL ) {
				knl_memcpy(t, obf_known, (INT)( sizeof(OBFKNOWN) * (SZ)obf_nknown ));
				Kfree(obf_known);
			}
			obf_known = t;
			obf_known_cap = cap;
		}
		k = &obf_known[obf_nknown++];
		k->vuuid = *vuuid;
		k->name = NULL;
	}
	if ( k->name != NULL ) {
		Kfree(k->name);
	}
	k->name = nm;
}

/* Whether a volume was attached before since the start. Under the lock */
LOCAL BOOL known_has( CONST TS_UUID *vuuid )
{
	INT	i;

	if ( knl_ob_uuid_zero(vuuid) ) {
		return FALSE;
	}
	for ( i = 0; i < obf_nknown; i++ ) {
		if ( ts_uuid_cmp(&obf_known[i].vuuid, vuuid) == 0 ) return TRUE;
	}
	return FALSE;
}

/*
 * Whether the volume attached with a path had been attached before since
 * the start: its disk was taken out and put back. What is emptied at
 * each start is not emptied again for it.
 */
EXPORT BOOL knl_obfile_again( CONST char *path )
{
	OBFVOL	*v;
	BOOL	again;

	LOCK();
	v = vent_by_path(path);
	again = (BOOL)( v != NULL && v->again );
	UNLOCK();
	return again;
}

EXPORT ER knl_obfile_vol_nam( CONST TS_UUID *vuuid, UB *buf, SZ size, SZ *p_asize )
{
	INT	i;
	ER	er = E_NOEXS;

	if ( vuuid == NULL || size < 0 || ( size > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	LOCK();
	for ( i = 0; i < obf_nknown && er < E_OK; i++ ) {
		if ( ts_uuid_cmp(&obf_known[i].vuuid, vuuid) == 0 ) {
			er = knl_ob_name_put(obf_known[i].name, -1, buf, size, p_asize);
		}
	}
	UNLOCK();
	return er;
}

/* The UUID of the volume an object is on (E_NOEXS: on none attached; zero for a volume without one) */
EXPORT ER knl_obfile_vuuid_of( CONST TS_UUID *uuid, TS_UUID *p_vuuid )
{
	ID	vol = knl_obfile_vol_of(uuid);
	INT	i;
	ER	er = E_NOEXS;

	if ( vol <= 0 || p_vuuid == NULL ) {
		return E_NOEXS;
	}
	LOCK();
	for ( i = 0; i < obf_nvol && er < E_OK; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && v->vol == vol ) {
			*p_vuuid = v->vuuid;
			er = E_OK;
		}
	}
	UNLOCK();
	return er;
}

EXPORT ER knl_obfile_vuuid_near( CONST TS_UUID *uuid, CONST TS_UUID *near, TS_UUID *p_vuuid )
{
	INT	i;
	ID	vol = 0;

	if ( uuid == NULL || p_vuuid == NULL ) {
		return E_PAR;
	}
	if ( near != NULL && !knl_ob_uuid_zero(near) && ts_uuid_cmp(uuid, &ob_uuid_domain) != 0 ) {
		LOCK();
		for ( i = 0; i < obf_nvol && vol <= 0; i++ ) {
			OBFVOL	*v = obf_vol[i];

			if ( v->used && !v->gone && ts_uuid_cmp(&v->vuuid, near) == 0 ) {
				vol = v->vol;
			}
		}
		UNLOCK();
		if ( vol > 0 && knl_tsfs_exists(vol, uuid) >= E_OK ) {
			*p_vuuid = *near;
			return E_OK;
		}
	}
	return knl_obfile_vuuid_of(uuid, p_vuuid);
}

/* Whether the volume of a UUID is attached now */
EXPORT BOOL knl_obfile_vuuid_on( CONST TS_UUID *vuuid )
{
	INT	i;
	BOOL	on = FALSE;

	LOCK();
	for ( i = 0; i < obf_nvol && !on; i++ ) {
		OBFVOL	*v = obf_vol[i];

		on = (BOOL)( v->used && !v->gone && ts_uuid_cmp(&v->vuuid, vuuid) == 0 );
	}
	UNLOCK();
	return on;
}

/*
 * This machine as the outer part of a link's id names it: the UUID of
 * its system volume, the first attached.
 */
EXPORT ER knl_obfile_machine( TS_UUID *p_uuid )
{
	INT	i;
	ER	er = E_NOEXS;

	LOCK();
	for ( i = 0; i < obf_nvol && er < E_OK; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && !v->gone && !v->foreign ) {
			*p_uuid = v->vuuid;
			er = E_OK;
		}
	}
	UNLOCK();
	return er;
}

EXPORT UW knl_obfile_serial( void )
{
	return __atomic_load_n(&obf_serial, __ATOMIC_ACQUIRE);
}

EXPORT ER knl_obfile_vol_ref( CONST char *path, T_OBFVREF *pk )
{
	T_TSFSVINF	*inf;
	ID		vol;
	ER		er;

	if ( path == NULL || pk == NULL ) {
		return E_PAR;
	}
	LOCK();
	vol = vol_by_path(path);
	UNLOCK();
	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	inf = (T_TSFSVINF *)Kmalloc(sizeof(T_TSFSVINF));
	if ( inf == NULL ) {
		return E_NOMEM;
	}
	er = knl_tsfs_vol_info(vol, inf);
	if ( er >= E_OK ) {
		knl_memset(pk, 0, sizeof(*pk));
		pk->native = inf->native;
		pk->rdonly = inf->rdonly;
		pk->vuuid = inf->vuuid;
		pk->top = inf->top;
		pk->open = inf->open;
		pk->system = (BOOL)( vol == knl_obfile_first_vol() );
		pk->foreign = knl_obfile_foreign(path);
	}
	Kfree(inf);
	return er;
}

EXPORT ER knl_obfile_vol_label( CONST char *path, UB *buf, SZ size, SZ *p_asize )
{
	ID	vol;

	if ( path == NULL || size < 0 || ( size > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	LOCK();
	vol = vol_by_path(path);
	UNLOCK();
	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	return knl_tsfs_vol_label(vol, buf, size, p_asize);
}

/*
 * After an object was renamed: when it is the top of its native volume,
 * the label changed with it. The superblock's copy is written again, cut
 * at a whole letter, and the name the volume is known by follows. An
 * object that is no volume's top is nothing to it (E_OK).
 */
EXPORT ER knl_obfile_label_sync( CONST TS_UUID *uuid )
{
	T_TSFSVINF	*inf;
	UB		*lab;
	TS_UUID		vu;
	ID		vol;
	BOOL		top;
	ER		er;

	if ( uuid == NULL ) {
		return E_PAR;
	}
	vol = knl_obfile_vol_of(uuid);
	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	inf = (T_TSFSVINF *)Kmalloc(sizeof(T_TSFSVINF));
	if ( inf == NULL ) {
		return E_NOMEM;
	}
	er = knl_tsfs_vol_info(vol, inf);
	top = (BOOL)( er >= E_OK && inf->native && ts_uuid_cmp(&inf->top, uuid) == 0 );
	vu = inf->vuuid;
	Kfree(inf);
	if ( !top ) {
		return ( er < E_OK ) ? er : E_OK;
	}
	lab = label_dup(vol);
	if ( lab == NULL ) {
		return E_NOMEM;
	}
	er = knl_tsfs_label_copy(vol, lab);
	if ( er == E_RONLY ) {
		er = E_OK;			/* the copy stays; the label is still the top's name */
	}
	LOCK();
	known_note(&vu, lab, NULL);
	obf_serial++;
	UNLOCK();
	Kfree(lab);
	return er;
}

/*
 * The top object of a volume set: held once, so that it is never
 * garbage though nothing on the volume links to it. One set before is
 * let go of.
 */
EXPORT ER knl_obfile_set_top( CONST char *path, CONST TS_UUID *uuid )
{
	T_OBFVREF	ref;
	ID		vol;
	ER		er;

	er = knl_obfile_vol_ref(path, &ref);
	if ( er < E_OK ) {
		return er;
	}
	vol = knl_obfile_vol_path(path);
	if ( !ref.native ) {
		return E_NOSPT;
	}
	if ( uuid != NULL && knl_tsfs_exists(vol, uuid) < E_OK ) {
		return E_NOEXS;			/* only an object of the volume is its top */
	}
	er = knl_tsfs_set_top(vol, uuid);
	if ( er < E_OK ) {
		return er;
	}
	if ( !knl_ob_uuid_zero(&ref.top) && ( uuid == NULL || ts_uuid_cmp(&ref.top, uuid) != 0 ) ) {
		(void)ts_unl_obj(vol, &ref.top);
	}
	if ( uuid != NULL && ( knl_ob_uuid_zero(&ref.top) || ts_uuid_cmp(&ref.top, uuid) != 0 ) ) {
		(void)ts_lnk_obj(vol, uuid);
	}
	LOCK();
	obf_serial++;
	UNLOCK();
	return E_OK;
}

/*
 * The top of a native volume just made (knl_tsfs_format, which works
 * below the object layer and gives it no protection) given the default
 * protection of an object crd makes, as ob_cre_obj gives one made with
 * none said: owner and group of crd, mode 0644, no ACL. A top that has a
 * protection already is left as it is.
 */
LOCAL ER obf_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt );

EXPORT ER knl_obfile_top_prot( CONST char *path, CONST T_OBCRD *crd )
{
	T_OBFVREF	ref;
	T_OBPRT		*prt;
	UB		*j = NULL;
	INT		len;
	ID		vol;
	ER		er;

	if ( path == NULL || crd == NULL ) {
		return E_PAR;
	}
	er = knl_obfile_vol_ref(path, &ref);
	if ( er < E_OK ) {
		return er;
	}
	if ( !ref.native ) {
		return E_NOSPT;
	}
	if ( knl_ob_uuid_zero(&ref.top) ) {
		return E_NOEXS;
	}
	vol = knl_obfile_vol_path(path);
	len = knl_tsfs_meta_all(vol, &ref.top, 0, &j, NULL);
	if ( len < 0 ) {
		if ( j != NULL ) Kfree(j);
		return (ER)len;
	}
	if ( knl_oj_path(j, len, "tessronos", "access") >= 0 ) {
		Kfree(j);
		return E_OK;			/* protected already */
	}
	if ( j != NULL ) Kfree(j);
	prt = knl_ob_prt_new(0, 0);
	if ( prt == NULL ) {
		return E_NOMEM;
	}
	knl_ob_prt_default(prt, crd);
	er = obf_setprot(&ref.top, prt);
	Kfree(prt);
	return er;
}

EXPORT INT knl_obfile_slots( INT *p_used )
{
	INT	i, n, used = 0;

	LOCK();
	n = obf_nvol;
	for ( i = 0; i < n; i++ ) {
		if ( obf_vol[i]->used ) used++;
	}
	UNLOCK();
	if ( p_used != NULL ) *p_used = used;
	return n;
}

/* ---------------------------------------------------------------- record types */

/* The declarations of the metadata, at most max into d; with d NULL, how many there are */
LOCAL INT decl_read( CONST UB *j, INT len, OBFDECL *d, INT max )
{
	INT	arr = knl_oj_path(j, len, "tessronos", "records"), pos = arr, e, n = 0;

	while ( arr >= 0 && n < max && ( e = knl_oj_next(j, len, arr, &pos) ) >= 0 ) {
		D	v;

		if ( j[e] != '{' ) continue;
		if ( !knl_oj_num(j, len, knl_oj_member(j, len, e, "n"), &v) ) continue;
		if ( d == NULL ) {
			n++;
			continue;
		}
		d[n].n = (INT)v;
		d[n].rt = knl_oj_num(j, len, knl_oj_member(j, len, e, "rt"), &v)
			? (UINT)v : OB_RT_SYSDATA;
		d[n].sub = knl_oj_num(j, len, knl_oj_member(j, len, e, "sub"), &v)
			 ? (UINT)v : 0;
		n++;
	}
	return n;
}

/* The room the text of `n` declarations takes: three numbers of up to 11 letters each */
#define DECL_TEXT_MAX	( ( n ) * 56 + 8 )

LOCAL INT decl_write( UB *j, INT len, INT max, CONST OBFDECL *d, INT n )
{
	UB	*t;
	INT	k = 0, i;

	t = (UB *)Kmalloc(DECL_TEXT_MAX);
	if ( t == NULL ) {
		return -1;
	}
	k = knl_oj_put(t, k, DECL_TEXT_MAX, "[");
	for ( i = 0; i < n; i++ ) {
		k = knl_oj_put(t, k, DECL_TEXT_MAX, ( i > 0 ) ? ",{\"n\":" : "{\"n\":");
		k = knl_oj_put_num(t, k, DECL_TEXT_MAX, d[i].n);
		k = knl_oj_put(t, k, DECL_TEXT_MAX, ",\"rt\":");
		k = knl_oj_put_num(t, k, DECL_TEXT_MAX, d[i].rt);
		k = knl_oj_put(t, k, DECL_TEXT_MAX, ",\"sub\":");
		k = knl_oj_put_num(t, k, DECL_TEXT_MAX, d[i].sub);
		k = knl_oj_put(t, k, DECL_TEXT_MAX, "}");
	}
	k = knl_oj_put(t, k, DECL_TEXT_MAX, "]");
	if ( k >= 0 ) {
		k = knl_oj_set_path(j, len, max, "tessronos", "records", t, k);
	}
	Kfree(t);
	return k;
}

/* The record the metadata says holds the program, or -1 */
LOCAL INT exec_rec( CONST UB *j, INT len )
{
	INT	v = knl_oj_path(j, len, "tessronos", "exec");
	D	r;

	if ( v >= 0 && knl_oj_num(j, len, knl_oj_member(j, len, v, "record"), &r) ) {
		return (INT)r;
	}
	return -1;
}

/* ---------------------------------------------------------------- the manager */

LOCAL ER obf_find( CONST TS_UUID *uuid )
{
	return ( knl_obfile_vol_of(uuid) > 0 ) ? E_OK : E_NOEXS;
}

LOCAL ER obf_ref( CONST TS_UUID *uuid, T_OBREF *r )
{
	T_ROBJ	ro;
	TS_UUID	ru;
	ID	vol = obf_real(uuid, &ru);
	ER	er;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	er = ts_ref_obj(vol, &ru, &ro);
	if ( er < E_OK ) {
		return er;
	}
	r->refcnt = ro.refcnt;
	r->nrec = ro.nrec;
	r->size = ro.size;
	if ( ( ro.flags & TSFS_F_AUTOREF ) != 0 ) {
		r->flags |= OB_F_AUTOREF;
	}

	return E_OK;
}

/* The name, whole, as the metadata holds it */
LOCAL ER obf_refnam( CONST TS_UUID *uuid, UB *buf, SZ size, SZ *p_asize )
{
	TS_UUID	ru;
	ID	vol = obf_real(uuid, &ru);

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	return ts_ref_nam(vol, &ru, buf, size, p_asize);
}

LOCAL ER obf_prot( CONST TS_UUID *uuid, T_OBPRT **p_prt )
{
	UB	*j = NULL;
	INT	len;
	TS_UUID	ru;
	ID	vol = obf_real(uuid, &ru);
	ER	er;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	len = knl_tsfs_meta_all(vol, &ru, 0, &j, NULL);
	er = ( len < 0 ) ? (ER)len : knl_ob_prt_parse(j, len, p_prt);
	if ( j != NULL ) Kfree(j);

	return er;
}

LOCAL ER obf_setprot( CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	UB	*j = NULL;
	INT	len, cap = 0;
	TS_UUID	ru;
	ID	vol = obf_real(uuid, &ru);
	ER	er;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	len = knl_tsfs_meta_all(vol, &ru, 0, &j, &cap);
	er = ( len < 0 ) ? (ER)len : E_OK;
	if ( er >= E_OK ) {
		len = knl_ob_prt_store(&j, len, &cap, prt);
		er = ( len < 0 ) ? E_NOMEM : knl_tsfs_set_meta(vol, &ru, j, len);
	}
	if ( er >= E_OK ) {
		/* the store's own flags say the same as the attributes */
		er = knl_tsfs_set_flags(vol, &ru,
				( ( prt->attr & OB_A_RONLY ) ? 0 : TSFSO_F_EDITABLE )
				| ( ( prt->attr & OB_A_PERM ) ? 0 : TSFSO_F_DELETABLE ),
				TSFSO_F_EDITABLE | TSFSO_F_DELETABLE);
	}
	if ( j != NULL ) Kfree(j);

	return er;
}

LOCAL ER obf_create( CONST T_OBCRE *c, CONST T_OBPRT *prt, TS_UUID *p_uuid )
{
	T_COBJ	cobj;
	UB	*j, *q = NULL;
	SZ	qn = 0;
	INT	len, i, cap;
	ID	vol;
	ER	er;

	vol = !knl_ob_uuid_zero(&c->near) ? knl_obfile_vol_of(&c->near)
	    : ( c->vol != NULL ) ? vol_by_path(c->vol) : knl_obfile_first_vol();
	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	{
		/* making an object on a volume is writing to it */
		CONST T_OBCRD	*crd = knl_ob_crd_of(ts_get_pid());
		BOOL		may = TRUE;

		LOCK();
		for ( i = 0; i < obf_nvol; i++ ) {
			OBFVOL	*e = obf_vol[i];

			if ( e->used && !e->gone && e->vol == vol && e->dom != NULL ) {
				may = ( knl_ob_permit(crd, e->dom, -1) & OB_OP_WRITE ) != 0;
			}
		}
		UNLOCK();
		if ( !may ) {
			return E_OACV;		/* the volume's domain does not let it */
		}
	}
	/* the metadata given, however long, with room for the name */
	len = ( c->json != NULL && c->jsonsz > 0 && c->jsonsz < 0x3FFFFFFF ) ? (INT)c->jsonsz : 0;
	for ( i = 0; len == 0 && obf_default_json[i] != 0; i++ ) ;
	/* the name quoted as JSON quotes it, however long, and room for it in the metadata */
	if ( c->name != NULL ) {
		q = om_json_quote_dup(c->name, -1, &qn);
		if ( q == NULL ) {
			return E_NOMEM;
		}
		/* metadata that could not be written is not made (OB_META_LIMIT) */
		if ( qn + (SZ)( ( len > 0 ) ? len : i ) + 64 > (SZ)OB_META_LIMIT ) {
			Kfree(q);
			return E_LIMIT;
		}
	}
	cap = ( len > 0 ) ? len : i;
	cap += (INT)qn + 64;
	j = (UB *)Kmalloc((SZ)cap + 1);
	if ( j == NULL ) {
		if ( q != NULL ) Kfree(q);
		return E_NOMEM;
	}
	if ( len > 0 ) {
		knl_memcpy(j, c->json, len);
	} else {
		for ( i = 0; obf_default_json[i] != 0; i++ ) {
			j[len++] = (UB)obf_default_json[i];
		}
	}
	j[len] = 0;
	if ( q != NULL ) {
		len = knl_oj_set(j, len, cap, knl_oj_root(j, len), "name", q, (INT)qn);
		Kfree(q);
	}
	if ( len >= 0 ) {
		len = knl_ob_prt_store(&j, len, &cap, prt);
	}
	if ( len < 0 ) {
		Kfree(j);
		return E_PAR;
	}
	if ( c->icon != NULL && c->iconsz > 0 ) {
		/* made with its icon, in one transaction on the native store */
		er = knl_tsfs_cre_icon(vol, knl_ob_uuid_zero(&c->uuid) ? NULL : &c->uuid, j, len,
				      (CONST UB *)c->icon, c->iconsz, p_uuid);
		Kfree(j);
		return er;
	}
	if ( !knl_ob_uuid_zero(&c->uuid) ) {
		/* a UUID is one object's on every volume attached, not one on each; an alias is no object's own */
		if ( al_is(&c->uuid) ) {
			Kfree(j);
			return E_PAR;
		}
		if ( knl_obfile_vol_of(&c->uuid) > 0 ) {
			Kfree(j);
			return E_OBJ;
		}
		er = knl_tsfs_cre_as(vol, &c->uuid, j, len);
		if ( er >= E_OK ) {
			*p_uuid = c->uuid;
		}
		Kfree(j);
		return er;
	}
	cobj.json = j;
	cobj.jsonsz = len;
	er = ts_cre_obj(vol, &cobj, p_uuid);
	Kfree(j);

	return er;
}

LOCAL ER obf_remove( CONST TS_UUID *uuid )
{
	TS_UUID	ru;
	ID	vol = obf_real(uuid, &ru);
	ER	er;

	er = ( vol > 0 ) ? ts_del_obj(vol, &ru) : E_NOEXS;
	knl_obxref_drain();			/* links to other volumes it took with it */
	return er;
}

LOCAL INT obf_open( CONST TS_UUID *uuid, UINT ops )
{
	UINT	omode = TFO_READ;
	TS_UUID	ru;
	ID	vol = obf_real(uuid, &ru), od;
	INT	i;

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	if ( ( ops & ( OB_OP_WRITE | OB_OP_RECORD | OB_OP_ATRWR ) ) != 0 ) {
		omode |= TFO_WRITE;
	}
	/*
	 * A key that only reads the attributes -- one held to be told of
	 * changes, as the settings objects are watched -- is not to keep
	 * the one who writes the object from opening it.
	 */
	if ( ( ops & OB_OP_ALL & ~OB_OP_ATRRD ) == 0 ) {
		omode |= TFO_SHARE;
	}
	/* the exclusive mode asked for (OB_O_EXCL, OB_O_WEXCL, OB_O_NOEXCL) */
	if ( ( ops & OB_O_EXCL ) != 0 ) {
		omode |= TFO_EXCL;
	} else if ( ( ops & OB_O_WEXCL ) != 0 ) {
		omode |= TFO_WEXCL;
	} else if ( ( ops & OB_O_NOEXCL ) != 0 ) {
		omode |= TFO_NOEXCL;
	}
	od = ts_opn_obj(vol, &ru, omode);
	if ( od <= 0 ) {
		return ( od < 0 ) ? (INT)od : E_SYS;
	}
	LOCK();
	for ( i = 0; i < OBF_HND_MAX; i++ ) {
		if ( !obf_hnd[i].used ) {
			obf_hnd[i].used = TRUE;
			obf_hnd[i].vol = vol;
			obf_hnd[i].od = od;
			UNLOCK();
			return i + 1;
		}
	}
	UNLOCK();
	ts_cls_obj(od);

	return E_LIMIT;
}

LOCAL OBFHND *hnd_of( INT h )
{
	return ( h >= 1 && h <= OBF_HND_MAX && obf_hnd[h - 1].used ) ? &obf_hnd[h - 1] : NULL;
}

/*
 * Whether the volume a key's object is on went with its medium: the key
 * reads and writes nothing more, whatever the caches still hold, and is
 * only to be closed.
 */
LOCAL BOOL hnd_gone( CONST OBFHND *o )
{
	INT	i;
	BOOL	gone = FALSE;

	LOCK();
	for ( i = 0; i < obf_nvol; i++ ) {
		OBFVOL	*v = obf_vol[i];

		if ( v->used && v->vol == o->vol ) {
			gone = v->gone;
			break;
		}
	}
	UNLOCK();
	return gone;
}

LOCAL ER obf_close( INT h )
{
	OBFHND	*o = hnd_of(h);
	ER	er;

	if ( o == NULL ) {
		return E_ID;
	}
	er = ts_cls_obj(o->od);
	o->used = FALSE;
	knl_obxref_drain();			/* links to other volumes made or given up */

	/* a volume whose medium went is closed once nothing of it is open */
	LOCK();
	reap();
	UNLOCK();

	return er;
}

/*
 * A record of another machine's object read with the UUIDs in it turned
 * into their aliases: read with a UUID's length more on each side, so
 * that one cut by where the read begins or ends is turned whole, and the
 * part asked for given. The lengths are those of the record.
 */
#define AL_EDGE		36

LOCAL ER al_rea( OBFVOL *v, OBFHND *o, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	D	from = ( off > AL_EDGE ) ? off - AL_EDGE : 0;
	SZ	pre = (SZ)( off - from ), got = 0, n;
	UB	*w;
	ER	er;

	if ( buf == NULL || size <= 0 ) {
		return ts_rea_rec(o->od, recno, off, buf, size, p_asize);
	}
	w = (UB *)Kmalloc(size + pre + AL_EDGE);
	if ( w == NULL ) {
		return E_NOMEM;
	}
	er = ts_rea_rec(o->od, recno, from, w, size + pre + AL_EDGE, &got);
	if ( er >= E_OK ) {
		(void)al_swap(v, w, got, TRUE);
		n = ( got > pre ) ? got - pre : 0;
		if ( n > size ) n = size;
		knl_memcpy(buf, w + pre, (INT)n);
		if ( p_asize != NULL ) *p_asize = n;
	}
	Kfree(w);
	return er;
}

/* Bytes to be written to another machine's object, the aliases in them turned back, in memory of their own */
LOCAL UB *al_back( OBFVOL *v, CONST void *buf, SZ size )
{
	UB	*w = (UB *)Kmalloc(( size > 0 ) ? size : 1);

	if ( w != NULL && size > 0 ) {
		knl_memcpy(w, buf, (INT)size);
		(void)al_swap(v, w, size, FALSE);
	}
	return w;
}

LOCAL ER obf_rea( INT h, INT recno, D off, void *buf, SZ size, SZ *p_asize, BOOL nowait )
{
	OBFHND	*o = hnd_of(h);
	OBFVOL	*v;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	(void)nowait;
	if ( o != NULL && ( v = al_vol_of_hnd(o) ) != NULL ) {
		return al_rea(v, o, recno, off, buf, size, p_asize);
	}
	return ( o != NULL ) ? ts_rea_rec(o->od, recno, off, buf, size, p_asize) : E_ID;
}

LOCAL ER obf_wri( INT h, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize,
		  BOOL nowait )
{
	OBFHND	*o = hnd_of(h);
	OBFVOL	*v;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	(void)nowait;
	if ( o != NULL && buf != NULL && ( v = al_vol_of_hnd(o) ) != NULL ) {
		UB	*w = al_back(v, buf, size);
		ER	er;

		if ( w == NULL ) {
			return E_NOMEM;
		}
		er = ts_wri_rec(o->od, recno, off, w, size, p_asize);
		Kfree(w);
		return er;
	}
	return ( o != NULL ) ? ts_wri_rec(o->od, recno, off, buf, size, p_asize) : E_ID;
}

/* An open object's metadata whole (knl_meta_all) */
LOCAL ER od_meta_get( void *ctx, UB *buf, SZ size, SZ *p_asize )
{
	return ts_get_met(((OBFHND *)ctx)->od, buf, size, p_asize);
}

LOCAL INT od_meta_all( OBFHND *o, INT room, UB **p_buf, INT *p_cap )
{
	return knl_meta_all(od_meta_get, o, room, p_buf, p_cap);
}

/* The declarations in the metadata changed: one added (rt given) or taken away */
LOCAL ER decl_change( OBFHND *o, INT recno, BOOL add, UINT rt, UINT sub )
{
	OBFDECL	*d;
	UB	*j = NULL;
	INT	n, i, k, len, cap = 0;
	ER	er;

	/* the metadata whole, with room for the declarations written again */
	len = od_meta_all(o, 0, &j, NULL);
	if ( len < 0 ) {
		return (ER)len;
	}
	n = decl_read(j, len, NULL, 0x7FFFFFFF) + 1;
	Kfree(j);
	j = NULL;
	len = od_meta_all(o, DECL_TEXT_MAX, &j, &cap);
	d = ( len >= 0 ) ? (OBFDECL *)Kmalloc(sizeof(OBFDECL) * (SZ)n) : NULL;
	if ( len < 0 || d == NULL ) {
		if ( j != NULL ) Kfree(j);
		return ( len < 0 ) ? (ER)len : E_NOMEM;
	}
	k = n;
	n = decl_read(j, len, d, k - 1);
	for ( i = 0, k = 0; i < n; i++ ) {
		if ( d[i].n != recno ) {
			d[k++] = d[i];
		}
	}
	if ( !add && k == n ) {
		Kfree(j);
		Kfree(d);
		return E_OK;			/* nothing declared for it */
	}
	n = k;
	if ( add ) {
		d[n].n = recno;
		d[n].rt = rt;
		d[n].sub = sub;
		n++;
	}
	len = decl_write(j, len, cap, d, n);
	er = ( len < 0 ) ? E_LIMIT : ts_set_met(o->od, j, (SZ)len);
	Kfree(j);
	Kfree(d);

	return er;
}

LOCAL ER obf_apd( INT h, UINT rt, UINT sub, INT *p_recno )
{
	OBFHND	*o = hnd_of(h);
	INT	n = 0;
	ER	er;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o == NULL ) {
		return E_ID;
	}
	/* the native store keeps the type with the record */
	er = ts_ins_rec(o->od, -1, rt, sub, &n);
	if ( er != E_NOSPT ) {
		if ( er >= E_OK && p_recno != NULL ) {
			*p_recno = n;
		}
		return er;
	}
	er = ts_apd_rec(o->od, ( rt == OB_RT_TAD ) ? TSFS_REC_XTAD : TSFS_REC_BIN, &n);
	if ( er < E_OK ) {
		return er;
	}
	/* xmlTAD is what an undeclared .xtad is taken for; the rest is declared */
	if ( rt != OB_RT_TAD ) {
		er = decl_change(o, n, TRUE, rt, sub);
	}
	if ( p_recno != NULL ) {
		*p_recno = n;
	}
	return er;
}

LOCAL ER obf_rpl( INT h, INT recno, CONST void *buf, SZ size )
{
	OBFHND	*o = hnd_of(h);
	OBFVOL	*v;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o != NULL && buf != NULL && ( v = al_vol_of_hnd(o) ) != NULL ) {
		UB	*w = al_back(v, buf, size);
		ER	er;

		if ( w == NULL ) {
			return E_NOMEM;
		}
		er = ts_rpl_rec(o->od, recno, w, size);
		Kfree(w);
		return er;
	}
	return ( o != NULL ) ? ts_rpl_rec(o->od, recno, buf, size) : E_ID;
}

LOCAL ER obf_syn( INT h )
{
	OBFHND	*o = hnd_of(h);

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	return ( o != NULL ) ? ts_syn_obj(o->od) : E_ID;
}

LOCAL ER obf_trn( INT h, INT recno, UD size )
{
	OBFHND	*o = hnd_of(h);

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	return ( o != NULL ) ? ts_trn_rec(o->od, recno, size) : E_ID;
}

LOCAL ER obf_drc( INT h, INT recno )
{
	OBFHND	*o = hnd_of(h);
	ER	er;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o == NULL ) {
		return E_ID;
	}
	er = ts_del_rec(o->od, recno);
	if ( er >= E_OK ) {
		(void)decl_change(o, recno, FALSE, 0, 0);
	}
	return er;
}

LOCAL ER obf_lrc( INT h, T_OBREC *buf, INT n, INT *p_cnt )
{
	OBFHND	*o = hnd_of(h);
	T_RREC	*r;
	OBFDECL	*d = NULL;
	UB	*j = NULL;
	SZ	asz = 0;
	INT	got = 0, nd = 0, i, k, ex = -1;
	ER	er;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o == NULL || buf == NULL || p_cnt == NULL || n <= 0 ) {
		return E_PAR;
	}
	r = (T_RREC *)Kmalloc(sizeof(T_RREC) * (SZ)n);
	if ( r == NULL ) {
		return E_NOMEM;
	}
	er = ts_lst_rec(o->od, r, n, &got);
	if ( er >= E_OK && ( asz = od_meta_all(o, 0, &j, NULL) ) >= 0 ) {
		nd = decl_read(j, (INT)asz, NULL, 0x7FFFFFFF);
		d = (OBFDECL *)Kmalloc(sizeof(OBFDECL) * (SZ)( nd + 1 ));
		if ( d == NULL ) {
			er = E_NOMEM;
			nd = 0;
		} else {
			nd = decl_read(j, (INT)asz, d, nd);
		}
		ex = exec_rec(j, (INT)asz);
	}
	for ( i = 0; er >= E_OK && i < got; i++ ) {
		buf[i].recno = r[i].recno;
		buf[i].size = r[i].size;
		buf[i].sub = r[i].sub;
		buf[i].rt = r[i].rt;
		if ( r[i].rt == OB_RT_SYSDATA && r[i].recno == ex ) {
			buf[i].rt = OB_RT_PROG;
		}
		for ( k = 0; k < nd; k++ ) {
			if ( d[k].n == r[i].recno ) {
				buf[i].rt = d[k].rt;
				buf[i].sub = d[k].sub;
				break;
			}
		}
	}
	if ( er >= E_OK ) {
		*p_cnt = got;
	}
	Kfree(r);
	if ( j != NULL ) Kfree(j);
	if ( d != NULL ) Kfree(d);

	return er;
}

LOCAL ER obf_gat( INT h, UB *json, SZ size, SZ *p_asize )
{
	OBFHND	*o = hnd_of(h);
	OBFVOL	*v;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o != NULL && json != NULL && ( v = al_vol_of_hnd(o) ) != NULL ) {
		SZ	asz = 0;
		ER	er = ts_get_met(o->od, json, size, &asz);

		if ( er >= E_OK ) {
			(void)al_swap(v, json, ( asz < size ) ? asz : size, TRUE);
		}
		if ( p_asize != NULL ) *p_asize = asz;
		return er;
	}
	return ( o != NULL ) ? ts_get_met(o->od, json, size, p_asize) : E_ID;
}

LOCAL ER obf_sat( INT h, CONST UB *json, SZ size )
{
	OBFHND	*o = hnd_of(h);
	OBFVOL	*v;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o != NULL && json != NULL && ( v = al_vol_of_hnd(o) ) != NULL ) {
		UB	*w = al_back(v, json, size);
		ER	er;

		if ( w == NULL ) {
			return E_NOMEM;
		}
		er = ts_set_met(o->od, w, size);
		Kfree(w);
		return er;
	}
	return ( o != NULL ) ? ts_set_met(o->od, json, size) : E_ID;
}

LOCAL ER obf_gic( INT h, UB *buf, SZ size, SZ *p_asize )
{
	OBFHND	*o = hnd_of(h);

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	return ( o != NULL ) ? ts_get_ico(o->od, buf, size, p_asize) : E_ID;
}

LOCAL ER obf_sic( INT h, CONST UB *buf, SZ size )
{
	OBFHND	*o = hnd_of(h);

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	return ( o != NULL ) ? ts_set_ico(o->od, buf, size) : E_ID;
}

LOCAL ER obf_lnk( CONST TS_UUID *uuid, INT delta )
{
	TS_UUID	ru;
	ID	vol = obf_real(uuid, &ru);

	if ( vol <= 0 ) {
		return E_NOEXS;
	}
	return ( delta > 0 ) ? ts_lnk_obj(vol, &ru) : ts_unl_obj(vol, &ru);
}

/* Every volume's objects after 'from', put together in UUID order */
LOCAL ER obf_lst( CONST TS_UUID *from, TS_UUID *buf, INT n, INT *p_cnt )
{
	TS_UUID	*part, *merged;
	INT	cnt = 0, i;

	part = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	merged = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	if ( part == NULL || merged == NULL ) {
		if ( part != NULL ) Kfree(part);
		if ( merged != NULL ) Kfree(merged);
		return E_NOMEM;
	}
	for ( i = 0; i < __atomic_load_n(&obf_nvol, __ATOMIC_ACQUIRE); i++ ) {
		INT	got = 0, a, b, k;
		ID	vol;

		LOCK();
		vol = live_at(i);
		UNLOCK();
		if ( vol <= 0 || ts_lst_obj(vol, from, part, n, &got) < E_OK || got <= 0 ) {
			continue;
		}
		LOCK();
		for ( a = 0, b = 0; a < got; a++ ) {
			/* the domain is not listed, nor another machine's object of one of this system's UUIDs */
			if ( ts_uuid_cmp(&part[a], &ob_uuid_domain) != 0 && al_of(vent_at(i), &part[a]) == NULL ) {
				part[b++] = part[a];
			}
		}
		UNLOCK();
		got = b;
		for ( a = 0, b = 0, k = 0; k < n && ( a < cnt || b < got ); k++ ) {
			if ( b >= got || ( a < cnt && ts_uuid_cmp(&buf[a], &part[b]) <= 0 ) ) {
				merged[k] = buf[a++];
			} else {
				merged[k] = part[b++];
			}
		}
		knl_memcpy(buf, merged, (INT)( sizeof(TS_UUID) * (SZ)k ));
		cnt = k;
	}
	Kfree(part);
	Kfree(merged);
	*p_cnt = cnt;

	return E_OK;
}

/* ---------------------------------------------------------------- resources */

LOCAL ER obf_rres( INT h, CONST UB *name, D off, void *buf, SZ size, SZ *p_asize )
{
	OBFHND	*o = hnd_of(h);
	TS_UUID	u;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o == NULL || ts_od_uuid(o->od, &u) < E_OK ) return E_ID;
	return knl_tsfs_res_rea(o->vol, &u, name, off, buf, size, p_asize);
}

LOCAL ER obf_wres( INT h, CONST UB *name, CONST void *buf, SZ size )
{
	OBFHND	*o = hnd_of(h);
	TS_UUID	u;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o == NULL || ts_od_uuid(o->od, &u) < E_OK ) return E_ID;
	return knl_tsfs_res_wri(o->vol, &u, name, buf, size);
}

LOCAL ER obf_dres( INT h, CONST UB *name )
{
	OBFHND	*o = hnd_of(h);
	TS_UUID	u;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o == NULL || ts_od_uuid(o->od, &u) < E_OK ) return E_ID;
	return knl_tsfs_res_del(o->vol, &u, name);
}

LOCAL ER obf_lres( INT h, UB *buf, SZ size, INT *p_cnt )
{
	OBFHND	*o = hnd_of(h);
	TS_UUID	u;

	if ( o != NULL && hnd_gone(o) ) {
		return E_IO;			/* its volume went */
	}
	if ( o == NULL || ts_od_uuid(o->od, &u) < E_OK ) return E_ID;
	return knl_tsfs_res_lst(o->vol, &u, buf, size, p_cnt);
}

LOCAL CONST T_OBMGR obf_mgr = {
	OB_T_STORAGE, OB_S_FILE, "file",
	obf_find, obf_ref, obf_prot, obf_setprot, obf_create, obf_remove,
	obf_open, obf_close, obf_rea, obf_wri, obf_apd, obf_trn, obf_drc, obf_lrc,
	obf_gat, obf_sat, obf_lnk, obf_lst, NULL,
	obf_rres, obf_wres, obf_dres, obf_lres,
	NULL, NULL,
	obf_gic, obf_sic,
	obf_rpl, obf_syn,
	NULL,			/* nothing comes in to wait for (ob_rea_tmo) */
	NULL,
	obf_refnam
};

EXPORT ER knl_obfile_init( void )
{
	T_CMTX	cmtx;
	INT	no;

	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	obf_mtx = tk_cre_mtx(&cmtx);
	if ( obf_mtx <= 0 ) {
		return (ER)obf_mtx;
	}
	knl_obxref_init();
	no = knl_ob_regist(&obf_mgr);

	return ( no > 0 ) ? E_OK : (ER)no;
}
