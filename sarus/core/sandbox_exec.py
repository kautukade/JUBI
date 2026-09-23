"""Unprivileged Linux sandbox wrapper for Jubi test commands.

Landlock confines filesystem mutation to the selected project and /tmp.
libseccomp denies networking syscalls. Both restrictions survive exec and apply
to the test runner and descendants without root, Docker, or user namespaces.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import errno
import os
import platform
import sys
from pathlib import Path


_SCMP_ACT_ALLOW = 0x7FFF0000

# Linux Landlock UAPI. AWS targets supported by this profile are x86_64/aarch64,
# where Landlock uses the generic syscall numbers below.
_SYS_LANDLOCK_CREATE_RULESET = 444
_SYS_LANDLOCK_ADD_RULE = 445
_SYS_LANDLOCK_RESTRICT_SELF = 446
_LANDLOCK_CREATE_RULESET_VERSION = 1
_LANDLOCK_RULE_PATH_BENEATH = 1

_LL_EXECUTE = 1 << 0
_LL_WRITE_FILE = 1 << 1
_LL_READ_FILE = 1 << 2
_LL_READ_DIR = 1 << 3
_LL_REMOVE_DIR = 1 << 4
_LL_REMOVE_FILE = 1 << 5
_LL_MAKE_CHAR = 1 << 6
_LL_MAKE_DIR = 1 << 7
_LL_MAKE_REG = 1 << 8
_LL_MAKE_SOCK = 1 << 9
_LL_MAKE_FIFO = 1 << 10
_LL_MAKE_BLOCK = 1 << 11
_LL_MAKE_SYM = 1 << 12
_LL_REFER = 1 << 13
_LL_TRUNCATE = 1 << 14

_LL_READ_EXEC = _LL_EXECUTE | _LL_READ_FILE | _LL_READ_DIR
_LL_WRITE_V1 = (
    _LL_WRITE_FILE | _LL_REMOVE_DIR | _LL_REMOVE_FILE | _LL_MAKE_CHAR
    | _LL_MAKE_DIR | _LL_MAKE_REG | _LL_MAKE_SOCK | _LL_MAKE_FIFO
    | _LL_MAKE_BLOCK | _LL_MAKE_SYM
)
_PR_SET_NO_NEW_PRIVS = 38


class _RulesetAttr(ctypes.Structure):
    _fields_ = [("handled_access_fs", ctypes.c_uint64)]


class _PathBeneathAttr(ctypes.Structure):
    _pack_ = 1
    _fields_ = [
        ("allowed_access", ctypes.c_uint64),
        ("parent_fd", ctypes.c_int32),
    ]


def _libc():
    libc = ctypes.CDLL(None, use_errno=True)
    libc.syscall.restype = ctypes.c_long
    libc.prctl.restype = ctypes.c_int
    return libc


def landlock_abi() -> int:
    if not sys.platform.startswith("linux"):
        return 0
    if platform.machine().lower() not in {"x86_64", "amd64", "aarch64", "arm64"}:
        return 0
    libc = _libc()
    rc = libc.syscall(
        _SYS_LANDLOCK_CREATE_RULESET,
        ctypes.c_void_p(),
        ctypes.c_size_t(0),
        ctypes.c_uint32(_LANDLOCK_CREATE_RULESET_VERSION),
    )
    return int(rc) if rc >= 1 else 0


def _handled_access(abi: int) -> int:
    rights = (1 << 13) - 1  # ABI v1 through MAKE_SYM.
    if abi >= 2:
        rights |= _LL_REFER
    if abi >= 3:
        rights |= _LL_TRUNCATE
    return rights


def install_filesystem_filter(project: Path) -> dict:
    abi = landlock_abi()
    if abi < 1:
        raise RuntimeError("Linux Landlock is unavailable or disabled")
    project = project.resolve()
    if not project.is_dir():
        raise RuntimeError("Sandbox project directory does not exist")

    libc = _libc()
    handled = _handled_access(abi)
    attr = _RulesetAttr(handled_access_fs=handled)
    ruleset_fd = libc.syscall(
        _SYS_LANDLOCK_CREATE_RULESET,
        ctypes.byref(attr),
        ctypes.sizeof(attr),
        ctypes.c_uint32(0),
    )
    if ruleset_fd < 0:
        err = ctypes.get_errno()
        raise RuntimeError("landlock_create_ruleset failed: " + os.strerror(err))

    opened: list[int] = []
    try:
        def allow(path: Path, access: int):
            path = path.resolve()
            fd = os.open(str(path), os.O_PATH | os.O_CLOEXEC)
            opened.append(fd)
            rule = _PathBeneathAttr(
                allowed_access=access & handled,
                parent_fd=fd,
            )
            rc = libc.syscall(
                _SYS_LANDLOCK_ADD_RULE,
                ctypes.c_int(ruleset_fd),
                ctypes.c_int(_LANDLOCK_RULE_PATH_BENEATH),
                ctypes.byref(rule),
                ctypes.c_uint32(0),
            )
            if rc != 0:
                err = ctypes.get_errno()
                raise RuntimeError(
                    "landlock_add_rule failed for " + str(path) + ": " + os.strerror(err)
                )

        # Read/execute from the host remains possible so language runtimes and
        # shared libraries work. Mutating operations are handled but not granted
        # by this root rule.
        allow(Path("/"), _LL_READ_EXEC)

        write_access = _LL_READ_EXEC | _LL_WRITE_V1
        if abi >= 2:
            write_access |= _LL_REFER
        if abi >= 3:
            write_access |= _LL_TRUNCATE
        allow(project, write_access)
        allow(Path("/tmp"), write_access)

        if libc.prctl(_PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0:
            err = ctypes.get_errno()
            raise RuntimeError("PR_SET_NO_NEW_PRIVS failed: " + os.strerror(err))
        rc = libc.syscall(
            _SYS_LANDLOCK_RESTRICT_SELF,
            ctypes.c_int(ruleset_fd),
            ctypes.c_uint32(0),
        )
        if rc != 0:
            err = ctypes.get_errno()
            raise RuntimeError("landlock_restrict_self failed: " + os.strerror(err))
    finally:
        for fd in opened:
            os.close(fd)
        os.close(ruleset_fd)

    return {"abi": abi, "project": str(project)}


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
    project = None
    if len(args) >= 2 and args[0] == "--project":
        project = Path(args[1])
        args = args[2:]
    if args and args[0] == "--":
        args = args[1:]
    if project is None or not args:
        raise SystemExit("sandbox_exec requires --project PATH -- COMMAND [ARGS...]")

    install_filesystem_filter(project)
    install_no_network_filter()
    os.execvpe(args[0], args, os.environ.copy())


if __name__ == "__main__":
    main()
