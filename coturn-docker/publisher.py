#!/usr/bin/env python3
import decimal
import base64
import datetime
import hashlib
import hmac
import fcntl
import json
import os
import math
import re
import signal
import sys
import tempfile
import time
import urllib.request
import urllib.parse
from pathlib import Path


MAX_SAFE_INTEGER = 9007199254740991
METRICS_BODY_LIMIT = 2 * 1024 * 1024
NUMBER = r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?"
LABEL = re.compile(r'\s*([a-zA-Z_][a-zA-Z_0-9]*)\s*=\s*"((?:[^"\\\n]|\\[\\"n])*)"\s*(,|$)')


def cycle_clock():
    return time.clock_gettime(time.CLOCK_BOOTTIME)


def safe_integer(value, positive=False):
    if not re.fullmatch(NUMBER, value) or len(value) > 128:
        raise ValueError("invalid number")
    number = decimal.Decimal(value)
    if not number.is_finite() or not 0 <= number <= MAX_SAFE_INTEGER:
        raise ValueError("unsafe number")
    if number != number.to_integral_value() or (positive and not number):
        raise ValueError("invalid integer")
    return int(number)


def admission_limit(environment):
    total = environment.get("TOTAL_QUOTA") or "600"
    if not re.fullmatch(r"[0-9]+", total):
        raise ValueError("invalid total quota")
    total = safe_integer(total)
    if "ADMISSION_LIMIT" in environment:
        admission = environment["ADMISSION_LIMIT"]
        if not re.fullmatch(r"[0-9]+", admission):
            raise ValueError("invalid admission limit")
        admission = safe_integer(admission, positive=True)
    else:
        admission = total * 9 // 10
    if not admission or (total and admission > total):
        raise ValueError("invalid admission limit")
    return total, admission


def metric_samples(body):
    samples = []
    for line in body.splitlines():
        if not line or line.startswith("#"):
            continue
        if not re.match(r"turn_(?:quota_used|quota_limit|observability_snapshot_timestamp_seconds|total_traffic_rcvb|total_traffic_sentb)(?:[\s{]|$)", line):
            continue
        match = re.fullmatch(r"([a-zA-Z_:][a-zA-Z_0-9:]*)(?:\{(.*)\})?\s+(\S+)\s*", line)
        if not match:
            raise ValueError("invalid metric sample")
        name, raw_labels, value = match.groups()
        labels = {}
        if raw_labels:
            position = 0
            while position < len(raw_labels):
                label = LABEL.match(raw_labels, position)
                if not label or label[1] in labels:
                    raise ValueError("invalid metric labels")
                labels[label[1]] = re.sub(r'\\([\\"n])', lambda escaped: "\n" if escaped[1] == "n" else escaped[1], label[2])
                position = label.end()
        samples.append((name, labels, value))
    return samples


def capacity_snapshot(body, realm, environment, now_ms):
    total, admission = admission_limit(environment)
    required = {
        "turn_quota_used": {"realm": realm},
        "turn_quota_limit": {"realm": realm, "scope": "total"},
        "turn_observability_snapshot_timestamp_seconds": {},
    }
    values = {}
    for name, labels, value in metric_samples(body):
        if name not in required or labels != required[name]:
            continue
        if name in values:
            raise ValueError("duplicate capacity sample")
        values[name] = safe_integer(value)
    if values.keys() != required.keys():
        raise ValueError("missing capacity sample")
    observed = values["turn_observability_snapshot_timestamp_seconds"] * 1000
    if not 0 < observed <= MAX_SAFE_INTEGER or not -2000 <= now_ms - observed <= 5000:
        raise ValueError("unready or stale capacity source")
    if values["turn_quota_limit"] != total:
        raise ValueError("source quota differs from configuration")
    return {"version": 1, "state": "known", "realm": realm, "observedAtMs": observed,
            "used": values["turn_quota_used"], "totalLimit": total, "admissionLimit": admission}


def durable_write(path, body):
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as output:
            output.write(body)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def supports_stun(configuration, domain):
    options = {}
    for line in configuration.splitlines():
        key, separator, value = line.partition("=")
        key = key.strip()
        if key in {"realm", "listening-port", "no-stun", "secure-stun", "no-udp"}:
            options[key] = value.split("#", 1)[0].strip() if separator else ""
    return (options.get("realm") == domain and options.get("listening-port", "3478") == "3478"
            and all(options.get(flag, "false").lower() in {"0", "no", "false", "off"}
                    for flag in ["no-stun", "secure-stun", "no-udp"]))


