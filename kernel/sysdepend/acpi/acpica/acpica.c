/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	acpica.c
 *	The kernel's use of the ACPI interpreter (ACPICA, tsacpi_os.h):
 *
 *	- start: the tables, the namespace (DSDT and SSDTs), the hardware
 *	  put in ACPI mode, the SCI, the objects initialized (_INI) and the
 *	  general purpose events that have methods enabled;
 *	- the power button, both the fixed one of the FADT and one the
 *	  firmware tells of by notification (a PNP0C0C device's 0x80, the
 *	  Surface's MSHW0028/MSHW0040 0xC6): the session ends as the
 *	  desktop's 〈電源を切る〉 ends it. Every notification is said on
 *	  the console with the device it is about, for finding out what a
 *	  machine's buttons send;
 *	- the reset through the FADT (off, S5: acpica_off.c);
 *	- the devices of the namespace as text (their paths, _HID and _CID,
 *	  _UID, _ADR, _STA and what their _CRS asks for: memory, ports,
 *	  interrupts, GPIO and I2C, SPI and UART connections), which the
 *	  boot log writes to ACPI.TXT on the EFI system partition.
 *
 *	Built with the interpreter by tools/x64/acpica/build.sh.
 */

#include "acpi.h"
#include "accommon.h"
#include "tsacpi_os.h"

#define _COMPONENT	ACPI_OS_SERVICES
ACPI_MODULE_NAME("tsacpi")

static int	ready = 0;

int knl_acpica_ready( void )
{
	return ready;
}

/* ---------------------------------------------------------------- the power button */

static UINT32 fixed_power_button( void *Context )
{
	(void)Context;
	knl_acpi_os_puts("acpi: the power button (fixed event)\n");
	(void)AcpiOsExecute(OSL_NOTIFY_HANDLER, (ACPI_OSD_EXEC_CALLBACK)knl_acpi_os_power_button, NULL);
	return ACPI_INTERRUPT_HANDLED;
}

static int has_id( ACPI_HANDLE h, const char *id )
{
	ACPI_DEVICE_INFO	*info = NULL;
	int			yes = 0;
	UINT32			i;

	if ( ACPI_FAILURE(AcpiGetObjectInfo(h, &info)) || info == NULL ) {
		return 0;
	}
	if ( ( info->Valid & ACPI_VALID_HID ) && strcmp(info->HardwareId.String, id) == 0 ) yes = 1;
	if ( info->Valid & ACPI_VALID_CID ) {
		for ( i = 0; i < info->CompatibleIdList.Count; i++ ) {
			if ( strcmp(info->CompatibleIdList.Ids[i].String, id) == 0 ) yes = 1;
		}
	}
	ACPI_FREE(info);
	return yes;
}

/* Every notification the firmware sends, said; the power button's acted on */
static void notified( ACPI_HANDLE Device, UINT32 Value, void *Context )
{
	char		path[128], b[200];
	ACPI_BUFFER	buf = { sizeof(path), path };

	(void)Context;
	if ( ACPI_FAILURE(AcpiGetName(Device, ACPI_FULL_PATHNAME, &buf)) ) {
		strcpy(path, "?");
	}
	snprintf(b, sizeof(b), "acpi: notification %02x for %s\n", Value, path);
	knl_acpi_os_puts(b);
	knl_acpi_os_notify(Device, Value);
	if ( ( Value == 0x80 && has_id(Device, "PNP0C0C") )
	  || ( Value == 0xc6 && ( has_id(Device, "MSHW0040") || has_id(Device, "MSHW0028") ) ) ) {
		knl_acpi_os_power_button();
	}
}

/* ---------------------------------------------------------------- start */

static int	ec_early(void);
static void	ec_late(void);
static void	wake_gpes(void);
static void	platform_osc(void);

/*
 * The order: the tables and the namespace; the embedded controller of
 * the ECDT, when there is one, before anything runs that may read it;
 * the hardware in ACPI mode with the SCI; the objects initialized (_STA
 * and _INI); the platform told what the OS does (\_SB._OSC) and that
 * interrupts go through the IOAPIC (\_PIC(1)); the embedded controller
 * of the DSDT when the ECDT named none; the GPEs of the devices that
 * wake the machine (_PRW) kept for waking, and the others with methods
 * enabled; the buttons and the notifications
 */
int knl_acpica_init( void )
{
	ACPI_STATUS	s;
	ACPI_OBJECT	arg;
	ACPI_OBJECT_LIST args = { 1, &arg };

	/*
	 * The interpreter as lenient as the firmware's AML was tested with:
	 * a field whose access width runs past the end of its region only
	 * by the rounding up to that width is read and written (an
	 * OperationRegion of 0x1003 bytes with a DWordAcc field at 0x1000),
	 * a method's implicit return taken, and the like
	 */
	AcpiGbl_EnableInterpreterSlack = TRUE;
	s = AcpiInitializeSubsystem();
	if ( ACPI_SUCCESS(s) ) s = AcpiInitializeTables(NULL, 32, FALSE);
	if ( ACPI_SUCCESS(s) ) knl_acpica_wake_found();
	if ( ACPI_SUCCESS(s) ) s = AcpiLoadTables();
	if ( ACPI_SUCCESS(s) ) (void)ec_early();
	if ( ACPI_SUCCESS(s) ) s = AcpiEnableSubsystem(ACPI_FULL_INITIALIZATION);
	if ( ACPI_SUCCESS(s) ) s = AcpiInitializeObjects(ACPI_FULL_INITIALIZATION);
	if ( ACPI_FAILURE(s) ) {
		char	b[96];

		snprintf(b, sizeof(b), "acpi: the interpreter did not start: %s\n", AcpiFormatException(s));
		knl_acpi_os_puts(b);
		return -1;
	}
	ready = 1;
	platform_osc();
	arg.Type = ACPI_TYPE_INTEGER;
	arg.Integer.Value = 1;				/* the APIC model */
	(void)AcpiEvaluateObject(NULL, "\\_PIC", &args, NULL);
	ec_late();
	wake_gpes();
	(void)AcpiUpdateAllGpes();
	if ( ACPI_SUCCESS(AcpiInstallFixedEventHandler(ACPI_EVENT_POWER_BUTTON, fixed_power_button, NULL)) ) {
		(void)AcpiEnableEvent(ACPI_EVENT_POWER_BUTTON, 0);
	}
	(void)AcpiInstallNotifyHandler(ACPI_ROOT_OBJECT, ACPI_ALL_NOTIFY, notified, NULL);
	knl_acpi_os_puts("acpi: the interpreter started: namespace loaded, SCI and events on\n");
	return 0;
}

/* ---------------------------------------------------------------- reset (off: acpica_off.c) */

int knl_acpica_reset( void )
{
	if ( !ready ) {
		return -1;
	}
	(void)AcpiReset();
	knl_acpi_os_stall(100000);
	return -1;
}

/* ---------------------------------------------------------------- devices by their IDs */

static ACPI_STATUS found( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	UINT32	sta = ACPI_UINT32_MAX;

	(void)Level;
	(void)ReturnValue;
	if ( ACPI_FAILURE(AcpiUtExecute_STA((ACPI_NAMESPACE_NODE *)h, &sta)) || ( sta & 1 ) ) {
		(*(int *)Context)++;
	}
	return AE_OK;
}

int knl_acpica_has_hid( const char *hid )
{
	int	n = 0;

	if ( !ready ) {
		return 0;
	}
	(void)AcpiGetDevices((char *)hid, found, &n, NULL);
	return n;
}

/* ---------------------------------------------------------------- devices and their methods */

int knl_acpica_int( void *dev, const char *method, unsigned long long *v );

typedef struct {
	int		want, seen;
	ACPI_HANDLE	h;
} PICK;

static ACPI_STATUS pick( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	PICK	*p = (PICK *)Context;
	UINT32	sta = ACPI_UINT32_MAX;

	(void)Level;
	(void)ReturnValue;
	if ( ACPI_SUCCESS(AcpiUtExecute_STA((ACPI_NAMESPACE_NODE *)h, &sta)) && ( sta & 1 ) == 0 ) {
		return AE_OK;
	}
	if ( p->seen++ == p->want ) {
		p->h = h;
		return AE_CTRL_TERMINATE;
	}
	return AE_OK;
}

void *knl_acpica_device( const char *hid, int index )
{
	PICK	p = { index, 0, NULL };

	if ( !ready ) return NULL;
	(void)AcpiGetDevices((char *)hid, pick, &p, NULL);
	return p.h;
}

/*
 * The SoundWire controllers the namespace has: a device whose _ADR has link
 * type 4 in bits 28-31 (a PCI function's _ADR, device << 16 | function, never
 * does), not turned off by its _STA. A firmware may declare one with nothing
 * on it, so the devices below are counted too: a peripheral has an _ADR of
 * its own.
 */
typedef struct {
	int	ctrls, devs;
	char	*path;
	int	max;
} SDW;

static ACPI_STATUS sdw_look( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	SDW			*s = (SDW *)Context;
	unsigned long long	adr = 0, a;
	UINT32			sta = ACPI_UINT32_MAX;
	ACPI_HANDLE		c = NULL;

	(void)Level;
	(void)ReturnValue;
	if ( knl_acpica_int(h, "_ADR", &adr) != 0 || adr > 0xffffffffULL || ( ( adr >> 28 ) & 0xf ) != 4 ) {
		return AE_OK;
	}
	if ( ACPI_SUCCESS(AcpiUtExecute_STA((ACPI_NAMESPACE_NODE *)h, &sta)) && ( sta & 1 ) == 0 ) {
		return AE_OK;
	}
	if ( s->ctrls++ == 0 ) (void)knl_acpica_path(h, s->path, s->max);
	while ( ACPI_SUCCESS(AcpiGetNextObject(ACPI_TYPE_DEVICE, h, c, &c)) ) {
		a = 0;
		if ( knl_acpica_int(c, "_ADR", &a) == 0 && a != 0 ) s->devs++;
	}
	return AE_OK;
}

int knl_acpica_sdw( int *devs, char *path, int max )
{
	SDW	s = { 0, 0, path, max };

	if ( max > 0 ) path[0] = '\0';
	if ( ready ) {
		(void)AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX, sdw_look, NULL, &s, NULL);
	}
	*devs = s.devs;
	return s.ctrls;
}

