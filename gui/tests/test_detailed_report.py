"""The detailed report (port 6668): layout against docs/protocol.md, round trip,
forward compatibility with a manager that appends fields, and malformed payloads."""

import ctypes

import pytest

import health_structs as hs
from health_structs import (
    GPU_RECORD_SIZE,
    REPORT_HEADER_SIZE,
    REPORT_MAGIC,
    SERVICE_RECORD_SIZE,
    GpuRecord,
    ReportHeader,
    RestartMode,
    ServiceRecord,
    ServiceState,
    encode_detailed_report,
    exit_text,
    format_duration_short,
    parse_detailed_report,
    parse_report_frames,
    service_state_text,
)

SEC = 1_000_000_000
GB = 1024**3
MB = 1024**2


def offsets(struct):
    return {name: getattr(struct, name).offset for name, _type in struct._fields_}


def test_layouts_match_the_protocol_tables():
    assert (REPORT_HEADER_SIZE, SERVICE_RECORD_SIZE, GPU_RECORD_SIZE) == (192, 368, 160)
    header = offsets(ReportHeader)
    assert header["magic"] == 0 and header["version"] == 4 and header["headerSize"] == 6
    assert header["serviceRecordSize"] == 8 and header["gpuRecordSize"] == 10
    assert header["serviceCount"] == 12 and header["gpuCount"] == 16 and header["managerPid"] == 20
    assert header["snapshotTime"] == 24 and header["managerStartTime"] == 32
    assert header["publishIntervalMs"] == 40 and header["flags"] == 44
    assert header["hostCpuPercent"] == 48 and header["memoryTotalBytes"] == 56
    assert header["memoryAvailableBytes"] == 64 and header["loadAverage1"] == 72
    assert header["uptimeSeconds"] == 96 and header["cpuCount"] == 104
    assert header["hostName"] == 112 and header["managerVersion"] == 176

    service = offsets(ServiceRecord)
    assert service["name"] == 0 and service["binary"] == 32 and service["description"] == 160
    assert service["pid"] == 224 and service["state"] == 228 and service["restartMode"] == 229
    assert service["flags"] == 230 and service["restartCount"] == 232 and service["missedBeats"] == 236
    assert service["lastExitCode"] == 240 and service["processCount"] == 244
    assert service["threadCount"] == 248 and service["openFiles"] == 252
    assert service["startTime"] == 256 and service["lastSeen"] == 264
    assert service["lastExitTime"] == 272 and service["nextRestartTime"] == 280
    assert service["cpuTimeUsec"] == 288 and service["cpuPercent"] == 296
    assert service["memoryBytes"] == 304 and service["memoryPeakBytes"] == 312
    assert service["memoryLimitBytes"] == 320 and service["ioReadBytes"] == 328
    assert service["ioWriteBytes"] == 336 and service["gpuPercent"] == 344
    assert service["gpuMemoryBytes"] == 352 and service["oomKills"] == 360
    assert service["cpuLimitPercent"] == 364

    gpu = offsets(GpuRecord)
    assert gpu["name"] == 0 and gpu["uuid"] == 64 and gpu["index"] == 112
    assert gpu["temperatureC"] == 116 and gpu["utilizationPercent"] == 120
    assert gpu["memoryUtilizationPercent"] == 128 and gpu["memoryTotalBytes"] == 136
    assert gpu["memoryUsedBytes"] == 144 and gpu["powerMilliwatts"] == 152


