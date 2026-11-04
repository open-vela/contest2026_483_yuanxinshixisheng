"""Windows Codex Stop hook; project-scoped, local-only, partial native export."""
import contextlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

from export_project_logs import ROOT, TEAM, LOGIN, now, sha
from log_export_core import Cleaner, convert
from log_daily import partition, write_session
from jsonschema import Draft7Validator, FormatChecker

STATE = ROOT / 'private/log-collector'

def atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(dir=path.parent, prefix='.collector-')
    try:
        with os.fdopen(fd, 'wb') as out:
            out.write(data)
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)

def save(path, value):
    atomic(path, (json.dumps(value, ensure_ascii=False, indent=2)+'\n').encode())

@contextlib.contextmanager
def writer_lock():
    import msvcrt
    STATE.mkdir(parents=True, exist_ok=True)
    with (STATE/'writer.lock').open('a+b') as lock:
        # Windows byte-range locks also reject reads from a locked byte.
        # Acquire before initialization, including for an empty lock file.
        lock.seek(0)
        msvcrt.locking(lock.fileno(), msvcrt.LK_LOCK, 1)
        try:
            if os.fstat(lock.fileno()).st_size == 0:
                lock.write(b'0')
                lock.flush()
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)

def in_scope(cwd):
    path = Path(cwd).resolve()
    return path == ROOT.parent or path == ROOT or ROOT in path.parents

def collect(payload):
    if payload.get('hook_event_name') != 'Stop' or not in_scope(payload.get('cwd', '/')):
        return {'status': 'skipped', 'reason': 'event or workspace outside scope'}
    sid = payload.get('session_id', '')
    if sid in {'01a07ed4-3f0d-7450-8bb1-bb756849cb4e', '01a13989-4404-7142-9ecc-15546ceddbc3', '01a15df2-df64-7f93-9eaa-8d577c09db8d', '01a176ee-ed35-7143-a002-ee0b40c08ca8', '01a18695-8083-70a0-bfb7-3d02bcd1fc2e', '01a19a88-1ab7-7c10-a3db-a3450cbf218e', '01a19c94-f9d4-75a0-a1aa-88e2c18c2bf4'}:
        # The finals monitor owns these streams and freezes pre-finals files.
        # A full-session Stop export would violate the user's retained history.
        return {'status': 'skipped', 'reason': 'managed by scoped finals file monitor'}
    if not re.fullmatch(r'[0-9a-fA-F-]{36}', sid):
        raise ValueError('Invalid session ID')
    home = Path(os.environ.get('CODEX_HOME', str(Path.home()/'.codex'))).resolve()
    transcript = Path(payload['transcript_path']).resolve()
    if not any(base == transcript.parent or base in transcript.parents for base in (home/'sessions', home/'archived_sessions')):
        raise ValueError('Transcript is outside Codex session storage')
    raw = transcript.read_bytes()
    first = json.loads(raw.splitlines()[0])
    meta = first['payload']
    if isinstance(meta.get('source'), dict) or meta.get('forked_from_id'):
        return {'status': 'skipped', 'reason': 'subagent or fork requires explicit selection'}
    if not in_scope(meta['cwd']):
        return {'status': 'skipped', 'reason': 'source workspace outside scope'}
    # Fail closed if the project's known-secret redaction configuration is unavailable.
    auth = json.loads((ROOT.parent/'本地私密配置/wireless-auth.json').read_text(encoding='utf-8-sig'))
    cleaner = Cleaner(auth.get('redaction_values', []) + [auth['password']])
    events, report = convert(raw, cleaner, expected_sid=sid, expected_workspace=meta['cwd'])
    schema = ROOT/'tools/official-validator/schema'
    validator = Draft7Validator(json.loads((schema/'event.schema.json').read_bytes()), format_checker=FormatChecker())
    for seq, event in enumerate(events):
        event.update(schema_version='1.0', session_id=sid, team_id=TEAM, github_login=LOGIN, tool='codex', seq=seq)
        validator.validate(event)
    blob = ''.join(json.dumps(event, ensure_ascii=False)+'\n' for event in events).encode()
    if any(secret.encode() in blob for secret in cleaner.secrets):
        raise ValueError('Redaction verification failed')
    last_day = max(partition(events, sid, report['started_at']))
    rel = f'logs/{LOGIN}/{last_day}/codex__{sid}.jsonl'
    item = dict(session_id=sid, tool='codex', started_at=report['started_at'], last_event_at=events[-1]['ts'],
                event_count=len(events), file_path=rel, collection_mode='vscode_extension_partial', health='degraded',
                collection_method='native-jsonl-stop-hook', source_surface=meta.get('originator', 'Codex'),
                data_completeness_warning='Partial native transcript export. No internal reasoning, system/developer prompts, media or secrets. Source truncation and outstanding tool calls cannot be recovered. Subagents and forks are not automatically included. Format validation is not contest acceptance.',
                sha256=sha(blob), redacted_count_total=sum(cleaner.counts.values()))
    with writer_lock():
        manifest_path = ROOT/f'logs/{LOGIN}/manifest.json'
        manifest = json.loads(manifest_path.read_bytes())
        if manifest['team_id'] != TEAM or manifest['github_login'] != LOGIN:
            raise ValueError('Existing manifest identity mismatch')
        previous = next((s for s in manifest['sessions'] if (s['tool'], s['session_id']) == ('codex', sid)), None)
        if previous and previous['event_count'] > len(events):
            raise ValueError('Refusing to replace a newer session with a shorter snapshot')
        manifest['sessions'] = [s for s in manifest['sessions'] if (s['tool'], s['session_id']) != ('codex', sid)] + [item]
        manifest.update(updated_at=now(), generator='velavision-native-hook@1.0')
        Draft7Validator(json.loads((schema/'manifest.schema.json').read_bytes()), format_checker=FormatChecker()).validate(manifest)
        parts=write_session(ROOT, LOGIN, sid, events, report['started_at'])
        item.update(files=parts,sha256_scope='complete session stream')
        save(manifest_path, manifest)
        save(STATE/f'{sid}.json', report)
        result = subprocess.run([sys.executable, '-X', 'utf8', str(ROOT/'tools/official-validator/tools/validate-log.py'), str(ROOT/'logs')], capture_output=True, encoding='utf-8')
        atomic(STATE/'official-validation.txt', (result.stdout+result.stderr).encode())
        if result.returncode:
            raise RuntimeError('Official validation failed; see private/log-collector/official-validation.txt')
    return dict(status='collected', session_id=sid, event_count=len(events), file_path=rel,
                captured_at=now(), official_validation_exit_code=0, invocation='hook-input')

def main():
    try:
        raw_input = sys.stdin.buffer.read().decode('utf-8-sig')
        if not raw_input.strip():
            raise ValueError('Empty hook input')
        status = collect(json.loads(raw_input))
        if status['status'] != 'skipped':
            save(STATE/'status.json', status)
    except Exception as exc:
        # Do not print transcript content or secret-bearing exception details.
        save(STATE/'error.json', {'at': now(), 'error_type': type(exc).__name__,
                                'input_characters': len(locals().get('raw_input', ''))})
        print('VelaVision log collector failed; inspect private/log-collector/error.json', file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