int knl_acpica_path( void *dev, char *buf, int max )
{
	ACPI_BUFFER	b = { (ACPI_SIZE)max, buf };

	if ( dev == NULL || ACPI_FAILURE(AcpiGetName(dev, ACPI_FULL_PATHNAME, &b)) ) {
		if ( max > 0 ) buf[0] = '\0';
		return -1;
	}
	return 0;
}

int knl_acpica_int( void *dev, const char *method, unsigned long long *v )
{
	ACPI_OBJECT	o;
	ACPI_BUFFER	b = { sizeof(o), &o };

	if ( dev == NULL || ACPI_FAILURE(AcpiEvaluateObjectTyped(dev, (char *)method, NULL, &b, ACPI_TYPE_INTEGER)) ) {
		return -1;
	}
	*v = o.Integer.Value;
	return 0;
}

int knl_acpica_pkg( void *dev, const char *method, unsigned long long *v, char (*s)[32], int max )
{
	ACPI_BUFFER	b = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT	*o;
	int		i, n;

	if ( dev == NULL || ACPI_FAILURE(AcpiEvaluateObjectTyped(dev, (char *)method, NULL, &b, ACPI_TYPE_PACKAGE)) ) {
		return -1;
	}
	o = (ACPI_OBJECT *)b.Pointer;
	n = (int)o->Package.Count;
	for ( i = 0; i < n && i < max; i++ ) {
		ACPI_OBJECT	*e = &o->Package.Elements[i];

		v[i] = 0;
		s[i][0] = '\0';
		if ( e->Type == ACPI_TYPE_INTEGER ) {
			v[i] = e->Integer.Value;
		} else if ( e->Type == ACPI_TYPE_STRING ) {
			strncpy(s[i], e->String.Pointer, 31);
			s[i][31] = '\0';
		} else if ( e->Type == ACPI_TYPE_BUFFER ) {
			int	k;

			for ( k = 0; k < (int)e->Buffer.Length && k < 31 && e->Buffer.Pointer[k] != 0; k++ ) {
				s[i][k] = (char)e->Buffer.Pointer[k];
			}
			s[i][k] = '\0';
		}
	}
	ACPI_FREE(b.Pointer);
	return n;
}

/* ---------------------------------------------------------------- I2C devices */

typedef struct {
	unsigned int	addr, hz;
	char		*ctrl, *gctrl;
	int		max, gmax, pin, found;
	int		irq, irq_mode;
} I2CRES;

