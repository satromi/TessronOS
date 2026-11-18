/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obfsck.c
 *	The file manager's side of the disk check (ts/tsfsck.h,
 *	docs/tessronos/07-filesystem.md 7.5)
 *
 *	Which native volumes the machine has: every partition of every disk
 *	the device management knows whose superblock says it is one, whether
 *	its objects are in use or not. A volume in use is checked in place,
 *	its writers kept away for the time; any other is opened for the
 *	check alone, and kept from being attached meanwhile.
 *
 *	The volume the system runs on is in use from start to stop: its
 *	fonts, its programs and the desktop's own windows are read from it
 *	all the time. It is looked at in place, but repaired only at the
 *	next start: the request is a line in a file of the boot volume, and
 *	the start carries it out before the system volume is attached, so
 *	that nothing has an object of it open while the repair writes. What
 *	that repair found and did is kept for the accessory to show.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/fs.h>
#include <ts/tsfs.h>
#include <ts/tsfsblk.h>
#include <ts/tsfsck.h>
#include <ts/crc32c.h>
#include "obj.h"

#define CHK_BOOT_EV	64		/* events the list of the repair at start is first taken for */
#define SB_GEN		16
#define SB_LABEL	40
#define SB_TOTAL	112
#define SB_CRC		4092

LOCAL ID	chk_mtx = 0;
LOCAL UB	chk_busy[TSFSCK_DEVNM_MAX];	/* the device being checked, "" for none */
LOCAL INT	chk_held = 0;			/* volumes being attached, detached or made larger */
LOCAL BOOL	boot_given = FALSE;		/* the repair at start given to a program */

LOCAL BOOL		boot_done = FALSE;
LOCAL UB		boot_dev[TSFSCK_DEVNM_MAX];
LOCAL T_TSFSCK		boot_res;
LOCAL T_TSFSCKEV	*boot_ev = NULL;
LOCAL INT		boot_nev = 0;
LOCAL INT		boot_cap = 0;		/* events boot_ev has room for */
LOCAL INT		boot_lost = 0;		/* events not kept for want of memory */

/* ---------------------------------------------------------------- words */

LOCAL BOOL same_name( CONST UB *a, CONST char *b )
{
	INT	i;

	for ( i = 0; a[i] != 0 && b[i] != 0; i++ ) {
		if ( a[i] != (UB)b[i] ) return FALSE;
	}
	return (BOOL)( a[i] == 0 && b[i] == 0 );
}

LOCAL void name_copy( UB *to, CONST char *from )
{
	INT	i;

	for ( i = 0; from[i] != 0 && i < TSFSCK_DEVNM_MAX - 1; i++ ) {
		to[i] = (UB)from[i];
	}
	to[i] = 0;
}

LOCAL UW rd32( CONST UB *p )
{
	return (UW)p[0] | ((UW)p[1] << 8) | ((UW)p[2] << 16) | ((UW)p[3] << 24);
}

LOCAL UD rd64( CONST UB *p )
{
	return (UD)rd32(p) | ((UD)rd32(p + 4) << 32);
}

LOCAL BOOL is_system( CONST char *devnm )
{
#ifdef CNF_OB_SYSVOL
	return same_name((CONST UB *)devnm, CNF_OB_SYSVOL);
#else
	(void)devnm;
	return FALSE;
#endif
}

LOCAL ER chk_init( void )
{
	T_CMTX	cm;

	if ( chk_mtx > 0 ) {
		return E_OK;
	}
	cm.exinf = NULL;
	cm.mtxatr = TA_TFIFO;
	chk_mtx = tk_cre_mtx(&cm);
	return ( chk_mtx > 0 ) ? E_OK : (ER)chk_mtx;
}

/* ---------------------------------------------------------------- the volumes there are */

/*
 * The superblock of a device, when it is a native volume: the copy that
 * passes its check, the newer of the two when both do.
 */
