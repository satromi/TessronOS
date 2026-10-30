/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	ktest_pcdev.c (x64 PC)
 *	The devices of a PC that are not on USB, as objects (design 18.7):
 *	the i8042's keyboard and mouse ("ps2kbd", "ps2aux"), the power
 *	devices of ACPI ("battery0", "acpower", "lid", "powerbutton"), the
 *	SATA disks ("sataa") and the Intel Ethernet card as the network card
 *	("neta"). Each is a device object of its kind, in the list of the
 *	devices, with its records; the power devices' state is read and a
 *	change of it told. And VT-d as the start leaves it (test_vtd), and
 *	how the xHCI, the Intel card and the NVMe controller were given
 *	their interrupts (test_irq). The NVMe disk's interrupts arrive and
 *	are counted (test_nvme); a command it never answers has it reset and
 *	the request failed, and the disk goes on (test_nvme_lost); a disk of
 *	4096-byte blocks (tools/x64/ktest64.sh with NVME4K) is read and
 *	written in its own blocks (test_nvme_4k).
 *
 *	The UUIDs they have are written to /boot/KTPCDEV.TXT; when the file
 *	is there from a start before, each must be the same (the device box
 *	keeps them under their names). tools/x64/ktest64.sh is run twice on
 *	one image for that: the second run checks the first's.
 *
 *	The QEMU machine for all of it (tools/x64/ktest64.sh with EXTRA):
 *	  -acpitable file=acpi_battery.aml (tools/x64/tests/acpi_battery.asl)
 *	  -drive file=sata.img,if=none,id=s0 -device ide-hd,drive=s0,bus=ide.0
 *	  -device e1000e,netdev=... (in place of virtio-net)
 *	What the machine does not have is skipped.
 */

#include <sys/machine.h>
#include "kernel.h"
#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <ts/ob.h>
#include <ts/hid.h>
#include <ts/fs.h>
#include <ts/uuid.h>
#include <ts/pcie.h>
#include <ts/usb.h>
#include "../ktest.h"
#include "../../../device/i2c/x86_64/i2c.h"
#include "../../../device/nvme/nvme.h"

#ifdef X64

#define TEXT_MAX	4096
#define UUID_FILE	"/boot/KTPCDEV.TXT"

LOCAL UB	txt[TEXT_MAX];
LOCAL UB	kept[2048];
LOCAL INT	kept_len = 0;
LOCAL UB	now[2048];
LOCAL INT	now_len = 0;

LOCAL BOOL has( CONST UB *t, INT len, CONST char *w )
{
	INT	i, k;

	for ( i = 0; i < len; i++ ) {
		for ( k = 0; w[k] != '\0' && i + k < len && t[i + k] == (UB)w[k]; k++ ) ;
		if ( w[k] == '\0' ) return TRUE;
	}
	return FALSE;
}

LOCAL INT rec( CONST TS_UUID *u, INT recno )
{
	SZ	asz = 0;
	ER	er;
	ID	k = ob_opn_obj(u, OB_OP_R);

	if ( k <= 0 ) return (INT)k;
	er = ob_rea_rec(k, recno, 0, txt, TEXT_MAX - 1, &asz);
	(void)ob_cls_obj(k);
	if ( er < E_OK ) return (INT)er;
	txt[asz] = 0;
	return (INT)asz;
}

LOCAL BOOL in_list( CONST TS_UUID *u )
{
	char	us[TS_UUID_STRLEN + 1];
	INT	n = rec(&ob_uuid_devlist, 0);

	(void)ts_uuid_to_str(u, us, sizeof(us));
	return (BOOL)( n > 0 && has(txt, n, us) );
}

/* The device object of a name among those of a subtype; FALSE when there is none */
LOCAL BOOL find( UINT sub, CONST char *name, TS_UUID *pu )
{
	TS_UUID	all[64];
	T_OBREF	r;
	INT	cnt = 0, i, k;

	if ( ob_lst_obj(OB_T_DEVICE, sub, NULL, all, 64, &cnt) < E_OK ) return FALSE;
	for ( i = 0; i < cnt; i++ ) {
		if ( ob_ref_obj(&all[i], &r) < E_OK ) continue;
		for ( k = 0; name[k] != '\0' && r.name[k] == (UB)name[k]; k++ ) ;
		if ( name[k] == '\0' && r.name[k] == 0 ) {
			*pu = all[i];
			return TRUE;
		}
	}
	return FALSE;
}

