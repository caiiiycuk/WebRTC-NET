import os
import signal
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

ENTRYPOINT = Path(__file__).parent / "entrypoint.sh"
STUBS = {
    "envsubst": "#!/bin/bash\nexec cat\n",
    # The entrypoint polls certificates every 60 seconds; shorten only that pause.
    "sleep": "#!/bin/bash\n[ \"$1\" = 60 ] && set -- 1\nexec /bin/sleep \"$@\"\n",
    "turnserver": """#!/bin/bash
while [ $# -gt 0 ]; do [ "$1" = --pidfile ] && pidfile=$2; shift; done
echo $$ > "$pidfile"
echo "turnserver $$ $EPOCHREALTIME" >> "$FIXTURE_LOG"
trap 'exit 0' TERM
while :; do /bin/sleep 1 & wait $!; done
""",
    "python3": """#!/bin/bash
echo "start $$ $EPOCHREALTIME" >> "$FIXTURE_LOG"
read -r mode code < "$FIXTURE_MODE"
if [ "$mode" = exit ]; then echo "exit $$ $EPOCHREALTIME" >> "$FIXTURE_LOG"; exit "$code"; fi
trap 'trap "" TERM INT; echo "term $$ $EPOCHREALTIME" >> "$FIXTURE_LOG"; exit 0' TERM INT
while :; do /bin/sleep 1 & wait $!; done
""",
}


def wait_for(predicate, timeout, message):
    deadline = time.monotonic() + timeout
    while True:
        result = predicate()
        if result:
            return result
        if time.monotonic() > deadline:
            raise AssertionError(message)
        time.sleep(0.05)


class EntrypointTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        stubs = self.root / "stubs"
        stubs.mkdir()
        for name, body in STUBS.items():
            (stubs / name).write_text(body)
            (stubs / name).chmod(0o755)
        for layer in ["etc", "varlib"]:
            (self.root / "upper" / layer).mkdir(parents=True)
            (self.root / "work" / layer).mkdir(parents=True)
        self.log = self.root / "fixture.log"
        self.log.touch()
        self.mode = self.root / "fixture.mode"
        self.set_mode("run")
        self.stdout = open(self.root / "stdout", "w+")
        self.stderr = open(self.root / "stderr", "w+")
        self.addCleanup(self.stdout.close)
        self.addCleanup(self.stderr.close)
        self.process = None

    def set_mode(self, mode):
        self.mode.write_text(mode + "\n")

    def start(self, publish_url="https://cloud.invalid/gulp"):
        environment = {"PATH": f"{self.root / 'stubs'}:/usr/bin:/bin", "DOMAIN": "turn.invalid",
                       "TURN_SECRET": "fixture-secret", "PUBLIC_IP": "203.0.113.1",
                       "FIXTURE_LOG": str(self.log), "FIXTURE_MODE": str(self.mode)}
        if publish_url is not None:
            environment["PUBLISH_URL"] = publish_url
        mounts = " && ".join(
            f"mount -t overlay overlay -o lowerdir={lower},upperdir={self.root / 'upper' / layer},workdir={self.root / 'work' / layer} {lower}"
            for layer, lower in [("etc", "/etc"), ("varlib", "/var/lib")])
        script = (f"{mounts} && mount -t tmpfs tmpfs /run && mkdir -p /etc/letsencrypt/live/turn.invalid && : > /etc/turnserver.conf.template"
                  f" && echo cert > /etc/letsencrypt/live/turn.invalid/fullchain.pem"
                  f" && echo key > /etc/letsencrypt/live/turn.invalid/privkey.pem && exec bash {ENTRYPOINT}")
        self.process = subprocess.Popen(["unshare", "-rm", "bash", "-c", script], env=environment,
                                        stdout=self.stdout, stderr=self.stderr, start_new_session=True)
        self.addCleanup(self.cleanup_group)
        wait_for(lambda: self.records("turnserver"), 5, "turnserver did not start")
        return self.process

    def cleanup_group(self):
        try:
            os.killpg(self.process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        if self.process.poll() is None:
            self.process.wait(5)

    def records(self, kind):
        return [(int(pid), float(moment)) for name, pid, moment in
                (line.split() for line in self.log.read_text().splitlines()) if name == kind]

    def group_empty(self):
        try:
            os.killpg(self.process.pid, 0)
        except ProcessLookupError:
            return True
        return False

    def assert_stopped(self, code, timeout=5):
        self.assertEqual(self.process.wait(timeout), code)
        wait_for(self.group_empty, 3, "processes left in the entrypoint process group")
        self.stdout.seek(0)
        self.stderr.seek(0)
        output = self.stdout.read() + self.stderr.read()
        self.assertNotIn("fixture-secret", output)
        return output

    def test_restarts_after_exit_kill_and_repeated_failure_without_touching_coturn(self):
        self.set_mode("exit 0")
        self.start()
        wait_for(lambda: len(self.records("exit")) == 1, 5, "first publisher did not run")
        self.set_mode("exit 3")
        wait_for(lambda: len(self.records("exit")) == 2, 8, "publisher was not restarted after exit 0")
        self.set_mode("run")
        third = wait_for(lambda: len(self.records("start")) == 3 and self.records("start")[2], 8, "publisher was not restarted after exit 3")
        time.sleep(0.2)
        os.kill(third[0], signal.SIGKILL)
        killed = time.time()
        fourth = wait_for(lambda: len(self.records("start")) == 4 and self.records("start")[3], 8, "publisher was not restarted after SIGKILL")
        starts = [moment for _, moment in self.records("start")]
        exits = [moment for _, moment in self.records("exit")]
        for previous, following in [(exits[0], starts[1]), (exits[1], starts[2]), (killed, fourth[1])]:
            self.assertGreaterEqual(following - previous, 4.9)
            self.assertLess(following - previous, 8)
        self.assertEqual(len(self.records("turnserver")), 1)
        self.assertTrue(len({pid for pid, _ in self.records("start")}) == 4)
        self.process.send_signal(signal.SIGTERM)
        output = self.assert_stopped(0)
        for status in [0, 3, 137]:
            self.assertIn(f"publisher exited with status {status}; restarting in 5 seconds", output)
        self.assertEqual(len(self.records("start")), 4)

    def test_term_and_int_stop_running_publisher_without_restart(self):
        for signum in [signal.SIGTERM, signal.SIGINT]:
            with self.subTest(signal=signum.name):
                self.setUp()
                self.start()
                started = wait_for(lambda: self.records("start"), 5, "publisher did not start")
                self.process.send_signal(signum)
                self.assert_stopped(0, timeout=3)
                self.assertEqual([pid for pid, _ in self.records("term")], [started[0][0]])
                self.assertEqual(len(self.records("start")), 1)
                self.doCleanups()

    def test_term_and_int_during_pause_skip_next_start(self):
        for signum in [signal.SIGTERM, signal.SIGINT]:
            with self.subTest(signal=signum.name):
                self.setUp()
                self.set_mode("exit 0")
                self.start()
                wait_for(lambda: self.records("exit"), 5, "publisher did not exit")
                wait_for(lambda: "restarting in 5 seconds" in Path(self.root / "stderr").read_text(), 3, "restart was not announced")
                self.process.send_signal(signum)
                self.assert_stopped(0, timeout=3)
                self.assertEqual(len(self.records("start")), 1)
                self.doCleanups()

    def test_unexpected_turnserver_exit_stops_container_and_publisher(self):
        self.start()
        started = wait_for(lambda: self.records("start"), 5, "publisher did not start")
        os.kill(self.records("turnserver")[0][0], signal.SIGKILL)
        output = self.assert_stopped(1)
        self.assertIn("turnserver exited unexpectedly", output)
        self.assertEqual([pid for pid, _ in self.records("term")], [started[0][0]])
        self.assertEqual(len(self.records("start")), 1)

    def test_empty_or_missing_publish_url_never_starts_publisher(self):
        for publish_url in ["", None]:
            with self.subTest(publish_url=publish_url):
                self.setUp()
                self.start(publish_url)
                time.sleep(1.5)
                self.assertEqual(self.records("start"), [])
                self.process.send_signal(signal.SIGTERM)
                self.assert_stopped(0, timeout=3)
                self.assertEqual(self.records("start"), [])
                self.doCleanups()

    def test_certificate_change_restarts_only_turnserver(self):
        self.start()
        first_turnserver = self.records("turnserver")[0][0]
        started = wait_for(lambda: self.records("start"), 5, "publisher did not start")
        certificate = self.root / "upper" / "etc" / "letsencrypt" / "live" / "turn.invalid" / "fullchain.pem"
        os.utime(certificate, (time.time() + 60, time.time() + 60))
        wait_for(lambda: len(self.records("turnserver")) == 2, 8, "turnserver was not restarted after certificate change")
        self.assertNotEqual(self.records("turnserver")[1][0], first_turnserver)
        time.sleep(0.5)
        self.assertEqual(self.records("start"), started)
        self.assertEqual(self.records("term"), [])
        self.assertIsNone(self.process.poll())
        self.process.send_signal(signal.SIGTERM)
        output = self.assert_stopped(0, timeout=3)
        self.assertIn("Certificate changed", output)
        self.assertEqual(len(self.records("start")), 1)


if __name__ == "__main__":
    unittest.main()
