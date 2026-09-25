# Wire protocol

The GUI (`gui/`) and the process manager (`manager/`) talk over ZeroMQ. The
manager binds three sockets; clients connect to them.

| Socket | Default endpoint | Pattern | Carries |
|--------|------------------|---------|---------|
| Health | `tcp://*:6667` | PUB → SUB | Simplified health report, one frame |
| Report | `tcp://*:6668` | PUB → SUB | Detailed report, frames `report` + payload |
| Command | `tcp://*:5557` | ROUTER ← DEALER | Start / stop / restart / heartbeat / reload, with a reply |

All integers are little-endian. Text fields are UTF-8, NUL-padded, cut at a
character boundary so the last byte of a field is always NUL. Times are
nanoseconds since the Unix epoch.

The health socket and the command socket are unchanged from the protocol the
GUI was built for; everything else is an addition that older clients ignore.

## Health report (port 6667)

One single-frame message per publish interval: the records back to back,
128 bytes each, no header. Zero services send an empty frame. (Readers also
accept the same array prefixed with a `uint32` record count.)

This is the struct the GUI calls `DetailedHealthReport` (`gui/health_structs.py`,
8-byte packing). Its name is historical; it is the *simplified* report.

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0 | 64 | `processName` | service name |
| 64 | 4 | `pid` | int32; 0 unless a process is alive |
| 68 | 4 | — | padding |
| 72 | 8 | `memoryUsageInBytes` | uint64; resident memory of every process of the service |
| 80 | 8 | `cpuUsageInUsec` | uint64; cumulative CPU time; the GUI derives CPU % from two reports |
| 88 | 1 | `state` | RuntimeState, below |
| 89 | 7 | — | padding |
| 96 | 8 | `start_time` | int64 ns; 0 unless a process is alive |
| 104 | 8 | `lastSeen` | int64 ns; last heartbeat, or last time the process was seen alive |
| 112 | 4 | `missedBeats` | int32 |
| 116 | 4 | `restartCount` | int32; automatic restarts plus restart commands |
| 120 | 8 | `snapshotTime` | int64 ns |

RuntimeState: `0` unknown, `1` starting, `2` running, `3` stopped, `4` unhealthy.
The manager's eight states fold into these five:

| Manager state | Health state | Why |
|---------------|--------------|-----|
| stopped, stopping | stopped | nothing to start while it goes down |
| waiting, starting, backoff | starting | the manager is bringing it up |
| running | running | |
| unhealthy, failed | unhealthy | needs attention; the GUI offers Start |

## Detailed report (port 6668)

Two frames: the topic `report`, then a payload made of a 192-byte header,
`serviceCount` service records and `gpuCount` GPU records. The header states
its own size and the record sizes, so a reader skips fields added later;
`version` only changes for incompatible layouts.

Header:

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 4 | magic `0x524D5042` (bytes `B P M R`) |
| 4 | 2 | version, currently 1 |
| 6 | 2 | header size (192) |
| 8 | 2 | service record size (368) |
| 10 | 2 | GPU record size (160) |
| 12 | 4 | service count |
| 16 | 4 | GPU count |
| 20 | 4 | manager PID |
| 24 | 8 | snapshot time |
| 32 | 8 | manager start time |
| 40 | 4 | publish interval, ms |
| 44 | 4 | flags: `1` cgroups in use, `2` GPU monitoring, `4` shutting down, `8` Windows host (exit codes are Windows codes) |
| 48 | 8 | host CPU %, double, 0–100; negative = unknown |
| 56 | 8 | host memory total, bytes |
| 64 | 8 | host memory available, bytes |
| 72 | 24 | load average 1 / 5 / 15 min, three doubles |
| 96 | 8 | host uptime, seconds |
| 104 | 4 | CPU count |
| 108 | 4 | reserved |
| 112 | 64 | host name |
| 176 | 16 | manager version |

