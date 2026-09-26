"""The mock manager speaks both reports and answers commands like the real one."""

import random

import mock_publisher as mp
from health_structs import (
    ALL_SERVICES,
    REPORT_TOPIC,
    CommandEnum,
    CommandResult,
    RuntimeState,
    ServiceState,
    parse_detailed_report,
    parse_health_reports,
    parse_report_frames,
    report_to_dict,
)

SEC = 1_000_000_000


def manager():
    return mp.MockManager(rng=random.Random(7), now_ns=1_000 * SEC, interval_ms=500)


def test_health_frame_holds_one_record_per_service():
    m = manager()
    reports = [report_to_dict(r) for r in parse_health_reports(m.health_frame(1_001 * SEC))]
    assert [r["processName"] for r in reports] == mp.PROCESSES
    assert all(r["state"] == RuntimeState.RUNNING and r["pid"] > 0 for r in reports)
    assert all(r["snapshotTime"] == 1_001 * SEC for r in reports)


def test_report_frames_parse_and_match_the_health_records():
    m = manager()
    frames = m.report_frames(1_001 * SEC)
    assert frames[0] == REPORT_TOPIC
    report = parse_report_frames(frames)
    assert report == parse_detailed_report(frames[1])
    assert report["managerVersion"] == "mock" and report["publishIntervalMs"] == 500
    assert report["cgroups"] and report["gpuMonitoring"] and not report["windows"]
    assert report["memoryTotalBytes"] > report["memoryAvailableBytes"] > 0
    assert [s["name"] for s in report["services"]] == mp.PROCESSES
    health = {report_to_dict(r)["processName"]: report_to_dict(r) for r in parse_health_reports(m.health_frame(1_001 * SEC))}
    for service in report["services"]:
        record = health[service["name"]]
        assert service["pid"] == record["pid"]
        assert service["cpuTimeUsec"] == record["cpuUsageInUsec"]
        assert service["memoryBytes"] == record["memoryUsageInBytes"]
        assert service["state"] == ServiceState.RUNNING and service["usageValid"]
        assert service["gpuValid"] == (service["name"] in mp.GPU_USERS)
        assert service["heartbeat"] == (service["name"] in mp.HEARTBEAT_USERS)
    assert len(report["gpus"]) == 1 and report["gpus"][0]["memoryTotalBytes"] == 8 * 1024**3


def service(m, name, now_ns):
    return next(s for s in m.detailed_report(now_ns)["services"] if s["name"] == name)


def test_stop_passes_through_stopping_and_records_the_exit():
    m = manager()
    assert m.handle(CommandEnum.STOP, "vision_pipeline", 1_002 * SEC) == (CommandResult.OK, "stopping")
    assert m.state["vision_pipeline"] == ServiceState.STOPPING and m.pid["vision_pipeline"] > 0
    assert service(m, "vision_pipeline", 1_002 * SEC)["state"] == ServiceState.STOPPING
    m.advance(1_003 * SEC)
    assert m.state["vision_pipeline"] == ServiceState.STOPPED and m.pid["vision_pipeline"] == 0
    vision = service(m, "vision_pipeline", 1_005 * SEC)
    assert vision["state"] == ServiceState.STOPPED and vision["pid"] == 0
    assert vision["lastExitCode"] == -15 and vision["lastExitTime"] == 1_002 * SEC
    assert not vision["usageValid"] and not vision["gpuValid"] and vision["openFiles"] is None
    assert m.handle(CommandEnum.STOP, "vision_pipeline", 1_006 * SEC) == (CommandResult.OK, "stopped")
    assert m.last_exit["vision_pipeline"] == (-15, 1_002 * SEC)  # a stopped service does not exit again


def test_start_and_restart_answer_like_the_manager():
    m = manager()
    assert m.handle(CommandEnum.RESTART, "control_loop", 1_002 * SEC) == (CommandResult.OK, "restarting")
    assert m.state["control_loop"] == ServiceState.STARTING and m.restarts["control_loop"] >= 1
    assert m.pid["control_loop"] > 0 and m.start_ns["control_loop"] == 1_002 * SEC
    assert m.handle(CommandEnum.START, "ghost", 1_002 * SEC) == (CommandResult.UNKNOWN_SERVICE, "no service named ghost")
    assert m.handle(99, "control_loop", 1_002 * SEC) == (CommandResult.UNKNOWN_COMMAND, "unknown command 99")


