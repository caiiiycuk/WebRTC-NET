import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parent
ENVIRONMENT = {"DOMAIN": "turn.invalid", "TURN_SECRET": "fixture-secret", "PUBLIC_IP": "203.0.113.1",
               "RELAY_PORT_MIN": "49152", "RELAY_PORT_MAX": "65535", "TOTAL_QUOTA": "600", "USER_QUOTA": "600"}
FAMILIES = ["turn_total_traffic_rcvb", "turn_total_traffic_sentb", "turn_quota_used", "turn_quota_limit",
            "turn_observability_snapshot_timestamp_seconds"]

# Runs inside an unprivileged user+network namespace: 10.99.0.1 is a dummy
# interface, so a scrape through it is a real non-loopback HTTP request that
# never leaves the namespace. Prints one JSON object per probed address.
PROBE = '''
import json, subprocess, sys, time, urllib.error, urllib.request
turnserver, config, pidfile, log = sys.argv[1:5]
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with open(log, "w") as output:
    process = subprocess.Popen([turnserver, "-c", config, "--pidfile", pidfile], stdout=output, stderr=subprocess.STDOUT)
try:
    results = {}
    for address in ["127.0.0.1", "10.99.0.1"]:
        deadline = time.monotonic() + 10
        while True:
            try:
                with opener.open(f"http://{address}:9641/metrics", timeout=2) as response:
                    results[address] = {"status": response.status, "body": response.read().decode()}
                break
            except (OSError, urllib.error.URLError) as error:
                reason = error.reason if isinstance(getattr(error, "reason", None), OSError) else error
                settled = address != "127.0.0.1" and "status" in results.get("127.0.0.1", {})
                if isinstance(reason, ConnectionRefusedError) and time.monotonic() < deadline and not settled:
                    time.sleep(0.2)
                    continue
                results[address] = {"error": type(reason).__name__, "log": open(log).read()[-2000:]}
                break
    print(json.dumps(results))
finally:
    process.kill()
    process.wait()
'''


def render(template):
    return subprocess.run(["envsubst"], input=template, capture_output=True, text=True, env=ENVIRONMENT, check=True).stdout


class TemplateTests(unittest.TestCase):
    def test_generated_config_binds_metrics_externally_and_keeps_the_rest(self):
        config = render((ROOT / "turnserver.conf.template").read_text())
        lines = config.splitlines()
        self.assertNotIn("$", config)
        self.assertIn("prometheus", lines)
        self.assertEqual([line for line in lines if line.startswith("prometheus-")], ["prometheus-address=0.0.0.0"])
        self.assertEqual(len([line for line in lines if line.startswith("denied-peer-ip=")]), 13)
        self.assertIn("denied-peer-ip=127.0.0.0-127.255.255.255", lines)
        self.assertIn("realm=turn.invalid", lines)
        self.assertIn("static-auth-secret=fixture-secret", lines)
        self.assertIn('"http://127.0.0.1:9641/metrics"', (ROOT / "publisher.py").read_text())
        self.assertNotIn("9641", (ROOT / "docker-compose.yml").read_text())


class MetricsBindTests(unittest.TestCase):
    def setUp(self):
        self.turnserver = os.environ.get("TURNSERVER") or shutil.which("turnserver")
        if not self.turnserver:
            raise unittest.SkipTest("turnserver binary not found; set TURNSERVER")
        if subprocess.run(["unshare", "-rn", "true"], capture_output=True).returncode:
            raise unittest.SkipTest("unprivileged user+network namespaces are not available")

    def probe(self, directory, bind_address):
        template = (ROOT / "turnserver.conf.template").read_text().replace("prometheus-address=0.0.0.0",
                                                                              f"prometheus-address={bind_address}")
        self.assertIn(f"prometheus-address={bind_address}", template)
        config = directory / "turnserver.conf"
        config.write_text(render(template))
        script = ("ip link add dummy0 type dummy && ip addr add 10.99.0.1/24 dev dummy0 && ip link set dummy0 up"
                  f" && ip link set lo up && exec {sys.executable} -c \"$PROBE\" \"$@\"")
        completed = subprocess.run(["unshare", "-rn", "bash", "-c", script, "probe", self.turnserver, str(config),
                                    str(directory / "turnserver.pid"), str(directory / "turnserver.log")],
                                   capture_output=True, text=True, timeout=60, env=dict(os.environ, PROBE=PROBE), cwd=directory)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return json.loads(completed.stdout.splitlines()[-1])

    def test_external_and_loopback_scrape_then_loopback_only_opt_out(self):
        with tempfile.TemporaryDirectory() as name:
            directory = Path(name)
            results = self.probe(directory, "0.0.0.0")
            for address in ["127.0.0.1", "10.99.0.1"]:
                with self.subTest(address=address):
                    self.assertEqual(results[address].get("status"), 200, results[address])
                    for family in FAMILIES:
                        self.assertIn(f"# TYPE {family} ", results[address]["body"])
                    self.assertRegex(results[address]["body"], r"(?m)^turn_observability_snapshot_timestamp_seconds \d+$")
            results = self.probe(directory, "127.0.0.1")
            self.assertEqual(results["127.0.0.1"].get("status"), 200, results["127.0.0.1"])
            self.assertEqual(results["10.99.0.1"].get("error"), "ConnectionRefusedError", results["10.99.0.1"])


if __name__ == "__main__":
    unittest.main()
