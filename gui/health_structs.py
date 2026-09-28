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
    uint8_t command;          // CommandEnum
    char    ServiceName[32];
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

The full protocol, including the detailed report on port 6668, is described
in docs/protocol.md at the top of the repository.
"""

from __future__ import annotations

import ctypes
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
    START   = 78
    STOP    = 79
    RESTART = 81


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
    line = f"{verb} {reply['serviceName']}: {text}".replace("  ", " ")
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
