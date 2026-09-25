from health_structs import _utf8_fit, make_command_message, CommandEnum


def test_utf8_fit_ascii():
    assert _utf8_fit("hello", 31) == b"hello"


def test_utf8_fit_does_not_split_multibyte():
    # "ü" is 2 bytes; 10 a's + ü = 12 bytes
    s = "aaaaaaaaaaü"
    raw = _utf8_fit(s, 11)
    raw.decode("utf-8")  # must not raise
    assert raw == b"aaaaaaaaaa"


def test_command_message_name_utf8_safe():
    payload = make_command_message(CommandEnum.START, "x" * 40 + "ü", "")
    assert len(payload) == 65


E_ACUTE, EURO, GRIN = chr(0xE9), chr(0x20AC), chr(0x1F600)  # 2, 3 and 4 bytes


def test_utf8_fit_keeps_a_character_that_ends_at_the_limit():
    assert _utf8_fit("a" + E_ACUTE + "b", 3) == ("a" + E_ACUTE).encode("utf-8")
    assert _utf8_fit("ab" + EURO + "c", 5) == ("ab" + EURO).encode("utf-8")


def test_utf8_fit_drops_only_the_character_the_cut_splits():
    assert _utf8_fit("ab" + EURO + "c", 4) == b"ab"
    assert _utf8_fit("ab" + EURO + "c", 3) == b"ab"
    assert _utf8_fit(GRIN + "x", 3) == b""
    assert _utf8_fit(GRIN + "x", 4) == GRIN.encode("utf-8")