def test_a_crash_backs_off_then_restarts_and_the_budget_ends_in_failed():
    m = manager()
    before = m.restarts["control_loop"]
    m.crash("control_loop", 1_002 * SEC, 3)
    assert m.state["control_loop"] == ServiceState.BACKOFF and m.pid["control_loop"] == 0
    record = service(m, "control_loop", 1_002 * SEC)
    assert record["state"] == ServiceState.BACKOFF and record["nextRestartTime"] == 1_003 * SEC
    m.advance(1_002 * SEC + SEC // 2)  # not due yet
    assert m.state["control_loop"] == ServiceState.BACKOFF
    m.advance(1_003 * SEC)
    assert m.state["control_loop"] == ServiceState.STARTING and m.restarts["control_loop"] == before + 1
    # Each further crash in the window doubles the delay; the sixth gives up.
    now = 1_003 * SEC
    for i in range(1, mp.MAX_RESTARTS):
        now += SEC
        m.crash("control_loop", now, 3)
        assert m.next_restart["control_loop"] == now + mp.RESTART_DELAY_NS * 2**i
        now = m.next_restart["control_loop"]
        m.advance(now)
        assert m.state["control_loop"] == ServiceState.STARTING
    now += SEC
    m.crash("control_loop", now, 3)
    assert m.state["control_loop"] == ServiceState.FAILED
    assert service(m, "control_loop", now)["nextRestartTime"] == 0
    # A start by hand resets the budget.
    assert m.handle(CommandEnum.START, "control_loop", now + SEC) == (CommandResult.OK, "starting")
    m.crash("control_loop", now + 2 * SEC, 3)
    assert m.state["control_loop"] == ServiceState.BACKOFF


def test_a_service_waits_for_its_dependency_and_fails_with_it():
    m = manager()
    m.crash("sensor_fusion", 1_002 * SEC, -9)
    assert m.handle(CommandEnum.RESTART, "path_planner", 1_002 * SEC) == (CommandResult.OK, "restarting")
    assert m.state["path_planner"] == ServiceState.WAITING and m.pid["path_planner"] == 0
    m.state["sensor_fusion"] = ServiceState.RUNNING  # its restart went through
    m.advance(1_003 * SEC)
    assert m.state["path_planner"] == ServiceState.STARTING
    m.state["sensor_fusion"] = ServiceState.FAILED
    m.state["path_planner"] = ServiceState.WAITING
    m.advance(1_004 * SEC)
    assert m.state["path_planner"] == ServiceState.FAILED


def test_the_health_record_folds_the_states_like_the_manager():
    m = manager()
    for state, health in mp.HEALTH_STATE.items():
        m.state["control_loop"] = state
        record = next(
            report_to_dict(r) for r in parse_health_reports(m.health_frame(1_001 * SEC))
            if report_to_dict(r)["processName"] == "control_loop"
        )
        assert record["state"] == health


def test_star_reaches_every_service_and_reload_is_answered():
    m = manager()
    assert m.handle(CommandEnum.STOP, ALL_SERVICES, 1_002 * SEC) == (CommandResult.OK, "stop sent to 6 services")
    assert all(state == ServiceState.STOPPING for state in m.state.values())
    m.advance(1_003 * SEC)
    assert all(state == ServiceState.STOPPED for state in m.state.values())
    assert all(pid == 0 for pid in m.pid.values())
    assert m.handle(CommandEnum.START, ALL_SERVICES, 1_003 * SEC) == (CommandResult.OK, "start sent to 6 services")
    # Everything starts, except what waits for a dependency that does not run yet.
    assert m.state.pop("path_planner") == ServiceState.WAITING
    assert all(state == ServiceState.STARTING for state in m.state.values())
    assert m.handle(CommandEnum.RELOAD, "", 1_004 * SEC) == (CommandResult.OK, "0 added, 0 removed, 0 changed")
    assert m.reloads == 1
    assert m.handle(CommandEnum.HEARTBEAT, "control_loop", 1_005 * SEC) == (CommandResult.OK, "")
    assert m.last_seen["control_loop"] == 1_005 * SEC


def test_advance_keeps_the_figures_consistent():
    m = manager()
    for tick in range(1, 200):
        m.advance((1_000 + tick) * SEC)
    report = m.detailed_report(1_200 * SEC)
    for record in report["services"]:
        alive = record["state"] in mp.ALIVE
        assert (record["pid"] > 0) == alive
        assert record["memoryPeakBytes"] >= record["memoryBytes"]
        if not alive:
            assert record["lastExitTime"] > 0 and record["lastExitCode"] in mp.EXIT_CODES
        assert (record["nextRestartTime"] > 0) == (record["state"] == ServiceState.BACKOFF)
    assert 0.0 <= report["hostCpuPercent"] <= 100.0
    assert report["gpus"][0]["memoryUsedBytes"] <= report["gpus"][0]["memoryTotalBytes"]


def test_parse_command_skips_delimiters():
    payload = bytes(65)
    assert mp.parse_command([b"PMC", b"BPM", payload]) == (b"PMC", b"BPM", payload)
    assert mp.parse_command([b"PMC", b"", b"BPM", payload]) == (b"PMC", b"BPM", payload)
    assert mp.parse_command([b"PMC", payload]) == (b"PMC", None, payload)