static ACPI_STATUS i2c_res( ACPI_RESOURCE *r, void *Context )
{
	I2CRES	*q = (I2CRES *)Context;

	if ( r->Type == ACPI_RESOURCE_TYPE_SERIAL_BUS && r->Data.CommonSerialBus.Type == ACPI_RESOURCE_SERIAL_TYPE_I2C
	  && !q->found ) {
		q->addr = r->Data.I2cSerialBus.SlaveAddress;
		q->hz = r->Data.I2cSerialBus.ConnectionSpeed;
		if ( r->Data.CommonSerialBus.ResourceSource.StringPtr != NULL ) {
			strncpy(q->ctrl, r->Data.CommonSerialBus.ResourceSource.StringPtr, q->max - 1);
			q->ctrl[q->max - 1] = '\0';
		}
		q->found = 1;
	}
	if ( r->Type == ACPI_RESOURCE_TYPE_GPIO && r->Data.Gpio.ConnectionType == ACPI_RESOURCE_GPIO_TYPE_INT
	  && q->pin < 0 && r->Data.Gpio.PinTableLength > 0 ) {
		q->pin = r->Data.Gpio.PinTable[0];
		if ( r->Data.Gpio.ResourceSource.StringPtr != NULL && q->gctrl != NULL ) {
			strncpy(q->gctrl, r->Data.Gpio.ResourceSource.StringPtr, q->gmax - 1);
			q->gctrl[q->gmax - 1] = '\0';
		}
	}
	/* an input of the IOAPICs: a GPIO pad the firmware routed there, or a line of its own */
	if ( r->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ && q->irq < 0 && r->Data.ExtendedIrq.InterruptCount > 0 ) {
		q->irq = (int)r->Data.ExtendedIrq.Interrupts[0];
		q->irq_mode = ( r->Data.ExtendedIrq.Triggering == ACPI_EDGE_SENSITIVE ? 1 : 0 )
			    | ( r->Data.ExtendedIrq.Polarity == ACPI_ACTIVE_LOW ? 2 : 0 );
	}
	if ( r->Type == ACPI_RESOURCE_TYPE_IRQ && q->irq < 0 && r->Data.Irq.InterruptCount > 0 ) {
		q->irq = (int)r->Data.Irq.Interrupts[0];
		q->irq_mode = ( r->Data.Irq.Triggering == ACPI_EDGE_SENSITIVE ? 1 : 0 )
			    | ( r->Data.Irq.Polarity == ACPI_ACTIVE_LOW ? 2 : 0 );
	}
	return AE_OK;
}

int knl_acpica_i2c( void *dev, unsigned int *addr, unsigned int *hz, char *ctrl, int max,
		    int *gpio_pin, char *gpio_ctrl, int gmax, int *irq, int *irq_mode )
{
	I2CRES	q;

	if ( dev == NULL || max <= 0 ) return -1;
	ctrl[0] = '\0';
	if ( gpio_ctrl != NULL && gmax > 0 ) gpio_ctrl[0] = '\0';
	q.addr = q.hz = 0;
	q.ctrl = ctrl;
	q.max = max;
	q.gctrl = gpio_ctrl;
	q.gmax = gmax;
	q.pin = -1;
	q.found = 0;
	q.irq = -1;
	q.irq_mode = 0;
	(void)AcpiWalkResources(dev, METHOD_NAME__CRS, i2c_res, &q);
	*addr = q.addr;
	*hz = q.hz;
	if ( gpio_pin != NULL ) *gpio_pin = q.pin;
	if ( irq != NULL ) *irq = q.irq;
	if ( irq_mode != NULL ) *irq_mode = q.irq_mode;
	return q.found ? 0 : -1;
}

static ACPI_STATUS gpioint_res( ACPI_RESOURCE *r, void *Context )
{
	int	*mode = (int *)Context;

	if ( *mode < 0 && r->Type == ACPI_RESOURCE_TYPE_GPIO && r->Data.Gpio.ConnectionType == ACPI_RESOURCE_GPIO_TYPE_INT ) {
		*mode = ( r->Data.Gpio.Triggering == ACPI_EDGE_SENSITIVE ? 1 : 0 )
		      | ( r->Data.Gpio.Polarity == ACPI_ACTIVE_LOW ? 2 : 0 )
		      | ( r->Data.Gpio.Polarity == ACPI_ACTIVE_BOTH ? 4 : 0 );
	}
	return AE_OK;
}

int knl_acpica_gpioint( void *dev, int *mode )
{
	int	m = -1;

	if ( dev == NULL ) return -1;
	(void)AcpiWalkResources(dev, METHOD_NAME__CRS, gpioint_res, &m);
	if ( m < 0 ) return -1;
	*mode = m;
	return 0;
}

int knl_acpica_irq( void *dev, int *irq, int *mode )
{
	I2CRES	q;

	if ( dev == NULL ) return -1;
	memset(&q, 0, sizeof(q));
	q.pin = -1;
	q.irq = -1;
	q.found = 1;		/* no I2C connection is looked at, nor its strings written */
	(void)AcpiWalkResources(dev, METHOD_NAME__CRS, i2c_res, &q);
	if ( q.irq < 0 ) return -1;
	*irq = q.irq;
	*mode = q.irq_mode;
	return 0;
}

int knl_acpica_call( void *dev, const char *method )
{
	ACPI_HANDLE	h;

	if ( dev == NULL || ACPI_FAILURE(AcpiGetHandle(dev, (char *)method, &h)) ) return -1;
	return ACPI_FAILURE(AcpiEvaluateObject(dev, (char *)method, NULL, NULL)) ? -2 : 0;
}

void *knl_acpica_handle( const char *path )
{
	ACPI_HANDLE	h = NULL;

	if ( !ready || ACPI_FAILURE(AcpiGetHandle(NULL, (char *)path, &h)) ) return NULL;
	return h;
}

int knl_acpica_dsm_int( void *dev, const unsigned char *uuid, unsigned int rev, unsigned int func,
			unsigned long long *v )
{
	ACPI_OBJECT		arg[4], *o;
	ACPI_OBJECT_LIST	args = { 4, arg };
	ACPI_BUFFER		b = { ACPI_ALLOCATE_BUFFER, NULL };
	int			r = -1;

	if ( dev == NULL ) return -1;
	arg[0].Type = ACPI_TYPE_BUFFER;
	arg[0].Buffer.Length = 16;
	arg[0].Buffer.Pointer = (UINT8 *)uuid;
	arg[1].Type = ACPI_TYPE_INTEGER;
	arg[1].Integer.Value = rev;
	arg[2].Type = ACPI_TYPE_INTEGER;
	arg[2].Integer.Value = func;
	arg[3].Type = ACPI_TYPE_PACKAGE;
	arg[3].Package.Count = 0;
	arg[3].Package.Elements = NULL;
	if ( ACPI_FAILURE(AcpiEvaluateObject(dev, "_DSM", &args, &b)) || b.Pointer == NULL ) return -1;
	o = (ACPI_OBJECT *)b.Pointer;
	if ( o->Type == ACPI_TYPE_INTEGER ) {
		*v = o->Integer.Value;
		r = 0;
	} else if ( o->Type == ACPI_TYPE_BUFFER && o->Buffer.Length > 0 ) {
		UINT32	i;

		*v = 0;
		for ( i = 0; i < o->Buffer.Length && i < 8; i++ ) *v |= (unsigned long long)o->Buffer.Pointer[i] << ( 8 * i );
		r = 0;
	}
	ACPI_FREE(b.Pointer);
	return r;
}

