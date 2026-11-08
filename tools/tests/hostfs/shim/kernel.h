/*
 * The little of the kernel the fimps of exFAT and NTFS use, on the host:
 * the "device" is an image file, so a fimp is run against images the
 * standard tools make and check (tools/tests/hostfs).
 */
#ifndef HOSTFS_SHIM_KERNEL_H
#define HOSTFS_SHIM_KERNEL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef signed char B;
typedef short H;
typedef int W;
typedef long long D;
typedef unsigned char UB;
typedef unsigned short UH;
typedef unsigned int UW;
typedef unsigned long long UD;
typedef char VB;
typedef int INT;
typedef unsigned int UINT;
typedef long BINT;
typedef unsigned long UBINT;
typedef long SZ;
typedef int ER;
typedef int ID;
typedef int TMO;
typedef int BOOL;
typedef unsigned int ATR;
typedef int PRI;
typedef void (*FP)();
#define TRUE 1
#define FALSE 0
#define CONST const
#define LOCAL static
#define EXPORT
#define IMPORT extern
#define E_OK 0
#define E_IO (-57)
#define TMO_FEVR (-1)
#define TA_TFIFO 0
#define TA_HLNG 1
#define TA_RNG0 0
#define CNF_MAX_TSKPRI 32

typedef struct { W hi; UW lo; } SYSTIM;
typedef struct { void *exinf; ATR mtxatr; PRI ceilpri; } T_CMTX;
typedef struct { void *exinf; ATR tskatr; FP task; PRI itskpri; SZ stksz; } T_CTSK;

ER tk_srea_dev_d(ID dd, D start, void *buf, SZ size, SZ *asize);
ER tk_swri_dev_d(ID dd, D start, const void *buf, SZ size, SZ *asize);
#define Kmalloc(n) malloc(n)
#define Kfree(p) free(p)
#define knl_memset memset
#define knl_memcpy memcpy

#define tm_printf(...) ((void)fprintf(stderr, (const char *)__VA_ARGS__))
static inline ER tk_get_otm(SYSTIM *t) { t->hi = 0; t->lo = 1000000; return 0; }
static inline ER tk_get_tim(SYSTIM *t) { t->hi = 300; t->lo = 0; return 0; }
static inline ER tk_cre_mtx(const T_CMTX *c) { (void)c; return 1; }
static inline ER tk_del_mtx(ID i) { (void)i; return 0; }
static inline ER tk_loc_mtx(ID i, TMO t) { (void)i; (void)t; return 0; }
static inline ER tk_unl_mtx(ID i) { (void)i; return 0; }
static inline ER tk_dly_tsk(TMO t) { (void)t; return 0; }
static inline ID tk_cre_tsk(const T_CTSK *c) { (void)c; return 1; }
static inline ER tk_sta_tsk(ID i, INT s) { (void)i; (void)s; return 0; }
static inline INT ConvPhysicalAddress(const void *p, INT len, void **pa) { *pa = (void *)p; return len; }
static inline INT knl_prc_dma(void *p, INT len, BOOL into, void **k) { (void)p; (void)len; (void)into; (void)k; return 0; }
#endif
