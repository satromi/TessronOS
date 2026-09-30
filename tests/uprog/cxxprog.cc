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
 *	getrandom, getentropy and /dev/urandom (the object 乱数). Each step says
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

extern "C" {
ER	dt_gettime( int64_t *p_t );
ER	ts_ref_prc( int pid, void *pk );
int	ts_get_pid( void );
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
	UM_CHECK(!( ts_map_mem(&q, 4096, 0, TS_MEM_EXEC | TS_MEM_READ) != E_NOSPT ));
	UM_CHECK(!( ts_map_mem(&q, 4096, 3 * 4096, TS_MEM_RW) != E_PAR ));
	UM_CHECK(!( ts_ctl_mem((void *)0x500000, 4096, TS_MEM_RW) != E_PAR ));
	UM_CHECK(!( ts_unm_mem((char *)p + 100, 4096) != E_PAR ));
	UM_CHECK(mmap(nullptr, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
		 == MAP_FAILED && errno == EACCES);

	/* mmap and its relatives */
	char *m = (char *)mmap(nullptr, 3 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	UM_CHECK(!( m == MAP_FAILED ));
	m[0] = 5;
	m[3 * 4096 - 1] = 6;
	UM_CHECK(!( madvise(m, 4096, MADV_DONTNEED) != 0 || m[0] != 0 ));
	UM_CHECK(!( munmap(m, 3 * 4096) != 0 ));

	UM_CHECK(!( ts_unm_mem(p, 8 << 20) != E_OK ));
	(void)t;
	return true;
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

/* ---------------------------------------------------------------- */

int main()
{
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

	std::printf("cxxprog: done %d\n", failed);
	return failed;
}
