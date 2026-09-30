/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	usbman.c
 *	The USB manager: controllers, the tree of devices, plugging in and
 *	taking out (design 10.14).
 *
 *	The manager finds the controllers the board has -- the xHCI
 *	functions on the PCI Express bus of the emulated machine, the two
 *	controllers inside the RP1 of the Raspberry Pi 5 -- and starts one
 *	task that serves them all. That task:
 *
 *	  takes what the controllers finished (on their interrupt, or every
 *	  few milliseconds for one that has none, or while sound streams);
 *	  looks at the root ports when a controller says one changed, and at
 *	  a hub's ports when the hub's status change endpoint says so;
 *	  brings up what was plugged in -- reset, enumerate, hand to a class
 *	  driver -- and lets go of what was taken out, the devices behind a
 *	  hub before the hub;
 *	  runs the sound device's service while it streams.
 *
 *	The manager's lock is held by that task while it does any of this,
 *	and by whatever else talks to a device. A scan waits for a device to
 *	settle or a port to reset; those waits let the lock go in short
 *	steps and serve the controllers in between (knl_usb_wait), so a
 *	stream already running does not stall while another device comes up.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/time.h>
#include <ts/snd.h>
#include "sysdepend.h"
#include "usbdev.h"
#ifdef PCIE_ECAM_PA
#include <ts/pcie.h>
#endif
#ifdef RPI5
#include <ts/gpio.h>
#endif

#define USB_TASK_PRI		8	/* above the programs and the tests */
#define USB_TASK_STKSZ		(16 * 1024)
#define STREAM_TMO		4	/* ms between services while streaming */
#define POLL_TMO		8	/* ... when a controller has no interrupt */
#define HUB_POLL_MS		1000	/* a hub without a status endpoint */
#define MEDIA_TMO		5000	/* ... and a removable medium to look at */
#define FIRST_SCAN_TMO		10000	/* start-up waits this long at most */

EXPORT T_USBSTAT	knl_usb_stat;

IMPORT void knl_obusb_changed( void );		/* peripheral_kernel/obj/obusb.c */

LOCAL USBHC	*hc_tbl[USB_MAX_HC];
LOCAL INT	nhc = 0;
LOCAL UW	root_ignored[USB_MAX_HC];	/* bit p: root port p given up on */
LOCAL USBDEV	*dev_tbl[USB_MAX_DEV];
LOCAL UW	dev_gen = 0;

LOCAL ID	usb_lock_id = 0;
LOCAL ID	usb_flg = 0;
LOCAL ID	usb_tskid = 0;
LOCAL ID	first_scan_sem = 0;
LOCAL BOOL	polled_hc = FALSE;	/* a controller without an interrupt */

/* ---------------------------------------------------------------- lock */

EXPORT void knl_usb_lock( void )
{
	tk_wai_sem(usb_lock_id, 1, TMO_FEVR);
}

EXPORT void knl_usb_unlock( void )
{
	tk_sig_sem(usb_lock_id, 1);
}

EXPORT UW knl_usb_ms( void )
{
	UD	ns = 0;

	ts_get_mono(&ns);

	return (UW)(ns / 1000000U);
}

EXPORT BOOL knl_usb_streaming( void )
{
	return knl_snd_active();
}

EXPORT void knl_usb_hub_changed( void )
{
	if ( usb_flg > 0 ) {
		tk_set_flg(usb_flg, USBF_HUB);
	}
}

LOCAL void serve_controllers( void )
{
	INT	i;

	for ( i = 0; i < nhc; i++ ) {
		if ( !hc_tbl[i]->lost ) {
			hc_tbl[i]->ops->poll(hc_tbl[i]);
		}
	}
	knl_snd_poll();
}

