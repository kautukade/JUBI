#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <Windows.h>
#include <winioctl.h>
#endif

#define SARUS_RING0_PROTOCOL_VERSION 1u
#define SARUS_RING0_CAP_PING   0x00000001u
#define SARUS_RING0_CAP_STATUS 0x00000002u

#define IOCTL_SARUS_RING0_PING \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
#define IOCTL_SARUS_RING0_STATUS \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA)

typedef struct _SARUS_RING0_STATUS {
    ULONG ProtocolVersion;
    ULONG CapabilityFlags;
    ULONG CurrentIrql;
    ULONG Reserved;
    ULONGLONG InterruptTime100ns;
    CHAR BuildTag[32];
} SARUS_RING0_STATUS, *PSARUS_RING0_STATUS;
