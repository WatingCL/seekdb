#!/usr/bin/env python3
"""Local public-case runner for Windows hosts.

The official protocol_check/_transport.py uses selectors over pipes and
os.killpg, both unavailable on Windows, so it cannot drive a local build here.
This runner keeps the judging contract identical by importing the official
_assertions.same/assert_response and mirroring the request/response ordering,
strict JSON rules, handshake check and restart semantics of _transport.py.

It is a development tool only: the official image-based check still has to pass
in Linux before submission.

Usage (from the repository root):
    py -3 minivectordb/tools/local_check.py --command '["./minivectordb/minivectordb.exe","--data-dir","{data_dir}"]'
    py -3 minivectordb/tools/local_check.py --case sql_crud --case vector_euclidean
"""
import argparse
import json
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "protocol_check"))

from _assertions import assert_response  # noqa: E402  official judging semantics


class ProtocolError(RuntimeError):
    pass


def strict_json(text):
    """Same rules as the official transport: no duplicate keys, no non-finite numbers."""
    def invalid(value):
        raise ValueError("non-finite JSON number: %s" % value)

    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError("duplicate JSON key: %s" % key)
            result[key] = value
        return result

    return json.loads(text, parse_constant=invalid, object_pairs_hook=pairs)


class Reader(threading.Thread):
    """Line reader so that reads can be bounded by a timeout on any OS."""

    def __init__(self, stream):
        super().__init__(daemon=True)
        self.stream = stream
        self.q = queue.Queue()
        self.stopped = False

    def run(self):
        while not self.stopped:
            line = self.stream.readline()
            if not line:
                self.q.put(None)
                return
            self.q.put(line)

    def stop(self):
        self.stopped = True


class Session:
    """One database process plus its (reused) data directory."""

    def __init__(self, config, command, log_path, data_dir):
        self.config = config
        self.command = command
        self.log_path = log_path
        self.data_dir = data_dir
        self.sequence = 0
        self.proc = None
        self.reader = None
        self.deadline = time.monotonic() + config["case_timeout"]
        self.log = open(log_path, "ab", buffering=0)

    def start(self, ready=True):
        argv = [part.replace("{data_dir}", self.data_dir) for part in self.command]
        env = dict(os.environ, VECTORDB_DATA_DIR=self.data_dir)
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=self.log, env=env)
        self.reader = Reader(self.proc.stdout)
        self.reader.start()
        if ready:
            message = self.read(self.config["startup_timeout"])
            if message != {"type": "ready"}:
                raise ProtocolError("expected ready, got %r" % (message,))

    def remaining(self, timeout):
        left = self.deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError("case deadline exceeded")
        return min(timeout, left)

    def read(self, timeout):
        try:
            line = self.reader.q.get(timeout=self.remaining(timeout))
        except queue.Empty:
            raise TimeoutError("JSON response timeout")
        if line is None:
            raise ProtocolError("database exited before response")
        if len(line) > self.config["max_response_bytes"]:
            raise ProtocolError("response exceeds byte limit")
        try:
            value = strict_json(line.decode("utf-8"))
        except (ValueError, UnicodeError) as exc:
            raise ProtocolError("invalid JSON response: %s; stdout line (first 240 bytes)=%r"
                                % (exc, line[:240])) from exc
        if not isinstance(value, dict):
            raise ProtocolError("response must be an object")
        return value

    def request(self, sql=None, mode="exact", op="execute", ident=None):
        if not self.reader.q.empty():
            raise ProtocolError("unsolicited or duplicate response")
        self.sequence += 1
        request = {"id": self.sequence if ident is None else ident, "op": op}
        if sql is not None:
            request.update(sql=sql, search_mode=mode)
        data = (json.dumps(request, ensure_ascii=False, allow_nan=False) + "\n").encode("utf-8")
        self.proc.stdin.write(data)
        self.proc.stdin.flush()
        response = self.read(self.config["request_timeout"])
        if type(response.get("id")) is not type(request["id"]) or response["id"] != request["id"]:
            raise ProtocolError("response id differs from request")
        if type(response.get("ok")) is not bool:
            raise ProtocolError("missing boolean ok")
        return response

    def shutdown(self):
        response = self.request(op="shutdown")
        if response["ok"] is not True:
            raise ProtocolError("shutdown failed")
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=self.remaining(self.config["request_timeout"]))
        except subprocess.TimeoutExpired:
            raise ProtocolError("process did not exit after shutdown")
        extra = self.proc.stdout.read()
        if extra or self.proc.returncode:
            raise ProtocolError("extra stdout or nonzero exit after shutdown")
        self.reader.stop()

    def kill(self):
        """Crash simulation: TerminateProcess is the closest Windows analogue of kill -9."""
        if self.proc and self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait(timeout=10)
        if self.reader:
            self.reader.stop()

    def close(self):
        try:
            self.kill()
        finally:
            self.log.close()


