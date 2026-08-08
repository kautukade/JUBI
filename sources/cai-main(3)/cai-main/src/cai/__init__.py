"""
A library to build Bug Bounty-level grade Cybersecurity AIs (CAIs).
"""


def is_pentestperf_available():
    """
    Check if caibench (formerly pentestperf) is available
    """
    try:
        from cai.caibench.ctf import CTF  # pylint: disable=import-error,import-outside-toplevel,unused-import  # noqa: E501,F401
    except ImportError:
        return False
    return True


def is_caiextensions_report_available():
    """
    Check if caiextensions report is available
    """
    try:
        from caiextensions.report.common import get_base_instructions  # pylint: disable=import-error,import-outside-toplevel,unused-import  # noqa: E501,F401
    except ImportError:
        return False
    return True


def is_caiextensions_memory_available():
    """
    Check if caiextensions memory is available
    """
    try:
        from caiextensions.memory import is_memory_installed  # pylint: disable=import-error,import-outside-toplevel,unused-import  # noqa: E501,F401
    except ImportError:
        return False
    return True
