"""Read only an explicitly selected native transcript, including Codex .zst.

No source mutation, transcript replacement, subagent discovery or decompressed
temporary files. The existing exporter still owns date selection and redaction.
"""
import ctypes
from pathlib import Path

def find_transcript(session_id):
    base=Path.home()/'.codex'
    candidates=[p for folder in ('sessions','archived_sessions')
                for suffix in ('.jsonl','.jsonl.zst')
                for p in (base/folder).rglob('*'+session_id+suffix)]
    # Exact SID at the filename end excludes files whose prefix is a parent SID
    # but whose actual session suffix identifies a subagent.
    if len(candidates)!=1:
        raise ValueError('Missing or ambiguous selected transcript: '+session_id+'; candidates='+str(len(candidates)))
    return candidates[0]

class _Input(ctypes.Structure):
    _fields_=[('src',ctypes.c_void_p),('size',ctypes.c_size_t),('pos',ctypes.c_size_t)]
class _Output(ctypes.Structure):
    _fields_=[('dst',ctypes.c_void_p),('size',ctypes.c_size_t),('pos',ctypes.c_size_t)]

def _zstd_chunks(path):
    # Use the installed Git for Windows libzstd by absolute path. Do not alter
    # Python packages, PATH or source compression. Git's native DLL has no
    # MSYS runtime dependency, unlike usr/bin/msys-zstd-1.dll.
    dll=Path('C:/Program Files/Git/mingw64/bin/libzstd.dll')
    if not dll.is_file():raise RuntimeError('Installed native zstd reader unavailable')
    z=ctypes.CDLL(str(dll))
    z.ZSTD_createDStream.argtypes=[];z.ZSTD_createDStream.restype=ctypes.c_void_p
    z.ZSTD_freeDStream.argtypes=[ctypes.c_void_p];z.ZSTD_freeDStream.restype=ctypes.c_size_t
    z.ZSTD_initDStream.argtypes=[ctypes.c_void_p];z.ZSTD_initDStream.restype=ctypes.c_size_t
    z.ZSTD_decompressStream.argtypes=[ctypes.c_void_p,ctypes.POINTER(_Output),ctypes.POINTER(_Input)];z.ZSTD_decompressStream.restype=ctypes.c_size_t
    z.ZSTD_isError.argtypes=[ctypes.c_size_t];z.ZSTD_isError.restype=ctypes.c_uint
    def checked(value):
        if z.ZSTD_isError(value):raise ValueError('Selected compressed transcript is invalid')
        return value
    decoder=z.ZSTD_createDStream()
    if not decoder:raise MemoryError('Cannot allocate transcript decoder')
    before=path.stat();remaining=before.st_size;last=None
    output=ctypes.create_string_buffer(1024*1024)
    try:
        checked(z.ZSTD_initDStream(decoder))
        with path.open('rb') as source:
            while remaining:
                block=source.read(min(remaining,128*1024))
                if not block:raise ValueError('Selected compressed transcript changed while reading')
                remaining-=len(block);input_bytes=ctypes.create_string_buffer(block)
                inp=_Input(ctypes.cast(input_bytes,ctypes.c_void_p),len(block),0)
                while True:
                    out=_Output(ctypes.cast(output,ctypes.c_void_p),len(output),0)
                    prior=inp.pos
                    last=checked(z.ZSTD_decompressStream(decoder,ctypes.byref(out),ctypes.byref(inp)))
                    if out.pos:yield output.raw[:out.pos]
                    if inp.pos==prior and not out.pos:
                        if inp.pos==inp.size:break
                        raise ValueError('Selected transcript decompressor made no progress')
                    if inp.pos==inp.size and (last==0 or out.pos<out.size):break
        if last!=0:raise ValueError('Selected compressed transcript is incomplete')
        after=path.stat()
        if (before.st_size,before.st_mtime_ns)!=(after.st_size,after.st_mtime_ns):
            raise ValueError('Selected compressed transcript changed while reading')
    finally:z.ZSTD_freeDStream(decoder)

def complete_records(path):
    path=Path(path)
    if path.name.endswith('.jsonl'):
        with path.open('rb') as source:
            remaining=path.stat().st_size
            for line in source:
                if len(line)>remaining:break
                remaining-=len(line)
                if not line.endswith(b'\n'):break
                yield line
                if not remaining:break
        return
    if not path.name.endswith('.jsonl.zst'):raise ValueError('Unsupported selected transcript format')
    pending=b''
    for chunk in _zstd_chunks(path):
        parts=(pending+chunk).split(b'\n');pending=parts.pop()
        for line in parts:yield line+b'\n'
    # Match the native JSONL snapshot rule: never export an incomplete record.
    # A valid compressed stream may still end with one incomplete JSONL line.
