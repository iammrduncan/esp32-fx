#!/usr/bin/env python3
"""The live run must include actual agent reads, edits and before/after tests."""
import importlib.util
import json
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
spec = importlib.util.spec_from_file_location("tool_trace", ROOT / "tools/tool_trace.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def acp(update):
    return "fx-eink: ACP <= " + json.dumps({"params": {"update": update}}) + "\n"


class EvidenceTests(unittest.TestCase):
    def trace(self):
        log = ""
        for index, name in enumerate(("SPEC.md", "battery.sh", "test.sh")):
            log += acp({"sessionUpdate": "tool_call", "name": "read_project", "toolCallId": str(index),
                        "rawInput": {"path": name}})
            log += "fx-tools: read_project begin\n"
            log += acp({"sessionUpdate": "tool_call_update", "toolCallId": str(index), "status": "completed"})
        log += "fx-tools: run_tests begin\nfx-tools: replace_battery begin\nfx-tools: run_tests begin\n"
        return log

    def test_complete_sequence(self):
        result = module.audit_tool_trace(self.trace())
        self.assertTrue(result["all_project_files_read"])
        self.assertTrue(result["agent_tested_before_and_after_edit"])
        self.assertEqual(result["tool_call_counts"]["run_tests"], 2)

    def test_final_success_text_is_not_tool_evidence(self):
        log = acp({"content": {"text": self.trace() + "EXAMPLE_PASS checks=14"}})
        result = module.audit_tool_trace(log)
        self.assertFalse(result["all_project_files_read"])
        self.assertFalse(result["agent_tested_before_and_after_edit"])

    def test_missing_post_edit_test_or_read_completion(self):
        result = module.audit_tool_trace(self.trace().rsplit("fx-tools: run_tests begin", 1)[0])
        self.assertFalse(result["agent_tested_before_and_after_edit"])
        result = module.audit_tool_trace(self.trace().replace('"completed"', '"failed"'))
        self.assertFalse(result["all_project_files_read"])


if __name__ == "__main__":
    unittest.main()
