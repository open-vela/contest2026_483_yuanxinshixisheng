"""Explicit October finals backfill; freeze every earlier log byte and mtime."""
import json
import hashlib
import subprocess
import sys
from pathlib import Path
from datetime import datetime
from export_project_logs import ROOT, LOGIN, TEAM, now, dump
from auto_collect_logs import writer_lock
from log_export_core import Cleaner, convert
from log_daily import DAILY_SESSIONS, LOCAL_TIME, write_session
from jsonschema import Draft7Validator, FormatChecker
from codex_transcript_source import find_transcript, complete_records

START='2026-10-10'
SIDS=['01a07ed4-3f0d-7450-8bb1-bb756849cb4e','01a13989-4404-7142-9ecc-15546ceddbc3', '01a15df2-df64-7f93-9eaa-8d577c09db8d', '01a176ee-ed35-7143-a002-ee0b40c08ca8', '01a18695-8083-70a0-bfb7-3d02bcd1fc2e', '01a19a88-1ab7-7c10-a3db-a3450cbf218e', '01a19c94-f9d4-75a0-a1aa-88e2c18c2bf4']
def digest(data): return hashlib.sha256(data).hexdigest()
def day(ts): return datetime.fromisoformat(ts.replace('Z','+00:00')).astimezone(LOCAL_TIME).date().isoformat()

