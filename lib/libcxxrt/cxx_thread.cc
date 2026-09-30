/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	cxx_thread.cc
 *	The parts of the C++ library's threads that are not in its headers
 *	(std::thread, std::condition_variable, std::call_once, the atomic
 *	operations on std::shared_ptr), which the
 *	toolchain's libstdc++, configured for one thread, does not have
 *	(include/tscxx, design 17.19).
 */

#define _GLIBCXX_THREAD_IMPL 1
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <bits/shared_ptr_atomic.h>
#include <system_error>
#include <cerrno>

namespace std _GLIBCXX_VISIBILITY(default)
{
_GLIBCXX_BEGIN_NAMESPACE_VERSION

namespace
{
	extern "C" void *execute_native_thread_routine( void *p )
	{
		thread::_State_ptr t{ static_cast<thread::_State *>(p) };

		t->_M_run();
		return nullptr;
	}
}

thread::_State::~_State() = default;

void thread::_M_start_thread( _State_ptr state, void (*)() )
{
	const int err = __gthread_create(&_M_id._M_thread, &execute_native_thread_routine,
					 state.get());

	if ( err ) {
		__throw_system_error(err);
	}
	state.release();
}

void thread::join()
{
	int	e = EINVAL;

	if ( _M_id != id() ) {
		e = __gthread_join(_M_id._M_thread, nullptr);
	}
	if ( e ) {
		__throw_system_error(e);
	}
	_M_id = id();
}

void thread::detach()
{
	int	e = EINVAL;

	if ( _M_id != id() ) {
		e = __gthread_detach(_M_id._M_thread);
	}
	if ( e ) {
		__throw_system_error(e);
	}
	_M_id = id();
}

/*
 * A process has eight tasks at most (design 9.15); a program that makes
 * a worker per processor is told two.
 */
unsigned int thread::hardware_concurrency() noexcept
{
	return 2;
}

condition_variable::condition_variable() noexcept = default;
condition_variable::~condition_variable() noexcept = default;

void condition_variable::wait( unique_lock<mutex> &lock )
{
	_M_cond.wait(*lock.mutex());
}

void condition_variable::notify_one() noexcept
{
	_M_cond.notify_one();
}

void condition_variable::notify_all() noexcept
{
	_M_cond.notify_all();
}

/*
 * The atomic operations on std::shared_ptr take one of a few mutexes,
 * chosen by the shared_ptr's address.
 */
namespace
{
	const unsigned char	sp_mask = 0xf;
	__gthread_mutex_t	sp_mutex[sp_mask + 1] = {
		__GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT,
		__GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT,
		__GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT,
		__GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT, __GTHREAD_MUTEX_INIT,
	};

	unsigned char sp_key( const void *addr )
	{
		return (unsigned char)( ( reinterpret_cast<__UINTPTR_TYPE__>(addr) >> 4 ) & sp_mask );
	}
}

_Sp_locker::_Sp_locker( const void *p ) noexcept
{
	_M_key1 = _M_key2 = sp_key(p);
	__gthread_mutex_lock(&sp_mutex[_M_key1]);
}

_Sp_locker::_Sp_locker( const void *p1, const void *p2 ) noexcept
{
	_M_key1 = sp_key(p1);
	_M_key2 = sp_key(p2);
	if ( _M_key2 < _M_key1 ) {
		__gthread_mutex_lock(&sp_mutex[_M_key2]);
	}
	__gthread_mutex_lock(&sp_mutex[_M_key1]);
	if ( _M_key2 > _M_key1 ) {
		__gthread_mutex_lock(&sp_mutex[_M_key2]);
	}
}

_Sp_locker::~_Sp_locker()
{
	if ( _M_key2 != _M_key1 ) {
		__gthread_mutex_unlock(&sp_mutex[_M_key2]);
	}
	__gthread_mutex_unlock(&sp_mutex[_M_key1]);
}

__thread void	*__once_callable;
__thread void	(*__once_call)();

extern "C" void __once_proxy()
{
	void	(*call)() = __once_call;

	call();
}

_GLIBCXX_END_NAMESPACE_VERSION
}
