"""
Binary layouts matching the C++ side.

Packing: _pack_ = 8  (8-byte alignment)

=== Health reports (ZMQ SUB) ===
Message body = ARRAY of DetailedHealthReport (128 bytes each with pack=8)
  - raw concatenation  OR
  - [uint32_t count LE] + array

=== Commands (ZMQ DEALER → ROUTER) ===
Identity set to b"PMC"
Multipart:
  frame 0 : b"BPM"
  frame 1 : packed CommandMessage (65 bytes)

struct CommandMessage {
    uint8_t command;          // CommandEnum: start 78, stop 79, restart 81,
                              //   heartbeat 90, reload 91
    char    ServiceName[32];  // a service, or "*" for every service
    char    Args[32];         // may be empty
};

=== Replies (ROUTER → DEALER), sent by the C++ manager ===
Multipart:
  frame 0 : b"BPM"
  frame 1 : packed CommandReply (128 bytes)

struct CommandReply {
    uint8_t command;          // echoed CommandEnum
    uint8_t result;           // CommandResult
    char    ServiceName[32];  // echoed
    char    Message[94];      // e.g. "started, pid 4242"
};

=== Detailed report (ZMQ SUB, port 6668), sent by the C++ manager ===
Multipart:
  frame 0 : b"report"
  frame 1 : ReportHeader (192 B) + ServiceRecord[] (368 B each) + GpuRecord[] (160 B each)
The header carries its own size and the record sizes, so a reader skips
fields a later manager appends (parse_detailed_report honours them).

The full protocol is described in docs/protocol.md at the top of the
repository; the report layout is manager/src/DetailedReport.cpp.
"""

from __future__ import annotations

import ctypes
import math
from enum import IntEnum
from typing import List, Optional


# ──────────────────────────────────────────────────────────────────────────────
# Runtime / Health
# ──────────────────────────────────────────────────────────────────────────────

class RuntimeState(IntEnum):
    """Wire values must match the C++ RuntimeState enum."""
    UNKNOWN   = 0
    STARTING  = 1
    RUNNING   = 2
    STOPPED   = 3
    UNHEALTHY = 4

    @classmethod
    def from_byte(cls, value: int) -> "RuntimeState":
        try:
            return cls(value)
        except ValueError:
            return cls.UNKNOWN


class RuntimeHealth(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("state",        ctypes.c_uint8),
        ("start_time",   ctypes.c_int64),   # ns
        ("lastSeen",     ctypes.c_int64),   # ns
        ("missedBeats",  ctypes.c_int32),
        ("restartCount", ctypes.c_int32),
    ]


class DetailedHealthReport(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("processName",        ctypes.c_char * 64),
        ("pid",                ctypes.c_int32),
        ("memoryUsageInBytes", ctypes.c_uint64),
        ("cpuUsageInUsec",     ctypes.c_uint64),
        ("runtimeHealth",      RuntimeHealth),
        ("snapshotTime",       ctypes.c_int64),  # ns
    ]


REPORT_SIZE = ctypes.sizeof(DetailedHealthReport)
assert REPORT_SIZE == 128, f"Unexpected REPORT_SIZE={REPORT_SIZE}"


def parse_health_reports(data: bytes) -> List[DetailedHealthReport]:
    """
    Parse ZMQ message that contains an ARRAY of DetailedHealthReport.
    Supports raw concat and length-prefixed (u32 count) formats.
    """
    if not data:
        return []

    # length-prefixed? (a zero count with no records is an empty list)
    if len(data) >= 4:
        count = int.from_bytes(data[:4], "little", signed=False)
        body = data[4:]
        if count < 10_000 and len(body) == count * REPORT_SIZE:
            return _unpack_array(body, count)

    # raw concatenation
    if len(data) % REPORT_SIZE == 0:
        n = len(data) // REPORT_SIZE
        if n > 0:
            return _unpack_array(data, n)

    raise ValueError(
        f"Cannot parse health-report array.\n"
        f"  received length = {len(data)} bytes\n"
        f"  REPORT_SIZE     = {REPORT_SIZE}\n"
        f"  remainder       = {len(data) % REPORT_SIZE}\n"
        f"  first 32 bytes  = {data[:32].hex(' ')}"
    )