/* ---------------------------------------------------------------- buffers, GPIO lines, memory, dependencies */

int knl_acpica_buf( void *dev, const char *method, unsigned char *buf, int max )
{
	ACPI_BUFFER	b = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT	*o;
	int		n, i;

	if ( dev == NULL || ACPI_FAILURE(AcpiEvaluateObjectTyped(dev, (char *)method, NULL, &b, ACPI_TYPE_BUFFER)) ) {
		return -1;
	}
	o = (ACPI_OBJECT *)b.Pointer;
	n = (int)o->Buffer.Length;
	for ( i = 0; i < n && i < max; i++ ) buf[i] = o->Buffer.Pointer[i];
	ACPI_FREE(b.Pointer);
	return n;
}

typedef struct {
	int		want, seen, found;
	unsigned int	pin;
	int		flags;
	char		*ctrl;
	int		max;
} GPIORES;

static ACPI_STATUS gpio_res( ACPI_RESOURCE *r, void *Context )
{
	GPIORES	*q = (GPIORES *)Context;

	if ( r->Type != ACPI_RESOURCE_TYPE_GPIO || r->Data.Gpio.ConnectionType != ACPI_RESOURCE_GPIO_TYPE_IO
	  || r->Data.Gpio.PinTableLength == 0 ) {
		return AE_OK;
	}
	if ( q->seen++ != q->want ) return AE_OK;
	q->pin = r->Data.Gpio.PinTable[0];
	q->flags = ( r->Data.Gpio.Polarity == ACPI_ACTIVE_LOW ? 1 : 0 )
		 | ( r->Data.Gpio.IoRestriction == ACPI_IO_RESTRICT_OUTPUT ? 2 : 0 )
		 | ( r->Data.Gpio.IoRestriction == ACPI_IO_RESTRICT_INPUT ? 4 : 0 );
	if ( r->Data.Gpio.ResourceSource.StringPtr != NULL ) {
		strncpy(q->ctrl, r->Data.Gpio.ResourceSource.StringPtr, q->max - 1);
		q->ctrl[q->max - 1] = '\0';
	}
	q->found = 1;
	return AE_CTRL_TERMINATE;
}

int knl_acpica_gpio( void *dev, int index, unsigned int *pin, char *ctrl, int max, int *flags )
{
	GPIORES	q;

	if ( dev == NULL || max <= 0 ) return -1;
	ctrl[0] = '\0';
	q.want = index;
	q.seen = 0;
	q.found = 0;
	q.pin = 0;
	q.flags = 0;
	q.ctrl = ctrl;
	q.max = max;
	(void)AcpiWalkResources(dev, METHOD_NAME__CRS, gpio_res, &q);
	if ( !q.found ) return -1;
	*pin = q.pin;
	if ( flags != NULL ) *flags = q.flags;
	return 0;
}

typedef struct {
	int			want, seen, found;
	unsigned long long	base, len;
} MEMRES;

static ACPI_STATUS mem_res( ACPI_RESOURCE *r, void *Context )
{
	MEMRES			*q = (MEMRES *)Context;
	unsigned long long	base, len;
	ACPI_RESOURCE_ADDRESS64	a;

	switch ( r->Type ) {
	case ACPI_RESOURCE_TYPE_MEMORY32:
		base = r->Data.Memory32.Minimum;
		len = r->Data.Memory32.AddressLength;
		break;
	case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		base = r->Data.FixedMemory32.Address;
		len = r->Data.FixedMemory32.AddressLength;
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS32:
	case ACPI_RESOURCE_TYPE_ADDRESS64:
		if ( ACPI_FAILURE(AcpiResourceToAddress64(r, &a)) || a.ResourceType != ACPI_MEMORY_RANGE ) return AE_OK;
		base = a.Address.Minimum;
		len = a.Address.AddressLength;
		break;
	default:
		return AE_OK;
	}
	if ( q->seen++ != q->want ) return AE_OK;
	q->base = base;
	q->len = len;
	q->found = 1;
	return AE_CTRL_TERMINATE;
}

int knl_acpica_mem( void *dev, int index, unsigned long long *base, unsigned long long *len )
{
	MEMRES	q = { index, 0, 0, 0, 0 };

	if ( dev == NULL ) return -1;
	(void)AcpiWalkResources(dev, METHOD_NAME__CRS, mem_res, &q);
	if ( !q.found ) return -1;
	*base = q.base;
	*len = q.len;
	return 0;
}

int knl_acpica_dep( void *dev, int index, char *path, int max )
{
	ACPI_BUFFER	b = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT	*o, *e;
	int		r = -1;

	if ( max > 0 ) path[0] = '\0';
	if ( dev == NULL || max <= 0
	  || ACPI_FAILURE(AcpiEvaluateObjectTyped(dev, "_DEP", NULL, &b, ACPI_TYPE_PACKAGE)) ) {
		return -1;
	}
	o = (ACPI_OBJECT *)b.Pointer;
	if ( index >= 0 && index < (int)o->Package.Count ) {
		e = &o->Package.Elements[index];
		if ( e->Type == ACPI_TYPE_LOCAL_REFERENCE && e->Reference.Handle != NULL ) {
			ACPI_BUFFER	nb = { (ACPI_SIZE)max, path };

			r = ACPI_SUCCESS(AcpiGetName(e->Reference.Handle, ACPI_FULL_PATHNAME, &nb)) ? 0 : -1;
		} else if ( e->Type == ACPI_TYPE_STRING ) {
			strncpy(path, e->String.Pointer, max - 1);
			path[max - 1] = '\0';
			r = 0;
		}
	}
	ACPI_FREE(b.Pointer);
	return r;
}

/* ---------------------------------------------------------------- the embedded controller */

/*
 * The EC of ACPI (12.2, 12.3): two ports, data and command/status. A
 * byte of its space is read with command 0x80 and the address, written
 * with 0x81, the address and the byte, each step waiting for the input
 * buffer to empty (IBF clear) before a byte goes in, or for the output
 * one to fill (OBF set) before one is taken out, no step longer than
 * EC_TMO_US. More than a byte at a time is done in burst mode (0x82,
 * answered with 0x90; 0x83 ends it). When the EC's device says so
 * (_GLK 1) the firmware's global lock is held around each access. A
 * transaction is had by one task at a time; it can take long, so a
 * semaphore guards it, not a spin lock.
 *
 * Its own events come as a general purpose event (_GPE, or the ECDT's):
 * while SCI_EVT is set, the query 0x84 gives the event's number, and
 * the method _Qxx of that number is run. Events left from before the
 * start are drained the same way once the handler is in.
 *
 * The ECDT, when the firmware has one, names the EC before the
 * namespace is initialized, so that methods run by _INI and _REG find
 * its space; without it, the EC of the DSDT (PNP0C09) is taken after.
 */
