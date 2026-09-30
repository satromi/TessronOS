/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	time.h
 *	TessronOS time services (design 7.3)
 */

#ifndef __TS_TIME_H__
#define __TS_TIME_H__

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Monotonic clock: nanoseconds since the kernel started. Never set back,
 * unaffected by tk_set_tim.
 */
IMPORT ER ts_get_mono( UD *p_ns );

#ifdef __cplusplus
}
#endif

#endif /* __TS_TIME_H__ */