def _unpack_array(data: bytes, n: int) -> List[DetailedHealthReport]:
    reports = []
    for i in range(n):
        reports.append(
            DetailedHealthReport.from_buffer_copy(data, i * REPORT_SIZE)
        )
    return reports


def report_to_dict(report: DetailedHealthReport) -> dict:
    name = report.processName.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
    rh = report.runtimeHealth
    return {
        "processName":        name,
        "pid":                int(report.pid),
        "memoryUsageInBytes": int(report.memoryUsageInBytes),
        "cpuUsageInUsec":     int(report.cpuUsageInUsec),
        "state":              RuntimeState.from_byte(rh.state),
        "start_time":         int(rh.start_time),
        "lastSeen":           int(rh.lastSeen),
        "missedBeats":        int(rh.missedBeats),
        "restartCount":       int(rh.restartCount),
        "snapshotTime":       int(report.snapshotTime),
    }


def format_duration_ns(ns: int) -> str:
    if ns < 0:
        return "—"
    total_sec = ns / 1_000_000_000
    if total_sec < 60:
        return f"{total_sec:.1f}s"
    minutes, sec = divmod(int(total_sec), 60)
    hours, minutes = divmod(minutes, 60)
    if hours:
        return f"{hours:02d}:{minutes:02d}:{sec:02d}"
    return f"{minutes:02d}:{sec:02d}"


def format_bytes(n: int) -> str:
    if n < 1024:
        return f"{n} B"
    for unit, div in (("KB", 1024), ("MB", 1024**2), ("GB", 1024**3), ("TB", 1024**4)):
        if n < div * 1024:
            return f"{n / div:.1f} {unit}"
    return f"{n / 1024**4:.1f} TB"


# ──────────────────────────────────────────────────────────────────────────────
# Commands (DEALER)
# ──────────────────────────────────────────────────────────────────────────────

class CommandEnum(IntEnum):
    """Must match the C++ CommandEnum wire values."""
    START     = 78
    STOP      = 79
    RESTART   = 81
    HEARTBEAT = 90  # a service reporting that it is alive
    RELOAD    = 91  # re-read the configuration file; the service name is ignored


# The service name that means every service (docs/protocol.md).
ALL_SERVICES = "*"


class CommandResult(IntEnum):
    """Result byte of a CommandReply (manager/include/CommandMessage.hpp)."""
    OK              = 0
    UNKNOWN_SERVICE = 1
    UNKNOWN_COMMAND = 2
    MALFORMED       = 3
    ALREADY         = 4
    INVALID_STATE   = 5
    LAUNCH_FAILED   = 6
    SHUTTING_DOWN   = 7
    RELOAD_FAILED   = 8


RESULT_TEXT = {
    CommandResult.OK: "ok",
    CommandResult.UNKNOWN_SERVICE: "unknown service",
    CommandResult.UNKNOWN_COMMAND: "unknown command",
    CommandResult.MALFORMED: "malformed request",
    CommandResult.ALREADY: "nothing to do",
    CommandResult.INVALID_STATE: "not possible now",
    CommandResult.LAUNCH_FAILED: "launch failed",
    CommandResult.SHUTTING_DOWN: "manager is shutting down",
    CommandResult.RELOAD_FAILED: "reload failed",
}


class CommandMessage(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("command",     ctypes.c_uint8),      # CommandEnum
        ("ServiceName", ctypes.c_char * 32),
        ("Args",        ctypes.c_char * 32),
    ]


COMMAND_SIZE = ctypes.sizeof(CommandMessage)
assert COMMAND_SIZE == 65, f"Unexpected COMMAND_SIZE={COMMAND_SIZE}"


def _utf8_fit(text: str, max_bytes: int) -> bytes:
    """Encode UTF-8 without splitting a multi-byte character."""
    raw = text.encode("utf-8")
    if len(raw) <= max_bytes:
        return raw
    # Decoding with "ignore" drops only the character the cut split.
    return raw[:max_bytes].decode("utf-8", errors="ignore").encode("utf-8")