LOCAL BOOL read_sb( CONST char *devnm, T_TSFSCKDEV *d )
{
	UB	*buf;
	UD	gen = 0;
	SZ	asz = 0;
	ID	dd;
	INT	c, i;
	BOOL	any = FALSE;

	buf = (UB *)Kmalloc(TSFSBLK_BLOCK_SIZE);
	if ( buf == NULL ) {
		return FALSE;
	}
	dd = tk_opn_dev((UB *)devnm, TD_READ);
	for ( c = 0; dd > 0 && c < 2; c++ ) {
		if ( tk_srea_dev_d(dd, (D)( c * TSFSBLK_SECT_PER_BLK ), buf, TSFSBLK_BLOCK_SIZE, &asz) < E_OK
		  || asz < TSFSBLK_BLOCK_SIZE ) {
			continue;
		}
		for ( i = 0; i < 8 && buf[i] == (UB)TSFSBLK_MAGIC[i]; i++ ) ;
		if ( i < 8 || rd32(buf + SB_CRC) != ts_crc32c(buf, SB_CRC) ) {
			continue;
		}
		if ( any && rd64(buf + SB_GEN) < gen ) {
			continue;
		}
		gen = rd64(buf + SB_GEN);
		any = TRUE;
		if ( d != NULL ) {
			SZ	n;

			/* a copy a system before cut in the middle of a letter is cut back to a whole one */
			for ( n = 0; n < TSFSBLK_LABEL_MAX - 1 && buf[SB_LABEL + n] != 0; n++ ) ;
			n = ts_label_whole(buf + SB_LABEL, n);
			knl_memset(d->label, 0, sizeof(d->label));
			knl_memcpy(d->label, buf + SB_LABEL, n);
			d->blocks = rd64(buf + SB_TOTAL);
		}
	}
	if ( dd > 0 ) {
		tk_cls_dev(dd, 0);
	}
	Kfree(buf);
	return any;
}

/*
 * The label of a volume listed: of one attached, the name of its top
 * object, cut at a whole letter, with TSFSCK_F_LABELCUT when it is
 * longer. One not attached keeps the copy read_sb took; its objects are
 * not read for a list, so a copy that is full may be cut.
 */
LOCAL void label_of( T_TSFSCKDEV *d )
{
	SZ	asz = 0, n;

	if ( d->attached
	  && knl_obfile_vol_label((CONST char *)d->devnm, d->label, sizeof(d->label), &asz) >= E_OK ) {
		if ( asz > (SZ)sizeof(d->label) ) {
			d->flags |= TSFSCK_F_LABELCUT;
		}
		return;
	}
	for ( n = 0; n < (SZ)sizeof(d->label) && d->label[n] != 0; n++ ) ;
	if ( n > (SZ)sizeof(d->label) - 5 ) {
		d->flags |= TSFSCK_F_LABELCUT;	/* within a letter of full */
	}
}

/* One device or unit looked at, and kept in the list when it is a native volume */
LOCAL INT list_one( CONST UB *name, T_TSFSCKDEV *buf, INT n, INT max )
{
	T_TSFSCKDEV	d;

	knl_memset(&d, 0, sizeof(d));
	name_copy(d.devnm, (CONST char *)name);
	if ( !read_sb((CONST char *)d.devnm, &d) ) {
		return n;
	}
	d.attached = (BOOL)( knl_obfile_vol_path((CONST char *)d.devnm) > 0 );
	d.system = is_system((CONST char *)d.devnm);
	label_of(&d);
	if ( n < max && buf != NULL ) {
		buf[n] = d;
	}
	return n + 1;
}

EXPORT INT ob_chk_list( T_TSFSCKDEV *buf, INT max )
{
	T_LDEV	ld[8];
	UB	name[TSFSCK_DEVNM_MAX];
	INT	start = 0, n = 0, k, s, m, got, cnt;

	for ( ;; ) {
		cnt = tk_lst_dev(ld, start, 8);
		if ( cnt <= 0 ) {
			break;
		}
		got = ( cnt > 8 ) ? 8 : cnt;
		for ( k = 0; k < got; k++ ) {
			if ( ( ld[k].devatr & TD_DEVKIND & 0xF0U ) != TDK_DISK ) {
				continue;		/* not a disk */
			}
			for ( s = -1; s < ld[k].nsub; s++ ) {
				for ( m = 0; m < L_DEVNM && ld[k].devnm[m] != 0 && m < TSFSCK_DEVNM_MAX - 3; m++ ) {
					name[m] = ld[k].devnm[m];
				}
				if ( s >= 0 ) {
					if ( s >= 100 ) name[m++] = (UB)( '0' + s / 100 );
					if ( s >= 10 ) name[m++] = (UB)( '0' + s / 10 % 10 );
					name[m++] = (UB)( '0' + s % 10 );
				}
				name[m] = 0;
				n = list_one(name, buf, n, max);
			}
		}
		start += got;
		if ( cnt <= 8 ) {
			break;
		}
	}
	return n;
}

/*
 * A listed volume's label, whole: of one attached, the name of its top
 * object (knl_obfile_vol_label); of one not attached, the same read with
 * the volume opened for it alone, as a check opens it. While a check or
 * a change of a volume is going on, or when it does not open, the copy
 * in its superblock is what there is.
 */