EXPORT void knl_usb_wait( INT ms )
{
	UW	start = knl_usb_ms();
	INT	left;

	if ( usb_tskid <= 0 || tk_get_tid() != usb_tskid ) {
		tk_dly_tsk(( ms > 0 ) ? ms : 1);
		return;
	}
	for (;;) {
		left = ms - (INT)(knl_usb_ms() - start);
		if ( left <= 0 ) {
			break;
		}
		knl_usb_unlock();
		tk_dly_tsk(( left < STREAM_TMO ) ? left : STREAM_TMO);
		knl_usb_lock();
		serve_controllers();
	}
}

/* ---------------------------------------------------------------- the tree */

LOCAL INT dev_slot( USBDEV *d )
{
	INT	i;

	for ( i = 0; i < USB_MAX_DEV; i++ ) {
		if ( dev_tbl[i] == d ) {
			return i;
		}
	}

	return -1;
}

LOCAL USBDEV *dev_at( USBHC *hc, USBDEV *parent, INT port )
{
	INT	i;

	for ( i = 0; i < USB_MAX_DEV; i++ ) {
		if ( dev_tbl[i] != NULL && dev_tbl[i]->hc == hc
		  && dev_tbl[i]->parent == parent && dev_tbl[i]->port == port ) {
			return dev_tbl[i];
		}
	}

	return NULL;
}

LOCAL USBDEV *dev_of_handle( INT handle )
{
	INT	i = (handle & 0xff) - 1;

	if ( handle <= 0 || i < 0 || i >= USB_MAX_DEV ) {
		return NULL;
	}
	if ( dev_tbl[i] == NULL || dev_tbl[i]->handle != handle ) {
		return NULL;
	}

	return dev_tbl[i];
}

/* Let go of a device and everything behind it, the deepest first */
LOCAL void detach_tree( USBDEV *dev, CONST char *why )
{
	INT	i;

	for ( i = 0; i < USB_MAX_DEV; i++ ) {
		if ( dev_tbl[i] != NULL && dev_tbl[i]->parent == dev ) {
			detach_tree(dev_tbl[i], why);
		}
	}
	if ( dev->role == USB_ROLE_HUB ) {
		knl_hub_detach(dev);
	}
	if ( dev->hid != NULL ) {
		knl_hid_detach(dev);
	}
	if ( dev->msc != NULL ) {
		knl_msc_detach(dev);
	}
	if ( dev->audio != NULL ) {
		knl_uac_detach(dev);
	}
	USB_LOG("usb: %d-%d route %05x addr %d gone (%s)\n", dev->hc->index,
		dev->root_port, dev->route, dev->addr, why);
	i = dev_slot(dev);
	if ( i >= 0 ) {
		dev_tbl[i] = NULL;
	}
	knl_usb_dev_free(dev);
	knl_usb_stat.detached++;
	knl_usb_stat.gen = ++dev_gen;
}

LOCAL void mark_ignored( USBHC *hc, USBDEV *parent, INT port )
{
	if ( parent != NULL ) {
		parent->hub_ignored |= 1UL << port;
	} else {
		root_ignored[hc->index] |= 1UL << port;
	}
}

/* The class drivers of a device that came up */
LOCAL void give_role( USBDEV *dev )
{
	INT	cls = knl_usb_dev_class(dev);
	ER	er;

	if ( cls == USB_CLASS_HUB ) {
		er = knl_hub_attach(dev);
		if ( er < E_OK ) {
			USB_LOG("usb: hub not usable (%d)\n", (INT)er);
		}
	} else if ( knl_msc_match(dev) ) {
		if ( knl_msc_attach(dev) >= E_OK ) {
			dev->role = USB_ROLE_STORAGE;
		}
	} else if ( knl_hid_match(dev) ) {
		if ( knl_hid_attach(dev) >= E_OK ) {
			dev->role = USB_ROLE_HID;
		}
	}
	/* an audio function, alone or beside the device's other ones */
	if ( dev->role != USB_ROLE_HUB && knl_uac_match(dev) ) {
		if ( knl_uac_attach(dev) >= E_OK && dev->role == USB_ROLE_NONE ) {
			dev->role = USB_ROLE_AUDIO;
		}
	}
}

