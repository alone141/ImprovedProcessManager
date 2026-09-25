from systemd_logs import (
    find_task_cgroup,
    index_task_cgroups,
    journal_cgroup_filter,
    list_cgroup_pids,
    snapshot_cgroup_members,
    task_cgroup_name,
)


def test_task_cgroup_name():
    assert task_cgroup_name("sensor_fusion") == "task_sensor_fusion"


def test_journal_cgroup_filter_strips_sysfs_root():
    assert (
        journal_cgroup_filter("/sys/fs/cgroup/system.slice/task_foo")
        == "/system.slice/task_foo"
    )


def test_journal_cgroup_filter_drops_hybrid_mount_directories():
    # cgroup v2 on a hybrid host lives at /sys/fs/cgroup/unified; journald omits that part.
    assert (
        journal_cgroup_filter("/sys/fs/cgroup/unified/system.slice/pm.service/task_foo")
        == "/system.slice/pm.service/task_foo"
    )
    assert (
        journal_cgroup_filter("/sys/fs/cgroup/systemd/system.slice/task_foo")
        == "/system.slice/task_foo"
    )
    assert journal_cgroup_filter("/sys/fs/cgroup/unified") == "/"


def test_journal_cgroup_filter_keeps_a_lookalike_root():
    assert journal_cgroup_filter("/sys/fs/cgroupx/task_foo") == "/sys/fs/cgroupx/task_foo"


def test_find_task_cgroup_on_a_hybrid_host(tmp_path):
    (tmp_path / "unified" / "system.slice" / "pm.service" / "task_vision").mkdir(parents=True)
    (tmp_path / "memory" / "system.slice").mkdir(parents=True)
    assert (
        find_task_cgroup("vision", root=str(tmp_path))
        == "/system.slice/pm.service/task_vision"
    )


def test_find_task_cgroup_in_fake_tree(tmp_path):
    target = tmp_path / "system.slice" / "foo.slice" / "task_sensor_fusion"
    target.mkdir(parents=True)
    (tmp_path / "other").mkdir()
    assert (
        find_task_cgroup("sensor_fusion", root=str(tmp_path))
        == "/system.slice/foo.slice/task_sensor_fusion"
    )


def test_find_task_cgroup_missing(tmp_path):
    (tmp_path / "empty").mkdir()
    assert find_task_cgroup("nope", root=str(tmp_path)) is None


def test_index_and_list_cgroup_pids(tmp_path):
    cg = tmp_path / "system.slice" / "task_vision"
    cg.mkdir(parents=True)
    (cg / "cgroup.procs").write_text("101\n202\n", encoding="utf-8")
    idx = index_task_cgroups(str(tmp_path))
    assert idx["vision"].endswith("task_vision")
    assert list_cgroup_pids(idx["vision"]) == [101, 202]
    snap = snapshot_cgroup_members(["vision", "missing"], root=str(tmp_path))
    assert [p.pid for p in snap["vision"]] == [101, 202]
    assert snap["missing"] == []
