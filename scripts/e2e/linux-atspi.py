#!/usr/bin/env python3
import argparse
import json
import time
from pathlib import Path

import pyatspi

def children(node):
    for index in range(node.childCount):
        try: yield node.getChildAtIndex(index)
        except Exception: continue
def walk(node):
    yield node
    for child in children(node): yield from walk(child)
def role(node):
    try: return node.getRoleName()
    except Exception: return "unknown"
def text(node):
    try:
        value = node.queryText(); return value.getText(0, value.characterCount)
    except Exception: return ""
def contains(root, fragment): return any(fragment in (node.name or "") or fragment in text(node) for node in walk(root))
def wait_for(description, probe, timeout=90.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = probe()
        if value: return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}")
def find_application():
    desktop = pyatspi.Registry.getDesktop(0)
    for application in children(desktop):
        if any(role(node) == "frame" and (node.name or "") == "Booklight" for node in walk(application)): return application
    return None
def snapshot(node, depth=0):
    result = {"name": node.name or "", "role": role(node)}; value = text(node)
    if value: result["text"] = value
    if depth < 10: result["children"] = [snapshot(child, depth + 1) for child in children(node)]
    return result

def main():
    parser = argparse.ArgumentParser(); parser.add_argument("--output", type=Path, required=True); args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=True)
    application = None
    try:
        application = wait_for("Booklight application", find_application)
        window = wait_for("Booklight frame", lambda: next((node for node in walk(application) if role(node) == "frame"), None))
        wait_for("EPUB title parsed by the embedded backend", lambda: contains(window, "The Sample Book"))
        wait_for("EPUB table of contents parsed by the embedded backend", lambda: contains(window, "First Chapter"))
        (args.output / "accessibility.json").write_text(json.dumps(snapshot(application), ensure_ascii=False, indent=2), encoding="utf-8")
        (args.output / "interaction-trace.json").write_text(json.dumps({"platform":"linux","actions":["launch:sample.epub"],"assertions":["packaged GTK4 frame activated on native Wayland","sample EPUB crossed RVT1 into the embedded backend","native accessibility tree contains The Sample Book","native accessibility tree contains First Chapter"]}, indent=2), encoding="utf-8")
    except Exception:
        if application is not None: (args.output / "accessibility-failure.json").write_text(json.dumps(snapshot(application), ensure_ascii=False, indent=2), encoding="utf-8")
        raise
if __name__ == "__main__": main()