EXPORT ER ob_chk_label( CONST char *devnm, UB *buf, SZ size, SZ *p_asize )
{
	T_TSFSCKDEV	d;
	ID		vol;
	BOOL		mine = FALSE;
	ER		er;

	if ( devnm == NULL || size < 0 || ( size > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	if ( knl_obfile_vol_path(devnm) > 0
	  && knl_obfile_vol_label(devnm, buf, size, p_asize) >= E_OK ) {
		return E_OK;
	}
	knl_memset(&d, 0, sizeof(d));
	if ( !read_sb(devnm, &d) ) {
		return E_NOEXS;
	}
	if ( chk_init() >= E_OK ) {
		tk_loc_mtx(chk_mtx, TMO_FEVR);
		if ( chk_busy[0] == 0 && chk_held == 0 ) {
			name_copy(chk_busy, devnm);
			mine = TRUE;
		}
		tk_unl_mtx(chk_mtx);
	}
	if ( mine ) {
		er = E_NOEXS;
		if ( knl_obfile_vol_path(devnm) <= 0 ) {
			vol = ts_opn_vol(devnm, TSFS_STORE_BLK);
			if ( vol > 0 ) {
				er = knl_tsfs_vol_label(vol, buf, size, p_asize);
				(void)ts_cls_vol(vol);
			}
		} else {
			er = knl_obfile_vol_label(devnm, buf, size, p_asize);	/* attached meanwhile */
		}
		tk_loc_mtx(chk_mtx, TMO_FEVR);
		chk_busy[0] = 0;
		tk_unl_mtx(chk_mtx);
		if ( er >= E_OK ) {
			return er;
		}
	}
	return knl_ob_name_put(d.label, -1, buf, size, p_asize);
}

/* ---------------------------------------------------------------- checking one */

EXPORT BOOL knl_ob_chk_busy( CONST char *devnm )
{
	BOOL	busy;

	if ( chk_mtx <= 0 || devnm == NULL ) {
		return FALSE;
	}
	tk_loc_mtx(chk_mtx, TMO_FEVR);
	busy = (BOOL)( chk_busy[0] != 0 && same_name(chk_busy, devnm) );
	tk_unl_mtx(chk_mtx);
	return busy;
}

/*
 * A change of a volume (attaching, detaching, making it larger) and its
 * check kept apart: the check opens the volume or works on the one
 * attached, and neither may be closed or have its size changed under it.
 */
EXPORT BOOL knl_ob_chk_hold( CONST char *devnm, BOOL on )
{
	BOOL	ok = TRUE;

	if ( devnm == NULL || chk_init() < E_OK ) {
		return FALSE;
	}
	tk_loc_mtx(chk_mtx, TMO_FEVR);
	if ( !on ) {
		if ( chk_held > 0 ) chk_held--;
	} else if ( chk_busy[0] != 0 && same_name(chk_busy, devnm) ) {
		ok = FALSE;
	} else {
		chk_held++;
	}
	tk_unl_mtx(chk_mtx);
	return ok;
}

EXPORT BOOL knl_ob_chk_boot_once( void )
{
	BOOL	first;

	if ( chk_init() < E_OK ) {
		return FALSE;
	}
	tk_loc_mtx(chk_mtx, TMO_FEVR);
	first = (BOOL)!boot_given;
	boot_given = TRUE;
	tk_unl_mtx(chk_mtx);
	return first;
}

/*
 * The record of links from other volumes, checked after the volume
 * itself (design 7.5, 8.3), and put right with the repair. Not on a
 * volume the repair found damage on it does not put right.
 */
LOCAL void xref_check( ID vol, UINT mode, T_TSFSCK *res, FP_TSFSCK fn, void *ctx )
{
	BOOL	fix = (BOOL)( ( mode & TSFSCK_REPAIR ) != 0 && !res->refused );
	INT	k = TSFSCK_K_XREF, before = res->found[k];
	ER	er;

	er = knl_obxref_check(vol, fix, res, fn, ctx);
	if ( er < E_OK ) {
		tm_printf((UB *)"ob: links from other volumes not checked (%d)\n", (INT)er);
		return;
	}
	if ( !fix ) {
		res->remaining += res->found[k] - before;
		res->left[k] += res->found[k] - before;
	}
}

/*
 * A volume checked: in place when its objects are in use, else opened
 * for the check alone. The system's own volume is not repaired here.
 */
EXPORT ER ob_chk_run( CONST char *devnm, UINT mode, T_TSFSCK *res, FP_TSFSCK fn, void *ctx )
{
	ID	vol;
	ER	er;
	BOOL	detached = FALSE, inuse = FALSE;

	if ( devnm == NULL || res == NULL ) {
		return E_PAR;
	}
	mode &= ~(UINT)TSFSCK_ALONE;
	er = chk_init();
	if ( er < E_OK ) {
		return er;
	}
	if ( !read_sb(devnm, NULL) ) {
		return E_NOEXS;
	}
	tk_loc_mtx(chk_mtx, TMO_FEVR);
	if ( chk_busy[0] != 0 || chk_held > 0 ) {
		tk_unl_mtx(chk_mtx);
		return E_BUSY;			/* one at a time, and not while a volume is changing */
	}
	name_copy(chk_busy, devnm);
	tk_unl_mtx(chk_mtx);

	vol = knl_obfile_vol_path(devnm);
	if ( vol > 0 && ( mode & TSFSCK_REPAIR ) != 0 && !is_system(devnm) ) {
		/* a data volume in use: taken off for the repair, unless an object of it is open */
		if ( knl_obfile_detach(devnm) >= E_OK ) {
			detached = TRUE;
			vol = 0;
			tm_printf((UB *)"ob: %s taken off to be repaired\n", devnm);
		} else {
			inuse = TRUE;
		}
	}
	if ( vol > 0 ) {
		er = ( ( mode & TSFSCK_REPAIR ) != 0 && is_system(devnm) )
		   ? E_BUSY : ts_chk_vol(vol, mode, res, fn, ctx);
		if ( er >= E_OK ) {
			xref_check(vol, mode, res, fn, ctx);
		}
	} else {
		vol = ts_opn_vol(devnm, TSFS_STORE_BLK);
		if ( vol > 0 ) {
			er = ts_chk_vol(vol, mode | TSFSCK_ALONE, res, fn, ctx);
			if ( er >= E_OK ) {
				xref_check(vol, mode, res, fn, ctx);
			}
			(void)ts_cls_vol(vol);
		} else {
			er = ( vol < 0 ) ? (ER)vol : E_SYS;
		}
	}

	tk_loc_mtx(chk_mtx, TMO_FEVR);
	chk_busy[0] = 0;
	tk_unl_mtx(chk_mtx);

	/* put back as it was attached: a native volume */
	if ( detached ) {
		ER	ea = knl_obfile_attach(devnm, TSFS_STORE_BLK);

		tm_printf((UB *)"ob: %s put back (%d)\n", devnm, (INT)ea);
		if ( er >= E_OK && ea < E_OK ) {
			er = ea;
		}
	}
	res->detached = detached;
	res->inuse = inuse;
	return er;
}

/* ---------------------------------------------------------------- the system volume, at the next start */

LOCAL INT num_text( UB *b, INT at, UINT v )
{
	UB	t[12];
	INT	n = 0;

	do {
		t[n++] = (UB)( '0' + v % 10 );
		v /= 10;
	} while ( v > 0 );
	while ( n > 0 ) {
		b[at++] = t[--n];
	}
	return at;
}

/* "<device> <mode>" into the request file */
EXPORT ER ob_chk_request( CONST char *devnm, UINT mode )
{
	UB	line[TSFSCK_DEVNM_MAX + 16];
	INT	fd, n = 0, i;
	ER	er;

	if ( devnm == NULL || !read_sb(devnm, NULL) ) {
		return E_NOEXS;
	}
	for ( i = 0; devnm[i] != 0 && i < TSFSCK_DEVNM_MAX - 1; i++ ) {
		line[n++] = (UB)devnm[i];
	}
	line[n++] = ' ';
	n = num_text(line, n, mode | TSFSCK_REPAIR);
	line[n++] = '\n';
	fd = fs_open(TSFSCK_REQUEST, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd < 0 ) {
		return (ER)fd;
	}
	er = ( fs_write(fd, line, (SZ)n) == n ) ? E_OK : E_IO;
	(void)fs_close(fd);
	if ( er >= E_OK ) {
		(void)fs_sync();
	}
	return er;
}

/*
 * Every event of the repair at start, in a list taken twice as large
 * each time it is full. One the memory runs out for is counted in
 * boot_lost and told on the console when the repair ends.
 */
LOCAL void keep_ev( void *ctx, CONST T_TSFSCKEV *ev )
{
	(void)ctx;
	if ( boot_nev >= boot_cap ) {
		INT		cap = ( boot_cap > 0 ) ? boot_cap * 2 : CHK_BOOT_EV;
		T_TSFSCKEV	*b = (T_TSFSCKEV *)Kmalloc(sizeof(T_TSFSCKEV) * (SZ)cap);

		if ( b == NULL ) {
			boot_lost++;
			return;
		}
		if ( boot_ev != NULL ) {
			knl_memcpy(b, boot_ev, (INT)( sizeof(T_TSFSCKEV) * (SZ)boot_nev ));
			Kfree(boot_ev);
		}
		boot_ev = b;
		boot_cap = cap;
	}
	boot_ev[boot_nev++] = *ev;
}

/*
 * At start, before the system volume is attached: the request read and
 * taken away first, so that a repair that does not finish is not tried
 * at every start, and carried out on the volume opened for it alone.
 */
EXPORT ER knl_ob_chk_boot( CONST char *sysvol )
{
	UB	line[TSFSCK_DEVNM_MAX + 16];
	UINT	mode = 0;
	INT	fd, n, i, k = 0;
	ID	vol;
	ER	er;

	(void)chk_init();
	fd = fs_open(TSFSCK_REQUEST, O_RDONLY);
	if ( fd < 0 ) {
		return E_OK;			/* nothing asked */
	}
	n = fs_read(fd, line, sizeof(line) - 1);
	(void)fs_close(fd);
	(void)fs_unlink(TSFSCK_REQUEST);
	(void)fs_sync();
	if ( n <= 0 ) {
		return E_OK;
	}
	line[n] = 0;
	for ( i = 0; i < n && line[i] != ' ' && i < TSFSCK_DEVNM_MAX - 1; i++ ) {
		boot_dev[k++] = line[i];
	}
	boot_dev[k] = 0;
	for ( i++; i < n && line[i] >= '0' && line[i] <= '9'; i++ ) {
		mode = mode * 10 + (UINT)( line[i] - '0' );
	}
	if ( sysvol != NULL && !same_name(boot_dev, sysvol) ) {
		tm_printf((UB *)"ob: the disk repair asked for %s, which is not the system volume\n", boot_dev);
		return E_PAR;
	}
	tm_printf((UB *)"ob: %s is repaired before it is used (mode 0x%x)\n", boot_dev, mode);
	if ( boot_ev != NULL ) {
		Kfree(boot_ev);
		boot_ev = NULL;
	}
	boot_nev = 0;
	boot_cap = 0;
	boot_lost = 0;
	vol = ts_opn_vol((CONST char *)boot_dev, TSFS_STORE_BLK);
	if ( vol <= 0 ) {
		return ( vol < 0 ) ? (ER)vol : E_SYS;
	}
	er = ts_chk_vol(vol, mode | TSFSCK_REPAIR | TSFSCK_ALONE, &boot_res, keep_ev, NULL);
	(void)ts_cls_vol(vol);
	boot_done = (BOOL)( er >= E_OK );
	tm_printf((UB *)"ob: the repair of %s: %d found, %d left (%d)\n", boot_dev,
		  boot_res.problems, boot_res.remaining, (INT)er);
	if ( boot_lost > 0 ) {
		tm_printf((UB *)"ob: %d events of the repair of %s were not kept: no memory\n", boot_lost, boot_dev);
	}
	return er;
}

EXPORT void knl_ob_chk_boot_forget( void )
{
	if ( chk_init() < E_OK ) {
		return;
	}
	tk_loc_mtx(chk_mtx, TMO_FEVR);
	boot_done = FALSE;
	boot_given = FALSE;
	if ( boot_ev != NULL ) {
		Kfree(boot_ev);
		boot_ev = NULL;
	}
	boot_nev = 0;
	boot_cap = 0;
	boot_lost = 0;
	boot_dev[0] = 0;
	tk_unl_mtx(chk_mtx);
}

/* What the repair at start found and did: how many events were kept, 0 when there was none */
EXPORT INT ob_chk_boot_result( UB *devnm, INT nmax, T_TSFSCK *res, T_TSFSCKEV *ev, INT max )
{
	INT	i;

	if ( !boot_done ) {
		return 0;
	}
	if ( devnm != NULL && nmax > 0 ) {
		for ( i = 0; boot_dev[i] != 0 && i < nmax - 1; i++ ) devnm[i] = boot_dev[i];
		devnm[i] = 0;
	}
	if ( res != NULL ) {
		*res = boot_res;
	}
	for ( i = 0; ev != NULL && i < boot_nev && i < max; i++ ) {
		ev[i] = boot_ev[i];
	}
	return boot_nev;
}
