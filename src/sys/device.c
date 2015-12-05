/**
 * @file sys/device.c
 *
 * @copyright 2015 Bill Zissimopoulos
 */

#include <sys/driver.h>

NTSTATUS FspDeviceCreateSecure(UINT32 Kind, ULONG ExtraSize,
    PUNICODE_STRING DeviceName, DEVICE_TYPE DeviceType,
    PUNICODE_STRING DeviceSddl, LPCGUID DeviceClassGuid,
    PDEVICE_OBJECT *PDeviceObject);
NTSTATUS FspDeviceCreate(UINT32 Kind, ULONG ExtraSize,
    DEVICE_TYPE DeviceType,
    PDEVICE_OBJECT *PDeviceObject);
VOID FspDeviceDelete(PDEVICE_OBJECT DeviceObject);
static NTSTATUS FspFsctlDeviceInit(PDEVICE_OBJECT DeviceObject);
static VOID FspFsctlDeviceFini(PDEVICE_OBJECT DeviceObject);
static NTSTATUS FspFsvrtDeviceInit(PDEVICE_OBJECT DeviceObject);
static VOID FspFsvrtDeviceFini(PDEVICE_OBJECT DeviceObject);
static NTSTATUS FspFsvolDeviceInit(PDEVICE_OBJECT DeviceObject);
static VOID FspFsvolDeviceFini(PDEVICE_OBJECT DeviceObject);
BOOLEAN FspDeviceRetain(PDEVICE_OBJECT DeviceObject);
VOID FspDeviceRelease(PDEVICE_OBJECT DeviceObject);
VOID FspFsctlDeviceVolumeCreated(PDEVICE_OBJECT DeviceObject);
VOID FspFsctlDeviceVolumeDeleted(PDEVICE_OBJECT DeviceObject);
NTSTATUS FspFsvolDeviceInsertContext(PDEVICE_OBJECT DeviceObject, UINT64 Identifier, PVOID Context,
    PBOOLEAN PInserted);
VOID FspFsvolDeviceDeleteContext(PDEVICE_OBJECT DeviceObject, UINT64 Identifier,
    PBOOLEAN PDeleted);
static RTL_AVL_COMPARE_ROUTINE FspFsvolDeviceCompareElement;
static RTL_AVL_ALLOCATE_ROUTINE FspFsvolDeviceAllocateElement;
static RTL_AVL_FREE_ROUTINE FspFsvolDeviceFreeElement;
NTSTATUS FspDeviceCopyList(
    PDEVICE_OBJECT **PDeviceObjects, PULONG PDeviceObjectCount);
VOID FspDeviceDeleteList(
    PDEVICE_OBJECT *DeviceObjects, ULONG DeviceObjectCount);
VOID FspDeviceDeleteAll(VOID);

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FspDeviceCreateSecure)
#pragma alloc_text(PAGE, FspDeviceCreate)
#pragma alloc_text(PAGE, FspDeviceDelete)
#pragma alloc_text(PAGE, FspFsctlDeviceInit)
#pragma alloc_text(PAGE, FspFsctlDeviceFini)
#pragma alloc_text(PAGE, FspFsvrtDeviceInit)
#pragma alloc_text(PAGE, FspFsvrtDeviceFini)
#pragma alloc_text(PAGE, FspFsvolDeviceInit)
//! #pragma alloc_text(PAGE, FspFsvolDeviceFini)
#pragma alloc_text(PAGE, FspFsctlDeviceVolumeCreated)
#pragma alloc_text(PAGE, FspFsctlDeviceVolumeDeleted)
#pragma alloc_text(PAGE, FspDeviceCopyList)
#pragma alloc_text(PAGE, FspDeviceDeleteList)
#pragma alloc_text(PAGE, FspDeviceDeleteAll)
#endif

typedef struct
{
    UINT64 Identifier;
    PVOID Context;
} FSP_DEVICE_GENERIC_TABLE_ELEMENT;