#define EC_OBF		0x01
#define EC_IBF		0x02
#define EC_BURST	0x10
#define EC_SCI_EVT	0x20
#define EC_TMO_US	500000
#define EC_POLL_US	50
#define EC_DRAIN_MAX	100

static ACPI_HANDLE	ec_dev = NULL;
static UINT16		ec_data = 0, ec_sc = 0;
static UINT32		ec_gpe = 0;
static int		ec_sem = 0;
static int		ec_glk = 0;
static int		ec_on = 0;

static UINT8 ec_in( UINT16 port )
{
	UINT32	v = 0;

	(void)AcpiOsReadPort(port, &v, 8);
	return (UINT8)v;
}

static void ec_out( UINT16 port, UINT8 v )
{
	(void)AcpiOsWritePort(port, v, 8);
}

static int ec_wait( UINT8 mask, UINT8 want )
{
	int	t;

	for ( t = 0; t < EC_TMO_US; t += EC_POLL_US ) {
		if ( ( ec_in(ec_sc) & mask ) == want ) return 0;
		knl_acpi_os_stall(EC_POLL_US);
	}
	return -1;
}

static int ec_transact( UINT8 cmd, int addr, int wval, UINT8 *rval )
{
	if ( ec_wait(EC_IBF, 0) ) return -1;
	ec_out(ec_sc, cmd);
	if ( addr >= 0 ) {
		if ( ec_wait(EC_IBF, 0) ) return -1;
		ec_out(ec_data, (UINT8)addr);
	}
	if ( wval >= 0 ) {
		if ( ec_wait(EC_IBF, 0) ) return -1;
		ec_out(ec_data, (UINT8)wval);
	}
	if ( rval != NULL ) {
		if ( ec_wait(EC_OBF, EC_OBF) ) return -1;
		*rval = ec_in(ec_data);
	}
	return 0;
}

static void ec_take( UINT32 *glk )
{
	(void)knl_acpi_os_sem_wait(ec_sem, 1, -1);
	*glk = 0;
	if ( ec_glk ) (void)AcpiAcquireGlobalLock(1000, glk);
}

static void ec_give( UINT32 glk )
{
	if ( ec_glk && glk != 0 ) (void)AcpiReleaseGlobalLock(glk);
	knl_acpi_os_sem_signal(ec_sem, 1);
}

static ACPI_STATUS ec_space( UINT32 Function, ACPI_PHYSICAL_ADDRESS Address, UINT32 BitWidth, UINT64 *Value,
			     void *HandlerContext, void *RegionContext )
{
	UINT32	i, n = BitWidth / 8, glk;
	int	r = 0, burst = 0;
	UINT8	ack = 0;

	(void)HandlerContext;
	(void)RegionContext;
	if ( Address > 0xff || BitWidth == 0 || BitWidth % 8 != 0 ) return AE_BAD_PARAMETER;
	ec_take(&glk);
	if ( n > 1 && ec_transact(0x82, -1, -1, &ack) == 0 && ack == 0x90 ) burst = 1;
	if ( Function == ACPI_READ ) *Value = 0;
	for ( i = 0; i < n && r == 0; i++ ) {
		if ( Function == ACPI_READ ) {
			UINT8	b = 0;

			r = ec_transact(0x80, (int)( Address + i ), -1, &b);
			*Value |= (UINT64)b << ( 8 * i );
		} else {
			r = ec_transact(0x81, (int)( Address + i ), (int)( ( *Value >> ( 8 * i ) ) & 0xff ), NULL);
		}
	}
	if ( burst ) (void)ec_transact(0x83, -1, -1, NULL);
	ec_give(glk);
	return r ? AE_TIME : AE_OK;
}

/* The events waiting, each one's _Qxx run; at most max of them */
static void ec_drain( int max )
{
	UINT8	q;
	UINT32	glk;
	char	m[8];
	int	n;

	for ( n = 0; n < max; n++ ) {
		q = 0;
		ec_take(&glk);
		if ( ( ec_in(ec_sc) & EC_SCI_EVT ) == 0 || ec_transact(0x84, -1, -1, &q) != 0 ) q = 0;
		ec_give(glk);
		if ( q == 0 ) break;
		if ( ec_dev != NULL ) {
			snprintf(m, sizeof(m), "_Q%02X", q);
			(void)AcpiEvaluateObject(ec_dev, m, NULL, NULL);
		}
	}
}

static void ec_query( void *Context )
{
	(void)Context;
	ec_drain(EC_DRAIN_MAX);
	(void)AcpiFinishGpe(NULL, ec_gpe);
}

static UINT32 ec_gpe_handler( ACPI_HANDLE GpeDevice, UINT32 GpeNumber, void *Context )
{
	(void)GpeDevice;
	(void)GpeNumber;
	(void)AcpiOsExecute(OSL_GPE_HANDLER, ec_query, Context);
	return 0;				/* finished by ec_query, once the events are taken */
}

static ACPI_STATUS ec_res( ACPI_RESOURCE *r, void *Context )
{
	int	*n = (int *)Context;
	UINT16	port = 0;

	if ( r->Type == ACPI_RESOURCE_TYPE_IO ) port = r->Data.Io.Minimum;
	if ( r->Type == ACPI_RESOURCE_TYPE_FIXED_IO ) port = r->Data.FixedIo.Address;
	if ( port != 0 ) {
		if ( *n == 0 ) ec_data = port; else if ( *n == 1 ) ec_sc = port;
		(*n)++;
	}
	return AE_OK;
}

static ACPI_STATUS ec_find( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	(void)Level;
	(void)Context;
	(void)ReturnValue;
	if ( ec_dev == NULL ) ec_dev = h;
	return AE_CTRL_TERMINATE;
}

