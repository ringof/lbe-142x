#!/usr/bin/env python3
"""Trace the line-level provenance of every tracked source file from git.

For each surviving line in a file, `git blame` is run with whitespace-
insensitive move/copy detection (`-w -M -C -C -C`) so the line is attributed to
the commit/author that ORIGINALLY wrote it -- even if the code was later moved
or copied between files (e.g. extracted out of main.c into cli_view.c, or UBX
parsing consolidated out of model_mini.c into ubx.c). The author counted is the
commit author of that original line.

This is pure git data -- no heuristics, no judgement about "who should be
credited". It answers exactly one question per file: of the lines that are in
the file right now, who originally wrote each one?

Usage:  python3 scripts/trace_attribution.py [--csv]
Outputs a Markdown table (default) or CSV (--csv) plus a repo-wide summary.
"""
import subprocess
import collections
import sys

# Map commit-author emails seen in this repo's history to display names.
# Anything not listed is reported verbatim under "other" so nothing is hidden.
KNOWN = {
    "bvernoux@gmail.com": "Benjamin Vernoux",
    "davegoncalves@gmail.com": "Dave Goncalves",
    "noreply@anthropic.com": "Claude (AI)",
}
COLS = ["Benjamin Vernoux", "Dave Goncalves", "Claude (AI)", "other"]

# git blame flags: -w ignore whitespace; -M detect moves within a file;
# -C -C -C detect copies/moves from other files (the third -C searches the
# whole history, so a refactor that *moved* code is traced back to its author).
BLAME = ["git", "blame", "-w", "-M", "-C", "-C", "-C", "--line-porcelain", "--"]


def sh(args):
    return subprocess.run(args, capture_output=True, text=True).stdout


def bucket(email):
    e = email.strip("<>").lower()
    return KNOWN.get(e, "other:" + e)


def tracked_source():
    exts = (".c", ".h", ".py")
    return [f for f in sh(["git", "ls-files"]).splitlines() if f.endswith(exts)]


def blame_counts(path):
    c = collections.Counter()
    for line in sh(BLAME + [path]).splitlines():
        if line.startswith("author-mail "):
            c[bucket(line.split(" ", 1)[1])] += 1
    return c


def origin(path):
    """The commit that first added the file (oldest add, following renames)."""
    out = sh(["git", "log", "--follow", "--diff-filter=A",
              "--format=%an, %ad", "--date=short", "--", path]).splitlines()
    return out[-1] if out else "?"


def col_value(counts, col):
    if col == "other":
        return sum(v for k, v in counts.items() if k.startswith("other"))
    return counts.get(col, 0)


def main():
    csv = "--csv" in sys.argv
    files = tracked_source()
    total = collections.Counter()
    rows = []
    for f in files:
        counts = blame_counts(f)
        total.update(counts)
        rows.append((f, sum(counts.values()), counts))

    headers = ["file", "lines"] + COLS + ["origin (added by, date)"]
    if csv:
        print(",".join(headers))
        for f, n, c in rows:
            print(",".join([f, str(n)] + [str(col_value(c, col)) for col in COLS]
                           + ['"%s"' % origin(f)]))
    else:
        print("| " + " | ".join(headers) + " |")
        print("|" + "---|" * len(headers))
        for f, n, c in rows:
            cells = [str(col_value(c, col)) for col in COLS]
            print("| %s | %d | %s | %s |" % (f, n, " | ".join(cells), origin(f)))

    print("\n### Repo-wide surviving-line totals")
    grand = sum(total.values()) or 1
    for k, v in total.most_common():
        print("- %-18s %6d  (%4.1f%%)" % (k + ":", v, 100.0 * v / grand))


if __name__ == "__main__":
    main()
