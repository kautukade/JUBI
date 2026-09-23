"""Fail-closed Linux network sandbox wrapper for Jubi test commands.

This process is launched *inside* the bubblewrap filesystem sandbox. It installs
a libseccomp filter that denies socket creation/connection syscalls, then execs
the fixed test command. The filter survives exec and therefore applies to the
test runner and its descendants.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import errno
import os
import sys


_SCMP_ACT_ALLOW = 0x7FFF0000


def _act_errno(value: int) -> int:
    return 0x00050000 | (int(value) & 0xFFFF)


def install_no_network_filter() -> None:
    if not sys.platform.startswith("linux"):
        raise RuntimeError("seccomp network sandbox is Linux-only")

    library = ctypes.util.find_library("seccomp")
    if not library:
        raise RuntimeError("libseccomp is required for VPS autonomous test isolation")
    lib = ctypes.CDLL(library, use_errno=True)

    lib.seccomp_init.argtypes = [ctypes.c_uint32]
    lib.seccomp_init.restype = ctypes.c_void_p
    lib.seccomp_release.argtypes = [ctypes.c_void_p]
    lib.seccomp_release.restype = None
    lib.seccomp_syscall_resolve_name.argtypes = [ctypes.c_char_p]
    lib.seccomp_syscall_resolve_name.restype = ctypes.c_int
    lib.seccomp_rule_add.argtypes = [
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32
    ]
    lib.seccomp_rule_add.restype = ctypes.c_int
    lib.seccomp_load.argtypes = [ctypes.c_void_p]
    lib.seccomp_load.restype = ctypes.c_int

    ctx = lib.seccomp_init(_SCMP_ACT_ALLOW)
    if not ctx:
        raise RuntimeError("seccomp_init failed")
    try:
        denied = (
            b"socket", b"socketpair", b"connect", b"bind", b"listen",
            b"accept", b"accept4", b"sendto", b"sendmsg", b"sendmmsg",
            b"recvfrom", b"recvmsg", b"recvmmsg",
        )
        action = _act_errno(errno.EPERM)
        installed = 0
        for name in denied:
            number = lib.seccomp_syscall_resolve_name(name)
            if number < 0:
                continue
            rc = lib.seccomp_rule_add(ctx, action, number, 0)
            if rc != 0:
                raise RuntimeError("seccomp_rule_add failed for " + name.decode())
            installed += 1
        if installed < 3:
            raise RuntimeError("seccomp network deny set is unexpectedly incomplete")
        rc = lib.seccomp_load(ctx)
        if rc != 0:
            err = ctypes.get_errno()
            raise RuntimeError("seccomp_load failed: " + os.strerror(err or abs(rc)))
    finally:
        lib.seccomp_release(ctx)


def main() -> None:
    args = sys.argv[1:]
    if args and args[0] == "--":
        args = args[1:]
    if not args:
        raise SystemExit("sandbox_exec requires a command")
    install_no_network_filter()
    os.execvpe(args[0], args, os.environ.copy())


if __name__ == "__main__":
    main()