/*
 * Something was plugged into a root port (parent NULL) or a hub's port:
 * let it settle, reset the port, bring the device up and give it a role.
 */
LOCAL void attach_at( USBHC *hc, USBDEV *parent, INT port )
{
	USBDEV	*dev = NULL;
	INT	speed = USB_SPEED_HIGH, slot;
	ER	er;
	UW	st = 0;

	knl_usb_wait(100);				/* connect debounce */
	if ( parent == NULL ) {
		if ( !hc->ops->port_connected(hc, port) ) {
			return;
		}
	} else {
		if ( knl_hub_port_status(parent, port, &st) < E_OK
		  || (st & HUB_PS_CONNECTION) == 0 ) {
			return;
		}
		if ( parent->depth + 1 > USB_MAX_DEPTH ) {
			mark_ignored(hc, parent, port);
			return;
		}
	}
	slot = dev_slot(NULL);
	if ( slot < 0 ) {
		USB_LOG("usb: too many devices\n");
		mark_ignored(hc, parent, port);
		return;
	}
	er = ( parent == NULL ) ? hc->ops->port_reset(hc, port, &speed)
				: knl_hub_port_reset(parent, port, &speed);
	if ( er >= E_OK ) {
		er = knl_usb_enumerate(hc, parent, port, speed, &dev);
	}
	if ( er < E_OK ) {
		USB_LOG("usb: %d-%d %s port %d: device not brought up (%d)\n",
			hc->index, ( parent != NULL ) ? parent->root_port : port,
			( parent != NULL ) ? "hub" : "root", port, (INT)er);
		knl_usb_stat.enum_failed++;
		mark_ignored(hc, parent, port);
		return;
	}
	dev->handle = (INT)((((++dev_gen) & 0x7fffff) << 8) | (UW)(slot + 1));
	dev_tbl[slot] = dev;
	give_role(dev);
	knl_usb_stat.attached++;
	knl_usb_stat.gen = dev_gen;
}

LOCAL void scan_root( USBHC *hc )
{
	INT	n = hc->ops->nports(hc), p;

	for ( p = 0; p < n; p++ ) {
		BOOL	conn = hc->ops->port_connected(hc, p);
		BOOL	chg  = hc->ops->port_changed(hc, p);
		USBDEV	*dev = dev_at(hc, NULL, p);

		if ( dev != NULL && ( !conn || chg ) ) {
			detach_tree(dev, conn ? "reconnected" : "unplugged");
			dev = NULL;
		}
		if ( !conn || chg ) {
			root_ignored[hc->index] &= ~(1UL << p);
		}
		if ( conn && dev == NULL && (root_ignored[hc->index] & (1UL << p)) == 0 ) {
			attach_at(hc, NULL, p);
		}
	}
}

/* The ports of a hub: those that reported a change, or all of them */
LOCAL void scan_hub( USBDEV *hub, BOOL all )
{
	UW	change = hub->hub_change;
	INT	p;

	hub->hub_change &= ~change;
	for ( p = 1; p <= hub->hub_ports; p++ ) {
		UW	st = 0;
		BOOL	conn, chg;
		USBDEV	*dev;

		if ( !all && (change & (1UL << p)) == 0 ) {
			continue;
		}
		if ( knl_hub_port_status(hub, p, &st) < E_OK ) {
			return;				/* the hub itself went away */
		}
		conn = ( (st & HUB_PS_CONNECTION) != 0 );
		chg  = ( (st & HUB_PC_CONNECTION) != 0 );
		if ( (st & HUB_PC_ALL) != 0 ) {
			knl_hub_clear_changes(hub, p, st);
		}
		dev = dev_at(hub->hc, hub, p);
		if ( dev != NULL && ( !conn || chg ) ) {
			detach_tree(dev, conn ? "reconnected" : "unplugged");
			dev = NULL;
		}
		if ( !conn || chg ) {
			hub->hub_ignored &= ~(1UL << p);
		}
		if ( conn && dev == NULL && (hub->hub_ignored & (1UL << p)) == 0 ) {
			attach_at(hub->hc, hub, p);
		}
	}
}