def make_command_message(
    command: CommandEnum,
    service_name: str,
    args: str = "",
) -> bytes:
    """Build the binary payload that is sent as the second multipart frame."""
    msg = CommandMessage()
    msg.command = int(command)

    # c_char arrays are C strings: assign bytes without forcing a 32-byte
    # pad (ctypes copies and NUL-terminates). Cap at 31 so the last byte
    # stays NUL — same as the original packer.
    name_b = _utf8_fit(service_name, 31)
    args_b = _utf8_fit(args, 31)
    msg.ServiceName = name_b
    msg.Args = args_b

    return bytes(msg)


# ──────────────────────────────────────────────────────────────────────────────
# Replies (DEALER receives)
# ──────────────────────────────────────────────────────────────────────────────

class CommandReply(ctypes.Structure):
    _pack_ = 8
    _fields_ = [
        ("command",     ctypes.c_uint8),
        ("result",      ctypes.c_uint8),
        ("ServiceName", ctypes.c_char * 32),
        ("Message",     ctypes.c_char * 94),
    ]


REPLY_SIZE = ctypes.sizeof(CommandReply)
assert REPLY_SIZE == 128, f"Unexpected REPLY_SIZE={REPLY_SIZE}"


def parse_command_reply(frames) -> Optional[dict]:
    """The reply in a DEALER message ([""] "BPM" reply), or None for anything else."""
    parts = [bytes(f) for f in frames]
    while parts and not parts[0]:
        parts.pop(0)  # delimiter frames
    if len(parts) != 2 or parts[0] != b"BPM" or len(parts[1]) != REPLY_SIZE:
        return None
    reply = CommandReply.from_buffer_copy(parts[1])
    try:
        command = CommandEnum(reply.command)
    except ValueError:
        command = int(reply.command)
    try:
        result = CommandResult(reply.result)
    except ValueError:
        result = int(reply.result)
    return {
        "command": command,
        "result": result,
        "serviceName": reply.ServiceName.split(b"\x00", 1)[0].decode("utf-8", errors="replace"),
        "message": reply.Message.split(b"\x00", 1)[0].decode("utf-8", errors="replace"),
    }


def reply_succeeded(reply: dict) -> bool:
    return reply["result"] in (CommandResult.OK, CommandResult.ALREADY)


def describe_reply(reply: dict) -> str:
    """One status-bar line, for example "restart vision: ok (restarting)"."""
    command = reply["command"]
    verb = command.name.lower() if isinstance(command, CommandEnum) else f"command {command}"
    result = reply["result"]
    text = RESULT_TEXT.get(result, f"result {int(result)}")
    subject = verb + (f" {reply['serviceName']}" if reply["serviceName"] else "")
    line = f"{subject}: {text}"
    if reply["message"]:
        line += f" ({reply['message']})"
    return line


def make_command_reply(
    command: int,
    result: CommandResult,
    service_name: str,
    message: str = "",
) -> bytes:
    """Build a reply the way the C++ manager does (the mock publisher uses it)."""
    reply = CommandReply()
    reply.command = int(command)
    reply.result = int(result)
    reply.ServiceName = _utf8_fit(service_name, 31)
    reply.Message = _utf8_fit(message, 93)
    return bytes(reply)


# ──────────────────────────────────────────────────────────────────────────────
# Detailed report (SUB, port 6668)
# ──────────────────────────────────────────────────────────────────────────────

REPORT_TOPIC = b"report"
REPORT_MAGIC = 0x524D5042  # the bytes "BPMR"
REPORT_VERSION = 1
MAX_REPORT_RECORDS = 100_000  # the manager's own limit on either count


class ServiceState(IntEnum):
    """The manager's eight states; the health record folds them into RuntimeState."""
    STOPPED   = 0
    WAITING   = 1
    STARTING  = 2
    RUNNING   = 3
    UNHEALTHY = 4
    STOPPING  = 5
    BACKOFF   = 6
    FAILED    = 7

    @classmethod
    def from_byte(cls, value: int) -> "ServiceState":
        try:
            return cls(value)
        except ValueError:
            return cls.STOPPED  # the manager's own fallback when decoding


