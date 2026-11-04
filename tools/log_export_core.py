import collections, hashlib, json, re
from pathlib import Path
BASE = Path(__file__).resolve().parents[1]
SID = None
OMIT_KEYS = {'internal_chat_message_metadata_passthrough', 'encrypted_content'}
MEDIA_TYPES = {'output_image', 'input_image', 'input_audio', 'audio', 'image', 'image_url', 'video', 'output_audio'}
SENSITIVE_FIELDS = {'credential', 'proof'}

def sha(data):
    return hashlib.sha256(data).hexdigest()

class Cleaner:
    def __init__(self, values):
        self.secrets = set()
        for value in values:
            if not isinstance(value, str) or not value:
                continue
            for _ in range(9):
                self.secrets.add(value)
                value = json.dumps(value, ensure_ascii=False)[1:-1]
        self.counts = collections.Counter()

    def text(self, text):
        for secret in sorted(self.secrets, key=len, reverse=True):
            self.counts['wifi_password'] += text.count(secret)
            text = text.replace(secret, '[Wi-Fi password redacted]')
        # JSON can sit inside a tool's code/patch string rather than a parsed
        # object. Match bounded JSON-encoding layers without decoding or
        # executing that code. A delimiter has 2**layer-1 backslashes; escaped
        # quotes in its value have a different parity at that same layer.
        for layer in range(8, -1, -1):
            slashes = (1 << layer) - 1
            quote = '\\' * slashes + '"'
            pattern = re.compile(re.escape(quote) + r'(credential|proof)' +
                                 re.escape(quote) + r'\s*:\s*' +
                                 re.escape(quote), re.IGNORECASE)
            position = 0
            while True:
                match = pattern.search(text, position)
                if not match:
                    break
                end = match.end()
                closing = None
                while end < len(text):
                    if text[end] == '"':
                        first = end
                        while first and text[first - 1] == '\\':
                            first -= 1
                        if (end - first) % (1 << (layer + 1)) == slashes:
                            closing = end - slashes
                            break
                    end += 1
                marker = '[' + match.group(1).lower() + ' redacted]'
                if closing is None:
                    # A source-truncated secret must not survive in a tail.
                    text = text[:match.end()] + marker
                else:
                    text = text[:match.end()] + marker + text[closing:]
                self.counts['sensitive_field'] += 1
                position = match.end() + len(marker) + len(quote)
        for name, pattern, replacement in [
            ('embedded_media', r'data:(?:image|audio|video)/[^;,\s]+;base64,[A-Za-z0-9+/=\r\n]+', '[embedded media omitted]'),
            ('private_key', r'-----BEGIN [^-]*PRIVATE KEY-----[\s\S]*?-----END [^-]*PRIVATE KEY-----', '[private key redacted]'),
            ('api_key', r'\b(?:tp-[A-Za-z0-9_-]{20,}|sk-(?:proj-)?[A-Za-z0-9_-]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,})', '[API token redacted]'),
            ('bearer', r'\bBearer\s+[A-Za-z0-9._\-+/=]{12,}', 'Bearer [redacted]'),
            ('local_ip', r'\b192\.168\.\d{1,3}\.\d{1,3}\b', '[development LAN IP]'),
            ('mac', r'\b(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}\b', '[device MAC]'),
        ]:
            text, n = re.subn(pattern, replacement, text)
            self.counts[name] += n
        return text

    def value(self, value):
        if isinstance(value, str):
            return self.text(value)
        if isinstance(value, list):
            return [self.value(v) for v in value]
        if isinstance(value, dict):
            if value.get('type') in MEDIA_TYPES:
                self.counts['media_blocks'] += 1
                return {'type': 'omitted_media', 'source_type': value['type']}
            cleaned = {}
            for k, v in value.items():
                if k in OMIT_KEYS:
                    continue
                if k.casefold() in SENSITIVE_FIELDS:
                    cleaned[k] = '[' + k.casefold() + ' redacted]'
                    self.counts['sensitive_field'] += 1
                else:
                    cleaned[k] = self.value(v)
            return cleaned
        return value

