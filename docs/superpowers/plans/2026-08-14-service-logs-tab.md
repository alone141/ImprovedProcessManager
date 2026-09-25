# Service Logs Tab Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Service tab that auto-refreshes `systemctl status` and journalctl for `berayprocessmanager.service`.

**Architecture:** Pure helpers in `systemd_logs.py`; `SystemdLogWorker` on a QThread polls every 2s; `QTabWidget` wraps Processes + Service panes. Fetch off UI thread.

**Tech Stack:** Python 3, PyQt6, subprocess (systemctl/journalctl)

## Global Constraints

- Unit fixed: `berayprocessmanager.service`
- Refresh: 2000 ms; journal: last 200 lines
- shell=False; no-pager; off UI thread
- Linux-only feature; graceful error elsewhere
- Do not break ZMQ/GPU Processes tab
- Commits optional

---

### Task 1: `systemd_logs.py` helpers

Create pure fetch functions + constants. Optional unit test with mocked subprocess.

### Task 2: GUI tabs + worker

Modify `process_monitor_gui.py`: QTabWidget, Service pane, worker lifecycle.

### Task 3: README note

Document the Service tab and Linux requirement.