/* The UUID a device had at a start before (from the file), or noted for the next */
LOCAL void same_as_before( CONST char *name, CONST TS_UUID *u )
{
	char	us[TS_UUID_STRLEN + 1], line[80];
	INT	n, i;

	(void)ts_uuid_to_str(u, us, sizeof(us));
	n = tm_sprintf((UB *)line, (UB *)"%s %s\n", name, us);
	for ( i = 0; i < n && now_len < (INT)sizeof(now); i++ ) now[now_len++] = (UB)line[i];
	tm_printf((UB *)"  %s %s\n", name, us);
	if ( kept_len > 0 ) {
		line[n - 1] = 0;		/* the line without its end */
		{
			char	head[40];
			INT	k = tm_sprintf((UB *)head, (UB *)"%s ", name);

			if ( has(kept, kept_len, head) ) {
				KT_ASSERT(has(kept, kept_len, line));	/* the same UUID as at the start before */
			}
			(void)k;
		}
	}
}

/* ---------------------------------------------------------------- the i8042 */

LOCAL void test_ps2( void )
{
	T_HIDINFO	h[16];
	TS_UUID		u;
	T_OBREF		r;
	INT		n, i, nps2 = 0;

	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( h[i].usbdev != 0 ) continue;
		nps2++;
		KT_ASSERT_ER(knl_obinput_uuid(h[i].id, &u), E_OK);
		KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
		KT_ASSERT_EQ(r.sub, OB_S_INPUT);
		KT_ASSERT(in_list(&u));
		KT_ASSERT(h[i].keyboard ? ( r.name[3] == 'k' ) : ( r.name[3] == 'a' ));	/* ps2kbd, ps2aux */
		same_as_before((CONST char *)r.name, &u);
	}
	if ( nps2 == 0 ) KT_SKIP("no i8042");
	KT_ASSERT(nps2 >= 1);
}

/* ---------------------------------------------------------------- ACPI's power devices */

LOCAL void test_power( void )
{
	TS_UUID	u, ul;
	INT	len;

	if ( !find(OB_S_POWER, "powerbutton", &u) ) KT_SKIP("no ACPI power devices");
	KT_ASSERT(in_list(&u));
	len = rec(&u, OB_PW_STATE);
	KT_ASSERT(len > 0 && has(txt, len, "PRESSES "));
	same_as_before("powerbutton", &u);

	if ( !find(OB_S_POWER, "battery0", &u) ) {
		tm_printf((UB *)"  no battery (the machine was given no -acpitable)\n");
		return;
	}
	KT_ASSERT(in_list(&u));
	same_as_before("battery0", &u);
	len = rec(&u, OB_PW_STATE);
	KT_ASSERT(len > 0 && has(txt, len, "PRESENT 1") && has(txt, len, "PERCENT ") && has(txt, len, "MODEL TSBAT1"));
	tm_printf((UB *)"  battery0: %s", txt);
	KT_ASSERT(find(OB_S_POWER, "acpower", &ul));
	same_as_before("acpower", &ul);
	len = rec(&ul, OB_PW_STATE);
	KT_ASSERT(len > 0 && has(txt, len, "ONLINE "));
	if ( find(OB_S_POWER, "lid", &ul) ) {
		same_as_before("lid", &ul);
		len = rec(&ul, OB_PW_STATE);
		KT_ASSERT(len > 0 && has(txt, len, "OPEN 1"));
	}

	/* a change told: the test's firmware sends the adapter's notification every fifth _BST */
	{
		T_OBCRE	c;
		T_OBNTF	req;
		T_OBNTM	m;
		TS_UUID	pu;
		ID	port, k;
		SZ	asz;
		INT	waited;
		BOOL	got = FALSE;

		knl_memset(&c, 0, sizeof(c));
		c.type = OB_T_CHANNEL;
		KT_ASSERT_ER(ob_cre_obj(&c, &pu), E_OK);
		port = ob_opn_obj(&pu, OB_OP_READ | OB_OP_WRITE | OB_O_NOWAIT);
		KT_ASSERT(port > 0);
		k = ob_opn_obj(&u, OB_OP_ATRRD);
		KT_ASSERT(k > 0);
		knl_memset(&req, 0, sizeof(req));
		req.events = OB_E_CHANGE;
		KT_ASSERT(ob_ntf_evt(k, OB_REC_ANY, &req, port) > 0);
		for ( waited = 0; waited < 200 && !got; waited++ ) {
			asz = 0;
			while ( ob_rea_rec(port, 0, 0, &m, sizeof(m), &asz) >= E_OK && asz == (SZ)sizeof(m) ) {
				if ( m.event == OB_E_CHANGE && ts_uuid_cmp(&m.uuid, &u) == 0 && m.recno == OB_PW_STATE ) got = TRUE;
				asz = 0;
			}
			if ( !got ) tk_dly_tsk(1000);
		}
		KT_ASSERT(got);
		tm_printf((UB *)"  the battery's change told after %d s\n", waited);
		(void)ob_cls_obj(k);
		(void)ob_cls_obj(port);
		(void)ob_del_obj(&pu);
	}
}

