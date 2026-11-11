import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).parent
README = (ROOT / "README.md").read_text()

# Logs every docker invocation; answers `compose config` with the fixture
# project name, swallows other `compose` subcommands, and passes the rest to
# the real docker (REAL_DOCKER) so volumes and archives are exercised for real.
FAKE_DOCKER = '''#!/bin/bash
printf '%s\\n' "$*" >> "$FAKE_LOG"
if [ "$1 $2 $3" = "compose config --format" ]; then printf '{"name": "%s"}\\n' "$FAKE_PROJECT"; exit 0; fi
if [ "$1" = compose ] || [ "$1" = info ]; then exit 0; fi
exec "$REAL_DOCKER" "$@"
'''


def readme_blocks(heading):
    """Top-level ```bash blocks of one README section, in order (quoted blocks excluded)."""
    section = re.search(rf"(?ms)^#{{2,3}} {re.escape(heading)}\n(.*?)(?=^#{{2,3}} |\Z)", README)
    assert section, heading
    blocks = re.findall(r"(?ms)^```bash\n(.*?)^```$", section.group(1))
    assert blocks, heading
    return blocks


def readme_block(heading):
    return readme_blocks(heading)[0]


class ReadmeFixture(unittest.TestCase):
    def setUp(self, real_docker="/bin/false"):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        (self.work / "bin").mkdir()
        (self.work / "bin" / "docker").write_text(FAKE_DOCKER)
        (self.work / "bin" / "docker").chmod(0o755)
        self.log = self.work / "docker.log"
        self.log.touch()
        self.environment = dict(os.environ, PATH=f"{self.work / 'bin'}:{os.environ['PATH']}", FAKE_LOG=str(self.log),
                                FAKE_PROJECT=f"readmefixture{os.getpid()}", REAL_DOCKER=real_docker)
        (self.work / ".env").write_text("DOMAIN=turn.invalid\nEMAIL=ops@turn.invalid\nTURN_SECRET=fixture-secret\n"
                                        "PUBLISH_URL=https://cloud.invalid/gulp\n")
        (self.work / ".env").chmod(0o600)

    def run_block(self, heading, stdin=""):
        completed = subprocess.run(["bash", "-euo", "pipefail", "-c", readme_block(heading)], cwd=self.work,
                                   env=self.environment, input=stdin, capture_output=True, text=True, timeout=300)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        return completed

    def docker_calls(self):
        return self.log.read_text().splitlines()

    def assert_non_destructive(self):
        for call in self.docker_calls():
            self.assertNotRegex(call, r"compose down|volume rm|initialize-new-identity")
        self.assertEqual((self.work / ".env").read_text(), "DOMAIN=turn.invalid\nEMAIL=ops@turn.invalid\n"
                         "TURN_SECRET=fixture-secret\nPUBLISH_URL=https://cloud.invalid/gulp\n")
        self.assertEqual((self.work / ".env").stat().st_mode & 0o777, 0o600)


class StagingToProductionTests(ReadmeFixture):
    def test_readme_switch_uses_production_webroot_and_keeps_state(self):
        self.run_block("5. Switch from staging to production certificates")
        calls = self.docker_calls()
        self.assertEqual(len(calls), 2, calls)
        self.assertRegex(calls[0], r"^compose run --rm --no-deps --entrypoint  certbot certbot certonly --webroot -w /var/www/certbot --force-renewal --email ops@turn.invalid --agree-tos --no-eff-email -d turn.invalid$")
        self.assertNotIn("--staging", calls[0])
        self.assertNotIn("--standalone", calls[0])
        self.assertEqual(calls[1], "compose restart nginx coturn")
        self.assert_non_destructive()
        self.assertEqual(sorted(path.name for path in self.work.iterdir()), [".env", "bin", "docker.log"])

    def test_init_sh_hint_points_to_the_same_switch(self):
        subprocess.run(["bash", "-n", str(ROOT / "init.sh")], check=True)
        shutil.copy(ROOT / "init.sh", self.work / "init.sh")
        shutil.copy(ROOT / ".env.example", self.work / ".env.example")
        completed = subprocess.run(["bash", "init.sh", "--staging"], cwd=self.work, env=self.environment, input="y\n" * 8,
                                   capture_output=True, text=True, timeout=120)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertNotIn("fixture-secret", completed.stdout)
        hint = completed.stdout[completed.stdout.index("STAGING certificates"):]
        self.assertIn("certbot certonly --webroot -w /var/www/certbot --force-renewal", hint)
        self.assertIn('--email "ops@turn.invalid" --agree-tos --no-eff-email -d "turn.invalid"', hint)
        self.assertIn("docker compose restart nginx coturn", hint)
        self.assertNotIn("down -v", completed.stdout)
        self.assertNotIn("rm -f .env", completed.stdout)
        self.assertIn("--staging", [call for call in self.docker_calls() if "certonly" in call][0])
        self.assert_non_destructive()


