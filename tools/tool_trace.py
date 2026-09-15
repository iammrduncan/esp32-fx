#!/usr/bin/env python3
"""Check that a coding turn performed its reads, edit, and before/after tests."""

import json
import re


def audit_tool_trace(diagnostics):
    """Summarize trusted native/ACP records without returning model text."""
    calls = re.findall(
        r"^fx-tools: (read_project|replace_battery|run_tests) begin$",
        diagnostics,
        flags=re.MULTILINE,
    )
    read_calls = {}
    successful_reads = set()
    for line in diagnostics.splitlines():
        prefix = "fx-eink: ACP <= "
        if not line.startswith(prefix):
            continue
        try:
            message = json.loads(line[len(prefix):])
            update = message.get("params", {}).get("update", {})
            if (update.get("sessionUpdate") == "tool_call"
                    and update.get("name") == "read_project"):
                arguments = update.get("rawInput", {})
                if (isinstance(arguments, dict)
                        and arguments.get("path") in
                        ("SPEC.md", "battery.sh", "test.sh")):
                    read_calls[update.get("toolCallId")] = arguments["path"]
            elif (update.get("sessionUpdate") == "tool_call_update"
                  and update.get("status") == "completed"):
                path = read_calls.get(update.get("toolCallId"))
                if path:
                    successful_reads.add(path)
        except (ValueError, TypeError, AttributeError):
            continue
    edits = [index for index, name in enumerate(calls)
             if name == "replace_battery"]
    tested_before_after = bool(
        edits
        and "run_tests" in calls[:edits[0]]
        and "run_tests" in calls[edits[-1] + 1:]
    )
    return {
        "tool_call_counts": {
            name: calls.count(name)
            for name in ("read_project", "replace_battery", "run_tests")
        },
        "all_project_files_read": successful_reads
        == {"SPEC.md", "battery.sh", "test.sh"},
        "agent_tested_before_and_after_edit": tested_before_after,
    }


def memory_summary(diagnostics):
    """Extract only numeric in-process checkpoints emitted by the target."""
    free = [int(value) for value in re.findall(
        r"^fx-wamr: .* system_free_kib=(\d+)(?: |$)",
        diagnostics,
        flags=re.MULTILINE,
    )]
    pages = [int(value) for value in re.findall(
        r"^fx-wamr: .* guest_pages=(\d+)(?: |$)",
        diagnostics,
        flags=re.MULTILINE,
    )]
    return {
        "system_memory_checkpoint_count": len(free),
        "minimum_observed_system_free_kib": min(free) if free else None,
        "maximum_observed_guest_pages": max(pages) if pages else None,
    }