/* ---------------------------------------------------------------- SATA, the network card */

LOCAL void test_sata( void )
{
	TS_UUID	u;
	T_OBREF	r;
	ID	dd;
	UB	*b;
	SZ	asz;

	if ( knl_obdev_uuid((CONST UB *)"sataa", &u) < E_OK ) KT_SKIP("no SATA disk");
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT_EQ(r.sub, OB_S_DISK);
	KT_ASSERT(in_list(&u));
	same_as_before("sataa", &u);
	dd = tk_opn_dev((UB *)"sataa", TD_READ);
	KT_ASSERT(dd > 0);
	b = (UB *)Kmalloc(4096);
	if ( dd > 0 && b != NULL ) {
		KT_ASSERT_ER(tk_srea_dev(dd, 1, b, 4096, &asz), E_OK);
		KT_ASSERT(b[0] == 'E' && b[1] == 'F' && b[2] == 'I');	/* the GPT's header */
	}
	if ( b != NULL ) Kfree(b);
	if ( dd > 0 ) tk_cls_dev(dd, 0);
}

IMPORT ER	knl_e1k_get_mac( UB *mac );

LOCAL void test_e1k( void )
{
	UB	m[6];
	TS_UUID	u;

	if ( knl_e1k_get_mac(m) < E_OK ) KT_SKIP("no Intel Ethernet card");
	KT_ASSERT_ER(knl_obdev_uuid((CONST UB *)"neta", &u), E_OK);
	KT_ASSERT(in_list(&u));
	same_as_before("neta", &u);
}

/* ---------------------------------------------------------------- HID over I2C, on a bus of the test's */

/* A pointer that says where it is: three buttons, X and Y of 0..32767 */
LOCAL CONST UB	fake_rd[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
	0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
	0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81,
	0x02, 0xc0, 0xc0
};

LOCAL UB	fake_in[16];			/* the input report waiting, its length first; none: zeros */
LOCAL INT	fake_cmds = 0, fake_reset = 0;

/*
 * The device of the test at 0x2c: its HID descriptor at register 1, its
 * report descriptor at 2, inputs at 3, commands at 5; a plain read gives
 * the input waiting (after a reset, the length 0 the reset is told by)
 */
LOCAL ER fake_xfer( void *ctx, UH addr, CONST UB *w, INT wn, UB *r, INT rn )
{
	INT	i;

	(void)ctx;
	if ( addr != 0x2c ) return E_IO;
	if ( wn >= 4 && ( w[0] | ( w[1] << 8 ) ) == 5 ) {
		fake_cmds++;
		if ( w[3] == 1 ) fake_reset = 1;
		return E_OK;
	}
	if ( wn == 2 && ( w[0] | ( w[1] << 8 ) ) == 1 ) {
		CONST UB d[30] = { 30, 0, 0, 1, sizeof(fake_rd), 0, 2, 0, 3, 0, 16, 0, 4, 0, 0, 0, 5, 0, 6, 0,
				   0x34, 0x12, 0x78, 0x56, 1, 0, 0, 0, 0, 0 };

		for ( i = 0; i < rn && i < 30; i++ ) r[i] = d[i];
		return E_OK;
	}
	if ( wn == 2 && ( w[0] | ( w[1] << 8 ) ) == 2 ) {
		for ( i = 0; i < rn && i < (INT)sizeof(fake_rd); i++ ) r[i] = fake_rd[i];
		return E_OK;
	}
	if ( wn == 0 ) {
		knl_memset(r, 0, rn);
		if ( fake_reset ) {
			fake_reset = 0;			/* the reset's 0 length */
			return E_OK;
		}
		for ( i = 0; i < rn && i < (INT)sizeof(fake_in); i++ ) r[i] = fake_in[i];
		knl_memset(fake_in, 0, sizeof(fake_in));
		return E_OK;
	}
	return E_IO;
}