def report():
    return {
        "managerPid": 12246,
        "snapshotTime": 100 * SEC,
        "managerStartTime": 89 * SEC,
        "publishIntervalMs": 1000,
        "cgroups": True,
        "gpuMonitoring": True,
        "stopping": False,
        "windows": False,
        "hostCpuPercent": 16.25,
        "memoryTotalBytes": 16 * GB,
        "memoryAvailableBytes": 3 * GB,
        "loadAverage": (0.52, 0.58, 0.59),
        "uptimeSeconds": 4000,
        "cpuCount": 16,
        "hostName": "rig-01",
        "managerVersion": "1.0.0",
        "services": [
            {
                "name": "vision_pipeline",
                "binary": "/opt/beray/bin/vision_pipeline",
                "description": "Camera capture and inference",
                "pid": 12252,
                "state": ServiceState.RUNNING,
                "restartMode": RestartMode.ON_FAILURE,
                "autostart": True,
                "heartbeat": True,
                "cgroup": True,
                "usageValid": True,
                "gpuValid": True,
                "removing": False,
                "restartCount": 2,
                "missedBeats": 1,
                "lastExitCode": -9,
                "processCount": 1,
                "threadCount": 12,
                "openFiles": 34,
                "startTime": 89 * SEC,
                "lastSeen": 100 * SEC,
                "lastExitTime": 80 * SEC,
                "nextRestartTime": 0,
                "cpuTimeUsec": 5_000_000,
                "cpuPercent": 4.0,
                "memoryBytes": 10 * MB,
                "memoryPeakBytes": 20 * MB,
                "memoryLimitBytes": GB,
                "ioReadBytes": 3 * MB,
                "ioWriteBytes": MB,
                "gpuPercent": 30.5,
                "gpuMemoryBytes": 210 * MB,
                "oomKills": 0,
                "cpuLimitPercent": 200,
            },
            {
                "name": "path_planner",
                "binary": "/opt/beray/bin/path_planner",
                "description": "",
                "pid": 0,
                "state": ServiceState.BACKOFF,
                "restartMode": RestartMode.ALWAYS,
                "autostart": False,
                "heartbeat": False,
                "cgroup": False,
                "usageValid": False,
                "gpuValid": False,
                "removing": False,
                "restartCount": 3,
                "missedBeats": 0,
                "lastExitCode": 3,
                "processCount": 0,
                "threadCount": 0,
                "openFiles": None,
                "startTime": 0,
                "lastSeen": 97 * SEC,
                "lastExitTime": 97 * SEC,
                "nextRestartTime": 102 * SEC + SEC // 2,
                "cpuTimeUsec": 0,
                "cpuPercent": None,
                "memoryBytes": 0,
                "memoryPeakBytes": 0,
                "memoryLimitBytes": 0,
                "ioReadBytes": 0,
                "ioWriteBytes": 0,
                "gpuPercent": None,
                "gpuMemoryBytes": 0,
                "oomKills": 0,
                "cpuLimitPercent": 0,
            },
        ],
        "gpus": [
            {
                "name": "NVIDIA RTX A4000",
                "uuid": "GPU-0123",
                "index": 0,
                "temperatureC": 61,
                "utilizationPercent": 34.0,
                "memoryUtilizationPercent": 19.0,
                "memoryTotalBytes": 16 * GB,
                "memoryUsedBytes": 3 * GB,
                "powerMilliwatts": 74_000,
            }
        ],
    }


def test_round_trip_keeps_every_field():
    original = report()
    raw = encode_detailed_report(original)
    assert len(raw) == REPORT_HEADER_SIZE + 2 * SERVICE_RECORD_SIZE + GPU_RECORD_SIZE
    assert raw[:4] == b"BPMR"

    parsed = parse_detailed_report(raw)
    assert parsed["flags"] == hs.REPORT_FLAG_CGROUPS | hs.REPORT_FLAG_GPU
    for key, value in original.items():
        if key != "services" and key != "gpus":
            assert parsed[key] == value, key
    for got, wanted in zip(parsed["services"], original["services"]):
        for key, value in wanted.items():
            assert got[key] == value, key
    assert parsed["services"][0]["flags"] == 0x1F
    assert parsed["gpus"] == original["gpus"]


def test_unknown_figures_are_none_and_encode_as_negative():
    raw = encode_detailed_report({"hostCpuPercent": None, "services": [{"cpuPercent": None, "gpuPercent": None, "openFiles": None}]})
    record = ServiceRecord.from_buffer_copy(raw, REPORT_HEADER_SIZE)
    assert record.cpuPercent == -1.0 and record.gpuPercent == -1.0 and record.openFiles == -1
    parsed = parse_detailed_report(raw)
    assert parsed["hostCpuPercent"] is None
    service = parsed["services"][0]
    assert service["cpuPercent"] is None and service["gpuPercent"] is None and service["openFiles"] is None
    assert service["state"] == ServiceState.STOPPED and service["restartMode"] == RestartMode.NEVER


def test_text_fields_are_cut_at_a_character_boundary():
    raw = encode_detailed_report({"hostName": "h" * 100, "services": [{"name": "ü" * 20}]})
    header = ReportHeader.from_buffer_copy(raw)
    assert header.hostName == b"h" * 63  # the last byte stays NUL
    parsed = parse_detailed_report(raw)
    assert parsed["hostName"] == "h" * 63
    assert parsed["services"][0]["name"] == "ü" * 15  # 31 bytes would split a character