/* The handler of its space on 'where', its GPE's handler, and what waits drained */
static int ec_start( ACPI_HANDLE where, const char *from )
{
	unsigned long long	g = 0;
	char			b[128];

	if ( ec_sem == 0 ) ec_sem = knl_acpi_os_sem_new(1, 1);
	if ( ec_sem == 0 ) return -1;
	if ( ACPI_FAILURE(AcpiInstallAddressSpaceHandler(where, ACPI_ADR_SPACE_EC, ec_space, NULL, NULL)) ) {
		knl_acpi_os_puts("acpi: the embedded controller's space not taken\n");
		return -1;
	}
	ec_on = 1;
	if ( ec_dev != NULL && knl_acpica_int(ec_dev, "_GLK", &g) == 0 ) ec_glk = ( g != 0 );
	if ( ec_dev != NULL && knl_acpica_int(ec_dev, "_GPE", &g) == 0 ) ec_gpe = (UINT32)g;
	if ( ACPI_SUCCESS(AcpiInstallGpeHandler(NULL, ec_gpe, ACPI_GPE_EDGE_TRIGGERED, ec_gpe_handler, NULL)) ) {
		(void)AcpiEnableGpe(NULL, ec_gpe);
	}
	snprintf(b, sizeof(b), "acpi: embedded controller (%s): data %x, command %x, GPE %u%s\n", from,
		 ec_data, ec_sc, ec_gpe, ec_glk ? ", global lock" : "");
	knl_acpi_os_puts(b);
	return 0;
}

/* The ECDT's EC, before the namespace's objects are initialized */
static int ec_early( void )
{
	ACPI_TABLE_HEADER	*t = NULL;
	ACPI_TABLE_ECDT		*e;

	if ( ACPI_FAILURE(AcpiGetTable(ACPI_SIG_ECDT, 1, &t)) || t == NULL ) return -1;
	e = (ACPI_TABLE_ECDT *)t;
	if ( e->Control.Address == 0 || e->Data.Address == 0 ) return -1;
	ec_sc = (UINT16)e->Control.Address;
	ec_data = (UINT16)e->Data.Address;
	ec_gpe = e->Gpe;
	(void)AcpiGetHandle(NULL, (char *)e->Id, &ec_dev);
	return ec_start(ACPI_ROOT_OBJECT, "ECDT");
}

/* Without the ECDT: the DSDT's EC (PNP0C09); with it, the events left from before the start */
static void ec_late( void )
{
	int	n = 0;

	if ( !ec_on ) {
		(void)AcpiGetDevices("PNP0C09", ec_find, NULL, NULL);
		if ( ec_dev == NULL ) return;
		(void)AcpiWalkResources(ec_dev, METHOD_NAME__CRS, ec_res, &n);
		if ( n < 2 ) {
			knl_acpi_os_puts("acpi: the embedded controller has no ports in its _CRS\n");
			return;
		}
		if ( ec_start(ec_dev, "DSDT") != 0 ) return;
	}
	ec_drain(EC_DRAIN_MAX);
}

int knl_acpica_ec( void )
{
	return ec_on ? 0 : -1;
}

/* ---------------------------------------------------------------- waking and the platform */

/* A device's _PRW: its GPE kept for waking the machine, so that it is not enabled as a runtime one */
static ACPI_STATUS prw( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	ACPI_BUFFER	b = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_OBJECT	*o, *e;

	(void)Level;
	(void)ReturnValue;
	if ( ACPI_FAILURE(AcpiEvaluateObjectTyped(h, "_PRW", NULL, &b, ACPI_TYPE_PACKAGE)) ) return AE_OK;
	o = (ACPI_OBJECT *)b.Pointer;
	if ( o->Package.Count >= 2 ) {
		e = &o->Package.Elements[0];
		if ( e->Type == ACPI_TYPE_INTEGER ) {
			(void)AcpiSetupGpeForWake(h, NULL, (UINT32)e->Integer.Value);
			(*(int *)Context)++;
		} else if ( e->Type == ACPI_TYPE_PACKAGE && e->Package.Count == 2
			 && e->Package.Elements[0].Type == ACPI_TYPE_LOCAL_REFERENCE
			 && e->Package.Elements[1].Type == ACPI_TYPE_INTEGER ) {
			(void)AcpiSetupGpeForWake(h, e->Package.Elements[0].Reference.Handle,
						  (UINT32)e->Package.Elements[1].Integer.Value);
			(*(int *)Context)++;
		}
	}
	ACPI_FREE(b.Pointer);
	return AE_OK;
}

static void wake_gpes( void )
{
	int	n = 0;
	char	b[64];

	(void)AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX, prw, NULL, &n, NULL);
	snprintf(b, sizeof(b), "acpi: %d devices wake the machine by a GPE (_PRW)\n", n);
	knl_acpi_os_puts(b);
}

/*
 * \_SB._OSC: the platform told what the OS takes charge of. Nothing is
 * asked for yet (the second word 0): the call itself tells a firmware
 * that waits for it that the OS is there and how far it goes.
 */
static void platform_osc( void )
{
	static const UINT8	uuid[16] = {	/* 0811B06E-4A27-44F9-8D60-3CBBC22E7B48 */
		0x6e, 0xb0, 0x11, 0x08, 0x27, 0x4a, 0xf9, 0x44, 0x8d, 0x60, 0x3c, 0xbb, 0xc2, 0x2e, 0x7b, 0x48
	};
	UINT32			caps[2] = { 0, 0 };
	ACPI_OBJECT		arg[4];
	ACPI_OBJECT_LIST	args = { 4, arg };
	ACPI_BUFFER		out = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_HANDLE		sb = NULL;

	if ( ACPI_FAILURE(AcpiGetHandle(NULL, "\\_SB_", &sb)) ) return;
	arg[0].Type = ACPI_TYPE_BUFFER;
	arg[0].Buffer.Length = 16;
	arg[0].Buffer.Pointer = (UINT8 *)uuid;
	arg[1].Type = ACPI_TYPE_INTEGER;
	arg[1].Integer.Value = 1;
	arg[2].Type = ACPI_TYPE_INTEGER;
	arg[2].Integer.Value = 2;
	arg[3].Type = ACPI_TYPE_BUFFER;
	arg[3].Buffer.Length = sizeof(caps);
	arg[3].Buffer.Pointer = (UINT8 *)caps;
	if ( ACPI_SUCCESS(AcpiEvaluateObject(sb, "_OSC", &args, &out)) ) {
		knl_acpi_os_puts("acpi: \\_SB._OSC told\n");
	}
	if ( out.Pointer != NULL ) ACPI_FREE(out.Pointer);
}

/* ---------------------------------------------------------------- the devices as text */

typedef struct {
	char	*p;
	int	len, max, ndev;
} TEXT;

static void put( TEXT *t, const char *f, ... )
{
	va_list	ap;
	int	n;

	if ( t->len >= t->max - 1 ) return;
	va_start(ap, f);
	n = vsnprintf(t->p + t->len, (ACPI_SIZE)( t->max - t->len ), f, ap);
	va_end(ap);
	if ( n > 0 ) t->len += ( n < t->max - t->len ) ? n : t->max - t->len - 1;
}