LOCAL T_I2CBUS	fake_bus = { NULL, fake_xfer, "i2c-test" };

LOCAL void test_hid_i2c( void )
{
	T_HIDINFO	h[16];
	TS_UUID		u;
	T_OBREF		r;
	INT		hd, n, i, x0 = 0, y0 = 0, x, y;
	UINT		b;
	BOOL		found = FALSE;

	hd = knl_hidi2c_attach(&fake_bus, 0x2c, 1, (CONST UB *)"hidi2c-test", 4);
	KT_ASSERT(hd > 0);
	if ( hd <= 0 ) return;
	KT_ASSERT(fake_cmds >= 2);			/* power on, reset */
	n = knl_hid_list(h, 16);
	for ( i = 0; i < n && i < 16; i++ ) {
		if ( h[i].usbdev == 0 && !h[i].keyboard && h[i].absolute ) found = TRUE;
	}
	KT_ASSERT(found);				/* an absolute pointer, from the report descriptor */
	/* a report: the first button down at the middle of the screen */
	(void)ts_hid_pointer(&x0, &y0, &b);
	fake_in[0] = 7;
	fake_in[1] = 0;
	fake_in[2] = 1;
	fake_in[3] = 0x00;
	fake_in[4] = 0x40;
	fake_in[5] = 0x00;
	fake_in[6] = 0x40;
	tk_dly_tsk(100);
	(void)ts_hid_pointer(&x, &y, &b);
	tm_printf((UB *)"  hidi2c-test: pointer %d,%d -> %d,%d, buttons %x\n", x0, y0, x, y, b);
	KT_ASSERT(x > 0 && y > 0);
	KT_ASSERT(( b & 1 ) != 0);
	/* and let go */
	fake_in[0] = 7;
	fake_in[2] = 0;
	fake_in[4] = 0x40;
	fake_in[6] = 0x40;
	tk_dly_tsk(100);
	(void)ts_hid_pointer(&x, &y, &b);
	KT_ASSERT(( b & 1 ) == 0);
	/* the device object, among the inputs */
	KT_ASSERT(find(OB_S_INPUT, "hidi2c-test", &u));
	KT_ASSERT_ER(ob_ref_obj(&u, &r), E_OK);
	KT_ASSERT(in_list(&u));
}

/*
 * VT-d (vtd.c), on QEMU's intel-iommu (EXTRA="-device intel-iommu"): the
 * DMAR table was read at the start, and what a firmware leaves on is
 * turned off: the invalidation queue, which changes nothing of the
 * devices' DMA, is turned on and then off as the start does it
 */
IMPORT INT	knl_vtd_count( void );
IMPORT ER	knl_vtd_test_qi( void );

LOCAL void test_vtd( void )
{
	if ( knl_vtd_count() == 0 ) KT_SKIP("no DMAR table (EXTRA=\"-device intel-iommu\")");
	KT_ASSERT_ER(knl_vtd_test_qi(), E_OK);
}

/*
 * Each xHCI's and Intel Ethernet card's interrupt: MSI-X when it has
 * it, else MSI, else its line through the _PRT (the run decides which:
 * a controller made with msix=off, msi=off, or the e1000 that has
 * neither), and the interrupts arrive. A plain MSI's address follows
 * the processor it is sent to.
 */