def run_case(session, case, package, config):
    expectations = strict_json((package / case["result"]).read_text(encoding="utf-8"))
    statements = (package / case["test"]).read_text(encoding="utf-8").splitlines()
    index, mode = 0, "exact"
    for line_number, text in enumerate(statements, 1):
        sql = text.strip()
        if not sql or sql.startswith("#"):
            continue
        if sql == "--restart":
            session.shutdown()
            session.start()
        elif sql in ("--mode exact", "--mode ann"):
            mode = sql.split()[1]
        elif sql.startswith("--"):
            raise ValueError("unsupported public directive: %s" % sql)
        else:
            if index >= len(expectations):
                raise ValueError("missing expected result")
            response = session.request(sql, mode)
            try:
                assert_response(response, expectations[index], config)
            except AssertionError as exc:
                raise AssertionError("line %d: %s: %s" % (line_number, sql, exc)) from exc
            index += 1
    if index != len(expectations):
        raise ValueError("SQL/result count mismatch")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--command", required=True, help="JSON argv; {data_dir} placeholder supported")
    parser.add_argument("--case", action="append", help="public case ID; repeat to select multiple")
    parser.add_argument("--package", type=Path, default=ROOT / "mysqltest")
    parser.add_argument("--config", type=Path, default=ROOT / "protocol_check" / "config.json")
    parser.add_argument("--output", type=Path, help="existing directory for reports; default is a temp dir")
    args = parser.parse_args(argv)

    config = strict_json(args.config.read_text(encoding="utf-8"))
    command = strict_json(args.command)
    if not isinstance(command, list) or not command or any(not isinstance(x, str) for x in command):
        parser.error("--command must be a nonempty JSON array of strings")

    manifest = strict_json((args.package / "manifest.json").read_text(encoding="utf-8"))
    cases = manifest["cases"]
    if args.case:
        unknown = set(args.case) - {c["id"] for c in cases}
        if unknown:
            parser.error("unknown public case: " + ", ".join(sorted(unknown)))
        cases = [c for c in cases if c["id"] in args.case]

    out = args.output or (Path(tempfile.gettempdir()) / ("local-check-" + uuid.uuid4().hex[:8]))
    out.mkdir(parents=True, exist_ok=True)

    results = []
    for case in cases:
        before = time.monotonic()
        row = {"id": case["id"], "status": "PASS", "message": ""}
        data_dir = tempfile.mkdtemp(prefix="mvd-data-")
        session = Session(config, command, out / (case["id"] + ".stderr.log"), data_dir)
        try:
            session.start()
            run_case(session, case, args.package, config)
            session.shutdown()
        except (TimeoutError, ProtocolError, ValueError, AssertionError) as exc:
            row.update(status="FAIL", message=str(exc))
        except Exception as exc:  # noqa: BLE001 - report harness-side breakage too
            row.update(status="ERROR", message="%s: %s" % (type(exc).__name__, exc))
        finally:
            session.close()
        row["duration_ms"] = round((time.monotonic() - before) * 1000)
        results.append(row)
        print("%s: %s %s" % (row["id"], row["status"], row["message"]), flush=True)

    passed = all(r["status"] == "PASS" for r in results)
    report = {"scope": "local-public-cases", "passed": passed, "cases": results,
              "note": "Local diagnostic only; run protocol_check with Docker in Linux before submission."}
    (out / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print("Report: %s" % (out / "result.json"))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