NTSTATUS FspDeviceCreateSecure(UINT32 Kind, ULONG ExtraSize,
    PUNICODE_STRING DeviceName, DEVICE_TYPE DeviceType,
    PUNICODE_STRING DeviceSddl, LPCGUID DeviceClassGuid,
    PDEVICE_OBJECT *PDeviceObject)
{
    PAGED_CODE();

    NTSTATUS Result;
    ULONG DeviceExtensionSize;
    PDEVICE_OBJECT DeviceObject;
    FSP_DEVICE_EXTENSION *DeviceExtension;

    switch (Kind)
    {
    case FspFsvolDeviceExtensionKind:
        DeviceExtensionSize = sizeof(FSP_FSVOL_DEVICE_EXTENSION);
        break;
    case FspFsvrtDeviceExtensionKind:
        DeviceExtensionSize = sizeof(FSP_FSVRT_DEVICE_EXTENSION);
        break;
    case FspFsctlDeviceExtensionKind:
        DeviceExtensionSize = sizeof(FSP_FSCTL_DEVICE_EXTENSION);
        break;
    default:
        ASSERT(0);
        return STATUS_INVALID_PARAMETER;
    }

    if (0 != DeviceSddl)
        Result = IoCreateDeviceSecure(FspDriverObject,
            DeviceExtensionSize + ExtraSize, DeviceName, DeviceType,
            FILE_DEVICE_SECURE_OPEN, FALSE,
            DeviceSddl, DeviceClassGuid,
            &DeviceObject);
    else
        Result = IoCreateDevice(FspDriverObject,
            DeviceExtensionSize + ExtraSize, DeviceName, DeviceType,
            0, FALSE,
            &DeviceObject);
    if (!NT_SUCCESS(Result))
        return Result;

    DeviceExtension = FspDeviceExtension(DeviceObject);
    KeInitializeSpinLock(&DeviceExtension->SpinLock);
    DeviceExtension->RefCount = 1;
    ExInitializeResourceLite(&DeviceExtension->Resource);
    DeviceExtension->Kind = Kind;

    switch (Kind)
    {
    case FspFsvolDeviceExtensionKind:
        Result = FspFsvolDeviceInit(DeviceObject);
        break;
    case FspFsvrtDeviceExtensionKind:
        Result = FspFsvrtDeviceInit(DeviceObject);
        break;
    case FspFsctlDeviceExtensionKind:
        Result = FspFsctlDeviceInit(DeviceObject);
        break;
    }

    if (!NT_SUCCESS(Result))
    {
        ExDeleteResourceLite(&DeviceExtension->Resource);
        IoDeleteDevice(DeviceObject);
    }
    else
        *PDeviceObject = DeviceObject;

    return Result;
}

NTSTATUS FspDeviceCreate(UINT32 Kind, ULONG ExtraSize,
    DEVICE_TYPE DeviceType,
    PDEVICE_OBJECT *PDeviceObject)
{
    PAGED_CODE();

    return FspDeviceCreateSecure(Kind, ExtraSize, 0, DeviceType, 0, 0, PDeviceObject);
}

VOID FspDeviceDelete(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    FSP_DEVICE_EXTENSION *DeviceExtension = FspDeviceExtension(DeviceObject);

    switch (DeviceExtension->Kind)
    {
    case FspFsvolDeviceExtensionKind:
        FspFsvolDeviceFini(DeviceObject);
        break;
    case FspFsvrtDeviceExtensionKind:
        FspFsvrtDeviceFini(DeviceObject);
        break;
    case FspFsctlDeviceExtensionKind:
        FspFsctlDeviceFini(DeviceObject);
        break;
    default:
        ASSERT(0);
        return;
    }

    ExDeleteResourceLite(&DeviceExtension->Resource);

    IoDeleteDevice(DeviceObject);
}

static NTSTATUS FspFsctlDeviceInit(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    return STATUS_SUCCESS;
}

static VOID FspFsctlDeviceFini(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();
}

static NTSTATUS FspFsvrtDeviceInit(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    FSP_FSVRT_DEVICE_EXTENSION *FsvrtDeviceExtension = FspFsvrtDeviceExtension(DeviceObject);

    FspIoqInitialize(&FsvrtDeviceExtension->Ioq);

    FsvrtDeviceExtension->SwapVpb = ExAllocatePoolWithTag(NonPagedPool,
        sizeof *FsvrtDeviceExtension->SwapVpb, FSP_TAG);
    if (0 == FsvrtDeviceExtension->SwapVpb)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(FsvrtDeviceExtension->SwapVpb, sizeof *FsvrtDeviceExtension->SwapVpb);

    return STATUS_SUCCESS;
}

static VOID FspFsvrtDeviceFini(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    FSP_FSVRT_DEVICE_EXTENSION *FsvrtDeviceExtension = FspFsvrtDeviceExtension(DeviceObject);

    if (0 != FsvrtDeviceExtension->SwapVpb)
        ExFreePoolWithTag(FsvrtDeviceExtension->SwapVpb, FSP_TAG);
}

