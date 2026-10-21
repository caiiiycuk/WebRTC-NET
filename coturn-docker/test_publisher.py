import unittest
import json
import tempfile
import io
import signal
import time
import subprocess
import sys
import threading
import queue
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import MagicMock, patch

from publisher import MAX_SAFE_INTEGER, METRICS_BODY_LIMIT, Publisher, admission_limit, capacity_snapshot, cycle_clock, main


def metrics(used="7", total="600", timestamp="1000", realm="turn.invalid"):
    return (f'turn_quota_used{{realm="{realm}"}} {used}\n'
            f'turn_quota_limit{{scope="total",realm="{realm}"}} {total}\n'
            f'turn_quota_limit{{realm="{realm}",scope="user"}} 128\n'
            f'turn_observability_snapshot_timestamp_seconds {timestamp}\n')


class CapacityParserTests(unittest.TestCase):
    def test_limits(self):
        for total, expected in [("600", 540), ("9000", 8100), ("1000", 900), ("601", 540),
                                (str(MAX_SAFE_INTEGER), MAX_SAFE_INTEGER * 9 // 10)]:
            with self.subTest(total=total):
                self.assertEqual(admission_limit({"TOTAL_QUOTA": total}), (int(total), expected))
        self.assertEqual(admission_limit({}), (600, 540))
        self.assertEqual(admission_limit({"TOTAL_QUOTA": ""}), (600, 540))
        self.assertEqual(admission_limit({"TOTAL_QUOTA": "600", "ADMISSION_LIMIT": "550"}), (600, 550))
        self.assertEqual(admission_limit({"TOTAL_QUOTA": "0", "ADMISSION_LIMIT": "500"}), (0, 500))

    def test_invalid_limits(self):
        for value in ["", "0", "-1", "0.5", "true", "1e2", "601", str(MAX_SAFE_INTEGER + 1)]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                admission_limit({"TOTAL_QUOTA": "600", "ADMISSION_LIMIT": value})
        for total in ["0", "1", "-1", "600.0", "true", str(MAX_SAFE_INTEGER + 1)]:
            with self.subTest(total=total), self.assertRaises(ValueError):
                admission_limit({"TOTAL_QUOTA": total})

    def test_snapshot_and_boundaries(self):
        for now in [998000, 1000000, 1005000]:
            snapshot = capacity_snapshot(metrics(used="6.01e2"), "turn.invalid", {}, now)
            self.assertEqual(snapshot["used"], 601)
            self.assertEqual(snapshot["admissionLimit"], 540)
            self.assertEqual(snapshot["observedAtMs"], 1000000)
        for now in [997999, 1005001]:
            with self.subTest(now=now), self.assertRaises(ValueError):
                capacity_snapshot(metrics(), "turn.invalid", {}, now)

    def test_invalid_samples(self):
        for body in [metrics(timestamp="0"), metrics(realm="other.invalid"), metrics(total="601"),
                     metrics() + metrics(), metrics().splitlines()[0],
                     metrics().replace('scope="total"', 'scope="other"'),
                     metrics().replace('realm="turn.invalid"', 'realm="turn.invalid",realm="turn.invalid"')]:
            with self.subTest(body=body), self.assertRaises(ValueError):
                capacity_snapshot(body, "turn.invalid", {}, 1000000)
        for value in ["NaN", "Inf", "-1", "1.5", '"7"', "true", str(MAX_SAFE_INTEGER + 1), "1e9999"]:
            for field in ["used", "total", "timestamp"]:
                with self.subTest(value=value, field=field), self.assertRaises(ValueError):
                    capacity_snapshot(metrics(**{field: value}), "turn.invalid", {}, 1000000)

    def test_exact_escaped_realm(self):
        self.assertEqual(capacity_snapshot(metrics(realm='turn\\"\\\\invalid'), 'turn"\\invalid', {}, 1000000)["used"], 7)


class PublisherTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.environment = {"DOMAIN": "turn.invalid", "TURN_SECRET": "fixture-secret",
                            "PUBLISH_URL": "https://cloud.invalid/gulp"}
        self.state_path = self.directory / "capacity_state.json"
        self.state_path.write_text(json.dumps({"version": 1, "binding": {"domain": "turn.invalid",
                                   "publishUrl": self.environment["PUBLISH_URL"]}, "attemptedThroughMs": 0}))
        self.publisher = Publisher(self.environment, self.directory)

    def test_freeze_failure_and_regression(self):
        first = self.publisher.capacity(metrics(), 1000000)
        self.assertEqual(self.publisher.capacity(metrics(used="99"), 1000000), first)
        self.assertEqual(self.publisher.capacity("", 1000000)["state"], "unknown")
        self.assertEqual(self.publisher.capacity(metrics(), 1000000)["state"], "unknown")
        newer = self.publisher.capacity(metrics(timestamp="1001"), 1001000)
        self.assertEqual(newer["state"], "known")
        self.assertEqual(self.publisher.capacity(metrics(), 1001000)["blockedThroughMs"], 1001000)
        self.assertEqual(self.publisher.capacity(metrics(timestamp="1001"), 1001000)["state"], "unknown")

    def test_configuration_changes(self):
        for changes in [{"ADMISSION_LIMIT": "550"}, {"ADMISSION_LIMIT": "540"}, {"TOTAL_QUOTA": "600"}]:
            with self.subTest(changes=changes):
                publisher = Publisher(self.environment.copy(), self.directory)
                watermark = publisher.attempted // 1000 + 1000
                self.assertEqual(publisher.capacity(metrics(timestamp=str(watermark)), watermark * 1000)["state"], "known")
                publisher.environment.update(changes)
                self.assertEqual(publisher.capacity(metrics(timestamp=str(watermark)), watermark * 1000)["state"], "unknown")

    def test_durable_barrier_before_send_restart_and_lost_ack(self):
        self.publisher.capacity(metrics(), 1000000)
        self.assertEqual(json.loads(self.state_path.read_text())["attemptedThroughMs"], 1000000)
        restarted = Publisher(self.environment, self.directory)
        self.assertEqual(restarted.capacity(metrics(used="20"), 1000000)["state"], "unknown")
        self.assertEqual(restarted.capacity(metrics(timestamp="1001"), 1001000)["state"], "known")
        self.assertEqual(self.state_path.stat().st_mode & 0o777, 0o600)

    def test_write_failure(self):
        self.publisher.capacity(metrics(), 1000000)
        with patch("publisher.durable_write", side_effect=OSError("fixture failure")):
            result = self.publisher.capacity(metrics(timestamp="1001"), 1001000)
        self.assertEqual(result["state"], "unknown")
        self.assertEqual(result["blockedThroughMs"], 1000000)
        self.assertEqual(json.loads(self.state_path.read_text())["attemptedThroughMs"], 1000000)

    def test_process_killed_before_ack_restarts_with_unknown(self):
        requests = queue.Queue()
        release_response = threading.Event()
        state_path = self.state_path

        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                requests.put((payload, json.loads(state_path.read_text())))
                if payload["capacity"]["state"] == "known" and payload["capacity"]["observedAtMs"] == 1000000:
                    release_response.wait(10)
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

            def log_message(self, format, *args):
                pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.addCleanup(server.server_close)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(server.shutdown)
        self.addCleanup(release_response.set)
        environment = dict(self.environment, PUBLISH_URL=f"http://127.0.0.1:{server.server_port}/gulp")
        self.state_path.write_text(json.dumps({"version": 1, "binding": {
            "domain": environment["DOMAIN"], "publishUrl": environment["PUBLISH_URL"]}, "attemptedThroughMs": 0}))
        code = '''
import json, sys, urllib.request
from pathlib import Path
from publisher import Publisher
publisher = Publisher(json.loads(sys.argv[1]), Path(sys.argv[2]))
capacity = publisher.capacity(sys.argv[3], int(sys.argv[4]))
request = urllib.request.Request(publisher.environment["PUBLISH_URL"],
    data=json.dumps(publisher.payload(capacity, 1000)).encode(), method="POST")
with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=8) as response:
    assert response.status == 200
'''
        for timestamp, expected in [("1000", "known"), ("1000", "unknown"), ("1001", "known")]:
            with self.subTest(timestamp=timestamp, expected=expected):
                process = subprocess.Popen([sys.executable, "-B", "-c", code, json.dumps(environment),
                                            str(self.directory), metrics(timestamp=timestamp), str(int(timestamp) * 1000)],
                                           cwd=Path(__file__).parent, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    payload, persisted = requests.get(timeout=5)
                    self.assertEqual(payload["capacity"]["state"], expected)
                    self.assertEqual(persisted["attemptedThroughMs"], int(timestamp) * 1000)
                    self.assertEqual(len(payload["iceServers"]), 2)
                    if timestamp == "1000" and expected == "known":
                        process.kill()
                        process.communicate(timeout=5)
                        self.assertLess(process.returncode, 0)
                    else:
                        stdout, stderr = process.communicate(timeout=5)
                        self.assertEqual(process.returncode, 0, (stdout, stderr))
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.communicate(timeout=5)

    def test_lost_history_never_uses_cloud_hwm_or_wall_time(self):
        self.publisher.capacity(metrics(), 1000000)
        self.state_path.unlink()
        self.assertIsNone(self.publisher.capacity(metrics(timestamp="1001"), 1001000))
        restarted = Publisher(self.environment, self.directory)
        self.assertIsNone(restarted.capacity(metrics(timestamp="1002"), 1002000))
        self.assertFalse(self.state_path.exists())

    def test_invalid_history_and_binding(self):
        for state in ["null", "[]", "{}", '{"version":true}', '{"attemptedThroughMs":0}']:
            with self.subTest(state=state):
                self.state_path.write_text(state)
                self.assertIsNone(Publisher(self.environment, self.directory).capacity(metrics(), 1000000))

    def test_accounting_repeat_restart_rollover_and_failure(self):
        def traffic(received, sent):
            return f"turn_total_traffic_rcvb {received}\nturn_total_traffic_sentb {sent}\n"
        self.publisher.update_traffic(traffic(100, 200), "2026-10-31")
        self.publisher.update_traffic(traffic(100, 200), "2026-10-31")
        restarted = Publisher(self.environment, self.directory)
        restarted.update_traffic(traffic(150, 200), "2026-10-31")
        self.assertEqual(restarted.stats["DAILY_BYTES"], 350)
        restarted.update_traffic(traffic(175, 200), "2026-11-01")
        self.assertEqual(restarted.stats["DAILY_BYTES"], 25)
        self.assertEqual(restarted.stats["MONTHLY_BYTES"], 25)
        restarted.update_traffic(traffic(10, 20), "2026-11-01")
        self.assertEqual(restarted.stats["DAILY_BYTES"], 55)
        with patch("publisher.durable_write", side_effect=OSError("fixture failure")):
            restarted.update_traffic(traffic(20, 20), "2026-11-01")
        restarted.update_traffic(traffic(20, 20), "2026-11-01")
        restarted.update_traffic("", "2026-11-01")
        self.assertEqual(restarted.stats["DAILY_BYTES"], 65)

    def test_legacy_payload_and_partial_credentials(self):
        payload = self.publisher.payload(None, 1000)
        self.assertNotIn("capacity", payload)
        self.assertEqual(payload["iceServers"][0], {"urls": ["stun:turn.invalid:3478"]})
        self.assertEqual(payload["iceServers"][1]["urls"], ["turn:turn.invalid:3478?transport=udp",
                         "turn:turn.invalid:3478?transport=tcp", "turns:turn.invalid:5349?transport=tcp"])
        self.assertEqual(payload["iceServers"][1]["username"], "1300:publisher:1000")
        self.environment["CREDENTIAL_TTL"] = "invalid"
        self.assertEqual(self.publisher.payload(None, 1000)["iceServers"], [payload["iceServers"][0]])

    def cycle_response(self, body):
        response = MagicMock()
        response.__enter__.return_value = io.BytesIO(body)
        return response

    def test_one_scrape_and_barrier_before_post(self):
        response = self.cycle_response(metrics(timestamp=str(int(time.time()))).encode())

        def post(request, timeout):
            payload = json.loads(request.data)
            self.assertEqual(payload["capacity"]["observedAtMs"], json.loads(self.state_path.read_text())["attemptedThroughMs"])
            self.assertTrue(0 < timeout <= 5)
            return MagicMock()

        with patch("publisher.urllib.request.build_opener") as opener, patch("publisher.urllib.request.urlopen", side_effect=post) as send:
            opener.return_value.open.return_value = response
            self.publisher.cycle(cycle_clock() + 5)
            opener.return_value.open.assert_called_once()
            send.assert_called_once()
        signal.setitimer(signal.ITIMER_REAL, 0)

    def test_oversized_body_and_scrape_failure_keep_base(self):
        for raw in [b"x" * (METRICS_BODY_LIMIT + 1), b"\xff", b""]:
            with self.subTest(size=len(raw)), patch("publisher.urllib.request.build_opener") as opener, patch("publisher.urllib.request.urlopen") as send:
                opener.return_value.open.return_value = self.cycle_response(raw)
                self.publisher.cycle(cycle_clock() + 5)
                self.assertNotIn("capacity", json.loads(send.call_args[0][0].data))
                self.assertEqual(len(json.loads(send.call_args[0][0].data)["iceServers"]), 2)
                signal.setitimer(signal.ITIMER_REAL, 0)

    def test_total_scrape_and_post_deadlines(self):
        def expired(signum, frame):
            raise TimeoutError("fixture deadline")

        previous = signal.signal(signal.SIGALRM, expired)
        self.addCleanup(signal.signal, signal.SIGALRM, previous)
        self.addCleanup(signal.setitimer, signal.ITIMER_REAL, 0)
        started = cycle_clock()
        with patch("publisher.urllib.request.build_opener") as opener, patch("publisher.urllib.request.urlopen") as send:
            opener.return_value.open.side_effect = lambda *args, **kwargs: time.sleep(2)
            self.publisher.cycle(started + 5)
            self.assertLess(cycle_clock() - started, 1.5)
            self.assertNotIn("capacity", json.loads(send.call_args[0][0].data))
            signal.setitimer(signal.ITIMER_REAL, 0)
        started = cycle_clock()
        with patch("publisher.urllib.request.build_opener") as opener, patch("publisher.urllib.request.urlopen", side_effect=lambda *args, **kwargs: time.sleep(10)):
            opener.return_value.open.return_value = self.cycle_response(metrics(timestamp=str(int(time.time()))).encode())
            with self.assertRaises(TimeoutError):
                self.publisher.cycle(started + 5)
        self.assertLess(cycle_clock() - started, 5.5)

        started = cycle_clock()
        with patch("publisher.urllib.request.build_opener") as opener, patch("publisher.urllib.request.urlopen") as send, patch("publisher.durable_write", side_effect=lambda *args: time.sleep(10)):
            opener.return_value.open.return_value = self.cycle_response(metrics(timestamp=str(int(time.time()))).encode())
            with self.assertRaises(TimeoutError):
                self.publisher.cycle(started + 5)
            send.assert_not_called()
        self.assertLess(cycle_clock() - started, 5.5)

    def test_main_loop_has_no_overlap_or_catch_up_and_blocks_after_failure(self):
        clock = [0]
        starts = []

        def cycle(deadline):
            starts.append(clock[0])
            self.assertEqual(deadline, clock[0] + 5)
            if len(starts) == 1:
                clock[0] += 12
                self.publisher.blocked = False
                raise OSError("fixture POST failure")
            self.assertTrue(self.publisher.blocked)
            if len(starts) == 3:
                raise KeyboardInterrupt()

        with (
            patch.dict("publisher.os.environ", self.environment, clear=True),
            patch("publisher.sys.argv", ["publisher.py"]),
            patch("publisher.os.open", return_value=123),
            patch("publisher.fcntl.flock"),
            patch("publisher.Publisher", return_value=self.publisher),
            patch.object(self.publisher, "cycle", side_effect=cycle),
            patch("publisher.signal.signal"),
            patch("publisher.signal.setitimer"),
            patch("publisher.cycle_clock", side_effect=lambda: clock[0]),
            patch("publisher.time.sleep", side_effect=lambda seconds: clock.__setitem__(0, clock[0] + seconds)) as sleep,
        ):
            with self.assertRaises(KeyboardInterrupt):
                main()
        self.assertEqual(starts, [0, 12, 17])
        self.assertEqual([call.args[0] for call in sleep.call_args_list], [0, 5])


if __name__ == "__main__":
    unittest.main()
