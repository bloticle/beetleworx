"""Intrusive live instruction sampling; includes waits, not CPU percentages.

Never sends input or writes guest memory. Each context read briefly suspends
the selected thread, which is always resumed before processing the sample.
Use the linker map from the exact running executable.
"""
import argparse
import ctypes as c
import random
import struct
import time
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--pid', type=int, required=True)
p.add_argument('--thread', type=int, required=True)
p.add_argument('--seconds', type=float, default=15)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
assert 0 < a.seconds <= 60 and c.sizeof(c.c_void_p) == 8
k = c.WinDLL('kernel32', use_last_error=True)
ps = c.WinDLL('psapi', use_last_error=True)
for name, args, result in [
    ('OpenProcess', [c.c_uint32, c.c_bool, c.c_uint32], c.c_void_p),
    ('OpenThread', [c.c_uint32, c.c_bool, c.c_uint32], c.c_void_p),
    ('GetProcessIdOfThread', [c.c_void_p], c.c_uint32),
    ('SuspendThread', [c.c_void_p], c.c_uint32),
    ('ResumeThread', [c.c_void_p], c.c_uint32),
    ('GetThreadContext', [c.c_void_p, c.c_void_p], c.c_bool),
    ('WaitForSingleObject', [c.c_void_p, c.c_uint32], c.c_uint32),
    ('CloseHandle', [c.c_void_p], c.c_bool),
]:
    fn = getattr(k, name)
    fn.argtypes, fn.restype = args, result
ps.EnumProcessModules.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32, c.POINTER(c.c_uint32)]
ps.GetModuleFileNameExW.argtypes = [c.c_void_p, c.c_void_p, c.c_wchar_p, c.c_uint32]
class ModuleInfo(c.Structure):
    _fields_ = [('base', c.c_void_p), ('size', c.c_uint32), ('entry', c.c_void_p)]
ps.GetModuleInformation.argtypes = [c.c_void_p, c.c_void_p, c.POINTER(ModuleInfo), c.c_uint32]
process = k.OpenProcess(0x410, False, a.pid)
thread = k.OpenThread(0x10004A, False, a.thread)
try:
    assert process and thread and k.GetProcessIdOfThread(thread) == a.pid
    modules = (c.c_void_p * 2048)()
    needed = c.c_uint32()
    assert ps.EnumProcessModules(process, modules, c.sizeof(modules), c.byref(needed))
    assert needed.value <= c.sizeof(modules)
    images = []
    for module in modules[:needed.value // c.sizeof(c.c_void_p)]:
        info = ModuleInfo()
        name = c.create_unicode_buffer(32768)
        if (ps.GetModuleInformation(process, module, c.byref(info), c.sizeof(info))
                and ps.GetModuleFileNameExW(process, module, name, len(name))):
            images.append((info.base, info.size, name.value))
    main = next(x for x in images if Path(x[2]).name.lower() == 'beetleworx.exe')
    buffer = c.create_string_buffer(1232 + 16)
    context = (c.addressof(buffer) + 15) & ~15
    samples = []
    rng = random.Random(0x5EED1234)
    deadline = time.monotonic() + a.seconds
    while time.monotonic() < deadline:
        assert k.WaitForSingleObject(thread, 0) == 258, 'Target thread ended'
        c.memset(context, 0, 1232)
        c.c_uint32.from_address(context + 48).value = 0x100001
        assert k.SuspendThread(thread) != 0xFFFFFFFF
        try:
            ok = k.GetThreadContext(thread, context)
            rip = c.c_uint64.from_address(context + 248).value
        finally:
            resumed = k.ResumeThread(thread)
        assert resumed != 0xFFFFFFFF and ok and rip
        samples.append(rip)
        time.sleep(rng.uniform(.001, .003))
    with a.output.open('w', encoding='utf-8') as out:
        out.write('# Intrusive wall-time instruction samples, including waits; not a benchmark\n')
        out.write(f'# image_base {main[0]:X}\n# rip\tmodule_base\tmodule\n')
        for rip in samples:
            module = next((x for x in images if x[0] <= rip < x[0] + x[1]), (0, 0, 'unknown'))
            out.write(f'{rip:X}\t{module[0]:X}\t{module[2]}\n')
    print(f'{len(samples)} samples of thread {a.thread}; target resumed; {a.output}')
finally:
    if thread:
        k.CloseHandle(thread)
    if process:
        k.CloseHandle(process)