/*
 * The hubs, a tier at a time: scanning one may bring up or let go of
 * others. hubs_all looks at every port of every hub.
 */
LOCAL void scan_hubs( BOOL hubs_all )
{
	USBDEV	*hubs[USB_MAX_DEV];
	INT	i, n = 0;

	for ( i = 0; i < USB_MAX_DEV; i++ ) {
		if ( dev_tbl[i] != NULL && dev_tbl[i]->role == USB_ROLE_HUB
		  && !dev_tbl[i]->hc->lost ) {
			hubs[n++] = dev_tbl[i];
		}
	}
	for ( i = 0; i < n; i++ ) {
		if ( dev_slot(hubs[i]) < 0 ) {
			continue;			/* it went while another was scanned */
		}
		if ( hubs_all || hubs[i]->hub_pipe == NULL ) {
			scan_hub(hubs[i], TRUE);
		} else if ( hubs[i]->hub_change != 0 ) {
			scan_hub(hubs[i], FALSE);
		}
	}
}

LOCAL BOOL hub_without_pipe( void )
{
	INT	i;

	for ( i = 0; i < USB_MAX_DEV; i++ ) {
		if ( dev_tbl[i] != NULL && dev_tbl[i]->role == USB_ROLE_HUB
		  && dev_tbl[i]->hub_pipe == NULL ) {
			return TRUE;
		}
	}

	return FALSE;
}

/*
 * A controller stopped on an error of its own and does not run again.
 * Its devices cannot answer any more, so they are let go without talking
 * to them, and its ports are not looked at again.
 */
LOCAL void hc_lost( USBHC *hc )
{
	INT	i;

	for ( i = 0; i < USB_MAX_DEV; i++ ) {
		if ( dev_tbl[i] != NULL && dev_tbl[i]->hc == hc
		  && dev_tbl[i]->parent == NULL ) {
			detach_tree(dev_tbl[i], "controller stopped");
		}
	}
	root_ignored[hc->index] = 0;
}

/* ---------------------------------------------------------------- task */

LOCAL void usb_task( INT stacd, void *exinf )
{
	UW	lost_seen = 0, last_hub = 0, obj_gen = 0;
	BOOL	first = TRUE;
	INT	i;

	for (;;) {
		UINT	ptn = 0;
		TMO	tmo;

		if ( first ) {
			ptn = USBF_RESCAN;
		} else {
			if ( knl_usb_streaming() ) {
				tmo = STREAM_TMO;
			} else if ( polled_hc ) {
				tmo = POLL_TMO;
			} else if ( hub_without_pipe() ) {
				tmo = HUB_POLL_MS;
			} else if ( knl_msc_wants_poll() ) {
				tmo = MEDIA_TMO;
			} else {
				tmo = TMO_FEVR;
			}
			tk_wai_flg(usb_flg, USBF_IRQ | USBF_RESCAN | USBF_HUB,
				   TWF_ORW | TWF_CLR, &ptn, tmo);
		}
		knl_usb_lock();
		serve_controllers();
		for ( i = 0; i < nhc; i++ ) {
			if ( hc_tbl[i]->lost && (lost_seen & (1UL << i)) == 0 ) {
				lost_seen |= 1UL << i;
				hc_lost(hc_tbl[i]);
			}
		}
		/*
		 * A service may have raised USBF_RESCAN or USBF_HUB (a port
		 * changed). They are taken here as well, and whatever else came
		 * meanwhile stays raised for the next wait.
		 */
		{
			UINT	p2 = 0;

			if ( tk_wai_flg(usb_flg, USBF_RESCAN | USBF_HUB,
					TWF_ORW | TWF_BITCLR, &p2, TMO_POL) >= E_OK ) {
				ptn |= p2;
			}
		}
		if ( (ptn & USBF_RESCAN) != 0 ) {
			for ( i = 0; i < nhc; i++ ) {
				if ( !hc_tbl[i]->lost ) {
					scan_root(hc_tbl[i]);
				}
			}
			knl_usb_stat.scans++;
		}
		if ( (ptn & (USBF_RESCAN | USBF_HUB)) != 0
		  || (INT)(knl_usb_ms() - last_hub) >= HUB_POLL_MS ) {
			scan_hubs(first);
			last_hub = knl_usb_ms();
		}
		knl_msc_poll();
		knl_usb_unlock();
		knl_msc_notify();
		knl_uac_notify();
		if ( knl_usb_stat.gen != obj_gen ) {
			obj_gen = knl_usb_stat.gen;
			knl_obusb_changed();	/* the devices' objects follow, outside the lock */
		}
		if ( first ) {
			first = FALSE;
			tk_sig_sem(first_scan_sem, 1);
		}
	}
}

