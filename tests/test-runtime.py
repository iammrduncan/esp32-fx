#!/usr/bin/env python3
"""Real compact fx/WAMR protocol, e-paper rendering, and coding tool regressions."""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parent.parent
IMAGE = "localhost/esp32-fx-host-tools:bookworm"
MARKER = "FX_WAMR_NATIVE_BRIDGE_OK"


@contextmanager
def provider(calls=()):
    state = {"requests": 0, "errors": []}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_POST(self):
            try:
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                assert isinstance(body.get("prompt", body.get("messages")), list)
                index = state["requests"]
                if calls:
                    wire = json.dumps(body)
                    assert "run_tests" in wire
                    if index:
                        assert calls[index-1][2] in wire, f"Missing tool result {index}"
                    if index == 4:
                        assert "exit_code=1" in wire
                    if index == 6:
                        assert "exit_code=0" in wire
                    assert index <= len(calls)
                if index < len(calls):
                    event = {"type": "tool-call", "toolCallId": f"call-{index}",
                             "toolName": calls[index][0], "input": calls[index][1]}
                    reason = "tool-calls"
                else:
                    event = {"type": "text-delta", "delta": MARKER}
                    reason = "stop"
                state["requests"] += 1
                finish = {"type": "finish", "finishReason": {"unified": reason, "raw": reason},
                          "usage": {"inputTokens": {"total": 1}, "outputTokens": {"total": 1}}}
                wire = "".join("data: " + json.dumps(e) + "\n\n" for e in (event, finish)) + "data: [DONE]\n\n"
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.end_headers()
                self.wfile.write(wire.encode())
            except Exception as error:
                state["errors"].append(error)
                self.send_error(500)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}", state
        assert not state["errors"], state["errors"]
    finally:
        server.shutdown()
        server.server_close()
        thread.join()


def environment(prefix=ROOT):
    return {**os.environ, "AI_GATEWAY_API_KEY": "fixture-key", "FX_MODEL": "fixture/wamr",
            "FX_WAMR_LOCAL_MODEL_CATALOG": "1", "FX_WAMR_MAX_PAGES": "16",
            "FX_WAMR_READONLY_MODULE": "1",
            "FX_WAMR_RUNNER": str(prefix / "build/fx-wamr-host/fx-wamr-targetsem"),
            "FX_WASM_MODULE": str(prefix / "build/fx-compact/bin/fx-core.wasm"),
            "EINKCTL": str(prefix / "build/einkctl-host/einkctl")}


