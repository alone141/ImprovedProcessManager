# C++ process manager

**Date:** 2026-09-24
**Status:** Implemented

## Goal

A process manager in C++ with CMake, built on offline machines, Linux first and
Windows second, that the existing GUI can drive without changes: start, stop and
restart services registered in a configuration file, restart them by policy,
measure CPU, memory and GPU use, and publish a detailed report and a simplified
health report over ZeroMQ. The same binary works as a console client
(`--status`, `-s`, `-k`, `-r`). The GUI and the manager live in separate
directories, `gui/` and `manager/`.

## Decisions

| Topic | Choice |
|-------|--------|
| Compatibility | The health socket (6667) sends exactly the GUI's 128-byte records, one frame; the command socket (5557) accepts `BPM` + the 65-byte command from identity `PMC`. Everything new is additive |
| Detailed report | Its own PUB socket (6668), frames `report` + a versioned header that states record sizes, so older readers skip appended fields. A separate socket keeps it away from existing SUB-everything clients |
| Replies | Every command is answered (`BPM` + 128 bytes); ROUTER drops what old GUIs never read. The CLI uses a libzmq-assigned identity, so it never collides with the GUI's `PMC`; `ROUTER_HANDOVER` lets a reconnecting GUI replace its stale connection |
| New commands | Heartbeat (90), reload (91); service name `*` means every service |
| State model | Eight states (stopped, waiting, starting, running, unhealthy, stopping, backoff, failed), folded into the GUI's five in the health report |
| Restarts | never / on-failure / always; delay doubling up to a maximum; `max_restarts` within `restart_window_s`, then failed. A start by hand resets the budget |
| Stopping | Stop signal and `SIGCONT` to the process group, kill after `stop_timeout_ms`; leftovers of an exited main process are killed, and the exit counts once they are gone (at most 5 s later); shutdown stops dependents first |
| Linux isolation | New session per service, `PR_SET_PDEATHSIG`, `task_<name>` cgroup v2 groups below the manager's own (the layout the GUI's journal and PID views read), per-service journald stream opened by the child. The manager takes its cgroup's controllers only when it is alone there |
| Windows isolation | Job object per service with kill-on-close; memory and CPU-rate limits through the job |
| Measurements | One process-table snapshot per publish interval; per-service sums over cgroup members or session and descendants; cgroup `cpu.stat`/`io.stat` where available; GPU through NVML loaded at run time |
| Configuration | INI with `[manager]`, `[defaults]`, `[service NAME]`; strict (unknown keys are errors, with line numbers); reload by `SIGHUP` or command |
| Dependencies | libzmq and, for tests, GoogleTest; found installed or vendored under `third_party/`; nothing is downloaded |
| Toolchain floor | CMake 3.30 and C++20 as `cpp-style.md` has it, with `std::span` for sequences a function only reads: GCC 10, Clang 10 or Visual Studio 2019 16.10. Most long-term releases need Kitware's CMake archive, and Ubuntu 20.04 needs `g++-10` |
| Style | `cpp-style.md`: enum returns with out-parameters, Doxygen on every public declaration, one module and one test file per type |

## GUI changes

`gui/health_structs.py` parses the reply; `ZmqWorker` shows it in the status
bar. `mock_publisher.py` answers the same way. `systemd_logs.journal_cgroup_filter`
drops the `unified` / `systemd` mount directory of hybrid cgroup hosts, so the
Journal tab finds the manager's task cgroups there.

## Review

A project-wide review after the first version fixed, among others:

- Manager: restarts that ignored dependencies, a queued restart that survived
  shutdown, heartbeat restarts outside the restart budget, `*` commands that
  launched a service twice, restarts that met the old instance's dying
  processes, a manager that moved a login session's processes into its
  supervisor cgroup, I/O counted twice on LVM and md devices, an inherited
  ignored `SIGCHLD`, and relative working directories.
- GUI: an empty health frame that was ignored, commands sent while
  disconnected, graph holes at slow publish rates, stale CPU baselines after a
  reconnect, GPU rows dropped on Windows drivers, and journal lines lost to
  journalctl's 4096-byte field limit or to PID reuse across boots.

## Verification

- Linux (WSL1 Ubuntu 20.04, CMake 3.30.9, GCC 10.5, libzmq 4.3.2): 230 tests
  with warnings as errors, including launching real processes and the daemon
  end to end over ZeroMQ. Configuring with GCC 9 stops with the install hint.
- Windows (CMake 3.30.9, Visual Studio 2022 with MSVC 19.32 at `/W4`, GoogleTest
  1.8.1): the 196 tests that do not need libzmq, which was not available there,
  with warnings as errors.
- GUI: 132 pytest tests.
- The unchanged GUI against the running manager: health parsing, CPU %, state
  colours and commands; the updated GUI shows the replies.
- Not exercised: cgroups (WSL1 has none), NVML (no NVIDIA driver on the build
  host), the vendored-source build path, CMake 4.