LOCAL void test_irq( void )
{
	T_PCIDEV	d;
	T_USBSTAT	us;
	UINT		intno, cap;
	INT		i, kind, want, seen = 0;
	UW		a0, a1;

	if ( ts_pcie_count() <= 0 ) KT_SKIP("no bus");

	for ( i = 0; i < ts_pcie_count(); i++ ) {
		KT_ASSERT_ER(ts_pcie_get(i, &d), E_OK);
		if ( !( ( d.class_code == 0x0c && d.subclass == 0x03 && d.prog_if == 0x30 )
		     || ( d.class_code == 0x01 && d.subclass == 0x08 && d.prog_if == 0x02 )
		     || ( d.class_code == 0x02 && d.vendor == 0x8086 ) ) ) {
			continue;
		}
		kind = ts_pcie_irq_kind(&d, &intno);
		if ( kind < 0 ) continue;		/* a second card the driver did not take */
		want = ( ts_pcie_cap_find(&d, PCI_CAP_MSIX) != 0 ) ? PCI_IRQ_MSIX
		     : ( ts_pcie_cap_find(&d, PCI_CAP_MSI) != 0 ) ? PCI_IRQ_MSI
		     : ( d.int_pin != 0 ) ? PCI_IRQ_INTX : PCI_IRQ_POLL;
		tm_printf((UB *)"  %02x:%02x.%d %04x:%04x: %s, interrupt %d (expected %s)\n", d.bus, d.dev, d.fn,
			  d.vendor, d.device, ts_pcie_irq_name(kind), intno, ts_pcie_irq_name(want));
		KT_ASSERT_EQ(kind, want);
		KT_ASSERT(( kind == PCI_IRQ_POLL ) == ( intno == 0 ));
		seen++;
		if ( kind == PCI_IRQ_POLL ) continue;
		KT_ASSERT_ER(SetIntTarget(intno, 1), E_OK);
		KT_ASSERT_EQ(GetIntTarget(intno), 1);
		if ( kind == PCI_IRQ_MSI && knl_num_prc >= 2 ) {
			cap = ts_pcie_cap_find(&d, PCI_CAP_MSI);
			a0 = ts_pcie_cfg_read(d.bus, d.dev, d.fn, cap + 4, 4);
			KT_ASSERT_ER(SetIntTarget(intno, 2), E_OK);
			a1 = ts_pcie_cfg_read(d.bus, d.dev, d.fn, cap + 4, 4);
			tm_printf((UB *)"  MSI address %08x on PRC 1, %08x on PRC 2\n", a0, a1);
			KT_ASSERT(a1 != a0);
			KT_ASSERT_ER(SetIntTarget(intno, 0), E_OK);
			KT_ASSERT_EQ(ts_pcie_cfg_read(d.bus, d.dev, d.fn, cap + 4, 4), a0);
		}
		KT_ASSERT_ER(SetIntTarget(intno, 0), E_OK);
	}
	if ( seen == 0 ) KT_SKIP("no xHCI or Intel card was given an interrupt");

	/* the controllers' interrupts arrive, however they were had */
	if ( ts_usb_hc_count() > 0 && ts_usb_stat(&us) >= E_OK ) {
		tm_printf((UB *)"  USB interrupts so far: %u\n", us.irqs);
		KT_ASSERT(us.irqs > 0);
	}
}
/* ---------------------------------------------------------------- NVMe */

LOCAL void nv_fill( UB *b, INT n, UW seed )
{
	INT	i;

	for ( i = 0; i < n; i++ ) b[i] = (UB)( ( seed + (UW)i * 7 + ( (UW)i >> 9 ) ) & 0xff );
}

LOCAL BOOL nv_eq( CONST UB *a, CONST UB *b, INT n )
{
	INT	i;

	for ( i = 0; i < n && a[i] == b[i]; i++ ) ;
	return ( i == n );
}

LOCAL BOOL nv_same( CONST UB *b, INT n, UW seed )
{
	INT	i;

	for ( i = 0; i < n; i++ ) {
		if ( b[i] != (UB)( ( seed + (UW)i * 7 + ( (UW)i >> 9 ) ) & 0xff ) ) return FALSE;
	}
	return TRUE;
}

/*
 * The disk started from: taken with its interrupt (MSI-X on QEMU's
 * nvme), the interrupts arrive as its reads complete, none is found by
 * the look in their place, and the device's object shows their count
 */
