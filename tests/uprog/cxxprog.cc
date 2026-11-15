/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cxxprog.cc
 *	A program in C++ the kernel tests start as a process
 *	(tests/ktest/ktest_cxx.c, /boot/CXXPROG.ELF), built with
 *	lib/libcxxrt and lib/libpthread (design 17.19).
 *
 *	It tries what a C++ program leans on -- constructors of statics,
 *	virtual calls, exceptions and RTTI, the standard containers, the
 *	heap, thread local variables, std::thread with std::mutex and
 *	std::condition_variable, std::call_once, statics made from several
 *	threads, the POSIX threads underneath -- and the memory calls of
 *	include/ts/umem.h with what they must refuse, and random bytes by
 *	getrandom, getentropy and /dev/urandom (the object 乱数), IPv6
 *	through the BSD sockets over the loopback interface, and code it
 *	writes and runs (exec_pages; run_mode for the faults). Each step says
 *	"cxxprog: <step> ok" on the console; the exit code is 0 when all
 *	went as they should, or the number of the first step that did not.
 */

#include <config.h>
#include <stdint.h>
#include <tk/typedef.h>
#include <tk/errno.h>
#include <ts/umem.h>

#include <cstdio>
#include <cstring>
#include <new>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <memory>
#include <functional>
#include <stdexcept>
#include <typeinfo>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <shared_mutex>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ts/ob.h>

extern "C" {
ER	dt_gettime( int64_t *p_t );
ER	ts_ref_prc( int pid, void *pk );
int	ts_get_pid( void );
SZ	ts_get_arg( void *buf, SZ size );
}

static int	failed = 0;

#define STEP(n, name, cond) \
	do { \
		if ( cond ) { \
			std::printf("cxxprog: %s ok\n", name); \
		} else { \
			std::printf("cxxprog: %s FAILED\n", name); \
			if ( failed == 0 ) failed = (n); \
		} \
	} while (0)

/* ---------------------------------------------------------------- statics */

static int	ctor_ran = 0;

struct AtStart {
	int	v;
	AtStart() : v(42) { ctor_ran++; }
};

static AtStart	at_start;

/* ---------------------------------------------------------------- virtual calls, RTTI */

struct Shape {
	virtual ~Shape() {}
	virtual int area() const = 0;
};

struct Rect : Shape {
	int	w, h;
	Rect( int a, int b ) : w(a), h(b) {}
	int area() const override { return w * h; }
};

struct Square : Rect {
	explicit Square( int a ) : Rect(a, a) {}
};

static int shape_area( const Shape &s )
{
	return s.area();
}

/* ---------------------------------------------------------------- exceptions */

struct Oops : std::runtime_error {
	int	code;
	explicit Oops( int c ) : std::runtime_error("oops"), code(c) {}
};

static int thrower( int depth )
{
	std::string	keep = "frame " + std::to_string(depth);

	if ( depth == 0 ) throw Oops(7);
	return thrower(depth - 1) + (int)keep.size();
}

static bool exceptions_work()
{
	int	got = 0;

	try {
		thrower(10);
	} catch ( const Oops &e ) {
		got = e.code;
	}
	try {
		try {
			throw std::out_of_range("range");
		} catch ( ... ) {
			throw;
		}
	} catch ( const std::logic_error &e ) {
		got += ( std::strcmp(e.what(), "range") == 0 ) ? 100 : 0;
	}
	try {
		std::vector<int> v(3);
		(void)v.at(5);
	} catch ( const std::out_of_range & ) {
		got += 1000;
	}
	return got == 1107;
}

/* ---------------------------------------------------------------- threads */

thread_local int	tl_count = 5;
static std::atomic<int>	tl_sum{0};

static void tl_worker( int add )
{
	for ( int i = 0; i < 100; i++ ) tl_count += add;
	tl_sum += tl_count;
}

static int	statics_made = 0;

struct Slow {
	int	v;
	Slow() {
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		statics_made++;
		v = 99;
	}
};

static int use_slow()
{
	static Slow	s;

	return s.v;
}

static bool producer_consumer()
{
	std::mutex		m;
	std::condition_variable	cv;
	std::vector<int>	q;
	bool			done = false;
	long			sum = 0;

	std::thread consumer([&] {
		std::unique_lock<std::mutex> lk(m);
		for ( ;; ) {
			cv.wait(lk, [&] { return !q.empty() || done; });
			while ( !q.empty() ) {
				sum += q.back();
				q.pop_back();
			}
			if ( done ) break;
		}
	});
	std::thread producer([&] {
		for ( int i = 1; i <= 1000; i++ ) {
			{
				std::lock_guard<std::mutex> lk(m);
				q.push_back(i);
			}
			cv.notify_one();
			if ( i % 100 == 0 ) std::this_thread::yield();
		}
		{
			std::lock_guard<std::mutex> lk(m);
			done = true;
		}
		cv.notify_all();
	});
	producer.join();
	consumer.join();
	return sum == 500500;
}

static bool mutex_counts()
{
	std::mutex	m;
	long		n = 0;
	std::vector<std::thread> ts;

	for ( int t = 0; t < 4; t++ ) {
		ts.emplace_back([&] {
			for ( int i = 0; i < 20000; i++ ) {
				std::lock_guard<std::mutex> lk(m);
				n++;
			}
		});
	}
	for ( auto &t : ts ) t.join();
	return n == 80000;
}

/* ---------------------------------------------------------------- POSIX threads */

static pthread_key_t	key;
static std::atomic<int>	key_dtors{0};

static void key_dtor( void *p )
{
	if ( p == (void *)0x1234 ) key_dtors++;
}

static void *pt_worker( void *arg )
{
	pthread_setspecific(key, (void *)0x1234);
	return (void *)( (intptr_t)arg * 2 );
}

static bool pthreads_work()
{
	pthread_t	t[3];
	void		*r;
	long		sum = 0;

	if ( pthread_key_create(&key, key_dtor) != 0 ) return false;
	for ( int i = 0; i < 3; i++ ) {
		if ( pthread_create(&t[i], nullptr, pt_worker, (void *)(intptr_t)( i + 1 )) != 0 ) {
			return false;
		}
	}
	for ( int i = 0; i < 3; i++ ) {
		pthread_join(t[i], &r);
		sum += (intptr_t)r;
	}
	return sum == 12 && key_dtors == 3;
}

static bool timed_wait()
{
	pthread_mutex_t	m = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t	c = PTHREAD_COND_INITIALIZER;
	struct timespec	ts, t0, t1;
	int		r;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_nsec += 50 * 1000000L;
	if ( ts.tv_nsec >= 1000000000L ) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&m);
	r = pthread_cond_timedwait(&c, &m, &ts);
	pthread_mutex_unlock(&m);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	long ms = ( t1.tv_sec - t0.tv_sec ) * 1000 + ( t1.tv_nsec - t0.tv_nsec ) / 1000000;
	std::printf("cxxprog: timed wait %ld ms\n", ms);
	return r == ETIMEDOUT && ms >= 45 && ms < 2000;
}