class BackupRestoreTests(ReadmeFixture):
    def setUp(self):
        docker = shutil.which("docker")
        if not docker or subprocess.run([docker, "info"], capture_output=True).returncode:
            raise unittest.SkipTest("docker is not available")
        super().setUp(real_docker=docker)
        self.docker = docker
        self.volumes = {"coturn-data": f"{self.environment['FAKE_PROJECT']}_coturn-data",
                        "certbot-certs": f"{self.environment['FAKE_PROJECT']}_certbot-certs"}
        self.addCleanup(subprocess.run, [docker, "volume", "rm", "-f", *self.volumes.values()], capture_output=True)

    def alpine(self, volume, mount, script, read_only=False):
        suffix = ":ro" if read_only else ""
        completed = subprocess.run([self.docker, "run", "--rm", "-v", f"{volume}:{mount}{suffix}", "alpine", "sh", "-ec", script],
                                   capture_output=True, text=True, timeout=120)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        return completed.stdout

    def listing(self, name):
        mount = {"coturn-data": "/data", "certbot-certs": "/certs"}[name]
        return self.alpine(self.volumes[name], mount, f"cd {mount} && find . -type f | sort | while read -r file; do"
                           " echo \"$file $(stat -c %a:%u:%g \"$file\") $(md5sum < \"$file\" | cut -d' ' -f1)\"; done", read_only=True)

    def test_backup_and_restore_keep_identity_barrier_traffic_and_modes(self):
        state = ('{"version": 1, "binding": {"domain": "turn.invalid", "publishUrl": "https://cloud.invalid/gulp"},'
                 ' "attemptedThroughMs": 1762860000000}')
        self.alpine(self.volumes["coturn-data"], "/data", f"cd /data && printf '%s' '{state}' > capacity_state.json"
                    " && printf 'LAST_TOTAL=123\\nDAILY_BYTES=45\\nMONTHLY_BYTES=67\\nCURRENT_DAY=2026-11-11\\nCURRENT_MONTH=2026-11\\n' > traffic_stats"
                    " && : > publisher.lock && chmod 600 capacity_state.json traffic_stats publisher.lock && chown -R 65534:65534 /data")
        self.alpine(self.volumes["certbot-certs"], "/certs", "mkdir -p /certs/live/turn.invalid && echo cert > /certs/live/turn.invalid/fullchain.pem"
                    " && echo key > /certs/live/turn.invalid/privkey.pem && chmod 600 /certs/live/turn.invalid/privkey.pem")
        before = {name: self.listing(name) for name in self.volumes}
        self.assertIn("./capacity_state.json 600:65534:65534", before["coturn-data"])

        self.run_block("Back up")
        calls = self.docker_calls()
        self.assertEqual(calls[1], "compose stop coturn")
        self.assertEqual(calls[-1], "compose start coturn")
        self.assertEqual((self.work / ".env.backup").stat().st_mode & 0o777, 0o600)
        self.assertEqual((self.work / ".env.backup").read_text(), (self.work / ".env").read_text())
        self.assertTrue((self.work / "coturn-data-backup.tar.gz").exists())
        self.assertTrue((self.work / "certs-backup.tar.gz").exists())

        subprocess.run([self.docker, "volume", "rm", *self.volumes.values()], check=True, capture_output=True)
        (self.work / ".env").unlink()
        self.log.write_text("")
        self.run_block("Restore")
        calls = self.docker_calls()
        self.assertEqual(calls[1:3], ["compose stop coturn", "compose create coturn"])
        self.assertEqual(calls[-1], "compose up -d")
        self.assertEqual({name: self.listing(name) for name in self.volumes}, before)
        self.assertIn('"attemptedThroughMs": 1762860000000', self.alpine(self.volumes["coturn-data"], "/data", "cat /data/capacity_state.json", read_only=True))
        self.assert_non_destructive()


class ReadmeConsistencyTests(unittest.TestCase):
    def test_tester_image_in_readme_matches_dockerfile(self):
        pinned = re.search(r"(?m)^FROM (\S+)", (ROOT / "Dockerfile").read_text()).group(1)
        self.assertIn(f"IMAGE={pinned}", readme_blocks("Testing TURN from the command line")[1])

    def test_documented_blocks_parse_and_never_destroy_state(self):
        for heading in ["Testing TURN from the command line", "Back up", "Restore", "5. Switch from staging to production certificates", "Metrics"]:
            for index, block in enumerate(readme_blocks(heading)):
                with self.subTest(heading=heading, block=index):
                    subprocess.run(["bash", "-n"], input=block, check=True, text=True)
                    self.assertNotRegex(block, r"down -v|rm -f? ?\.env|initialize-new-identity|set -x|echo .*(SECRET|CREDENTIAL)")
        self.assertEqual(README.count("down -v"), 1)
        self.assertIn("Do not use `docker compose down -v`", README)


if __name__ == "__main__":
    unittest.main()
