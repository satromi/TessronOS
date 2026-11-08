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
#include <ts/gpt.h>
#include <ts/proc.h>
#include <ts/tsfs.h>
#include <ts/dt.h>
#include <ts/tsfsck.h>
#include <ts/tsfsjrnl.h>
#include <ts/uuid.h>
#include <ts/conf.h>
#include <ts/om.h>
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
	T_OBPRT	*prt;			/* the protection as it was opened, the handle's own */
} OBHND;

typedef struct {
	BOOL	used;
	INT	hnd;
	ID	pid;			/* who holds it; 0 the kernel */
	ID	tsk;			/* the kernel's: the task that opened it */
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
EXPORT CONST TS_UUID ob_user_person = OB_USER_PERSON_INIT;
EXPORT CONST TS_UUID ob_group_users = OB_GROUP_USERS_INIT;
EXPORT CONST TS_UUID ob_user_admin = OB_USER_ADMIN_INIT;

/* The kernel's own tasks, and init: the system, an administrator */
EXPORT CONST T_OBCRD knl_ob_crd_system = {
	OB_USER_SYSTEM_INIT, 1, { OB_GROUP_ADMIN_INIT }
};

/* The ordinary user 利用者: what a program from the desktop acts as when no one logged in */
EXPORT CONST T_OBCRD knl_ob_crd_person = {
	OB_USER_PERSON_INIT, 1, { OB_GROUP_USERS_INIT }
};

/* A process whose credentials were lost: nobody, in no group */
LOCAL CONST T_OBCRD ob_crd_nobody;

/*
 * The person logged in at the desktop (knl_ob_session), as a kept set
 * without the administrators' group; NULL: no one, and the programs
 * the desktop starts act as 利用者.
 */
LOCAL CONST T_OBCRD	*ob_session = NULL;

/*
 * The processes acting as administrators for a while (knl_ob_adm_give):
 * what each acted as before, given back by ob_end_adm, when the time is
 * up, or when the process ends. A few at a time at most -- a person
 * types the password for each -- so a short table, looked at only when
 * it has something in it.
 */
#define OB_ADM_MAX	16

typedef struct {
	ID		pid;		/* 0: free */
	CONST T_OBCRD	*base;		/* what it acted as before */
	UD		until;		/* the system time (ms) the rights go at */
} OBADM;

LOCAL OBADM	ob_adm[OB_ADM_MAX];
LOCAL INT	ob_nadm = 0;

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

/*
 * The protection an object has, in memory of its own (Kfree it); one
 * whose manager keeps none lets all through. NULL when there is no
 * memory for it.
 */
LOCAL T_OBPRT *prot_of( CONST T_OBMGR *m, CONST TS_UUID *uuid )
{
	T_OBPRT	*prt = NULL;

	if ( m->prot == NULL || m->prot(uuid, &prt) < E_OK || prt == NULL ) {
		if ( prt != NULL ) Kfree(prt);
		prt = knl_ob_prt_new(0, 0);
		if ( prt != NULL ) {
			knl_ob_prt_legacy(prt, TRUE, TRUE, TRUE);
		}
	}
	return prt;
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
	dom = knl_obfile_domain(uuid);
	if ( dom != NULL ) {
		ops &= knl_ob_permit(crd, dom, -1) | OB_OP_PROT;
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
	prt = prot_of(m, uuid);
	if ( prt != NULL ) {
		ops = permit_on(m, uuid, crd, prt);
		Kfree(prt);
	}
	return ops;
}

#ifdef OB_DENY_LOG
/*
 * A refusal told on the console (make UITEST=1 or OACVLOG=1), for
 * finding what a program the person starts is not let do: which
 * process, which object, what it asked and what it may. A line is not
 * said again while it is one of the last few said; the few are kept
 * without a lock, so that two processes refused at once may each have
 * their line said twice, which a tool for looking does not mind.
 */
#define DENY_SEEN	16
LOCAL struct {
	ID	pid;
	TS_UUID	uuid;
	UINT	want;
	CONST char *at;
}	deny_seen[DENY_SEEN];
LOCAL UINT	deny_next = 0;

/* The "name" of an object's metadata, or nothing */
LOCAL void deny_obj_name( CONST TS_UUID *u, UB *name, INT max )
{
	UB	*j = (UB *)Kmalloc(1024);
	INT	len = 0, i, n = 0;

	name[0] = 0;
	if ( j == NULL ) {
		return;
	}
	if ( knl_ob_meta_read(u, j, 1024, &len) >= E_OK ) {
		for ( i = 0; i + 7 < len; i++ ) {
			if ( j[i] == '"' && j[i + 1] == 'n' && j[i + 2] == 'a' && j[i + 3] == 'm'
			  && j[i + 4] == 'e' && j[i + 5] == '"' ) {
				i += 6;
				while ( i < len && j[i] != '"' ) i++;
				for ( i++; i < len && j[i] != '"' && n < max - 1; i++ ) {
					name[n++] = j[i];
				}
				break;
			}
		}
	}
	name[n] = 0;
	Kfree(j);
}

EXPORT void knl_ob_deny_note( CONST char *at, CONST TS_UUID *u, UINT want, UINT may )
{
	CONST T_OBMGR	*m;
	ID		pid = ts_get_pid();
	UB		*who, *what;
	char		us[40];
	UINT		i;

	if ( pid <= 0 ) {
		return;				/* the kernel's own: refused on purpose or a fault of its own */
	}
	for ( i = 0; i < DENY_SEEN; i++ ) {
		if ( deny_seen[i].pid == pid && deny_seen[i].want == want && deny_seen[i].at == at
		  && ( u == NULL || same_uuid(&deny_seen[i].uuid, u) ) ) {
			return;
		}
	}
	i = deny_next++ % DENY_SEEN;
	deny_seen[i].pid = pid;
	deny_seen[i].want = want;
	deny_seen[i].at = at;
	if ( u != NULL ) deny_seen[i].uuid = *u;

	who = (UB *)Kmalloc(64 + 64);
	if ( who == NULL ) {
		return;
	}
	what = who + 64;
	(void)knl_prc_name(pid, who, 64);
	us[0] = 0;
	what[0] = 0;
	m = ( u != NULL ) ? knl_ob_mgr_of(u) : NULL;
	if ( u != NULL ) {
		(void)ts_uuid_to_str(u, us, sizeof(us));
		if ( m != NULL && m->type == OB_T_STORAGE && m->sub == OB_S_FILE ) {
			deny_obj_name(u, what, 64);
		}
	}
	if ( u == NULL ) {
		tm_printf((UB *)"!OACV! pid=%d \"%s\" %s want=0x%x may=0x%x\n", (INT)pid, who, at, want, may);
	} else {
		tm_printf((UB *)"!OACV! pid=%d \"%s\" %s obj=%s type=%d.%d \"%s\" want=0x%x may=0x%x\n",
			  (INT)pid, who, at, us, ( m != NULL ) ? (INT)m->type : 0,
			  ( m != NULL ) ? (INT)m->sub : 0, what, want, may);
	}
	Kfree(who);
}
#else
EXPORT void knl_ob_deny_note( CONST char *at, CONST TS_UUID *u, UINT want, UINT may )
{
	(void)at;  (void)u;  (void)want;  (void)may;
}
#endif

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

/* The system time in milliseconds, for the administrator's rights given for a while */
LOCAL UD now_ms( void )
{
	SYSTIM	t;

	if ( tk_get_otm(&t) < E_OK ) {
		return 0;
	}
	return ( (UD)(UW)t.hi << 32 ) | t.lo;
}

/*
 * What an administrator for a while acted as before, once the time is
 * up: NULL while the rights hold, or when pid has none. Read without
 * the lock -- a slot is filled before its pid is set and its pid
 * cleared before it is used again -- since the credentials are asked
 * for under the lock as well.
 */
LOCAL CONST T_OBCRD *adm_lapsed( ID pid )
{
	INT	i;

	if ( __atomic_load_n(&ob_nadm, __ATOMIC_ACQUIRE) == 0 ) {
		return NULL;
	}
	for ( i = 0; i < OB_ADM_MAX; i++ ) {
		if ( __atomic_load_n(&ob_adm[i].pid, __ATOMIC_ACQUIRE) == pid ) {
			return ( now_ms() >= ob_adm[i].until ) ? ob_adm[i].base : NULL;
		}
	}
	return NULL;
}

EXPORT CONST T_OBCRD *knl_ob_crd_of( ID pid )
{
	CONST T_OBCRD	*c, *was;

	if ( pid <= 0 ) {
		return &knl_ob_crd_system;
	}
	c = ( pid < CNF_PID_MAX ) ? ob_crd_pid[pid] : NULL;
	if ( c != NULL && ( was = adm_lapsed(pid) ) != NULL ) {
		c = was;
	}

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

/* The slot of pid among the administrators for a while, or -1. Under the lock. */
LOCAL INT adm_slot( ID pid )
{
	INT	i;

	for ( i = 0; i < OB_ADM_MAX && ob_nadm > 0; i++ ) {
		if ( ob_adm[i].pid == pid ) {
			return i;
		}
	}
	return -1;
}

/* The rights of slot i given back: the process acts as it did. Under the lock. */
LOCAL void adm_drop( INT i, BOOL restore )
{
	ID	pid = ob_adm[i].pid;

	if ( restore && pid > 0 && pid < CNF_PID_MAX && ob_crd_pid[pid] != NULL ) {
		ob_crd_pid[pid] = ob_adm[i].base;
	}
	__atomic_store_n(&ob_adm[i].pid, 0, __ATOMIC_RELEASE);
	ob_nadm--;
}

/* What pid acts as, the administrator's rights for a while left out. Under the lock. */
LOCAL CONST T_OBCRD *crd_base( ID pid )
{
	INT	i = adm_slot(pid);

	return ( i >= 0 ) ? ob_adm[i].base : knl_ob_crd_of(pid);
}

/*
 * A start a person asked for: while it is armed, the process the arming
 * task makes is marked with the ticket before its first instruction can
 * run, so that its first window, however soon, finds it.
 */
LOCAL ID	ob_act_tsk;
LOCAL UINT	ob_act_arm;
LOCAL UINT	ob_act_pid[CNF_PID_MAX];

EXPORT void knl_ob_act_arm( UINT ticket )
{
	if ( ob_mtx <= 0 ) {
		return;
	}
	LOCK();
	ob_act_tsk = ( ticket != 0 ) ? tk_get_tid() : 0;
	ob_act_arm = ticket;
	UNLOCK();
}

EXPORT UINT knl_ob_act_take( ID pid )
{
	UINT	t = 0;

	if ( ob_mtx <= 0 || pid <= 0 || pid >= CNF_PID_MAX ) {
		return 0;
	}
	LOCK();
	t = ob_act_pid[pid];
	ob_act_pid[pid] = 0;
	UNLOCK();
	return t;
}

/*
 * A new process acts as its parent did, without the administrator's
 * rights the parent may have for a while: they were given for what the
 * parent was doing, not for what it starts. One the kernel starts acts
 * as the system, or for the person at the machine when 'person' says so
 * (TS_PRC_USER): the user logged in at the desktop, or 利用者.
 */
EXPORT void knl_ob_prc_start( ID pid, ID ppid, BOOL person )
{
	if ( ob_mtx <= 0 || pid <= 0 || pid >= CNF_PID_MAX ) {
		return;
	}
	LOCK();
	ob_act_pid[pid] = ( ob_act_tsk != 0 && ob_act_tsk == tk_get_tid() ) ? ob_act_arm : 0;
	if ( ppid > 0 ) {
		ob_crd_pid[pid] = crd_base(ppid);
	} else if ( person ) {
		ob_crd_pid[pid] = ( ob_session != NULL ) ? ob_session : &knl_ob_crd_person;
	} else {
		ob_crd_pid[pid] = &knl_ob_crd_system;
	}
	UNLOCK();
}

/*
 * The person logged in at the desktop: the programs the desktop starts
 * from now on act as crd (NULL: as 利用者). The administrators' group is
 * left out, so that a program does not act as an administrator because
 * the person is one; it asks for the rights when it needs them, as any
 * program does (DT_RQ_ADMIN).
 */
EXPORT ER knl_ob_session( CONST T_OBCRD *crd )
{
	T_OBCRD		*s;
	CONST T_OBCRD	*c = NULL;
	INT		g;

	if ( crd == NULL ) {
		LOCK();
		ob_session = NULL;
		UNLOCK();
		return E_OK;
	}
	s = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD));
	if ( s == NULL ) {
		return E_NOMEM;
	}
	knl_memset(s, 0, sizeof(*s));
	s->user = crd->user;
	for ( g = 0; g < crd->ngrp && g < CNF_OB_MAX_GROUPS; g++ ) {
		if ( !same_uuid(&crd->grp[g], &ob_group_admin) ) {
			s->grp[s->ngrp++] = crd->grp[g];
		}
	}
	LOCK();
	if ( !same_uuid(&s->user, &ob_user_system) ) {
		c = crd_intern(s);
		if ( c != NULL ) {
			ob_session = c;
		}
	}
	UNLOCK();
	Kfree(s);

	return ( c != NULL ) ? E_OK : same_uuid(&crd->user, &ob_user_system) ? E_PAR : E_NOMEM;
}

/*
 * The kernel's tasks that make objects for the person at the machine:
 * the desktop's, which make documents as the person works them. What
 * such a task makes with no protection given is the person's
 * (ob_cre_obj); what the other tasks of the kernel make is the system's.
 */
#define OB_MAKER_MAX	8
LOCAL ID	ob_maker_tsk[OB_MAKER_MAX];

EXPORT void knl_ob_make_for_person( BOOL on )
{
	ID	me = tk_get_tid();
	INT	i, free = -1;

	for ( i = 0; i < OB_MAKER_MAX; i++ ) {
		if ( ob_maker_tsk[i] == me ) {
			if ( !on ) ob_maker_tsk[i] = 0;
			return;
		}
		if ( ob_maker_tsk[i] == 0 && free < 0 ) free = i;
	}
	if ( on && free >= 0 ) {
		ob_maker_tsk[free] = me;
	}
}

LOCAL BOOL maker_for_person( void )
{
	ID	me = tk_get_tid();
	INT	i;

	for ( i = 0; i < OB_MAKER_MAX; i++ ) {
		if ( ob_maker_tsk[i] == me ) return TRUE;
	}
	return FALSE;
}

/* What the programs the desktop starts act as now */
EXPORT CONST T_OBCRD *knl_ob_session_crd( void )
{
	CONST T_OBCRD	*c = ob_session;

	return ( c != NULL ) ? c : &knl_ob_crd_person;
}

/*
 * Process pid acts as an administrator for ms milliseconds (at most
 * OB_ADM_MS), or until it gives the rights back (ob_end_adm) or ends:
 * the credentials it has, with the administrators' group. Given only by
 * the kernel, once the administrator's password was typed for it.
 * Asked again while it has them, the time starts again.
 */
EXPORT ER knl_ob_adm_give( ID pid, UINT ms )
{
	T_OBCRD		*a;
	CONST T_OBCRD	*base, *c;
	INT		i, g;
	ER		er = E_OK;

	if ( pid <= 0 ) {
		return E_OK;			/* the kernel is an administrator */
	}
	if ( pid >= CNF_PID_MAX ) {
		return E_NOEXS;
	}
	if ( ms == 0 || ms > OB_ADM_MS ) {
		ms = OB_ADM_MS;
	}
	a = (T_OBCRD *)Kmalloc(sizeof(T_OBCRD));
	if ( a == NULL ) {
		return E_NOMEM;
	}
	LOCK();
	/* a slot whose time is up is given back first */
	for ( i = 0; i < OB_ADM_MAX && ob_nadm > 0; i++ ) {
		if ( ob_adm[i].pid > 0 && now_ms() >= ob_adm[i].until ) {
			adm_drop(i, TRUE);
		}
	}
	if ( ob_crd_pid[pid] == NULL ) {
		er = E_NOEXS;
		goto out;
	}
	i = adm_slot(pid);
	if ( i >= 0 ) {
		ob_adm[i].until = now_ms() + ms;
		goto out;
	}
	base = ob_crd_pid[pid];
	if ( knl_ob_is_admin(base) ) {
		goto out;			/* one already */
	}
	*a = *base;
	if ( a->ngrp >= CNF_OB_MAX_GROUPS ) {
		a->ngrp = CNF_OB_MAX_GROUPS - 1;
	}
	for ( g = a->ngrp; g > 0; g-- ) {
		a->grp[g] = a->grp[g - 1];
	}
	a->grp[0] = ob_group_admin;
	a->ngrp++;
	for ( i = 0; i < OB_ADM_MAX && ob_adm[i].pid != 0; i++ ) ;
	if ( i >= OB_ADM_MAX ) {
		er = E_LIMIT;
		goto out;
	}
	c = crd_intern(a);
	if ( c == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	ob_adm[i].base = base;
	ob_adm[i].until = now_ms() + ms;
	ob_nadm++;
	__atomic_store_n(&ob_adm[i].pid, pid, __ATOMIC_RELEASE);
	ob_crd_pid[pid] = c;
    out:
	UNLOCK();
	Kfree(a);

	return er;
}

/* Whether pid acts as an administrator for a while now */
EXPORT BOOL knl_ob_adm_held( ID pid )
{
	BOOL	held;

	if ( pid <= 0 || pid >= CNF_PID_MAX || ob_mtx <= 0 ) {
		return FALSE;
	}
	LOCK();
	held = (BOOL)( adm_slot(pid) >= 0 && adm_lapsed(pid) == NULL );
	UNLOCK();

	return held;
}

EXPORT ER ob_end_adm( void )
{
	ID	pid = ts_get_pid();
	INT	i;

	if ( pid <= 0 || ob_mtx <= 0 ) {
		return E_OK;
	}
	LOCK();
	i = adm_slot(pid);
	if ( i >= 0 ) {
		adm_drop(i, TRUE);
	}
	UNLOCK();

	return E_OK;
}

LOCAL ER hnd_release( INT hi );
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

/*
 * The keys the kernel's tasks tids opened, closed: the desktop's tasks
 * were stopped where they stood and cannot close them themselves
 * (fquit.c). An object a key kept open is let go with it.
 */
EXPORT INT knl_ob_tsk_end( CONST ID *tids, INT ntid )
{
	INT	i, k, c = 0;

	if ( ob_mtx <= 0 || ntid <= 0 ) {
		return 0;
	}
	/* what they made was the person's (knl_ob_make_for_person): no more */
	for ( i = 0; i < OB_MAKER_MAX; i++ ) {
		for ( k = 0; k < ntid; k++ ) {
			if ( ob_maker_tsk[i] == tids[k] ) {
				ob_maker_tsk[i] = 0;
			}
		}
	}
	for ( i = 0; i < OB_KEY_MAX; ) {
		INT	his[16], n = 0, j;
		ID	keys[16];

		LOCK();
		for ( ; i < OB_KEY_MAX && n < 16; i++ ) {
			if ( !ob_key[i].used || ob_key[i].pid != 0 ) {
				continue;
			}
			for ( k = 0; k < ntid; k++ ) {
				if ( ob_key[i].tsk == tids[k] ) {
					break;
				}
			}
			if ( k == ntid ) {
				continue;
			}
			his[n] = ob_key[i].hnd;
			keys[n] = (ID)( ( ( (*ob_hndp[ob_key[i].hnd]).mgr + 1 ) << KEY_MGR_SHIFT ) | ( i + 1 ) );
			n++;
			ob_key[i].used = FALSE;
		}
		UNLOCK();
		for ( j = 0; j < n; j++ ) {
			ntf_drop(keys[j], 0);
			hnd_release(his[j]);
		}
		c += n;
	}
	return c;
}

#if USE_KTEST
/* The objects' lock held by a test, for a process's call to wait on */
EXPORT void knl_ob_test_hold( BOOL on )
{
	if ( on ) {
		LOCK();
	} else {
		UNLOCK();
	}
}
#endif

EXPORT void knl_ob_prc_gone( ID pid )
{
	if ( ob_mtx <= 0 || pid <= 0 || pid >= CNF_PID_MAX ) {
		return;
	}
	LOCK();
	if ( adm_slot(pid) >= 0 ) {
		adm_drop(adm_slot(pid), FALSE);
	}
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
		if ( adm_slot(pid) >= 0 ) {
			adm_drop(adm_slot(pid), FALSE);	/* a login is what it acts as from now */
		}
		ob_crd_pid[pid] = c;
	}
	UNLOCK();

	return er;
}

/* ---------------------------------------------------------------- keys */

/* A handle made, holding prt from now on (it frees it when it closes); -1 none */
LOCAL INT hnd_new( INT mgr, INT h, CONST TS_UUID *uuid, T_OBPRT *prt )
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
			o->prt = prt;
			ob_hnd_next = i + 1;
			return i;
		}
	}
	return -1;
}