static NTSTATUS FspFsvolDeviceInit(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    FSP_FSVOL_DEVICE_EXTENSION *FsvolDeviceExtension = FspFsvolDeviceExtension(DeviceObject);

    ExInitializeFastMutex(&FsvolDeviceExtension->GenericTableFastMutex);
    RtlInitializeGenericTableAvl(&FsvolDeviceExtension->GenericTable, 0, 0, 0, 0);

    return STATUS_SUCCESS;
}

static VOID FspFsvolDeviceFini(PDEVICE_OBJECT DeviceObject)
{
    // !PAGED_CODE(); /* because of fast mutex use in GenericTable */

    FSP_FSVOL_DEVICE_EXTENSION *FsvolDeviceExtension = FspFsvolDeviceExtension(DeviceObject);

    /*
     * Enumerate and delete all entries in the GenericTable.
     */
    ExAcquireFastMutex(&FsvolDeviceExtension->GenericTableFastMutex);
    FSP_DEVICE_GENERIC_TABLE_ELEMENT *Element;
    while (0 != (Element = RtlGetElementGenericTableAvl(&FsvolDeviceExtension->GenericTable, 0)))
        RtlDeleteElementGenericTableAvl(&FsvolDeviceExtension->GenericTable, &Element->Identifier);
    ExReleaseFastMutex(&FsvolDeviceExtension->GenericTableFastMutex);

    /*
     * Dereference the virtual volume device so that it can now go away.
     */
    if (0 != FsvolDeviceExtension->FsvrtDeviceObject)
        ObDereferenceObject(FsvolDeviceExtension->FsvrtDeviceObject);
}

BOOLEAN FspDeviceRetain(PDEVICE_OBJECT DeviceObject)
{
    // !PAGED_CODE();

    BOOLEAN Result;
    FSP_DEVICE_EXTENSION *DeviceExtension;
    KIRQL Irql;

    DeviceExtension = FspDeviceExtension(DeviceObject);
    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    Result = 0 != DeviceExtension->RefCount;
    if (Result)
        DeviceExtension->RefCount++;
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    return Result;
}

VOID FspDeviceRelease(PDEVICE_OBJECT DeviceObject)
{
    // !PAGED_CODE();

    BOOLEAN Delete = FALSE;
    FSP_DEVICE_EXTENSION *DeviceExtension;
    KIRQL Irql;

    DeviceExtension = FspDeviceExtension(DeviceObject);
    KeAcquireSpinLock(&DeviceExtension->SpinLock, &Irql);
    if (0 != DeviceExtension->RefCount)
    {
        DeviceExtension->RefCount--;
        Delete = 0 == DeviceExtension->RefCount;
    }
    KeReleaseSpinLock(&DeviceExtension->SpinLock, Irql);

    if (Delete)
        FspDeviceDelete(DeviceObject);
}

VOID FspFsctlDeviceVolumeCreated(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    ASSERT(FspFsctlDeviceExtensionKind == FspDeviceExtension(DeviceObject)->Kind);
    ASSERT(ExIsResourceAcquiredExclusiveLite(&FspDeviceExtension(DeviceObject)->Resource));

    ULONG FsvrtDeviceObjectCount = FspFsctlDeviceExtension(DeviceObject)->FsvrtDeviceObjectCount++;
    if (0 == FsvrtDeviceObjectCount)
        IoRegisterFileSystem(DeviceObject);
}

VOID FspFsctlDeviceVolumeDeleted(PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();

    ASSERT(FspFsctlDeviceExtensionKind == FspDeviceExtension(DeviceObject)->Kind);
    ASSERT(ExIsResourceAcquiredExclusiveLite(&FspDeviceExtension(DeviceObject)->Resource));

    ULONG FsvrtDeviceObjectCount = --FspFsctlDeviceExtension(DeviceObject)->FsvrtDeviceObjectCount;
    if (0 == FsvrtDeviceObjectCount)
        IoUnregisterFileSystem(DeviceObject);
}

NTSTATUS FspFsvolDeviceInsertContext(PDEVICE_OBJECT DeviceObject, UINT64 Identifier, PVOID Context,
    PBOOLEAN PInserted)
{
    // !PAGED_CODE();

    FSP_FSVOL_DEVICE_EXTENSION *FsvolDeviceExtension = FspFsvolDeviceExtension(DeviceObject);
    ASSERT(FspFsvolDeviceExtensionKind == FsvolDeviceExtension->Base.Kind);

    NTSTATUS Result;
    FSP_DEVICE_GENERIC_TABLE_ELEMENT Element = { 0 };
    Element.Identifier = Identifier;
    Element.Context = Context;

    ExAcquireFastMutex(&FsvolDeviceExtension->GenericTableFastMutex);
    Result = 0 != RtlInsertElementGenericTableAvl(&FsvolDeviceExtension->GenericTable,
        &Element, sizeof Element, PInserted) ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
    ExReleaseFastMutex(&FsvolDeviceExtension->GenericTableFastMutex);

    return Result;
}

