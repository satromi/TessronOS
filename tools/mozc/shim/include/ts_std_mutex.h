/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ts_std_mutex.h
 *	std::mutex and std::condition_variable for one thread
 *
 *	The toolchain's libstdc++ is built for a single thread and leaves
 *	out the mutexes and condition variables, but abseil's time zones
 *	and some waiters are written with them. The engine runs in one
 *	task, so a lock is always free and a condition is always already
 *	true by the time it is looked at: these do nothing.
 */

#ifndef TS_STD_MUTEX_H
#define TS_STD_MUTEX_H

#include <mutex>
#include <chrono>

#ifndef _GLIBCXX_HAS_GTHREADS

namespace std {

class mutex {
public:
	constexpr mutex() noexcept = default;
	mutex(const mutex &) = delete;
	mutex &operator=(const mutex &) = delete;
	void lock() {}
	bool try_lock() { return true; }
	void unlock() {}
};

class recursive_mutex {
public:
	constexpr recursive_mutex() noexcept = default;
	recursive_mutex(const recursive_mutex &) = delete;
	recursive_mutex &operator=(const recursive_mutex &) = delete;
	void lock() {}
	bool try_lock() { return true; }
	void unlock() {}
};

enum class cv_status { no_timeout, timeout };

class condition_variable {
public:
	condition_variable() noexcept = default;
	condition_variable(const condition_variable &) = delete;
	condition_variable &operator=(const condition_variable &) = delete;
	void notify_one() noexcept {}
	void notify_all() noexcept {}
	void wait(unique_lock<mutex> &) {}
	template <class Pred>
	void wait(unique_lock<mutex> &, Pred pred) { (void)pred(); }
	template <class Clock, class Duration>
	cv_status wait_until(unique_lock<mutex> &,
			     const chrono::time_point<Clock, Duration> &)
	{
		return cv_status::timeout;
	}
	template <class Rep, class Period>
	cv_status wait_for(unique_lock<mutex> &,
			   const chrono::duration<Rep, Period> &)
	{
		return cv_status::timeout;
	}
};

}  // namespace std

#endif /* !_GLIBCXX_HAS_GTHREADS */

#endif /* TS_STD_MUTEX_H */