# States with a live process.
SERVICE_ALIVE_STATES = (
    ServiceState.STARTING, ServiceState.RUNNING, ServiceState.UNHEALTHY, ServiceState.STOPPING,
)


class RestartMode(IntEnum):
    NEVER      = 0
    ON_FAILURE = 1
    ALWAYS     = 2

    @classmethod
    def from_byte(cls, value: int) -> "RestartMode":
        try:
            return cls(value)
        except ValueError:
            return cls.NEVER


RESTART_MODE_TEXT = {
    RestartMode.NEVER: "never",
    RestartMode.ON_FAILURE: "on-failure",
    RestartMode.ALWAYS: "always",
}

# Service record flags (manager/include/DetailedReport.hpp).
SERVICE_FLAG_AUTOSTART = 0x01
SERVICE_FLAG_HEARTBEAT = 0x02
SERVICE_FLAG_CGROUP    = 0x04
SERVICE_FLAG_USAGE     = 0x08
SERVICE_FLAG_GPU       = 0x10
SERVICE_FLAG_REMOVING  = 0x20

# Report header flags.
REPORT_FLAG_CGROUPS  = 0x1
REPORT_FLAG_GPU      = 0x2
REPORT_FLAG_STOPPING = 0x4
REPORT_FLAG_WINDOWS  = 0x8  # exit codes are Windows status codes, never minus a signal


class ReportHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("magic",                ctypes.c_uint32),
        ("version",              ctypes.c_uint16),
        ("headerSize",           ctypes.c_uint16),
        ("serviceRecordSize",    ctypes.c_uint16),
        ("gpuRecordSize",        ctypes.c_uint16),
        ("serviceCount",         ctypes.c_uint32),
        ("gpuCount",             ctypes.c_uint32),
        ("managerPid",           ctypes.c_int32),
        ("snapshotTime",         ctypes.c_int64),   # ns
        ("managerStartTime",     ctypes.c_int64),   # ns
        ("publishIntervalMs",    ctypes.c_uint32),
        ("flags",                ctypes.c_uint32),
        ("hostCpuPercent",       ctypes.c_double),  # 0–100; negative = unknown
        ("memoryTotalBytes",     ctypes.c_uint64),
        ("memoryAvailableBytes", ctypes.c_uint64),
        ("loadAverage1",         ctypes.c_double),
        ("loadAverage5",         ctypes.c_double),
        ("loadAverage15",        ctypes.c_double),
        ("uptimeSeconds",        ctypes.c_uint64),
        ("cpuCount",             ctypes.c_uint32),
        ("reserved",             ctypes.c_uint32),
        ("hostName",             ctypes.c_char * 64),
        ("managerVersion",       ctypes.c_char * 16),
    ]


class ServiceRecord(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("name",             ctypes.c_char * 32),
        ("binary",           ctypes.c_char * 128),
        ("description",      ctypes.c_char * 64),
        ("pid",              ctypes.c_int32),
        ("state",            ctypes.c_uint8),    # ServiceState
        ("restartMode",      ctypes.c_uint8),    # RestartMode
        ("flags",            ctypes.c_uint8),    # SERVICE_FLAG_*
        ("reserved",         ctypes.c_uint8),
        ("restartCount",     ctypes.c_int32),
        ("missedBeats",      ctypes.c_int32),
        ("lastExitCode",     ctypes.c_int32),    # exit code, or minus the signal number
        ("processCount",     ctypes.c_int32),
        ("threadCount",      ctypes.c_int32),
        ("openFiles",        ctypes.c_int32),    # -1 = unknown
        ("startTime",        ctypes.c_int64),    # ns; 0 = never started
        ("lastSeen",         ctypes.c_int64),    # ns
        ("lastExitTime",     ctypes.c_int64),    # ns; 0 = never exited
        ("nextRestartTime",  ctypes.c_int64),    # ns; 0 = none scheduled
        ("cpuTimeUsec",      ctypes.c_uint64),
        ("cpuPercent",       ctypes.c_double),   # 100 = one busy core; negative = unknown
        ("memoryBytes",      ctypes.c_uint64),
        ("memoryPeakBytes",  ctypes.c_uint64),
        ("memoryLimitBytes", ctypes.c_uint64),   # 0 = none
        ("ioReadBytes",      ctypes.c_uint64),
        ("ioWriteBytes",     ctypes.c_uint64),
        ("gpuPercent",       ctypes.c_double),   # summed over GPUs; negative = unknown
        ("gpuMemoryBytes",   ctypes.c_uint64),
        ("oomKills",         ctypes.c_uint32),
        ("cpuLimitPercent",  ctypes.c_uint32),   # 0 = none
    ]