LOCAL void test_nvme( void )
{
	T_NVSTAT	a, b;
	TS_UUID		u;
	UB		*buf, *meta = NULL;
	SZ		asz;
	INT		i, n;
	ID		dd;

	if ( knl_nvme_stat((CONST UB *)"nvmea", &a) < E_OK ) KT_SKIP("no NVMe disk");
	tm_printf((UB *)"  nvmea: %s, interrupt %d (%s): %u interrupts, %u commands, %u without their interrupt,"
		  " %u not answered, %u resets; blocks of %d bytes; host memory buffer %ld KB%s\n",
		  ( a.state == NV_LIVE ) ? "live" : "NOT live", a.intno, ts_pcie_irq_name(a.irqkind), a.nint,
		  a.ncmd, a.nlate, a.ntmo, a.nreset, a.blksz, (D)( a.hmb_bytes >> 10 ), a.hmb_on ? " in use" : "");
	KT_ASSERT_EQ(a.state, NV_LIVE);
	KT_ASSERT_EQ(a.blksz, 512);
	KT_ASSERT_EQ(a.irqkind, PCI_IRQ_MSIX);
	KT_ASSERT(a.intno != 0);
	KT_ASSERT(a.nint > 0);
	KT_ASSERT(a.ncmd > 0);
	KT_ASSERT(a.nlate * 100 <= a.ncmd);
	KT_ASSERT_EQ(a.shst_ms, 0);

	dd = tk_opn_dev((UB *)"nvmea", TD_READ);
	KT_ASSERT(dd > 0);
	buf = (UB *)Kmalloc(65536);
	if ( dd > 0 && buf != NULL ) {
		for ( i = 0; i < 16; i++ ) {
			KT_ASSERT_ER(tk_srea_dev(dd, i * 128, buf, 65536, &asz), E_OK);
		}
		KT_ASSERT_ER(knl_nvme_stat((CONST UB *)"nvmea", &b), E_OK);
		tm_printf((UB *)"  16 reads of 64KB: %u interrupts, %u commands, %u without their interrupt\n",
			  b.nint - a.nint, b.ncmd - a.ncmd, b.nlate - a.nlate);
		KT_ASSERT(b.ncmd - a.ncmd >= 16);
		KT_ASSERT(b.nint - a.nint >= 16);
		KT_ASSERT_EQ(b.nlate - a.nlate, 0);
	}
	if ( buf != NULL ) Kfree(buf);
	if ( dd > 0 ) tk_cls_dev(dd, 0);

	/* the count in the device's object: "irq":{...,"count":[n]} */
	KT_ASSERT_ER(knl_obdev_uuid((CONST UB *)"nvmea", &u), E_OK);
	n = knl_ob_meta_all(&u, 0, &meta, NULL);
	KT_ASSERT(n > 0);
	if ( n > 0 && meta != NULL ) {
		char	line[81];
		INT	k, j;

		for ( k = 0; k + 4 <= n && !has(meta + k, 4, "\"irq"); k++ ) ;
		for ( j = 0; j < 80 && k + j < n && meta[k + j] != '}'; j++ ) line[j] = (char)meta[k + j];
		line[j] = '\0';
		tm_printf((UB *)"  the object's interrupts: %s}\n", line);
		KT_ASSERT(has(meta, n, "\"count\":["));
	}
	if ( meta != NULL ) Kfree(meta);
}

/*
 * A command the controller never sees (its doorbell not rung): the read
 * fails with E_IO after the command's time (10 s), the controller is
 * reset and its queues made again, and the next read of the same block
 * comes back whole
 */
