"""Tests for live packet-capture failure notices."""

from cai.tools.evidence.capture_notice import (
    apply_packet_capture_notice,
    packet_capture_failure_notice,
)


def test_detects_tcpdump_cap_net_raw():
    cmd = "tcpdump -i any -w /tmp/x.pcap -c 5 host 10.0.0.1"
    out = "tcpdump: permission to perform this capture (CAP_NET_RAW may be required)"
    notice = packet_capture_failure_notice(cmd, out)
    assert notice is not None
    assert "PACKET-CAPTURE FAILURE" in notice


def test_skips_tshark_read_and_write_filter():
    cmd = "tshark -r in.pcap -Y http -w out.pcap"
    out = "Running as user"
    assert packet_capture_failure_notice(cmd, out) is None


def test_apply_prepends_notice():
    cmd = "tshark -i eth0 -w out.pcap"
    out = "Couldn't run dumpcap: Permission denied"
    result = apply_packet_capture_notice(cmd, out)
    assert result.startswith("[CAI PACKET-CAPTURE FAILURE]")