class GpuRecord(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("name",                     ctypes.c_char * 64),
        ("uuid",                     ctypes.c_char * 48),
        ("index",                    ctypes.c_uint32),
        ("temperatureC",             ctypes.c_uint32),  # 0 = unknown
        ("utilizationPercent",       ctypes.c_double),
        ("memoryUtilizationPercent", ctypes.c_double),
        ("memoryTotalBytes",         ctypes.c_uint64),
        ("memoryUsedBytes",          ctypes.c_uint64),
        ("powerMilliwatts",          ctypes.c_uint32),
        ("reserved",                 ctypes.c_uint32),
    ]


REPORT_HEADER_SIZE = ctypes.sizeof(ReportHeader)
SERVICE_RECORD_SIZE = ctypes.sizeof(ServiceRecord)
GPU_RECORD_SIZE = ctypes.sizeof(GpuRecord)
assert REPORT_HEADER_SIZE == 192, f"Unexpected REPORT_HEADER_SIZE={REPORT_HEADER_SIZE}"
assert SERVICE_RECORD_SIZE == 368, f"Unexpected SERVICE_RECORD_SIZE={SERVICE_RECORD_SIZE}"
assert GPU_RECORD_SIZE == 160, f"Unexpected GPU_RECORD_SIZE={GPU_RECORD_SIZE}"


