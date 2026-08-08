"""Detect failed live packet captures and prepend a model-facing notice."""

from __future__ import annotations

import re

_PACKET_CAPTURE_TOOL_RE = re.compile(r"\b(tcpdump|tshark|dumpcap|wireshark)\b", re.I)
_PACKET_CAPTURE_FAILURE_RE = re.compile(
    r"cap_net_raw|packet socket failed|permission to perform this capture|"
    r"couldn't run dumpcap|dumpcap.*permission denied|0 packets captured",
    re.I,
)
_PACKET_CAPTURE_NOTICE = (
    "[CAI PACKET-CAPTURE FAILURE]\n"
    "Live packet capture did not succeed (CAP_NET_RAW / dumpcap permissions are often required).\n"
    "Do NOT replace PCAPs with curl/openssl/logs saved as .txt under capture or screenshot paths.\n"
    "Tell the operator immediately. Remediation: sudo setcap cap_net_raw+eip $(which dumpcap); "
    "run capture inside CAI Docker (--cap-add=NET_RAW); or analyze an existing .pcap.\n"
    "Filtering an existing PCAP with tshark -r ... -Y ... -w is fine.\n"
)


def packet_capture_failure_notice(command: str, output: str) -> str | None:
    """Return a notice when a live capture command failed."""
    if not output or not _PACKET_CAPTURE_TOOL_RE.search(command):
        return None
    if re.search(r"\b-r\b", command, re.I) and not re.search(r"\b-i\b", command, re.I):
        return None
    if not _PACKET_CAPTURE_FAILURE_RE.search(output):
        return None
    return _PACKET_CAPTURE_NOTICE


def apply_packet_capture_notice(command: str, output: str) -> str:
    """Prepend capture-failure notice to tool output when applicable."""
    if not isinstance(output, str):
        return output
    notice = packet_capture_failure_notice(command, output)
    if notice:
        return f"{notice}\n{output}"
    return output
