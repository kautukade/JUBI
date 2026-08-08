"""Tests for verify_csv_inventory."""

import json
from pathlib import Path

import pytest

from cai.sdk.agents import RunContextWrapper
from cai.tools.evidence.inventory_check import verify_csv_inventory


@pytest.mark.asyncio
async def test_verify_csv_inventory_counts_file_ids(tmp_path: Path):
    csv_file = tmp_path / "assets.csv"
    csv_file.write_text(
        "id,name\nPAsset-01,foo\nPAsset-02,bar\nPAsset-03,baz\n",
        encoding="utf-8",
    )
    args = {"file_path": str(csv_file), "id_pattern": r"PAsset-\d+"}
    result = await verify_csv_inventory.on_invoke_tool(
        RunContextWrapper(None), json.dumps(args)
    )
    assert "Total unique IDs in file: 3" in result
    assert "PAsset-01" in result


@pytest.mark.asyncio
async def test_verify_csv_inventory_reports_missing(tmp_path: Path):
    csv_file = tmp_path / "assets.csv"
    csv_file.write_text("id\nPAsset-01\nPAsset-02\nPAsset-03\n", encoding="utf-8")
    args = {
        "file_path": str(csv_file),
        "response_text": "Assessment for PAsset-01 and PAsset-03 only.",
    }
    result = await verify_csv_inventory.on_invoke_tool(
        RunContextWrapper(None), json.dumps(args)
    )
    assert "MISSING from response (1)" in result
    assert "PAsset-02" in result
    assert "2/3" in result