/* ---------------------------------------------------------------- controllers */

LOCAL void add_hc( USBHC *hc )
{
	hc->index = nhc;
	hc_tbl[nhc++] = hc;
	if ( hc->intno == 0 ) {
		polled_hc = TRUE;
	}
}

#ifdef PCIE_ECAM_PA
#define PCI_CLASS_SERIAL	0x0c
#define PCI_SUBCLASS_USB	0x03
#define PCI_PROGIF_XHCI		0x30

/* Every xHCI function on the PCI Express bus, with a message interrupt each */
LOCAL void find_pci( void )
{
	T_PCIDEV	d;
	UBINT		base;
	UINT		intno;
	USBHC		*hc;
	INT		i;

	for ( i = 0; nhc < USB_MAX_HC; i++ ) {
		if ( ts_pcie_find(PCI_CLASS_SERIAL, PCI_SUBCLASS_USB,
				  PCI_PROGIF_XHCI, i, &d) < E_OK ) {
			break;
		}
		base = ts_pcie_bar_base(&d, 0);
		if ( base == 0 ) {
			continue;			/* it was given no window */
		}
		ts_pcie_enable(&d);
		if ( ts_pcie_msi_alloc(&d, &intno) < E_OK ) {
			intno = 0;
		}
		if ( knl_xhci_attach(base, d.bar_size[0], d.bar[0], intno,
				     usb_flg, &hc) >= E_OK ) {
			add_hc(hc);
		}
	}
}
#endif

#ifdef RPI5
/*
 * The two controllers inside the RP1 (design 4.7): DesignWare USB 3 cores
 * whose xHCI registers start at the foot of each block, with the core's
 * own registers further up. The firmware leaves them in host mode when
 * it booted from USB; otherwise the port capability is set to host here.
 * Interrupts from the RP1 need its MSI-X table set up (design 10.7), so
 * both are polled.
 *
 * NOT VERIFIED ON HARDWARE.
 */
#define DWC3_GCTL		0xc110
#define DWC3_GSNPSID		0xc120
#define DWC3_GUSB2PHYCFG	0xc200
#define DWC3_GUSB3PIPECTL	0xc2c0
#define GCTL_PRTCAP_MASK	0x00003000
#define GCTL_PRTCAP_HOST	0x00001000
#define USB2PHY_SUSPHY		0x00000040	/* the USB 2 PHY may be put to sleep */
#define USB2PHY_ENBLSLPM	0x00000100	/* and may sleep between packets */
#define USB3PIPE_SUSPHY		0x00020000	/* the USB 3 PHY may be put to sleep */
#define RP1_USB_SIZE		0x100000

