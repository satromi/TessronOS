/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_pcie.c
 *	The PCI Express bus (design 10.4).
 *
 *	The machine the tests run on has a host bridge, a network card and a
 *	USB controller on it. The controller is the one that matters: it is
 *	what the USB stack will sit on, and it is the case that shows the
 *	walk handing out a window to a card that had none.
 */

#include "ktest.h"
#include <ts/pcie.h>

#define CLASS_BRIDGE	0x06
#define CLASS_SERIAL	0x0c
#define SUBCLASS_USB	0x03
#define PROGIF_XHCI	0x30

LOCAL BOOL	have_bus = FALSE;

/* the walk found the bridge the bus hangs off */
LOCAL void test_walk( void )
{
	T_PCIDEV	d;
	INT		n;

	n = ts_pcie_count();
	if ( n <= 0 ) {
		KT_SKIP("the machine has no PCI Express bus");
	}
	have_bus = TRUE;

	KT_ASSERT_ER(ts_pcie_get(0, &d), E_OK);
	KT_ASSERT_EQ(d.bus, 0);
	KT_ASSERT(d.vendor != 0 && d.vendor != 0xffff);

	/* a host bridge is there, at the root of everything else */
	KT_ASSERT_ER(ts_pcie_find(CLASS_BRIDGE, 0x00, 0xff, 0, &d), E_OK);
	KT_ASSERT_EQ(d.class_code, CLASS_BRIDGE);

	/* an index past the end, and a class nothing has */
	KT_ASSERT_ER(ts_pcie_get(n, &d), E_PAR);
	KT_ASSERT_ER(ts_pcie_get(-1, &d), E_PAR);
	KT_ASSERT_ER(ts_pcie_find(0x7f, 0xff, 0xff, 0, &d), E_NOEXS);
	KT_ASSERT_ER(ts_pcie_find(CLASS_BRIDGE, 0x00, 0xff, 99, &d), E_NOEXS);
}

/* configuration space reads what the walk recorded */
LOCAL void test_config( void )
{
	T_PCIDEV	d;
	UW		id;

	if ( !have_bus ) KT_SKIP("no bus");

	KT_ASSERT_ER(ts_pcie_get(0, &d), E_OK);

	id = ts_pcie_cfg_read(d.bus, d.dev, d.fn, PCI_VENDOR_ID, 4);
	KT_ASSERT_EQ((UH)(id & 0xffff), d.vendor);
	KT_ASSERT_EQ((UH)(id >> 16), d.device);

	/* the same register, read in pieces */
	KT_ASSERT_EQ(ts_pcie_cfg_read(d.bus, d.dev, d.fn, PCI_VENDOR_ID, 2),
		     (UW)d.vendor);
	KT_ASSERT_EQ(ts_pcie_cfg_read(d.bus, d.dev, d.fn, PCI_CLASS, 1),
		     (UW)d.class_code);

	/* a slot with nothing in it answers all ones */
	KT_ASSERT_EQ(ts_pcie_cfg_read(0, 31, 7, PCI_VENDOR_ID, 4), 0xFFFFFFFFU);
}

/*
 * The USB controller: nothing gave it a window, so the walk did. This is
 * what the USB stack needs before it can touch a register.
 */
LOCAL void test_xhci( void )
{
	T_PCIDEV	d;
	UBINT		base;
	UW		cap;

	if ( !have_bus ) KT_SKIP("no bus");

	if ( ts_pcie_find(CLASS_SERIAL, SUBCLASS_USB, PROGIF_XHCI, 0, &d) < E_OK ) {
		KT_SKIP("the machine has no USB controller");
	}
	tm_printf((UB*)"  xhci at %d:%d.%d %04x:%04x\n",
		  d.bus, d.dev, d.fn, d.vendor, d.device);

	/* it was given somewhere to live, and the window is a real size */
	KT_ASSERT(d.bar[0] != 0);
	KT_ASSERT(d.bar_size[0] >= 0x1000);
	KT_ASSERT_EQ(d.bar_is_io[0], 0);
	/* and it starts on a boundary of its own size */
	KT_ASSERT_EQ(d.bar[0] & (d.bar_size[0] - 1), 0);

	base = ts_pcie_bar_base(&d, 0);
	KT_ASSERT(base != 0);
	KT_ASSERT_ER(ts_pcie_enable(&d), E_OK);

	/*
	 * The first register of a controller says how long its own header
	 * is and which version it speaks. Both are read through the window
	 * the walk handed out, so a wrong address shows up at once.
	 */
	cap = *(volatile UW *)base;
	tm_printf((UB*)"  caplength %d version %d.%d\n",
		  (INT)(cap & 0xff), (INT)((cap >> 24) & 0xff),
		  (INT)((cap >> 20) & 0xf));
	KT_ASSERT((cap & 0xff) >= 0x20);	/* the header is at least that */
	KT_ASSERT_EQ((cap >> 24) & 0xff, 1);	/* version 1.x */

	/* it says it is a PCI Express function */
	cap = ts_pcie_cap_find(&d, PCI_CAP_EXPRESS);
	KT_ASSERT(cap >= 0x40);

	/* and it raises either a line or messages */
	KT_ASSERT(d.int_pin != 0 || ts_pcie_cap_find(&d, PCI_CAP_MSIX) != 0
		  || ts_pcie_cap_find(&d, PCI_CAP_MSI) != 0);
}

/* two cards never get the same window */
LOCAL void test_no_overlap( void )
{
	T_PCIDEV	a, b;
	INT		i, k, x, y;

	if ( !have_bus ) KT_SKIP("no bus");

	for ( i = 0; i < ts_pcie_count(); i++ ) {
		KT_ASSERT_ER(ts_pcie_get(i, &a), E_OK);
		for ( x = 0; x < PCI_MAX_BAR; x++ ) {
			if ( a.bar[x] == 0 ) continue;

			for ( k = i + 1; k < ts_pcie_count(); k++ ) {
				KT_ASSERT_ER(ts_pcie_get(k, &b), E_OK);
				for ( y = 0; y < PCI_MAX_BAR; y++ ) {
					if ( b.bar[y] == 0 ) continue;
					KT_ASSERT(a.bar[x] + a.bar_size[x] <= b.bar[y]
					       || b.bar[y] + b.bar_size[y] <= a.bar[x]);
				}
			}
		}
	}
}

EXPORT void ktest_pcie( void )
{
	KT_RUN(test_walk);
	KT_RUN(test_config);
	KT_RUN(test_xhci);
	KT_RUN(test_no_overlap);
}