VOID FspFsvolDeviceDeleteContext(PDEVICE_OBJECT DeviceObject, UINT64 Identifier,
    PBOOLEAN PDeleted)
{
    // !PAGED_CODE();

    FSP_FSVOL_DEVICE_EXTENSION *FsvolDeviceExtension = FspFsvolDeviceExtension(DeviceObject);
    ASSERT(FspFsvolDeviceExtensionKind == FsvolDeviceExtension->Base.Kind);

    ExAcquireFastMutex(&FsvolDeviceExtension->GenericTableFastMutex);
    BOOLEAN Deleted = RtlDeleteElementGenericTableAvl(&FsvolDeviceExtension->GenericTable,
        &Identifier);
    ExReleaseFastMutex(&FsvolDeviceExtension->GenericTableFastMutex);

    if (0 != PDeleted)
        *PDeleted = Deleted;
}

static RTL_GENERIC_COMPARE_RESULTS NTAPI FspFsvolDeviceCompareElement(
    PRTL_AVL_TABLE Table, PVOID FirstElement, PVOID SecondElement)
{
    // !PAGED_CODE();

    if (FirstElement < SecondElement)
        return GenericLessThan;
    else
    if (SecondElement < FirstElement)
        return GenericGreaterThan;
    else
        return GenericEqual;
}

static PVOID NTAPI FspFsvolDeviceAllocateElement(
    PRTL_AVL_TABLE Table, CLONG ByteSize)
{
    // !PAGED_CODE();

    /* allocate from non-paged pool because of fast mutex use in GenericTable */
    return ExAllocatePoolWithTag(NonPagedPool, ByteSize, FSP_TAG);
}

static VOID NTAPI FspFsvolDeviceFreeElement(
    PRTL_AVL_TABLE Table, PVOID Buffer)
{
    // !PAGED_CODE();

    ExFreePoolWithTag(Buffer, FSP_TAG);
}

NTSTATUS FspDeviceCopyList(
    PDEVICE_OBJECT **PDeviceObjects, PULONG PDeviceObjectCount)
{
    PAGED_CODE();

    PDEVICE_OBJECT *DeviceObjects = 0;
    ULONG DeviceObjectCount = 0;

    while (STATUS_BUFFER_TOO_SMALL == IoEnumerateDeviceObjectList(FspDriverObject,
        DeviceObjects, sizeof *DeviceObjects * DeviceObjectCount, &DeviceObjectCount))
    {
        if (0 != DeviceObjects)
            ExFreePoolWithTag(DeviceObjects, FSP_TAG);
        DeviceObjects = ExAllocatePoolWithTag(NonPagedPool,
            sizeof *DeviceObjects * DeviceObjectCount, FSP_TAG);
        if (0 == DeviceObjects)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlZeroMemory(DeviceObjects, sizeof *DeviceObjects * DeviceObjectCount);
    }

    *PDeviceObjects = DeviceObjects;
    *PDeviceObjectCount = DeviceObjectCount;

    return STATUS_SUCCESS;
}

VOID FspDeviceDeleteList(
    PDEVICE_OBJECT *DeviceObjects, ULONG DeviceObjectCount)
{
    PAGED_CODE();

    for (ULONG i = 0; DeviceObjectCount > i; i++)
        ObDereferenceObject(DeviceObjects[i]);

    ExFreePoolWithTag(DeviceObjects, FSP_TAG);
}

VOID FspDeviceDeleteAll(VOID)
{
    PAGED_CODE();

    NTSTATUS Result;
    PDEVICE_OBJECT *DeviceObjects = 0;
    ULONG DeviceObjectCount = 0;

    Result = FspDeviceCopyList(&DeviceObjects, &DeviceObjectCount);
    if (!NT_SUCCESS(Result))
        return;

    for (ULONG i = 0; DeviceObjectCount > i; i++)
        FspDeviceDelete(DeviceObjects[i]);

    FspDeviceDeleteList(DeviceObjects, DeviceObjectCount);
}