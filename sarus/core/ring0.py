from __future__ import annotations

import ctypes
import os
from ctypes import wintypes


# Shared ABI with driver/SarusRing0/sarus_ring0_shared.h.
# These are fixed control codes; callers cannot supply an arbitrary IOCTL.
FILE_DEVICE_UNKNOWN = 0x22
METHOD_BUFFERED = 0
FILE_READ_DATA = 0x0001


def _ctl_code(device_type: int, function: int, method: int, access: int) -> int:
    return (device_type << 16) | (access << 14) | (function << 2) | method


IOCTL_SARUS_RING0_PING = _ctl_code(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_DATA)
IOCTL_SARUS_RING0_STATUS = _ctl_code(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_DATA)


class _Ring0Status(ctypes.Structure):
    _fields_ = [
        ('protocol_version', wintypes.DWORD),
        ('capability_flags', wintypes.DWORD),
        ('current_irql', wintypes.DWORD),
        ('reserved', wintypes.DWORD),
        ('interrupt_time_100ns', ctypes.c_uint64),
        ('build_tag', ctypes.c_char * 32),
    ]


class Ring0Bridge:
    """Client for SARUS' narrow kernel-mode bridge.

    The class intentionally exposes no generic DeviceIoControl primitive and no
    kernel-address/memory parameters. Every permitted kernel call has a fixed
    method and fixed IOCTL constant compiled into SARUS.
    """

    DEVICE_PATH = r'\\.\SarusRing0'
    PROTOCOL_VERSION = 1

    def available(self) -> bool:
        return bool(self.status().get('ok'))

    @staticmethod
    def _kernel32():
        if os.name != 'nt':
            return None
        k32 = ctypes.WinDLL('kernel32', use_last_error=True)
        k32.CreateFileW.argtypes = [
            wintypes.LPCWSTR,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.DWORD,
            wintypes.HANDLE,
        ]
        k32.CreateFileW.restype = wintypes.HANDLE
        k32.DeviceIoControl.argtypes = [
            wintypes.HANDLE,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            wintypes.LPVOID,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
            wintypes.LPVOID,
        ]
        k32.DeviceIoControl.restype = wintypes.BOOL
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        k32.CloseHandle.restype = wintypes.BOOL
        return k32

    def _fixed_call(self, ioctl: int) -> dict:
        if os.name != 'nt':
            return {'ok': False, 'driver_present': False, 'error': 'Ring0 bridge is Windows-only'}

        k32 = self._kernel32()
        assert k32 is not None
        GENERIC_READ = 0x80000000
        FILE_SHARE_READ = 0x00000001
        FILE_SHARE_WRITE = 0x00000002
        OPEN_EXISTING = 3
        INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

        handle = k32.CreateFileW(
            self.DEVICE_PATH,
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            0,
            None,
        )
        if handle == INVALID_HANDLE_VALUE:
            err = ctypes.get_last_error()
            return {
                'ok': False,
                'driver_present': False,
                'winerror': err,
                'error': ctypes.FormatError(err).strip(),
            }

        try:
            status = _Ring0Status()
            returned = wintypes.DWORD(0)
            ok = bool(k32.DeviceIoControl(
                handle,
                ioctl,
                None,
                0,
                ctypes.byref(status),
                ctypes.sizeof(status),
                ctypes.byref(returned),
                None,
            ))
            if not ok:
                err = ctypes.get_last_error()
                return {
                    'ok': False,
                    'driver_present': True,
                    'winerror': err,
                    'error': ctypes.FormatError(err).strip(),
                }
            if returned.value < ctypes.sizeof(_Ring0Status):
                return {'ok': False, 'driver_present': True, 'error': 'short Ring0 status response'}
            tag = bytes(status.build_tag).split(b'\x00', 1)[0].decode('ascii', errors='replace')
            protocol_ok = status.protocol_version == self.PROTOCOL_VERSION
            return {
                'ok': protocol_ok,
                'driver_present': True,
                'protocol_version': int(status.protocol_version),
                'protocol_expected': self.PROTOCOL_VERSION,
                'capability_flags': int(status.capability_flags),
                'current_irql': int(status.current_irql),
                'interrupt_time_100ns': int(status.interrupt_time_100ns),
                'build_tag': tag,
                'error': None if protocol_ok else 'Ring0 protocol version mismatch',
            }
        finally:
            k32.CloseHandle(handle)

    def ping(self) -> dict:
        return self._fixed_call(IOCTL_SARUS_RING0_PING)

    def status(self) -> dict:
        return self._fixed_call(IOCTL_SARUS_RING0_STATUS)
