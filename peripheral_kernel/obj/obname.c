/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obname.c
 *	The name manager: every basic operation on a real object comes
 *	here first and goes on to the manager that serves the object
 *	(design 18.3, 18.5, 18.9)
 *
 *	It keeps the managers by number, finds which one an object belongs
 *	to, and keeps the keys: a key names an open object, carries the
 *	number of its manager, the process that holds it and what it may
 *	be used for. Whether an operation is allowed is decided here, when
 *	the object is opened, against the protection its manager keeps and
 *	the credentials of the process; the key then carries the answer.
 *
 *	Keys of one open object share a handle of the manager, so a key
 *	handed on with fewer operations (ob_dup_key) does not open the
 *	object a second time. The handle is closed when the last key of it
 *	is, and not while an operation on it is still running.
 *
 *	It also keeps the requests for notices (ob_ntf_evt): which object
 *	and record is watched, for which events, and the port a notice goes
 *	to. A change made through a key here is noticed here; what happens
 *	inside a manager (a window pressed, a process ending) the manager
 *	reports with knl_ob_post.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/proc.h>
#include <ts/tsfs.h>
#include <ts/dt.h>
#include "obj.h"

/*
 * The tables are sized for as many processes as the system may have,
 * each with its window, its channel and a few objects open: keys and
 * requests for notices are small and kept in place; a handle carries
 * the protection it was opened with and is made only when a slot is
 * first used.
 */
#define OB_KEY_MAX	65536
#define OB_HND_MAX	16384
#define OB_NTF_MAX	16384
#define OB_NTF_HASH	1024		/* chains of requests, by the object watched */
#define OB_GRANT_MAX	1024
#define KEY_MGR_SHIFT	24

typedef struct {
	BOOL	used;
	INT	mgr;			/* index into ob_mgr */
	INT	h;			/* the manager's handle */
	INT	refs;			/* keys, and operations under way */
	TS_UUID	uuid;
	T_OBPRT	prt;			/* the protection as it was opened */
} OBHND;

typedef struct {
	BOOL	used;
	INT	hnd;
	ID	pid;			/* who holds it; 0 the kernel */
	UINT	ops;			/* OB_OP_* it may be used for */
	BOOL	nowait;
} OBKEY;

/*
 * Credentials as a process holds them: one set for each different user
 * and groups there have been, kept for good once made, and a pointer to
 * one for each process by its number. A child takes its parent's
 * pointer, so however many processes there are the sets are as many as
 * the ways people logged in; and a set a caller holds a pointer to is
 * never freed under it.
 */
typedef struct CRDSET {
	struct CRDSET	*next;
	T_OBCRD		crd;
} CRDSET;

LOCAL CONST T_OBMGR	*ob_mgr[OB_MGR_MAX];
LOCAL INT		ob_nmgr = 0;
LOCAL OBHND		*ob_hndp[OB_HND_MAX];	/* NULL until the slot is first used */
LOCAL INT		ob_hnd_next = 0;	/* where to look for a free slot first */
LOCAL INT		ob_key_next = 0;
LOCAL OBKEY		ob_key[OB_KEY_MAX];
LOCAL CRDSET		*ob_crdsets = NULL;
LOCAL CONST T_OBCRD	*ob_crd_pid[CNF_PID_MAX];
LOCAL ID		ob_mtx = 0;

/* A request for notices */
typedef struct {
	BOOL	used;
	TS_UUID	uuid;			/* the object watched */
	INT	recno;			/* OB_REC_ANY for all of it */
	UINT	events;
	UINT	id;
	UINT	flags;
	TS_UUID	port;
	ID	key;			/* closing it ends the request */
	ID	pid;
	UINT	lost;			/* not delivered since the last that was */
	INT	next;			/* the next in its chain, as index + 1; 0 at the end */
} OBNTF;

LOCAL OBNTF		ob_ntf[OB_NTF_MAX];
LOCAL INT		ob_ntf_chain[OB_NTF_HASH];	/* first of each chain, as index + 1 */
LOCAL INT		ob_ntf_next = 0;
LOCAL INT		ob_nntf = 0;	/* requests in use: none, nothing to look for */

LOCAL INT ntf_hash( CONST TS_UUID *u )
{
	UD	x = u->d.hi ^ u->d.lo;

	x ^= x >> 29;
	x ^= x >> 17;
	return (INT)( x & ( OB_NTF_HASH - 1 ) );
}

/* A request in slot i put in use, on the chain of what it watches. Under the lock. */
LOCAL void ntf_on( INT i )
{
	INT	c = ntf_hash(&ob_ntf[i].uuid);

	ob_ntf[i].used = TRUE;
	ob_ntf[i].next = ob_ntf_chain[c];
	ob_ntf_chain[c] = i + 1;
	ob_nntf++;
}

/* The request in slot i taken out of use. Under the lock. */
LOCAL void ntf_off( INT i )
{
	INT	*l = &ob_ntf_chain[ntf_hash(&ob_ntf[i].uuid)];

	while ( *l != 0 && *l != i + 1 ) {
		l = &ob_ntf[*l - 1].next;
	}
	if ( *l == i + 1 ) {
		*l = ob_ntf[i].next;
	}
	ob_ntf[i].used = FALSE;
	ob_nntf--;
}

/* What a process was granted on an object beyond its credentials (a drop) */
typedef struct {
	BOOL	used;
	ID	pid;
	TS_UUID	uuid;
	UINT	ops;
	UINT	age;			/* when it was given, to find the oldest */
} OBGRANT;

LOCAL OBGRANT		ob_grant[OB_GRANT_MAX];
LOCAL UINT		ob_grant_age = 0;

EXPORT CONST TS_UUID ob_user_system = OB_USER_SYSTEM_INIT;
EXPORT CONST TS_UUID ob_group_admin = OB_GROUP_ADMIN_INIT;
EXPORT CONST TS_UUID ob_uuid_clock  = OB_UUID_CLOCK_INIT;
EXPORT CONST TS_UUID ob_uuid_devbox = OB_UUID_DEVBOX_INIT;
EXPORT CONST TS_UUID ob_uuid_domain = OB_UUID_DOMAIN_INIT;
EXPORT CONST TS_UUID ob_uuid_devlist = OB_UUID_DEVLIST_INIT;
EXPORT CONST TS_UUID ob_uuid_system = OB_UUID_SYSTEM_INIT;
EXPORT CONST TS_UUID ob_uuid_random = OB_UUID_RANDOM_INIT;
EXPORT CONST TS_UUID ob_uuid_input = OB_UUID_INPUT_INIT;
EXPORT CONST TS_UUID ob_uuid_display = OB_UUID_DISPLAY_INIT;

/* The kernel's own tasks, and init: the system, an administrator */
EXPORT CONST T_OBCRD knl_ob_crd_system = {
	OB_USER_SYSTEM_INIT, 1, { OB_GROUP_ADMIN_INIT }
};

/* A process whose credentials were lost: nobody, in no group */
LOCAL CONST T_OBCRD ob_crd_nobody;

#define LOCK()		tk_loc_mtx(ob_mtx, TMO_FEVR)
#define UNLOCK()	tk_unl_mtx(ob_mtx)

LOCAL BOOL same_uuid( CONST TS_UUID *a, CONST TS_UUID *b )
{
	return (BOOL)( a->d.hi == b->d.hi && a->d.lo == b->d.lo );
}

/* ---------------------------------------------------------------- managers */

EXPORT INT knl_ob_regist( CONST T_OBMGR *mgr )
{
	if ( mgr == NULL || ob_nmgr >= OB_MGR_MAX ) {
		return E_LIMIT;
	}
	ob_mgr[ob_nmgr] = mgr;

	return ++ob_nmgr;			/* its number, from 1 */
}

/* Which manager an object is: asked of each in the order they came */
LOCAL INT mgr_index( CONST TS_UUID *uuid )
{
	INT	i;

	if ( uuid == NULL || knl_ob_uuid_zero(uuid) ) {
		return -1;
	}
	for ( i = 0; i < ob_nmgr; i++ ) {
		if ( ob_mgr[i]->find != NULL && ob_mgr[i]->find(uuid) >= E_OK ) {
			return i;
		}
	}
	return -1;
}

EXPORT CONST T_OBMGR *knl_ob_mgr_of( CONST TS_UUID *uuid )
{
	INT	i = mgr_index(uuid);

	return ( i >= 0 ) ? ob_mgr[i] : NULL;
}

/* The protection an object has; one whose manager keeps none lets all through */
LOCAL void prot_of( CONST T_OBMGR *m, CONST TS_UUID *uuid, T_OBPRT *prt )
{
	if ( m->prot == NULL || m->prot(uuid, prt) < E_OK ) {
		knl_ob_prt_legacy(prt, TRUE, TRUE, TRUE);
	}
}

/*
 * What a subject may do to an object: its own protection, looked at
 * through the domain of the volume it is on first (18.9). The domain
 * bounds everything but the owner's right to change the protection.
 */
LOCAL UINT permit_on( CONST T_OBMGR *m, CONST TS_UUID *uuid, CONST T_OBCRD *crd,
		      CONST T_OBPRT *prt )
{
	UINT	ops = knl_ob_permit(crd, prt, -1);
	T_OBPRT	*dom;

	if ( m->type != OB_T_STORAGE || m->sub != OB_S_FILE || ops == 0 ) {
		return ops;
	}
	dom = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
	if ( dom != NULL ) {
		if ( knl_obfile_domain(uuid, dom) >= E_OK ) {
			ops &= knl_ob_permit(crd, dom, -1) | OB_OP_PROT;
		}
		Kfree(dom);
	}
	return ops;
}

EXPORT UINT knl_ob_permit_on( CONST TS_UUID *uuid, CONST T_OBCRD *crd )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);
	T_OBPRT		*prt;
	UINT		ops = 0;

	if ( m == NULL || crd == NULL ) {
		return 0;
	}
	prt = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
	if ( prt != NULL ) {
		prot_of(m, uuid, prt);
		ops = permit_on(m, uuid, crd, prt);
		Kfree(prt);
	}
	return ops;
}

/* ---------------------------------------------------------------- credentials */

EXPORT BOOL knl_ob_is_admin( CONST T_OBCRD *crd )
{
	INT	g;

	if ( crd == &knl_ob_crd_system || same_uuid(&crd->user, &ob_user_system) ) {
		return TRUE;
	}
	for ( g = 0; g < crd->ngrp && g < CNF_OB_MAX_GROUPS; g++ ) {
		if ( same_uuid(&crd->grp[g], &ob_group_admin) ) {
			return TRUE;
		}
	}
	return FALSE;
}

EXPORT CONST T_OBCRD *knl_ob_crd_of( ID pid )
{
	CONST T_OBCRD	*c;

	if ( pid <= 0 ) {
		return &knl_ob_crd_system;
	}
	c = ( pid < CNF_PID_MAX ) ? ob_crd_pid[pid] : NULL;

	return ( c != NULL ) ? c : &ob_crd_nobody;
}

LOCAL BOOL same_crd( CONST T_OBCRD *a, CONST T_OBCRD *b )
{
	INT	g;

	if ( !same_uuid(&a->user, &b->user) || a->ngrp != b->ngrp ) {
		return FALSE;
	}
	for ( g = 0; g < a->ngrp && g < CNF_OB_MAX_GROUPS; g++ ) {
		if ( !same_uuid(&a->grp[g], &b->grp[g]) ) {
			return FALSE;
		}
	}
	return TRUE;
}

