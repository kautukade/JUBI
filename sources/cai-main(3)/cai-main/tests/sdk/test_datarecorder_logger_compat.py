import json


def test_datarecorder_warning_accepts_printf_args(tmp_path, monkeypatch):
    # DataRecorder writes under ~/.cai/logs; keep tests hermetic.
    monkeypatch.setenv("HOME", str(tmp_path))

    from cai.sdk.agents.run_to_jsonl import DataRecorder

    r = DataRecorder(workspace_name="pytest")
    r.warning("Empty assistant completion (%s/%s)", 1, 3)

    # Ensure the warning was written and formatted (no raw %s placeholders remain).
    with open(r.filename, encoding="utf-8") as f:
        lines = [json.loads(line) for line in f if line.strip()]

    warning_events = [x for x in lines if isinstance(x, dict) and x.get("event") == "log_warning"]
    assert warning_events, "expected at least one warning event"
    msg = warning_events[-1].get("message", "")
    assert "Empty assistant completion" in msg
    assert "%s" not in msg

