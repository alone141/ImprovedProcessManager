#!/usr/bin/env python3
"""
Mock process manager for testing the GUI without berayprocessmanager.

PUB    (bind) – the health report: an array of DetailedHealthReport (128 B each)
PUB    (bind) – the detailed report: frames b"report" + payload (docs/protocol.md)
ROUTER (bind) – accepts DEALER commands with:
      identity = b"PMC"
      frame0   = b"BPM"
      frame1   = packed CommandMessage (65 B)
  and answers each with b"BPM" + CommandReply (128 B), like the C++ manager:
  start / stop / restart of one service or of "*" (every service), heartbeat,
  and reload (91), which the mock only counts.

The services go through the manager's eight states: a crash is restarted
after a doubling delay (backoff) until the restart budget is used up (failed),
a stop passes through stopping, and path_planner waits for sensor_fusion.
The health record carries them folded into its five, as the manager does.

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
    ALL_SERVICES,
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
# A service that waits for another, so waiting (and failing along) shows up.
DEPENDS_ON = {"path_planner": ("sensor_fusion",)}

# The manager's defaults (manager/README.md): the restart delay doubles from
# 1 s up to 30 s, and 5 automatic restarts within 60 s make a service failed.
RESTART_DELAY_NS = 1 * SEC
RESTART_DELAY_MAX_NS = 30 * SEC
MAX_RESTARTS = 5
RESTART_WINDOW_NS = 60 * SEC

# States with a live process, and so a PID and figures.
ALIVE = (ServiceState.STARTING, ServiceState.RUNNING, ServiceState.UNHEALTHY, ServiceState.STOPPING)

# The manager's eight states folded into the health record's five (docs/protocol.md).
HEALTH_STATE = {
    ServiceState.STOPPED: RuntimeState.STOPPED,
    ServiceState.STOPPING: RuntimeState.STOPPED,
    ServiceState.WAITING: RuntimeState.STARTING,
    ServiceState.STARTING: RuntimeState.STARTING,
    ServiceState.BACKOFF: RuntimeState.STARTING,
    ServiceState.RUNNING: RuntimeState.RUNNING,
    ServiceState.UNHEALTHY: RuntimeState.UNHEALTHY,
    ServiceState.FAILED: RuntimeState.UNHEALTHY,
}


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
        self.state: Dict[str, ServiceState] = {n: ServiceState.RUNNING for n in self.names}
        self.next_restart: Dict[str, int] = {n: 0 for n in self.names}  # while in backoff
        # Automatic restarts within the restart window: the budget.
        self.restart_times: Dict[str, List[int]] = {n: [] for n in self.names}
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
        self.reloads = 0

    # ── commands ──────────────────────────────────────────────────────────

    def handle(self, command, name: str, now_ns: int) -> Tuple[CommandResult, str]:
        """Apply a command the way the manager would; (result, note) for the reply."""
        if command == CommandEnum.RELOAD:
            self.reloads += 1
            return CommandResult.OK, "0 added, 0 removed, 0 changed"
        if command not in (CommandEnum.START, CommandEnum.STOP, CommandEnum.RESTART):
            if command == CommandEnum.HEARTBEAT and name in self.state:
                self.last_seen[name] = now_ns
                return CommandResult.OK, ""
            return CommandResult.UNKNOWN_COMMAND, f"unknown command {int(command)}"
        if name == ALL_SERVICES:
            for each in self.names:
                self._apply(command, each, now_ns)
            return CommandResult.OK, f"{command.name.lower()} sent to {len(self.names)} services"
        if name not in self.state:
            return CommandResult.UNKNOWN_SERVICE, f"no service named {name}"
        return CommandResult.OK, self._apply(command, name, now_ns)

    def _apply(self, command: CommandEnum, name: str, now_ns: int) -> str:
        if command == CommandEnum.START:
            self.restart_times[name].clear()  # a start by hand resets the budget
            return self._start(name, now_ns)
        if command == CommandEnum.STOP:
            return self._stop(name, now_ns, -15)
        self._stop(name, now_ns, -15)
        self.restarts[name] += 1
        self.restart_times[name].clear()
        self._start(name, now_ns)
        return "restarting"

    def _start(self, name: str, now_ns: int) -> str:
        """Start now, or wait while a dependency is not running."""
        self.missed[name] = 0
        waiting_for = [d for d in DEPENDS_ON.get(name, ()) if self.state[d] != ServiceState.RUNNING]
        if waiting_for:
            self.state[name] = ServiceState.WAITING
            self.pid[name] = 0
            return "waiting for " + ", ".join(waiting_for)
        self._launch(name, now_ns)
        return "starting"

    def _launch(self, name: str, now_ns: int) -> None:
        self.state[name] = ServiceState.STARTING
        self.pid[name] = self.rng.randint(10000, 20000)
        self.start_ns[name] = now_ns
        self.threads[name] = self.rng.randint(2, 40)

    def _stop(self, name: str, now_ns: int, code: int) -> str:
        """A live process exits at the next tick (stopping); anything else
        (waiting, backoff, failed) stops at once."""
        if self.state[name] == ServiceState.STOPPING:
            return "stopping"
        if self.state[name] in ALIVE:
            self.last_exit[name] = (code, now_ns)
            self.state[name] = ServiceState.STOPPING
            return "stopping"
        self.state[name] = ServiceState.STOPPED
        self.pid[name] = 0
        return "stopped"

    def crash(self, name: str, now_ns: int, code: int) -> None:
        """The process exited on its own: restart it after a delay that doubles
        with each restart in the window, or give up once the budget is used."""
        self.last_exit[name] = (code, now_ns)
        self.pid[name] = 0
        recent = [t for t in self.restart_times[name] if now_ns - t <= RESTART_WINDOW_NS]
        self.restart_times[name] = recent
        if len(recent) >= MAX_RESTARTS:
            self.state[name] = ServiceState.FAILED
            return
        self.state[name] = ServiceState.BACKOFF
        self.next_restart[name] = now_ns + min(RESTART_DELAY_NS * 2 ** len(recent), RESTART_DELAY_MAX_NS)

    # ── simulation ────────────────────────────────────────────────────────

    def advance(self, now_ns: int) -> None:
        """One tick: random state changes, due restarts and growing counters."""
        rng = self.rng
        for name in self.names:
            state = self.state[name]
            if state == ServiceState.STARTING and rng.random() < 0.3:
                self.state[name] = ServiceState.RUNNING
            elif state == ServiceState.RUNNING and rng.random() < 0.01:
                self.state[name] = ServiceState.UNHEALTHY
                self.missed[name] += 1
            elif state == ServiceState.UNHEALTHY and rng.random() < 0.05:
                self.crash(name, now_ns, rng.choice(EXIT_CODES))
            elif state == ServiceState.STOPPING:
                self.state[name] = ServiceState.STOPPED
                self.pid[name] = 0
            elif state == ServiceState.BACKOFF and now_ns >= self.next_restart[name]:
                self.restart_times[name].append(now_ns)
                self.restarts[name] += 1
                self._start(name, now_ns)
            elif state == ServiceState.WAITING:
                dependencies = [self.state[d] for d in DEPENDS_ON.get(name, ())]
                if ServiceState.FAILED in dependencies:
                    self.state[name] = ServiceState.FAILED  # the manager stops waiting
                elif all(d == ServiceState.RUNNING for d in dependencies):
                    self._launch(name, now_ns)

        elapsed_us = (now_ns - self.last_snap) / 1000.0
        for name in self.names:
            alive = self.state[name] in ALIVE
            load = rng.uniform(0.05, 0.6) if self.state[name] == ServiceState.RUNNING else 0.0
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
            if self.state[name] == ServiceState.RUNNING:
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
            state = HEALTH_STATE[self.state[name]]
            last_seen = self.last_seen[name]
            if state in (RuntimeState.STOPPED, RuntimeState.UNHEALTHY):
                last_seen = self.start_ns[name]
            reports.append(
                make_report(
                    name=name,
                    pid=self.pid[name],
                    state=state,
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
                "state": state,
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
                "nextRestartTime": self.next_restart[name] if state == ServiceState.BACKOFF else 0,
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
