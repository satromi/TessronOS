/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_kern.c (x64 PC, WIFI_NET80211=obsd)
 *	The kernel services OpenBSD's IEEE 802.11 layer and iwm(4) call
 *	(design 9.6.9): the one lock, sleeping and waking, timeouts, task
 *	queues, read/write locks, reference counts, memory and its pools,
 *	time, random numbers and the console. Compiled with OpenBSD's
 *	kernel headers (tools/openbsd/build.sh); what it needs of TessronOS
 *	is obsd_hc.h.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/rwlock.h>
#include <sys/refcnt.h>
#include <sys/mutex.h>
#include <sys/percpu.h>
#include <sys/time.h>
#include <sys/device.h>
#include <sys/mbuf.h>
#include <sys/sysctl.h>
#include <sys/stdarg.h>
#include <uvm/uvm_extern.h>
#include <machine/cpu.h>
#include <machine/cpu_full.h>

#include "obsd_glue.h"

/* ---------------------------------------------------------------- the lock */

static int	big_mtx = -1;
static volatile int big_owner;		/* the task holding it, 0 none */
static int	big_depth;

void
obsd_lock(void)
{
	int me = obsd_hc_self();

	if (big_owner == me) {
		big_depth++;
		return;
	}
	obsd_hc_mtx_lock(big_mtx);
	big_owner = me;
	big_depth = 1;
}

void
obsd_unlock(void)
{
	if (--big_depth > 0)
		return;
	big_owner = 0;
	obsd_hc_mtx_unlock(big_mtx);
}

int
obsd_lock_drop(void)
{
	int depth = big_depth;

	big_depth = 0;
	big_owner = 0;
	obsd_hc_mtx_unlock(big_mtx);
	return depth;
}

void
obsd_lock_take(int depth)
{
	obsd_hc_mtx_lock(big_mtx);
	big_owner = obsd_hc_self();
	big_depth = depth;
}

/* The levels of the interrupts: nothing runs without the lock */
int
splraise(int ipl)
{
	return 0;
}

int
spllower(int ipl)
{
	return 0;
}

/* Mutexes guard what the lock guards already */
void
__mtx_init(struct mutex *mtx, int ipl)
{
	memset(mtx, 0, sizeof(*mtx));
}

void
mtx_enter(struct mutex *mtx)
{
}

int
mtx_enter_try(struct mutex *mtx)
{
	return 1;
}

void
mtx_leave(struct mutex *mtx)
{
}

/* ---------------------------------------------------------------- sleep and wakeup */

struct sleeper {
	struct sleeper		*next;
	const volatile void	*chan;
	int			sem;
	int			woken;
};

static struct sleeper	*sleeping;	/* those asleep (the lock guards the list) */
static struct sleeper	*sleep_free;

static struct sleeper *
sleeper_get(void)
{
	struct sleeper *s = sleep_free;

	if (s != NULL) {
		sleep_free = s->next;
		return s;
	}
	s = obsd_hc_malloc(sizeof(*s));
	if (s == NULL)
		return NULL;
	s->sem = obsd_hc_sem_new();
	if (s->sem < 0) {
		obsd_hc_free(s);
		return NULL;
	}
	return s;
}

/*
 * Asleep on ident until it is woken or nsecs pass: the lock is let go
 * while asleep and taken back. 0 when woken, EWOULDBLOCK when the time
 * ran out.
 */
int
tsleep_nsec(const volatile void *ident, int priority, const char *wmesg,
    uint64_t nsecs)
{
	struct sleeper *s, **pp;
	long long ms;
	int depth, timed_out, woken;

	s = sleeper_get();
	if (s == NULL) {
		/* no room to sleep: as if woken at once */
		return 0;
	}
	s->chan = ident;
	s->woken = 0;
	s->next = sleeping;
	sleeping = s;
	if (nsecs == INFSLP)
		ms = -1;
	else
		ms = (long long)((nsecs + 999999) / 1000000);
	depth = obsd_lock_drop();
	timed_out = obsd_hc_sem_wait(s->sem, ms);
	obsd_lock_take(depth);
	for (pp = &sleeping; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	}
	woken = s->woken;
	if (timed_out && woken) {
		/* woken as the time ran out: the count given is taken */
		(void)obsd_hc_sem_wait(s->sem, 0);
	}
	s->next = sleep_free;
	sleep_free = s;
	return woken ? 0 : EWOULDBLOCK;
}

