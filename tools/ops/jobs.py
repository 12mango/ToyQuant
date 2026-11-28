#!/usr/bin/env python3
"""Two views of a workflow run's jobs, for `dispatch_deploy.sh logs`: the list, a failed job's log tail, and the log.

    curl ... /actions/runs/<id>/jobs | python3 tools/ops/jobs.py list
    ... | python3 tools/ops/jobs.py failed          # the job ids that did not succeed
    python3 tools/ops/jobs.py tail 40               # the last lines of /tmp/tq/joblog.zip
    python3 tools/ops/jobs.py lines > log.txt       # the whole log, unprefixed, for a parser

The log itself is a zip behind the API, which is why reading it is its own program: a bash function that both parsed JSON
and unpacked an archive was the third quoting layer in one line (2026-11-27). `lines` exists because
`tools/ops/window_ledger.py --from-log` reads a record, not a rendering: `tail`'s two-space prefix would make every field
of the ledger CSV depend on this printer.
"""
import json
import sys
import zipfile


def jobs():
    raw = sys.stdin.read().strip()
    if not raw:
        return []
    return json.loads(raw).get("jobs") or []


def read_log(path="/tmp/tq/joblog.zip"):
    """The job log as text, from the zip the runs endpoint hands back or from the plain text the jobs one does.

    A job's log endpoint redirects to **plain text** and a run's to a zip: try the archive, else read the text, so neither
    endpoint is a special case the caller has to know (measured 2026-11-27: the job endpoint answered text and the reader
    printed "File is not a zip file" instead of the result of the action).

    `lines` prints this without the `  ` prefix `tail` adds: a caller that greps a marker out of it is reading a record
    rather than a rendering, and a prefixed line makes every parse depend on the printer.
    """
    try:
        with zipfile.ZipFile(path) as archive:
            return "\n".join(archive.read(name).decode("utf-8", "replace") for name in archive.namelist())
    except (zipfile.BadZipFile, OSError):
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                return handle.read()
        except OSError:
            return None


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
    if command == "last":
        items = jobs()
        if items:
            print(items[-1].get("id"))
        return 0
    if command == "lines":
        text = read_log()
        if text is None:
            print(f"  (no readable log at {read_log.__defaults__[0]})", file=sys.stderr)
            return 1
        sys.stdout.write(text if text.endswith("\n") else text + "\n")
        return 0
    if command == "tail":
        limit = int(sys.argv[2]) if len(sys.argv) > 2 else 40
        text = read_log()
        if text is None:
            print("  (no readable log: /tmp/tq/joblog.zip is absent)")
            return 1
        if not text.strip():
            print("  (the log is empty)")
        for line in text.splitlines()[-limit:]:
            print("  " + line)
        return 0
    print(f"jobs.py: unknown command {command}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
