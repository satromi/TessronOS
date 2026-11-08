/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	obsd_arm.h (Raspberry Pi, WIFI_NET80211=obsd)
 *	The hypercalls of the Wi-Fi's SDIO host (wifi_user_rpi5.c) as the
 *	layer's files see them, and what obsd_bus_arm.c and tsw_bwfm.c share.
 */

#ifndef __OBSD_ARM_H__
#define __OBSD_ARM_H__

extern int	rumpcomp_tswifi_host( unsigned long long *pa, unsigned long long *size,
				      unsigned int *clk_khz, unsigned int *intr );
extern int	rumpcomp_tswifi_host_arasan( void );
extern void	*rumpcomp_tswifi_map( unsigned long long pa, unsigned long long size );
extern void	rumpcomp_tswifi_intr_done( unsigned int cookie );
extern void	rumpcomp_tswifi_dmasync( void *va, unsigned long long len, int how );

/* The bus: obsd_bus_arm.c */
extern struct bus_space	obsd_host_space;
extern int		obsd_bus_write_gap;
extern struct device	*tsw_host_dev;
extern struct device	*tsw_chip_dev;

#endif /* __OBSD_ARM_H__ */
