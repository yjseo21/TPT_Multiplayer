"""Integration qualification for fixed-seed command recording and independent replay.

Requires Meson-built tpt-server, tpt-remote, and tpt-replay. The standalone
TickTimeline unit test is runnable without the complete upstream build.
"""
import json
import runpy
from pathlib import Path
import subprocess
import sys
import tempfile

fixture = runpy.run_path(str(Path(__file__).with_name('network_test.py')))
server_path, client_path, replay = sys.argv[1:4]
assert fixture['SERVER'] == str(Path(server_path).resolve())

with tempfile.TemporaryDirectory(prefix='tpt-replay-') as tmp:
    directory = Path(tmp)
    journal = directory / 'session.jsonl'
    server, port, log, env = fixture['start'](directory, '--seed', '12345', '--journal', str(journal))
    viewer = None
    try:
        viewer = fixture['WS'](port)
        viewer.hello(admin=True, commandCommits=True)

        def submit(seq, **payload):
            viewer.send(dict(seq=seq, **payload))
            commit = ack = None
            for _ in range(300):
                typ, data = viewer.receive()
                assert typ != 'close', data
                if typ != 'text':
                    continue
                if data.get('type') == 'command_commit' and data.get('command', {}).get('seq') == seq:
                    commit = data
                if data.get('type') == 'ack' and data.get('seq') == seq:
                    ack = data
                if commit is not None and ack is not None:
                    assert commit['revision'] == ack['revision']
                    assert commit['id'] == seq, commit
                    return commit
            raise AssertionError(f'missing commit/ack for sequence {seq}')

        first = submit(1, op='brush', x1=100, y1=60, x2=130, y2=60, radius=2, element=1)
        second = submit(2, op='tool', tool='DEFAULT_PT_WATR', action='point',
                        x1=140, y1=80, radius=5)
        assert first['tick'] == second['tick'] == 0
        assert first['ordinal'] == 0 and second['ordinal'] == 1
        submit(3, op='pause', paused=False)
        viewer.frame(tick=5)
        submit(4, op='pause', paused=True)
        submit(5, op='step')
        cleared = submit(6, op='clear')
        assert cleared['epoch'] == 1
    finally:
        if viewer:
            viewer.close()
        fixture['stop'](server, log)

    records = [json.loads(line) for line in journal.read_text().splitlines()]
    assert records[0]['kind'] == 'header'
    assert len([r for r in records if r['kind'] == 'edit']) == 6
    assert records[-1]['kind'] == 'check'
    assert all('token' not in json.dumps(r).lower() for r in records)

    outputs = []
    for i in range(2):
        result_path = directory / f'digests-{i}.jsonl'
        result = subprocess.run([replay, '--journal', str(journal), '--digest-output', str(result_path)],
                                capture_output=True, text=True, timeout=120)
        assert result.returncode == 0, result.stdout + result.stderr
        outputs.append(result_path.read_bytes())
    assert outputs[0] == outputs[1], 'independent replays must produce identical checks'

    # The replay checker must fail visibly at the very first differing field.
    broken = directory / 'tampered.jsonl'
    fields = [r for r in records if r['kind'] == 'check'][-1]['fields']
    for record in reversed(records):
        if record['kind'] == 'check':
            record['fields']['particle-slots'] = '0000000000000000' if fields['particle-slots'] != '0000000000000000' else 'ffffffffffffffff'
            break
    broken.write_text(''.join(json.dumps(record) + '\n' for record in records))
    rejected = subprocess.run([replay, '--journal', str(broken)], capture_output=True, text=True, timeout=120)
    assert rejected.returncode != 0 and 'DIVERGENCE' in rejected.stderr, rejected.stdout + rejected.stderr
    print('PASS: server-ordered commits, paused edits, step, reset epochs, 2 independent replays and tamper detection')