class Publisher:
    def __init__(self, environment, directory, stun_enabled):
        self.environment = environment
        self.stun_enabled = stun_enabled
        self.realm = environment["DOMAIN"]
        self.binding = None
        self.state_path = directory / "capacity_state.json"
        self.stats_path = directory / "traffic_stats"
        self.attempted = 0
        self.recovered = False
        self.frozen = None
        self.config = None
        self.blocked = True
        try:
            state = json.loads(self.state_path.read_text())
            if not isinstance(state, dict) or type(state.get("version")) is not int or state.get("version") != 1:
                raise ValueError("invalid state version")
            binding = state["binding"]
            # The saved publishUrl is history metadata; delivery uses the current PUBLISH_URL.
            if (not isinstance(binding, dict) or binding.keys() != {"domain", "publishUrl"}
                    or binding["domain"] != self.realm or not isinstance(binding["publishUrl"], str)):
                raise ValueError("invalid state binding")
            attempted = state["attemptedThroughMs"]
            if type(attempted) is not int or not 0 <= attempted <= MAX_SAFE_INTEGER:
                raise ValueError("invalid state barrier")
            self.binding = binding
            self.attempted = attempted
            self.recovered = True
        except (OSError, ValueError, KeyError, TypeError):
            print("ice-publisher: capacity history unavailable; known disabled", file=sys.stderr)
        today = datetime.date.today()
        self.stats = {"LAST_TOTAL": 0, "DAILY_BYTES": 0, "MONTHLY_BYTES": 0,
                      "CURRENT_DAY": str(today), "CURRENT_MONTH": str(today)[:7]}
        try:
            for line in self.stats_path.read_text().splitlines():
                key, value = line.split("=", 1)
                if key in ("LAST_TOTAL", "DAILY_BYTES", "MONTHLY_BYTES"):
                    if not re.fullmatch(r"[0-9]+", value):
                        raise ValueError("invalid traffic history")
                    self.stats[key] = int(value)
                elif key in ("CURRENT_DAY", "CURRENT_MONTH"):
                    self.stats[key] = value
        except FileNotFoundError:
            pass

    def capacity(self, body, now_ms):
        try:
            if self.recovered:
                try:
                    saved = json.loads(self.state_path.read_text())
                    if saved != {"version": 1, "binding": self.binding, "attemptedThroughMs": self.attempted}:
                        raise ValueError("capacity history changed")
                except (OSError, ValueError):
                    self.recovered = False
                    self.blocked = True
            snapshot = capacity_snapshot(body, self.realm, self.environment, now_ms)
            config = (self.environment.get("TOTAL_QUOTA"), "ADMISSION_LIMIT" in self.environment,
                      self.environment.get("ADMISSION_LIMIT"))
            watermark = snapshot["observedAtMs"]
            if not self.recovered or watermark < self.attempted:
                self.blocked = True
            elif watermark > self.attempted:
                self.blocked = True
                durable_write(self.state_path, json.dumps({"version": 1, "binding": self.binding,
                                                         "attemptedThroughMs": watermark}))
                self.attempted = watermark
                self.frozen = snapshot
                self.config = config
                self.blocked = False
            elif config != self.config:
                self.blocked = True
        except (ValueError, OSError):
            self.blocked = True
        if not self.blocked:
            return self.frozen.copy()
        if self.recovered and self.attempted:
            return {"version": 1, "state": "unknown", "realm": self.realm,
                    "blockedThroughMs": self.attempted}
        return None

    def update_traffic(self, body, today=None):
        values = {}
        try:
            for name, labels, value in metric_samples(body):
                if name not in ("turn_total_traffic_rcvb", "turn_total_traffic_sentb") or labels:
                    continue
                if name in values:
                    raise ValueError("duplicate traffic sample")
                values[name] = safe_integer(value)
            if len(values) != 2:
                return
            total = sum(values.values())
            today = str(today or datetime.date.today())
            updated = self.stats.copy()
            if today != updated["CURRENT_DAY"]:
                updated.update(DAILY_BYTES=0, CURRENT_DAY=today)
            if today[:7] != updated["CURRENT_MONTH"]:
                updated.update(MONTHLY_BYTES=0, CURRENT_MONTH=today[:7])
            delta = total - updated["LAST_TOTAL"] if total >= updated["LAST_TOTAL"] else total
            updated["DAILY_BYTES"] += delta
            updated["MONTHLY_BYTES"] += delta
            updated["LAST_TOTAL"] = total
            durable_write(self.stats_path, "".join(f"{key}={value}\n" for key, value in updated.items()))
            self.stats = updated
        except (ValueError, OSError):
            print("ice-publisher: traffic update unavailable", file=sys.stderr)

    def payload(self, capacity, now_seconds):
        domain = self.realm
        payload = {"domain": domain, "turnSecret": self.environment["TURN_SECRET"],
                   "iceServers": [],
                   "traffic": {"dailyBytes": self.stats["DAILY_BYTES"],
                               "monthlyBytes": self.stats["MONTHLY_BYTES"]}}
        if self.stun_enabled:
            payload["iceServers"].append({"urls": [f"stun:{domain}:3478"]})
        try:
            ttl = int(self.environment.get("CREDENTIAL_TTL") or "300")
            if ttl <= 0:
                raise ValueError("invalid credential lifetime")
            username = f"{now_seconds + ttl}:publisher:{now_seconds}"
            credential = base64.b64encode(hmac.digest(self.environment["TURN_SECRET"].encode(),
                                                     username.encode(), hashlib.sha1)).decode()
            payload["iceServers"].append({"urls": [f"turn:{domain}:3478?transport=udp",
                                                   f"turn:{domain}:3478?transport=tcp",
                                                   f"turns:{domain}:5349?transport=tcp"],
                                          "username": username, "credential": credential})
        except (ValueError, OverflowError):
            print("ice-publisher: TURN credentials unavailable", file=sys.stderr)
        for variable, field in [("PUBLISH_LABEL", "label"), ("PUBLISH_FLAG", "flag")]:
            if self.environment.get(variable):
                payload[field] = self.environment[variable]
        maximum = self.environment.get("PUBLISH_MAX_TRAFFIC_GB", "")
        if re.fullmatch(r"[0-9]+(?:[.][0-9]+)?", maximum):
            maximum = float(maximum) if "." in maximum else int(maximum)
            if not isinstance(maximum, float) or math.isfinite(maximum):
                payload["maxTrafficGb"] = maximum
        if capacity is not None:
            payload["capacity"] = capacity
        return payload

    def cycle(self, deadline):
        body = ""
        signal.setitimer(signal.ITIMER_REAL, min(1, max(0.001, deadline - cycle_clock())))
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open("http://127.0.0.1:9641/metrics", timeout=min(1, deadline - cycle_clock())) as response:
                raw = response.read(METRICS_BODY_LIMIT + 1)
                if len(raw) > METRICS_BODY_LIMIT:
                    raise ValueError("oversized metrics")
                body = raw.decode("utf-8")
        except (OSError, ValueError):
            pass
        finally:
            signal.setitimer(signal.ITIMER_REAL, max(0.001, deadline - cycle_clock()))
        capacity = self.capacity(body, time.time_ns() // 1000000)
        self.update_traffic(body)
        payload = json.dumps(self.payload(capacity, int(time.time())), allow_nan=False).encode()
        remaining = deadline - cycle_clock()
        if remaining <= 0:
            raise TimeoutError("cycle deadline")
        request = urllib.request.Request(self.environment["PUBLISH_URL"], data=payload,
                                         headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(request, timeout=remaining) as response:
            print(f"ice-publisher: HTTP {response.status}", flush=True)


def main():
    environment = dict(os.environ)
    if not environment.get("DOMAIN") or not environment.get("TURN_SECRET"):
        raise ValueError("publisher requires domain and secret")
    if not re.fullmatch(r"[a-zA-Z0-9](?:[a-zA-Z0-9.-]*[a-zA-Z0-9])?", environment["DOMAIN"]):
        raise ValueError("invalid publisher domain")
    url = urllib.parse.urlsplit(environment["PUBLISH_URL"])
    if url.scheme != "https" or not url.hostname or url.username or url.password or url.fragment:
        raise ValueError("publisher requires HTTPS")
    directory = Path("/var/lib/coturn")
    lock = os.open(directory / "publisher.lock", os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    if sys.argv[1:] == ["--initialize-new-identity"]:
        path = directory / "capacity_state.json"
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w") as output:
            json.dump({"version": 1, "binding": {"domain": environment["DOMAIN"], "publishUrl": environment["PUBLISH_URL"]},
                       "attemptedThroughMs": 0}, output)
            output.flush()
            os.fsync(output.fileno())
        descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
        return
    if sys.argv[1:]:
        raise ValueError("unknown publisher argument")
    try:
        stun_enabled = supports_stun(Path("/etc/turnserver.conf").read_text(), environment["DOMAIN"])
    except OSError:
        stun_enabled = False
        print("ice-publisher: STUN configuration unavailable; advertisement disabled", file=sys.stderr)
    publisher = Publisher(environment, directory, stun_enabled)

    def deadline_expired(signum, frame):
        raise TimeoutError("cycle deadline")

    signal.signal(signal.SIGALRM, deadline_expired)
    while True:
        # SAFETY: the SIGALRM handler may raise after cycle() returned, inside
        # the except/finally below or the pause; keep that inside the iteration.
        try:
            started = cycle_clock()
            signal.setitimer(signal.ITIMER_REAL, 5)
            try:
                publisher.cycle(started + 5)
            except Exception as error:
                publisher.blocked = True
                print(f"ice-publisher: cycle failed: {type(error).__name__}: {error}", file=sys.stderr)
            finally:
                signal.setitimer(signal.ITIMER_REAL, 0)
            time.sleep(max(0, started + 5 - cycle_clock()))
        except TimeoutError:
            publisher.blocked = True


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ice-publisher: startup failed: {type(error).__name__}: {error}", file=sys.stderr)
        sys.exit(1)