/*
 * The entries of the table of opened objects made so far, and the bytes
 * of one: an entry is made the first time its place comes round and is
 * kept, so the table grows by itself up to OB_HND_MAX entries.
 */
EXPORT INT knl_ob_hnd_made( SZ *p_each )
{
	INT	i, n = 0;

	LOCK();
	for ( i = 0; i < OB_HND_MAX; i++ ) {
		if ( ob_hndp[i] != NULL ) n++;
	}
	UNLOCK();
	if ( p_each != NULL ) *p_each = (SZ)sizeof(OBHND);
	return n;
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
			ob_key[i].tsk = ( pid == 0 ) ? tk_get_tid() : 0;
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
/*
 * A hold of a handle given back; the last one closes it with its
 * manager, whose error comes back (a store that makes sure at closing
 * that what was written is on the medium says when it is not)
 */
LOCAL ER hnd_release( INT hi )
{
	CONST T_OBMGR	*m = NULL;
	T_OBPRT		*prt = NULL;
	INT		h = 0;

	LOCK();
	if ( --(*ob_hndp[hi]).refs <= 0 ) {
		m = ob_mgr[(*ob_hndp[hi]).mgr];
		h = (*ob_hndp[hi]).h;
		prt = (*ob_hndp[hi]).prt;
		(*ob_hndp[hi]).prt = NULL;
		(*ob_hndp[hi]).used = FALSE;
	}
	UNLOCK();
	if ( prt != NULL ) {
		Kfree(prt);
	}
	if ( m != NULL && m->close != NULL ) {
		return m->close(h);
	}
	return E_OK;
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

		for ( i = 0; o->prt != NULL && i < o->prt->nrmask; i++ ) {
			if ( OB_PRT_RMASK(o->prt)[i].recno == recno ) {
				UINT	lim = OB_PRT_RMASK(o->prt)[i].ops;

				/* a limit that names x: only a key that may control the object */
				if ( ( lim & OB_OP_EXEC ) != 0 && ( ops & OB_OP_EXEC ) == 0 ) lim = 0;
				ops = ( ops & ~onrec ) | ( ops & lim & onrec );
				break;
			}
		}
	}
	if ( ( ops & need ) != need ) {
		TS_UUID	u = o->uuid;

		UNLOCK();
		knl_ob_deny_note(( recno >= 0 ) ? "record" : "key", &u, need, ops);
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
	UINT		excl = ops & OB_O_EXCLS, allowed;
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
	prt = prot_of(m, uuid);
	if ( prt == NULL ) {
		return E_NOMEM;
	}
	allowed = permit_on(m, uuid, knl_ob_crd_of(pid), prt);
	if ( ( ops & ~allowed ) != 0 && pid > 0 ) {
		allowed |= granted(uuid, pid);
	}
	if ( ( ops & ~allowed ) != 0 ) {
		Kfree(prt);
		knl_ob_deny_note("open", uuid, ops, allowed);
		return E_OACV;
	}
	h = m->open(uuid, ops | excl);		/* the exclusive mode for the manager that keeps it */
	if ( h <= 0 ) {
		Kfree(prt);
		return ( h < 0 ) ? (ER)h : E_SYS;
	}
	LOCK();
	hi = hnd_new(mi, h, uuid, prt);
	key = ( hi >= 0 ) ? key_new(hi, pid, ops, nowait) : E_LIMIT;
	if ( hi >= 0 && key < 0 ) {
		(*ob_hndp[hi]).prt = NULL;
		(*ob_hndp[hi]).used = FALSE;
	}
	UNLOCK();
	if ( hi < 0 || key < 0 ) {
		Kfree(prt);			/* the handle did not take it */
	}
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

	return hnd_release(hi);
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

/* How many bytes record recno of the open handle holds, from its manager's list */
LOCAL ER rec_bytes( INT hi, INT recno, SZ *p_size )
{
	T_OBREC	*r;
	INT	cnt = 0;
	ER	er;

	if ( recno < 0 || MGR(hi)->lrc == NULL ) {
		return E_NOSPT;
	}
	r = (T_OBREC *)Kmalloc(sizeof(T_OBREC) * (SZ)( recno + 1 ));
	if ( r == NULL ) {
		return E_NOMEM;
	}
	er = MGR(hi)->lrc(HND(hi), r, recno + 1, &cnt);
	if ( er >= E_OK ) {
		if ( cnt > recno && r[recno].recno == recno ) {
			*p_size = (SZ)r[recno].size;
		} else {
			er = E_NOEXS;
		}
	}
	Kfree(r);
	return er;
}

EXPORT ER ob_rea_rec( ID key, INT recno, D off, void *buf, SZ size, SZ *p_asize )
{
	BOOL	nowait;
	INT	hi;
	ER	er = key_take(key, OB_OP_READ, recno, &hi, &nowait);

	if ( er < E_OK ) return er;
	if ( buf == NULL && size == 0 && p_asize != NULL && MGR(hi)->type == OB_T_STORAGE ) {
		/* the size of the record, to one who may read it */
		er = rec_bytes(hi, recno, p_asize);
	} else {
		er = ( MGR(hi)->rea != NULL )
		   ? MGR(hi)->rea(HND(hi), recno, off, buf, size, p_asize, nowait) : E_NOSPT;
	}
	key_done(hi);
	return er;
}

EXPORT ER ob_rea_tmo( ID key, INT recno, D off, void *buf, SZ size, SZ *p_asize, TMO tmo )
{
	INT	hi;
	ER	er = key_take(key, OB_OP_READ, recno, &hi, NULL);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->rtm != NULL ) ? MGR(hi)->rtm(HND(hi), recno, off, buf, size, p_asize, tmo) : E_NOSPT;
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

EXPORT ER ob_rpl_rec( ID key, INT recno, CONST void *buf, SZ size )
{
	BOOL	nowait;
	OBCHG	chg;
	SZ	asz = 0;
	INT	hi;
	ER	er;

	if ( size < 0 || ( buf == NULL && size > 0 ) ) {
		return E_PAR;
	}
	er = key_take(key, OB_OP_WRITE, recno, &hi, &nowait);
	if ( er < E_OK ) return er;
	if ( MGR(hi)->type == OB_T_CHANNEL ) {
		er = E_NOSPT;			/* a channel's record is a message */
	} else if ( MGR(hi)->rpl != NULL ) {
		er = MGR(hi)->rpl(HND(hi), recno, buf, size);
	} else if ( MGR(hi)->wri == NULL || MGR(hi)->trn == NULL ) {
		er = E_NOSPT;
	} else {
		er = ( size > 0 ) ? MGR(hi)->wri(HND(hi), recno, 0, buf, size, &asz, nowait) : E_OK;
		if ( er >= E_OK ) er = MGR(hi)->trn(HND(hi), recno, (UD)size);
	}
	chg_take(hi, recno, &chg);
	key_done(hi);
	if ( er >= E_OK ) chg_post(&chg);
	return er;
}

EXPORT ER ob_syn_obj( ID key )
{
	INT	hi;
	ER	er = key_take(key, 0, -1, &hi, NULL);

	if ( er < E_OK ) return er;
	er = ( MGR(hi)->syn != NULL ) ? MGR(hi)->syn(HND(hi)) : E_OK;
	key_done(hi);
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

/*
 * A record read whole, in a buffer as large as it is with room bytes
 * more and a nought after the text. Its size is asked first when the
 * key may list the records, and it is read once into a buffer one byte
 * larger; one that fills it (it grew, or it is made as it is read and
 * has no size of its own) is read again from its start into a buffer
 * twice as large, so that it comes as one whole. Answers the length,
 * the buffer in *p_buf (the caller Kfrees it) and what it holds in
 * *p_cap, or an error; E_LIMIT for a record longer than the INT the
 * length is answered in.
 */
#define REA_ALL_FIRST	4096
#define REA_ALL_LIMIT	( (SZ)0x7FFFFFFF )

/* What record `recno` of the object open as key holds, or 0 when it cannot be told */
LOCAL SZ rec_size_of( ID key, INT recno )
{
	SZ	size = 0;

	if ( ob_rea_rec(key, recno, 0, NULL, 0, &size) < E_OK || size < 0 || size >= REA_ALL_LIMIT ) {
		return 0;
	}
	return size;
}

EXPORT INT knl_ob_rea_all( ID key, INT recno, INT room, UB **p_buf, INT *p_cap )
{
	UB	*b = NULL, *nb;
	SZ	cap = REA_ALL_FIRST, asz;
	ER	er;

	*p_buf = NULL;
	if ( room < 0 ) {
		return E_PAR;
	}
	asz = rec_size_of(key, recno);
	if ( asz >= cap ) {
		cap = asz + 1;
	}
	for ( ;; ) {
		b = (UB *)Kmalloc(cap + 1);
		if ( b == NULL ) {
			return E_NOMEM;
		}
		asz = 0;
		er = ob_rea_rec(key, recno, 0, b, cap, &asz);
		if ( er < E_OK ) {
			Kfree(b);
			return (INT)er;
		}
		if ( asz < cap ) {
			break;
		}
		Kfree(b);
		if ( cap >= REA_ALL_LIMIT / 2 ) {
			return E_LIMIT;
		}
		cap *= 2;
	}
	if ( asz + (SZ)room >= REA_ALL_LIMIT ) {
		Kfree(b);
		return E_LIMIT;
	}
	if ( cap - asz < (SZ)room ) {
		nb = (UB *)Kmalloc(asz + room + 1);
		if ( nb == NULL ) {
			Kfree(b);
			return E_NOMEM;
		}
		knl_memcpy(nb, b, asz);
		Kfree(b);
		b = nb;
		cap = asz + room;
	}
	b[asz] = 0;
	*p_buf = b;
	if ( p_cap != NULL ) *p_cap = (INT)cap;
	return (INT)asz;
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

/* A manager's gat of an open handle, for knl_meta_all */
typedef struct {
	CONST T_OBMGR	*m;
	INT		h;
} MGRMETA;

LOCAL ER mgr_meta_get( void *ctx, UB *buf, SZ size, SZ *p_asize )
{
	MGRMETA	*g = (MGRMETA *)ctx;

	return g->m->gat(g->h, buf, size, p_asize);
}

LOCAL INT mgr_meta_all( CONST T_OBMGR *m, INT h, INT room, UB **p_buf, INT *p_cap )
{
	MGRMETA	g;

	g.m = m;
	g.h = h;
	return knl_meta_all(mgr_meta_get, &g, room, p_buf, p_cap);
}

LOCAL ER key_meta_get( void *ctx, UB *buf, SZ size, SZ *p_asize )
{
	return ob_get_atr(*(ID *)ctx, buf, size, p_asize);
}

EXPORT INT knl_ob_atr_all( ID key, INT room, UB **p_buf, INT *p_cap )
{
	return knl_meta_all(key_meta_get, &key, room, p_buf, p_cap);
}

EXPORT INT knl_ob_meta_all( CONST TS_UUID *uuid, INT room, UB **p_buf, INT *p_cap )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);
	INT		h, n;

	*p_buf = NULL;
	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->open == NULL || m->gat == NULL ) {
		return E_NOSPT;
	}
	h = m->open(uuid, OB_OP_ATRRD);
	if ( h <= 0 ) {
		return ( h < 0 ) ? (INT)h : E_SYS;
	}
	n = mgr_meta_all(m, h, room, p_buf, p_cap);
	if ( m->close != NULL ) (void)m->close(h);
	return n;
}

EXPORT ER ob_get_atv( ID key, CONST UB *path, UB *buf, SZ size, SZ *p_asize )
{
	UB	*j = NULL, k[OB_ATV_PATH];
	INT	len, v, e, i, n;
	SZ	m;

	if ( path == NULL || size < 0 || ( size > 0 && buf == NULL ) ) {
		return E_PAR;
	}
	len = knl_ob_atr_all(key, 0, &j, NULL);
	if ( len < 0 ) {
		return (ER)len;
	}
	/* down the keys, one at a time */
	v = knl_oj_root(j, len);
	for ( i = 0; v >= 0 && path[i] != 0; ) {
		for ( n = 0; path[i] != 0 && path[i] != '.' && n < OB_ATV_PATH - 1; n++, i++ ) {
			k[n] = path[i];
		}
		k[n] = 0;
		if ( path[i] == '.' ) i++;
		v = ( j[v] == '{' ) ? knl_oj_member(j, len, v, (CONST char *)k) : -1;
	}
	e = ( v >= 0 ) ? knl_oj_skip(j, len, v) : -1;
	if ( e < 0 ) {
		Kfree(j);
		return E_NOEXS;
	}
	m = (SZ)( e - v );
	if ( p_asize != NULL ) *p_asize = m;
	if ( size > 0 ) {
		knl_memcpy(buf, j + v, (INT)( ( m < size ) ? m : size ));
	}
	Kfree(j);
	return E_OK;
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
	INT	hi, ao, al, no, nl, len, cap;
	BOOL	may_prot, had, has;
	OBKEY	*k;
	ER	er = key_take(key, OB_OP_ATRWR, -1, &hi, NULL);

	if ( er < E_OK ) return er;
	if ( MGR(hi)->sat == NULL || MGR(hi)->gat == NULL ) {
		key_done(hi);
		return E_NOSPT;
	}
	if ( json == NULL || size <= 0 || size > 0x3FFFFFFF ) {
		key_done(hi);
		return E_PAR;
	}
	LOCK();
	k = key_of(key);
	may_prot = (BOOL)( k != NULL && ( k->ops & OB_OP_PROT ) != 0 );
	UNLOCK();

	/* the metadata there is, whole, and room for the new with the old's protection put in */
	len = mgr_meta_all(MGR(hi), HND(hi), 0, &old, NULL);
	olen = ( len > 0 ) ? (SZ)len : 0;
	cap = (INT)size + (INT)olen + 64;
	nw = (UB *)Kmalloc((SZ)cap + 1);
	if ( nw == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	knl_memcpy(nw, json, (INT)size);
	nw[size] = 0;
	len = (INT)size;

	had = ( old != NULL && access_span(old, (INT)olen, &ao, &al) );
	has = access_span(nw, len, &no, &nl);
	if ( !may_prot ) {
		if ( had ) {
			len = knl_oj_set_path(nw, len, cap, "tessronos", "access",
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
		T_OBPRT	*np = prot_of(MGR(hi), &(*ob_hndp[hi]).uuid), *op = NULL;

		if ( np != NULL ) {
			LOCK();
			op = (*ob_hndp[hi]).prt;
			(*ob_hndp[hi]).prt = np;
			UNLOCK();
		}
		if ( op != NULL ) Kfree(op);
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
	/* the protection given, as long as it is, else one of the head alone */
	prt = ( c->prt != NULL ) ? knl_ob_prt_dup(c->prt) : knl_ob_prt_new(0, 0);
	if ( prt == NULL ) {
		return E_NOMEM;
	}
	crd = knl_ob_crd_of(ts_get_pid());

	/* an identity chosen by the maker: the system's own objects only */
	if ( !knl_ob_uuid_zero(&c->uuid) ) {
		if ( !knl_ob_is_admin(crd) ) {
			Kfree(prt);
			knl_ob_deny_note("create-with-uuid", &c->uuid, 0, 0);
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
		{
			T_OBPRT	*pp = prot_of(pm, &c->prog);
			UINT	may = ( pp != NULL ) ? permit_on(pm, &c->prog, crd, pp) : 0;

			if ( pp != NULL ) Kfree(pp);
			if ( ( may & OB_OP_EXEC ) == 0 ) {
				Kfree(prt);
				knl_ob_deny_note("run", &c->prog, OB_OP_EXEC, 0);
				return E_OACV;
			}
		}
	}

	if ( c->prt != NULL ) {
		if ( !knl_ob_is_admin(crd) ) {
			prt->owner = crd->user;	/* nobody makes objects for another */
		}
	} else if ( ts_get_pid() <= 0 && c->type == OB_T_STORAGE && c->sub == OB_S_FILE
		 && knl_ob_uuid_zero(&c->uuid) && maker_for_person() ) {
		/*
		 * A real object the desktop makes with nothing said of its
		 * protection -- a document made from a template, a copy, a
		 * new version -- is made for the person at the machine, whose
		 * programs are to open and change it.
		 */
		knl_ob_prt_default(prt, knl_ob_session_crd());
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
	T_OBPRT	*prt = prot_of(m, uuid);
	UINT	may;

	if ( prt == NULL ) {
		return E_NOMEM;
	}
	may = permit_on(m, uuid, knl_ob_crd_of(ts_get_pid()), prt);
	Kfree(prt);
	if ( ( may & ops ) != ops ) {
		knl_ob_deny_note("object", uuid, ops, may);
		return E_OACV;
	}
	return E_OK;
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

/*
 * Whether the caller may do every one of ops to an object, answered
 * without opening it (as access(2) answers): by the same rights
 * ob_opn_obj opens with, the caller's credentials through the volume's
 * domain and what it was granted. A list of objects asks this before
 * it opens one, so that an object it may not read is shown as such and
 * not opened to be refused. The answer is not a refusal of anything
 * asked, so it is not noted on the console as one.
 */
EXPORT ER ob_acc_obj( CONST TS_UUID *uuid, UINT ops )
{
	CONST T_OBMGR	*m;
	T_OBPRT		*prt;
	UINT		may;
	ID		pid = ts_get_pid();

	if ( uuid == NULL || ( ops & ~OB_OP_ALL ) != 0 ) {
		return E_PAR;
	}
	m = knl_ob_mgr_of(uuid);
	if ( m == NULL ) {
		return E_NOEXS;
	}
	prt = prot_of(m, uuid);
	if ( prt == NULL ) {
		return E_NOMEM;
	}
	may = permit_on(m, uuid, knl_ob_crd_of(pid), prt);
	Kfree(prt);
	if ( ( ops & ~may ) != 0 && pid > 0 ) {
		may |= granted(uuid, pid);
	}
	return ( ( ops & ~may ) == 0 ) ? E_OK : E_OACV;
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
LOCAL BOOL lnk_named( CONST TS_UUID *u, CONST UB *name )
{
	T_OBREF	r;
	UB	file[256], *meta = NULL;
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
	if ( ob_acc_obj(u, OB_OP_ATRRD) < E_OK ) {
		return FALSE;			/* its metadata is not the caller's to read: not opened to be refused */
	}
	key = ob_opn_obj(u, OB_OP_ATRRD);
	if ( key < E_OK ) {
		return FALSE;
	}
	len = knl_ob_atr_all(key, 0, &meta, NULL);
	if ( len > 0 ) {
		p = knl_oj_path(meta, len, "tessronos", "file");
		p = ( p >= 0 && meta[p] == '{' ) ? knl_oj_member(meta, len, p, "name") : -1;
		if ( p >= 0 && knl_oj_str(meta, len, p, file, sizeof(file)) > 0 ) {
			is = lnk_name_is(file, name);
		}
	}
	if ( meta != NULL ) Kfree(meta);
	(void)ob_cls_obj(key);
	return is;
}

EXPORT ER ob_lst_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *buf, INT n, INT *p_cnt )
{
	UB	*t = NULL;
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
	while ( t != NULL && lnk_next(t, len, &at, &u) ) {
		if ( !all && !lnk_named(&u, name) ) {
			continue;
		}
		if ( cnt < n ) {
			buf[cnt] = u;
		}
		cnt++;
	}
	if ( t != NULL ) Kfree(t);
	*p_cnt = cnt;
	return E_OK;
}

EXPORT ER ob_fnd_lnk( CONST TS_UUID *box, CONST UB *name, TS_UUID *p_uuid )
{
	UB	*t = NULL;
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
	while ( er == E_NOEXS && t != NULL && lnk_next(t, len, &at, &u) ) {
		if ( all || lnk_named(&u, name) ) {
			*p_uuid = u;
			er = E_OK;
		}
	}
	if ( t != NULL ) Kfree(t);
	return er;
}


/* ---------------------------------------------------------------- records between objects */

#define OB_REC_LIST	1024		/* records looked at in one object */
#define OB_CHUNK	( 16 * 1024 )	/* bytes of a resource read at a time, of the names of resources */
#define OB_COPY_MAX	( 4 * 1024 * 1024 )	/* bytes of a record copied at a time */

/*
 * The buffer a record goes through from one object to another: as
 * large as OB_COPY_MAX, each piece being one write of the target and so
 * one change made durable; smaller when memory is short. The size is
 * put in *p_size.
 */
LOCAL UB *copy_buf( SZ *p_size )
{
	SZ	n = OB_COPY_MAX;
	UB	*b;

	while ( ( b = (UB *)Kmalloc(n) ) == NULL && n > OB_CHUNK ) {
		n /= 2;
	}
	*p_size = n;
	return b;
}

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
		   INT drec, UB *buf, SZ bsize )
{
	D	off = 0;
	SZ	got, put;
	ER	er;

	if ( sm->rea == NULL || dm->wri == NULL ) {
		return E_NOSPT;
	}
	for ( ;; ) {
		got = 0;
		er = sm->rea(sh, srec, off, buf, bsize, &got, FALSE);
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
		if ( got < bsize ) {
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
	SZ	bsize = 0;
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
	buf = copy_buf(&bsize);
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
		er = rec_copy(MGR(shi), HND(shi), r->recno, MGR(dhi), HND(dhi), d, buf, bsize);
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
				knl_memcpy(more, all, have);
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

EXPORT ER ob_dup_obj( CONST TS_UUID *uuid, CONST UB *name, TS_UUID *p_uuid )
{
	CONST T_OBMGR	*m;
	ER		er;

	if ( uuid == NULL || p_uuid == NULL || ( name != NULL && name[0] == 0 ) ) {
		return E_PAR;
	}
	m = knl_ob_mgr_of(uuid);
	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->type != OB_T_STORAGE || m->sub != OB_S_FILE ) {
		return E_NOSPT;
	}
	er = allowed(m, uuid, OB_OP_READ | OB_OP_ATRRD);
	if ( er < E_OK ) {
		return er;
	}
	return om_store_copy(uuid, name, FALSE, p_uuid);
}

/* A copy's metadata: the source's, counted by nobody yet (j holds max bytes) */
LOCAL INT meta_for_copy( UB *j, INT len, INT max )
{
	INT	root = knl_oj_root(j, len);

	if ( root >= 0 && knl_oj_member(j, len, root, "refCount") >= 0 ) {
		len = knl_oj_set(j, len, max, root, "refCount", (CONST UB *)"0", 1);
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
	SZ		mlen = 0, bsize = 0;
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
	buf = copy_buf(&bsize);
	if ( buf == NULL ) {
		er = E_NOMEM;
		goto out;
	}
	/* the source's metadata whole, with room for its count to be written as 0 */
	mlen = 0;
	if ( sm->gat != NULL ) {
		INT	mcap = 0, k = mgr_meta_all(sm, sh, 16, &meta, &mcap);

		mlen = ( k > 0 ) ? (SZ)k : 0;
		len = ( mlen > 0 ) ? meta_for_copy(meta, (INT)mlen, mcap) : 0;
	} else {
		len = 0;
	}

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
			er = rec_copy(sm, sh, sr[i].recno, dm, dh, d, buf, bsize);
		}
	}

	/* and what is kept beside them */
	if ( er >= E_OK && sm->lres != NULL && dm->wres != NULL && sm->rres != NULL ) {
		/* the names of all of them: how many asked first */
		if ( sm->lres(sh, NULL, 0, &cnt) < E_OK ) cnt = 0;
		names = (UB *)Kmalloc(( cnt > 0 ? (SZ)cnt : 1 ) * OB_RES_NAME);
		if ( names == NULL ) {
			er = E_NOMEM;
		} else if ( cnt > 0 && sm->lres(sh, names, (SZ)cnt * OB_RES_NAME, &cnt) >= E_OK ) {
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
		/* a move behind a move still waiting: that one says where the pointer is now */
		er = knl_obchan_merge_move(port, msg) ? E_OK : m->wri(h, 0, 0, msg, sizeof(*msg), &asz, TRUE);
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
		if ( event == OB_E_KEY && n != NULL && ( n->mods & OB_KEY_SEEN ) != 0 ) {
			if ( ( r->flags & OB_N_KEYSEEN ) == 0 ) {
				continue;		/* every key as it came: only to who asked */
			}
		} else if ( event == OB_E_KEY && n != NULL && ( n->mods & OB_KEY_UP ) != 0
		  && ( r->flags & OB_N_KEYUP ) == 0 ) {
			continue;			/* a key let go: only to who asked for those */
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

/*
 * A protection given out: its size in *p_asize; the whole of it into buf
 * when size has room for it, nothing with a size of 0, E_LIMIT when the
 * room given is less.
 */
LOCAL ER prt_give( CONST T_OBPRT *p, T_OBPRT *buf, SZ size, SZ *p_asize )
{
	SZ	n = (SZ)OB_PRT_LEN(p);

	if ( p_asize != NULL ) *p_asize = n;
	if ( size == 0 ) {
		return E_OK;
	}
	if ( buf == NULL || size < n ) {
		return E_LIMIT;
	}
	knl_memcpy(buf, p, (INT)n);
	return E_OK;
}

EXPORT T_OBPRT *knl_ob_prt_get( CONST TS_UUID *uuid )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);

	return ( m != NULL ) ? prot_of(m, uuid) : NULL;
}

EXPORT ER ob_get_prt( CONST TS_UUID *uuid, T_OBPRT *pk_prt, SZ size, SZ *p_asize )
{
	CONST T_OBMGR	*m = knl_ob_mgr_of(uuid);
	T_OBPRT		*p;
	ER		er;

	if ( size < 0 || ( size > 0 && pk_prt == NULL ) ) {
		return E_PAR;
	}
	if ( m == NULL ) {
		return E_NOEXS;
	}
	p = prot_of(m, uuid);
	if ( p == NULL ) {
		return E_NOMEM;
	}
	er = prt_give(p, pk_prt, size, p_asize);
	Kfree(p);
	return er;
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

	if ( pk_prt == NULL || pk_prt->nacl < 0 || pk_prt->nrmask < 0 ) {
		return E_PAR;
	}
	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->setprot == NULL ) {
		return E_NOSPT;
	}
	cur = prot_of(m, uuid);
	if ( cur == NULL ) {
		return E_NOMEM;
	}
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
	} else {
		knl_ob_deny_note("protection", uuid, OB_OP_PROT, 0);
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

EXPORT ER ob_wip_vol( CONST TS_UUID *near )
{
	CONST T_OBMGR	*m;
	ID		vol;

	if ( near == NULL ) {
		return E_PAR;
	}
	m = knl_ob_mgr_of(near);
	if ( m == NULL ) {
		return E_NOEXS;
	}
	if ( m->type != OB_T_STORAGE || m->sub != OB_S_FILE ) {
		return E_NOSPT;
	}
	if ( ( knl_ob_permit_on(near, knl_ob_crd_of(ts_get_pid())) & OB_OP_WRITE ) == 0 ) {
		return E_OACV;
	}
	vol = knl_obfile_vol_of(near);
	if ( vol <= 0 ) {
		return ( vol < 0 ) ? (ER)vol : E_NOEXS;
	}
	return ts_jrnl_wipe(vol);
}

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

EXPORT ER ob_ref_vid( CONST TS_UUID *obj, TS_UUID *p_vol, TS_UUID *p_mach )
{
	CONST T_OBMGR	*m;
	TS_UUID		v, mine;
	ER		er;

	if ( ( obj == NULL && p_vol != NULL ) || ( p_vol == NULL && p_mach == NULL ) ) {
		return E_PAR;
	}
	if ( p_vol != NULL ) {
		m = knl_ob_mgr_of(obj);
		if ( m == NULL ) {
			return E_NOEXS;
		}
		if ( m->type != OB_T_STORAGE || m->sub != OB_S_FILE ) {
			return E_NOSPT;
		}
		if ( ( knl_ob_permit_on(obj, knl_ob_crd_of(ts_get_pid())) & OB_OP_ATRRD ) == 0 ) {
			return E_OACV;
		}
		er = knl_obfile_vuuid_of(obj, &v);
		if ( er < E_OK ) {
			return er;
		}
	}
	if ( p_mach != NULL && knl_obfile_machine(&mine) < E_OK ) {
		knl_memset(&mine, 0, sizeof(mine));	/* no system volume yet: no machine to name */
	}
	if ( p_vol != NULL ) {
		*p_vol = v;
	}
	if ( p_mach != NULL ) {
		*p_mach = mine;
	}
	return E_OK;
}

EXPORT ER ob_att_vol( CONST char *path, UINT flags )
{
	ER	er;

	if ( !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		knl_ob_deny_note("attach-volume", NULL, 0, 0);
		return E_OACV;
	}
	er = knl_obfile_attach(path, flags);
	if ( er >= E_OK && !knl_obfile_again(path) ) {
		/* what is emptied at each start, of what is on it; not again for a disk put back */
		knl_sys_clear_start(path);
	}
	if ( er >= E_OK ) {
		knl_dt_zone_load();		/* the calendar may be on it */
	}
	return er;
}

EXPORT ER ob_det_vol( CONST char *path )
{
	if ( !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		knl_ob_deny_note("detach-volume", NULL, 0, 0);
		return E_OACV;
	}
	return knl_obfile_detach(path);
}

EXPORT ER ob_get_dom( CONST char *vol, T_OBPRT *pk_prt, SZ size, SZ *p_asize )
{
	T_OBPRT	*d;
	ER	er;

	if ( vol == NULL || size < 0 || ( size > 0 && pk_prt == NULL ) ) {
		return E_PAR;
	}
	d = knl_obfile_get_dom(vol);
	if ( d == NULL ) {
		return E_NOEXS;
	}
	er = prt_give(d, pk_prt, size, p_asize);
	Kfree(d);
	return er;
}

EXPORT ER ob_set_dom( CONST char *vol, CONST T_OBPRT *pk_prt )
{
	if ( vol == NULL ) {
		return E_PAR;
	}
	if ( !knl_ob_is_admin(knl_ob_crd_of(ts_get_pid())) ) {
		knl_ob_deny_note("volume-domain", NULL, 0, 0);
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
	(void)knl_ob_chk_boot(CNF_OB_SYSVOL);	/* a repair asked for: before anything uses it */
	knl_sysvol_grow(CNF_OB_SYSVOL);		/* the card's room taken in, the first time */
	er = knl_obfile_attach(CNF_OB_SYSVOL, TSFS_STORE_BLK);
	if ( er >= E_OK ) {
		tm_printf((UB *)"ob: the system volume is %s\n", CNF_OB_SYSVOL);
		knl_sys_clear_start(CNF_OB_SYSVOL);
	}
#endif
#ifdef CNF_OB_STORE
	if ( er < E_OK && knl_obfile_attach(CNF_OB_STORE, 0) >= E_OK ) {
		knl_sys_clear_start(CNF_OB_STORE);
	}
#endif
	knl_dt_zone_load();			/* the zone the calendar keeps */
	knl_obdev_start();			/* the devices outside the device management */
	return E_OK;
}