/* The kept set equal to crd, made when there is none yet; NULL for no memory. Under the lock. */
LOCAL CONST T_OBCRD *crd_intern( CONST T_OBCRD *crd )
{
	CRDSET	*c;

	if ( same_crd(crd, &knl_ob_crd_system) ) {
		return &knl_ob_crd_system;
	}
	for ( c = ob_crdsets; c != NULL; c = c->next ) {
		if ( same_crd(crd, &c->crd) ) {
			return &c->crd;
		}
	}
	c = (CRDSET *)Kmalloc(sizeof(CRDSET));
	if ( c == NULL ) {
		return NULL;
	}
	c->crd = *crd;
	c->next = ob_crdsets;
	ob_crdsets = c;

	return &c->crd;
}

/* A new process acts as its parent did */
EXPORT void knl_ob_prc_start( ID pid, ID ppid )
{
	if ( ob_mtx <= 0 || pid <= 0 || pid >= CNF_PID_MAX ) {
		return;
	}
	LOCK();
	ob_crd_pid[pid] = knl_ob_crd_of(ppid);
	UNLOCK();
}

LOCAL void hnd_release( INT hi );
LOCAL void ntf_drop( ID key, ID pid );
LOCAL void grant_drop( ID pid );

/* A process is ending: its keys are closed, and whoever watched it is told */
EXPORT void knl_ob_prc_end( ID pid )
{
	TS_UUID	me;
	INT	i;

	if ( ob_mtx <= 0 || pid <= 0 ) {
		return;
	}
	if ( knl_prc_uuid(pid, &me, NULL) >= E_OK ) {
		knl_ob_post(&me, -1, OB_E_EXIT, NULL);
	}
	ntf_drop(0, pid);
	grant_drop(pid);
	for ( i = 0; i < OB_KEY_MAX; ) {
		INT	his[16], n = 0, j;

		LOCK();
		for ( ; i < OB_KEY_MAX && n < 16; i++ ) {
			if ( ob_key[i].used && ob_key[i].pid == pid ) {
				his[n++] = ob_key[i].hnd;
				ob_key[i].used = FALSE;
			}
		}
		UNLOCK();
		for ( j = 0; j < n; j++ ) {
			hnd_release(his[j]);
		}
	}
}

EXPORT void knl_ob_prc_gone( ID pid )
{
	if ( ob_mtx <= 0 || pid <= 0 || pid >= CNF_PID_MAX ) {
		return;
	}
	LOCK();
	ob_crd_pid[pid] = NULL;
	UNLOCK();
}

EXPORT ER ob_get_crd( T_OBCRD *pk_crd )
{
	ID	pid = ts_get_pid();

	if ( pk_crd == NULL ) {
		return E_PAR;
	}
	LOCK();
	*pk_crd = *knl_ob_crd_of(pid);
	UNLOCK();

	return E_OK;
}

/* The credentials of a process replaced (the user logged in) */
EXPORT ER knl_ob_set_crd( ID pid, CONST T_OBCRD *crd )
{
	CONST T_OBCRD	*c;
	ER		er = E_OK;

	if ( pid <= 0 ) {
		return E_ID;			/* the kernel is always the system */
	}
	if ( pid >= CNF_PID_MAX ) {
		return E_NOEXS;
	}
	LOCK();
	if ( ob_crd_pid[pid] == NULL ) {
		er = E_NOEXS;
	} else if ( ( c = crd_intern(crd) ) == NULL ) {
		er = E_NOMEM;
	} else {
		ob_crd_pid[pid] = c;
	}
	UNLOCK();

	return er;
}

/* ---------------------------------------------------------------- keys */

LOCAL INT hnd_new( INT mgr, INT h, CONST TS_UUID *uuid, CONST T_OBPRT *prt )
{
	INT	i;

	OBHND	*o;
	INT	n;

	for ( n = 0, i = ob_hnd_next; n < OB_HND_MAX; n++, i++ ) {
		if ( i >= OB_HND_MAX ) {
			i = 0;
		}
		o = ob_hndp[i];
		if ( o == NULL ) {
			o = (OBHND *)Kmalloc(sizeof(OBHND));
			if ( o == NULL ) {
				return -1;
			}
			o->used = FALSE;
			ob_hndp[i] = o;
		}
		if ( !o->used ) {
			o->used = TRUE;
			o->mgr = mgr;
			o->h = h;
			o->refs = 0;
			o->uuid = *uuid;
			o->prt = *prt;
			ob_hnd_next = i + 1;
			return i;
		}
	}
	return -1;
}

LOCAL ID key_new( INT hi, ID pid, UINT ops, BOOL nowait )
{
	INT	i, n;

	for ( n = 0, i = ob_key_next; n < OB_KEY_MAX; n++, i++ ) {
		if ( i >= OB_KEY_MAX ) {
			i = 0;
		}
		if ( !ob_key[i].used ) {
			ob_key_next = i + 1;
			ob_key[i].used = TRUE;
			ob_key[i].hnd = hi;
			ob_key[i].pid = pid;
			ob_key[i].ops = ops;
			ob_key[i].nowait = nowait;
			(*ob_hndp[hi]).refs++;
			return (ID)( ( ( (*ob_hndp[hi]).mgr + 1 ) << KEY_MGR_SHIFT ) | ( i + 1 ) );
		}
	}
	return E_LIMIT;
}

/* The key's slot, when the caller may use it */
LOCAL OBKEY *key_of( ID key )
{
	INT	i = ( key & ( ( 1 << KEY_MGR_SHIFT ) - 1 ) ) - 1;
	INT	m = ( key >> KEY_MGR_SHIFT ) & 0x7F;
	OBKEY	*k;

	if ( key <= 0 || i < 0 || i >= OB_KEY_MAX ) {
		return NULL;
	}
	k = &ob_key[i];
	if ( !k->used || (*ob_hndp[k->hnd]).mgr + 1 != m ) {
		return NULL;
	}
	return k;
}

/* One reference fewer to a handle; the last one closes it */
LOCAL void hnd_release( INT hi )
{
	CONST T_OBMGR	*m = NULL;
	INT		h = 0;

	LOCK();
	if ( --(*ob_hndp[hi]).refs <= 0 ) {
		m = ob_mgr[(*ob_hndp[hi]).mgr];
		h = (*ob_hndp[hi]).h;
		(*ob_hndp[hi]).used = FALSE;
	}
	UNLOCK();
	if ( m != NULL && m->close != NULL ) {
		(void)m->close(h);
	}
}

/*
 * A key taken up for an operation: it must be the caller's and allow
 * what is asked (on the record, when there is one). The handle is held
 * until key_done, so closing the key meanwhile does not pull it away.
 */
LOCAL ER key_take( ID key, UINT need, INT recno, INT *p_hi, BOOL *p_nowait )
{
	OBKEY	*k;
	OBHND	*o;
	UINT	ops;
	INT	i;
	ID	pid = ts_get_pid();	/* before our lock: it takes the process table's */

	LOCK();
	k = key_of(key);
	if ( k == NULL ) {
		UNLOCK();
		return E_ID;
	}
	if ( k->pid != pid ) {
		UNLOCK();
		return E_OACV;			/* someone else's key */
	}
	o = &(*ob_hndp[k->hnd]);
	ops = k->ops;
	if ( recno >= 0 ) {
		CONST UINT	onrec = OB_OP_READ | OB_OP_WRITE | OB_OP_RECORD;

		for ( i = 0; i < o->prt.nrmask && i < OB_RMASK_MAX; i++ ) {
			if ( o->prt.rmask[i].recno == recno ) {
				UINT	lim = o->prt.rmask[i].ops;

				/* a limit that names x: only a key that may control the object */
				if ( ( lim & OB_OP_EXEC ) != 0 && ( ops & OB_OP_EXEC ) == 0 ) lim = 0;
				ops = ( ops & ~onrec ) | ( ops & lim & onrec );
				break;
			}
		}
	}
	if ( ( ops & need ) != need ) {
		UNLOCK();
		return E_OACV;
	}
	o->refs++;
	*p_hi = k->hnd;
	if ( p_nowait != NULL ) {
		*p_nowait = k->nowait;
	}
	UNLOCK();

	return E_OK;
}

#define key_done(hi)	hnd_release(hi)

#define MGR(hi)		ob_mgr[(*ob_hndp[hi]).mgr]
#define HND(hi)		(*ob_hndp[hi]).h

/* ---------------------------------------------------------------- opening */

/* What a process was granted on an object */
LOCAL UINT granted( CONST TS_UUID *uuid, ID pid )
{
	UINT	ops = 0;
	INT	i;

	LOCK();
	for ( i = 0; i < OB_GRANT_MAX; i++ ) {
		if ( ob_grant[i].used && ob_grant[i].pid == pid
		  && same_uuid(&ob_grant[i].uuid, uuid) ) {
			ops |= ob_grant[i].ops;
		}
	}
	UNLOCK();

	return ops;
}

EXPORT UINT knl_ob_granted( ID pid, CONST TS_UUID *uuid )
{
	if ( ob_mtx <= 0 || pid <= 0 || uuid == NULL ) {
		return 0;
	}
	return granted(uuid, pid);
}

EXPORT void knl_ob_grant( ID pid, CONST TS_UUID *uuid, UINT ops )
{
	OBGRANT	*g = NULL;
	INT	i;

	if ( ob_mtx <= 0 || pid <= 0 || uuid == NULL ) {
		return;				/* the kernel needs none */
	}
	LOCK();
	for ( i = 0; i < OB_GRANT_MAX && g == NULL; i++ ) {
		if ( ob_grant[i].used && ob_grant[i].pid == pid
		  && same_uuid(&ob_grant[i].uuid, uuid) ) {
			g = &ob_grant[i];
		}
	}
	if ( g == NULL && ( ops & OB_OP_ALL ) != 0 ) {
		for ( i = 0; i < OB_GRANT_MAX; i++ ) {
			if ( !ob_grant[i].used ) {
				g = &ob_grant[i];
				break;
			}
			if ( g == NULL || ob_grant[i].age - g->age > 0x80000000U ) {
				g = &ob_grant[i];	/* the oldest so far */
			}
		}
	}
	if ( g != NULL ) {
		g->used = (BOOL)( ( ops & OB_OP_ALL ) != 0 );
		g->pid = pid;
		g->uuid = *uuid;
		g->ops = ops & OB_OP_ALL;
		g->age = ++ob_grant_age;
	}
	UNLOCK();
}

/* A process's grants go with it */
LOCAL void grant_drop( ID pid )
{
	INT	i;

	LOCK();
	for ( i = 0; i < OB_GRANT_MAX; i++ ) {
		if ( ob_grant[i].used && ob_grant[i].pid == pid ) {
			ob_grant[i].used = FALSE;
		}
	}
	UNLOCK();
}

