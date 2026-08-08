#include <ntddk.h>
#include <wdmsec.h>
#include <initguid.h>
#include "sarus_ring0_shared.h"

DEFINE_GUID(GUID_SARUS_RING0_DEVICE_CLASS,
    0x9f15e987, 0x1f93, 0x4bf0, 0xa1, 0x74, 0x1c, 0x70, 0x2d, 0x69, 0x11, 0x42);

static const WCHAR kDeviceNameBuffer[] = L"\\Device\\SarusRing0";
static const WCHAR kDosNameBuffer[] = L"\\DosDevices\\SarusRing0";
static const CHAR kBuildTag[] = "SARUS-RING0-1";

static NTSTATUS SarusCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS SarusUnsupported(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return SarusCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS SarusCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return SarusCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static VOID SarusFillStatus(PSARUS_RING0_STATUS Status)
{
    RtlZeroMemory(Status, sizeof(*Status));
    Status->ProtocolVersion = SARUS_RING0_PROTOCOL_VERSION;
    Status->CapabilityFlags = SARUS_RING0_CAP_PING | SARUS_RING0_CAP_STATUS;
    Status->CurrentIrql = (ULONG)KeGetCurrentIrql();
    Status->InterruptTime100ns = KeQueryInterruptTime();
    RtlCopyMemory(Status->BuildTag, kBuildTag, sizeof(kBuildTag));
}

static NTSTATUS SarusDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    ULONG code;
    ULONG outputLength;
    PSARUS_RING0_STATUS statusOut;

    UNREFERENCED_PARAMETER(DeviceObject);

    stack = IoGetCurrentIrpStackLocation(Irp);
    code = stack->Parameters.DeviceIoControl.IoControlCode;
    outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;

    switch (code) {
    case IOCTL_SARUS_RING0_PING:
    case IOCTL_SARUS_RING0_STATUS:
        if (outputLength < sizeof(SARUS_RING0_STATUS) || Irp->AssociatedIrp.SystemBuffer == NULL) {
            return SarusCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        statusOut = (PSARUS_RING0_STATUS)Irp->AssociatedIrp.SystemBuffer;
        SarusFillStatus(statusOut);
        return SarusCompleteIrp(Irp, STATUS_SUCCESS, sizeof(SARUS_RING0_STATUS));

    default:
        return SarusCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static VOID SarusUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING dosName;
    RtlInitUnicodeString(&dosName, kDosNameBuffer);
    IoDeleteSymbolicLink(&dosName);
    if (DriverObject->DeviceObject != NULL) {
        IoDeleteDevice(DriverObject->DeviceObject);
    }
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    PDEVICE_OBJECT deviceObject = NULL;
    UNICODE_STRING deviceName;
    UNICODE_STRING dosName;
    UNICODE_STRING sddl;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) {
        DriverObject->MajorFunction[i] = SarusUnsupported;
    }
    DriverObject->MajorFunction[IRP_MJ_CREATE] = SarusCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = SarusCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SarusDeviceControl;
    DriverObject->DriverUnload = SarusUnload;

    RtlInitUnicodeString(&deviceName, kDeviceNameBuffer);
    RtlInitUnicodeString(&dosName, kDosNameBuffer);
    RtlInitUnicodeString(&sddl, SDDL_DEVOBJ_SYS_ALL_ADM_ALL);

    status = IoCreateDeviceSecure(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &sddl,
        &GUID_SARUS_RING0_DEVICE_CLASS,
        &deviceObject);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    deviceObject->Flags |= DO_BUFFERED_IO;

    status = IoCreateSymbolicLink(&dosName, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(deviceObject);
        return status;
    }

    deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
