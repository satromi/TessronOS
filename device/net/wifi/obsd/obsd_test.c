/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_test.c (x64 PC, WIFI_NET80211=obsd)
 *	What the layer of OpenBSD's IEEE 802.11 code and iwm stands on,
 *	tried on a machine with no card (the kernel's test group obsdwifi,
 *	tests/ktest/ktest_obsdwifi.c): the lock and sleeping, timeouts, task
 *	queues, read/write locks, mbufs and their clusters as the card
 *	reaches them by DMA, the transmit queue, the formats of printf, and
 *	the ciphers net80211 protects frames with, against the published
 *	vectors. Compiled with OpenBSD's kernel headers.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/rwlock.h>
#include <sys/timeout.h>
#include <sys/task.h>
#include <sys/socket.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_var.h>

#include <crypto/aes.h>
#include <crypto/md5.h>
#include <crypto/sha2.h>
#include <crypto/sha1.h>
#include <crypto/cmac.h>
#include <crypto/hmac.h>

#include "obsd_glue.h"

extern struct bus_dma_tag obsd_dma_tag;

static int	nfail;

#define CHECK(c, what) \
	do { if (!(c)) { nfail++; printf("obsd: test: FAILED: %s (%s, line %d)\n", what, #c, __LINE__); } } \
	while (0)

/* ---------------------------------------------------------------- sleeping and timeouts */

static int	to_fired, to_other;
static uint64_t	to_at;

static void
to_fn(void *arg)
{
	to_fired++;
	to_at = getnsecuptime();
	wakeup(&to_fired);
}

static void
to_never(void *arg)
{
	to_other++;
}

static void
test_timeout(void)
{
	struct timeout a, b;
	uint64_t t0;
	int r;

	timeout_set(&a, to_fn, NULL);
	timeout_set(&b, to_never, NULL);
	t0 = getnsecuptime();
	CHECK(timeout_add_msec(&a, 30) == 1, "a timeout added");
	CHECK(timeout_add_msec(&b, 10) == 1, "another added");
	CHECK(timeout_pending(&b), "pending");
	CHECK(timeout_del(&b) == 1, "deleted while pending");
	CHECK(!timeout_pending(&b), "no longer pending");
	r = tsleep_nsec(&to_fired, 0, "obtest", 2000000000ULL);
	CHECK(r == 0, "woken by the timeout, not by the time running out");
	CHECK(to_fired == 1, "the timeout ran once");
	CHECK(to_at - t0 >= 25000000ULL, "not before it was due");
	r = tsleep_nsec(&to_other, 0, "obtest", 50000000ULL);
	CHECK(r == EWOULDBLOCK, "nothing wakes a sleep of 50 ms");
	CHECK(to_other == 0, "the deleted timeout did not run");
}

/* ---------------------------------------------------------------- task queues and rwlocks */

static struct rwlock	tl;
static int		tk_ran, tk_has_lock, tk_let_go;

static void
tk_fn(void *arg)
{
	tk_ran++;
	rw_enter_write(&tl);
	tk_has_lock = 1;
	wakeup(&tk_has_lock);
	/* holds it asleep, as iwm holds its ioctl lock while the firmware answers */
	(void)tsleep_nsec(&tk_let_go, 0, "obtest", 60000000ULL);
	rw_exit(&tl);
	tk_has_lock = 0;
}

static void
test_task(void)
{
	struct taskq *tq;
	struct task t;
	int r;

	rw_init(&tl, "obtest");
	tq = taskq_create("obtest", 1, IPL_NET, 0);
	CHECK(tq != NULL, "a task queue made");
	if (tq == NULL)
		return;
	task_set(&t, tk_fn, NULL);
	CHECK(task_add(tq, &t) == 1, "a task added");
	CHECK(task_add(tq, &t) == 0, "the same task not added twice");
	while (!tk_has_lock)
		(void)tsleep_nsec(&tk_has_lock, 0, "obtest", 1000000000ULL);
	CHECK(tk_ran == 1, "the task ran");
	r = rw_enter(&tl, RW_WRITE | RW_NOSLEEP);
	CHECK(r == EBUSY, "the lock held by the task, asleep, is busy");
	r = rw_enter(&tl, RW_WRITE);
	CHECK(r == 0 && tk_has_lock == 0, "taken once the task let it go");
	rw_exit(&tl);
}

/* ---------------------------------------------------------------- mbufs and DMA */

static void
test_mbuf(void)
{
	struct mbuf *m, *m2, *m3;
	bus_dmamap_t map;
	uint8_t buf[300];
	int i, error;

	m = m_gethdr(M_DONTWAIT, MT_DATA);
	CHECK(m != NULL, "an mbuf");
	if (m == NULL)
		return;
	m2 = MCLGETL(m, M_DONTWAIT, 4096);
	CHECK(m2 != NULL && (m->m_flags & M_EXT), "a cluster of 4096 bytes");
	m->m_len = m->m_pkthdr.len = 4096;
	error = bus_dmamap_create(&obsd_dma_tag, 4096, 1, 4096, 0, BUS_DMA_NOWAIT, &map);
	CHECK(error == 0, "a DMA map of one segment");
	if (error == 0) {
		error = bus_dmamap_load_mbuf(&obsd_dma_tag, map, m, BUS_DMA_NOWAIT);
		CHECK(error == 0 && map->dm_nsegs == 1, "the cluster is one physical piece");
		CHECK(map->dm_segs[0].ds_len == 4096, "the whole of it");
		CHECK(map->dm_segs[0].ds_addr != 0 && map->dm_segs[0].ds_addr < (1ULL << 32),
		    "below 4GB");
		bus_dmamap_unload(&obsd_dma_tag, map);
		bus_dmamap_destroy(&obsd_dma_tag, map);
	}
	m_freem(m);

	/* a chain of three, made one */
	for (i = 0; i < (int)sizeof(buf); i++)
		buf[i] = (uint8_t)i;
	m = m_gethdr(M_DONTWAIT, MT_DATA);
	m2 = m_get(M_DONTWAIT, MT_DATA);
	m3 = m_get(M_DONTWAIT, MT_DATA);
	CHECK(m != NULL && m2 != NULL && m3 != NULL, "three mbufs");
	if (m == NULL || m2 == NULL || m3 == NULL)
		return;
	memcpy(mtod(m, void *), buf, 100);
	m->m_len = 100;
	memcpy(mtod(m2, void *), buf + 100, 100);
	m2->m_len = 100;
	memcpy(mtod(m3, void *), buf + 200, 100);
	m3->m_len = 100;
	m->m_next = m2;
	m2->m_next = m3;
	m->m_pkthdr.len = 300;
	error = m_defrag(m, M_DONTWAIT);
	CHECK(error == 0 && m->m_next == NULL && m->m_len == 300, "m_defrag made it one");
	memset(buf, 0, sizeof(buf));
	m_copydata(m, 0, 300, buf);
	for (i = 0; i < 300 && buf[i] == (uint8_t)i; i++)
		;
	CHECK(i == 300, "and its bytes are the same");
	m_freem(m);
}

/* ---------------------------------------------------------------- the transmit queue */

static int	start_calls;

static void
fake_start(struct ifnet *ifp)
{
	start_calls++;
}

static void
test_ifq(void)
{
	struct ifnet *ifp;
	struct mbuf *m;
	int i, error = 0, n = 0;

	ifp = malloc(sizeof(*ifp), M_DEVBUF, M_WAITOK | M_ZERO);
	CHECK(ifp != NULL, "an interface");
	if (ifp == NULL)
		return;
	strlcpy(ifp->if_xname, "obtest0", sizeof(ifp->if_xname));
	ifp->if_start = fake_start;
	if_attach(ifp);
	for (i = 0; i < 300 && error == 0; i++) {
		m = m_gethdr(M_DONTWAIT, MT_DATA);
		if (m == NULL)
			break;
		m->m_len = m->m_pkthdr.len = 60;
		error = if_enqueue(ifp, m);
		if (error == 0)
			n++;
	}
	CHECK(error == ENOBUFS && n == ifp->if_snd.ifq_maxlen, "the queue holds what it may, then refuses");
	CHECK(start_calls == n, "the driver's start called for each");
	CHECK(ifq_purge(&ifp->if_snd) == (unsigned int)n, "all purged");
	CHECK(ifq_dequeue(&ifp->if_snd) == NULL, "none left");
}

/* ---------------------------------------------------------------- printf and the ciphers */

static int
same(const uint8_t *a, const uint8_t *b, int n)
{
	return memcmp(a, b, n) == 0;
}

static void
test_format(void)
{
	char s[64];

	snprintf(s, sizeof(s), "%02x:%5d|%-4s|%s|%llu|%b", 0xa, -42, "ab", "c", 123456789012ULL,
	    5, "\20\1ONE\2TWO\3THREE");
	CHECK(strcmp(s, "0a:  -42|ab  |c|123456789012|5<ONE,THREE>") == 0, "the formats of printf");
	if (strcmp(s, "0a:  -42|ab  |c|123456789012|5<ONE,THREE>") != 0)
		printf("obsd: test: printed \"%s\"\n", s);
}

static void
test_crypto(void)
{
	/* RFC 4493 example 2: AES-CMAC of 16 bytes (BIP and the EAPOL-Key MIC of AKM 6 and 8) */
	static const uint8_t key[16] = {
		0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
		0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
	static const uint8_t msg[16] = {
		0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
		0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a };
	static const uint8_t cmac[16] = {
		0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
		0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c };
	/* FIPS 197 C.1: AES-128 of one block (CCMP) */
	static const uint8_t k197[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f };
	static const uint8_t p197[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };
	static const uint8_t c197[16] = {
		0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
		0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a };
	/* RFC 2202 test 2: HMAC-SHA1 */
	static const uint8_t hmac2[20] = {
		0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
		0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79 };
	AES_CMAC_CTX cctx;
	AES_CTX actx;
	HMAC_SHA1_CTX hctx;
	uint8_t out[20];

	AES_CMAC_Init(&cctx);
	AES_CMAC_SetKey(&cctx, key);
	AES_CMAC_Update(&cctx, msg, sizeof(msg));
	AES_CMAC_Final(out, &cctx);
	CHECK(same(out, cmac, 16), "AES-CMAC as RFC 4493");

	AES_Setkey(&actx, k197, 16);
	AES_Encrypt(&actx, p197, out);
	CHECK(same(out, c197, 16), "AES-128 as FIPS 197");

	HMAC_SHA1_Init(&hctx, (const uint8_t *)"Jefe", 4);
	HMAC_SHA1_Update(&hctx, (const uint8_t *)"what do ya want for nothing?", 28);
	HMAC_SHA1_Final(out, &hctx);
	CHECK(same(out, hmac2, 20), "HMAC-SHA1 as RFC 2202");
}

/* All of it: the failures, 0 when everything held */
int
rump_tswifi_selftest(void)
{
	obsd_init();
	obsd_lock();
	nfail = 0;
	test_format();
	test_crypto();
	test_timeout();
	test_task();
	test_mbuf();
	test_ifq();
	printf("obsd: test: %d failed\n", nfail);
	obsd_unlock();
	return nfail;
}
