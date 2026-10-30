/*
 *----------------------------------------------------------------------
 *    TessronOS
 *
 *    Copyright (C) 2026 satromi
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 */

/*
 *	osl.c
 *	The OS layer of the ACPI interpreter (the AcpiOs functions of
 *	ACPICA's acpiosxf.h) on the TessronOS kernel's services
 *	(tsacpi_os.h, made by kernel/sysdepend/acpi/acpi_os.c). Built with
 *	the interpreter by tools/x64/acpica/build.sh.
 *
 *	What the interpreter hands to be done later (AcpiOsExecute: the
 *	handlers of notifications, of general purpose events and the rest)
 *	is run by two tasks of its own, one for the notifications and one
 *	for everything else, so that a notification handler that waits for
 *	an event cannot keep that event from being served.
 */

#include "acpi.h"
#include "accommon.h"
#include "tsacpi_os.h"

#define _COMPONENT	ACPI_OS_SERVICES
ACPI_MODULE_NAME("osl")

/* ---------------------------------------------------------------- start, tables */

typedef struct {
	ACPI_OSD_EXEC_CALLBACK	fn;
	void			*ctx;
} JOB;

#define NJOB		128

typedef struct {
	JOB			job[NJOB];
	volatile UINT32		head, tail;
	void			*lock;
	int			sem;
} QUEUE;

static QUEUE		queue[2];		/* 0: notifications, 1: the rest */
static volatile int	pending = 0;
static void		*pending_lock;

static void worker( void *ctx )
{
	QUEUE		*q = &queue[(ACPI_SIZE)ctx];
	JOB		j;
	unsigned long long	f;

	for ( ;; ) {
		if ( knl_acpi_os_sem_wait(q->sem, 1, -1) != 0 ) {
			continue;
		}
		f = knl_acpi_os_lock(q->lock);
		j = q->job[q->tail % NJOB];
		q->tail++;
		knl_acpi_os_unlock(q->lock, f);
		(*j.fn)(j.ctx);
		f = knl_acpi_os_lock(pending_lock);
		pending--;
		knl_acpi_os_unlock(pending_lock, f);
	}
}

ACPI_STATUS AcpiOsInitialize( void )
{
	int	k;

	if ( pending_lock != NULL ) {
		return AE_OK;
	}
	pending_lock = knl_acpi_os_lock_new();
	for ( k = 0; k < 2; k++ ) {
		queue[k].lock = knl_acpi_os_lock_new();
		queue[k].sem = knl_acpi_os_sem_new(NJOB, 0);
		if ( queue[k].lock == NULL || queue[k].sem == 0
		  || knl_acpi_os_task(worker, (void *)(ACPI_SIZE)k) != 0 ) {
			return AE_NO_MEMORY;
		}
	}
	return ( pending_lock != NULL ) ? AE_OK : AE_NO_MEMORY;
}