def main():
    member=ROOT/f'logs/{LOGIN}'
    with writer_lock():
        frozen={str(p):{'sha256':digest(p.read_bytes()),'mtime_ns':p.stat().st_mtime_ns} for p in member.glob('*/*.jsonl') if p.parent.name<START}
        manifest=json.loads((member/'manifest.json').read_bytes())
        auth=json.loads((ROOT.parent/'本地私密配置/wireless-auth.json').read_text(encoding='utf-8-sig'))
        values=auth.get('redaction_values',[])+[auth['password']]
        # Read exact known values only from the ignored local configuration.
        # This also protects plaintext user prose and code containing a value
        # without its field name. Never copy the values into source or reports.
        cloud_auth_path=ROOT/'private/gimbal-device-auth.cloud.json'
        if cloud_auth_path.is_file():
            cloud_auth=json.loads(cloud_auth_path.read_text(encoding='utf-8-sig'))
            for key in ('credential','proof'):
                value=cloud_auth.get(key)
                if not isinstance(value,str) or not value:
                    raise ValueError('Local cloud redaction configuration is invalid')
                values.append(value)
        schema=ROOT/'tools/official-validator/schema'
        ev=Draft7Validator(json.loads((schema/'event.schema.json').read_bytes()),format_checker=FormatChecker())
        reports=[];retained_unavailable={}
        for sid in SIDS:
            source=find_transcript(sid)
            selected=[]; positions=[]; fullhash=hashlib.sha256(); total=0;source_first=None;source_last=None
            # Reader snapshots complete native records, including a read-only
            # stream of the same explicitly selected SID if Codex compressed it.
            for index,line in enumerate(complete_records(source),1):
                fullhash.update(line);total+=len(line)
                record=json.loads(line)
                if index==1:
                    meta=record.get('payload',{})
                    if record.get('type')!='session_meta' or meta.get('id')!=sid or str(meta.get('cwd','')).replace('\\','/').rstrip('/').casefold()!='e:/openvela':
                        raise ValueError('Selected transcript metadata does not match authorized session/workspace')
                timestamp=record.get('timestamp')
                if timestamp:
                    source_first=min(source_first,timestamp) if source_first else timestamp
                    source_last=max(source_last,timestamp) if source_last else timestamp
                if index==1 or (record.get('timestamp') and day(record['timestamp'])>=START):
                    selected.append(line);positions.append(index)
            if len(selected)==1 and source_last and day(source_last)<START:
                # Codex may retain a compressed historical prefix after the
                # formerly collected active source disappears. Never replace
                # delivered finals logs with that shorter prefix or another SID.
                previous=next((s for s in manifest['sessions'] if (s['tool'],s['session_id'])==('codex',sid)),None)
                kept=sorted(member.glob(f'*/codex__{sid}.jsonl'))
                retained_unavailable.update({str(p):{'sha256':digest(p.read_bytes()),'mtime_ns':p.stat().st_mtime_ns} for p in kept})
                reports.append(dict(session_id=sid,source=str(source),source_encoding='zstd-compressed-native-jsonl' if source.name.endswith('.zst') else 'native-jsonl',source_snapshot_digest_scope='complete uncompressed JSONL records',source_snapshot_bytes=total,source_snapshot_sha256=fullhash.hexdigest(),source_first_event_at=source_first,source_last_event_at=source_last,selected_events=0,last_event_at=None,files=[],collection_status='retained_not_updated',uncollected_boundary='Selected native source contains no records in the authorized finals date range; existing delivered files and manifest item retained unchanged, no substitute session selected',retained_manifest_last_event_at=previous.get('last_event_at') if previous else None,retained_event_count=previous.get('event_count') if previous else None,retained_file_count=len(kept)))
                continue
            cleaner=Cleaner(values)
            events,report=convert(b''.join(selected),cleaner,expected_sid=sid,expected_workspace='E:/openvela')
            cleaned_blob=json.dumps(events,ensure_ascii=False).encode('utf-8')
            if any(secret.encode('utf-8') in cleaned_blob for secret in cleaner.secrets):
                raise ValueError('Known-secret redaction verification failed')
            oldfiles=sorted(p for p in member.glob(f'*/codex__{sid}.jsonl') if p.parent.name<START)
            oldevents=[json.loads(line) for p in oldfiles for line in p.read_bytes().splitlines()]
            offset=max((e['seq'] for e in oldevents),default=-1)+1
            for seq,event in enumerate(events,offset):
                event.update(schema_version='1.0',session_id=sid,team_id=TEAM,github_login=LOGIN,tool='codex',seq=seq)
                event['metadata']['source_line']=positions[event['metadata']['source_line']-1]
                ev.validate(event)
            DAILY_SESSIONS.add(sid)
            parts=write_session(ROOT,LOGIN,sid,events,report['started_at'])
            allfiles=sorted(member.glob(f'*/codex__{sid}.jsonl'))
            blobs=[p.read_bytes() for p in allfiles]
            allparts=[dict(file_path=p.relative_to(ROOT).as_posix(),event_count=len(b.splitlines()),sha256=digest(b)) for p,b in zip(allfiles,blobs)]
            item=dict(session_id=sid,tool='codex',started_at=report['started_at'],last_event_at=events[-1]['ts'],event_count=len(oldevents)+len(events),file_path=parts[-1]['file_path'],files=allparts,collection_mode='vscode_extension_partial',health='degraded',collection_method='explicit-finals-native-jsonl-backfill',sha256=digest(b''.join(blobs)),sha256_scope='preserved history plus selected finals records',data_completeness_warning='Only explicitly selected main sessions (including the user-selected wake/interruption continuation). New export restricted to Beijing dates from 2026-10-10. Earlier exported files frozen; any unexported intervening period intentionally omitted. Export seq continues existing delivered records, not proof of complete source coverage. Internal reasoning, system/developer prompts and media excluded; secrets redacted. Subagent transcripts not separately exported. Active transcript is a snapshot; outstanding calls and source truncation remain unresolved.')
            manifest['sessions']=[s for s in manifest['sessions'] if (s['tool'],s['session_id'])!=('codex',sid)]+[item]
            reports.append(dict(session_id=sid,source=str(source),source_encoding='zstd-compressed-native-jsonl' if source.name.endswith('.zst') else 'native-jsonl',source_snapshot_digest_scope='complete uncompressed JSONL records',source_snapshot_bytes=total,source_snapshot_sha256=fullhash.hexdigest(),source_first_event_at=source_first,source_last_event_at=source_last,collection_status='collected',selected_events=len(events),last_event_at=events[-1]['ts'],files=parts,redactions=dict(cleaner.counts),warnings=report['warnings'],outstanding_tool_call_ids=report['outstanding_tool_call_ids']))
        manifest.update(updated_at=now(),generator='velavision-finals-scoped-backfill@1.0')
        Draft7Validator(json.loads((schema/'manifest.schema.json').read_bytes()),format_checker=FormatChecker()).validate(manifest)
        dump(member/'manifest.json',manifest)
        assert all(p.stat().st_mtime_ns==info['mtime_ns'] and digest(p.read_bytes())==info['sha256'] for name,info in frozen.items() for p in [Path(name)])
        assert all(p.stat().st_mtime_ns==info['mtime_ns'] and digest(p.read_bytes())==info['sha256'] for name,info in retained_unavailable.items() for p in [Path(name)])
        validator=ROOT/'tools/official-validator/tools/validate-log.py'
        result=subprocess.run([sys.executable,'-X','utf8',str(validator),str(ROOT/'logs')],capture_output=True,text=True,encoding='utf-8')
        dump(ROOT/'evidence/finals-log-backfill-20261019.json',dict(checked_at=now(),start_local_date=START,timezone='Asia/Shanghai',frozen_files=frozen,earlier_files_unchanged=True,retained_unavailable_files=retained_unavailable,unavailable_exports_unchanged=True,sessions=reports,official_validation=dict(exit_code=result.returncode,stdout=result.stdout,stderr=result.stderr,validator_sha256=digest(validator.read_bytes())),remote_submission=False))
        print(result.stdout)
        print(json.dumps([dict(session_id=r['session_id'],events=r['selected_events'],last=r['last_event_at'],days=[(p['file_path'].split('/')[-2],p['event_count']) for p in r['files']]) for r in reports]))
        raise SystemExit(result.returncode)

if __name__=='__main__':
    try:
        main()
    except Exception as exc:
        # The watcher captures stderr; retain only safe locations, never the
        # exception message, source text, locals or secret-bearing payloads.
        import traceback
        dump(ROOT/'private/log-collector/backfill-error.json',dict(
            at=now(),error_type=type(exc).__name__,
            locations=[dict(file=frame.filename,line=frame.lineno,function=frame.name)
                       for frame in traceback.extract_tb(exc.__traceback__)]))
        raise SystemExit(1)