LOCAL void test_nvme_lost( void )
{
	T_NVSTAT	a, b;
	UB		*buf, *was;
	SZ		asz;
	SYSTIM		t0, t1;
	UW		ms;
	ID		dd;
	ER		er;

	if ( knl_nvme_stat((CONST UB *)"nvmea", &a) < E_OK ) KT_SKIP("no NVMe disk");
	dd = tk_opn_dev((UB *)"nvmea", TD_READ);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;
	buf = (UB *)Kmalloc(4096);
	was = (UB *)Kmalloc(4096);
	if ( buf == NULL || was == NULL ) {
		if ( buf != NULL ) Kfree(buf);
		if ( was != NULL ) Kfree(was);
		tk_cls_dev(dd, 0);
		KT_SKIP("no memory");
	}
	KT_ASSERT_ER(tk_srea_dev(dd, 1, was, 4096, &asz), E_OK);
	KT_ASSERT(was[0] == 'E' && was[1] == 'F' && was[2] == 'I');	/* the GPT's header */

	KT_ASSERT_ER(knl_nvme_test_drop((CONST UB *)"nvmea"), E_OK);
	knl_memset(buf, 0, 4096);
	(void)tk_get_otm(&t0);
	er = tk_srea_dev(dd, 1, buf, 4096, &asz);
	(void)tk_get_otm(&t1);
	ms = t1.lo - t0.lo;
	KT_ASSERT_ER(knl_nvme_stat((CONST UB *)"nvmea", &b), E_OK);
	tm_printf((UB *)"  a lost read: %d after %u ms; %u not answered, %u resets, %s\n", er, ms,
		  b.ntmo - a.ntmo, b.nreset - a.nreset, ( b.state == NV_LIVE ) ? "live again" : "NOT live");
	KT_ASSERT_ER(er, E_IO);
	KT_ASSERT(ms >= 9900 && ms < 30000);
	KT_ASSERT_EQ(b.ntmo - a.ntmo, 1);
	KT_ASSERT_EQ(b.nreset - a.nreset, 1);
	KT_ASSERT_EQ(b.state, NV_LIVE);

	/* after the reset: the same block, whole, and the interrupts still arrive */
	knl_memset(buf, 0, 4096);
	KT_ASSERT_ER(tk_srea_dev(dd, 1, buf, 4096, &asz), E_OK);
	KT_ASSERT(nv_eq(buf, was, 4096));
	KT_ASSERT_ER(tk_srea_dev(dd, 0, buf, 4096, &asz), E_OK);
	KT_ASSERT_ER(knl_nvme_stat((CONST UB *)"nvmea", &a), E_OK);
	KT_ASSERT(a.nint > b.nint);
	Kfree(buf);
	Kfree(was);
	tk_cls_dev(dd, 0);
}

/*
 * A namespace of 4096-byte blocks (QEMU's nvme with logical_block_size=4096,
 * tools/x64/ktest64.sh with NVME4K): its blocks are the device's, read
 * and written straight from memory, through the driver's buffer (memory
 * not on a double word), in more than one command, and through to the
 * medium (FUA); a request of a part of a block is refused; it is one
 * whole unit, its partition table not read.
 */
#define NV4K		4096

