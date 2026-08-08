"""Verify CSV/YAML-style inventories were fully covered in agent output."""

from __future__ import annotations

import csv
import os
import re
from pathlib import Path

from cai.sdk.agents import function_tool


def _read_text(path: Path) -> str:
    for encoding in ("utf-8", "utf-8-sig", "latin-1"):
        try:
            return path.read_text(encoding=encoding)
        except UnicodeDecodeError:
            continue
    return path.read_text(encoding="utf-8", errors="replace")


def _extract_ids_from_csv(path: Path, id_pattern: re.Pattern[str], id_column: str | None) -> set[str]:
    text = _read_text(path)
    if not text.strip():
        return set()

    # Try structured CSV first when a column is named.
    try:
        reader = csv.DictReader(text.splitlines())
        if reader.fieldnames:
            target_col = id_column
            if not target_col:
                for name in reader.fieldnames:
                    if name and id_pattern.search(name):
                        target_col = name
                        break
                if not target_col:
                    for name in reader.fieldnames:
                        sample = name or ""
                        if id_pattern.search(sample):
                            target_col = name
                            break
            if target_col and target_col in (reader.fieldnames or []):
                found: set[str] = set()
                for row in reader:
                    val = (row.get(target_col) or "").strip()
                    for match in id_pattern.finditer(val):
                        found.add(match.group(0))
                if found:
                    return found
    except csv.Error:
        pass

    return set(id_pattern.findall(text))


def _extract_ids_from_response(response_text: str, id_pattern: re.Pattern[str]) -> set[str]:
    if not response_text.strip():
        return set()
    return set(id_pattern.findall(response_text))


@function_tool
def verify_csv_inventory(
    file_path: str,
    id_pattern: str = r"PAsset-\d+",
    id_column: str = "",
    response_text: str = "",
) -> str:
    """
    Count inventory IDs in a CSV/text file and compare with IDs mentioned in agent output.

    Use when the user asks to assess every PAsset-XX (or similar) in a spreadsheet:
    1) Run this tool with the CSV path before closing the task.
    2) Pass your latest assessment text in response_text.
    3) Report missing IDs to the user and continue until missing is empty.

    Args:
        file_path: Path to CSV or text inventory (absolute or relative to workspace).
        id_pattern: Regex for one ID token (default PAsset-NN).
        id_column: Optional CSV column name containing IDs; auto-detected when empty.
        response_text: Optional agent reply text to check coverage against the file.

    Returns:
        Summary with total IDs in file, IDs found in response_text, and missing IDs.
    """
    path = Path(os.path.expanduser(file_path.strip()))
    if not path.is_file():
        return f"Error: inventory file not found: {path}"

    try:
        pattern = re.compile(id_pattern)
    except re.error as exc:
        return f"Error: invalid id_pattern regex: {exc}"

    col = id_column.strip() or None
    file_ids = sorted(_extract_ids_from_csv(path, pattern, col), key=str)
    total = len(file_ids)

    if not file_ids:
        return (
            f"No IDs matched pattern {id_pattern!r} in {path}.\n"
            "Check id_pattern/id_column or file encoding."
        )

    lines = [
        f"Inventory file: {path}",
        f"ID pattern: {id_pattern}",
        f"Total unique IDs in file: {total}",
    ]

    if response_text.strip():
        response_ids = _extract_ids_from_response(response_text, pattern)
        missing = [i for i in file_ids if i not in response_ids]
        extra = sorted(response_ids - set(file_ids))
        covered = total - len(missing)
        lines.extend(
            [
                f"IDs referenced in response_text: {len(response_ids)}",
                f"Covered (in file ∩ response): {covered}/{total}",
            ]
        )
        if missing:
            preview = ", ".join(missing[:30])
            suffix = f" ... (+{len(missing) - 30} more)" if len(missing) > 30 else ""
            lines.append(f"MISSING from response ({len(missing)}): {preview}{suffix}")
        else:
            lines.append("MISSING from response: none — full coverage.")
        if extra:
            lines.append(f"Extra IDs in response (not in file): {', '.join(extra[:20])}")
    else:
        preview = ", ".join(file_ids[:40])
        suffix = f" ... (+{total - 40} more)" if total > 40 else ""
        lines.append(f"ID list (first 40): {preview}{suffix}")
        lines.append(
            "Tip: re-run with response_text set to your assessment to get missing IDs."
        )

    return "\n".join(lines)


from cai.tool_registry import TOOL_REGISTRY  # noqa: E402

TOOL_REGISTRY.register(
    "verify_csv_inventory",
    verify_csv_inventory,
    categories=["compliance", "misc"],
)