int
tsleep(const volatile void *ident, int priority, const char *wmesg, int timo)
{
	return tsleep_nsec(ident, priority, wmesg,
	    timo == 0 ? INFSLP : (uint64_t)timo * 10000000ULL);
}

void
wakeup_n(const volatile void *ident, int n)
{
	struct sleeper *s;

	for (s = sleeping; s != NULL && n != 0; s = s->next) {
		if (s->chan == ident && !s->woken) {
			s->woken = 1;
			obsd_hc_sem_post(s->sem);
			if (n > 0)
				n--;
		}
	}
}

void
wakeup(const volatile void *ident)
{
	wakeup_n(ident, -1);
}

/* ---------------------------------------------------------------- tasks of the layer */

struct obsd_start {
	void	(*fn)(void *);
	void	*arg;
};

static void
obsd_task_main(void *v)
{
	struct obsd_start st = *(struct obsd_start *)v;

	obsd_hc_free(v);
	obsd_lock();
	(*st.fn)(st.arg);
	obsd_unlock();
}

int
obsd_task(void (*fn)(void *), void *arg, int pri)
{
	struct obsd_start *st = obsd_hc_malloc(sizeof(*st));

	if (st == NULL)
		return -1;
	st->fn = fn;
	st->arg = arg;
	if (obsd_hc_thread(obsd_task_main, st, pri, OBSD_STKSZ) != 0) {
		obsd_hc_free(st);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------- timeouts */

static struct circq	to_head = { &to_head, &to_head };
static int		to_sem = -1;

static uint64_t
to_due(const struct timeout *to)
{
	return (uint64_t)to->to_abstime.tv_sec * 1000000000ULL +
	    (uint64_t)to->to_abstime.tv_nsec;
}

static void
to_unlink(struct timeout *to)
{
	to->to_list.prev->next = to->to_list.next;
	to->to_list.next->prev = to->to_list.prev;
	to->to_list.next = to->to_list.prev = NULL;
	to->to_flags &= ~TIMEOUT_ONQUEUE;
}

void
timeout_set(struct timeout *to, void (*fn)(void *), void *arg)
{
	memset(to, 0, sizeof(*to));
	to->to_func = fn;
	to->to_arg = arg;
	to->to_flags = TIMEOUT_INITIALIZED;
}

void
timeout_set_proc(struct timeout *to, void (*fn)(void *), void *arg)
{
	timeout_set(to, fn, arg);
	to->to_flags |= TIMEOUT_PROC;
}

int
timeout_add_nsec(struct timeout *to, uint64_t nsecs)
{
	struct circq *c;
	uint64_t due;
	int added = 1;

	if (to->to_flags & TIMEOUT_ONQUEUE) {
		to_unlink(to);
		added = 0;
	}
	due = obsd_hc_nsec() + nsecs;
	to->to_abstime.tv_sec = due / 1000000000ULL;
	to->to_abstime.tv_nsec = due % 1000000000ULL;
	to->to_flags &= ~TIMEOUT_TRIGGERED;
	to->to_flags |= TIMEOUT_ONQUEUE | TIMEOUT_INITIALIZED;
	/* in the order they are due */
	for (c = to_head.next; c != &to_head; c = c->next) {
		if (to_due((struct timeout *)c) > due)
			break;
	}
	to->to_list.next = c;
	to->to_list.prev = c->prev;
	c->prev->next = &to->to_list;
	c->prev = &to->to_list;
	if (to_head.next == &to->to_list && to_sem >= 0)
		obsd_hc_sem_post(to_sem);
	return added;
}

int
timeout_add(struct timeout *to, int ticks)
{
	return timeout_add_nsec(to, ticks <= 0 ? 0 : (uint64_t)ticks * 10000000ULL);
}

int
timeout_add_sec(struct timeout *to, int secs)
{
	return timeout_add_nsec(to, (uint64_t)secs * 1000000000ULL);
}

int
timeout_add_msec(struct timeout *to, uint64_t msecs)
{
	return timeout_add_nsec(to, msecs * 1000000ULL);
}

int
timeout_add_usec(struct timeout *to, uint64_t usecs)
{
	return timeout_add_nsec(to, usecs * 1000ULL);
}

int
timeout_del(struct timeout *to)
{
	int was = 0;

	if (to->to_flags & TIMEOUT_ONQUEUE) {
		to_unlink(to);
		was = 1;
	}
	to->to_flags &= ~TIMEOUT_TRIGGERED;
	return was;
}

int
timeout_del_barrier(struct timeout *to)
{
	return timeout_del(to);
}

/* The task of the timeouts: each runs when it is due, holding the lock */
static void
timeout_main(void *arg)
{
	struct timeout *to;
	uint64_t now, due;
	long long ms;
	int depth;

	for (;;) {
		now = obsd_hc_nsec();
		while (to_head.next != &to_head) {
			to = (struct timeout *)to_head.next;
			if (to_due(to) > now)
				break;
			to_unlink(to);
			to->to_flags |= TIMEOUT_TRIGGERED;
			(*to->to_func)(to->to_arg);
			now = obsd_hc_nsec();
		}
		if (to_head.next == &to_head) {
			ms = -1;
		} else {
			due = to_due((struct timeout *)to_head.next);
			ms = (long long)((due - now + 999999) / 1000000);
		}
		depth = obsd_lock_drop();
		(void)obsd_hc_sem_wait(to_sem, ms);
		obsd_lock_take(depth);
	}
}

/* ---------------------------------------------------------------- task queues */

struct taskq {
	const char	*tq_name;
	TAILQ_HEAD(, task) tq_list;
	int		tq_sem;
	int		tq_running;
	int		tq_pri;
};

static struct taskq tq_sys = { "systq" };
static struct taskq tq_sysmp = { "systqmp" };
struct taskq *const systq = &tq_sys;
struct taskq *const systqmp = &tq_sysmp;

static void
taskq_main(void *arg)
{
	struct taskq *tq = arg;
	struct task *t;
	int depth;

	for (;;) {
		while ((t = TAILQ_FIRST(&tq->tq_list)) != NULL) {
			TAILQ_REMOVE(&tq->tq_list, t, t_entry);
			t->t_flags &= ~TASK_ONQUEUE;
			(*t->t_func)(t->t_arg);
		}
		depth = obsd_lock_drop();
		(void)obsd_hc_sem_wait(tq->tq_sem, -1);
		obsd_lock_take(depth);
	}
}

static int
taskq_start(struct taskq *tq)
{
	if (tq->tq_running)
		return 0;
	TAILQ_INIT(&tq->tq_list);
	tq->tq_sem = obsd_hc_sem_new();
	if (tq->tq_sem < 0)
		return -1;
	if (obsd_task(taskq_main, tq, OBSD_PRI_TASKQ) != 0)
		return -1;
	tq->tq_running = 1;
	return 0;
}

struct taskq *
taskq_create(const char *name, unsigned int nthreads, int ipl, unsigned int flags)
{
	struct taskq *tq = obsd_hc_malloc(sizeof(*tq));

	if (tq == NULL)
		return NULL;
	memset(tq, 0, sizeof(*tq));
	tq->tq_name = name;
	if (taskq_start(tq) != 0) {
		obsd_hc_free(tq);
		return NULL;
	}
	return tq;
}

void
taskq_destroy(struct taskq *tq)
{
	/* the queues stay for as long as the machine runs */
}

void
task_set(struct task *t, void (*fn)(void *), void *arg)
{
	t->t_func = fn;
	t->t_arg = arg;
	t->t_flags = 0;
}

int
task_add(struct taskq *tq, struct task *t)
{
	if (t->t_flags & TASK_ONQUEUE)
		return 0;
	if (!tq->tq_running && taskq_start(tq) != 0)
		return 0;
	t->t_flags |= TASK_ONQUEUE;
	TAILQ_INSERT_TAIL(&tq->tq_list, t, t_entry);
	obsd_hc_sem_post(tq->tq_sem);
	return 1;
}

int
task_del(struct taskq *tq, struct task *t)
{
	if ((t->t_flags & TASK_ONQUEUE) == 0)
		return 0;
	TAILQ_REMOVE(&tq->tq_list, t, t_entry);
	t->t_flags &= ~TASK_ONQUEUE;
	return 1;
}

/* ---------------------------------------------------------------- read/write locks */

void
_rw_init_flags(struct rwlock *rwl, const char *name, int flags,
    const struct lock_type *type, int trace)
{
	memset(rwl, 0, sizeof(*rwl));
	rwl->rwl_name = name;
}

/*
 * Taken for writing or for reading. Who holds it may sleep (iwm sends
 * the firmware its commands with it held): another waits, asleep.
 */
int
rw_enter(struct rwlock *rwl, int flags)
{
	unsigned long me = (unsigned long)obsd_hc_self();

	switch (flags & RW_OPMASK) {
	case RW_WRITE:
		while (rwl->rwl_owner != 0 || rwl->rwl_readers != 0) {
			if (flags & RW_NOSLEEP)
				return EBUSY;
			if (rwl->rwl_owner == me)
				return EDEADLK;
			rwl->rwl_waiters++;
			(void)tsleep_nsec(rwl, 0, rwl->rwl_name, INFSLP);
			rwl->rwl_waiters--;
		}
		rwl->rwl_owner = me;
		return 0;
	case RW_READ:
		while (rwl->rwl_owner != 0) {
			if (flags & RW_NOSLEEP)
				return EBUSY;
			rwl->rwl_waiters++;
			(void)tsleep_nsec(rwl, 0, rwl->rwl_name, INFSLP);
			rwl->rwl_waiters--;
		}
		rwl->rwl_readers++;
		return 0;
	case RW_DOWNGRADE:
		rwl->rwl_owner = 0;
		rwl->rwl_readers++;
		if (rwl->rwl_waiters)
			wakeup(rwl);
		return 0;
	default:
		return EINVAL;
	}
}

void
rw_exit(struct rwlock *rwl)
{
	if (rwl->rwl_owner != 0)
		rwl->rwl_owner = 0;
	else if (rwl->rwl_readers > 0)
		rwl->rwl_readers--;
	if (rwl->rwl_waiters)
		wakeup(rwl);
}

void
rw_enter_write(struct rwlock *rwl)
{
	(void)rw_enter(rwl, RW_WRITE);
}

void
rw_enter_read(struct rwlock *rwl)
{
	(void)rw_enter(rwl, RW_READ);
}

void
rw_exit_write(struct rwlock *rwl)
{
	rw_exit(rwl);
}

void
rw_exit_read(struct rwlock *rwl)
{
	rw_exit(rwl);
}

int
rw_status(struct rwlock *rwl)
{
	if (rwl->rwl_owner != 0)
		return rwl->rwl_owner == (unsigned long)obsd_hc_self() ?
		    RW_WRITE : RW_WRITE_OTHER;
	return rwl->rwl_readers ? RW_READ : 0;
}

/* ---------------------------------------------------------------- reference counts */

void
refcnt_init(struct refcnt *r)
{
	r->r_refs = 1;
	r->r_traceidx = 0;
}

void
refcnt_init_trace(struct refcnt *r, int idx)
{
	refcnt_init(r);
}

void
refcnt_take(struct refcnt *r)
{
	r->r_refs++;
}

int
refcnt_rele(struct refcnt *r)
{
	return --r->r_refs == 0;
}

void
refcnt_rele_wake(struct refcnt *r)
{
	if (refcnt_rele(r))
		wakeup(r);
}

void
refcnt_finalize(struct refcnt *r, const char *wmesg)
{
	r->r_refs--;
	while (r->r_refs > 0)
		(void)tsleep_nsec(r, 0, wmesg, INFSLP);
}

unsigned int
refcnt_read(const struct refcnt *r)
{
	return r->r_refs;
}

/* ---------------------------------------------------------------- memory */

/* How large an allocation was, kept before it */
struct mhead {
	size_t	size;
	size_t	pad;
};

void *
malloc(size_t size, int type, int flags)
{
	struct mhead *h = obsd_hc_malloc(sizeof(*h) + size);

	if (h == NULL)
		return NULL;
	h->size = size;
	if (flags & M_ZERO)
		memset(h + 1, 0, size);
	return h + 1;
}

void *
mallocarray(size_t nmemb, size_t size, int type, int flags)
{
	if (size != 0 && nmemb > SIZE_MAX / size)
		return NULL;
	return malloc(nmemb * size, type, flags);
}

void
free(void *addr, int type, size_t freedsize)
{
	if (addr != NULL)
		obsd_hc_free((struct mhead *)addr - 1);
}

/*
 * The pools: items of one size, those given back kept for the next.
 * Items of a cluster's size or more (the mbufs' clusters, which the card
 * reaches by DMA as one piece) are whole pages, physically contiguous,
 * below 4GB, from the hypercalls of the card's DMA memory; the rest come
 * from the kernel's heap.
 */
struct pool_item {
	struct pool_item	*next;
};

struct pool_allocator pool_allocator_multi;
const struct kmem_pa_mode kp_dma_contig;
const struct kmem_pa_mode kp_mbuf_contig;

void
pool_init(struct pool *pp, size_t size, u_int align, int ipl, int flags,
    const char *wchan, struct pool_allocator *palloc)
{
	memset(pp, 0, sizeof(*pp));
	if (size < sizeof(struct pool_item))
		size = sizeof(struct pool_item);
	pp->pr_size = size;
	pp->pr_align = align;
	pp->pr_wchan = wchan;
	pp->pr_flags = flags;
	pp->pr_ipl = ipl;
}

void
pool_cache_init(struct pool *pp)
{
}

void
pool_set_constraints(struct pool *pp, const struct kmem_pa_mode *mode)
{
}

void
pool_setlowat(struct pool *pp, int n)
{
}

void
pool_sethiwat(struct pool *pp, int n)
{
}

int
pool_sethardlimit(struct pool *pp, u_int n)
{
	pp->pr_hardlimit = n;
	return 0;
}

void
pool_wakeup(struct pool *pp)
{
}

void
pool_destroy(struct pool *pp)
{
}

unsigned int	obsd_pool_fails;

void *
pool_get(struct pool *pp, int flags)
{
	struct pool_item *it = (struct pool_item *)pp->pr_cache;
	unsigned long pa, va;
	void *v;

	if (it != NULL) {
		pp->pr_cache = (struct cpumem *)it->next;
		pp->pr_nitems--;
		v = it;
	} else if (pp->pr_size >= MCLBYTES) {
		if (rumpcomp_pci_dmalloc(pp->pr_size, PAGE_SIZE, &pa, &va) != 0) {
			obsd_pool_fails++;
			return NULL;
		}
		v = (void *)va;
	} else {
		v = obsd_hc_malloc(pp->pr_size);
		if (v == NULL) {
			obsd_pool_fails++;
			return NULL;
		}
	}
	pp->pr_nout++;
	if (flags & PR_ZERO)
		memset(v, 0, pp->pr_size);
	return v;
}

void
pool_put(struct pool *pp, void *v)
{
	struct pool_item *it = v;

	if (v == NULL)
		return;
	it->next = (struct pool_item *)pp->pr_cache;
	pp->pr_cache = (struct cpumem *)it;
	pp->pr_nitems++;
	pp->pr_nout--;
}

/* The counters, of one processor */
struct cpumem *
counters_alloc_ncpus(struct cpumem *cm, unsigned int n)
{
	void *p = malloc(n * sizeof(uint64_t), M_COUNTERS, M_WAITOK | M_ZERO);

	return (struct cpumem *)p;
}

struct cpumem *
counters_alloc(unsigned int n)
{
	return counters_alloc_ncpus(NULL, n);
}

long	nmbclust = 4096;

/* ---------------------------------------------------------------- time */

void
getmicrouptime(struct timeval *tv)
{
	uint64_t ns = obsd_hc_nsec();

	tv->tv_sec = ns / 1000000000ULL;
	tv->tv_usec = (ns % 1000000000ULL) / 1000;
}

void
microuptime(struct timeval *tv)
{
	getmicrouptime(tv);
}

void
nanouptime(struct timespec *ts)
{
	uint64_t ns = obsd_hc_nsec();

	ts->tv_sec = ns / 1000000000ULL;
	ts->tv_nsec = ns % 1000000000ULL;
}

time_t
getuptime(void)
{
	return (time_t)(obsd_hc_nsec() / 1000000000ULL);
}

uint64_t
getnsecuptime(void)
{
	return obsd_hc_nsec();
}

void
microtime(struct timeval *tv)
{
	long long sec;
	long usec;

	obsd_hc_walltime(&sec, &usec);
	tv->tv_sec = sec;
	tv->tv_usec = usec;
}

void
getmicrotime(struct timeval *tv)
{
	microtime(tv);
}

void
microboottime(struct timeval *tv)
{
	struct timeval now, up;

	microtime(&now);
	getmicrouptime(&up);
	timersub(&now, &up, tv);
}

/* As OpenBSD's: true when mininterval has passed since *lasttime, which then becomes now */
int
ratecheck(struct timeval *lasttime, const struct timeval *mininterval)
{
	struct timeval tv, delta;
	int rv = 0;

	getmicrouptime(&tv);
	timersub(&tv, lasttime, &delta);
	if (timercmp(&delta, mininterval, >=) ||
	    (lasttime->tv_sec == 0 && lasttime->tv_usec == 0)) {
		*lasttime = tv;
		rv = 1;
	}
	return rv;
}

/* As OpenBSD's: true while fewer than maxpps events came in the current second */
int
ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps)
{
	struct timeval tv, delta;
	int rv;

	getmicrouptime(&tv);
	timersub(&tv, lasttime, &delta);
	if (delta.tv_sec >= 1 || (lasttime->tv_sec == 0 && lasttime->tv_usec == 0)) {
		*lasttime = tv;
		*curpps = 0;
	}
	rv = maxpps < 0 || *curpps < maxpps;
	if (*curpps + 1 > *curpps)
		*curpps = *curpps + 1;
	return rv;
}

static void
obsd_delay(int us)
{
	if (us > 0)
		obsd_hc_delay((unsigned int)us);
}

void	(*delay_func)(int) = obsd_delay;

/* The processor's structure, of which the layer reads nothing that matters */
struct cpu_info_full cpu_info_full_primary;

int	cold = 0;

/* ---------------------------------------------------------------- random numbers */

uint32_t
arc4random(void)
{
	uint32_t v;

	obsd_hc_random(&v, sizeof(v));
	return v;
}

void
arc4random_buf(void *buf, size_t n)
{
	obsd_hc_random(buf, n);
}

uint32_t
arc4random_uniform(uint32_t upper)
{
	uint32_t r, min;

	if (upper < 2)
		return 0;
	min = -upper % upper;
	for (;;) {
		r = arc4random();
		if (r >= min)
			break;
	}
	return r % upper;
}

/* ---------------------------------------------------------------- what libkern gives */

#undef memcpy
#undef memmove
#undef memset
#undef memcmp

void *
memcpy(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	while (n-- > 0)
		*d++ = *s++;
	return dst;
}

void *
memmove(void *dst, const void *src, size_t n)
{
	unsigned char *d = dst;
	const unsigned char *s = src;

	if (d < s) {
		while (n-- > 0)
			*d++ = *s++;
	} else {
		d += n;
		s += n;
		while (n-- > 0)
			*--d = *--s;
	}
	return dst;
}

void *
memset(void *dst, int c, size_t n)
{
	unsigned char *d = dst;

	while (n-- > 0)
		*d++ = (unsigned char)c;
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;

	for (; n > 0; n--, p++, q++) {
		if (*p != *q)
			return *p - *q;
	}
	return 0;
}

void
explicit_bzero(void *p, size_t n)
{
	volatile unsigned char *d = p;

	while (n-- > 0)
		*d++ = 0;
}

int
timingsafe_bcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	int r = 0;

	for (; n > 0; n--)
		r |= *p++ ^ *q++;
	return r != 0;
}