LOCAL void test_nvme_4k( void )
{
	static CONST char * CONST names[] = { "nvmeb", "nvmec", "nvmed" };
	T_NVSTAT	st;
	DiskInfo	di;
	T_DEVFUA	fw;
	UB		*w, *r;
	UW		iomax = 0;
	SZ		asz;
	INT		i, big = 3 * 1024 * 1024;
	ID		dd;
	CONST char	*dev = NULL;

	for ( i = 0; i < 3 && dev == NULL; i++ ) {
		if ( knl_nvme_stat((CONST UB *)names[i], &st) >= E_OK && st.blksz == NV4K ) dev = names[i];
	}
	if ( dev == NULL ) KT_SKIP("no NVMe disk of 4096-byte blocks (ktest64.sh with NVME4K)");
	dd = tk_opn_dev((UB *)dev, TD_UPDATE);
	KT_ASSERT(dd > 0);
	if ( dd <= 0 ) return;
	w = (UB *)Kmalloc(big + 16);
	r = (UB *)Kmalloc(big + 16);
	if ( w == NULL || r == NULL ) {
		if ( w != NULL ) Kfree(w);
		if ( r != NULL ) Kfree(r);
		tk_cls_dev(dd, 0);
		KT_SKIP("no memory");
	}
	KT_ASSERT_ER(tk_srea_dev(dd, TDN_DISKINFO, &di, sizeof(di), &asz), E_OK);
	(void)tk_srea_dev(dd, TDN_IOMAX, &iomax, sizeof(iomax), &asz);
	tm_printf((UB *)"  %s: %ld blocks of %d bytes, %u bytes a command at most\n", dev, (D)di.blockcont_d,
		  di.blocksize, iomax);
	KT_ASSERT_EQ(di.blocksize, NV4K);
	KT_ASSERT_EQ((UD)di.blockcont_d, st.capacity);
	KT_ASSERT(di.blockcont_d > 1024);
	KT_ASSERT_EQ(iomax % NV4K, 0);

	/* straight from memory */
	nv_fill(w, 3 * NV4K, 11);
	KT_ASSERT_ER(tk_swri_dev_d(dd, 10, w, 3 * NV4K, &asz), E_OK);
	KT_ASSERT_EQ(asz, 3 * NV4K);
	knl_memset(r, 0, 3 * NV4K);
	KT_ASSERT_ER(tk_srea_dev_d(dd, 10, r, 3 * NV4K, &asz), E_OK);
	KT_ASSERT(nv_same(r, 3 * NV4K, 11));
	/* one block alone is the second of them */
	KT_ASSERT_ER(tk_srea_dev_d(dd, 11, r, NV4K, &asz), E_OK);
	KT_ASSERT(nv_eq(r, w + NV4K, NV4K));

	/* through the driver's buffer: memory not on a double word */
	nv_fill(w + 1, 2 * NV4K, 23);
	KT_ASSERT_ER(tk_swri_dev_d(dd, 20, w + 1, 2 * NV4K, &asz), E_OK);
	knl_memset(r, 0, 2 * NV4K + 8);
	KT_ASSERT_ER(tk_srea_dev_d(dd, 20, r + 3, 2 * NV4K, &asz), E_OK);
	KT_ASSERT(nv_same(r + 3, 2 * NV4K, 23));

	/* more than one command: 3MB */
	nv_fill(w, big, 37);
	KT_ASSERT_ER(tk_swri_dev_d(dd, 100, w, big, &asz), E_OK);
	knl_memset(r, 0, big);
	KT_ASSERT_ER(tk_srea_dev_d(dd, 100, r, big, &asz), E_OK);
	KT_ASSERT(asz == big && nv_same(r, big, 37));

	/* through to the medium */
	nv_fill(w, NV4K, 41);
	fw.start = 30;
	fw.buf = w;
	fw.size = NV4K;
	KT_ASSERT(tk_swri_dev(dd, TDN_FUAWRITE, &fw, sizeof(fw), &asz) >= E_OK);
	fw.size = 512;
	KT_ASSERT(tk_swri_dev(dd, TDN_FUAWRITE, &fw, sizeof(fw), &asz) < E_OK);
	KT_ASSERT_ER(tk_srea_dev_d(dd, 30, r, NV4K, &asz), E_OK);
	KT_ASSERT(nv_same(r, NV4K, 41));

	/* a part of a block, the last block, one past it */
	KT_ASSERT(tk_srea_dev_d(dd, 0, r, 512, &asz) < E_OK);
	KT_ASSERT_ER(tk_srea_dev_d(dd, di.blockcont_d - 1, r, NV4K, &asz), E_OK);
	KT_ASSERT(tk_srea_dev_d(dd, di.blockcont_d, r, NV4K, &asz) < E_OK);
	KT_ASSERT(tk_swri_dev(dd, TDN_FLUSH, NULL, 0, &asz) >= E_OK);
	tk_cls_dev(dd, 0);

	/* one unit: no partition read on it */
	{
		char	p0[16];

		tm_sprintf((UB *)p0, (UB *)"%s0", dev);
		dd = tk_opn_dev((UB *)p0, TD_READ);
		KT_ASSERT(dd < E_OK);
		if ( dd > 0 ) tk_cls_dev(dd, 0);
	}
	Kfree(w);
	Kfree(r);
}

EXPORT void ktest_pcdev( void )
{
	INT	fd = fs_open(UUID_FILE, 0);

	kept_len = 0;
	now_len = 0;
	if ( fd >= 0 ) {
		INT	n = fs_read(fd, kept, sizeof(kept) - 1);

		kept_len = ( n > 0 ) ? n : 0;
		fs_close(fd);
		tm_printf((UB *)"  the UUIDs of a start before: %d bytes\n", kept_len);
	}
	KT_RUN(test_ps2);
	KT_RUN(test_power);
	KT_RUN(test_sata);
	KT_RUN(test_e1k);
	KT_RUN(test_hid_i2c);
	KT_RUN(test_vtd);
	KT_RUN(test_irq);
	KT_RUN(test_nvme);
	KT_RUN(test_nvme_lost);
	KT_RUN(test_nvme_4k);
	fd = fs_open(UUID_FILE, O_WRONLY | O_CREAT | O_TRUNC);
	if ( fd >= 0 ) {
		(void)fs_write(fd, now, now_len);
		fs_close(fd);
		fs_sync();
	}
}

#endif /* X64 */
