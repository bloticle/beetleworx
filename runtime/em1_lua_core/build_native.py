"""Build the embedded Rust library and retain its active unbundled Lua archive."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

cargo, manifest, destination = sys.argv[1:]
process = subprocess.Popen([cargo, 'build', '--locked', '--release',
    '--manifest-path', manifest, '--message-format=json'], stdout=subprocess.PIPE,
    text=True, encoding='utf-8')
libraries = set()
for line in process.stdout:
    message = json.loads(line)
    if message.get('reason') == 'compiler-message':
        print(message['message'].get('rendered', ''), end='')
    if message.get('reason') == 'build-script-executed' and 'static:-bundle=lua5.1' in message.get('linked_libs', []):
        for directory in message.get('linked_paths', []):
            path = Path(directory.removeprefix('native='))/'lua5.1.lib'
            if path.is_file():
                libraries.add(path.resolve())
if process.wait():
    sys.exit(process.returncode)
if len(libraries) != 1:
    raise RuntimeError('Expected exactly one active vendored Lua 5.1 static archive')
source = libraries.pop()
output = Path(destination)
if not output.exists() or output.read_bytes() != source.read_bytes():
    shutil.copyfile(source, output)
print('Embedded native Lua archive:', output)
