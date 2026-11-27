#!/usr/bin/env python3
"""Two views of a workflow run's jobs, for `dispatch_deploy.sh logs`: the list, and a failed job's log tail.

    curl ... /actions/runs/<id>/jobs | python3 tools/ops/jobs.py list
    ... | python3 tools/ops/jobs.py failed          # the job ids that did not succeed
    python3 tools/ops/jobs.py tail 40               # the last lines of /tmp/tq/joblog.zip

The log itself is a zip behind the API, which is why reading it is its own program: a bash function that both parsed JSON
and unpacked an archive was the third quoting layer in one line (2026-11-27).
"""
import json
import sys
import zipfile


def jobs():
    raw = sys.stdin.read().strip()
    if not raw:
        return []
    return json.loads(raw).get("jobs") or []


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else "list"
    if command == "list":
        for job in jobs():
            print(f"  job {job.get('name')}: {job.get('status')} {job.get('conclusion') or '-'}")
            for step in job.get("steps") or []:
                if step.get("conclusion") not in ("success", "skipped", None):
                    print(f"    step {step.get('number')} {step.get('name')}: {step.get('conclusion')}")
        return 0
    if command == "failed":
        for job in jobs():
            if job.get("conclusion") != "success":
                print(job.get("id"))
        return 0
    if command == "tail":
        limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
        try:
            with zipfile.ZipFile("/tmp/tq/joblog.zip") as archive:
                text = "\n".join(archive.read(name).decode("utf-8", "replace") for name in archive.namelist())
        except (zipfile.BadZipFile, OSError) as error:
            print(f"  (no readable log: {error})")
            return 1
        for line in text.splitlines()[-limit:]:
            print("  " + line)
        return 0
    print(f"jobs.py: unknown command {command}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
