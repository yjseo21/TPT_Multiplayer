#!/usr/bin/env python3
"""CLI integration: exact save/restart/failed loads. Run with built tpt-server."""
import os
import pathlib
import socket
import subprocess
import sys
import tempfile


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def run(server, *flags, good=True):
    env = dict(os.environ, TPT_ROOM_TOKEN='checkpoint-test-token',
               TPT_ADMIN_TOKEN='checkpoint-test-admin')
    argv = [server, '--port', str(port()), '--paused', '--run-for', '1', *map(str, flags)]
    result = subprocess.run(argv, capture_output=True, text=True, env=env, timeout=30)
    assert (result.returncode == 0) == good, (argv, result.stdout, result.stderr)
    return result


def main(server):
    with tempfile.TemporaryDirectory(prefix='tpt-exact-') as td:
        root = pathlib.Path(td)
        a = root/'first.tptx'
        b = root/'second.tptx'
        run(server, '--seed', '12345', '--exact-save', a)
        assert a.stat().st_size > 20
        run(server, '--exact-load', a, '--exact-save', b)
        assert a.read_bytes() == b.read_bytes(), 'checkpoint changed after reload'
        data = bytearray(a.read_bytes())
        data[-10] ^= 0x81
        bad = root/'corrupt.tptx'
        bad.write_bytes(data)
        run(server, '--exact-load', bad, good=False)
        run(server, '--exact-load', a, '--load', root/'other.cps', good=False)
        run(server, '--exact-load', a, '--seed', '5', good=False)
    print('PASS: checkpoint-save/load byte identity, corruption rejection, incompatible options')


if __name__ == '__main__':
    main(sys.argv[1])
