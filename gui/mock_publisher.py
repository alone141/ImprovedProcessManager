#!/usr/bin/env python3
"""
Mock process manager for testing the GUI without berayprocessmanager.

PUB    (bind) – the health report: an array of DetailedHealthReport (128 B each)
PUB    (bind) – the detailed report: frames b"report" + payload (docs/protocol.md)
ROUTER (bind) – accepts DEALER commands with:
      identity = b"PMC"
      frame0   = b"BPM"
      frame1   = packed CommandMessage (65 B)
  and answers each with b"BPM" + CommandReply (128 B), like the C++ manager.

The simulation lives in MockManager, so tests can drive it without sockets.
"""

from __future__ import annotations

import argparse
import ctypes
import os
import random
import signal
import socket
import sys
import time
from typing import Dict, List, Optional, Tuple

import zmq

from health_structs import (
    COMMAND_SIZE,
    REPORT_SIZE,
    REPORT_TOPIC,
    CommandEnum,
    CommandMessage,
    CommandResult,
    DetailedHealthReport,
    RestartMode,
    RuntimeState,
    ServiceState,
    encode_detailed_report,
    make_command_reply,
)

SEC = 1_000_000_000
MB = 1024**2
GB = 1024**3

PROCESSES = [
    "sensor_fusion",
    "path_planner",
    "vision_pipeline",
    "control_loop",
    "telemetry_bridge",
    "logger_daemon",
]
DESCRIPTIONS = {
    "sensor_fusion": "Fuses IMU, GPS and odometry",
    "path_planner": "Plans the route",
    "vision_pipeline": "Camera capture and inference",
    "control_loop": "Closes the control loop",
    "telemetry_bridge": "Forwards telemetry",
    "logger_daemon": "Collects logs from every sensor",
}
# Services the mock shows with GPU figures, heartbeats and limits.
GPU_USERS = ("vision_pipeline", "sensor_fusion")
HEARTBEAT_USERS = ("vision_pipeline", "control_loop")
MEMORY_LIMITS = {"sensor_fusion": GB, "vision_pipeline": 2 * GB}
CPU_LIMITS = {"sensor_fusion": 200}
EXIT_CODES = (1, 3, -9, -11)

# The GUI's five states to the manager's eight (the report's state byte).
MANAGER_STATE = {
    RuntimeState.UNKNOWN: ServiceState.STOPPED,
    RuntimeState.STARTING: ServiceState.STARTING,
    RuntimeState.RUNNING: ServiceState.RUNNING,
    RuntimeState.STOPPED: ServiceState.STOPPED,
    RuntimeState.UNHEALTHY: ServiceState.UNHEALTHY,
}
ALIVE = (RuntimeState.STARTING, RuntimeState.RUNNING, RuntimeState.UNHEALTHY)


def make_report(
    name: str,
    pid: int,
    state: RuntimeState,
    mem: int,
    cpu_usec: int,
    start_ns: int,
    last_seen_ns: int,
    missed: int,
    restarts: int,
    snap_ns: int,
) -> DetailedHealthReport:
    r = DetailedHealthReport()
    name_bytes = name.encode("utf-8")[:63]
    r.processName = name_bytes + b"\x00" * (64 - len(name_bytes))
    r.pid = pid
    r.memoryUsageInBytes = mem
    r.cpuUsageInUsec = cpu_usec
    r.runtimeHealth.state = int(state)
    r.runtimeHealth.start_time = start_ns
    r.runtimeHealth.lastSeen = last_seen_ns
    r.runtimeHealth.missedBeats = missed
    r.runtimeHealth.restartCount = restarts
    r.snapshotTime = snap_ns
    return r


def reports_to_bytes(reports: List[DetailedHealthReport]) -> bytes:
    size = len(reports) * REPORT_SIZE
    buf = (ctypes.c_char * size)()
    for i, rep in enumerate(reports):
        ctypes.memmove(
            ctypes.addressof(buf) + i * REPORT_SIZE,
            ctypes.addressof(rep),
            REPORT_SIZE,
        )
    return bytes(buf)


