"""Build border-safe SDR/HDR presenter shaders as SM 5.1 DXBC, never at runtime."""
import ctypes as c
import os
from pathlib import Path


def compile_cas_shaders(root: Path):
    compiler = c.WinDLL(str(Path(os.environ['SystemRoot']) / 'System32/d3dcompiler_47.dll'))

    class Macro(c.Structure):
        _fields_ = [('name', c.c_char_p), ('value', c.c_char_p)]

    compile_shader = compiler.D3DCompileFromFile
    compile_shader.argtypes = [c.c_wchar_p, c.POINTER(Macro), c.c_void_p,
                              c.c_char_p, c.c_char_p, c.c_uint, c.c_uint,
                              c.POINTER(c.c_void_p), c.POINTER(c.c_void_p)]
    compile_shader.restype = c.c_long

    def bytes_of(blob):
        table = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
        pointer = c.WINFUNCTYPE(c.c_void_p, c.c_void_p)(table[3])(blob)
        size = c.WINFUNCTYPE(c.c_size_t, c.c_void_p)(table[4])(blob)
        return c.string_at(pointer, size)

    def release(blob):
        if blob:
            table = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
            c.WINFUNCTYPE(c.c_ulong, c.c_void_p)(table[2])(blob)

    result = {}
    for hdr in (False, True):
        for resample in (False, True):
            for dither in ((False,) if hdr else (False, True)):
                name = ('guest_output_ffx_cas_' + ('resample' if resample else 'sharpen')
                        + ('_hdr' if hdr else '_dither' if dither else '') + '_ps')
                macros = (Macro * 4)(Macro(b'CAS_RESAMPLE', b'1' if resample else b'0'),
                                     Macro(b'CAS_DITHER', b'1' if dither else b'0'),
                                     Macro(b'CAS_HDR', b'1' if hdr else b'0'), Macro())
                shader, errors = c.c_void_p(), c.c_void_p()
                try:
                    # Standard relative file includes; strict syntax and level-3 optimization.
                    status = compile_shader(str(root / 'clamped_cas.hlsl'), macros, 1,
                                            b'main', b'ps_5_1', (1 << 11) | (1 << 15), 0,
                                            c.byref(shader), c.byref(errors))
                    if status < 0:
                        message = bytes_of(errors).decode(errors='replace') if errors else ''
                        raise RuntimeError(f'{name}: D3DCompile {status:#x}\n{message}')
                    data = bytes_of(shader)
                    if data[:4] != b'DXBC':
                        raise RuntimeError(f'{name}: expected Shader Model 5.1 DXBC')
                    result[name] = data
                finally:
                    release(shader)
                    release(errors)
    shader, errors = c.c_void_p(), c.c_void_p()
    try:
        status = compile_shader(str(root / 'hdr_cas_bilinear.hlsl'), None, 1,
                                b'main', b'ps_5_1', (1 << 11) | (1 << 15), 0,
                                c.byref(shader), c.byref(errors))
        if status < 0:
            raise RuntimeError(bytes_of(errors).decode(errors='replace') if errors else 'HDR prefilter compilation failed')
        result['guest_output_hdr_cas_bilinear_ps'] = bytes_of(shader)
    finally:
        release(shader)
        release(errors)
    for easu in (False, True):
        shader, errors = c.c_void_p(), c.c_void_p()
        macros=(Macro*2)(Macro(b'HDR_FSR_EASU',b'1' if easu else b'0'),Macro())
        try:
            status=compile_shader(str(root/'hdr_fsr.hlsl'),macros,1,b'main',b'ps_5_1',
                                  (1<<11)|(1<<15),0,c.byref(shader),c.byref(errors))
            if status<0:
                raise RuntimeError(bytes_of(errors).decode(errors='replace') if errors else 'HDR FSR compilation failed')
            result['guest_output_ffx_fsr_'+('easu' if easu else 'rcas')+'_hdr_ps']=bytes_of(shader)
        finally:
            release(shader)
            release(errors)
    return result