LOCAL void find_rp1( void )
{
	UD		pa[2] = { RP1_USB0_PA, RP1_USB1_PA };
	USBHC		*hc;
	INT		i;

	/*
	 * The GPIO driver has already looked whether the RP1's window
	 * answers; without it every read below would reach nothing.
	 */
	if ( rp1_gpio_read(0) < 0 ) {
		return;
	}
	{
		/*
		 * What the firmware left around the two host controllers:
		 * their configuration blocks, the reset and the clock
		 * controls. For a port that stays silent, this is what there
		 * is to go on.
		 */
		UBINT	w = DEV_BASE(RP1_WINDOW_PA);

		tm_printf((UB *)"TessronOS: rp1 chip %08x usbh0cfg %08x %08x usbh1cfg %08x %08x\n",
			  in_w(w + 0x000000), in_w(w + 0x160000), in_w(w + 0x160004),
			  in_w(w + 0x164000), in_w(w + 0x164004));
		tm_printf((UB *)"TessronOS: rp1 resets %08x %08x %08x %08x  usbh clocks %08x %08x %08x %08x\n",
			  in_w(w + 0x014000), in_w(w + 0x014004), in_w(w + 0x014008), in_w(w + 0x01400c),
			  in_w(w + 0x0180f4), in_w(w + 0x018104), in_w(w + 0x018114), in_w(w + 0x018124));
	}
	for ( i = 0; i < 2 && nhc < USB_MAX_HC; i++ ) {
		UBINT	base = DEV_BASE(pa[i]);
		UW	gctl;

		if ( *(volatile UW *)base == 0xffffffff ) {
			/* the read failed: the block does not answer on the RP1's bus */
			tm_printf((UB *)"TessronOS: rp1 usb%d does not answer\n", i);
			continue;
		}
		gctl = *(volatile UW *)(base + DWC3_GCTL);
		/* what the firmware left, for whoever reads the console when a port stays dead */
		tm_printf((UB *)"TessronOS: rp1 usb%d id %08x gctl %08x usb2phy %08x usb3pipe %08x\n", i,
			  *(volatile UW *)(base + DWC3_GSNPSID), gctl,
			  *(volatile UW *)(base + DWC3_GUSB2PHYCFG),
			  *(volatile UW *)(base + DWC3_GUSB3PIPECTL));
		if ( (gctl & GCTL_PRTCAP_MASK) != GCTL_PRTCAP_HOST ) {
			*(volatile UW *)(base + DWC3_GCTL)
				= (gctl & ~GCTL_PRTCAP_MASK) | GCTL_PRTCAP_HOST;
			Asm("dsb sy" ::: "memory");
			tk_dly_tsk(10);
		}
		/*
		 * The PHYs kept awake: a PHY let sleep while nothing is
		 * plugged in does not see a device arrive until the
		 * controller wakes it, which a host that only polls may
		 * never do.
		 */
		*(volatile UW *)(base + DWC3_GUSB2PHYCFG)
			&= ~(UW)( USB2PHY_SUSPHY | USB2PHY_ENBLSLPM );
		*(volatile UW *)(base + DWC3_GUSB3PIPECTL) &= ~(UW)USB3PIPE_SUSPHY;
		Asm("dsb sy" ::: "memory");
		tk_dly_tsk(1);
		if ( knl_xhci_attach(base, RP1_USB_SIZE, pa[i], 0, usb_flg,
				     &hc) >= E_OK ) {
			add_hc(hc);
		}
	}
}
#endif