class RuntimeTests(unittest.TestCase):
    def assert_frame(self, path):
        data = path.read_bytes()
        self.assertEqual(data[:11], b"P4\n800 480\n")
        self.assertEqual(len(data), 48011)
        self.assertTrue(any(data[11:]))
        self.assertTrue(any(byte != 255 for byte in data[11:]))
        self.assertEqual(data[11:111], bytes([255])*100)

    def test_acp_session(self):
        with provider() as (url, state):
            env = {**environment(), "FX_WAMR_GATEWAY_BASE_URL": url}
            process = subprocess.Popen([env["FX_WAMR_RUNNER"], env["FX_WASM_MODULE"]],
                                       stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=subprocess.PIPE, text=True, env=env)
            messages = queue.Queue()
            received = []

            def pump():
                for line in process.stdout:
                    messages.put(json.loads(line))

            thread = threading.Thread(target=pump, daemon=True)
            thread.start()

            def request(identifier, method, params):
                process.stdin.write(json.dumps({"jsonrpc": "2.0", "id": identifier,
                                                "method": method, "params": params}) + "\n")
                process.stdin.flush()
                while True:
                    message = messages.get(timeout=20)
                    received.append(message)
                    if message.get("id") == identifier:
                        self.assertNotIn("error", message)
                        return message["result"]
            try:
                info = request(1, "initialize", {"protocolVersion": 1, "clientCapabilities": {}})
                self.assertEqual(info["agentInfo"]["name"], "fx")
                self.assertEqual(info["agentInfo"]["version"], "0.0.8")
                session = request(2, "libfx/new", {})["sessionId"]
                completed = request(3, "session/prompt", {"sessionId": session,
                                    "prompt": [{"type": "text", "text": "Return the fixture marker."}]})
                self.assertEqual(completed["stopReason"], "end_turn")
                self.assertIn(MARKER, json.dumps(received))
                self.assertEqual(state["requests"], 1)
                process.stdin.close()
                self.assertEqual(process.wait(timeout=5), 0)
                self.assertEqual(process.stderr.read(), "")
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                thread.join(timeout=5)
                for stream in (process.stdin, process.stdout, process.stderr):
                    stream.close()

    def test_provider_and_offline_display(self):
        with tempfile.TemporaryDirectory() as scratch, provider() as (url, state):
            frame = Path(scratch) / "answer.pbm"
            env = {**environment(), "FX_WAMR_GATEWAY_BASE_URL": url}
            command = [str(ROOT / "build/fx-eink-host/fx-eink"), "--render", str(frame), "Test the display"]
            response = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(response.returncode, 0, response.stderr)
            self.assertEqual(response.stdout, MARKER + "\n")
            self.assertEqual(state["requests"], 1)
            self.assert_frame(frame)
            fixture = 'Offline "quotes", backslash \\ and\nnewline preserved.'
            env["FX_WAMR_FIXTURE_TEXT"] = fixture
            ready = Path(scratch) / "ready"
            ready.write_text("ready\n")
            response = subprocess.run(command[:-1] + ["--wait-ready", str(ready), command[-1]],
                                      env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(response.returncode, 0, response.stderr)
            self.assertEqual(response.stdout, fixture + "\n")
            self.assertEqual(state["requests"], 1)
            self.assert_frame(frame)

    def test_coding_session(self):
        original = (ROOT / "example/project/battery.sh").read_text()
        repaired = original.replace('"$1" -le 20', '"$1" -lt 20').replace('"$1" -gt 80', '"$1" -ge 80')
        calls = [("read_project", {"path": "SPEC.md"}, "Battery indicator"),
                 ("read_project", {"path": "battery.sh"}, "-le 20"),
                 ("read_project", {"path": "test.sh"}, "checks=0"),
                 ("run_tests", {}, "EXAMPLE_FAIL checks=14 failures=4"),
                 ("replace_battery", {"path": "battery.sh", "content": repaired}, "wrote battery.sh"),
                 ("run_tests", {}, "EXAMPLE_PASS checks=14")]
        with tempfile.TemporaryDirectory() as scratch, provider(calls) as (url, state):
            base = ["podman", "run", "--rm", "--user", "0", "--network", "host",
                    "--volume", f"{ROOT}:/work:ro", "--volume", f"{scratch}:/scratch"]
            subprocess.run(base + [IMAGE, "/bin/sh", "/work/example/prepare-jail.sh",
                                  "/work/example/project", "/scratch/jail"], check=True)
            env = environment(Path("/work"))
            env.update(FX_WAMR_GATEWAY_BASE_URL=url, FX_EXAMPLE_JAIL="/scratch/jail",
                       FX_EXAMPLE_TEST_HELPER="/work/build/example-host/fx-example-test")
            settings = [item for key,value in env.items() if key.startswith(("FX_", "AI_GATEWAY_", "EINKCTL"))
                        for item in ("--env", f"{key}={value}")]
            name = "fx-runtime-test-" + str(os.getpid())
            try:
                response = subprocess.run(base + ["--name", name] + settings + [IMAGE,
                    "/work/build/fx-eink-host/fx-eink", "--render", "/scratch/answer.pbm",
                    (ROOT / "example/prompt.md").read_text()], capture_output=True, text=True, timeout=60)
                self.assertEqual(response.returncode, 0, response.stderr)
                self.assertEqual(state["requests"], 7)
                self.assertEqual(response.stdout, MARKER + "\n")
                self.assertEqual((Path(scratch) / "jail/project/battery.sh").read_text(), repaired)
                for filename in ("SPEC.md", "test.sh"):
                    self.assertEqual((Path(scratch) / "jail/project" / filename).read_bytes(),
                                     (ROOT / "example/project" / filename).read_bytes())
                verified = subprocess.run(base + [IMAGE, "/work/build/example-host/fx-example-test", "/scratch/jail"],
                                          capture_output=True, text=True, timeout=30)
                self.assertEqual(verified.returncode, 0, verified.stderr)
                self.assertIn("EXAMPLE_PASS checks=14", verified.stdout)
                self.assert_frame(Path(scratch) / "answer.pbm")
            finally:
                subprocess.run(["podman", "rm", "--force", name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
