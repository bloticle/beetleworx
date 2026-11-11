"""Package completeness checks; shader semantics are verified by the runtime."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('shader_pack', Path(__file__).with_name('verify-prepared-shader-pack.py'))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class ShaderPackTests(unittest.TestCase):
    def test_completeness_and_rejections(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / 'translated-dxbc-v1'
            directory.mkdir()
            first, second = b'A' * 64, b'B' * 80
            (directory / 'first.em2dxbc').write_bytes(first)
            (directory / 'second.em2dxbc').write_bytes(second)
            pack = directory / 'library.em2dxbcpack'

            def bundle(records):
                return b'EM2DXPK1' + struct.pack('<I', len(records)) + b''.join(struct.pack('<I', len(record)) + record for record in records)

            valid = bundle([second, first])  # Bundle ordering need not match filenames.
            pack.write_bytes(valid)
            self.assertEqual(module.verify(root)['records'], 2)
            invalid = {
                'changed bytes': bundle([first, b'C' * 80]),
                'duplicate/missing record': bundle([first, first]),
                'missing count': bundle([first]),
                'truncated record': valid[:-1],
                'trailing data': valid + b'x',
                'wrong magic': b'INVALID!' + valid[8:],
                'oversized extent': b'EM2DXPK1' + struct.pack('<II', 2, 8 * 1024 * 1024 + 1),
            }
            for reason, contents in invalid.items():
                with self.subTest(reason=reason):
                    pack.write_bytes(contents)
                    with self.assertRaises(ValueError):
                        module.verify(root)


if __name__ == '__main__':
    unittest.main()