EXPORT ID ob_opn_obj( CONST TS_UUID *uuid, UINT ops )
{
	CONST T_OBMGR	*m;
	T_OBPRT		*prt;
	BOOL		nowait = (BOOL)( ( ops & OB_O_NOWAIT ) != 0 );
	UINT		allowed;
	INT		mi, h, hi;
	ID		key, pid = ts_get_pid();

	ops &= OB_OP_ALL;
	mi = mgr_index(uuid);
	if ( mi < 0 ) {
		return E_NOEXS;
	}
	m = ob_mgr[mi];
	if ( m->open == NULL ) {
		return E_NOSPT;
	}
	prt = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
	if ( prt == NULL ) {
		return E_NOMEM;
	}
	prot_of(m, uuid, prt);
	allowed = permit_on(m, uuid, knl_ob_crd_of(pid), prt);
	if ( ( ops & ~allowed ) != 0 && pid > 0 ) {
		allowed |= granted(uuid, pid);
	}
	if ( ( ops & ~allowed ) != 0 ) {
		Kfree(prt);
		return E_OACV;
	}
	h = m->open(uuid, ops);
	if ( h <= 0 ) {
		Kfree(prt);
		return ( h < 0 ) ? (ER)h : E_SYS;
	}
	LOCK();
	hi = hnd_new(mi, h, uuid, prt);
	key = ( hi >= 0 ) ? key_new(hi, pid, ops, nowait) : E_LIMIT;
	if ( hi >= 0 && key < 0 ) {
		(*ob_hndp[hi]).used = FALSE;
	}
	UNLOCK();
	Kfree(prt);
	if ( key < 0 && m->close != NULL ) {
		(void)m->close(h);
	}
	return key;
}

EXPORT ER ob_cls_obj( ID key )
{
	OBKEY	*k;
	INT	hi;
	ID	me = ts_get_pid();

	LOCK();
	k = key_of(key);
	if ( k == NULL || k->pid != me ) {
		UNLOCK();
		return ( k == NULL ) ? E_ID : E_OACV;
	}
	hi = k->hnd;
	k->used = FALSE;
	UNLOCK();
	ntf_drop(key, 0);
	hnd_release(hi);

	return E_OK;
}

EXPORT ER knl_ob_key_rec( ID key, UINT ops, INT recno, TS_UUID *p_uuid )
{
	INT	hi;
	ER	er = key_take(key, ops, recno, &hi, NULL);

	if ( er < E_OK ) {
		return er;
	}
	*p_uuid = (*ob_hndp[hi]).uuid;
	key_done(hi);

	return E_OK;
}

EXPORT ER knl_ob_key_uuid( ID key, UINT ops, TS_UUID *p_uuid )
{
	return knl_ob_key_rec(key, ops, -1, p_uuid);
}

/*
 * A key handed to a process is written into what it holds: a process
 * hands one only to itself, its parent or its children, or to a
 * process whose object it may write (knl_obprc_may).
 */
EXPORT ID ob_dup_key( ID key, UINT ops, ID pid )
{
	OBKEY	*k;
	ID	nk, me = ts_get_pid();

	if ( me > 0 ) {
		ER	er = knl_obprc_may(pid, OB_OP_WRITE, OBP_PARENT | OBP_CHILD);

		if ( er < E_OK ) {
			return er;
		}
	}
	LOCK();
	k = key_of(key);
	if ( k == NULL || k->pid != me ) {
		UNLOCK();
		return ( k == NULL ) ? E_ID : E_OACV;
	}
	nk = key_new(k->hnd, pid, k->ops & ops & OB_OP_ALL, k->nowait);
	UNLOCK();

	return nk;
}

/* ---------------------------------------------------------------- records and attributes */

/*
 * A change to be noticed, taken while the handle is still held and
 * sent once it is let go: a notice may go to storage the same manager
 * keeps. Writing to a channel is sending, which is not a change.
 */
typedef struct {
	BOOL	on;
	TS_UUID	uuid;
	INT	recno;
} OBCHG;

LOCAL void chg_take( INT hi, INT recno, OBCHG *c )
{
	c->on = (BOOL)( ob_nntf > 0 && ob_mgr[(*ob_hndp[hi]).mgr]->type != OB_T_CHANNEL );
	c->uuid = (*ob_hndp[hi]).uuid;
	c->recno = recno;
}

LOCAL void chg_post( CONST OBCHG *c )
{
	if ( c->on ) {
		knl_ob_post(&c->uuid, c->recno, OB_E_CHANGE, NULL);
	}
}

EXPORT ER ob_rea_rec( ID key, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	BOOL	nowait;
	INT	hi;
	ER	er = key_take(key, OB_OP_READ, recno, &hi, &nowait);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->rea != NULL )
	   ? MGR(hi)->rea(HND(hi), recno, off, buf, size, p_asize, nowait) : E_NOSPT;
	key_done(hi);
	return er;
}

EXPORT ER ob_wri_rec( ID key, INT recno, D off, CONST void *buf, SZ size, SZ *p_asize )
{
	BOOL	nowait;
	OBCHG	chg;
	INT	hi;
	ER	er = key_take(key, OB_OP_WRITE, recno, &hi, &nowait);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->wri != NULL )
	   ? MGR(hi)->wri(HND(hi), recno, off, buf, size, p_asize, nowait) : E_NOSPT;
	chg_take(hi, recno, &chg);
	key_done(hi);
	if ( er >= E_OK ) chg_post(&chg);
	return er;
}

EXPORT ER ob_apd_rec( ID key, UINT rt, UINT sub, INT *p_recno )
{
	OBCHG	chg;
	INT	hi;
	ER	er = key_take(key, OB_OP_RECORD, -1, &hi, NULL);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->apd != NULL ) ? MGR(hi)->apd(HND(hi), rt, sub, p_recno) : E_NOSPT;
	chg_take(hi, ( er >= E_OK && p_recno != NULL ) ? *p_recno : -1, &chg);
	key_done(hi);
	if ( er >= E_OK ) chg_post(&chg);
	return er;
}

EXPORT ER ob_trn_rec( ID key, INT recno, UD size )
{
	OBCHG	chg;
	INT	hi;
	ER	er = key_take(key, OB_OP_WRITE, recno, &hi, NULL);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->trn != NULL ) ? MGR(hi)->trn(HND(hi), recno, size) : E_NOSPT;
	chg_take(hi, recno, &chg);
	key_done(hi);
	if ( er >= E_OK ) chg_post(&chg);
	return er;
}

EXPORT ER ob_del_rec( ID key, INT recno )
{
	OBCHG	chg;
	INT	hi;
	ER	er = key_take(key, OB_OP_RECORD, recno, &hi, NULL);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->drc != NULL ) ? MGR(hi)->drc(HND(hi), recno) : E_NOSPT;
	chg_take(hi, recno, &chg);
	key_done(hi);
	if ( er >= E_OK ) chg_post(&chg);
	return er;
}

EXPORT ER ob_lst_rec( ID key, T_OBREC *buf, INT n, INT *p_cnt )
{
	INT	hi;
	ER	er = key_take(key, OB_OP_ATRRD, -1, &hi, NULL);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->lrc != NULL ) ? MGR(hi)->lrc(HND(hi), buf, n, p_cnt) : E_NOSPT;
	key_done(hi);
	return er;
}

/* ".ico": the icon, which the resource calls reach as well */
LOCAL BOOL is_icon_name( CONST UB *name )
{
	return (BOOL)( name != NULL && name[0] == '.' && name[1] == 'i' && name[2] == 'c'
		    && name[3] == 'o' && name[4] == 0 );
}

/* A resource's name: after the UUID, and not a record or the metadata; of the "." ones only the icon */
LOCAL BOOL res_name_ok( CONST UB *name )
{
	INT	n, k;

	if ( name == NULL || ( name[0] != '_' && name[0] != '.' ) ) {
		return FALSE;
	}
	if ( name[0] == '.' ) {
		return is_icon_name(name);
	}
	for ( n = 0; name[n] != 0; n++ ) {
		if ( name[n] == '/' || n >= OB_RES_NAME - 1 ) return FALSE;
	}
	if ( n == 5 && name[1] == 'j' && name[2] == 's' && name[3] == 'o' && name[4] == 'n' ) {
		return FALSE;			/* .json */
	}
	/* _<number>.xtad and _<number>.bin are records */
	if ( name[0] == '_' ) {
		for ( k = 1; name[k] >= '0' && name[k] <= '9'; k++ ) ;
		if ( k > 1 && name[k] == '.' ) {
			CONST UB	*e = name + k + 1;

			if ( ( e[0] == 'x' && e[1] == 't' && e[2] == 'a' && e[3] == 'd' && e[4] == 0 )
			  || ( e[0] == 'b' && e[1] == 'i' && e[2] == 'n' && e[3] == 0 ) ) {
				return FALSE;
			}
		}
	}
	return TRUE;
}

/* The icon read, and part of it from `off` for the resource calls */
LOCAL ER icon_read( INT hi, D off, UB *buf, SZ size, SZ *p_asize )
{
	UB	*all;
	SZ	len = 0, n;
	ER	er;

	if ( MGR(hi)->gic == NULL ) {
		return E_NOSPT;
	}
	if ( off == 0 ) {
		er = MGR(hi)->gic(HND(hi), buf, size, p_asize);
		if ( er >= E_OK && p_asize != NULL && *p_asize > size ) *p_asize = size;
		return er;
	}
	er = MGR(hi)->gic(HND(hi), NULL, 0, &len);
	if ( er < E_OK ) {
		return er;
	}
	all = (UB *)Kmalloc(( len > 0 ) ? len : 1);
	if ( all == NULL ) {
		return E_NOMEM;
	}
	er = MGR(hi)->gic(HND(hi), all, len, &len);
	if ( er >= E_OK ) {
		n = ( off < (D)len ) ? len - (SZ)off : 0;
		if ( n > size ) n = size;
		if ( n > 0 ) knl_memcpy(buf, all + off, (INT)n);
		if ( p_asize != NULL ) *p_asize = n;
	}
	Kfree(all);
	return er;
}