class MockManager:
    """Simulated services: states that drift, counters that grow, commands
    that are answered, and the two reports the real manager publishes."""

    def __init__(
        self,
        names: List[str] = PROCESSES,
        rng: Optional[random.Random] = None,
        now_ns: Optional[int] = None,
        interval_ms: int = 500,
    ):
        self.rng = rng or random.Random()
        now = time.time_ns() if now_ns is None else now_ns
        self.names = list(names)
        self.interval_ms = interval_ms
        self.started_ns = now
        self.last_snap = now
        self.state: Dict[str, RuntimeState] = {n: RuntimeState.RUNNING for n in self.names}
        self.pid: Dict[str, int] = {n: 10000 + i for i, n in enumerate(self.names)}
        self.start_ns: Dict[str, int] = {
            n: now - self.rng.randint(60, 3600) * SEC for n in self.names
        }
        self.cpu_usec: Dict[str, int] = {n: self.rng.randint(1_000_000, 50_000_000) for n in self.names}
        self.cpu_pct: Dict[str, float] = {n: 0.0 for n in self.names}
        self.mem: Dict[str, int] = {n: self.rng.randint(20, 400) * MB for n in self.names}
        self.mem_peak: Dict[str, int] = dict(self.mem)
        self.restarts: Dict[str, int] = {n: self.rng.randint(0, 3) for n in self.names}
        self.missed: Dict[str, int] = {n: 0 for n in self.names}
        self.last_seen: Dict[str, int] = {n: now for n in self.names}
        self.threads: Dict[str, int] = {n: self.rng.randint(2, 40) for n in self.names}
        self.files: Dict[str, int] = {n: self.rng.randint(5, 120) for n in self.names}
        self.io_read: Dict[str, int] = {n: self.rng.randint(1, 500) * MB for n in self.names}
        self.io_write: Dict[str, int] = {n: self.rng.randint(1, 100) * MB for n in self.names}
        self.gpu: Dict[str, Tuple[float, int]] = {
            n: (self.rng.uniform(5, 60), self.rng.randint(100, 900) * MB)
            for n in self.names if n in GPU_USERS
        }
        self.last_exit: Dict[str, Optional[Tuple[int, int]]] = {n: None for n in self.names}
        self.host_cpu = self.rng.uniform(5, 40)
        self.host_memory_total = 16 * GB
        self.host_memory_available = self.rng.randint(4, 10) * GB
        self.host_started_ns = now - self.rng.randint(1, 30) * 86400 * SEC

    # ── commands ──────────────────────────────────────────────────────────

    def handle(self, command, name: str, now_ns: int) -> Tuple[CommandResult, str]:
        """Apply a command the way the manager would; (result, note) for the reply."""
        if name not in self.state:
            return CommandResult.UNKNOWN_SERVICE, f"no service named {name}"
        if command == CommandEnum.START:
            self._start(name, now_ns)
            return CommandResult.OK, "starting"
        if command == CommandEnum.STOP:
            self._stop(name, now_ns, -15)
            return CommandResult.OK, "stopped"
        if command == CommandEnum.RESTART:
            self._stop(name, now_ns, -15)
            self.restarts[name] += 1
            self._start(name, now_ns)
            return CommandResult.OK, "restarting"
        return CommandResult.UNKNOWN_COMMAND, f"unknown command {int(command)}"

    def _start(self, name: str, now_ns: int) -> None:
        self.state[name] = RuntimeState.STARTING
        self.missed[name] = 0

    def _stop(self, name: str, now_ns: int, code: int) -> None:
        if self.state[name] in ALIVE:
            self.last_exit[name] = (code, now_ns)
        self.state[name] = RuntimeState.STOPPED
        self.pid[name] = 0

    # ── simulation ────────────────────────────────────────────────────────

    def advance(self, now_ns: int) -> None:
        """One tick: random state changes and growing counters."""
        rng = self.rng
        for name in self.names:
            if self.state[name] == RuntimeState.STARTING and rng.random() < 0.3:
                self.state[name] = RuntimeState.RUNNING
                self.pid[name] = rng.randint(10000, 20000)
                self.start_ns[name] = now_ns  # restarts were counted at the command
                self.threads[name] = rng.randint(2, 40)
            elif self.state[name] == RuntimeState.RUNNING and rng.random() < 0.01:
                self.state[name] = RuntimeState.UNHEALTHY
                self.missed[name] += 1
            elif self.state[name] == RuntimeState.UNHEALTHY and rng.random() < 0.05:
                self._stop(name, now_ns, rng.choice(EXIT_CODES))

        elapsed_us = (now_ns - self.last_snap) / 1000.0
        for name in self.names:
            alive = self.state[name] in ALIVE
            load = rng.uniform(0.05, 0.6) if self.state[name] == RuntimeState.RUNNING else 0.0
            self.cpu_usec[name] += int(elapsed_us * load)
            self.cpu_pct[name] = load * 100.0
            if alive:
                self.mem[name] = max(10 * MB, self.mem[name] + rng.randint(-2 * MB, 2 * MB))
                self.mem_peak[name] = max(self.mem_peak[name], self.mem[name])
                self.io_read[name] += rng.randint(0, 2 * MB)
                self.io_write[name] += rng.randint(0, MB // 2)
                self.files[name] = max(3, self.files[name] + rng.randint(-2, 2))
                if name in self.gpu:
                    util, vram = self.gpu[name]
                    self.gpu[name] = (
                        min(100.0, max(0.0, util + rng.uniform(-5, 5))),
                        max(50 * MB, vram + rng.randint(-10 * MB, 10 * MB)),
                    )
            if self.state[name] == RuntimeState.RUNNING:
                self.last_seen[name] = now_ns - rng.randint(0, 200_000_000)
        self.host_cpu = min(100.0, max(0.0, self.host_cpu + rng.uniform(-3, 3)))
        self.host_memory_available = min(
            self.host_memory_total,
            max(GB, self.host_memory_available + rng.randint(-100 * MB, 100 * MB)),
        )
        self.last_snap = now_ns

    # ── the two reports ───────────────────────────────────────────────────

    def health_reports(self, now_ns: int) -> List[DetailedHealthReport]:
        reports = []
        for name in self.names:
            last_seen = self.last_seen[name]
            if self.state[name] in (RuntimeState.UNKNOWN, RuntimeState.STOPPED, RuntimeState.UNHEALTHY):
                last_seen = self.start_ns[name]
            reports.append(
                make_report(
                    name=name,
                    pid=self.pid[name],
                    state=self.state[name],
                    mem=self.mem[name],
                    cpu_usec=self.cpu_usec[name],
                    start_ns=self.start_ns[name],
                    last_seen_ns=last_seen,
                    missed=self.missed[name],
                    restarts=self.restarts[name],
                    snap_ns=now_ns,
                )
            )
        return reports

    def detailed_report(self, now_ns: int) -> dict:
        """The detailed report as the dict encode_detailed_report takes."""
        services = []
        for name in self.names:
            state = self.state[name]
            alive = state in ALIVE
            exit_code, exit_time = self.last_exit[name] or (0, 0)
            util, vram = self.gpu.get(name, (None, 0))
            services.append({
                "name": name,
                "binary": f"/opt/beray/bin/{name}",
                "description": DESCRIPTIONS.get(name, ""),
                "pid": self.pid[name] if alive else 0,
                "state": MANAGER_STATE[state],
                "restartMode": RestartMode.ON_FAILURE,
                "autostart": True,
                "heartbeat": name in HEARTBEAT_USERS,
                "cgroup": True,
                "usageValid": alive,
                "gpuValid": alive and name in self.gpu,
                "restartCount": self.restarts[name],
                "missedBeats": self.missed[name],
                "lastExitCode": exit_code,
                "processCount": 1 if alive else 0,
                "threadCount": self.threads[name] if alive else 0,
                "openFiles": self.files[name] if alive else None,
                "startTime": self.start_ns[name] if alive else 0,
                "lastSeen": self.last_seen[name],
                "lastExitTime": exit_time,
                "nextRestartTime": 0,
                "cpuTimeUsec": self.cpu_usec[name],
                "cpuPercent": self.cpu_pct[name] if alive else None,
                "memoryBytes": self.mem[name] if alive else 0,
                "memoryPeakBytes": self.mem_peak[name],
                "memoryLimitBytes": MEMORY_LIMITS.get(name, 0),
                "ioReadBytes": self.io_read[name],
                "ioWriteBytes": self.io_write[name],
                "gpuPercent": util if alive else None,
                "gpuMemoryBytes": vram if alive else 0,
                "oomKills": 0,
                "cpuLimitPercent": CPU_LIMITS.get(name, 0),
            })
        gpu_busy = sum(util for name, (util, _vram) in self.gpu.items() if self.state[name] in ALIVE)
        gpu_used = sum(vram for name, (_util, vram) in self.gpu.items() if self.state[name] in ALIVE)
        gpus = [{
            "name": "Mock GPU 0",
            "uuid": "GPU-00000000-mock-0000-0000-000000000000",
            "index": 0,
            "temperatureC": 45 + int(gpu_busy / 5),
            "utilizationPercent": min(100.0, gpu_busy),
            "memoryUtilizationPercent": min(100.0, gpu_busy / 2),
            "memoryTotalBytes": 8 * GB,
            "memoryUsedBytes": min(8 * GB, gpu_used + 500 * MB),
            "powerMilliwatts": 30_000 + int(gpu_busy * 1_500),
        }]
        return {
            "managerPid": os.getpid(),
            "snapshotTime": now_ns,
            "managerStartTime": self.started_ns,
            "publishIntervalMs": self.interval_ms,
            "cgroups": True,
            "gpuMonitoring": True,
            "stopping": False,
            "windows": False,
            "hostCpuPercent": self.host_cpu,
            "memoryTotalBytes": self.host_memory_total,
            "memoryAvailableBytes": self.host_memory_available,
            "loadAverage": (self.host_cpu / 20, self.host_cpu / 25, self.host_cpu / 30),
            "uptimeSeconds": max(0, (now_ns - self.host_started_ns) // SEC),
            "cpuCount": os.cpu_count() or 4,
            "hostName": socket.gethostname(),
            "managerVersion": "mock",
            "services": services,
            "gpus": gpus,
        }

    def health_frame(self, now_ns: int) -> bytes:
        return reports_to_bytes(self.health_reports(now_ns))

    def report_frames(self, now_ns: int) -> List[bytes]:
        return [REPORT_TOPIC, encode_detailed_report(self.detailed_report(now_ns))]


def parse_command(frames: List[bytes]) -> Tuple[bytes, Optional[bytes], Optional[bytes]]:
    """(identity, topic, payload) of a ROUTER message; the delimiter frames
    some stacks add are skipped."""
    identity = frames[0]
    topic = payload = None
    for f in frames[1:]:
        if f == b"BPM":
            topic = f
        elif len(f) == COMMAND_SIZE:
            payload = f
    return identity, topic, payload


def main():
    # Arrows in the log must not fail when the output goes to a cp1252 file or pipe.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser(description="Mock process manager for the GUI")
    parser.add_argument("--pub", default="tcp://*:6667", help="health report PUB (default tcp://*:6667)")
    parser.add_argument("--report", default="tcp://*:6668", help="detailed report PUB (default tcp://*:6668)")
    parser.add_argument("--router", default="tcp://*:5557", help="command ROUTER (default tcp://*:5557)")
    parser.add_argument("--interval", type=float, default=0.5, help="seconds between reports")
    args = parser.parse_args()

    ctx = zmq.Context()

    pub = ctx.socket(zmq.PUB)
    pub.bind(args.pub)
    print(f"[PUB]    bound to {args.pub}  (health report)")

    report_pub = ctx.socket(zmq.PUB)
    report_pub.bind(args.report)
    print(f"[PUB]    bound to {args.report}  (detailed report, topic 'report')")

    router = ctx.socket(zmq.ROUTER)
    router.bind(args.router)
    print(f"[ROUTER] bound to {args.router}")
    print("Expecting DEALER identity = 'PMC', frames: 'BPM' + CommandMessage(65 B)\n")

    manager = MockManager(interval_ms=int(args.interval * 1000))
    running = True

    def handle_sig(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    poller = zmq.Poller()
    poller.register(router, zmq.POLLIN)

    print("Publishing health and detailed reports… (Ctrl+C to stop)\n")

    while running:
        # ── handle commands ──────────────────────────────────────────────
        try:
            events = dict(poller.poll(10))
        except zmq.ZMQError:
            break

        if router in events:
            try:
                frames = router.recv_multipart(flags=zmq.NOBLOCK)
                if len(frames) < 2:
                    continue
                identity, topic, payload = parse_command(frames)
                print(f"← from identity={identity!r}  topic={topic!r}  "
                      f"payload={len(payload) if payload else 0} B")
                if identity != b"PMC":
                    print(f"   WARNING: expected identity 'PMC', got {identity!r}")
                if topic != b"BPM":
                    print(f"   WARNING: expected topic 'BPM', got {topic!r}")

                if payload and len(payload) == COMMAND_SIZE:
                    cmd_msg = CommandMessage.from_buffer_copy(payload)
                    try:
                        cmd = CommandEnum(cmd_msg.command)
                    except ValueError:
                        cmd = cmd_msg.command
                    svc = cmd_msg.ServiceName.split(b"\x00", 1)[0].decode(errors="replace")
                    cmd_args = cmd_msg.Args.split(b"\x00", 1)[0].decode(errors="replace")
                    print(f"   CMD={cmd}  ServiceName={svc!r}  Args={cmd_args!r}")
                    result, note = manager.handle(cmd, svc, time.time_ns())
                    print(f"   → {result.name.lower()} ({note})")
                    # Answer like the C++ manager: [identity] "BPM" CommandReply.
                    router.send_multipart(
                        [identity, b"BPM", make_command_reply(int(cmd), result, svc, note)]
                    )
                else:
                    print(f"   payload size mismatch "
                          f"(got {len(payload) if payload else 0}, expected {COMMAND_SIZE})")
                    if payload:
                        print(f"   hex: {payload[:32].hex(' ')}…")
            except zmq.Again:
                pass
            except Exception as e:
                print(f"   command handling error: {e}")

        # ── simulate, then publish both reports ──────────────────────────
        now = time.time_ns()
        manager.advance(now)
        pub.send(manager.health_frame(now))
        report_pub.send_multipart(manager.report_frames(now))

        time.sleep(args.interval)

    print("\nShutting down…")
    pub.close(0)
    report_pub.close(0)
    router.close(0)
    ctx.term()


if __name__ == "__main__":
    main()
