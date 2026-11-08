/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	igt12.h
 *	The 12th generation of Intel's graphics: its state as the firmware
 *	left it, only read (igt12.c), and with IGT12=1 the formats of its
 *	execlists (igt12_fmt.c) (docs/tessronos/13-gpu.md 13.11.16).
 */

#ifndef _DEVICE_GPU_INTEL_IGT12_H_
#define _DEVICE_GPU_INTEL_IGT12_H_

#include "sysdepend/x64/igt_gen.h"

#define IGT12_NPIPE	4

typedef struct {
	BOOL	dead;			/* the device answered all ones: nothing more was read */
	UW	ggc, dsm_lo, dsm_hi;
	UW	slice_en, dss_en, eu_dis, media_dis;
	UW	ack_render, ack_gt, ack_media;
	BOOL	rcs_read, gt_read;	/* its forcewake said awake, so these were read */
	UW	rcs_mode, rcs_st_lo, rcs_st_hi;
	UW	bcs_mode, bcs_st_lo, bcs_st_hi;
	UW	guc_status, guc_wopcm;
	UW	pwr_bios, pwr_driver;
	struct {
		UW	conf, ddi, plane_ctl, plane_surf;
	} pipe[IGT12_NPIPE];
} T_IGT12STATE;

/* Reads the registers through 'mmio' (the first BAR) and writes none of them */
IMPORT void	igt12_read( UBINT mmio, T_IGT12STATE *s );

/* From the driver's start, on a machine that is not of the 9th generation */
IMPORT void	knl_igt12_look( void );

#ifdef IGT12
/* A context's descriptor for the submission queue; 0 when a field does not fit */
IMPORT UD	igt12_desc( UW lrca, UINT eng_class, UINT instance, UINT sw_ctx, UINT sw_count, INT prio,
			    BOOL force_restore );

#define IGT12_IDLE_CTX		0x7ff	/* the context number that stands for no context */
#define IGT12_CSB_DW		0x28	/* the status entries' place in the status page, in words */
#define IGT12_CSB_PTR_DW	0x47	/* and the last one written */
#define IGT12_CSB_ENTRIES	12

/* What one entry of the context status buffer says */
#define IGT12_CS_IDLE_TO_ACTIVE		1
#define IGT12_CS_ACTIVE_TO_IDLE		2
#define IGT12_CS_PREEMPT_TO_IDLE	3
#define IGT12_CS_NEW_QUEUE		4	/* to a new queue from a context */
#define IGT12_CS_ELEMENT		5	/* to the next element of the queue */
#define IGT12_CS_IDLE_REPORT		6	/* preempt to idle while idle */

typedef struct {
	UH	to, away;		/* context numbers */
	UB	eng_class, instance;
	UB	detail;			/* why the context left; 0 it is complete */
	BOOL	new_queue;
	UB	kind;
} T_IGT12CS;

IMPORT void	igt12_cs_decode( UD qw, T_IGT12CS *cs );

/* One context handed to an engine: element 0, the others invalid, then the load */
IMPORT void	igt12_sq_load( UBINT mmio, UW eng_base, UD desc );
#endif /* IGT12 */

#endif /* _DEVICE_GPU_INTEL_IGT12_H_ */