/* What an icon may be: an ICO or a PNG */
LOCAL BOOL icon_ok( CONST UB *b, SZ size )
{
	if ( size >= 6 && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0 ) {
		return TRUE;
	}
	return (BOOL)( size >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' );
}

EXPORT ER ob_get_ico( ID key, UB *buf, SZ size, SZ *p_asize )
{
	INT	hi;
	ER	er;

	if ( size < 0 || ( size > 0 && buf == NULL ) ) return E_PAR;
	er = key_take(key, OB_OP_ATRRD, -1, &hi, NULL);
	if ( er < E_OK ) return er;
	er = ( MGR(hi)->gic != NULL ) ? MGR(hi)->gic(HND(hi), buf, size, p_asize) : E_NOSPT;
	key_done(hi);
	return er;
}

EXPORT ER ob_set_ico( ID key, CONST UB *buf, SZ size )
{
	INT	hi;
	OBCHG	chg;
	ER	er;

	if ( size < 0 || size > OB_ICO_MAX || ( size > 0 && ( buf == NULL || !icon_ok(buf, size) ) ) ) {
		return E_PAR;
	}
	er = key_take(key, OB_OP_ATRWR, -1, &hi, NULL);
	if ( er < E_OK ) return er;
	er = ( MGR(hi)->sic != NULL ) ? MGR(hi)->sic(HND(hi), buf, size) : E_NOSPT;
	chg_take(hi, -1, &chg);
	key_done(hi);
	if ( er >= E_OK ) chg_post(&chg);
	return er;
}

EXPORT ER ob_rea_res( ID key, CONST UB *name, D off, void *buf, SZ size, SZ *p_asize )
{
	INT	hi;
	ER	er;

	if ( !res_name_ok(name) ) return E_PAR;
	if ( is_icon_name(name) ) {
		/* the icon, under the icon's rights */
		if ( off < 0 || size < 0 ) return E_PAR;
		er = key_take(key, OB_OP_ATRRD, -1, &hi, NULL);
		if ( er < E_OK ) return er;
		er = icon_read(hi, off, (UB *)buf, size, p_asize);
		key_done(hi);
		return er;
	}
	er = key_take(key, OB_OP_READ, -1, &hi, NULL);
	if ( er < E_OK ) return er;
	er = ( MGR(hi)->rres != NULL )
	   ? MGR(hi)->rres(HND(hi), name, off, buf, size, p_asize) : E_NOSPT;
	key_done(hi);
	return er;
}

EXPORT ER ob_wri_res( ID key, CONST UB *name, CONST void *buf, SZ size )
{
	INT	hi;
	ER	er;

	if ( !res_name_ok(name) ) return E_PAR;
	if ( is_icon_name(name) ) {
		return ob_set_ico(key, (CONST UB *)buf, size);
	}
	er = key_take(key, OB_OP_WRITE, -1, &hi, NULL);
	if ( er < E_OK ) return er;
	er = ( MGR(hi)->wres != NULL ) ? MGR(hi)->wres(HND(hi), name, buf, size) : E_NOSPT;
	key_done(hi);
	return er;
}

EXPORT ER ob_del_res( ID key, CONST UB *name )
{
	INT	hi;
	ER	er;
	SZ	had = 0;

	if ( !res_name_ok(name) ) return E_PAR;
	if ( is_icon_name(name) ) {
		er = ob_get_ico(key, NULL, 0, &had);
		return ( er < E_OK ) ? er : ob_set_ico(key, NULL, 0);
	}
	er = key_take(key, OB_OP_WRITE, -1, &hi, NULL);
	if ( er < E_OK ) return er;
	er = ( MGR(hi)->dres != NULL ) ? MGR(hi)->dres(HND(hi), name) : E_NOSPT;
	key_done(hi);
	return er;
}

EXPORT ER ob_lst_res( ID key, UB *buf, SZ size, INT *p_cnt )
{
	INT	hi;
	ER	er = key_take(key, OB_OP_ATRRD, -1, &hi, NULL);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->lres != NULL ) ? MGR(hi)->lres(HND(hi), buf, size, p_cnt) : E_NOSPT;
	key_done(hi);
	return er;
}

EXPORT ER ob_get_atr( ID key, UB *json, SZ size, SZ *p_asize )
{
	INT	hi;
	ER	er = key_take(key, OB_OP_ATRRD, -1, &hi, NULL);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->gat != NULL ) ? MGR(hi)->gat(HND(hi), json, size, p_asize) : E_NOSPT;
	key_done(hi);
	return er;
}

/* The text of the protection a piece of metadata carries, as a span */
LOCAL BOOL access_span( CONST UB *j, INT len, INT *p_off, INT *p_len )
{
	INT	v = knl_oj_path(j, len, "tessronos", "access");
	INT	e = ( v >= 0 ) ? knl_oj_skip(j, len, v) : -1;

	if ( e < 0 ) {
		return FALSE;
	}
	*p_off = v;
	*p_len = e - v;
	return TRUE;
}

/*
 * Attributes written. The protection rides in the same text, so a
 * writer of attributes who may not change the protection gets the old
 * protection kept whatever it wrote.
 */
EXPORT ER ob_set_atr( ID key, CONST UB *json, SZ size )
{
	UB	*old = NULL, *nw = NULL;
	SZ	olen = 0;
	INT	hi, ao, al, no, nl, len;
	BOOL	may_prot, had, has;
	OBKEY	*k;
	ER	er = key_take(key, OB_OP_ATRWR, -1, &hi, NULL);

	if ( er < E_OK ) return er;
	if ( MGR(hi)->sat == NULL || MGR(hi)->gat == NULL ) {
		key_done(hi);
		return E_NOSPT;
	}
	if ( json == NULL || size <= 0 || size >= OB_META_MAX ) {
		key_done(hi);
		return E_PAR;
	}
	LOCK();
	k = key_of(key);
	may_prot = (BOOL)( k != NULL && ( k->ops & OB_OP_PROT ) != 0 );
	UNLOCK();

	old = (UB *)Kmalloc(OB_META_MAX);
	nw = (UB *)Kmalloc(OB_META_MAX);
	if ( old == NULL || nw == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	er = MGR(hi)->gat(HND(hi), old, OB_META_MAX - 1, &olen);
	if ( er < E_OK ) {
		olen = 0;
	}
	knl_memcpy(nw, json, (INT)size);
	nw[size] = 0;
	len = (INT)size;

	had = access_span(old, (INT)olen, &ao, &al);
	has = access_span(nw, len, &no, &nl);
	if ( !may_prot ) {
		if ( had ) {
			len = knl_oj_set_path(nw, len, OB_META_MAX, "tessronos", "access",
					      old + ao, al);
		} else if ( has ) {
			er = E_OACV;		/* protection added by one who may not */
			goto out;
		}
		if ( len < 0 ) {
			er = E_PAR;
			goto out;
		}
	}
	er = MGR(hi)->sat(HND(hi), nw, (SZ)len);
	if ( er >= E_OK ) {
		prot_of(MGR(hi), &(*ob_hndp[hi]).uuid, &(*ob_hndp[hi]).prt);
	}
    out:
	if ( old != NULL ) Kfree(old);
	if ( nw != NULL ) Kfree(nw);
	{
		OBCHG	chg;

		chg_take(hi, -1, &chg);
		key_done(hi);
		if ( er >= E_OK ) chg_post(&chg);
	}
	return er;
}

/* ---------------------------------------------------------------- objects */

/* The manager that makes objects of a kind */
LOCAL INT mgr_for( UINT type, UINT sub )
{
	INT	i;

	for ( i = 0; i < ob_nmgr; i++ ) {
		if ( ob_mgr[i]->type == type && ( sub == 0 || ob_mgr[i]->sub == 0
					       || ob_mgr[i]->sub == sub ) ) {
			return i;
		}
	}
	return -1;
}

EXPORT ER ob_cre_obj( CONST T_OBCRE *c, TS_UUID *p_uuid )
{
	CONST T_OBCRD	*crd;
	T_OBPRT		*prt;
	INT		mi;
	ER		er;

	if ( c == NULL || p_uuid == NULL ) {
		return E_PAR;
	}
	if ( c->icon != NULL && ( c->iconsz <= 0 || c->iconsz > OB_ICO_MAX
				  || !icon_ok((CONST UB *)c->icon, c->iconsz) ) ) {
		return E_PAR;			/* an icon is an ICO or a PNG, whole */
	}
	mi = mgr_for(c->type, c->sub);
	if ( mi < 0 ) {
		return E_NOSPT;
	}
	if ( ob_mgr[mi]->create == NULL ) {
		return E_NOSPT;
	}
	prt = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
	if ( prt == NULL ) {
		return E_NOMEM;
	}
	crd = knl_ob_crd_of(ts_get_pid());

	/* an identity chosen by the maker: the system's own objects only */
	if ( !knl_ob_uuid_zero(&c->uuid) ) {
		if ( !knl_ob_is_admin(crd) ) {
			Kfree(prt);
			return E_OACV;
		}
		if ( mgr_index(&c->uuid) >= 0 ) {
			Kfree(prt);
			return E_OBJ;		/* there is one already */
		}
	}

	/* starting a program is running it */
	if ( c->type == OB_T_PROCESS ) {
		CONST T_OBMGR	*pm = knl_ob_mgr_of(&c->prog);

		if ( pm == NULL ) {
			Kfree(prt);
			return E_NOEXS;
		}
		prot_of(pm, &c->prog, prt);
		if ( ( permit_on(pm, &c->prog, crd, prt) & OB_OP_EXEC ) == 0 ) {
			Kfree(prt);
			return E_OACV;
		}
	}

	if ( c->prt != NULL ) {
		*prt = *c->prt;
		if ( !knl_ob_is_admin(crd) ) {
			prt->owner = crd->user;	/* nobody makes objects for another */
		}
	} else {
		knl_ob_prt_default(prt, crd);
	}
	er = ob_mgr[mi]->create(c, prt, p_uuid);
	Kfree(prt);

	return er;
}

/* Whether the caller may do ops to an object */
LOCAL ER allowed( CONST T_OBMGR *m, CONST TS_UUID *uuid, UINT ops )
{
	T_OBPRT	*prt = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
	UINT	may;

	if ( prt == NULL ) {
		return E_NOMEM;
	}
	prot_of(m, uuid, prt);
	may = permit_on(m, uuid, knl_ob_crd_of(ts_get_pid()), prt);
	Kfree(prt);

	return ( ( may & ops ) == ops ) ? E_OK : E_OACV;
}

EXPORT ER ob_del_obj( CONST TS_UUID *uuid )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);
	ER		er;

	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->remove == NULL ) {
		return E_NOSPT;
	}
	er = allowed(m, uuid, OB_OP_DELETE);
	if ( er >= E_OK ) {
		TS_UUID	gone = *uuid;		/* the caller's may go with the object */

		er = m->remove(uuid);
		/* the window manager tells of every window that closes, and
		   the socket manager of every socket, however it closed, so
		   it is not told twice */
		if ( er >= E_OK && m->type != OB_T_WINDOW
		  && !( m->type == OB_T_CHANNEL && m->sub == OB_S_SOCKET ) ) {
			knl_ob_post(&gone, -1, OB_E_DELETE, NULL);
		}
	}
	return er;
}

EXPORT ER ob_ref_obj( CONST TS_UUID *uuid, T_OBREF *pk_ref )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);

	if ( pk_ref == NULL ) {
		return E_PAR;
	}
	if ( m == NULL ) {
		return E_NOEXS;
	}
	knl_memset(pk_ref, 0, sizeof(*pk_ref));
	pk_ref->uuid = *uuid;
	pk_ref->type = m->type;
	pk_ref->sub = m->sub;

	return ( m->ref != NULL ) ? m->ref(uuid, pk_ref) : E_OK;
}

LOCAL ER lnk_by( CONST TS_UUID *uuid, INT delta )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);
	ER		er;

	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->lnk == NULL ) {
		return E_NOSPT;
	}
	er = allowed(m, uuid, OB_OP_LINK);

	return ( er < E_OK ) ? er : m->lnk(uuid, delta);
}

EXPORT ER ob_lnk_obj( CONST TS_UUID *uuid )
{
	return lnk_by(uuid, 1);
}

EXPORT ER ob_unl_obj( CONST TS_UUID *uuid )
{
	return lnk_by(uuid, -1);
}

/*
 * The objects of a type (0: every type) in UUID order: each manager of
 * the type is asked for what follows 'from', and the answers are put
 * together keeping the smallest.
 */
