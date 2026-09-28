from health_structs import (
    REPLY_SIZE,
    CommandEnum,
    CommandResult,
    describe_reply,
    make_command_reply,
    parse_command_reply,
    parse_health_reports,
    reply_succeeded,
)


def test_reply_layout_matches_the_manager():
    # manager/src/CommandMessage.cpp: command 0, result 1, name 2..33, message 34..127
    raw = make_command_reply(81, CommandResult.LAUNCH_FAILED, "vision", "binary missing")
    assert len(raw) == REPLY_SIZE == 128
    assert raw[0] == 81
    assert raw[1] == 6
    assert raw[2:8] == b"vision"
    assert raw[34:48] == b"binary missing"
    assert raw[127] == 0


def test_parse_reply_after_the_tag():
    reply = parse_command_reply([b"BPM", make_command_reply(78, CommandResult.OK, "a", "started, pid 7")])
    assert reply == {
        "command": CommandEnum.START,
        "result": CommandResult.OK,
        "serviceName": "a",
        "message": "started, pid 7",
    }


def test_parse_reply_skips_a_delimiter_frame():
    reply = parse_command_reply([b"", b"BPM", make_command_reply(79, CommandResult.ALREADY, "b")])
    assert reply is not None
    assert reply["result"] == CommandResult.ALREADY


def test_parse_reply_rejects_other_messages():
    good = make_command_reply(79, CommandResult.OK, "b")
    assert parse_command_reply([good]) is None
    assert parse_command_reply([b"XYZ", good]) is None
    assert parse_command_reply([b"BPM", good[:-1]]) is None
    assert parse_command_reply([b"BPM", good, b"extra"]) is None


def test_unknown_codes_are_kept_as_numbers():
    reply = parse_command_reply([b"BPM", make_command_reply(99, CommandResult.OK, "c")])
    raw = bytearray(make_command_reply(90, CommandResult.OK, "c"))
    raw[1] = 42
    odd = parse_command_reply([b"BPM", bytes(raw)])
    assert reply["command"] == 99
    assert odd["result"] == 42
    assert describe_reply(odd) == "command 90 c: result 42"


def test_describe_reply_reads_like_the_cli():
    ok = parse_command_reply([b"BPM", make_command_reply(81, CommandResult.OK, "vision", "restarting")])
    missing = parse_command_reply(
        [b"BPM", make_command_reply(78, CommandResult.UNKNOWN_SERVICE, "ghost", "no service named ghost")]
    )
    assert describe_reply(ok) == "restart vision: ok (restarting)"
    assert describe_reply(missing) == "start ghost: unknown service (no service named ghost)"
    assert reply_succeeded(ok)
    assert not reply_succeeded(missing)


def test_empty_count_prefixed_health_array_is_empty():
    assert parse_health_reports(b"\x00\x00\x00\x00") == []
