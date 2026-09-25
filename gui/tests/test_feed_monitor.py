"""Connection indicator: only arriving reports make the feed "Live"."""

from process_monitor_gui import STALE_AFTER_SEC, FeedMonitor


def test_not_connected_has_no_status():
    assert FeedMonitor().status(0.0) is None


def test_connected_without_reports_is_waiting_then_no_data():
    feed = FeedMonitor()
    feed.on_connected(100.0)
    assert feed.status(101.0) == ("Waiting for data…", "waiting")
    text, level = feed.status(100.0 + STALE_AFTER_SEC + 7)
    assert level == "stale" and text == f"No data for {STALE_AFTER_SEC + 7:.0f}s"


def test_reports_are_live_until_they_stop():
    feed = FeedMonitor()
    feed.on_connected(0.0)
    for t in (1.0, 1.5, 2.0):
        feed.on_report(t)
    assert feed.status(2.5) == ("Live", "live")
    assert not feed.data_stale(2.5)

    later = 2.0 + STALE_AFTER_SEC + 1
    assert feed.status(later)[1] == "stale"
    assert feed.data_stale(later)


def test_slow_publisher_does_not_flap():
    feed = FeedMonitor()
    feed.on_connected(0.0)
    for t in range(0, 41, 4):  # a report every 4 s
        feed.on_report(float(t))
    assert feed.stale_after() == 12.0  # three intervals
    assert feed.status(40.0 + 6)[1] == "live"
    assert feed.status(40.0 + 13)[1] == "stale"


def test_one_outage_does_not_loosen_the_threshold():
    feed = FeedMonitor()
    feed.on_connected(0.0)
    times = [0.5 * i for i in range(9)] + [60.0, 60.5, 61.0]  # 56 s gap
    for t in times:
        feed.on_report(t)
    assert feed.stale_after() == STALE_AFTER_SEC


def test_reconnect_waits_for_a_report_on_the_new_connection():
    feed = FeedMonitor()
    feed.on_connected(0.0)
    feed.on_report(1.0)
    feed.on_disconnected()
    assert feed.status(2.0) is None
    feed.on_connected(3.0)
    # The old report doesn't make the new connection live...
    assert feed.status(3.5) == ("Waiting for data…", "waiting")
    # ...and its rows go stale on their own schedule.
    assert not feed.data_stale(3.5)
    assert feed.data_stale(1.0 + STALE_AFTER_SEC + 0.1)


def test_report_interval_is_the_median_gap():
    feed = FeedMonitor()
    assert feed.report_interval() is None
    feed.on_connected(0.0)
    for t in (0.0, 10.0, 20.0, 31.0):
        feed.on_report(t)
    assert feed.report_interval() == 10.0