static bool detached_and_sem()
{
	static sem_t	s;
	pthread_attr_t	a;
	pthread_t	t;
	int		ok = 1;

	sem_init(&s, 0, 0);
	pthread_attr_init(&a);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	for ( int i = 0; i < 5 && ok; i++ ) {
		ok = ( pthread_create(&t, &a, [](void *) -> void * { sem_post(&s); return nullptr; },
				      nullptr) == 0 );
		sem_wait(&s);
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return ok;
}

/*
 * A mutex ended as soon as its holder lets it go (POSIX allows a mutex
 * unlocked to be destroyed): a caller waits in a frame of its own for a
 * worker thread to finish a job, the worker taking the frame's mutex while
 * the caller still holds it (so that it waits, queued), and the caller
 * writes over the frame the moment it has the mutex back. The worker's
 * unlock must not read the mutex after the caller may have it: what it
 * would find there points nowhere, and it would write through it.
 */
struct EndWait {
	pthread_mutex_t		m;
	pthread_cond_t		c;
	volatile int		done;
};

static std::atomic<EndWait *>	end_job{nullptr};
static std::atomic<int>		end_trying{0};
static std::atomic<bool>	end_stop{false};

static void end_spin( long n )
{
	for ( long i = 0; i < n; i++ ) __asm__ __volatile__("" ::: "memory");
}

static void end_worker()
{
	for ( ;; ) {
		EndWait *w = end_job.exchange(nullptr);
		if ( w == nullptr ) {
			if ( end_stop ) return;
			std::this_thread::yield();
			continue;
		}
		end_trying = 1;
		pthread_mutex_lock(&w->m);
		w->done = 1;
		pthread_cond_signal(&w->c);
		pthread_mutex_unlock(&w->m);
	}
}

static bool end_one()
{
	alignas(16) unsigned char frame[256];
	EndWait *w = new ( frame ) EndWait;

	pthread_mutex_init(&w->m, nullptr);
	pthread_cond_init(&w->c, nullptr);
	w->done = 0;
	pthread_mutex_lock(&w->m);
	end_trying = 0;
	end_job = w;
	/* held a moment after the worker tries it: it is queued on the mutex */
	for ( long i = 0; i < 2000000 && end_trying == 0; i++ ) end_spin(1);
	end_spin(2000);
	while ( !w->done ) pthread_cond_wait(&w->c, &w->m);
	pthread_mutex_unlock(&w->m);
	pthread_cond_destroy(&w->c);
	if ( pthread_mutex_destroy(&w->m) != 0 ) return false;
	/* the frame is the caller's again: anything read through it now points nowhere */
	std::memset(frame, 0x5a, sizeof(frame));
	return true;
}

static bool mutex_ended()
{
	bool	ok = true;
	int	n = 0;

	end_stop = false;
	std::thread worker(end_worker);
	for ( ; n < 4000 && ok; n++ ) ok = end_one();
	end_stop = true;
	worker.join();
	std::printf("cxxprog: a mutex ended after its unlock: %d times\n", n);
	return ok;
}

/*
 * Waits with short limits that end about when they are let go: waiters
 * on a condition variable with limits of tens of microseconds, threads
 * taking a mutex with such limits, and threads taking the C library's
 * heap lock by turns, all at once. Each count is kept only under its
 * lock and must come out exact; a waiter let go twice, or let into a
 * lock another holds, makes it come out otherwise.
 */
struct RaceState {
	pthread_mutex_t		m;
	pthread_cond_t		c;
	long			posted, taken, timeouts;
	pthread_mutex_t		tm;
	long			tm_count, tm_missed;
	std::atomic<bool>	stop{false};
};

static void race_deadline( struct timespec *ts, long us )
{
	clock_gettime(CLOCK_REALTIME, ts);
	ts->tv_nsec += us * 1000L;
	while ( ts->tv_nsec >= 1000000000L ) {
		ts->tv_sec++;
		ts->tv_nsec -= 1000000000L;
	}
}

static bool waits_racing()
{
	static RaceState	s;
	const int		kWaiters = 4, kPosts = 6000, kTimedLockers = 3, kLocks = 4000, kHeap = 2;
	std::vector<std::thread> ts;
	std::atomic<long>	heap_rounds{0};

	pthread_mutex_init(&s.m, nullptr);
	pthread_cond_init(&s.c, nullptr);
	pthread_mutex_init(&s.tm, nullptr);
	s.posted = s.taken = s.timeouts = 0;
	s.tm_count = s.tm_missed = 0;
	s.stop = false;

	for ( int w = 0; w < kWaiters; w++ ) {
		ts.emplace_back([w] {
			unsigned r = 12345u + (unsigned)w * 7919u;
			pthread_mutex_lock(&s.m);
			for ( ;; ) {
				while ( s.taken >= s.posted && !s.stop ) {
					struct timespec dl;
					r = r * 1103515245u + 12345u;
					race_deadline(&dl, 5 + (long)( ( r >> 16 ) % 120 ));
					if ( pthread_cond_timedwait(&s.c, &s.m, &dl) == ETIMEDOUT ) s.timeouts++;
				}
				if ( s.taken >= s.posted ) break;
				s.taken++;
			}
			pthread_mutex_unlock(&s.m);
		});
	}
	for ( int l = 0; l < kTimedLockers; l++ ) {
		ts.emplace_back([l] {
			unsigned r = 777u + (unsigned)l * 104729u;
			for ( int i = 0; i < kLocks; ) {
				struct timespec dl;
				r = r * 1103515245u + 12345u;
				race_deadline(&dl, 2 + (long)( ( r >> 16 ) % 60 ));
				if ( pthread_mutex_timedlock(&s.tm, &dl) != 0 ) {
					__atomic_add_fetch(&s.tm_missed, 1, __ATOMIC_RELAXED);
					continue;
				}
				long v = s.tm_count;
				for ( int k = 0; k < (int)( r % 64 ); k++ ) __asm__ __volatile__("" ::: "memory");
				s.tm_count = v + 1;
				pthread_mutex_unlock(&s.tm);
				i++;
			}
		});
	}
	for ( int h = 0; h < kHeap; h++ ) {
		ts.emplace_back([&heap_rounds, h] {
			void *keep[64] = {};
			unsigned r = 99u + (unsigned)h;
			while ( !s.stop ) {
				r = r * 1103515245u + 12345u;
				int k = (int)( ( r >> 8 ) % 64 );
				std::free(keep[k]);
				keep[k] = std::malloc(16 + ( r >> 16 ) % 3000);
				if ( keep[k] != nullptr ) std::memset(keep[k], k, 16);
				heap_rounds++;
			}
			for ( void *p : keep ) std::free(p);
		});
	}
	for ( int i = 0; i < kPosts; i++ ) {
		pthread_mutex_lock(&s.m);
		s.posted++;
		if ( i % 7 == 0 ) pthread_cond_broadcast(&s.c);
		else pthread_cond_signal(&s.c);
		pthread_mutex_unlock(&s.m);
		if ( i % 3 == 0 ) std::this_thread::yield();
	}
	/* the waiters take the rest, then are let go */
	for ( int i = 0; i < 20000; i++ ) {
		pthread_mutex_lock(&s.m);
		bool all = s.taken >= s.posted;
		pthread_mutex_unlock(&s.m);
		if ( all ) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	pthread_mutex_lock(&s.m);
	s.stop = true;
	pthread_cond_broadcast(&s.c);
	pthread_mutex_unlock(&s.m);
	for ( auto &t : ts ) t.join();
	std::printf("cxxprog: waits racing: %ld of %ld taken, %ld limits reached; %ld of %d timed locks (%ld missed); %ld heap rounds\n",
		    s.taken, s.posted, s.timeouts, s.tm_count, kTimedLockers * kLocks, s.tm_missed, heap_rounds.load());
	return s.taken == kPosts && s.posted == kPosts && s.tm_count == (long)kTimedLockers * kLocks;
}

/* ---------------------------------------------------------------- memory */

static bool heap_big()
{
	const size_t	n = 64u * 1024 * 1024;
	char		*p = (char *)std::malloc(n);
	unsigned	sum = 0;

	if ( p == nullptr ) return false;
	for ( size_t i = 0; i < n; i += 4096 ) p[i] = (char)( i >> 12 );
	for ( size_t i = 0; i < n; i += 4096 ) sum += (unsigned char)p[i];
	std::free(p);
	return sum != 0;
}

static int umem_line;

#define UM_CHECK(c)	do { if ( !(c) ) { umem_line = __LINE__; return false; } } while (0)

static bool umem_calls()
{
	void	*p = nullptr, *q;
	char	*c;
	int64_t	t;

	/* aligned, reserved only */
	UM_CHECK(!( ts_map_mem(&p, 8 << 20, 1 << 20, TS_MEM_NONE) != E_OK ));
	UM_CHECK(!( ( (uintptr_t)p & ( ( 1 << 20 ) - 1 ) ) != 0 ));
	UM_CHECK(!( (uintptr_t)p < TS_MEM_BASE || (uintptr_t)p >= TS_MEM_END ));

	/* a reserved page with nothing behind it is not the process's to hand the kernel */
	UM_CHECK(!( dt_gettime((int64_t *)p) != E_MACV ));

	/* pages made in the middle: zero, writable */
	c = (char *)p + ( 1 << 20 );
	UM_CHECK(!( ts_ctl_mem(c, 1 << 20, TS_MEM_RW) != E_OK ));
	for ( int i = 0; i < ( 1 << 20 ); i += 4096 ) {
		UM_CHECK(!( c[i] != 0 ));
		c[i] = 1;
	}
	UM_CHECK(!( dt_gettime((int64_t *)( c + 64 )) != E_OK ));

	/* read only: the kernel may not write there */
	UM_CHECK(!( ts_ctl_mem(c, 4096, TS_MEM_READ) != E_OK ));
	UM_CHECK(!( dt_gettime((int64_t *)c) != E_MACV ));
	UM_CHECK(!( ts_ctl_mem(c, 4096, TS_MEM_RW) != E_OK ));
	UM_CHECK(!( c[0] != 1 ));				/* kept across */

	/* taken away and made again: zero */
	UM_CHECK(!( ts_ctl_mem(c, 1 << 20, TS_MEM_NONE) != E_OK ));
	UM_CHECK(!( ts_ctl_mem(c, 1 << 20, TS_MEM_RW) != E_OK ));
	UM_CHECK(!( c[0] != 0 || c[4096] != 0 ));

	/* a hole in the middle splits the reservation; both halves stay */
	UM_CHECK(!( ts_unm_mem(c, 1 << 20) != E_OK ));
	UM_CHECK(!( ts_ctl_mem(c, 4096, TS_MEM_RW) != E_PAR ));
	UM_CHECK(!( ts_ctl_mem(p, 4096, TS_MEM_RW) != E_OK ));
	UM_CHECK(!( ts_ctl_mem(c + ( 1 << 20 ), 4096, TS_MEM_RW) != E_OK ));

	/* the hole is free again for one who asks for it there */
	q = c;
	UM_CHECK(!( ts_map_mem(&q, 1 << 20, 0, TS_MEM_RW) != E_OK || q != c ));

	/* refused */
	q = nullptr;
	UM_CHECK(!( ts_map_mem(&q, 4096, 3 * 4096, TS_MEM_RW) != E_PAR ));
	UM_CHECK(!( ts_map_mem(&q, 4096, 0, 0x10) != E_PAR ));
	UM_CHECK(!( ts_ctl_mem((void *)0x500000, 4096, TS_MEM_RW) != E_PAR ));
	UM_CHECK(!( ts_unm_mem((char *)p + 100, 4096) != E_PAR ));

	/* executable pages are given (exec_pages tries them) */
	q = nullptr;
	UM_CHECK(!( ts_map_mem(&q, 4096, 0, TS_MEM_EXEC | TS_MEM_READ) != E_OK ));
	UM_CHECK(!( ts_unm_mem(q, 4096) != E_OK ));
	q = mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UM_CHECK(!( q == MAP_FAILED || munmap(q, 4096) != 0 ));

	/* mmap and its relatives */
	char *m = (char *)mmap(nullptr, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UM_CHECK(!( m == MAP_FAILED ));
	m[0] = 5;
	m[3 * 4096 - 1] = 6;
	UM_CHECK(!( madvise(m, 4096, MADV_DONTNEED) != 0 || m[0] != 0 ));
	UM_CHECK(!( munmap(m, 3 * 4096) != 0 ));

	/*
	 * A large writable mapping takes only the pages touched; a page
	 * never touched handed to the kernel is made for it; given up, the
	 * pages go back and read as zeros
	 */
	{
		struct { int pid, ppid; unsigned state; int exitcd, ntsk; uint64_t memsz; int maintsk; } r0, r1, r2;
		const size_t big = (size_t)256 << 20;
		char *b = (char *)mmap(nullptr, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		UM_CHECK(!( b == MAP_FAILED || ts_ref_prc(0, &r0) != E_OK ));
		for ( size_t i = 0; i < 16; i++ ) b[i << 20] = 1;
		UM_CHECK(!( dt_gettime((int64_t *)( b + ( (size_t)100 << 20 ) )) != E_OK ));
		UM_CHECK(!( ts_ref_prc(0, &r1) != E_OK ));
		UM_CHECK(!( r1.memsz < r0.memsz + 17 * 4096 || r1.memsz > r0.memsz + ( 4 << 20 ) ));
		UM_CHECK(!( madvise(b, big, MADV_DONTNEED) != 0 || ts_ref_prc(0, &r2) != E_OK ));
		UM_CHECK(!( r2.memsz + 15 * 4096 > r1.memsz ));
		UM_CHECK(!( b[0] != 0 || b[( (size_t)100 << 20 ) + 1] != 0 ));
		UM_CHECK(!( munmap(b, big) != 0 ));
	}

	UM_CHECK(!( ts_unm_mem(p, 8 << 20) != E_OK ));
	(void)t;
	return true;
}

/*
 * Code the program writes and runs (include/ts/umem.h). A function of
 * two instructions, "mov w0, #v; ret", is written into a page and
 * called: in pages made executable with mprotect, and in pages written
 * and run by turns (PROT_READ | PROT_WRITE | PROT_EXEC), where the
 * kernel turns each page on the fault of the access. Another thread,
 * on another processor while this one spins, runs the function after
 * this one rewrote it and must get the new value each time, without
 * cleaning a cache itself: the kernel makes the instructions fetchable
 * when a page becomes executable. Once more with the thread running the
 * function all the time while it is rewritten, which turns its page
 * back and forth. __builtin___clear_cache, which cleans and invalidates
 * the caches from EL0, must not fault.
 */
typedef int (*CODEFN)( void );

static int	exec_line;
static int	exec_flips;

#define EX_CHECK(c)	do { if ( !(c) ) { exec_line = __LINE__; return false; } } while (0)

static void code_put( void *page, int v )
{
#if defined(__x86_64__)
	volatile uint8_t	*b = (volatile uint8_t *)page;
	uint32_t		u = (uint32_t)v & 0xffff;

	b[0] = 0xb8;						/* mov $v, %eax */
	*(volatile uint32_t *)( b + 1 ) = u;			/* the value in one store, within a cache line */
	b[5] = 0xc3;						/* ret */
#else
	volatile uint32_t	*w = (volatile uint32_t *)page;

	w[0] = 0x52800000u | ( ( (uint32_t)v & 0xffff ) << 5 );	/* movz w0, #v */
	w[1] = 0xd65f03c0u;					/* ret */
#endif
}

static void code_put( CODEFN fn, int v )
{
	code_put((void *)fn, v);
}

static std::atomic<int>	ex_gen{0};		/* the value the function was last written with */
static std::atomic<int>	ex_ack{0};		/* what the other thread got for it */
static std::atomic<bool> ex_stop{false};
static std::atomic<int>	ex_bad{0};		/* values out of what may be seen */
static std::atomic<long> ex_calls{0};

/*
 * The threads spin, so that each keeps a processor of its own; now and
 * then they give way, so that on one processor the other gets to run.
 */
static void spin_pause( long n )
{
	if ( ( n & 255 ) == 255 ) std::this_thread::yield();
}

/* Runs the function each time the value changes, and answers what it got */
static void ex_runner( CODEFN fn )
{
	int	seen = 0;

	for ( long n = 0; !ex_stop.load(std::memory_order_acquire); n++ ) {
		int g = ex_gen.load(std::memory_order_acquire);

		if ( g == seen ) {
			spin_pause(n);
			continue;
		}
		seen = g;
		ex_ack.store(fn(), std::memory_order_release);
	}
}

/* Runs the function all the time; a value is the one written last or the next */
static void ex_spinner( CODEFN fn )
{
	for ( long n = 0; !ex_stop.load(std::memory_order_acquire); n++ ) {
		int g1 = ex_gen.load(std::memory_order_acquire);
		int v = fn();
		int g2 = ex_gen.load(std::memory_order_acquire);

		if ( v < g1 || v > g2 + 1 ) ex_bad++;
		ex_ack.store(v, std::memory_order_release);
		ex_calls++;
		spin_pause(n);
	}
}

static bool ack_wait( int v )
{
	for ( long n = 0; n < 20000000L; n++ ) {
		if ( ex_ack.load(std::memory_order_acquire) == v ) return true;
		spin_pause(n);
	}
	return false;
}

static bool exec_pages()
{
	const size_t	sz = 4 * 4096;
	char		*x, *j;
	CODEFN		fx, fj;

	/* mprotect: written while writable, run while executable */
	x = (char *)mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	EX_CHECK(x != MAP_FAILED);
	fx = (CODEFN)(void *)( x + 4096 );
	code_put(fx, 1234);
	EX_CHECK(mprotect(x, sz, PROT_READ | PROT_EXEC) == 0);
	EX_CHECK(fx() == 1234);
	EX_CHECK(mprotect(x, sz, PROT_READ | PROT_WRITE) == 0);
	code_put(fx, 4321);
	EX_CHECK(mprotect(x, sz, PROT_READ | PROT_EXEC) == 0);
	EX_CHECK(fx() == 4321);

	/* written and run by turns: made on first touch, turned by the faults */
	j = (char *)mmap(nullptr, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	EX_CHECK(j != MAP_FAILED);
	fj = (CODEFN)(void *)( j + 2 * 4096 );
	code_put(fj, 77);
	EX_CHECK(fj() == 77);
	for ( int i = 0; i < 50; i++ ) {
		code_put(fj, 1000 + i);
		if ( i & 1 ) __builtin___clear_cache((char *)fj, (char *)fj + 8);
		EX_CHECK(fj() == 1000 + i);
	}
	/* a page never touched is made, zero filled, when it is first read */
	EX_CHECK(*(volatile uint32_t *)( j + 3 * 4096 ) == 0);

	/* given up (madvise), the pages read as zeros and are still written and run by turns */
	EX_CHECK(madvise(j, sz, MADV_DONTNEED) == 0);
	EX_CHECK(*(volatile uint32_t *)fj == 0);
	code_put(fj, 88);
	EX_CHECK(fj() == 88);
	/* and pages that stay executable keep that */
	EX_CHECK(madvise(x, sz, MADV_DONTNEED) == 0);
	EX_CHECK(*(volatile uint32_t *)fx == 0);
	EX_CHECK(mprotect(x, sz, PROT_READ | PROT_WRITE) == 0);
	code_put(fx, 4321);
	EX_CHECK(mprotect(x, sz, PROT_READ | PROT_EXEC) == 0);
	EX_CHECK(fx() == 4321);

	/* another thread runs what this one rewrote: mprotect */
	ex_gen = 0;
	ex_ack = -1;
	ex_stop = false;
	{
		std::thread t(ex_runner, fx);
		for ( int g = 1; g <= 100; g++ ) {
			EX_CHECK(mprotect(x, sz, PROT_READ | PROT_WRITE) == 0);
			code_put(fx, g);
			EX_CHECK(mprotect(x, sz, PROT_READ | PROT_EXEC) == 0);
			ex_gen.store(g, std::memory_order_release);
			if ( !ack_wait(g) ) {
				ex_stop = true;
				t.join();
				EX_CHECK(false);
			}
		}
		ex_stop = true;
		t.join();
	}

	/* the same with pages run by turns: no mprotect, the faults turn them */
	ex_gen = 0;
	ex_ack = -1;
	ex_stop = false;
	{
		std::thread t(ex_runner, fj);
		for ( int g = 1; g <= 100; g++ ) {
			code_put(fj, g);
			ex_gen.store(g, std::memory_order_release);
			if ( !ack_wait(g) ) {
				ex_stop = true;
				t.join();
				EX_CHECK(false);
			}
		}
		ex_stop = true;
		t.join();
	}

	/* rewritten while the other thread runs it all the time */
	code_put(fj, 0);
	ex_gen = 0;
	ex_ack = -1;
	ex_bad = 0;
	ex_calls = 0;
	ex_stop = false;
	{
		std::thread t(ex_spinner, fj);
		for ( int g = 1; g <= 200; g++ ) {
			code_put(fj, g);
			ex_gen.store(g, std::memory_order_release);
			for ( long n = 0; n < 2000000L && ex_calls.load() < (long)g * 4; n++ ) {
				spin_pause(n);
			}
		}
		bool last = ack_wait(200);
		ex_stop = true;
		t.join();
		exec_flips = (int)ex_calls.load();
		EX_CHECK(last && ex_bad == 0);
	}

	EX_CHECK(munmap(x, sz) == 0);
	EX_CHECK(munmap(j, sz) == 0);
	return true;
}

/*
 * Pages given back while another task of the process waits inside a
 * call of the network stack, as a browser's thread of the network does
 * all the time: a thread waits in poll on a socket that nothing reaches,
 * and the main thread makes and takes away the pages of a 64MB stretch
 * 128 times over (8GB in all, more than the machine has), the way V8
 * and PartitionAlloc commit and decommit theirs. Every round has to get
 * its pages: what was taken away must come back to the system while
 * the waiting call is still under way.
 */
static std::atomic<int>	churn_in{0};

static void *churn_waiter( void *a )
{
	int		s = *(int *)a;
	struct pollfd	p = { s, POLLIN, 0 };

	churn_in = 1;
	(void)poll(&p, 1, -1);		/* until the process ends */
	return nullptr;
}

static int churn_pages( void )
{
	const size_t		sz = 64UL << 20;
	struct sockaddr_in	a;
	pthread_t		th;
	static int		s;
	char			*m;

	s = socket(AF_INET, SOCK_DGRAM, 0);
	if ( s < 0 ) return 70;
	std::memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if ( bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 ) return 71;
	if ( pthread_create(&th, nullptr, churn_waiter, &s) != 0 ) return 72;
	while ( churn_in == 0 ) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	std::this_thread::sleep_for(std::chrono::milliseconds(300));	/* inside its poll by now */

	m = (char *)mmap(nullptr, sz, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ( m == MAP_FAILED ) return 73;
	for ( int i = 0; i < 128; i++ ) {
		if ( mprotect(m, sz, PROT_READ | PROT_WRITE) != 0 ) {
			std::printf("cxxprog: churn: round %d got no pages (%dMB given back before)\n",
				    i, i * 64);
			std::fflush(stdout);
			return 74;
		}
		m[i * 4096] = (char)i;
		if ( mprotect(m, sz, PROT_NONE) != 0 ) return 75;
	}
	if ( munmap(m, sz) != 0 ) return 76;
	std::printf("cxxprog: churn: 8GB made and given back\n");
	std::fflush(stdout);
	return 0;
}

/*
 * Pages given back while another processor may still translate them
 * (the program's "tlbre", tests/ktest/ktest_cxx.c): TLBRE_USERS threads
 * each write their own pages, wait (their processors go on to other
 * tasks and processes meanwhile), and read them again after a thread
 * of their own has given them back (madvise MADV_DONTNEED) on another
 * processor: they must read zeros, the pages made anew -- an old
 * content is a translation of the page given back left on the
 * processor, and a write through it goes into a page that is free
 * (which PF_FREE_CHECK sees). The count of old contents seen is the
 * exit code (0 when none).
 */
#define TLBRE_USERS	3
#define TLBRE_PAGES	16
#define TLBRE_ROUNDS	3000

static std::mutex		tlbre_mu;
static std::condition_variable	tlbre_cv;
static int			tlbre_round = 0, tlbre_written = 0, tlbre_dropped = -1;
static std::atomic<int>		tlbre_old{0};

static void tlbre_user( volatile uint32_t *pages, int k )
{
	for ( int r = 0; r < TLBRE_ROUNDS; r++ ) {
		for ( int i = 0; i < TLBRE_PAGES; i++ ) {
			pages[i * 1024] = (uint32_t)( ( r << 8 ) | ( k << 4 ) | 1 );
			pages[i * 1024 + 1023] = (uint32_t)r;
		}
		{
			std::unique_lock<std::mutex> g(tlbre_mu);
			tlbre_written++;
			tlbre_cv.notify_all();
			tlbre_cv.wait(g, [r] { return tlbre_dropped >= r; });
		}
		for ( int i = 0; i < TLBRE_PAGES; i++ ) {
			if ( pages[i * 1024] != 0 || pages[i * 1024 + 1023] != 0 ) tlbre_old++;
		}
	}
}

static int tlbre_pages( void )
{
	const size_t	each = TLBRE_PAGES * 4096;
	char		*m = (char *)mmap(nullptr, each * TLBRE_USERS, PROT_READ | PROT_WRITE,
					  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	std::vector<std::thread> th;

	if ( m == MAP_FAILED ) return 120;
	for ( int k = 0; k < TLBRE_USERS; k++ ) {
		th.emplace_back(tlbre_user, (volatile uint32_t *)( m + each * k ), k);
	}
	for ( int r = 0; r < TLBRE_ROUNDS; r++ ) {
		{
			std::unique_lock<std::mutex> g(tlbre_mu);
			tlbre_cv.wait(g, [] { return tlbre_written >= TLBRE_USERS; });
			tlbre_written = 0;
		}
		if ( madvise(m, each * TLBRE_USERS, MADV_DONTNEED) != 0 ) return 121;
		{
			std::lock_guard<std::mutex> g(tlbre_mu);
			tlbre_dropped = r;
			tlbre_round = r;
		}
		tlbre_cv.notify_all();
	}
	for ( auto &t : th ) t.join();
	std::printf("cxxprog: tlbre: %d rounds, %d old contents seen\n", TLBRE_ROUNDS, tlbre_old.load());
	std::fflush(stdout);
	return tlbre_old.load() > 100 ? 100 : tlbre_old.load();
}

/* ---------------------------------------------------------------- between processes */

/*
 * Shared memory and the ends of processes, for tests/ktest/ktest_shm2.c.
 * A key is handed on with ob_dup_key and its number told in a message
 * of the process's queue: the kernel's channels will carry keys the same
 * way.
 */
extern "C" {
ER	ts_snd_msg( ID pid, const void *msg, TMO tmout );
ER	ts_rcv_msg( void *buf, TMO tmout );
ID	ts_memfd_key( int fd );
int	ts_memfd_from_key( ID key );
}

struct prc_msg {			/* T_TSMSG of include/ts/proc.h */
	UINT	type;
	ID	from;
	SZ	size;
	UB	body[256];
};

#define SHM_SIZE	65536

static ER msg_ids( ID to, ID a, ID b )
{
	prc_msg	m;

	std::memset(&m, 0, sizeof(m));
	m.type = 1;
	m.size = 2 * sizeof(ID);
	std::memcpy(m.body, &a, sizeof(ID));
	std::memcpy(m.body + sizeof(ID), &b, sizeof(ID));
	return ts_snd_msg(to, &m, 5000);
}

static char *map_shm( int fd, int prot )
{
	return (char *)mmap(nullptr, SHM_SIZE, prot, MAP_SHARED, fd, 0);
}

/*
 * "shma <b> <c>": the maker. It makes 64KB, writes "from A", makes a
 * read-only descriptor, hands B both and C the read-only one, and
 * looks for B's "from B". Then two more one after another: the second
 * is mapped where a stretch given back was.
 */
static int shm_a( ID pb, ID pc )
{
	prc_msg	m;
	int	fd, fr, rb = -1;
	char	*p, *a2, *a3;
	ID	kb, kbr, kc;

	fd = memfd_create("shm2", 0);
	if ( fd < 0 ) return 10;
	if ( ftruncate(fd, SHM_SIZE) != 0 ) return 11;
	p = map_shm(fd, PROT_READ | PROT_WRITE);
	if ( p == MAP_FAILED ) return 12;
	std::strcpy(p, "from A");
	fr = dup(fd);
	if ( fr < 0 || fcntl(fr, F_SETFL, O_RDONLY) != 0 || fcntl(fr, F_GETFL) != O_RDONLY ) return 13;
	errno = 0;
	if ( map_shm(fr, PROT_READ | PROT_WRITE) != MAP_FAILED || errno != EACCES ) return 14;
	kb = ob_dup_key(ts_memfd_key(fd), OB_OP_ALL, pb);
	kbr = ob_dup_key(ts_memfd_key(fr), OB_OP_ALL, pb);
	kc = ob_dup_key(ts_memfd_key(fr), OB_OP_ALL, pc);
	if ( kb <= 0 || kbr <= 0 || kc <= 0 ) return 15;
	if ( msg_ids(pb, kb, kbr) < E_OK ) return 16;
	if ( msg_ids(pc, kc, 0) < E_OK ) return 17;
	if ( ts_rcv_msg(&m, 60000) < E_OK || m.from != pb ) return 18;
	std::memcpy(&rb, m.body, sizeof(int));
	if ( rb != 0 ) return 100 + rb;
	if ( std::strcmp(p + 4096, "from B") != 0 ) return 19;
	std::printf("cxxprog: shm: B wrote, A sees it\n");
	/* the descriptors closed, the mapping stays */
	close(fd);
	close(fr);
	if ( std::strcmp(p, "from A") != 0 ) return 20;
	if ( munmap(p, SHM_SIZE) != 0 ) return 21;

	fd = memfd_create("again", 0);
	if ( fd < 0 || ftruncate(fd, SHM_SIZE) != 0 ) return 22;
	a2 = map_shm(fd, PROT_READ | PROT_WRITE);
	if ( a2 == MAP_FAILED ) return 23;
	if ( munmap(a2, SHM_SIZE) != 0 || close(fd) != 0 ) return 24;
	fd = memfd_create("third", 0);
	if ( fd < 0 || ftruncate(fd, SHM_SIZE) != 0 ) return 25;
	a3 = map_shm(fd, PROT_READ | PROT_WRITE);
	if ( a3 == MAP_FAILED ) return 26;
	std::printf("cxxprog: shm: mapped at %p, then at %p\n", (void *)a2, (void *)a3);
	if ( a3 > a2 ) return 27;		/* a stretch given back is handed out again first */
	munmap(a3, SHM_SIZE);
	close(fd);
	return 0;
}

/* "shmb": given a writable and a read-only key: reads A's, writes its own, cannot map the other to write */
static int shm_b( void )
{
	prc_msg		m;
	struct stat	st;
	ID		kb, kbr;
	int		fd = -1, fr = -1, rc = 0;
	char		*p = (char *)MAP_FAILED;

	if ( ts_rcv_msg(&m, 60000) < E_OK ) return 30;
	std::memcpy(&kb, m.body, sizeof(ID));
	std::memcpy(&kbr, m.body + sizeof(ID), sizeof(ID));
	fd = ts_memfd_from_key(kb);
	fr = ts_memfd_from_key(kbr);
	if ( fd < 0 || fr < 0 ) rc = 31;
	else if ( fcntl(fd, F_GETFL) != O_RDWR || fcntl(fr, F_GETFL) != O_RDONLY ) rc = 32;
	else if ( fstat(fd, &st) != 0 || st.st_size != SHM_SIZE ) rc = 33;
	if ( rc == 0 ) {
		errno = 0;
		if ( map_shm(fr, PROT_READ | PROT_WRITE) != MAP_FAILED || errno != EACCES ) rc = 34;
	}
	if ( rc == 0 && ( p = map_shm(fd, PROT_READ | PROT_WRITE) ) == MAP_FAILED ) rc = 35;
	if ( rc == 0 && std::strcmp(p, "from A") != 0 ) rc = 36;
	if ( rc == 0 ) std::strcpy(p + 4096, "from B");
	if ( p != MAP_FAILED ) munmap(p, SHM_SIZE);
	if ( fd >= 0 ) close(fd);
	if ( fr >= 0 ) close(fr);
	(void)msg_ids(m.from, rc, 0);
	return rc;
}

/* "shmc": given a read-only key: reads A's, then writes, which must end it by the fault */
static int shm_c( void )
{
	prc_msg	m;
	ID	kc;
	int	fr;
	char	*q;

	if ( ts_rcv_msg(&m, 60000) < E_OK ) return 40;
	std::memcpy(&kc, m.body, sizeof(ID));
	fr = ts_memfd_from_key(kc);
	if ( fr < 0 || fcntl(fr, F_GETFL) != O_RDONLY ) return 41;
	q = map_shm(fr, PROT_READ);
	if ( q == MAP_FAILED ) return 42;
	if ( std::strcmp(q, "from A") != 0 ) return 43;
	std::printf("cxxprog: shm: writing a page handed on read-only\n");
	std::fflush(stdout);
	*(volatile char *)q = 'x';
	return 44;
}

/*
 * A child started as a process object of the program prog, waiting
 * ("wait"); with the metadata json when it is given. Its number, read
 * from its attributes (tessronos.process.pid), or 0.
 */
static ID start_child( const TS_UUID *prog, const char *json, TS_UUID *pu )
{
	T_OBCRE		c;
	char		j[512];
	SZ		asz = 0;
	ID		k;
	const char	*at;

	std::memset(&c, 0, sizeof(c));
	c.type = OB_T_PROCESS;
	c.prog = *prog;
	c.arg = "wait";
	c.argsz = 4;
	if ( json != nullptr ) {
		c.json = (const UB *)json;
		c.jsonsz = (SZ)std::strlen(json);
	}
	if ( ob_cre_obj(&c, pu) < E_OK ) return 0;
	k = ob_opn_obj(pu, OB_OP_ATRRD);
	if ( k <= 0 ) return 0;
	if ( ob_get_atr(k, (UB *)j, sizeof(j) - 1, &asz) < E_OK ) asz = 0;
	ob_cls_obj(k);
	j[asz] = '\0';
	at = std::strstr(j, "\"pid\":");
	return ( at != nullptr ) ? (ID)std::strtol(at + 6, nullptr, 10) : 0;
}

/* tessronos.process.withParent set on a child's process object */
static int set_withparent( const TS_UUID *pu )
{
	static const char	want[] = "{\"tessronos\":{\"process\":{\"withParent\":true}}}";
	ID			k = ob_opn_obj(pu, OB_OP_ATRRD | OB_OP_ATRWR);
	ER			er;

	if ( k <= 0 ) return 1;
	er = ob_set_atr(k, (const UB *)want, (SZ)( sizeof(want) - 1 ));
	ob_cls_obj(k);
	return ( er < E_OK ) ? 2 : 0;
}

/*
 * "pdp": a parent of four, each started as a process object of the
 * program the kernel sends (its UUID, the first message). One is
 * started to end with it (the metadata), one is made to afterwards
 * through its process object, one is ended by kill and collected by
 * waitpid, and one is left alone. It waits for the kernel's word to end.
 */
static int pd_parent( void )
{
	static const char	withpar[] = "{\"tessronos\":{\"process\":{\"withParent\":true}}}";
	prc_msg	m;
	TS_UUID	prog, u, u2, u3, u4;
	ID	q, q2, q3, q4;
	int	st = -1;

	if ( ts_rcv_msg(&m, 60000) < E_OK || m.size < (SZ)sizeof(TS_UUID) ) return 49;
	std::memcpy(&prog, m.body, sizeof(TS_UUID));
	q = start_child(&prog, withpar, &u);
	q2 = start_child(&prog, nullptr, &u2);
	q3 = start_child(&prog, nullptr, &u3);
	q4 = start_child(&prog, nullptr, &u4);
	if ( q <= 0 || q2 <= 0 || q3 <= 0 || q4 <= 0 ) return 50;
	if ( waitpid(q3, &st, WNOHANG) != 0 ) return 51;
	if ( kill(q3, 0) != 0 ) return 52;
	if ( kill(q3, SIGTERM) != 0 ) return 53;
	if ( waitpid(q3, &st, 0) != q3 || !WIFSIGNALED(st) || WTERMSIG(st) != SIGKILL ) return 54;
	errno = 0;
	if ( kill(q3, 0) == 0 || errno != ESRCH ) return 55;
	if ( set_withparent(&u2) != 0 ) return 56;
	std::printf("cxxprog: pdp: children %d %d %d, %d killed and collected\n",
		    (int)q, (int)q2, (int)q4, (int)q3);
	std::fflush(stdout);
	/* the kernel's word, not the news of the child that was killed (TS_MSG_CHILD_EXIT) */
	do {
		if ( ts_rcv_msg(&m, 60000) < E_OK ) return 57;
	} while ( m.from != 0 || m.type != 1 );
	return 0;
}

/* "wait": waits for a message, or to be ended */
static int wait_end( void )
{
	prc_msg	m;

	(void)ts_rcv_msg(&m, 120000);
	return 7;
}

/* ---------------------------------------------------------------- restricted processes */

/*
 * The modes of tests/ktest/ktest_sbx.c (include/ts/sbx.h). Each answers
 * by its exit code the first check that went wrong, 0 when all held.
 */
extern "C" {
ER	ts_set_sbx( UINT groups, UINT how );
INT	ts_get_sbx( void );
int	ts_sandbox_enter( unsigned groups, unsigned how );
INT	gp_opn_gpu( void );
INT	sr_lst_prt( void *list, INT max );
INT	us_lst_dev( void *list, INT max );
void	wm_screen( INT *p_w, INT *p_h );
INT	tm_log_read( UD *p_pos, UB *buf, INT max );
ER	ob_fnd_sys( const char *role, const char *dflt, TS_UUID *p_uuid );
ER	dt_settime( const int64_t *p_t );
ssize_t	getrandom( void *buf, size_t len, unsigned int flags );
}

#define SBX_OBJ		0x0001		/* TS_SBX_* of include/ts/sbx.h */
#define SBX_NET		0x0002
#define SBX_DEV		0x0004
#define SBX_GPU		0x0008
#define SBX_PROC	0x0010
#define SBX_WIN		0x0020
#define SBX_SYS		0x0040
#define SBX_ALL		0x007f
#define SBX_REFUSE	0x0001
#define SBX_NETWORK	( SBX_ALL & ~( SBX_OBJ | SBX_NET ) )

static const TS_UUID	sbx_rnd = OB_UUID_RANDOM_INIT;	/* 乱数, a device object */

static void *sbx_thread( void *a )
{
	pthread_mutex_t	*m = (pthread_mutex_t *)a;

	pthread_mutex_lock(m);
	pthread_mutex_unlock(m);
	return a;
}

/* What a restricted process is still allowed: memory, threads and their waits, time, random bytes */
static int sbx_allowed( void )
{
	static pthread_mutex_t	m = PTHREAD_MUTEX_INITIALIZER;
	pthread_t		t;
	void			*r = nullptr;
	struct timespec		ts;
	unsigned char		rb[16];
	char			*p;
	int			sv[2], fd;
	char			c = 0;

	p = (char *)mmap(nullptr, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if ( p == MAP_FAILED ) return 1;
	p[12345] = 1;
	if ( munmap(p, 1 << 20) != 0 ) return 2;
	{
		std::vector<int>	v(100000, 3);	/* the heap */
		if ( v[99999] != 3 ) return 3;
	}
	pthread_mutex_lock(&m);			/* a thread that has to wait for the lock */
	if ( pthread_create(&t, nullptr, sbx_thread, &m) != 0 ) return 4;
	usleep(20000);
	pthread_mutex_unlock(&m);
	if ( pthread_join(t, &r) != 0 || r != &m ) return 5;
	if ( clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || clock_gettime(CLOCK_REALTIME, &ts) != 0 ) return 6;
	if ( getrandom(rb, sizeof(rb), 0) != (ssize_t)sizeof(rb) ) return 7;
	/* memory and streams without a name, made now */
	fd = memfd_create("sbx", 0);
	if ( fd < 0 || ftruncate(fd, SHM_SIZE) != 0 ) return 8;
	p = map_shm(fd, PROT_READ | PROT_WRITE);
	if ( p == MAP_FAILED ) return 9;
	p[0] = 'm';
	if ( socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ) return 10;
	if ( write(sv[0], "s", 1) != 1 || read(sv[1], &c, 1) != 1 || c != 's' ) return 11;
	{
		struct pollfd	pf = { sv[1], POLLIN, 0 };

		if ( write(sv[0], "t", 1) != 1 || poll(&pf, 1, 1000) != 1 || ( pf.revents & POLLIN ) == 0 ) return 12;
	}
	close(sv[0]);
	close(sv[1]);
	munmap(p, SHM_SIZE);
	close(fd);
	std::printf("cxxprog: sbx: memory, threads, time, random bytes, memory and streams made ok\n");
	std::fflush(stdout);
	return 0;
}

/* "sbx r": the renderer's set, refusing */
static int sbx_renderer( void )
{
	int		held, sv[2], r, s;
	char		*hp, c = 0;
	TS_UUID		u;
	INT		w = 0, h = 0;
	UD		pos = 0;
	UB		lb[16];
	int64_t		now = 0;
	const pid_t	other = 32000;	/* a number no process has: refused before it is looked for */

	/* what it holds before: a memory object and a stream */
	held = memfd_create("held", 0);
	if ( held < 0 || ftruncate(held, SHM_SIZE) != 0 ) return 20;
	hp = map_shm(held, PROT_READ | PROT_WRITE);
	if ( hp == MAP_FAILED ) return 21;
	std::strcpy(hp, "held");
	if ( socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ) return 22;

	if ( ts_sandbox_enter(SBX_ALL, SBX_REFUSE) != 0 ) return 23;
	if ( ts_get_sbx() != ( SBX_ALL | ( SBX_REFUSE << 16 ) ) ) return 24;

	r = sbx_allowed();
	if ( r != 0 ) return r;
	if ( std::strcmp(hp, "held") != 0 ) return 25;
	if ( write(sv[0], "h", 1) != 1 || read(sv[1], &c, 1) != 1 || c != 'h' ) return 26;

	/* objects by UUID and by name, files by path */
	if ( ob_opn_obj(&sbx_rnd, OB_OP_READ) != E_OACV ) return 30;
	if ( ob_fnd_sys("calendar", nullptr, &u) != E_OACV ) return 31;
	if ( ob_fnd_nam((const UB *)"CXXPROG.ELF", &u) != E_OACV ) return 32;
	errno = 0;
	if ( open("/boot/CXXPROG.ELF", O_RDONLY) >= 0 || errno != EPERM ) return 33;
	{
		T_OBCRE	cr;

		std::memset(&cr, 0, sizeof(cr));
		cr.type = OB_T_STORAGE;
		cr.sub = OB_S_FILE;
		if ( ob_cre_obj(&cr, &u) != E_OACV ) return 34;
	}
	/* the network */
	errno = 0;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if ( s >= 0 || errno != EPERM ) return 35;
	/* devices, the GPU, the windows */
	if ( sr_lst_prt(nullptr, 0) != E_OACV ) return 36;
	if ( us_lst_dev(nullptr, 0) != E_OACV ) return 37;
	if ( gp_opn_gpu() != E_OACV ) return 38;
	wm_screen(&w, &h);
	if ( w != 0 || h != 0 ) return 39;
	/* other processes */
	errno = 0;
	if ( kill(other, SIGTERM) == 0 || errno != EPERM ) return 40;
	if ( ob_dup_key(ts_memfd_key(held), OB_OP_ALL, (ID)other) != E_OACV ) return 41;
	if ( kill(getpid(), 0) != 0 ) return 42;		/* itself it may ask of */
	/* the system's own */
	if ( tm_log_read(&pos, lb, sizeof(lb)) != E_OACV ) return 43;
	if ( dt_settime(&now) != E_OACV ) return 44;
	/* the set does not shrink nor stop refusing on asking */
	if ( ts_set_sbx(0, SBX_REFUSE) != E_OK || ts_get_sbx() != ( SBX_ALL | ( SBX_REFUSE << 16 ) ) ) return 45;
	if ( ts_set_sbx(0x100, SBX_REFUSE) != E_PAR ) return 46;
	std::printf("cxxprog: sbx: renderer: objects, files, network, devices, GPU, windows, processes, system refused ok\n");
	std::fflush(stdout);
	return 0;
}

/* "sbx n": the network's set, refusing; the program object's UUID comes in the first message */
static int sbx_network( void )
{
	prc_msg		m;
	TS_UUID		prog, ck = { { 0 } };
	ID		k;
	int		s;

	if ( ts_rcv_msg(&m, 60000) < E_OK || m.size < (SZ)sizeof(TS_UUID) ) return 50;
	std::memcpy(&prog, m.body, sizeof(TS_UUID));
	if ( ts_sandbox_enter(SBX_NETWORK, SBX_REFUSE) != 0 ) return 51;
	if ( ts_get_sbx() != ( SBX_NETWORK | ( SBX_REFUSE << 16 ) ) ) return 52;
	/* a storage object of its own made, opened by its UUID, written and deleted, as a store is */
	{
		T_OBCRE	cr;
		TS_UUID	u;
		SZ	asz = 0;
		INT	rn = 0;

		std::memset(&cr, 0, sizeof(cr));
		cr.type = OB_T_STORAGE;
		cr.sub = OB_S_FILE;
		if ( ob_cre_obj(&cr, &u) < E_OK ) return 53;
		k = ob_opn_obj(&u, OB_OP_R | OB_OP_W | OB_OP_RECORD);
		if ( k <= 0 ) return 54;
		if ( ob_apd_rec(k, OB_RT_SYSDATA, 0, &rn) < E_OK
		  || ob_wri_rec(k, rn, 0, "store", 5, &asz) < E_OK ) return 55;
		ob_cls_obj(k);
		if ( ob_del_obj(&u) < E_OK ) return 56;
	}
	/* a socket */
	s = socket(AF_INET, SOCK_STREAM, 0);
	if ( s < 0 ) return 57;
	close(s);
	/* but no device's object, nor the GPU, the windows, or another process */
	if ( ob_opn_obj(&sbx_rnd, OB_OP_READ) != E_OACV ) return 58;
	(void)ck;
	if ( gp_opn_gpu() != E_OACV ) return 60;
	if ( us_lst_dev(nullptr, 0) != E_OACV ) return 61;
	errno = 0;
	if ( kill(32000, SIGTERM) == 0 || errno != EPERM ) return 62;	/* refused before it is looked for */
	{
		T_OBCRE	cr;
		TS_UUID	u;

		std::memset(&cr, 0, sizeof(cr));
		cr.type = OB_T_PROCESS;
		cr.prog = prog;
		if ( ob_cre_obj(&cr, &u) != E_OACV ) return 63;
	}
	std::printf("cxxprog: sbx: network: store and sockets open, devices, GPU, processes refused ok\n");
	std::fflush(stdout);
	return 0;
}

/* "sbx k": the renderer's set, ended by the first call shut out */
static int sbx_end( void )
{
	if ( ts_sandbox_enter(SBX_ALL, 0) != 0 ) return 60;
	if ( ts_get_sbx() != SBX_ALL ) return 61;
	if ( ts_set_sbx(0, SBX_REFUSE) != E_OACV ) return 62;	/* refusing is not to be had back */
	std::printf("cxxprog: sbx: opening an object by its UUID, which ends it\n");
	std::fflush(stdout);
	(void)ob_opn_obj(&sbx_rnd, OB_OP_READ);
	return 63;
}

/*
 * "sbx p": a parent shut out of the network and devices, refusing,
 * whose child ("sbx q") starts with that set
 */
static int sbx_parent( void )
{
	prc_msg		m;
	TS_UUID		prog, u;
	ID		q;
	int		st = 0;

	if ( ts_rcv_msg(&m, 60000) < E_OK || m.size < (SZ)sizeof(TS_UUID) ) return 70;
	std::memcpy(&prog, m.body, sizeof(TS_UUID));
	if ( ts_sandbox_enter(SBX_NET | SBX_DEV, SBX_REFUSE) != 0 ) return 71;
	{
		T_OBCRE		c;
		char		j[512];
		SZ		asz = 0;
		ID		k;
		const char	*at;
		prc_msg		go;
		int		tries;

		std::memset(&go, 0, sizeof(go));
		go.type = 1;
		/* the parent's own refusal, which its child does not take over */
		errno = 0;
		if ( socket(AF_INET, SOCK_STREAM, 0) >= 0 || errno != EPERM ) return 95;
		std::memset(&c, 0, sizeof(c));
		c.type = OB_T_PROCESS;
		c.prog = prog;
		c.arg = "sbx q";
		c.argsz = 5;
		if ( ob_cre_obj(&c, &u) < E_OK ) return 72;
		k = ob_opn_obj(&u, OB_OP_ATRRD);
		if ( k <= 0 ) return 73;
		if ( ob_get_atr(k, (UB *)j, sizeof(j) - 1, &asz) < E_OK ) asz = 0;
		ob_cls_obj(k);
		j[asz] = '\0';
		at = std::strstr(j, "\"pid\":");
		q = ( at != nullptr ) ? (ID)std::strtol(at + 6, nullptr, 10) : 0;
		if ( q <= 0 ) return 74;
		if ( std::strstr(j, "\"sandbox\":6") == nullptr ) return 75;
		/* a child counts its refusals from 0, whatever its parent's are */
		if ( std::strstr(j, "\"sandbox_refused\":\"total=0 ") == nullptr ) return 90;
		if ( ts_snd_msg(q, &go, 5000) < E_OK ) return 91;
		/* its one refusal is counted and read back as the attribute: total, the group, the call */
		for ( tries = 0; tries < 400; tries++ ) {
			k = ob_opn_obj(&u, OB_OP_ATRRD);
			if ( k <= 0 ) return 92;
			if ( ob_get_atr(k, (UB *)j, sizeof(j) - 1, &asz) < E_OK ) asz = 0;
			ob_cls_obj(k);
			j[asz] = '\0';
			if ( std::strstr(j, "\"sandbox_refused\":\"total=1 obj=0 net=1 dev=0 gpu=0 proc=0 win=0 sys=0 last=") != nullptr
			  && std::strstr(j, " last=0\"") == nullptr ) break;
			usleep(50000);
		}
		if ( tries >= 400 ) return 93;
		if ( ts_snd_msg(q, &go, 5000) < E_OK ) return 94;
	}
	if ( waitpid(q, &st, 0) != q ) return 76;
	if ( WIFEXITED(st) ) return 80 + WEXITSTATUS(st);	/* a check of the child's went wrong */
	if ( !WIFSIGNALED(st) || WTERMSIG(st) != SIGSYS ) return 77;
	std::printf("cxxprog: sbx: the child started restricted and ended by SIGSYS ok\n");
	std::fflush(stdout);
	return 0;
}

/* "sbx q": the child of "sbx p" */
static int sbx_child( void )
{
	int	s;
	prc_msg	m;

	if ( ts_get_sbx() != ( ( SBX_NET | SBX_DEV ) | ( SBX_REFUSE << 16 ) ) ) return 1;
	if ( ts_rcv_msg(&m, 60000) < E_OK ) return 8;		/* the parent has read the count at its start */
	errno = 0;
	s = socket(AF_INET, SOCK_STREAM, 0);
	if ( s >= 0 || errno != EPERM ) return 2;
	if ( ts_set_sbx(SBX_WIN, SBX_REFUSE) != E_OK ) return 3;	/* it adds a group */
	if ( ts_get_sbx() != ( ( SBX_NET | SBX_DEV | SBX_WIN ) | ( SBX_REFUSE << 16 ) ) ) return 4;
	if ( ts_set_sbx(0, 0) != E_OK || ts_get_sbx() != ( SBX_NET | SBX_DEV | SBX_WIN ) ) return 5;
	if ( ts_set_sbx(0, SBX_REFUSE) != E_OACV ) return 6;
	if ( ts_rcv_msg(&m, 60000) < E_OK ) return 9;		/* the parent has read the count after the refusal */
	(void)socket(AF_INET, SOCK_STREAM, 0);			/* ends it */
	return 7;
}

static int sbx_mode( const char *mode )
{
	switch ( mode[4] ) {
	case 'r':	return sbx_renderer();
	case 'n':	return sbx_network();
	case 'k':	return sbx_end();
	case 'p':	return sbx_parent();
	case 'q':	return sbx_child();
	default:	return 99;
	}
}

static int run_prc_mode( const char *mode )
{
	if ( std::strncmp(mode, "sbx ", 4) == 0 ) return sbx_mode(mode);
	if ( std::strncmp(mode, "shma ", 5) == 0 ) {
		char	*e;
		long	b = std::strtol(mode + 5, &e, 10);
		long	c = std::strtol(e, nullptr, 10);

		return shm_a((ID)b, (ID)c);
	}
	if ( std::strcmp(mode, "shmb") == 0 ) return shm_b();
	if ( std::strcmp(mode, "shmc") == 0 ) return shm_c();
	if ( std::strcmp(mode, "pdp") == 0 ) return pd_parent();
	if ( std::strcmp(mode, "wait") == 0 ) return wait_end();
	return -1;
}

/*
 * The modes the kernel tests start the program in (its argument):
 * "wx": write to a page that is executable, "xw": run a page that is
 * writable -- each must end the process by the fault; "hold": pages of
 * every kind are left in place, and the program waits to be ended, for
 * the kernel to look at what its pages allow; "churn": pages made and
 * taken away while a task waits inside a call (churn_pages); "tlbre":
 * pages given back while other threads wait to read them (tlbre_pages);
 * "quick": ends at once; "bp" (x86
 * only): an INT3, which must end the process as a breakpoint. Those
 * between processes (run_prc_mode): "shma", "shmb", "shmc" share memory,
 * "pdp" is a parent whose children end with it, "wait" one of them;
 * "sbx r", "sbx n", "sbx k", "sbx p" (and its child "sbx q") are
 * restricted (sbx_mode).
 */
static int run_mode( const char *mode )
{
	int	r = run_prc_mode(mode);

	if ( r >= 0 ) {
		return r;			/* one of those between processes */
	}
	if ( std::strcmp(mode, "churn") == 0 ) {
		return churn_pages();
	}
	if ( std::strcmp(mode, "tlbre") == 0 ) {
		return tlbre_pages();
	}
	if ( std::strcmp(mode, "quick") == 0 ) {
		return 0;			/* a process that ends at once */
	}
#if defined(__x86_64__)
	if ( std::strcmp(mode, "bp") == 0 ) {
		/* a breakpoint of the program's: #BP, as the program's own exception */
		std::printf("cxxprog: a breakpoint\n");
		std::fflush(stdout);
		__asm__ volatile("int3");
		return 98;
	}
#endif

	char	*x = (char *)mmap(nullptr, 4 * 4096, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if ( x == MAP_FAILED ) return 90;
	code_put(x, 5);
	if ( std::strcmp(mode, "wx") == 0 ) {
		if ( mprotect(x, 4096, PROT_READ | PROT_EXEC) != 0 || ((CODEFN)(void *)x)() != 5 ) return 91;
		std::printf("cxxprog: writing an executable page\n");
		std::fflush(stdout);
		*(volatile uint32_t *)x = 0;
		return 92;
	}
	if ( std::strcmp(mode, "xw") == 0 ) {
		std::printf("cxxprog: running a writable page\n");
		std::fflush(stdout);
		return ((CODEFN)(void *)x)() == 5 ? 93 : 94;
	}
	if ( std::strcmp(mode, "hold") == 0 ) {
		char *j = (char *)mmap(nullptr, 4 * 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if ( j == MAP_FAILED || mprotect(x, 4096, PROT_READ | PROT_EXEC) != 0 ) return 95;
		code_put(j, 6);					/* writable */
		code_put(j + 4096, 7);
		if ( ((CODEFN)(void *)( j + 4096 ))() != 7 ) return 96;	/* executable */
		std::printf("cxxprog: holding\n");
		std::fflush(stdout);
		for ( ;; ) std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return 97;
}

/*
 * Random bytes the ways a C++ program asks for them -- getrandom,
 * getentropy, /dev/urandom through stdio and through a descriptor --
 * all of which read the random source object 乱数. The process is the
 * administrator's, as its parent is, so it may stir bytes in as well.
 */
extern "C" {
ssize_t	getrandom( void *buf, size_t len, unsigned int flags );
int	getentropy( void *buf, size_t len );
}

static bool differ( const unsigned char *a, const unsigned char *b, size_t n )
{
	size_t	same = 0;

	for ( size_t i = 0; i < n; i++ ) {
		if ( a[i] == b[i] ) same++;
	}
	return same < n / 4;
}

static bool random_bytes()
{
	unsigned char	a[256], b[256];
	int		fd;

	if ( getrandom(a, sizeof(a), 0) != (ssize_t)sizeof(a) ) return false;
	if ( getrandom(b, sizeof(b), 0) != (ssize_t)sizeof(b) || !differ(a, b, sizeof(a)) ) return false;
	if ( getentropy(a, 32) != 0 || getentropy(a, 257) != -1 ) return false;

	FILE *f = std::fopen("/dev/urandom", "rb");
	if ( f == nullptr ) return false;
	size_t got = std::fread(a, 1, sizeof(a), f);
	std::fclose(f);
	if ( got != sizeof(a) || !differ(a, b, sizeof(a)) ) return false;

	fd = open("/dev/random", 0);
	if ( fd < 0 ) return false;
	if ( read(fd, b, 100) != 100 || write(fd, a, 16) != 16 ) {
		close(fd);
		return false;
	}
	return close(fd) == 0 && differ(a, b, 100);
}

/*
 * IPv6 through the BSD sockets of lib/libcxxrt: addresses as text both
 * ways, getaddrinfo of a number and of "localhost" (::1 before
 * 127.0.0.1), and a connection over ::1 on an AF_INET6 socket that takes
 * IPv6 only, its ends read back.
 */
static int ip6_line;

#define IP6_CHECK(c)	do { if ( !(c) ) { ip6_line = __LINE__; return false; } } while (0)

static bool ipv6_sockets()
{
	unsigned char		a[16];
	char			t[INET6_ADDRSTRLEN], host[64], serv[8], b[4];
	struct addrinfo		hints, *res = nullptr;
	struct sockaddr_in6	me, peer;
	socklen_t		len;
	int			srv, cli, acc, on = 1, v = 0, got;
	ssize_t			n = 0;

	IP6_CHECK(inet_pton(AF_INET6, "2001:0db8:0:0::1", a) == 1);
	IP6_CHECK(inet_ntop(AF_INET6, a, t, sizeof(t)) != nullptr && std::strcmp(t, "2001:db8::1") == 0);
	IP6_CHECK(inet_pton(AF_INET6, "::ffff:10.0.2.2", a) == 1);
	IP6_CHECK(inet_ntop(AF_INET6, a, t, sizeof(t)) != nullptr && std::strcmp(t, "::ffff:10.0.2.2") == 0);
	IP6_CHECK(inet_pton(AF_INET6, "1:::2", a) == 0 && inet_pton(AF_INET6, "1:2", a) == 0);

	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET6;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_NUMERICHOST;
	IP6_CHECK(getaddrinfo("::1", "17500", &hints, &res) == 0 && res != nullptr);
	IP6_CHECK(res->ai_family == AF_INET6 && res->ai_addrlen == sizeof(me) && res->ai_next == nullptr);
	std::memcpy(&me, res->ai_addr, sizeof(me));
	freeaddrinfo(res);
	IP6_CHECK(ntohs(me.sin6_port) == 17500 && IN6_IS_ADDR_LOOPBACK(&me.sin6_addr));
	IP6_CHECK(getaddrinfo("127.0.0.1", nullptr, &hints, &res) == EAI_NONAME);

	std::memset(&hints, 0, sizeof(hints));
	IP6_CHECK(getaddrinfo("localhost", "80", &hints, &res) == 0 && res != nullptr && res->ai_next != nullptr);
	IP6_CHECK(res->ai_family == AF_INET6 && res->ai_next->ai_family == AF_INET);
	freeaddrinfo(res);

	srv = socket(AF_INET6, SOCK_STREAM, 0);
	IP6_CHECK(srv >= 0);
	IP6_CHECK(setsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) == 0);
	len = sizeof(v);
	IP6_CHECK(getsockopt(srv, IPPROTO_IPV6, IPV6_V6ONLY, &v, &len) == 0 && v == 1);
	IP6_CHECK(bind(srv, (struct sockaddr *)&me, sizeof(me)) == 0 && listen(srv, 2) == 0);
	cli = socket(AF_INET6, SOCK_STREAM, 0);
	IP6_CHECK(cli >= 0 && connect(cli, (struct sockaddr *)&me, sizeof(me)) == 0);
	len = sizeof(peer);
	acc = accept(srv, (struct sockaddr *)&peer, &len);
	IP6_CHECK(acc >= 0 && len == sizeof(peer) && peer.sin6_family == AF_INET6);
	IP6_CHECK(IN6_IS_ADDR_LOOPBACK(&peer.sin6_addr));
	IP6_CHECK(getnameinfo((struct sockaddr *)&peer, len, host, sizeof(host), serv, sizeof(serv),
			      NI_NUMERICHOST | NI_NUMERICSERV) == 0 && std::strcmp(host, "::1") == 0);
	len = sizeof(peer);
	IP6_CHECK(getpeername(cli, (struct sockaddr *)&peer, &len) == 0 && ntohs(peer.sin6_port) == 17500);
	len = sizeof(peer);
	IP6_CHECK(getsockname(cli, (struct sockaddr *)&peer, &len) == 0 && peer.sin6_family == AF_INET6
		  && peer.sin6_port != 0);
	n = send(cli, "v6", 2, 0);
	if ( n != 2 ) std::printf("cxxprog: IPv6 send: %d (errno %d)\n", (int)n, errno);
	IP6_CHECK(n == 2);
	for ( got = 0; got < 2; got += (int)n ) {
		n = recv(acc, b + got, sizeof(b) - (size_t)got, 0);
		if ( n <= 0 ) {
			std::printf("cxxprog: IPv6 recv after %d bytes: %d (errno %d)\n", got, (int)n, errno);
			break;
		}
	}
	IP6_CHECK(got == 2 && b[0] == 'v' && b[1] == '6');

	/*
	 * Closed with a reset (a linger of 0): neither end is left waiting
	 * out the connection's time, so the stack holds nothing of it after
	 */
	struct linger lg = { 1, 0 };
	IP6_CHECK(setsockopt(cli, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) == 0);
	close(cli);
	IP6_CHECK(recv(acc, b, sizeof(b), 0) <= 0);
	close(acc);
	close(srv);
	return true;
}

/* ---------------------------------------------------------------- */

int main()
{
	{
		char	mode[32];
		SZ	n = ts_get_arg(mode, sizeof(mode) - 1);

		if ( n > 0 && n < (SZ)sizeof(mode) ) {
			mode[n] = '\0';
			return run_mode(mode);
		}
	}

	STEP(1, "static constructor", ctor_ran == 1 && at_start.v == 42);

	{
		std::unique_ptr<Shape>	s(new Square(6));
		Rect			r(3, 4);
		STEP(2, "virtual call", shape_area(*s) == 36 && shape_area(r) == 12);
		STEP(3, "rtti", dynamic_cast<Square *>(s.get()) != nullptr
				&& dynamic_cast<Square *>(static_cast<Shape *>(&r)) == nullptr
				&& typeid(*s) == typeid(Square));
	}

	STEP(4, "exceptions", exceptions_work());

	{
		std::vector<int>	v;
		for ( int i = 0; i < 100000; i++ ) v.push_back(( i * 7919 ) % 100003);
		std::sort(v.begin(), v.end());
		std::map<std::string, int>		m;
		std::unordered_map<int, std::string>	u;
		for ( int i = 0; i < 1000; i++ ) {
			m["k" + std::to_string(i)] = i;
			u[i] = std::to_string(i * i);
		}
		std::shared_ptr<int>	sp = std::make_shared<int>(3);
		std::function<int(int)>	f = [sp]( int x ) { return x + *sp; };
		char			buf[64];
		std::snprintf(buf, sizeof(buf), "%.3f %s", 3.14159, u[12].c_str());
		STEP(5, "containers", std::is_sorted(v.begin(), v.end()) && m["k500"] == 500
				      && u[30] == "900" && f(4) == 7 && std::strcmp(buf, "3.142 144") == 0);
	}

	STEP(6, "heap 64MB", heap_big());

	{
		std::vector<std::thread> ts;
		for ( int i = 1; i <= 3; i++ ) ts.emplace_back(tl_worker, i);
		for ( auto &t : ts ) t.join();
		/* each thread started from 5: 105, 205, 305; the main thread's is untouched */
		STEP(7, "thread_local", tl_sum == 615 && tl_count == 5);
	}

	STEP(8, "mutex", mutex_counts());
	STEP(9, "condition variable", producer_consumer());

	{
		std::once_flag		once;
		std::atomic<int>	calls{0};
		std::vector<std::thread> ts;
		for ( int i = 0; i < 3; i++ ) {
			ts.emplace_back([&] { std::call_once(once, [&] { calls++; }); });
		}
		for ( auto &t : ts ) t.join();
		STEP(10, "call_once", calls == 1);
	}

	{
		std::atomic<int>	sum{0};
		std::vector<std::thread> ts;
		for ( int i = 0; i < 3; i++ ) ts.emplace_back([&] { sum += use_slow(); });
		for ( auto &t : ts ) t.join();
		STEP(11, "local static", statics_made == 1 && sum == 297);
	}

	{
		std::shared_mutex	sm;
		int			v = 0;
		std::thread w([&] { std::unique_lock<std::shared_mutex> lk(sm); v = 1; });
		w.join();
		std::shared_lock<std::shared_mutex> rl(sm);
		STEP(12, "shared_mutex", v == 1);
	}

	STEP(13, "pthreads", pthreads_work());
	STEP(14, "timed wait", timed_wait());
	STEP(15, "detached threads", detached_and_sem());
	STEP(21, "mutex ended after its unlock", mutex_ended());
	STEP(22, "waits racing", waits_racing());
	{
		bool ok = umem_calls();
		if ( !ok ) std::printf("cxxprog: memory calls: line %d\n", umem_line);
		STEP(16, "memory calls", ok);
	}

	{
		auto t0 = std::chrono::steady_clock::now();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - t0).count();
		STEP(17, "sleep_for", ms >= 19 && ms < 2000);
	}

	STEP(18, "random", random_bytes());
	{
		bool ok = ipv6_sockets();
		if ( !ok ) std::printf("cxxprog: IPv6 sockets: line %d\n", ip6_line);
		STEP(19, "IPv6 sockets", ok);
	}
	{
		bool ok = exec_pages();
		if ( !ok ) std::printf("cxxprog: executable pages: line %d\n", exec_line);
		else std::printf("cxxprog: executable pages: %d calls while rewritten\n", exec_flips);
		STEP(20, "executable pages", ok);
	}

	std::printf("cxxprog: done %d\n", failed);
	return failed;
}