#define LST_PAGE	64		/* objects asked of a manager at a time when some are left out */

EXPORT ER ob_lst_obj( UINT type, UINT sub, CONST TS_UUID *from, TS_UUID *buf, INT n,
		      INT *p_cnt )
{
	TS_UUID	*part;
	INT	cnt = 0, i;

	if ( buf == NULL || p_cnt == NULL || n <= 0 ) {
		return E_PAR;
	}
	part = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
	if ( part == NULL ) {
		return E_NOMEM;
	}
	for ( i = 0; i < ob_nmgr; i++ ) {
		CONST T_OBMGR	*m = ob_mgr[i];
		INT		got = 0, a, b, k;
		TS_UUID		*merged;

		if ( ( type != 0 && m->type != type ) || m->lst == NULL
		  || ( sub != 0 && m->sub != 0 && m->sub != sub ) ) {
			continue;
		}
		if ( sub != 0 && m->sub == 0 && m->ref != NULL ) {
			/*
			 * A manager of every subtype: only those of the one asked
			 * for, read on a page at a time until n are found or there
			 * are no more
			 */
			TS_UUID		*pg = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * LST_PAGE), at;
			CONST TS_UUID	*fr = from;
			T_OBREF		r;
			INT		pn, j;

			if ( pg == NULL ) {
				break;
			}
			while ( got < n ) {
				pn = 0;
				if ( m->lst(fr, pg, LST_PAGE, &pn) < E_OK || pn <= 0 ) break;
				for ( j = 0; j < pn && got < n; j++ ) {
					knl_memset(&r, 0, sizeof(r));
					if ( m->ref(&pg[j], &r) >= E_OK && r.sub == sub ) {
						part[got++] = pg[j];
					}
				}
				if ( pn < LST_PAGE ) break;
				at = pg[pn - 1];
				fr = &at;
			}
			Kfree(pg);
		} else if ( m->lst(from, part, n, &got) < E_OK ) {
			got = 0;
		}
		if ( got <= 0 ) {
			continue;
		}
		merged = (TS_UUID *)Kmalloc(sizeof(TS_UUID) * (SZ)n);
		if ( merged == NULL ) {
			break;
		}
		for ( a = 0, b = 0, k = 0; k < n && ( a < cnt || b < got ); k++ ) {
			if ( b >= got || ( a < cnt && ts_uuid_cmp(&buf[a], &part[b]) <= 0 ) ) {
				merged[k] = buf[a++];
			} else {
				merged[k] = part[b++];
			}
		}
		knl_memcpy(buf, merged, (INT)( sizeof(TS_UUID) * (SZ)k ));
		cnt = k;
		Kfree(merged);
	}
	Kfree(part);
	*p_cnt = cnt;

	return E_OK;
}

EXPORT ER ob_fnd_nam( CONST UB *name, TS_UUID *p_uuid )
{
	INT	i;

	if ( name == NULL || p_uuid == NULL || name[0] == 0 ) {
		return E_PAR;
	}
	for ( i = 0; i < ob_nmgr; i++ ) {
		if ( ob_mgr[i]->fndnam != NULL && ob_mgr[i]->fndnam(name, p_uuid) >= E_OK ) {
			return E_OK;
		}
	}
	return E_NOEXS;
}

/* ---------------------------------------------------------------- the links of a box */

#define LNK_CHUNK	( 16 * 1024 )	/* bytes of the box's record 0 read at a time */
#define LNK_TEXT_MAX	( 4 * 1024 * 1024 )	/* and at most */

/* Record 0 of the box, read whole with the caller's rights, in a buffer the caller frees */
LOCAL ER lnk_text( CONST TS_UUID *box, UB **p_t, SZ *p_len )
{
	UB	*t = NULL, *nt;
	SZ	have = 0, room = 0, got;
	ID	key;
	ER	er = E_OK;

	key = ob_opn_obj(box, OB_OP_READ);
	if ( key < E_OK ) {
		return (ER)key;
	}
	for ( ;; ) {
		if ( room - have < LNK_CHUNK ) {
			if ( room + LNK_CHUNK > LNK_TEXT_MAX ) {
				break;			/* the rest is not looked at */
			}
			nt = (UB *)Kmalloc(room + LNK_CHUNK);
			if ( nt == NULL ) {
				er = E_NOMEM;
				break;
			}
			if ( t != NULL ) {
				knl_memcpy(nt, t, (INT)have);
				Kfree(t);
			}
			t = nt;
			room += LNK_CHUNK;
		}
		got = 0;
		er = ob_rea_rec(key, 0, (D)have, t + have, room - have, &got);
		if ( er < E_OK ) {
			/* past the end, or no record 0: no more links */
			er = ( have > 0 || er == E_NOEXS ) ? E_OK : er;
			break;
		}
		if ( got <= 0 ) {
			break;
		}
		have += got;
		if ( have < room ) {
			break;			/* read to the end */
		}
	}
	(void)ob_cls_obj(key);
	if ( er < E_OK ) {
		if ( t != NULL ) Kfree(t);
		return er;
	}
	*p_t = t;
	*p_len = have;
	return E_OK;
}

LOCAL BOOL lnk_blank( UB c )
{
	return (BOOL)( c == ' ' || c == '\t' || c == '\r' || c == '\n' );
}

/*
 * The next <link> at or after *p_at whose id attribute starts with a
 * UUID ("{uuid}_0.xtad", or the UUID alone): the UUID, and *p_at moved
 * past the tag. The text is taken as words; the XML is not parsed.
 */
LOCAL BOOL lnk_next( CONST UB *t, SZ len, SZ *p_at, TS_UUID *u )
{
	char	us[TS_UUID_STRLEN + 1];
	SZ	i, j, k, v;

	for ( i = *p_at; i + 5 < len; i++ ) {
		if ( t[i] != '<' || t[i + 1] != 'l' || t[i + 2] != 'i' || t[i + 3] != 'n'
		  || t[i + 4] != 'k' || !lnk_blank(t[i + 5]) ) {
			continue;
		}
		for ( j = i + 5; j < len && t[j] != '>'; j++ ) ;
		for ( k = i + 5; k + 4 < j; k++ ) {
			if ( lnk_blank(t[k - 1]) && t[k] == 'i' && t[k + 1] == 'd'
			  && t[k + 2] == '=' && ( t[k + 3] == '"' || t[k + 3] == '\'' ) ) {
				break;
			}
		}
		*p_at = j;
		v = k + 4;
		if ( k + 4 >= j || v + TS_UUID_STRLEN > j ) {
			i = j;
			continue;
		}
		knl_memcpy(us, t + v, TS_UUID_STRLEN);
		us[TS_UUID_STRLEN] = 0;
		if ( ts_str_to_uuid(us, u) >= E_OK ) {
			return TRUE;
		}
		i = j;
	}
	*p_at = len;
	return FALSE;
}

LOCAL UB lnk_lower( UB c )
{
	return ( c >= 'A' && c <= 'Z' ) ? (UB)( c - 'A' + 'a' ) : c;
}

/* Whether 'have' is 'want' without regard to case, or 'want' and an extension */
LOCAL BOOL lnk_name_is( CONST UB *have, CONST UB *want )
{
	INT	i;

	for ( i = 0; have[i] != 0 && want[i] != 0; i++ ) {
		if ( lnk_lower(have[i]) != lnk_lower(want[i]) ) return FALSE;
	}
	if ( want[i] != 0 ) return FALSE;
	return (BOOL)( have[i] == 0 || have[i] == '.' );
}

/* Whether the object is named 'name': its object name, or the file name its metadata keeps */
LOCAL BOOL lnk_named( CONST TS_UUID *u, CONST UB *name, UB *meta )
{
	T_OBREF	r;
	UB	file[256];
	SZ	asz = 0;
	INT	p, len;
	ID	key;
	BOOL	is = FALSE;

	if ( ob_ref_obj(u, &r) < E_OK ) {
		return FALSE;
	}
	r.name[OB_NAME_MAX - 1] = 0;
	if ( lnk_name_is(r.name, name) ) {
		return TRUE;
	}
	key = ob_opn_obj(u, OB_OP_ATRRD);
	if ( key < E_OK ) {
		return FALSE;
	}
	if ( ob_get_atr(key, meta, OB_META_MAX - 1, &asz) >= E_OK && asz > 0 ) {
		len = (INT)asz;
		p = knl_oj_path(meta, len, "tessronos", "file");
		p = ( p >= 0 && meta[p] == '{' ) ? knl_oj_member(meta, len, p, "name") : -1;
		if ( p >= 0 && knl_oj_str(meta, len, p, file, sizeof(file)) > 0 ) {
			is = lnk_name_is(file, name);
		}
	}
	(void)ob_cls_obj(key);
	return is;
}

EXPORT ER ob_lst_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *buf, INT n, INT *p_cnt )
{
	UB	*t = NULL, *meta = NULL;
	SZ	len = 0, at = 0;
	TS_UUID	u;
	INT	cnt = 0;
	BOOL	all = (BOOL)( name == NULL || name[0] == 0 );
	ER	er;

	if ( box == NULL || p_cnt == NULL || n < 0 || ( n > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	er = lnk_text(box, &t, &len);
	if ( er < E_OK ) {
		return er;
	}
	if ( !all ) {
		meta = (UB *)Kmalloc(OB_META_MAX);
		if ( meta == NULL ) {
			if ( t != NULL ) Kfree(t);
			return E_NOMEM;
		}
	}
	while ( t != NULL && lnk_next(t, len, &at, &u) ) {
		if ( !all && !lnk_named(&u, name, meta) ) {
			continue;
		}
		if ( cnt < n ) {
			buf[cnt] = u;
		}
		cnt++;
	}
	if ( meta != NULL ) Kfree(meta);
	if ( t != NULL ) Kfree(t);
	*p_cnt = cnt;
	return E_OK;
}

EXPORT ER ob_fnd_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *p_uuid )
{
	UB	*t = NULL, *meta = NULL;
	SZ	len = 0, at = 0;
	TS_UUID	u;
	BOOL	all = (BOOL)( name == NULL || name[0] == 0 );
	ER	er;

	if ( box == NULL || p_uuid == NULL ) {
		return E_PAR;
	}
	er = lnk_text(box, &t, &len);
	if ( er < E_OK ) {
		return er;
	}
	er = E_NOEXS;
	if ( !all ) {
		meta = (UB *)Kmalloc(OB_META_MAX);
		if ( meta == NULL ) {
			er = E_NOMEM;
		}
	}
	while ( er == E_NOEXS && t != NULL && lnk_next(t, len, &at, &u) ) {
		if ( all || lnk_named(&u, name, meta) ) {
			*p_uuid = u;
			er = E_OK;
		}
	}
	if ( meta != NULL ) Kfree(meta);
	if ( t != NULL ) Kfree(t);
	return er;
}


/* ---------------------------------------------------------------- records between objects */

#define OB_REC_LIST	1024		/* records looked at in one object */
#define OB_CHUNK	( 16 * 1024 )	/* bytes copied at a time */

/* The records of an open object, in a buffer the caller frees */
LOCAL ER rec_list( CONST T_OBMGR *m, INT h, T_OBREC **p_buf, INT *p_cnt )
{
	T_OBREC	*buf;
	ER	er;

	*p_buf = NULL;
	*p_cnt = 0;
	if ( m->lrc == NULL ) {
		return E_NOSPT;
	}
	buf = (T_OBREC *)Kmalloc(sizeof(T_OBREC) * OB_REC_LIST);
	if ( buf == NULL ) {
		return E_NOMEM;
	}
	er = m->lrc(h, buf, OB_REC_LIST, p_cnt);
	if ( er < E_OK ) {
		Kfree(buf);
		return er;
	}
	*p_buf = buf;
	return E_OK;
}

LOCAL CONST T_OBREC *rec_find( CONST T_OBREC *r, INT n, INT recno )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( r[i].recno == recno ) {
			return &r[i];
		}
	}
	return NULL;
}

