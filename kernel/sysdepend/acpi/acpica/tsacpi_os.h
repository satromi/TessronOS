/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	tsacpi_os.h
 *	Between the ACPI interpreter (ACPICA, built apart by
 *	tools/x64/acpica/build.sh with the files of this directory) and the
 *	kernel: the services of the kernel the interpreter's OS layer
 *	(osl.c) is built on, made by acpi_os.c in the kernel, and what the
 *	kernel calls of the interpreter (acpica.c). Only plain C types are
 *	used, so that neither side needs the other's headers.
 */

#ifndef _SYSDEPEND_ACPI_TSACPI_OS_
#define _SYSDEPEND_ACPI_TSACPI_OS_

/* ---------------------------------------------------------------- the kernel's, for the OS layer */

/* The RSDP the loader found */
extern unsigned long long	knl_acpi_os_rsdp( void );
/* Physical memory: RAM through the linear map, anything else through the device window (uncached) */
extern void	*knl_acpi_os_map( unsigned long long pa, unsigned long long len );
/* A PCI function's configuration space (ECAM), NULL when there is none */
extern void	*knl_acpi_os_pci( unsigned int seg, unsigned int bus, unsigned int dev, unsigned int fn );
/* The SCI: an interrupt (an ISA number or a GSI), level triggered, served by fn(ctx); 0 when done */
extern int	knl_acpi_os_irq( unsigned int irq, void (*fn)( void *ctx ), void *ctx );
extern void	knl_acpi_os_irq_remove( unsigned int irq );
/* Time in units of 100 ns; a wait without sleeping; a sleep */
extern unsigned long long	knl_acpi_os_timer( void );
extern void	knl_acpi_os_stall( unsigned int usec );
extern void	knl_acpi_os_sleep( unsigned int msec );
/* Memory of the kernel's */
extern void	*knl_acpi_os_alloc( unsigned long long size );
extern void	knl_acpi_os_free( void *p );
/* Semaphores (an ID, 0 for none); a wait of tmo ms (-1 for ever, 0 a poll): 0 when had */
extern int	knl_acpi_os_sem_new( unsigned int max, unsigned int init );
extern void	knl_acpi_os_sem_del( int sem );
extern int	knl_acpi_os_sem_wait( int sem, unsigned int units, int tmo );
extern void	knl_acpi_os_sem_signal( int sem, unsigned int units );
/* The task running (non-zero), whether waiting is allowed here, a task started on fn(ctx) */
extern unsigned int	knl_acpi_os_tid( void );
extern int	knl_acpi_os_can_wait( void );
extern int	knl_acpi_os_task( void (*fn)( void *ctx ), void *ctx );
/* Spin locks taken with the interrupts off, and given back with them as they were */
extern void	*knl_acpi_os_lock_new( void );
extern void	knl_acpi_os_lock_del( void *lock );
extern unsigned long long	knl_acpi_os_lock( void *lock );
extern void	knl_acpi_os_unlock( void *lock, unsigned long long was );
/* Text to the console */
extern void	knl_acpi_os_puts( const char *s );
/* The power button pressed: the session ended as the desktop's 〈電源を切る〉 ends it */
extern void	knl_acpi_os_power_button( void );
/* A notification from the firmware for a device (acpi_power.c takes the battery's, the adapter's, the lid's) */
extern void	knl_acpi_os_notify( void *dev, unsigned int value );

/* ---------------------------------------------------------------- the interpreter's, for the kernel */

/* The namespace loaded, the SCI and the events on: 0 when done */
extern int	knl_acpica_init( void );
extern int	knl_acpica_ready( void );
/*
 * Off (acpica_off.c): \_TTS(state) while the system still runs (1 run, 0
 * none, -1 failed); what the last step will do, said for the records;
 * the last step itself, with every other processor stopped and the
 * interrupts off (\_PTS(5), the GPEs disabled, SLP_TYP and SLP_EN). The
 * wake status the firmware left, said as the machine starts (called by
 * knl_acpica_init before ACPI mode clears it).
 */