int
strcmp(const char *a, const char *b)
{
	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

int
strncmp(const char *a, const char *b, size_t n)
{
	for (; n > 0; n--, a++, b++) {
		if (*a != *b)
			return (unsigned char)*a - (unsigned char)*b;
		if (*a == '\0')
			break;
	}
	return 0;
}

size_t
strlen(const char *s)
{
	const char *p = s;

	while (*p != '\0')
		p++;
	return p - s;
}

size_t
strlcpy(char *dst, const char *src, size_t size)
{
	size_t n = strlen(src);

	if (size > 0) {
		size_t k = (n >= size) ? size - 1 : n;

		memcpy(dst, src, k);
		dst[k] = '\0';
	}
	return n;
}

/* An ioctl's argument is the kernel's own memory here */
int
copyin(const void *uaddr, void *kaddr, size_t len)
{
	memcpy(kaddr, uaddr, len);
	return 0;
}

int
copyout(const void *kaddr, void *uaddr, size_t len)
{
	memcpy(uaddr, kaddr, len);
	return 0;
}

/* Every caller is the kernel */
int
suser(struct proc *p)
{
	return 0;
}

int
sysctl_int(void *oldp, size_t *oldlenp, void *newp, size_t newlen, int *valp)
{
	return EOPNOTSUPP;
}

int
sysctl_rdint(void *oldp, size_t *oldlenp, void *newp, int val)
{
	return EOPNOTSUPP;
}

/* ---------------------------------------------------------------- the console */

static void
put_num(char **p, char *e, unsigned long long v, int base, int upper, int width,
    int zero, int left, int neg)
{
	char buf[32];
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int n = 0, pad;

	do {
		buf[n++] = digits[v % base];
		v /= base;
	} while (v != 0 && n < (int)sizeof(buf));
	pad = width - n - (neg ? 1 : 0);
	if (neg && zero && *p < e)
		*(*p)++ = '-';
	if (!left) {
		while (pad-- > 0 && *p < e)
			*(*p)++ = zero ? '0' : ' ';
	}
	if (neg && !zero && *p < e)
		*(*p)++ = '-';
	while (n > 0 && *p < e)
		*(*p)++ = buf[--n];
	if (left) {
		while (pad-- > 0 && *p < e)
			*(*p)++ = ' ';
	}
}

/*
 * The formats the layer prints with: flags - and 0, a width, a
 * precision for strings, the lengths h, l, ll, q, z and t, and the
 * conversions d, i, u, x, X, o, c, s, p, % and OpenBSD's %b (a number,
 * then the names of its bits)
 */
int
vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	char *p = buf, *e = buf + (size > 0 ? size - 1 : 0);
	int total;

	for (; *fmt != '\0'; fmt++) {
		int left = 0, zero = 0, width = 0, prec = -1, lng = 0;
		unsigned long long v;
		long long sv;
		const char *s;

		if (*fmt != '%') {
			if (p < e)
				*p++ = *fmt;
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = 1;
			else if (*fmt == '0')
				zero = 1;
			else if (*fmt == '+' || *fmt == ' ' || *fmt == '#')
				;
			else
				break;
		}
		if (*fmt == '*') {
			width = va_arg(ap, int);
			fmt++;
		} else {
			while (*fmt >= '0' && *fmt <= '9')
				width = width * 10 + (*fmt++ - '0');
		}
		if (*fmt == '.') {
			fmt++;
			prec = 0;
			if (*fmt == '*') {
				prec = va_arg(ap, int);
				fmt++;
			} else {
				while (*fmt >= '0' && *fmt <= '9')
					prec = prec * 10 + (*fmt++ - '0');
			}
		}
		for (;; fmt++) {
			if (*fmt == 'h')
				;
			else if (*fmt == 'l')
				lng++;
			else if (*fmt == 'q')
				lng = 2;
			else if (*fmt == 'z' || *fmt == 't')
				lng = 1;
			else
				break;
		}
		switch (*fmt) {
		case 'd':
		case 'i':
			sv = lng >= 2 ? va_arg(ap, long long) :
			    lng == 1 ? va_arg(ap, long) : va_arg(ap, int);
			put_num(&p, e, sv < 0 ? -(unsigned long long)sv : (unsigned long long)sv,
			    10, 0, width, zero, left, sv < 0);
			break;
		case 'u':
		case 'x':
		case 'X':
		case 'o':
			v = lng >= 2 ? va_arg(ap, unsigned long long) :
			    lng == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
			put_num(&p, e, v, *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16,
			    *fmt == 'X', width, zero, left, 0);
			break;
		case 'p':
			v = (unsigned long)va_arg(ap, void *);
			if (p < e)
				*p++ = '0';
			if (p < e)
				*p++ = 'x';
			put_num(&p, e, v, 16, 0, 0, 0, 0, 0);
			break;
		case 'c':
			if (p < e)
				*p++ = (char)va_arg(ap, int);
			break;
		case 's':
			s = va_arg(ap, const char *);
			if (s == NULL)
				s = "(null)";
			{
				int n = 0, pad;

				while (s[n] != '\0' && (prec < 0 || n < prec))
					n++;
				pad = width - n;
				while (!left && pad-- > 0 && p < e)
					*p++ = ' ';
				while (n-- > 0 && p < e)
					*p++ = *s++;
				while (left && pad-- > 0 && p < e)
					*p++ = ' ';
			}
			break;
		case 'b':
			v = va_arg(ap, int);
			s = va_arg(ap, const char *);
			{
				int base = *s++, any = 0, bit;

				put_num(&p, e, v, base == 8 ? 8 : base == 10 ? 10 : 16, 0, 0, 0, 0, 0);
				while ((bit = *s++) != '\0') {
					if (v & (1ULL << (bit - 1))) {
						if (p < e)
							*p++ = any ? ',' : '<';
						any = 1;
						while (*s > ' ' && p < e)
							*p++ = *s++;
					}
					while (*s > ' ')
						s++;
				}
				if (any && p < e)
					*p++ = '>';
			}
			break;
		case '%':
			if (p < e)
				*p++ = '%';
			break;
		case '\0':
			fmt--;
			break;
		default:
			if (p < e)
				*p++ = *fmt;
			break;
		}
	}
	total = p - buf;
	if (size > 0)
		*p = '\0';
	return total;
}