EXPORT ER ob_sch_rec( ID key, INT from, UINT rt, UINT sub, UINT mask, INT *p_recno )
{
	T_OBREC	*r;
	INT	hi, n, i, best = -1;
	ER	er;

	if ( p_recno == NULL ) {
		return E_PAR;
	}
	er = key_take(key, OB_OP_ATRRD, -1, &hi, NULL);
	if ( er < E_OK ) return er;
	er = rec_list(MGR(hi), HND(hi), &r, &n);
	key_done(hi);
	if ( er < E_OK ) {
		return er;
	}
	for ( i = 0; i < n; i++ ) {
		if ( r[i].recno >= from && r[i].rt == rt && ( r[i].sub & mask ) == sub
		  && ( best < 0 || r[i].recno < best ) ) {
			best = r[i].recno;
		}
	}
	Kfree(r);
	if ( best < 0 ) {
		return E_NOEXS;
	}
	*p_recno = best;
	return E_OK;
}

/*
 * One record's bytes from one handle to another. The target has been
 * made empty (or new) by the caller, so it is written from the start.
 */
LOCAL ER rec_copy( CONST T_OBMGR *sm, INT sh, INT srec, CONST T_OBMGR *dm, INT dh,
		   INT drec, UB *buf )
{
	D	off = 0;
	SZ	got, put;
	ER	er;

	if ( sm->rea == NULL || dm->wri == NULL ) {
		return E_NOSPT;
	}
	for ( ;; ) {
		got = 0;
		er = sm->rea(sh, srec, off, buf, OB_CHUNK, &got, FALSE);
		if ( er < E_OK ) {
			return ( off > 0 && er == E_PAR ) ? E_OK : er;
		}
		if ( got <= 0 ) {
			return E_OK;
		}
		put = 0;
		er = dm->wri(dh, drec, off, buf, got, &put, FALSE);
		if ( er < E_OK ) {
			return er;
		}
		off += got;
		if ( got < OB_CHUNK ) {
			return E_OK;
		}
	}
}

/*
 * The target record for a record copied: an existing one emptied, or a
 * new one of the source's type after the last.
 */
LOCAL ER rec_target( CONST T_OBMGR *dm, INT dh, INT want, INT have, CONST T_OBREC *src,
		     INT *p_drec )
{
	if ( want >= 0 && want < have ) {
		*p_drec = want;
		return ( dm->trn != NULL ) ? dm->trn(dh, want, 0) : E_NOSPT;
	}
	if ( want >= 0 && want != have ) {
		return E_PAR;			/* a gap would be left */
	}
	return ( dm->apd != NULL ) ? dm->apd(dh, src->rt, src->sub, p_drec) : E_NOSPT;
}

EXPORT ER ob_trs_rec( ID dst, INT drec, ID src, INT srec, INT n )
{
	T_OBREC	*sr = NULL, *dr = NULL;
	UB	*buf = NULL;
	OBCHG	chg;
	INT	shi, dhi, sn = 0, dn = 0, i;
	ER	er;

	if ( n <= 0 || srec < 0 ) {
		return E_PAR;
	}
	er = key_take(src, OB_OP_READ, -1, &shi, NULL);
	if ( er < E_OK ) return er;
	er = key_take(dst, OB_OP_WRITE | OB_OP_RECORD, -1, &dhi, NULL);
	if ( er < E_OK ) {
		key_done(shi);
		return er;
	}
	if ( MGR(shi)->type == OB_T_CHANNEL || MGR(dhi)->type == OB_T_CHANNEL ) {
		er = E_NOSPT;			/* a channel's record is a message */
		goto out;
	}
	er = rec_list(MGR(shi), HND(shi), &sr, &sn);
	if ( er >= E_OK ) er = rec_list(MGR(dhi), HND(dhi), &dr, &dn);
	buf = (UB *)Kmalloc(OB_CHUNK);
	if ( er >= E_OK && buf == NULL ) er = E_NOMEM;

	for ( i = 0; i < n && er >= E_OK; i++ ) {
		CONST T_OBREC	*r = rec_find(sr, sn, srec + i);
		INT		d = 0;

		if ( r == NULL ) {
			er = E_NOEXS;
			break;
		}
		er = rec_target(MGR(dhi), HND(dhi), ( drec == OB_REC_END ) ? -1 : drec + i,
				dn, r, &d);
		if ( er < E_OK ) {
			break;
		}
		if ( d >= dn ) {
			dn = d + 1;
		}
		er = rec_copy(MGR(shi), HND(shi), r->recno, MGR(dhi), HND(dhi), d, buf);
	}
    out:
	chg_take(dhi, -1, &chg);
	key_done(shi);
	key_done(dhi);
	if ( sr != NULL ) Kfree(sr);
	if ( dr != NULL ) Kfree(dr);
	if ( buf != NULL ) Kfree(buf);
	if ( er >= E_OK ) chg_post(&chg);
	return er;
}

/* One resource copied whole: read in chunks into a buffer that grows */
LOCAL ER res_copy( CONST T_OBMGR *sm, INT sh, CONST T_OBMGR *dm, INT dh, CONST UB *name )
{
	UB	*all = NULL, *more;
	SZ	have = 0, cap = 0, got;
	ER	er;

	for ( ;; ) {
		if ( have + OB_CHUNK > cap ) {
			cap = ( cap == 0 ) ? OB_CHUNK * 4 : cap * 2;
			more = (UB *)Kmalloc(cap);
			if ( more == NULL ) {
				er = E_NOMEM;
				goto out;
			}
			if ( all != NULL ) {
				knl_memcpy(more, all, (INT)have);
				Kfree(all);
			}
			all = more;
		}
		got = 0;
		er = sm->rres(sh, name, (D)have, all + have, OB_CHUNK, &got);
		if ( er < E_OK || got <= 0 ) {
			break;
		}
		have += got;
		if ( got < OB_CHUNK ) {
			break;
		}
	}
	if ( er >= E_OK || have > 0 ) {
		er = dm->wres(dh, name, all, have);
	}
    out:
	if ( all != NULL ) Kfree(all);
	return er;
}

/* A copy's metadata: the source's, counted by nobody yet */
LOCAL INT meta_for_copy( UB *j, INT len )
{
	INT	root = knl_oj_root(j, len);

	if ( root >= 0 && knl_oj_member(j, len, root, "refCount") >= 0 ) {
		len = knl_oj_set(j, len, OB_META_MAX, root, "refCount", (CONST UB *)"0", 1);
	}
	return len;
}

