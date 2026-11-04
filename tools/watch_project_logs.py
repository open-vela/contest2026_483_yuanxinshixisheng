"""Local file monitor for the explicitly selected ongoing project session.
No model calls, network, Git operations or hardware access.
"""
import argparse
import json
import os
import time
import subprocess
import sys
from pathlib import Path
from auto_collect_logs import ROOT, STATE, collect, save, now
from codex_transcript_source import find_transcript

SID = '01a07ed4-3f0d-7450-8bb1-bb756849cb4e'
SIDS = [SID, '01a13989-4404-7142-9ecc-15546ceddbc3', '01a15df2-df64-7f93-9eaa-8d577c09db8d', '01a176ee-ed35-7143-a002-ee0b40c08ca8', '01a18695-8083-70a0-bfb7-3d02bcd1fc2e', '01a19a88-1ab7-7c10-a3db-a3450cbf218e', '01a19c94-f9d4-75a0-a1aa-88e2c18c2bf4']

def source_stamp():
    result=[]
    for sid in SIDS:
        path=find_transcript(sid); stat=path.stat()
        result.append((str(path),stat.st_mtime_ns,stat.st_size))
    return result

def main():
    import msvcrt
    parser=argparse.ArgumentParser()
    parser.add_argument('--once', action='store_true')
    args=parser.parse_args()
    STATE.mkdir(parents=True, exist_ok=True)
    with (STATE/'watcher.lock').open('a+b') as lock:
        lock.seek(0)
        try:
            msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
        except OSError:
            return
        if os.fstat(lock.fileno()).st_size == 0:
            lock.write(b'0'); lock.flush()
        save(STATE/'watcher.json',dict(pid=os.getpid(),started_at=now(),session_ids=SIDS,interval_seconds=30,scope='Beijing dates >= 2026-10-10; earlier logs frozen'))
        previous=None
        while True:
            try:
                stamp=source_stamp()
                if stamp != previous:
                    runner=Path(sys.executable).with_name('python.exe')
                    proc=subprocess.run([str(runner),'-X','utf8',str(ROOT/'tools/backfill_finals_logs.py')],capture_output=True,text=True,encoding='utf-8',timeout=600)
                    if proc.returncode:
                        raise RuntimeError('Scoped export or official validation failed')
                    report=json.loads((ROOT/'evidence/finals-log-backfill-20261019.json').read_bytes())
                    partial=any(s.get('collection_status')=='retained_not_updated' for s in report['sessions'])
                    save(STATE/'watcher-status.json',dict(status='collected_partial' if partial else 'collected',captured_at=now(),pid=os.getpid(),session_ids=SIDS,official_validation_exit_code=0,earlier_files_unchanged=report['earlier_files_unchanged'],unavailable_exports_unchanged=report.get('unavailable_exports_unchanged',True),sessions=[dict(session_id=s['session_id'],events=s['selected_events'],last_event_at=s['last_event_at'],collection_status=s.get('collection_status','collected'),retained_manifest_last_event_at=s.get('retained_manifest_last_event_at'),uncollected_boundary=s.get('uncollected_boundary')) for s in report['sessions']]))
                    previous=stamp
                save(STATE/'watcher-heartbeat.json',dict(checked_at=now(),pid=os.getpid(),status='watching',session_ids=SIDS))
                if args.once:
                    return
            except Exception as exc:
                save(STATE/'watcher-error.json',dict(at=now(),error_type=type(exc).__name__))
                if args.once:
                    raise
            time.sleep(30)

if __name__=='__main__': main()