int
snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}

/*
 * What the layer prints, gathered into lines: a part of a line waits
 * for its end, so that the console's lines are whole. A line longer
 * than 510 characters is cut there.
 */
static char	line[512];
static int	line_len;

static void
say(const char *s)
{
	for (; *s != '\0'; s++) {
		if (line_len < (int)sizeof(line) - 2)
			line[line_len++] = *s;
		if (*s == '\n' || line_len >= (int)sizeof(line) - 2) {
			if (line[line_len - 1] != '\n')
				line[line_len++] = '\n';
			line[line_len] = '\0';
			obsd_hc_puts(line);
			line_len = 0;
		}
	}
}

int
vprintf(const char *fmt, va_list ap)
{
	char buf[512];
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);

	say(buf);
	return n;
}

int
printf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vprintf(fmt, ap);
	va_end(ap);
	return n;
}

void
log(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	(void)vprintf(fmt, ap);
	va_end(ap);
}

/* The layer cannot go on: said, and the task that found it stops for good */
void
panic(const char *fmt, ...)
{
	va_list ap;
	int sem;

	say("obsd: panic: ");
	va_start(ap, fmt);
	(void)vprintf(fmt, ap);
	va_end(ap);
	say("\n");
	(void)obsd_lock_drop();
	sem = obsd_hc_sem_new();
	for (;;)
		(void)obsd_hc_sem_wait(sem, -1);
}

/* ---------------------------------------------------------------- the start */

static struct device	*mr_dev;
static void		(*mr_fn)(struct device *);

void
config_mountroot(struct device *dev, void (*fn)(struct device *))
{
	mr_dev = dev;
	mr_fn = fn;
}

void
obsd_mountroot_run(void)
{
	void (*fn)(struct device *) = mr_fn;

	mr_fn = NULL;
	if (fn != NULL)
		(*fn)(mr_dev);
}

void	mbinit(void);

void
obsd_init(void)
{
	static int done;

	if (done)
		return;
	done = 1;
	big_mtx = obsd_hc_mtx_new();
	to_sem = obsd_hc_sem_new();
	obsd_lock();
	mbinit();
	(void)taskq_start(&tq_sys);
	(void)taskq_start(&tq_sysmp);
	(void)obsd_task(timeout_main, NULL, OBSD_PRI_TIMER);
	obsd_net_init();
	obsd_unlock();
}