extern int	knl_acpica_tts( int state );
extern void	knl_acpica_off_plan( void );
extern void	knl_acpica_wake_found( void );
/* Off (S5) and a reset (the FADT's reset register): they return only on failure */
extern int	knl_acpica_poweroff( void );
extern int	knl_acpica_reset( void );
/* Whether a device present (_STA) has the hardware or compatible ID */
extern int	knl_acpica_has_hid( const char *hid );
/* The index-th present device with the hardware or compatible ID, NULL when there is none */
extern void	*knl_acpica_device( const char *hid, int index );
/* Its full path ("\_SB_.BAT0") */
extern int	knl_acpica_path( void *dev, char *buf, int max );
/* An integer a method of it gives (_PSR, _LID, _STA): 0 when had */
extern int	knl_acpica_int( void *dev, const char *method, unsigned long long *v );
/*
 * A package a method gives (_BIF, _BIX, _BST): its elements, integers
 * into v and strings into s (32 bytes each, "" for an integer), at most
 * max; the count, or -1 when the method failed
 */
extern int	knl_acpica_pkg( void *dev, const char *method, unsigned long long *v, char (*s)[32], int max );
/*
 * A device on an I2C bus (HID over I2C): its address, the bus's speed
 * and the path of its controller, from its _CRS; its interrupt from a
 * GpioInt there (the pin and the GPIO controller's path, -1 none), or
 * from an Interrupt there, an input of the IOAPICs (irq, -1 none; its
 * mode irq_mode: 1 edge, 2 active low). 0 when it has an I2C connection.
 */
extern int	knl_acpica_i2c( void *dev, unsigned int *addr, unsigned int *hz, char *ctrl, int max,
				int *gpio_pin, char *gpio_ctrl, int gmax, int *irq, int *irq_mode );
/* A method of a device run with no arguments (_PS0): 0 when run, -1 when it has none, -2 when it failed */
extern int	knl_acpica_call( void *dev, const char *method );
/* The device of a path, NULL when there is none */
extern void	*knl_acpica_handle( const char *path );
/* An integer a _DSM gives for the UUID (16 bytes, as ACPI lays it), revision and function: 0 when had */
extern int	knl_acpica_dsm_int( void *dev, const unsigned char *uuid, unsigned int rev, unsigned int func,
				    unsigned long long *v );
/* A buffer a method gives (a camera sensor's SSDB, its power controller's CLDB): its length, -1 when it failed */
extern int	knl_acpica_buf( void *dev, const char *method, unsigned char *buf, int max );
/*
 * The index-th GpioIo of a device's _CRS (GpioInt left out): its first
 * pin, its controller's path, flags bit 0 active low, bit 1 output
 * only, bit 2 input only. 0 when there is one.
 */
extern int	knl_acpica_gpio( void *dev, int index, unsigned int *pin, char *ctrl, int max, int *flags );
/* The index-th memory range of a device's _CRS: 0 when there is one */
extern int	knl_acpica_mem( void *dev, int index, unsigned long long *base, unsigned long long *len );
/* The path of the index-th device its _DEP names: 0 when there is one */
extern int	knl_acpica_dep( void *dev, int index, char *path, int max );
/* The embedded controller's handler in place: 0 when there is one and it is served */
extern int	knl_acpica_ec( void );
/* The devices of the namespace as text (paths, IDs, status, resources): its length */
extern int	knl_acpica_report( char *buf, int max );
/*
 * The GSI the interrupt line (pin 1 for INTA) of a PCI function reaches,
 * from the _PRT tables: the function by its path from the root bus
 * (dev[0], fn[0] on it, the function last, n of them). -1 when none
 * names it; how says which tables were used.
 */
extern int	knl_acpica_pci_gsi( unsigned int root_bus, const unsigned char *dev, const unsigned char *fn, int n,
				    unsigned int pin, char *how, int max );

#endif /* _SYSDEPEND_ACPI_TSACPI_OS_ */