static ACPI_STATUS resource( ACPI_RESOURCE *r, void *Context )
{
	TEXT	*t = (TEXT *)Context;
	UINT32	i;

	switch ( r->Type ) {
	case ACPI_RESOURCE_TYPE_IRQ:
		put(t, "      IRQ");
		for ( i = 0; i < r->Data.Irq.InterruptCount; i++ ) put(t, " %u", r->Data.Irq.Interrupts[i]);
		put(t, " (%s, active %s)\n", r->Data.Irq.Triggering ? "edge" : "level", r->Data.Irq.Polarity ? "low" : "high");
		break;
	case ACPI_RESOURCE_TYPE_EXTENDED_IRQ:
		put(t, "      interrupt");
		for ( i = 0; i < r->Data.ExtendedIrq.InterruptCount; i++ ) put(t, " %u", r->Data.ExtendedIrq.Interrupts[i]);
		put(t, " (%s, active %s)\n", r->Data.ExtendedIrq.Triggering ? "edge" : "level",
		    r->Data.ExtendedIrq.Polarity ? "low" : "high");
		break;
	case ACPI_RESOURCE_TYPE_IO:
		put(t, "      I/O %x-%x\n", r->Data.Io.Minimum, r->Data.Io.Minimum + r->Data.Io.AddressLength - 1);
		break;
	case ACPI_RESOURCE_TYPE_FIXED_IO:
		put(t, "      I/O %x, %u ports\n", r->Data.FixedIo.Address, r->Data.FixedIo.AddressLength);
		break;
	case ACPI_RESOURCE_TYPE_MEMORY32:
		put(t, "      memory %x, %x bytes\n", r->Data.Memory32.Minimum, r->Data.Memory32.AddressLength);
		break;
	case ACPI_RESOURCE_TYPE_FIXED_MEMORY32:
		put(t, "      memory %x, %x bytes\n", r->Data.FixedMemory32.Address, r->Data.FixedMemory32.AddressLength);
		break;
	case ACPI_RESOURCE_TYPE_ADDRESS16:
	case ACPI_RESOURCE_TYPE_ADDRESS32:
	case ACPI_RESOURCE_TYPE_ADDRESS64: {
		ACPI_RESOURCE_ADDRESS64	a;

		if ( ACPI_SUCCESS(AcpiResourceToAddress64(r, &a)) ) {
			put(t, "      %s window %llx-%llx\n",
			    ( a.ResourceType == ACPI_MEMORY_RANGE ) ? "memory" : ( a.ResourceType == ACPI_IO_RANGE ) ? "I/O" : "bus",
			    (unsigned long long)a.Address.Minimum, (unsigned long long)a.Address.Maximum);
		}
		break;
	}
	case ACPI_RESOURCE_TYPE_GPIO:
		put(t, "      GPIO %s on %s pins", ( r->Data.Gpio.ConnectionType == ACPI_RESOURCE_GPIO_TYPE_INT ) ? "interrupt" : "I/O",
		    r->Data.Gpio.ResourceSource.StringPtr ? r->Data.Gpio.ResourceSource.StringPtr : "?");
		for ( i = 0; i < r->Data.Gpio.PinTableLength; i++ ) put(t, " %u", r->Data.Gpio.PinTable[i]);
		put(t, " (%s, %s)\n", r->Data.Gpio.Triggering ? "edge" : "level",
		    ( r->Data.Gpio.Polarity == ACPI_ACTIVE_LOW ) ? "active low" : ( r->Data.Gpio.Polarity == ACPI_ACTIVE_BOTH ) ? "both" : "active high");
		break;
	case ACPI_RESOURCE_TYPE_SERIAL_BUS: {
		ACPI_RESOURCE_COMMON_SERIALBUS	*c = &r->Data.CommonSerialBus;
		const char			*on = c->ResourceSource.StringPtr ? c->ResourceSource.StringPtr : "?";

		if ( c->Type == ACPI_RESOURCE_SERIAL_TYPE_I2C ) {
			put(t, "      I2C address %x on %s, %u Hz\n", r->Data.I2cSerialBus.SlaveAddress, on,
			    r->Data.I2cSerialBus.ConnectionSpeed);
		} else if ( c->Type == ACPI_RESOURCE_SERIAL_TYPE_SPI ) {
			put(t, "      SPI device %u on %s, %u Hz\n", r->Data.SpiSerialBus.DeviceSelection, on,
			    r->Data.SpiSerialBus.ConnectionSpeed);
		} else if ( c->Type == ACPI_RESOURCE_SERIAL_TYPE_UART ) {
			put(t, "      UART on %s, %u baud\n", on, r->Data.UartSerialBus.DefaultBaudRate);
		} else {
			put(t, "      serial bus type %u on %s\n", c->Type, on);
		}
		break;
	}
	case ACPI_RESOURCE_TYPE_END_TAG:
		break;
	default:
		put(t, "      resource type %u\n", r->Type);
		break;
	}
	return AE_OK;
}

static ACPI_STATUS device( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	TEXT			*t = (TEXT *)Context;
	ACPI_DEVICE_INFO	*info = NULL;
	char			path[160];
	ACPI_BUFFER		buf = { sizeof(path), path };
	UINT32			sta = 0xf;
	UINT32			i;

	(void)Level;
	(void)ReturnValue;
	if ( ACPI_FAILURE(AcpiGetName(h, ACPI_FULL_PATHNAME, &buf)) ) {
		return AE_OK;
	}
	t->ndev++;
	put(t, "%s", path);
	if ( ACPI_SUCCESS(AcpiGetObjectInfo(h, &info)) && info != NULL ) {
		if ( info->Valid & ACPI_VALID_HID ) put(t, " _HID %s", info->HardwareId.String);
		if ( info->Valid & ACPI_VALID_CID ) {
			put(t, " _CID");
			for ( i = 0; i < info->CompatibleIdList.Count; i++ ) put(t, " %s", info->CompatibleIdList.Ids[i].String);
		}
		if ( info->Valid & ACPI_VALID_UID ) put(t, " _UID %s", info->UniqueId.String);
		if ( info->Valid & ACPI_VALID_ADR ) put(t, " _ADR %llx", (unsigned long long)info->Address);
		ACPI_FREE(info);
	}
	if ( ACPI_SUCCESS(AcpiUtExecute_STA((ACPI_NAMESPACE_NODE *)h, &sta)) ) {
		put(t, " _STA %x", sta);		/* ffffffff when the device has none */
	}
	put(t, "%s\n", ( sta & 1 ) ? "" : " (not present)");
	if ( sta & 1 ) {
		(void)AcpiWalkResources(h, METHOD_NAME__CRS, resource, t);
	}
	return AE_OK;
}