def test_a_newer_manager_that_appends_fields_still_parses():
    raw = bytearray(encode_detailed_report(report()))
    # Grow every structure by 8 bytes and say so in the header.
    header_size, record_size, gpu_size = REPORT_HEADER_SIZE + 8, SERVICE_RECORD_SIZE + 8, GPU_RECORD_SIZE + 8
    grown = bytearray()
    grown += raw[:REPORT_HEADER_SIZE] + b"\xff" * 8
    offset = REPORT_HEADER_SIZE
    for _ in range(2):
        grown += raw[offset:offset + SERVICE_RECORD_SIZE] + b"\xff" * 8
        offset += SERVICE_RECORD_SIZE
    grown += raw[offset:offset + GPU_RECORD_SIZE] + b"\xff" * 8
    grown[6:8] = header_size.to_bytes(2, "little")
    grown[8:10] = record_size.to_bytes(2, "little")
    grown[10:12] = gpu_size.to_bytes(2, "little")

    parsed = parse_detailed_report(bytes(grown))
    assert [s["name"] for s in parsed["services"]] == ["vision_pipeline", "path_planner"]
    assert parsed["services"][1]["nextRestartTime"] == 102 * SEC + SEC // 2
    assert parsed["gpus"][0]["name"] == "NVIDIA RTX A4000"


def test_an_empty_report_has_no_records():
    parsed = parse_detailed_report(encode_detailed_report({"hostName": "h"}))
    assert parsed["services"] == [] and parsed["gpus"] == [] and parsed["hostName"] == "h"


@pytest.mark.parametrize(
    "damage, reason",
    [
        (lambda raw: raw[:100], "too short"),
        (lambda raw: b"XXXX" + raw[4:], "not a report"),
        (lambda raw: raw[:4] + (2).to_bytes(2, "little") + raw[6:], "version 2"),
        (lambda raw: raw[:6] + (100).to_bytes(2, "little") + raw[8:], "layout too small"),
        (lambda raw: raw[:12] + (5).to_bytes(4, "little") + raw[16:], "announces"),
        (lambda raw: raw + b"\x00", "announces"),
        (lambda raw: raw[:12] + (200_000).to_bytes(4, "little") + raw[16:], "out of range"),
    ],
    ids=["short", "magic", "version", "layout", "count", "trailing", "huge-count"],
)
def test_malformed_payloads_raise(damage, reason):
    raw = encode_detailed_report(report())
    with pytest.raises(ValueError, match=reason):
        parse_detailed_report(damage(raw))


def test_report_frames_need_the_topic():
    raw = encode_detailed_report(report())
    assert parse_report_frames([b"report", raw])["managerPid"] == 12246
    assert parse_report_frames([b"health", raw]) is None
    assert parse_report_frames([raw]) is None
    assert parse_report_frames([b"report", raw, b"extra"]) is None
    with pytest.raises(ValueError):
        parse_report_frames([b"report", raw[:-1]])


def test_magic_is_the_bytes_bpmr():
    assert REPORT_MAGIC.to_bytes(4, "little") == b"BPMR"
    assert ctypes.sizeof(ReportHeader) == 192


def test_exit_text_matches_the_cli():
    assert exit_text(0) == "exit 0"
    assert exit_text(3) == "exit 3"
    assert exit_text(-9) == "signal 9 (KILL)"
    assert exit_text(-15) == "signal 15 (TERM)"
    assert exit_text(-30) == "signal 30"
    assert exit_text(-9, windows=True) == "exit 0xFFFFFFF7"
    assert exit_text(-1073741819, windows=True) == "exit 0xC0000005"
    assert exit_text(-1073741819) == "exit 0xC0000005"  # not a signal number either


def test_short_durations_read_like_the_cli():
    assert format_duration_short(11 * SEC) == "11s"
    assert format_duration_short(185 * SEC) == "3m 05s"
    assert format_duration_short(7627 * SEC) == "2h 07m"
    assert format_duration_short(100_000 * SEC) == "1d 03h"
    assert format_duration_short(-5) == "0s"


def test_service_state_text_matches_the_cli():
    snapshot = 100 * SEC
    assert service_state_text({"state": ServiceState.RUNNING}, snapshot) == "running"
    assert service_state_text(
        {"state": ServiceState.BACKOFF, "nextRestartTime": 102 * SEC + SEC // 2}, snapshot
    ) == "backoff 2s"
    assert service_state_text({"state": ServiceState.BACKOFF, "nextRestartTime": 90 * SEC}, snapshot) == "backoff"
    assert service_state_text({"state": ServiceState.RUNNING, "missedBeats": 2}, snapshot) == "running (2 missed)"
    assert service_state_text({"state": ServiceState.STOPPED, "missedBeats": 2}, snapshot) == "stopped"
    assert service_state_text({"state": ServiceState.FAILED, "removing": True}, snapshot) == "failed (removing)"
