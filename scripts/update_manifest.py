#!/usr/bin/env python3
"""Update manifest.json (full history, newest first) and latest.json
(newest version per x1 line + hw) next to it."""
import argparse, json, os, re, subprocess, sys


def die(msg):
    print(f"::error::{msg}")
    sys.exit(1)


def vkey(v):
    return tuple(int(p) for p in v.split("."))


def clean_notes(text):
    text = re.sub(r"\s+", " ", text)
    text = re.sub(r"[^\x20-\x7E]", "", text)
    text = re.sub(r'[{}\[\]"\\]', "", text)     # the ESP JSON reader cannot handle these
    return text.strip()[:60].rstrip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--hw", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--sha256", required=True)
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    if not re.fullmatch(r"\d+\.\d+\.\d+", a.version):
        die(f"bad version {a.version!r}")
    if not re.fullmatch(r"[0-9a-f]{64}", a.sha256):
        die("bad sha256")

    subject = subprocess.run(["git", "log", "-1", "--format=%s", a.commit],
                             capture_output=True, text=True, check=True).stdout
    notes = clean_notes(subject)

    try:
        with open(a.manifest, encoding="utf-8") as f:
            m = json.load(f)
    except FileNotFoundError:
        m = {}
    versions = m.get("versions", [])

    for e in versions:
        if e.get("v") == a.version and e.get("sha256") != a.sha256:
            die(f"version {a.version} already in manifest with a different sha256. Bump GC_VERSION.")

    versions = [e for e in versions if e.get("v") != a.version]
    versions.append({"v": a.version, "hw": a.hw, "url": a.url,
                     "sha256": a.sha256, "size": a.size, "notes": notes})
    versions.sort(key=lambda e: vkey(e["v"]), reverse=True)      # newest first

    latest = {}
    for e in reversed(versions):
        latest[e["hw"]] = e["v"]

    if a.dry_run:
        print(f"OK (dry run): {a.version} hw {a.hw}")
        return

    d = os.path.dirname(os.path.abspath(a.manifest))
    os.makedirs(d, exist_ok=True)
    with open(a.manifest, "w", encoding="utf-8") as f:
        json.dump({"latest": latest, "versions": versions}, f, indent=2)
        f.write("\n")

    best = {}
    for e in versions:
        best.setdefault((e["v"].split(".")[0], e["hw"]), e)
    small = sorted(best.values(), key=lambda e: vkey(e["v"]), reverse=True)
    with open(os.path.join(d, "latest.json"), "w", encoding="utf-8") as f:
        json.dump({"versions": small}, f, indent=1)
        f.write("\n")
    print(f"manifest updated: {a.version} hw {a.hw}")


if __name__ == "__main__":
    main()
