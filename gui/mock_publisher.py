#!/usr/bin/env python3
"""
Mock process-manager for testing the GUI.

PUB  (bind)  – broadcasts array of DetailedHealthReport
ROUTER (bind) – accepts DEALER commands with:
      identity = b"PMC"
      frame0   = b"BPM"
      frame1   = packed CommandMessage (65 B)
"""

from __future__ import annotations

import argparse
import ctypes
import random
import signal
import sys
import time
from typing import List

import zmq

from health_structs import (
    COMMAND_SIZE,
    CommandEnum,
    CommandMessage,
    CommandResult,
    DetailedHealthReport,
    REPORT_SIZE,
    RuntimeState,
    make_command_reply,
)


PROCESSES = [
    "sensor_fusion",
    "path_planner",
    "vision_pipeline",
    "control_loop",
    "telemetry_bridge",
    "logger_daemon",
]


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


def main():
    # Arrows in the log must not fail when the output goes to a cp1252 file or pipe.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    parser = argparse.ArgumentParser()
    parser.add_argument("--pub", default="tcp://*:6667")
    parser.add_argument("--router", default="tcp://*:5557")
    parser.add_argument("--interval", type=float, default=0.5)
    args = parser.parse_args()

    ctx = zmq.Context()

    pub = ctx.socket(zmq.PUB)
    pub.bind(args.pub)
    print(f"[PUB]    bound to {args.pub}")

    router = ctx.socket(zmq.ROUTER)
    router.bind(args.router)
    print(f"[ROUTER] bound to {args.router}")
    print("Expecting DEALER identity = 'PMC', frames: 'BPM' + CommandMessage(65 B)\n")

    # simulated state
    states     = {n: RuntimeState.RUNNING for n in PROCESSES}
    pids       = {n: 10000 + i for i, n in enumerate(PROCESSES)}
    start_times = {
        n: time.time_ns() - random.randint(60, 3600) * 1_000_000_000
        for n in PROCESSES
    }
    cpu_base   = {n: random.randint(1_000_000, 50_000_000) for n in PROCESSES}
    mem_base   = {n: random.randint(20, 400) * 1024 * 1024 for n in PROCESSES}
    restarts   = {n: random.randint(0, 3) for n in PROCESSES}
    missed     = {n: 0 for n in PROCESSES}

    last_snap = time.time_ns()
    running = True

    def handle_sig(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, handle_sig)
    signal.signal(signal.SIGTERM, handle_sig)

    poller = zmq.Poller()
    poller.register(router, zmq.POLLIN)

    print("Publishing health reports… (Ctrl+C to stop)\n")

    while running:
        # ── handle commands ──────────────────────────────────────────────
        try:
            events = dict(poller.poll(10))
        except zmq.ZMQError:
            break

        if router in events:
            try:
                # ROUTER frames: [identity][empty?][…]  we take last two meaningful
                frames = router.recv_multipart(flags=zmq.NOBLOCK)
                identity = frames[0]
                # remaining frames after identity
                payload_frames = frames[1:]
                if not payload_frames:
                    continue

                # Expect: identity | "BPM" | CommandMessage
                # (some stacks put an empty delimiter, so we are flexible)
                topic = None
                payload = None
                for f in payload_frames:
                    if f == b"BPM":
                        topic = f
                    elif len(f) == COMMAND_SIZE:
                        payload = f

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
                    svc  = cmd_msg.ServiceName.split(b"\x00", 1)[0].decode(errors="replace")
                    args = cmd_msg.Args.split(b"\x00", 1)[0].decode(errors="replace")
                    print(f"   CMD={cmd}  ServiceName={svc!r}  Args={args!r}")

                    result, note = CommandResult.OK, ""
                    if svc not in states:
                        result, note = CommandResult.UNKNOWN_SERVICE, f"no service named {svc}"
                    elif cmd == CommandEnum.START:
                        states[svc] = RuntimeState.STARTING
                        note = "starting"
                        print(f"   → starting {svc}")
                    elif cmd == CommandEnum.STOP:
                        states[svc] = RuntimeState.STOPPED
                        pids[svc] = 0
                        note = "stopped"
                        print(f"   → stopped {svc}")
                    elif cmd == CommandEnum.RESTART:
                        states[svc] = RuntimeState.STARTING
                        restarts[svc] += 1
                        note = "restarting"
                        print(f"   → restarting {svc}")
                    else:
                        result, note = CommandResult.UNKNOWN_COMMAND, f"unknown command {int(cmd)}"
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

        # ── simulate state changes ───────────────────────────────────────
        for name in PROCESSES:
            if states[name] == RuntimeState.STARTING and random.random() < 0.3:
                states[name] = RuntimeState.RUNNING
                pids[name] = random.randint(10000, 20000)
                start_times[name] = time.time_ns()  # restarts were counted at the command
            elif states[name] == RuntimeState.RUNNING and random.random() < 0.01:
                states[name] = RuntimeState.UNHEALTHY
                missed[name] += 1
            elif states[name] == RuntimeState.UNHEALTHY and random.random() < 0.05:
                states[name] = RuntimeState.STOPPED
                pids[name] = 0

        # ── publish health array ─────────────────────────────────────────
        now = time.time_ns()
        reports = []
        for name in PROCESSES:
            elapsed_us = (now - last_snap) / 1000.0
            load = random.uniform(0.05, 0.6) if states[name] == RuntimeState.RUNNING else 0.0
            cpu_base[name] += int(elapsed_us * load)

            mem = mem_base[name] + random.randint(-2_000_000, 2_000_000)
            mem = max(10 * 1024 * 1024, mem)

            last_seen = now - random.randint(0, 200_000_000)
            if states[name] in (
                RuntimeState.UNKNOWN,
                RuntimeState.STOPPED,
                RuntimeState.UNHEALTHY,
            ):
                last_seen = start_times[name]

            rep = make_report(
                name=name,
                pid=pids[name],
                state=states[name],
                mem=mem,
                cpu_usec=cpu_base[name],
                start_ns=start_times[name],
                last_seen_ns=last_seen,
                missed=missed[name],
                restarts=restarts[name],
                snap_ns=now,
            )
            reports.append(rep)

        last_snap = now
        pub.send(reports_to_bytes(reports))

        time.sleep(args.interval)

    print("\nShutting down…")
    pub.close(0)
    router.close(0)
    ctx.term()


if __name__ == "__main__":
    main()
