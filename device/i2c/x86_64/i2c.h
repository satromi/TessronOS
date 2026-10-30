/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	i2c.h (x64 PC)
 *	The PC's I2C buses (T_I2CBUS, ../i2cbus.h) are the chipset's LPSS
 *	controllers (dw_i2c.c); a test gives one of its own (ktest_pcdev.c).
 */

#ifndef _DEVICE_I2C_X86_64_I2C_
#define _DEVICE_I2C_X86_64_I2C_

#include "../i2cbus.h"

/* The LPSS I2C controllers on the PCI bus brought up: how many */
IMPORT INT	knl_dwi2c_init( void );
/*
 * The controller of a PCI device and function (an ACPI _ADR: device << 16
 * | function), set up for a device of hz on it: acpi is its ACPI device,
 * whose _PS0 is run and whose SSCN, FMCN and FPCN give the clock counts
 * (NULL none). NULL when there is no such controller.
 */
IMPORT T_I2CBUS	*knl_dwi2c_bus( UW adr, void *acpi, UW hz );

/*
 * A HID device on a bus: its HID descriptor read at the register desc,
 * its report descriptor, powered and reset, and its reports read and
 * handed to the HID layer (knl_hid_ext_report) as a device object of
 * the name. A pen or a touch screen is put into the mode it is read in
 * (SET_REPORT of its Input Mode feature). The reports are read every
 * period ms. Answers the HID layer's handle.
 */
IMPORT INT	knl_hidi2c_attach( T_I2CBUS *bus, UH addr, UH desc, CONST UB *name, INT period );
/* The devices of the ACPI namespace that are HID over I2C (PNP0C50) attached */
IMPORT INT	knl_hidi2c_init( void );

#endif /* _DEVICE_I2C_X86_64_I2C_ */