Service record:

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 32 | name |
| 32 | 128 | binary |
| 160 | 64 | description |
| 224 | 4 | PID |
| 228 | 1 | state: `0` stopped, `1` waiting, `2` starting, `3` running, `4` unhealthy, `5` stopping, `6` backoff, `7` failed |
| 229 | 1 | restart mode: `0` never, `1` on-failure, `2` always |
| 230 | 1 | flags: `1` autostart, `2` heartbeat supervised, `4` cgroup accounting, `8` usage figures valid, `16` GPU figures valid, `32` being removed |
| 231 | 1 | reserved |
| 232 | 4 | restart count |
| 236 | 4 | missed heartbeats |
| 240 | 4 | last exit: exit code, or minus the signal number; on a Windows host (flag `8`) always the exit code, which may be a negative NTSTATUS such as `0xC0000005` |
| 244 | 4 | process count |
| 248 | 4 | thread count |
| 252 | 4 | open files (handles on Windows); -1 unknown |
| 256 | 8 | start time |
| 264 | 8 | last seen |
| 272 | 8 | last exit time; 0 = never exited |
| 280 | 8 | next restart time; 0 = none scheduled |
| 288 | 8 | CPU time, µs |
| 296 | 8 | CPU %, double; 100 = one busy core; negative = unknown |
| 304 | 8 | resident memory, bytes |
| 312 | 8 | peak resident memory seen, bytes |
| 320 | 8 | memory limit, bytes; 0 = none |
| 328 | 8 | bytes read |
| 336 | 8 | bytes written |
| 344 | 8 | GPU %, double, summed over GPUs; negative = unknown |
| 352 | 8 | GPU memory, bytes |
| 360 | 4 | OOM kills (cgroup memory.events) |
| 364 | 4 | CPU limit, % of one core; 0 = none |

GPU record:

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 64 | name |
| 64 | 48 | UUID |
| 112 | 4 | index |
| 116 | 4 | temperature, °C; 0 = unknown |
| 120 | 8 | utilisation %, double |
| 128 | 8 | memory utilisation %, double |
| 136 | 8 | memory total, bytes |
| 144 | 8 | memory used, bytes |
| 152 | 4 | power, mW |
| 156 | 4 | reserved |

GPU figures come from NVML, which the manager loads at run time from the
NVIDIA driver; a service's GPU use is the sum over all of its processes.

## Commands (port 5557)

A client connects a DEALER and sends two frames: `BPM`, then the 65-byte
command. The GUI uses the fixed identity `PMC`; the CLI lets libzmq pick one, so
both can be connected at once. The ROUTER also accepts an empty delimiter frame
(REQ clients) and a missing `BPM` frame.

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 1 | command |
| 1 | 32 | service name, or `*` for every service |
| 33 | 32 | arguments; reserved, ignored |

Commands: `78` start (also starts stopped dependencies), `79` stop, `81`
restart, `90` heartbeat, `91` reload the configuration file (name ignored).
The first three are the GUI's values.

### Reply

Every command gets a reply: `BPM`, then 128 bytes (an empty delimiter frame
first when the request had one). Clients that never read replies, such as older
GUIs, lose nothing: libzmq drops what they leave unread.

| Offset | Size | Field |
|-------:|-----:|-------|
| 0 | 1 | command, echoed |
| 1 | 1 | result |
| 2 | 32 | service name, echoed |
| 34 | 94 | message, for example `started, pid 4242` |

Results: `0` ok, `1` unknown service, `2` unknown command, `3` malformed
request, `4` nothing to do (already in that state), `5` not possible now,
`6` launch failed, `7` the manager is shutting down, `8` reload failed.

### Heartbeats

A service with `heartbeat_interval_ms` set sends command `90` with its own name
at least that often. After `heartbeat_tolerance` intervals without one it is
unhealthy, and with `unhealthy_action = restart` it is restarted. The manager
starts every service with these variables:

| Variable | Value |
|----------|-------|
| `BPM_SERVICE_NAME` | the service's name |
| `BPM_COMMAND_ENDPOINT` | where to send heartbeats, for example `tcp://127.0.0.1:5557` |
| `BPM_HEARTBEAT_INTERVAL_MS` | the interval, when heartbeats are supervised |

A shell script can beat with `berayprocessmanager --heartbeat NAME --command "$BPM_COMMAND_ENDPOINT"`.