EXPORT INT knl_usb_init( void )
{
	T_CSEM	csem;
	T_CFLG	cflg;
	T_CTSK	ctsk;
	INT	i;

	if ( usb_tskid > 0 ) {
		return nhc;
	}
	csem.exinf = NULL;
	csem.sematr = TA_TFIFO;
	csem.isemcnt = 1;
	csem.maxsem = 1;
	usb_lock_id = tk_cre_sem(&csem);
	csem.isemcnt = 0;
	first_scan_sem = tk_cre_sem(&csem);
	cflg.exinf = NULL;
	cflg.flgatr = TA_TFIFO | TA_WMUL;
	cflg.iflgptn = 0;
	usb_flg = tk_cre_flg(&cflg);
	if ( usb_lock_id <= 0 || first_scan_sem <= 0 || usb_flg <= 0 ) {
		return E_LIMIT;
	}

#ifdef PCIE_ECAM_PA
	find_pci();
#endif
#ifdef RPI5
	find_rp1();
#endif
	if ( nhc == 0 ) {
		return 0;			/* the machine has no controller */
	}
	for ( i = 0; i < nhc; i++ ) {
		T_USBHC	info;

		hc_tbl[i]->ops->info(hc_tbl[i], &info);
		tm_printf((UB *)"TessronOS: xhci%d %d.%d, %d ports, %d slots, ctx %d, "
			  "%d scratch, interrupt %d\n", i,
			  (INT)(info.version >> 8), (INT)((info.version >> 4) & 0xf),
			  (INT)info.nports, (INT)info.nslots, (INT)info.ctx_size,
			  (INT)info.nscratch, (INT)info.intno);
	}

	ctsk.exinf = NULL;
	ctsk.tskatr = TA_HLNG | TA_RNG0;
	ctsk.task = (FP)usb_task;
	ctsk.itskpri = USB_TASK_PRI;
	ctsk.stksz = USB_TASK_STKSZ;
	usb_tskid = tk_cre_tsk(&ctsk);
	if ( usb_tskid <= 0 || tk_sta_tsk(usb_tskid, 0) < E_OK ) {
		return E_LIMIT;
	}
	/* what is plugged in now is up before the system goes on */
	tk_wai_sem(first_scan_sem, 1, FIRST_SCAN_TMO);

	return nhc;
}

/* ---------------------------------------------------------------- interface */

EXPORT INT ts_usb_hc_count( void )
{
	return nhc;
}

EXPORT ER ts_usb_ref_hc( INT hc, T_USBHC *pk_hc )
{
	if ( pk_hc == NULL ) {
		return E_PAR;
	}
	if ( hc < 0 || hc >= nhc ) {
		return E_NOEXS;
	}
	knl_memset(pk_hc, 0, sizeof(*pk_hc));
	hc_tbl[hc]->ops->info(hc_tbl[hc], pk_hc);

	return E_OK;
}

EXPORT ER ts_usb_ref_port( INT hc, UINT port, T_USBPORT *p )
{
	if ( p == NULL ) {
		return E_PAR;
	}
	if ( hc < 0 || hc >= nhc ) {
		return E_NOEXS;
	}
	if ( port >= (UINT)hc_tbl[hc]->ops->nports(hc_tbl[hc]) ) {
		return E_PAR;
	}

	return hc_tbl[hc]->ops->port_state(hc_tbl[hc], (INT)port, p);
}

LOCAL void dev_info( USBDEV *d, T_USBDEV *o )
{
	INT	k;

	knl_memset(o, 0, sizeof(*o));
	o->dev       = d->handle;
	o->parent    = ( d->parent != NULL ) ? d->parent->handle : 0;
	o->hc        = (UINT)d->hc->index;
	o->root_port = (UINT)d->root_port;
	o->port      = (UINT)d->port;
	o->depth     = (UINT)d->depth;
	o->route     = d->route;
	o->speed     = (UINT)d->speed;
	o->addr      = (UINT)d->addr;
	o->vendor    = GET16(d->ddesc + 8);
	o->product   = GET16(d->ddesc + 10);
	o->release   = GET16(d->ddesc + 12);
	o->dev_class = (UB)knl_usb_dev_class(d);
	o->subclass  = d->ddesc[5];
	o->protocol  = d->ddesc[6];
	o->role      = (UINT)d->role;
	o->hub_ports = (UINT)d->hub_ports;
	for ( k = 0; k < (INT)sizeof(o->name) - 1 && d->product[k] != 0; k++ ) {
		o->name[k] = d->product[k];
	}
	for ( k = 0; k < (INT)sizeof(o->devnm) - 1 && d->devnm[k] != 0; k++ ) {
		o->devnm[k] = d->devnm[k];
	}
}