EXPORT ER ob_cpy_obj( CONST TS_UUID *uuid, CONST T_OBCRE *pk_cre, TS_UUID *p_uuid )
{
	CONST T_OBMGR	*sm, *dm = NULL;
	T_OBCRE		cc;
	T_OBREF		ref;
	T_OBREC		*sr = NULL, *dr = NULL;
	UB		*meta = NULL, *buf = NULL, *names = NULL;
	SZ		mlen = 0;
	INT		sh = 0, dh = 0, sn = 0, dn = 0, i, cnt = 0, len;
	ER		er;

	if ( uuid == NULL || p_uuid == NULL ) {
		return E_PAR;
	}
	sm = knl_ob_mgr_of(uuid);
	if ( sm == NULL ) {
		return E_NOEXS;
	}
	if ( sm->type == OB_T_CHANNEL || sm->type == OB_T_PROCESS || sm->open == NULL ) {
		return E_NOSPT;
	}
	er = allowed(sm, uuid, OB_OP_READ | OB_OP_ATRRD);
	if ( er < E_OK ) {
		return er;
	}
	er = ob_ref_obj(uuid, &ref);
	if ( er < E_OK ) {
		return er;
	}
	sh = sm->open(uuid, OB_OP_READ | OB_OP_ATRRD);
	if ( sh <= 0 ) {
		return ( sh < 0 ) ? (ER)sh : E_SYS;
	}
	meta = (UB *)Kmalloc(OB_META_MAX);
	buf = (UB *)Kmalloc(OB_CHUNK);
	if ( meta == NULL || buf == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	if ( sm->gat == NULL || sm->gat(sh, meta, OB_META_MAX - 1, &mlen) < E_OK ) {
		mlen = 0;
	}
	meta[mlen] = 0;
	len = ( mlen > 0 ) ? meta_for_copy(meta, (INT)mlen) : 0;

	if ( pk_cre != NULL ) {
		cc = *pk_cre;
	} else {
		knl_memset(&cc, 0, sizeof(cc));
	}
	if ( cc.type == 0 ) {
		cc.type = ref.type;
		cc.sub = ref.sub;
	}
	if ( cc.json == NULL && len > 0 ) {
		cc.json = meta;
		cc.jsonsz = (SZ)len;
	}
	if ( cc.name == NULL && ref.name[0] != 0 ) {
		cc.name = ref.name;		/* a manager that keeps the name apart */
	}
	if ( cc.type == OB_T_STORAGE && sm->type == OB_T_STORAGE && cc.vol == NULL
	  && knl_ob_uuid_zero(&cc.near) && ref.sub == OB_S_FILE ) {
		cc.near = *uuid;		/* beside the original */
	}
	er = ob_cre_obj(&cc, p_uuid);
	if ( er < E_OK ) {
		goto out;
	}
	dm = knl_ob_mgr_of(p_uuid);
	dh = ( dm != NULL && dm->open != NULL ) ? dm->open(p_uuid, OB_OP_ALL) : E_NOEXS;
	if ( dh <= 0 ) {
		er = ( dh < 0 ) ? (ER)dh : E_SYS;
		goto undo;
	}

	/* the records, in order */
	er = rec_list(sm, sh, &sr, &sn);
	if ( er >= E_OK ) er = rec_list(dm, dh, &dr, &dn);
	for ( i = 0; i < sn && er >= E_OK; i++ ) {
		INT	d = 0;

		er = rec_target(dm, dh, i, dn, &sr[i], &d);
		if ( er >= E_OK ) {
			if ( d >= dn ) dn = d + 1;
			er = rec_copy(sm, sh, sr[i].recno, dm, dh, d, buf);
		}
	}

	/* and what is kept beside them */
	if ( er >= E_OK && sm->lres != NULL && dm->wres != NULL && sm->rres != NULL ) {
		names = (UB *)Kmalloc(OB_CHUNK);
		if ( names == NULL ) {
			er = E_NOMEM;
		} else if ( sm->lres(sh, names, OB_CHUNK, &cnt) >= E_OK ) {
			CONST UB	*nm = names;

			for ( i = 0; i < cnt && er >= E_OK; i++ ) {
				if ( !is_icon_name(nm) ) {	/* the icon goes below, as itself */
					er = res_copy(sm, sh, dm, dh, nm);
				}
				while ( *nm != 0 ) nm++;
				nm++;
			}
		}
	}
	/* and its icon, as the attributes are */
	if ( er >= E_OK && sm->gic != NULL && dm->sic != NULL ) {
		SZ	ilen = 0;
		UB	*ic;

		if ( sm->gic(sh, NULL, 0, &ilen) >= E_OK && ilen > 0 ) {
			ic = (UB *)Kmalloc(ilen);
			if ( ic == NULL ) {
				er = E_NOMEM;
			} else {
				er = sm->gic(sh, ic, ilen, &ilen);
				if ( er >= E_OK ) er = dm->sic(dh, ic, ilen);
				Kfree(ic);
			}
		}
	}
    undo:
	if ( dh > 0 && dm->close != NULL ) {
		(void)dm->close(dh);
	}
	if ( er < E_OK && dm != NULL && dm->remove != NULL ) {
		(void)dm->remove(p_uuid);
	}
    out:
	if ( sh > 0 && sm->close != NULL ) (void)sm->close(sh);
	if ( sr != NULL ) Kfree(sr);
	if ( dr != NULL ) Kfree(dr);
	if ( names != NULL ) Kfree(names);
	if ( buf != NULL ) Kfree(buf);
	if ( meta != NULL ) Kfree(meta);
	return er;
}

/* ---------------------------------------------------------------- mapping */

/*
 * A record of one object onto another. Onto a process, the manager of
 * the record is given the process's number (as -pid) and maps it into
 * that space; otherwise both must be served by the same manager, which
 * is the one that knows what placing one on the other means. The
 * target is changed, so it needs the right to change its records; the
 * one placed need only be readable, and writable when it is mapped so.
 */
LOCAL ER map_to_process( INT hi, INT recno, ID target, T_OBMAP *m )
{
	ID	pid;
	INT	thi;
	ER	er;

	if ( target == 0 ) {
		pid = ts_get_pid();		/* the caller's own */
	} else {
		er = key_take(target, OB_OP_RECORD, -1, &thi, NULL);
		if ( er < E_OK ) return er;
		pid = ( MGR(thi)->type == OB_T_PROCESS )
		    ? knl_prc_of_uuid(&(*ob_hndp[thi]).uuid) : 0;
		key_done(thi);
	}
	if ( pid <= 0 ) {
		return E_OBJ;			/* no space to map into */
	}
	if ( m != NULL ) {
		return ( MGR(hi)->map != NULL ) ? MGR(hi)->map(HND(hi), recno, -pid, m) : E_NOSPT;
	}
	return ( MGR(hi)->unm != NULL ) ? MGR(hi)->unm(HND(hi), recno, -pid) : E_NOSPT;
}

/* Whether a key names a process: the target of a mapping into a space */
LOCAL BOOL key_is_process( ID key )
{
	OBKEY	*k;
	BOOL	yes;

	LOCK();
	k = key_of(key);
	yes = ( k != NULL && ob_mgr[(*ob_hndp[k->hnd]).mgr]->type == OB_T_PROCESS );
	UNLOCK();

	return yes;
}

LOCAL ER map_by( ID key, INT recno, ID target, T_OBMAP *m )
{
	OBCHG	chg;
	INT	hi, thi;
	UINT	need = OB_OP_READ;
	ER	er;

	if ( m != NULL && ( m->flags & OB_M_WRITE ) != 0 ) {
		need |= OB_OP_WRITE;
	}
	er = key_take(key, need, recno, &hi, NULL);
	if ( er < E_OK ) return er;
	if ( target == 0 || key_is_process(target) ) {
		er = map_to_process(hi, recno, target, m);
		key_done(hi);
		return er;
	}
	er = key_take(target, OB_OP_RECORD, -1, &thi, NULL);
	if ( er < E_OK ) {
		key_done(hi);
		return er;
	}
	if ( (*ob_hndp[hi]).mgr != (*ob_hndp[thi]).mgr ) {
		er = E_NOSPT;			/* objects of two managers do not map */
	} else if ( m != NULL ) {
		er = ( MGR(hi)->map != NULL )
		   ? MGR(hi)->map(HND(hi), recno, HND(thi), m) : E_NOSPT;
	} else {
		er = ( MGR(hi)->unm != NULL )
		   ? MGR(hi)->unm(HND(hi), recno, HND(thi)) : E_NOSPT;
	}
	chg_take(thi, -1, &chg);
	key_done(hi);
	key_done(thi);
	if ( er >= E_OK ) chg_post(&chg);
	return er;
}

EXPORT ER ob_map_rec( ID key, INT recno, ID target, T_OBMAP *pk_map )
{
	return ( pk_map != NULL ) ? map_by(key, recno, target, pk_map) : E_PAR;
}

EXPORT ER ob_unm_rec( ID key, INT recno, ID target )
{
	return map_by(key, recno, target, NULL);
}

/* ---------------------------------------------------------------- notices */

/* The requests a key or a process held go with it */
LOCAL void ntf_drop( ID key, ID pid )
{
	INT	i;

	if ( ob_nntf == 0 ) {
		return;
	}
	LOCK();
	for ( i = 0; i < OB_NTF_MAX; i++ ) {
		if ( ob_ntf[i].used && ( ( key > 0 && ob_ntf[i].key == key )
				      || ( pid > 0 && ob_ntf[i].pid == pid ) ) ) {
			ntf_off(i);
		}
	}
	UNLOCK();
}

EXPORT ID ob_ntf_evt( ID key, INT recno, CONST T_OBNTF *pk_ntf, ID port )
{
	CONST T_OBMGR	*pm, *wm;
	TS_UUID		watched, to;
	UINT		need;
	INT		hi, phi, i, tries;
	ID		nid = E_LIMIT, pid = ts_get_pid();
	ER		er;

	if ( pk_ntf == NULL || ( pk_ntf->events & OB_E_ALL ) == 0 || recno < OB_REC_ANY ) {
		return E_PAR;
	}
	/* what happens in a window is seen by one who may read it; that
	   it changed or went, by one who may read its attributes */
	need = ( ( pk_ntf->events & OB_E_INPUT ) != 0 ) ? OB_OP_READ : OB_OP_ATRRD;
	er = key_take(key, need, -1, &hi, NULL);
	if ( er < E_OK ) {
		return er;
	}
	watched = (*ob_hndp[hi]).uuid;
	wm = MGR(hi);
	key_done(hi);
	if ( wm->find != NULL && wm->find(&watched) < E_OK ) {
		return E_NOEXS;			/* it has gone: nothing will be told of it */
	}

	/* the port must be one that notices can be written into */
	er = key_take(port, OB_OP_WRITE, -1, &phi, NULL);
	if ( er < E_OK ) {
		return er;
	}
	pm = MGR(phi);
	to = (*ob_hndp[phi]).uuid;
	key_done(phi);
	if ( pm->type != OB_T_CHANNEL && pm->type != OB_T_STORAGE ) {
		return E_PAR;
	}
	if ( pm->type == OB_T_STORAGE ) {
		er = key_take(port, OB_OP_RECORD, -1, &phi, NULL);
		if ( er < E_OK ) {
			return er;
		}
		key_done(phi);
	}

	LOCK();
	for ( tries = 0, i = ob_ntf_next; tries < OB_NTF_MAX; tries++, i++ ) {
		if ( i >= OB_NTF_MAX ) {
			i = 0;
		}
		if ( !ob_ntf[i].used ) {
			OBNTF	*n = &ob_ntf[i];

			n->uuid = watched;
			n->recno = recno;
			n->events = pk_ntf->events & OB_E_ALL;
			n->id = pk_ntf->id;
			n->flags = pk_ntf->flags;
			n->port = to;
			n->key = key;
			n->pid = pid;
			n->lost = 0;
			ntf_on(i);
			ob_ntf_next = i + 1;
			nid = (ID)( i + 1 );
			break;
		}
	}
	UNLOCK();

	/*
	 * Gone while it was being asked: the notice of its going was sent
	 * before this request was there to take it, so none will come.
	 */
	if ( nid > 0 && wm->find != NULL && wm->find(&watched) < E_OK ) {
		LOCK();
		if ( ob_ntf[nid - 1].used ) {
			ntf_off(nid - 1);
		}
		UNLOCK();
		return E_NOEXS;
	}
	return nid;
}

EXPORT ER ob_can_evt( ID nid )
{
	ID	pid = ts_get_pid();
	ER	er = E_ID;

	if ( nid < 1 || nid > OB_NTF_MAX ) {
		return E_ID;
	}
	LOCK();
	if ( ob_ntf[nid - 1].used ) {
		if ( ob_ntf[nid - 1].pid != pid ) {
			er = E_OACV;
		} else {
			ntf_off(nid - 1);
			er = E_OK;
		}
	}
	UNLOCK();

	return er;
}

/*
 * One notice into a port, with the manager's own calls and no leave
 * asked: the leave was given when the request was made. A channel that
 * is full is not waited on.
 */
/*
 * The manager of a port. A port is a channel or a storage object in
 * memory (18.14), so only those managers are asked: a notice is posted
 * without the store being searched, which could take the file system's
 * time at any point a notice is raised, a process ending included.
 */
LOCAL CONST T_OBMGR *port_mgr( CONST TS_UUID *uuid )
{
	INT	i;

	for ( i = 0; i < ob_nmgr; i++ ) {
		CONST T_OBMGR	*m = ob_mgr[i];

		if ( ( m->type == OB_T_CHANNEL
		       || ( m->type == OB_T_STORAGE && m->sub == OB_S_MEMORY ) )
		  && m->find != NULL && m->find(uuid) >= E_OK ) {
			return m;
		}
	}
	return NULL;
}

LOCAL ER ntf_deliver( CONST TS_UUID *port, CONST T_OBNTM *msg )
{
	CONST T_OBMGR	*m = port_mgr(port);
	SZ		asz = 0;
	INT		h, rec = 0;
	ER		er;

	if ( m == NULL || m->open == NULL || m->wri == NULL ) {
		return E_NOEXS;
	}
	h = m->open(port, OB_OP_WRITE | OB_OP_RECORD);
	if ( h <= 0 ) {
		return ( h < 0 ) ? (ER)h : E_SYS;
	}
	if ( m->type == OB_T_CHANNEL ) {
		er = m->wri(h, 0, 0, msg, sizeof(*msg), &asz, TRUE);
	} else {
		er = ( m->apd != NULL ) ? m->apd(h, OB_RT_SYSDATA, 0, &rec) : E_NOSPT;
		if ( er >= E_OK ) {
			er = m->wri(h, rec, 0, msg, sizeof(*msg), &asz, FALSE);
		}
	}
	if ( m->close != NULL ) {
		(void)m->close(h);
	}
	return er;
}

EXPORT BOOL knl_ob_taker( CONST TS_UUID *uuid, UINT event, ID *p_pid )
{
	BOOL	yes = FALSE;
	INT	i;

	if ( ob_nntf == 0 || ob_mtx <= 0 || uuid == NULL ) {
		return FALSE;
	}
	LOCK();
	for ( i = ob_ntf_chain[ntf_hash(uuid)] - 1; i >= 0 && !yes; i = ob_ntf[i].next - 1 ) {
		if ( ob_ntf[i].used && ( ob_ntf[i].events & event ) != 0
		  && same_uuid(&ob_ntf[i].uuid, uuid) ) {
			yes = TRUE;
			if ( p_pid != NULL ) {
				*p_pid = ob_ntf[i].pid;
			}
		}
	}
	UNLOCK();

	return yes;
}

#define NTF_BATCH	8		/* requests answered by one happening */

EXPORT void knl_ob_post( CONST TS_UUID *uuid, INT recno, UINT event, CONST T_OBNTM *n )
{
	knl_ob_post_as(uuid, uuid, recno, event, n);
}

EXPORT void knl_ob_post_as( CONST TS_UUID *watched, CONST TS_UUID *uuid, INT recno,
			    UINT event, CONST T_OBNTM *n )
{
	struct {
		TS_UUID	port;
		ID	nid;
		UINT	id;
		UINT	lost;
	} to[NTF_BATCH];
	SYSTIM	now;
	INT	i, cnt = 0, next;

	if ( ob_nntf == 0 || ob_mtx <= 0 || watched == NULL || uuid == NULL ) {
		return;
	}
	LOCK();
	for ( i = ob_ntf_chain[ntf_hash(watched)] - 1, next = -1; i >= 0 && cnt < NTF_BATCH; i = next ) {
		OBNTF	*r = &ob_ntf[i];

		next = r->next - 1;		/* before a request for once is taken off */
		if ( !r->used || ( r->events & event ) == 0 || !same_uuid(&r->uuid, watched) ) {
			continue;
		}
		if ( r->recno != OB_REC_ANY && recno != OB_REC_ANY && r->recno != recno ) {
			continue;
		}
		to[cnt].port = r->port;
		to[cnt].nid = (ID)( i + 1 );
		to[cnt].id = r->id;
		to[cnt].lost = r->lost;
		r->lost = 0;
		if ( ( r->flags & OB_N_ONCE ) != 0 ) {
			ntf_off(i);
		}
		cnt++;
	}
	UNLOCK();

	if ( cnt == 0 ) {
		return;
	}
	now.hi = 0;
	now.lo = 0;
	if ( n == NULL ) {
		(void)tk_get_otm(&now);
	}
	for ( i = 0; i < cnt; i++ ) {
		T_OBNTM	msg;

		if ( n != NULL ) {
			msg = *n;
		} else {
			knl_memset(&msg, 0, sizeof(msg));
			msg.when = ( (UD)(UW)now.hi << 32 ) | (UD)now.lo;
		}
		msg.id = to[i].id;
		msg.nid = to[i].nid;
		msg.uuid = *uuid;
		msg.recno = recno;
		msg.event = event;
		msg.lost = to[i].lost;

		/*
		 * A notice that does not go in is counted against the
		 * request, and the count rides on the next one that does.
		 * A move or a redraw is not counted: the next one says the
		 * same thing newer.
		 */
		if ( ntf_deliver(&to[i].port, &msg) < E_OK
		  && ( event & ( OB_E_MOVE | OB_E_REDRAW ) ) == 0 ) {
			LOCK();
			if ( ob_ntf[to[i].nid - 1].used ) {
				ob_ntf[to[i].nid - 1].lost += 1 + msg.lost;
			}
			UNLOCK();
		}
	}
}

/* ---------------------------------------------------------------- protection */

EXPORT ER ob_get_prt( CONST TS_UUID *uuid, T_OBPRT *pk_prt )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);

	if ( pk_prt == NULL ) {
		return E_PAR;
	}
	if ( m == NULL ) {
		return E_NOEXS;
	}
	prot_of(m, uuid, pk_prt);

	return E_OK;
}