ACPI_STATUS AcpiOsTerminate( void )
{
	return AE_OK;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer( void )
{
	return (ACPI_PHYSICAL_ADDRESS)knl_acpi_os_rsdp();
}

ACPI_STATUS AcpiOsPredefinedOverride( const ACPI_PREDEFINED_NAMES *InitVal, ACPI_STRING *NewVal )
{
	(void)InitVal;
	*NewVal = NULL;
	return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride( ACPI_TABLE_HEADER *ExistingTable, ACPI_TABLE_HEADER **NewTable )
{
	(void)ExistingTable;
	*NewTable = NULL;
	return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride( ACPI_TABLE_HEADER *ExistingTable, ACPI_PHYSICAL_ADDRESS *NewAddress,
					  UINT32 *NewTableLength )
{
	(void)ExistingTable;
	*NewAddress = 0;
	*NewTableLength = 0;
	return AE_OK;
}

/* ---------------------------------------------------------------- locks and semaphores */

ACPI_STATUS AcpiOsCreateLock( ACPI_SPINLOCK *OutHandle )
{
	*OutHandle = knl_acpi_os_lock_new();
	return ( *OutHandle != NULL ) ? AE_OK : AE_NO_MEMORY;
}

void AcpiOsDeleteLock( ACPI_SPINLOCK Handle )
{
	knl_acpi_os_lock_del(Handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock( ACPI_SPINLOCK Handle )
{
	return (ACPI_CPU_FLAGS)knl_acpi_os_lock(Handle);
}

void AcpiOsReleaseLock( ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags )
{
	knl_acpi_os_unlock(Handle, (unsigned long long)Flags);
}

ACPI_STATUS AcpiOsCreateSemaphore( UINT32 MaxUnits, UINT32 InitialUnits, ACPI_SEMAPHORE *OutHandle )
{
	int	s = knl_acpi_os_sem_new(MaxUnits, InitialUnits);

	if ( s == 0 ) {
		return AE_NO_MEMORY;
	}
	*OutHandle = (ACPI_SEMAPHORE)(ACPI_SIZE)s;
	return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore( ACPI_SEMAPHORE Handle )
{
	knl_acpi_os_sem_del((int)(ACPI_SIZE)Handle);
	return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore( ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout )
{
	int	tmo = ( Timeout == ACPI_WAIT_FOREVER ) ? -1 : (int)Timeout;

	return ( knl_acpi_os_sem_wait((int)(ACPI_SIZE)Handle, Units, tmo) == 0 ) ? AE_OK : AE_TIME;
}

ACPI_STATUS AcpiOsSignalSemaphore( ACPI_SEMAPHORE Handle, UINT32 Units )
{
	knl_acpi_os_sem_signal((int)(ACPI_SIZE)Handle, Units);
	return AE_OK;
}

/* The FACS's global lock: bit 0 pending, bit 1 owned (ACPI 5.2.10.1) */
int AcpiOsAcquireGlobalLock( unsigned int *lock )
{
	unsigned int	old, new;

	do {
		old = *(volatile unsigned int *)lock;
		new = ( ( old & ~1u ) | 2u ) + ( ( old >> 1 ) & 1u );
	} while ( !__sync_bool_compare_and_swap(lock, old, new) );
	return ( new < 3 ) ? -1 : 0;		/* -1: owned now; 0: pending, to be signalled */
}

int AcpiOsReleaseGlobalLock( unsigned int *lock )
{
	unsigned int	old, new;

	do {
		old = *(volatile unsigned int *)lock;
		new = old & ~3u;
	} while ( !__sync_bool_compare_and_swap(lock, old, new) );
	return (int)( old & 1u );		/* someone is waiting: the firmware is to be told */
}

/* ---------------------------------------------------------------- memory */

void *AcpiOsAllocate( ACPI_SIZE Size )
{
	return knl_acpi_os_alloc(Size);
}

void AcpiOsFree( void *Memory )
{
	knl_acpi_os_free(Memory);
}

void *AcpiOsMapMemory( ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length )
{
	return knl_acpi_os_map(Where, Length);
}

void AcpiOsUnmapMemory( void *LogicalAddress, ACPI_SIZE Size )
{
	(void)LogicalAddress;
	(void)Size;
}

ACPI_STATUS AcpiOsGetPhysicalAddress( void *LogicalAddress, ACPI_PHYSICAL_ADDRESS *PhysicalAddress )
{
	(void)LogicalAddress;
	(void)PhysicalAddress;
	return AE_SUPPORT;
}

BOOLEAN AcpiOsReadable( void *Pointer, ACPI_SIZE Length )
{
	(void)Pointer;
	(void)Length;
	return TRUE;
}

BOOLEAN AcpiOsWritable( void *Pointer, ACPI_SIZE Length )
{
	(void)Pointer;
	(void)Length;
	return TRUE;
}

/* ---------------------------------------------------------------- the SCI */

static ACPI_OSD_HANDLER	sci_routine = NULL;
static void		*sci_context;

static void sci( void *ctx )
{
	(void)ctx;
	if ( sci_routine != NULL ) {
		(void)(*sci_routine)(sci_context);
	}
}

ACPI_STATUS AcpiOsInstallInterruptHandler( UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine, void *Context )
{
	sci_routine = ServiceRoutine;
	sci_context = Context;
	if ( knl_acpi_os_irq(InterruptNumber, sci, NULL) != 0 ) {
		sci_routine = NULL;
		return AE_ALREADY_EXISTS;
	}
	return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler( UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine )
{
	(void)ServiceRoutine;
	knl_acpi_os_irq_remove(InterruptNumber);
	sci_routine = NULL;
	return AE_OK;
}

/* ---------------------------------------------------------------- tasks, time */

ACPI_THREAD_ID AcpiOsGetThreadId( void )
{
	return (ACPI_THREAD_ID)knl_acpi_os_tid();
}

ACPI_STATUS AcpiOsExecute( ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function, void *Context )
{
	QUEUE			*q = &queue[( Type == OSL_NOTIFY_HANDLER ) ? 0 : 1];
	unsigned long long	f;

	if ( q->sem == 0 ) {
		return AE_NOT_EXIST;
	}
	f = knl_acpi_os_lock(q->lock);
	if ( q->head - q->tail >= NJOB ) {
		knl_acpi_os_unlock(q->lock, f);
		return AE_NO_MEMORY;
	}
	q->job[q->head % NJOB].fn = Function;
	q->job[q->head % NJOB].ctx = Context;
	q->head++;
	knl_acpi_os_unlock(q->lock, f);
	f = knl_acpi_os_lock(pending_lock);
	pending++;
	knl_acpi_os_unlock(pending_lock, f);
	knl_acpi_os_sem_signal(q->sem, 1);
	return AE_OK;
}

void AcpiOsWaitEventsComplete( void )
{
	int	n;

	for ( n = 0; pending > 0 && n < 1000; n++ ) {
		knl_acpi_os_sleep(10);
	}
}

void AcpiOsSleep( UINT64 Milliseconds )
{
	knl_acpi_os_sleep((unsigned int)Milliseconds);
}

void AcpiOsStall( UINT32 Microseconds )
{
	knl_acpi_os_stall(Microseconds);
}

UINT64 AcpiOsGetTimer( void )
{
	return knl_acpi_os_timer();
}

/* ---------------------------------------------------------------- ports, memory, PCI */

ACPI_STATUS AcpiOsReadPort( ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width )
{
	UINT16	p = (UINT16)Address;

	switch ( Width ) {
	case 8: {
		UINT8	v;

		__asm__ volatile ( "inb %1, %0" : "=a"(v) : "Nd"(p) );
		*Value = v;
		break;
	}
	case 16: {
		UINT16	v;

		__asm__ volatile ( "inw %1, %0" : "=a"(v) : "Nd"(p) );
		*Value = v;
		break;
	}
	case 32: {
		UINT32	v;

		__asm__ volatile ( "inl %1, %0" : "=a"(v) : "Nd"(p) );
		*Value = v;
		break;
	}
	default:
		return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

ACPI_STATUS AcpiOsWritePort( ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width )
{
	UINT16	p = (UINT16)Address;

	switch ( Width ) {
	case 8:
		__asm__ volatile ( "outb %0, %1" :: "a"((UINT8)Value), "Nd"(p) );
		break;
	case 16:
		__asm__ volatile ( "outw %0, %1" :: "a"((UINT16)Value), "Nd"(p) );
		break;
	case 32:
		__asm__ volatile ( "outl %0, %1" :: "a"(Value), "Nd"(p) );
		break;
	default:
		return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

static ACPI_STATUS rd( volatile void *a, UINT64 *Value, UINT32 Width )
{
	switch ( Width ) {
	case 8:		*Value = *(volatile UINT8 *)a;	break;
	case 16:	*Value = *(volatile UINT16 *)a;	break;
	case 32:	*Value = *(volatile UINT32 *)a;	break;
	case 64:	*Value = *(volatile UINT64 *)a;	break;
	default:	return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

static ACPI_STATUS wr( volatile void *a, UINT64 Value, UINT32 Width )
{
	switch ( Width ) {
	case 8:		*(volatile UINT8 *)a = (UINT8)Value;	break;
	case 16:	*(volatile UINT16 *)a = (UINT16)Value;	break;
	case 32:	*(volatile UINT32 *)a = (UINT32)Value;	break;
	case 64:	*(volatile UINT64 *)a = Value;		break;
	default:	return AE_BAD_PARAMETER;
	}
	return AE_OK;
}

ACPI_STATUS AcpiOsReadMemory( ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width )
{
	return rd(knl_acpi_os_map(Address, Width / 8), Value, Width);
}

ACPI_STATUS AcpiOsWriteMemory( ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width )
{
	return wr(knl_acpi_os_map(Address, Width / 8), Value, Width);
}

ACPI_STATUS AcpiOsReadPciConfiguration( ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 *Value, UINT32 Width )
{
	UINT8	*c = knl_acpi_os_pci(PciId->Segment, PciId->Bus, PciId->Device, PciId->Function);

	if ( c == NULL || Reg >= 4096 ) {
		*Value = ~(UINT64)0;
		return AE_NOT_EXIST;
	}
	return rd(c + Reg, Value, Width);
}

ACPI_STATUS AcpiOsWritePciConfiguration( ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 Value, UINT32 Width )
{
	UINT8	*c = knl_acpi_os_pci(PciId->Segment, PciId->Bus, PciId->Device, PciId->Function);

	if ( c == NULL || Reg >= 4096 ) {
		return AE_NOT_EXIST;
	}
	return wr(c + Reg, Value, Width);
}

/* ---------------------------------------------------------------- the rest */

ACPI_STATUS AcpiOsSignal( UINT32 Function, void *Info )
{
	if ( Function == ACPI_SIGNAL_FATAL ) {
		ACPI_SIGNAL_FATAL_INFO	*f = (ACPI_SIGNAL_FATAL_INFO *)Info;
		char			b[96];

		snprintf(b, sizeof(b), "acpi: the firmware's Fatal: type %x code %x argument %x\n",
			 f->Type, f->Code, f->Argument);
		knl_acpi_os_puts(b);
	}
	return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep( UINT8 SleepState, UINT32 RegaValue, UINT32 RegbValue )
{
	(void)SleepState;
	(void)RegaValue;
	(void)RegbValue;
	return AE_OK;
}

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf( const char *Format, ... )
{
	va_list	args;

	va_start(args, Format);
	AcpiOsVprintf(Format, args);
	va_end(args);
}

void AcpiOsVprintf( const char *Format, va_list Args )
{
	char	b[256];

	vsnprintf(b, sizeof(b), Format, Args);
	knl_acpi_os_puts(b);
}

void AcpiOsRedirectOutput( void *Destination )
{
	(void)Destination;
}