def convert(raw, cleaner, expected_sid=SID, expected_workspace=None):
    lines = raw.splitlines()
    records = []
    skipped = collections.Counter()
    for i, line in enumerate(lines, 1):
        try:
            record = json.loads(line)
        except ValueError:
            if i == len(lines) and not raw.endswith(b'\n'):
                skipped['incomplete_final_line'] += 1
                continue
            raise ValueError(f'Invalid JSON at source line {i}') from None
        records.append((i, line, record))
    meta = records[0][2]['payload']
    if records[0][2].get('type') != 'session_meta' or meta.get('id') != expected_sid:
        raise ValueError('Unexpected source session')
    workspace = Path(expected_workspace) if expected_workspace is not None else BASE.parent
    if Path(meta['cwd']).resolve() != workspace.resolve():
        raise ValueError('Source session is outside the requested workspace')
    calls = {}
    events = []
    model = None
    seen_usage = {}
    warnings = []
    for i, line, r in records:
        p = r.get('payload', {})
        kind, subtype = r.get('type'), p.get('type')
        if kind == 'turn_context':
            model = p.get('model', model)
            continue
        event = None
        if kind == 'token_usage_record':
            response_id = p.get('response_id')
            usage = p.get('usage')
            if not response_id or not isinstance(usage, dict):
                skipped['usage_without_response_id_or_usage'] += 1
                continue
            if response_id in seen_usage:
                if seen_usage[response_id] != usage:
                    warnings.append(f'Conflicting usage for response at source line {i}; kept first, no total invented.')
                skipped['duplicate_usage_response'] += 1
                continue
            seen_usage[response_id] = usage
            event = {'role': 'system', 'metadata': {'usage': usage, 'response_id': response_id, 'usage_scope': 'one_source_response'}}
            for target, key in [('tokens_in', 'input_tokens'), ('tokens_out', 'output_tokens')]:
                if isinstance(usage.get(key), int) and usage[key] >= 0:
                    event[target] = usage[key]
        elif kind == 'response_item':
            if subtype == 'message':
                role = p.get('role')
                if role not in ('user', 'assistant') or p.get('channel') in ('analysis', 'summary'):
                    skipped['non_public_message'] += 1
                    continue
                text = '\n'.join(b.get('text', '') for b in p.get('content', []) if b.get('type') in ('input_text', 'output_text', 'text'))
                # Environment wrappers are not user requests. Preserve any request outside them.
                for tag in ('environment_context', 'recommended_plugins', 'in-app-browser-context'):
                    text, n = re.subn(r'<' + tag + r'\b[^>]*>[\s\S]*?</' + tag + '>', '', text)
                    skipped['ambient_context_blocks'] += n
                if not text.strip():
                    skipped['empty_or_media_only_message'] += 1
                    continue
                event = {'role': role, 'text': text}
            elif subtype in ('function_call', 'custom_tool_call', 'function_call_output', 'custom_tool_call_output'):
                cid = p.get('call_id')
                is_call = subtype in ('function_call', 'custom_tool_call')
                name = p.get('name') if is_call else calls.get(cid)
                if not cid or not name:
                    warning = f'Tool record at source line {i} lacks source call ID/name or preceding call; retained without a fabricated association.'
                    warnings.append(warning)
                    event = {'role': 'system', 'metadata': {'collection_warning': warning, 'source_record': {k: v for k, v in p.items() if k not in OMIT_KEYS}}}
                elif is_call:
                    calls[cid] = name
                    inp = p.get('arguments') if subtype == 'function_call' else p.get('input')
                    if subtype == 'function_call' and isinstance(inp, str):
                        try:
                            inp = json.loads(inp)
                        except ValueError:
                            pass
                    event = {'role': 'tool', 'tool_name': name, 'tool_call_id': cid, 'input': inp}
                else:
                    event = {'role': 'tool', 'tool_name': name, 'tool_call_id': cid, 'output': p.get('output')}
            else:
                skipped['response_item:' + str(subtype)] += 1
        else:
            skipped['record:' + str(kind)] += 1
        if event is None:
            continue
        if not r.get('timestamp'):
            raise ValueError(f'Missing real source timestamp at line {i}')
        event['ts'] = r['timestamp']
        if model:
            event['model'] = model
        event.setdefault('metadata', {}).update(source_line=i, source_record_sha256=sha(line))
        before = sum(cleaner.counts.values())
        event = cleaner.value(event)
        event['redacted_count'] = sum(cleaner.counts.values()) - before
        events.append(event)
    if not events:
        raise ValueError('No real events exported')
    # A running transcript can end with an outstanding call. Report it, do not invent results.
    called = {e['tool_call_id'] for e in events if e['role'] == 'tool' and 'input' in e}
    returned = {e['tool_call_id'] for e in events if e['role'] == 'tool' and 'output' in e}
    outstanding = sorted(called - returned)
    report = dict(source_session_id=meta['id'], source_surface=meta.get('originator'), source_type=meta.get('source'),
                  source_cwd=meta['cwd'], started_at=meta['timestamp'], snapshot_cutoff=records[-1][2].get('timestamp'),
                  source_snapshot_bytes=len(raw), source_snapshot_sha256=sha(raw), source_lines=len(lines),
                  exported_events=len(events), roles=dict(collections.Counter(e['role'] for e in events)),
                  exclusions=dict(skipped), redactions=dict(cleaner.counts), warnings=warnings,
                  outstanding_tool_call_ids=outstanding, usage_response_count=len(seen_usage),
                  text_truncation=False, source_modified=False, remote_submission=False)
    return events, report
