"""Resolve the live guest mapping through the matching Runtime accessor.

ReadProcessMemory only. ASLR and alternate guest reservations are supported;
unknown accessor machine code fails closed rather than guessing offsets.
"""
import argparse
import ctypes as c
import re
from pathlib import Path


def resolve_guest_base(module_base, map_path, read_host):
    mapping = Path(map_path).read_text(errors='replace')
    preferred = int(re.search(r'Preferred load address is ([0-9a-fA-F]+)', mapping)[1], 16)

    def symbol(name):
        lines = [line for line in mapping.splitlines() if name in line]
        assert len(lines) == 1, 'Missing or ambiguous accessor symbol'
        return int(re.search(r' ([0-9a-fA-F]{16}) ', lines[0])[1], 16) - preferred + module_base

    code = read_host(symbol('?virtual_membase@Runtime@rex@@QEBAPEAEXZ'), 19)
    # mov rax,[rcx+disp32]; test rax,rax; je +5;
    # mov rax,[rax+disp8]; ret; xor eax,eax; ret
    assert code[:3] == bytes.fromhex('488b81') and code[7:14] == bytes.fromhex('4885c07405488b')
    assert code[14] == 0x40 and code[16:] == bytes.fromhex('c331c0'), 'Unknown Runtime accessor'

    def pointer(address):
        return int.from_bytes(read_host(address, 8), 'little')

    runtime = pointer(symbol('?instance_@Runtime@rex@@0PEAV12@EA'))
    assert runtime, 'Runtime is not live'
    memory = pointer(runtime + int.from_bytes(code[3:7], 'little', signed=True))
    assert memory, 'Memory is not live'
    base = pointer(memory + int.from_bytes(code[15:16], 'little', signed=True))
    assert base and read_host(base + 0x820FAB5C, 4) == bytes.fromhex('829bddb0'), 'Guest image identity mismatch'
    return base


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('map', type=Path)
    args = parser.parse_args()
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.restype = c.c_void_p
    kernel.ReadProcessMemory.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_size_t, c.c_void_p]
    kernel.CloseHandle.argtypes = [c.c_void_p]
    psapi = c.WinDLL('psapi')
    psapi.EnumProcessModules.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32, c.c_void_p]
    handle = kernel.OpenProcess(0x410, False, args.pid)
    assert handle, 'Read-only process handle unavailable'
    try:
        modules = (c.c_void_p * 2048)()
        size = c.c_uint32()
        assert psapi.EnumProcessModules(handle, modules, c.sizeof(modules), c.byref(size))
        assert 0 < size.value <= c.sizeof(modules)

        def read(address, count):
            data = c.create_string_buffer(count)
            got = c.c_size_t()
            assert kernel.ReadProcessMemory(handle, address, data, count, c.byref(got)) and got.value == count, hex(address)
            return data.raw

        print(resolve_guest_base(modules[0], args.map, read))
    finally:
        kernel.CloseHandle(handle)