def _text(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def _unknown_if_negative(value: float) -> Optional[float]:
    # NaN and infinities are unknown too: no manager sends them, and one that
    # reached a usage graph would stop the GUI while painting its axis.
    return float(value) if math.isfinite(value) and value >= 0 else None


def service_record_to_dict(record: ServiceRecord) -> dict:
    flags = int(record.flags)
    return {
        "name":             _text(record.name),
        "binary":           _text(record.binary),
        "description":      _text(record.description),
        "pid":              int(record.pid),
        "state":            ServiceState.from_byte(record.state),
        "restartMode":      RestartMode.from_byte(record.restartMode),
        "flags":            flags,
        "autostart":        bool(flags & SERVICE_FLAG_AUTOSTART),
        "heartbeat":        bool(flags & SERVICE_FLAG_HEARTBEAT),
        "cgroup":           bool(flags & SERVICE_FLAG_CGROUP),
        "usageValid":       bool(flags & SERVICE_FLAG_USAGE),
        "gpuValid":         bool(flags & SERVICE_FLAG_GPU),
        "removing":         bool(flags & SERVICE_FLAG_REMOVING),
        "restartCount":     int(record.restartCount),
        "missedBeats":      int(record.missedBeats),
        "lastExitCode":     int(record.lastExitCode),
        "processCount":     int(record.processCount),
        "threadCount":      int(record.threadCount),
        "openFiles":        None if record.openFiles < 0 else int(record.openFiles),
        "startTime":        int(record.startTime),
        "lastSeen":         int(record.lastSeen),
        "lastExitTime":     int(record.lastExitTime),
        "nextRestartTime":  int(record.nextRestartTime),
        "cpuTimeUsec":      int(record.cpuTimeUsec),
        "cpuPercent":       _unknown_if_negative(record.cpuPercent),
        "memoryBytes":      int(record.memoryBytes),
        "memoryPeakBytes":  int(record.memoryPeakBytes),
        "memoryLimitBytes": int(record.memoryLimitBytes),
        "ioReadBytes":      int(record.ioReadBytes),
        "ioWriteBytes":     int(record.ioWriteBytes),
        "gpuPercent":       _unknown_if_negative(record.gpuPercent),
        "gpuMemoryBytes":   int(record.gpuMemoryBytes),
        "oomKills":         int(record.oomKills),
        "cpuLimitPercent":  int(record.cpuLimitPercent),
    }


def gpu_record_to_dict(record: GpuRecord) -> dict:
    return {
        "name":                     _text(record.name),
        "uuid":                     _text(record.uuid),
        "index":                    int(record.index),
        "temperatureC":             int(record.temperatureC),
        "utilizationPercent":       _unknown_if_negative(record.utilizationPercent),
        "memoryUtilizationPercent": _unknown_if_negative(record.memoryUtilizationPercent),
        "memoryTotalBytes":         int(record.memoryTotalBytes),
        "memoryUsedBytes":          int(record.memoryUsedBytes),
        "powerMilliwatts":          int(record.powerMilliwatts),
    }


def parse_detailed_report(payload: bytes) -> dict:
    """Decode the payload frame that follows the ``report`` topic.

    The sizes in the header win over this module's: a newer manager that
    appends fields still parses, and its extra bytes are skipped.
    Raises ValueError for anything that is not a version-1 report.
    """
    if len(payload) < REPORT_HEADER_SIZE:
        raise ValueError(f"report payload too short: {len(payload)} bytes, header is {REPORT_HEADER_SIZE}")
    header = ReportHeader.from_buffer_copy(payload)
    if header.magic != REPORT_MAGIC:
        raise ValueError(f"not a report: magic 0x{int(header.magic):08X}, expected 0x{REPORT_MAGIC:08X}")
    if header.version != REPORT_VERSION:
        raise ValueError(f"report version {int(header.version)}, this reader knows {REPORT_VERSION}")
    header_size = int(header.headerSize)
    record_size = int(header.serviceRecordSize)
    gpu_size = int(header.gpuRecordSize)
    if header_size < REPORT_HEADER_SIZE or record_size < SERVICE_RECORD_SIZE or gpu_size < GPU_RECORD_SIZE:
        raise ValueError(
            f"report layout too small: header {header_size}, service {record_size}, gpu {gpu_size}"
        )
    service_count = int(header.serviceCount)
    gpu_count = int(header.gpuCount)
    if service_count > MAX_REPORT_RECORDS or gpu_count > MAX_REPORT_RECORDS:
        raise ValueError(f"report counts out of range: {service_count} services, {gpu_count} GPUs")
    expected = header_size + service_count * record_size + gpu_count * gpu_size
    if len(payload) != expected:
        raise ValueError(f"report payload is {len(payload)} bytes, the header announces {expected}")

    offset = header_size
    services = []
    for _ in range(service_count):
        services.append(service_record_to_dict(ServiceRecord.from_buffer_copy(payload, offset)))
        offset += record_size
    gpus = []
    for _ in range(gpu_count):
        gpus.append(gpu_record_to_dict(GpuRecord.from_buffer_copy(payload, offset)))
        offset += gpu_size

    flags = int(header.flags)
    return {
        "managerPid":           int(header.managerPid),
        "snapshotTime":         int(header.snapshotTime),
        "managerStartTime":     int(header.managerStartTime),
        "publishIntervalMs":    int(header.publishIntervalMs),
        "flags":                flags,
        "cgroups":              bool(flags & REPORT_FLAG_CGROUPS),
        "gpuMonitoring":        bool(flags & REPORT_FLAG_GPU),
        "stopping":             bool(flags & REPORT_FLAG_STOPPING),
        "windows":              bool(flags & REPORT_FLAG_WINDOWS),
        "hostCpuPercent":       _unknown_if_negative(header.hostCpuPercent),
        "memoryTotalBytes":     int(header.memoryTotalBytes),
        "memoryAvailableBytes": int(header.memoryAvailableBytes),
        "loadAverage":          (float(header.loadAverage1), float(header.loadAverage5), float(header.loadAverage15)),
        "uptimeSeconds":        int(header.uptimeSeconds),
        "cpuCount":             int(header.cpuCount),
        "hostName":             _text(header.hostName),
        "managerVersion":       _text(header.managerVersion),
        "services":             services,
        "gpus":                 gpus,
    }


def parse_report_frames(frames) -> Optional[dict]:
    """The report in a SUB message (``report`` + payload), or None for another topic.
    A payload that is not a report raises ValueError."""
    parts = [bytes(f) for f in frames]
    if len(parts) != 2 or parts[0] != REPORT_TOPIC:
        return None
    return parse_detailed_report(parts[1])


def _flags_from(source: dict, table) -> int:
    """``flags`` when the dict has it, otherwise composed from the booleans in ``table``."""
    if "flags" in source:
        return int(source["flags"])
    flags = 0
    for key, bit in table:
        if source.get(key):
            flags |= bit
    return flags


_SERVICE_FLAG_KEYS = (
    ("autostart", SERVICE_FLAG_AUTOSTART), ("heartbeat", SERVICE_FLAG_HEARTBEAT),
    ("cgroup", SERVICE_FLAG_CGROUP), ("usageValid", SERVICE_FLAG_USAGE),
    ("gpuValid", SERVICE_FLAG_GPU), ("removing", SERVICE_FLAG_REMOVING),
)
_REPORT_FLAG_KEYS = (
    ("cgroups", REPORT_FLAG_CGROUPS), ("gpuMonitoring", REPORT_FLAG_GPU),
    ("stopping", REPORT_FLAG_STOPPING), ("windows", REPORT_FLAG_WINDOWS),
)


def _optional(value, unknown):
    return unknown if value is None else value


def encode_detailed_report(report: dict) -> bytes:
    """Build the payload frame the way the C++ manager does, from the dict
    shape parse_detailed_report returns (missing keys take their zero value).
    The mock publisher and the tests use it."""
    services = report.get("services", [])
    gpus = report.get("gpus", [])
    header = ReportHeader()
    header.magic = REPORT_MAGIC
    header.version = REPORT_VERSION
    header.headerSize = REPORT_HEADER_SIZE
    header.serviceRecordSize = SERVICE_RECORD_SIZE
    header.gpuRecordSize = GPU_RECORD_SIZE
    header.serviceCount = len(services)
    header.gpuCount = len(gpus)
    header.managerPid = int(report.get("managerPid", 0))
    header.snapshotTime = int(report.get("snapshotTime", 0))
    header.managerStartTime = int(report.get("managerStartTime", 0))
    header.publishIntervalMs = int(report.get("publishIntervalMs", 0))
    header.flags = _flags_from(report, _REPORT_FLAG_KEYS)
    header.hostCpuPercent = float(_optional(report.get("hostCpuPercent"), -1.0))
    header.memoryTotalBytes = int(report.get("memoryTotalBytes", 0))
    header.memoryAvailableBytes = int(report.get("memoryAvailableBytes", 0))
    load = report.get("loadAverage", (0.0, 0.0, 0.0))
    header.loadAverage1, header.loadAverage5, header.loadAverage15 = (float(v) for v in load)
    header.uptimeSeconds = int(report.get("uptimeSeconds", 0))
    header.cpuCount = int(report.get("cpuCount", 0))
    header.hostName = _utf8_fit(report.get("hostName", ""), 63)
    header.managerVersion = _utf8_fit(report.get("managerVersion", ""), 15)

    parts = [bytes(header)]
    for service in services:
        record = ServiceRecord()
        record.name = _utf8_fit(service.get("name", ""), 31)
        record.binary = _utf8_fit(service.get("binary", ""), 127)
        record.description = _utf8_fit(service.get("description", ""), 63)
        record.pid = int(service.get("pid", 0))
        record.state = int(service.get("state", ServiceState.STOPPED))
        record.restartMode = int(service.get("restartMode", RestartMode.NEVER))
        record.flags = _flags_from(service, _SERVICE_FLAG_KEYS)
        record.restartCount = int(service.get("restartCount", 0))
        record.missedBeats = int(service.get("missedBeats", 0))
        record.lastExitCode = int(service.get("lastExitCode", 0))
        record.processCount = int(service.get("processCount", 0))
        record.threadCount = int(service.get("threadCount", 0))
        record.openFiles = int(_optional(service.get("openFiles"), -1))
        record.startTime = int(service.get("startTime", 0))
        record.lastSeen = int(service.get("lastSeen", 0))
        record.lastExitTime = int(service.get("lastExitTime", 0))
        record.nextRestartTime = int(service.get("nextRestartTime", 0))
        record.cpuTimeUsec = int(service.get("cpuTimeUsec", 0))
        record.cpuPercent = float(_optional(service.get("cpuPercent"), -1.0))
        record.memoryBytes = int(service.get("memoryBytes", 0))
        record.memoryPeakBytes = int(service.get("memoryPeakBytes", 0))
        record.memoryLimitBytes = int(service.get("memoryLimitBytes", 0))
        record.ioReadBytes = int(service.get("ioReadBytes", 0))
        record.ioWriteBytes = int(service.get("ioWriteBytes", 0))
        record.gpuPercent = float(_optional(service.get("gpuPercent"), -1.0))
        record.gpuMemoryBytes = int(service.get("gpuMemoryBytes", 0))
        record.oomKills = int(service.get("oomKills", 0))
        record.cpuLimitPercent = int(service.get("cpuLimitPercent", 0))
        parts.append(bytes(record))
    for gpu in gpus:
        record = GpuRecord()
        record.name = _utf8_fit(gpu.get("name", ""), 63)
        record.uuid = _utf8_fit(gpu.get("uuid", ""), 47)
        record.index = int(gpu.get("index", 0))
        record.temperatureC = int(gpu.get("temperatureC", 0))
        record.utilizationPercent = float(_optional(gpu.get("utilizationPercent"), -1.0))
        record.memoryUtilizationPercent = float(_optional(gpu.get("memoryUtilizationPercent"), -1.0))
        record.memoryTotalBytes = int(gpu.get("memoryTotalBytes", 0))
        record.memoryUsedBytes = int(gpu.get("memoryUsedBytes", 0))
        record.powerMilliwatts = int(gpu.get("powerMilliwatts", 0))
        parts.append(bytes(record))
    return b"".join(parts)


# The names the manager prints (manager/src/ProcessLauncher.cpp, SignalName).
SIGNAL_NAMES = {
    1: "HUP", 2: "INT", 3: "QUIT", 4: "ILL", 6: "ABRT", 7: "BUS", 8: "FPE",
    9: "KILL", 11: "SEGV", 13: "PIPE", 14: "ALRM", 15: "TERM",
}
MAX_SIGNAL_NUMBER = 64


def exit_text(code: int, windows: bool = False) -> str:
    """The manager's wording for a last exit: ``exit 3``, ``signal 9 (KILL)``,
    or ``exit 0xC0000005`` for a Windows status code."""
    if code >= 0:
        return f"exit {code}"
    if not windows and -code <= MAX_SIGNAL_NUMBER:
        name = SIGNAL_NAMES.get(-code)
        return f"signal {-code}" + (f" ({name})" if name else "")
    return f"exit 0x{code & 0xFFFFFFFF:08X}"


def format_duration_short(ns: int) -> str:
    """The CLI's coarse duration: ``11s``, ``3m 05s``, ``2h 07m``, ``3d 04h``."""
    seconds = int(max(ns, 0) // 1_000_000_000)
    if seconds < 60:
        return f"{seconds}s"
    if seconds < 3600:
        return f"{seconds // 60}m {seconds % 60:02d}s"
    if seconds < 86400:
        return f"{seconds // 3600}h {(seconds // 60) % 60:02d}m"
    return f"{seconds // 86400}d {(seconds // 3600) % 24:02d}h"


def service_state_text(service: dict, snapshot_ns: int) -> str:
    """The state the CLI prints: ``backoff 2s``, ``running (2 missed)``, ``failed``."""
    state: ServiceState = service["state"]
    text = state.name.lower()
    next_restart = int(service.get("nextRestartTime", 0) or 0)
    if state == ServiceState.BACKOFF and next_restart > snapshot_ns:
        text += " " + format_duration_short(next_restart - snapshot_ns)
    missed = int(service.get("missedBeats", 0) or 0)
    if missed > 0 and state in SERVICE_ALIVE_STATES:
        text += f" ({missed} missed)"
    if service.get("removing"):
        text += " (removing)"
    return text