/*
 * The protection changed. The owner may change it but not give the
 * object away, and may name only a group of its own; an administrator
 * may do anything.
 */
EXPORT ER ob_set_prt( CONST TS_UUID *uuid, CONST T_OBPRT *pk_prt )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);
	CONST T_OBCRD	*crd = knl_ob_crd_of(ts_get_pid());
	T_OBPRT		*cur;
	ER		er;

	if ( pk_prt == NULL ) {
		return E_PAR;
	}
	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->setprot == NULL ) {
		return E_NOSPT;
	}
	cur = (T_OBPRT *)Kmalloc(sizeof(T_OBPRT));
	if ( cur == NULL ) {
		return E_NOMEM;
	}
	prot_of(m, uuid, cur);
	er = E_OK;
	if ( ( knl_ob_permit(crd, cur, -1) & OB_OP_PROT ) == 0 ) {
		er = E_OACV;
	} else if ( !knl_ob_is_admin(crd) ) {
		INT	g;
		BOOL	mine = knl_ob_uuid_zero(&pk_prt->group);

		if ( !same_uuid(&pk_prt->owner, &cur->owner) ) {
			er = E_OACV;
		}
		for ( g = 0; g < crd->ngrp && !mine; g++ ) {
			mine = same_uuid(&pk_prt->group, &crd->grp[g])
			    || same_uuid(&pk_prt->group, &cur->group);
		}
		if ( !mine ) {
			er = E_OACV;
		}
	}
	Kfree(cur);
	if ( er >= E_OK ) {
		er = m->setprot(uuid, pk_prt);
	}
	return er;
}

/* ---------------------------------------------------------------- the kernel's own */

/* The metadata of an object, read without asking anyone's leave */
EXPORT ER knl_ob_meta_read( CONST TS_UUID *uuid, UB *buf, INT max, INT *p_len )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);
	SZ		asz = 0;
	INT		h;
	ER		er;

	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->open == NULL || m->gat == NULL ) {
		return E_NOSPT;
	}
	h = m->open(uuid, OB_OP_ATRRD);
	if ( h <= 0 ) {
		return ( h < 0 ) ? (ER)h : E_SYS;
	}
	er = m->gat(h, buf, (SZ)max - 1, &asz);
	if ( m->close != NULL ) (void)m->close(h);
	if ( er >= E_OK ) {
		buf[asz] = 0;
		*p_len = (INT)asz;
	}
	return er;
}

EXPORT ER knl_ob_meta_write( CONST TS_UUID *uuid, CONST UB *buf, INT len )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);
	INT		h;
	ER		er;

	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->open == NULL || m->sat == NULL ) {
		return E_NOSPT;
	}
	h = m->open(uuid, OB_OP_ATRWR);
	if ( h <= 0 ) {
		return ( h < 0 ) ? (ER)h : E_SYS;
	}
	er = m->sat(h, buf, (SZ)len);
	if ( m->close != NULL ) (void)m->close(h);

	return er;
}

/* ---------------------------------------------------------------- transactions */

EXPORT ER ob_beg_trx( CONST TS_UUID *on )
{
	return ( on != NULL ) ? knl_obfile_trx(on, TRUE, FALSE) : E_PAR;
}

EXPORT ER ob_end_trx( CONST TS_UUID *on, BOOL commit )
{
	return ( on != NULL ) ? knl_obfile_trx(on, FALSE, commit) : E_PAR;
}

/* ---------------------------------------------------------------- volumes */

EXPORT ER ob_ref_vol( CONST TS_UUID *near, T_OBVOL *pk_vol )
{
	CONST T_OBMGR	*m;
	T_RVOL		rv;
	ID		vol;
	ER		er;

	if ( pk_vol == NULL ) {
		return E_PAR;
	}
	if ( near == NULL || knl_ob_uuid_zero(near) ) {
		vol = knl_obfile_first_vol();
	} else {
		m = knl_ob_mgr_of(near);
		if ( m == NULL ) {
			return E_NOEXS;
		}
		if ( m->type != OB_T_STORAGE || m->sub != OB_S_FILE ) {
			return E_NOSPT;
		}
		if ( ( knl_ob_permit_on(near, knl_ob_crd_of(ts_get_pid())) & OB_OP_ATRRD ) == 0 ) {
			return E_OACV;
		}
		vol = knl_obfile_vol_of(near);
	}
	if ( vol <= 0 ) {
		return ( vol < 0 ) ? (ER)vol : E_NOEXS;
	}
	er = ts_ref_vol(vol, &rv);
	if ( er < E_OK ) {
		return er;
	}
	pk_vol->blocks = rv.blocks;
	pk_vol->bfree = rv.bfree;
	pk_vol->bsize = (UINT)rv.bsize;
	pk_vol->nobj = rv.nobj;
	return E_OK;
}

EXPORT ER ob_att_vol( CONST char *path, UINT flags )
{
	ER	er;

	if ( !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		return E_OACV;
	}
	er = knl_obfile_attach(path, flags);
	if ( er >= E_OK ) {
		knl_dt_zone_load();		/* the calendar may be on it */
	}
	return er;
}

EXPORT ER ob_det_vol( CONST char *path )
{
	if ( !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		return E_OACV;
	}
	return knl_obfile_detach(path);
}

EXPORT ER ob_get_dom( CONST char *vol, T_OBPRT *pk_prt )
{
	return ( vol != NULL && pk_prt != NULL ) ? knl_obfile_get_dom(vol, pk_prt) : E_PAR;
}

EXPORT ER ob_set_dom( CONST char *vol, CONST T_OBPRT *pk_prt )
{
	if ( vol == NULL ) {
		return E_PAR;
	}
	if ( !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		return E_OACV;
	}
	return knl_obfile_set_dom(vol, pk_prt);
}

/* ---------------------------------------------------------------- start-up */

/*
 * The name manager and the managers under it. The order is the order
 * in which an object's owner is looked for: files first, since most
 * objects are, then what lives in memory.
 */
EXPORT ER knl_ob_init( void )
{
	T_CMTX	cmtx;
	ER	er;

	if ( ob_mtx > 0 ) {
		return E_OK;
	}
	cmtx.exinf = NULL;
	cmtx.mtxatr = TA_TFIFO;
	ob_mtx = tk_cre_mtx(&cmtx);
	if ( ob_mtx <= 0 ) {
		return (ER)ob_mtx;
	}
	er = knl_obfile_init();
	if ( er >= E_OK ) er = knl_obmem_init();
	if ( er >= E_OK ) er = knl_obdev_init();
	if ( er >= E_OK ) er = knl_obprc_init();
	if ( er >= E_OK ) er = knl_obchan_init();
	if ( er >= E_OK ) er = knl_obsock_init();
	if ( er < E_OK ) {
		tm_printf((UB *)"ob: a manager did not start (%d)\n", (INT)er);
		return er;
	}
	/* the volume the system's objects are on: the native system volume
	   when the machine has one, else the store on the boot file system */
	er = E_NOEXS;
#ifdef CNF_OB_SYSVOL
	er = knl_obfile_attach(CNF_OB_SYSVOL, TSFS_STORE_BLK);
	if ( er >= E_OK ) {
		tm_printf((UB *)"ob: the system volume is %s\n", CNF_OB_SYSVOL);
	}
#endif
#ifdef CNF_OB_STORE
	if ( er < E_OK ) {
		(void)knl_obfile_attach(CNF_OB_STORE, 0);
	}
#endif
	knl_dt_zone_load();			/* the zone the calendar keeps */
	knl_obdev_start();			/* the devices outside the device management */
	return E_OK;
}