int knl_acpica_report( char *buf, int max )
{
	TEXT	t;

	t.p = buf;
	t.len = 0;
	t.max = max;
	t.ndev = 0;
	if ( max <= 0 ) return 0;
	buf[0] = '\0';
	if ( !ready ) {
		put(&t, "the ACPI interpreter has not started\n");
		return t.len;
	}
	(void)AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX, device, NULL, &t, NULL);
	put(&t, "%d devices\n", t.ndev);
	return t.len;
}

/* ---------------------------------------------------------------- the lines of PCI functions */

/* The device below parent whose _ADR is adr, NULL when the namespace has none */
static ACPI_HANDLE child_at( ACPI_HANDLE parent, UINT64 adr )
{
	ACPI_HANDLE	prev = NULL, c = NULL;
	ACPI_OBJECT	o;
	ACPI_BUFFER	b;

	while ( ACPI_SUCCESS(AcpiGetNextObject(ACPI_TYPE_DEVICE, parent, prev, &c)) ) {
		b.Length = sizeof(o);
		b.Pointer = &o;
		if ( ACPI_SUCCESS(AcpiEvaluateObjectTyped(c, "_ADR", NULL, &b, ACPI_TYPE_INTEGER))
		  && o.Integer.Value == adr ) {
			return c;
		}
		prev = c;
	}
	return NULL;
}

typedef struct {
	unsigned int	bus;
	ACPI_HANDLE	h;
} ROOTQ;

static ACPI_STATUS root_of( ACPI_HANDLE h, UINT32 Level, void *Context, void **ReturnValue )
{
	ROOTQ			*q = (ROOTQ *)Context;
	unsigned long long	bbn = 0;

	(void)Level;
	(void)ReturnValue;
	(void)knl_acpica_int(h, "_BBN", &bbn);	/* none: the bus 0 */
	if ( (unsigned int)bbn == q->bus ) {
		q->h = h;
		return AE_CTRL_TERMINATE;
	}
	return AE_OK;
}

static ACPI_STATUS link_res( ACPI_RESOURCE *r, void *Context )
{
	int	*irq = (int *)Context;

	if ( *irq >= 0 ) return AE_OK;
	if ( r->Type == ACPI_RESOURCE_TYPE_IRQ && r->Data.Irq.InterruptCount > 0 ) {
		*irq = r->Data.Irq.Interrupts[0];
	} else if ( r->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ && r->Data.ExtendedIrq.InterruptCount > 0 ) {
		*irq = (int)r->Data.ExtendedIrq.Interrupts[0];
	}
	return AE_OK;
}

static void say( char *how, int max, const char *what, ACPI_HANDLE h )
{
	char		p[96];
	ACPI_BUFFER	b = { sizeof(p), p };
	int		n = (int)strlen(how);

	if ( h == NULL || ACPI_FAILURE(AcpiGetName(h, ACPI_FULL_PATHNAME, &b)) ) strcpy(p, "?");
	if ( n < max - 1 ) snprintf(how + n, max - n, "%s%s %s", ( n > 0 ) ? ", " : "", what, p);
}

/*
 * The entry of a _PRT for the slot and the pin (0 for INTA): the GSI it
 * names, or the one its link device is set to (the link's current
 * _CRS; a link not set up by the firmware is not set here). -1 when the
 * table has no such entry or none can be had.
 */
static int prt_gsi( ACPI_HANDLE router, unsigned int slot, unsigned int pin, char *how, int max )
{
	ACPI_BUFFER		b = { ACPI_ALLOCATE_BUFFER, NULL };
	ACPI_PCI_ROUTING_TABLE	*e;
	UINT8			*p;
	ACPI_HANDLE		lnk;
	int			gsi = -1;

	if ( ACPI_FAILURE(AcpiGetIrqRoutingTable(router, &b)) || b.Pointer == NULL ) {
		return -1;
	}
	for ( p = (UINT8 *)b.Pointer; ; p += e->Length ) {
		e = (ACPI_PCI_ROUTING_TABLE *)p;
		if ( e->Length == 0 ) break;
		if ( ( ( e->Address >> 16 ) & 0xffff ) != slot || e->Pin != pin ) continue;
		say(how, max, "_PRT of", router);
		if ( e->Source[0] == '\0' ) {
			gsi = (int)e->SourceIndex;
		} else if ( ACPI_SUCCESS(AcpiGetHandle(router, e->Source, &lnk)) ) {
			(void)AcpiWalkResources(lnk, METHOD_NAME__CRS, link_res, &gsi);
			say(how, max, "link", lnk);
			if ( gsi == 0 ) gsi = -1;	/* the link is off */
		}
		break;
	}
	ACPI_FREE(b.Pointer);
	return gsi;
}

/*
 * The GSI the line of a PCI function reaches, as the _PRT tables say
 * (ACPI 6.5 6.2.13). The function is given by the path to it from the
 * root bus: dev[0], fn[0] on the root bus, each next one behind the
 * bridge before it, the function last; pin is its interrupt pin (1 for
 * INTA). Each bridge that has a _PRT names the lines of the bus behind
 * it; behind one that has none, the pin is turned by the slot, as the
 * PCI-to-PCI bridge standard wires them, and looked for a bus higher.
 * -1 when no table names it; how says which tables were used.
 */
int knl_acpica_pci_gsi( unsigned int root_bus, const unsigned char *dev, const unsigned char *fn, int n,
			unsigned int pin, char *how, int max )
{
	ACPI_HANDLE	node[8];
	ROOTQ		q = { root_bus, NULL };
	unsigned int	p;
	int		i, k, gsi;

	if ( max > 0 ) how[0] = '\0';
	if ( !ready || n < 1 || n > 8 || pin < 1 || pin > 4 ) return -1;
	(void)AcpiGetDevices("PNP0A08", root_of, &q, NULL);
	if ( q.h == NULL ) (void)AcpiGetDevices("PNP0A03", root_of, &q, NULL);
	if ( q.h == NULL ) {
		if ( max > 0 ) snprintf(how, max, "no root bridge of bus %u", root_bus);
		return -1;
	}
	node[0] = q.h;
	for ( i = 1; i < n; i++ ) {
		node[i] = ( node[i - 1] != NULL ) ? child_at(node[i - 1], ( (UINT64)dev[i - 1] << 16 ) | fn[i - 1]) : NULL;
	}
	p = pin - 1;
	for ( k = n - 1; k >= 0; k-- ) {
		if ( node[k] != NULL && ( gsi = prt_gsi(node[k], dev[k], p, how, max) ) >= 0 ) {
			return gsi;
		}
		p = ( p + dev[k] ) % 4;		/* the pin the bridge above raises for it */
	}
	if ( max > 0 && how[0] == '\0' ) snprintf(how, max, "no _PRT entry");
	return -1;
}
