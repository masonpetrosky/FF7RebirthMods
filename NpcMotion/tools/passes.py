"""Group a survey's actions (rdc_survey.py's actions.tsv) into passes: runs of draws with the same render targets and
depth target, and runs of dispatches. Usage: passes.py <actions.tsv> [min draws to list a pass, default 1]
One line per pass: first-last eid, count, kind, targets (texture id, size, format)."""
import re
import sys

TARGET_RE = re.compile(r"(RT|DS)\{([^}]*)\}")
TEX_RE = re.compile(r"tex(\d+)\[(\d+)x(\d+)(?:x\d+)? (\S+)")


def targets(details):
    out = []
    for kind, body in TARGET_RE.findall(details):
        for tid, w, h, fmt in TEX_RE.findall(body):
            out.append("%s:%s %sx%s %s" % (kind, tid, w, h, fmt))
    return tuple(out)


def main():
    path = sys.argv[1]
    minimum = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    passes = []
    for line in open(path, encoding="utf-8"):
        if line.startswith("eid\t"):
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 5:
            continue
        eid, kind, name, markers, details = parts[:5]
        if kind not in ("draw", "dispatch", "clear", "copy"):
            continue
        key = (kind, targets(details) if kind == "draw" else ())
        if passes and passes[-1]["key"] == key:
            passes[-1]["last"] = int(eid)
            passes[-1]["count"] += 1
        else:
            passes.append({"key": key, "first": int(eid), "last": int(eid), "count": 1, "name": name})
    for p in passes:
        if p["count"] < minimum and p["key"][0] == "draw":
            continue
        kind, tg = p["key"]
        print("%7d-%-7d %5d %-8s %s%s" % (p["first"], p["last"], p["count"], kind, "  ".join(tg),
                                         "" if kind == "draw" else "  " + p["name"][:80]))


main()
