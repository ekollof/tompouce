#!/usr/bin/env python3
"""Minimal client for a resident tompouce --mcp-socket instance.

Reads the private token and socket path from the instance's owned runtime dir
(default $XDG_RUNTIME_DIR/tompouce, or $XDG_CONFIG_HOME/tompouce/daemon), then
speaks newline-delimited JSON-RPC over the named Unix socket. No TCP port, no
token on the command line, no access to other instances.
"""
import argparse
import json
import os
import socket
import sys
from pathlib import Path


def runtime_dir():
    override = os.environ.get("TOMPOUCE_DAEMON_DIR")
    if override:
        return Path(override)
    xdg_runtime = os.environ.get("XDG_RUNTIME_DIR")
    if xdg_runtime:
        return Path(xdg_runtime) / "tompouce"
    xdg_config = os.environ.get("XDG_CONFIG_HOME")
    if xdg_config:
        return Path(xdg_config) / "tompouce" / "daemon"
    return Path.home() / ".config" / "tompouce" / "daemon"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--daemon-dir", help="isolated daemon runtime directory")
    parser.add_argument("--socket", help="override the socket path")
    parser.add_argument("--token-file", help="override the token file path")
    parser.add_argument("--method", default="tools/call", help="JSON-RPC method")
    parser.add_argument("request", nargs="?", help="JSON object with name/arguments, or a tool name")
    parser.add_argument("--name", help="tool name when request is omitted")
    parser.add_argument("--args", default="{}", help="tool arguments as JSON")
    args = parser.parse_args()

    directory = Path(args.daemon_dir) if args.daemon_dir else runtime_dir()
    sock_path = Path(args.socket) if args.socket else directory / "mcp.sock"
    token_path = Path(args.token_file) if args.token_file else directory / "token"
    try:
        token = token_path.read_text().strip()
    except OSError as error:
        print(f"tompoucectl: cannot read token {token_path}: {error}", file=sys.stderr)
        return 1
    if len(token) != 64 or any(c not in "0123456789abcdef" for c in token):
        print("tompoucectl: invalid token format", file=sys.stderr)
        return 1

    if args.request:
        try:
            payload = json.loads(args.request)
        except ValueError:
            payload = {"name": args.request, "arguments": json.loads(args.args)}
    else:
        if not args.name:
            parser.error("provide a request or --name")
        payload = {"name": args.name, "arguments": json.loads(args.args)}

    message = {"jsonrpc": "2.0", "id": 1, "auth": token,
               "method": args.method, "params": payload}
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(10)
            client.connect(str(sock_path))
            client.sendall((json.dumps(message) + "\n").encode())
            chunks = []
            while True:
                chunk = client.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                if b"\n" in chunk:
                    break
    except OSError as error:
        print(f"tompoucectl: {sock_path}: {error}", file=sys.stderr)
        return 1
    reply = b"".join(chunks).split(b"\n", 1)[0].decode(errors="replace")
    try:
        result = json.loads(reply)
    except ValueError:
        print(reply)
        return 1
    text = result.get("result", {}).get("content", [{}])[0].get("text")
    print(text if text is not None else json.dumps(result))
    return 0 if "error" not in result else 1


if __name__ == "__main__":
    sys.exit(main())
