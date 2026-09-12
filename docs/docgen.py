#!/usr/bin/env python3
"""docgen — generated interface reference from the .proto definitions and
YAML schemas so documentation cannot drift (REQ §14)."""
import os, re, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROTO = os.path.join(ROOT, "proto")
SCHEMAS = os.path.join(ROOT, "schemas")
OUT = os.path.join(ROOT, "docs", "interface.md")

def parse_proto(path):
    name = os.path.basename(path)[:-6]
    text = open(path).read()
    pkg = re.search(r"package\s+([\w.]+);", text)
    pkg = pkg.group(1) if pkg else "?"
    services = []
    for m in re.finditer(r"service\s+(\w+)\s*\{(.*?)\n\}", text, re.S):
        svc, body = m.group(1), m.group(2)
        rpcs = []
        for r in re.finditer(r"rpc\s+(\w+)\s*\(([^)]*)\)\s*returns\s*\((stream\s+)?([^)]*)\)", body):
            streaming = "server-stream" if r.group(3) else "unary"
            if "stream " in r.group(2):
                streaming = "client-stream"
            rpcs.append((r.group(1), r.group(2).strip(), r.group(4).replace("stream", "").strip(), streaming))
        services.append((svc, rpcs))
    messages = re.findall(r"^message\s+(\w+)", text, re.M)
    return name, pkg, messages, services

def main():
    out = ["# Arches Instrumentation — Interface Reference",
           "",
           "Generated from proto/ and schemas/ — do not edit by hand.",
           ""]
    for path in sorted(glob.glob(os.path.join(PROTO, "*.proto"))):
        name, pkg, messages, services = parse_proto(path)
        out.append(f"## {name}.proto (package `{pkg}`)")
        out.append("")
        if messages:
            out.append("Messages: " + ", ".join("`" + m + "`" for m in messages))
            out.append("")
        for svc, rpcs in services:
            out.append(f"### service `{svc}`")
            out.append("")
            out.append("| RPC | request | response | streaming |")
            out.append("|-----|---------|----------|-----------|")
            for rpc, req, resp, mode in rpcs:
                out.append(f"| `{rpc}` | `{req}` | `{resp}` | {mode} |")
            out.append("")
    out.append("## YAML Schemas")
    out.append("")
    for path in sorted(glob.glob(os.path.join(SCHEMAS, "*.json"))):
        base = os.path.basename(path)
        title = re.search(r'"title":\s*"([^"]+)"', open(path).read())
        out.append(f"- `{base}` — {title.group(1) if title else ''}")
    out.append("")
    out.append("## Ctrl sockets")
    out.append("")
    out.append("Each daemon: gRPC over `/run/instrumentation/<daemon>.sock` (0660, instr:<daemon>);")
    out.append("raw-UDS ctrl over `/run/instrumentation/<daemon>-ctrl.sock` (0600, root-only);")
    out.append("one command table drives both surfaces (REQ §3.2).")
    open(OUT, "w").write("\n".join(out) + "\n")
    print(f"generated {OUT}")

if __name__ == "__main__":
    main()