EXPORT INT ts_usb_lst_dev( T_USBDEV *buf, INT max )
{
	INT	i, depth, n = 0;

	if ( max > 0 && buf == NULL ) {
		return E_PAR;
	}
	if ( usb_lock_id <= 0 ) {
		return 0;
	}
	knl_usb_lock();
	for ( depth = 0; depth <= USB_MAX_DEPTH; depth++ ) {
		for ( i = 0; i < USB_MAX_DEV; i++ ) {
			USBDEV	*d = dev_tbl[i];

			if ( d == NULL || d->depth != depth ) {
				continue;
			}
			if ( n < max ) {
				dev_info(d, &buf[n]);
			}
			n++;
		}
	}
	knl_usb_unlock();

	return n;
}

EXPORT ER ts_usb_ref_dev( INT dev, T_USBDEV *pk_dev )
{
	USBDEV	*d;

	if ( pk_dev == NULL ) {
		return E_PAR;
	}
	if ( usb_lock_id <= 0 ) {
		return E_NOEXS;
	}
	knl_usb_lock();
	d = dev_of_handle(dev);
	if ( d != NULL ) {
		dev_info(d, pk_dev);
	}
	knl_usb_unlock();

	return ( d != NULL ) ? E_OK : E_NOEXS;
}

EXPORT INT ts_usb_control( INT dev, CONST UB *setup, void *data, INT len )
{
	USBDEV	*d;
	UB	s[8];
	UB	*tmp = NULL;
	INT	actual = 0, i;
	ER	er;

	if ( setup == NULL || len < 0 || len > 4096 || ( len > 0 && data == NULL ) ) {
		return E_PAR;
	}
	if ( usb_lock_id <= 0 ) {
		return E_NOEXS;
	}
	for ( i = 0; i < 8; i++ ) {
		s[i] = setup[i];
	}
	PUT16(s + 6, len);
	if ( len > 0 ) {
		tmp = (UB *)Kmalloc((SZ)len);
		if ( tmp == NULL ) {
			return E_NOMEM;
		}
		if ( (s[0] & USB_RT_IN) == 0 ) {
			knl_memcpy(tmp, data, len);
		}
	}
	knl_usb_lock();
	d = dev_of_handle(dev);
	if ( d == NULL ) {
		er = E_NOEXS;
	} else {
		er = d->hc->ops->control(d->hc, d, s, tmp, &actual, 1000);
	}
	knl_usb_unlock();
	if ( er >= E_OK && (s[0] & USB_RT_IN) != 0 && actual > 0 ) {
		knl_memcpy(data, tmp, actual);
	}
	if ( tmp != NULL ) {
		Kfree(tmp);
	}

	return ( er < E_OK ) ? (INT)er : actual;
}

EXPORT INT ts_usb_get_desc( INT dev, UINT type, UINT index, void *buf, INT len )
{
	UB	setup[8];

	setup[0] = USB_RT_IN;
	setup[1] = USB_REQ_GET_DESCRIPTOR;
	setup[2] = (UB)index;
	setup[3] = (UB)type;
	setup[4] = 0;
	setup[5] = 0;
	PUT16(setup + 6, len);

	return ts_usb_control(dev, setup, buf, len);
}

EXPORT ER ts_usb_stat( T_USBSTAT *st )
{
	if ( st == NULL ) {
		return E_PAR;
	}
	*st = knl_usb_stat;

	return E_OK;
}

EXPORT ER ts_usb_rescan( void )
{
	if ( usb_flg <= 0 ) {
		return E_NOEXS;
	}
	tk_set_flg(usb_flg, USBF_RESCAN);

	return E_OK;
}
