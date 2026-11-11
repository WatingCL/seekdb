#!/usr/bin/env python3
"""Run FTS regression checks with an existing CMake unity build's runtime.

Optional --baseline-ref compares arbitration with a Git revision without changing
sources, the server executable, or build artifacts. --benchmark times 600000
synthetic 64-chain documents; this is not the competition scoring workload.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--baseline-ref')
    parser.add_argument('--benchmark', action='store_true')
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    build = args.build_dir.resolve()
    commands = json.loads((build / 'compile_commands.json').read_text())
    entries = []
    for stem in ('fts', 'ik'):
        entries.append(next(item for item in commands
                            if f'unity_ob_storage_ob_storage_{stem}_0_' in item['file']))
    with tempfile.TemporaryDirectory(prefix='fts-regression-') as directory:
        temp = Path(directory)
        overlay = temp / 'overlay'
        replacements = {}
        if args.baseline_ref:
            for name in ('ik/ob_ik_arbitrator.cpp', 'ik/ob_ik_arbitrator.h',
                         'utils/unicode_utils.cpp', 'utils/unicode_utils.h'):
                relative = 'src/storage/fts/' + name
                target = overlay / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(subprocess.check_output(
                    ['git', 'show', args.baseline_ref + ':' + relative], cwd=repo))
                replacements[str(repo / relative)] = str(target)
        sources = [repo / 'unittest/storage/fts/test_ik_chain_lookup.cpp']
        for index, entry in enumerate(entries):
            source = temp / f'unity_{index}.cpp'
            content = Path(entry['file']).read_text()
            for original, replacement in replacements.items():
                content = content.replace(original, replacement)
            source.write_text(content)
            sources.append(source)
        objects = []
        for index, source in enumerate(sources):
            entry = entries[max(0, index - 1)]
            command = entry.get('arguments') or shlex.split(entry['command'])
            obj = temp / f'{index}.o'
            command[command.index('-o') + 1] = str(obj)
            command[command.index('-c') + 1] = str(source)
            command[1:1] = ['-I' + str(overlay), '-I' + str(overlay / 'src')]
            subprocess.run(command, cwd=entry['directory'], check=True)
            objects.append(str(obj))
        link_dir = build / 'src/observer'
        link = shlex.split((link_dir / 'CMakeFiles/seekdb.dir/link.txt').read_text())
        link = [item for item in link if not item.startswith('CMakeFiles/ob_main.dir/')]
        binary = temp / 'test_ik_chain_lookup'
        link[link.index('-o') + 1] = str(binary)
        link[1:1] = objects
        subprocess.run(link, cwd=link_dir, check=True)
        subprocess.run([str(binary)] + (['--benchmark'] if args.benchmark else []), check=True)


if __name__ == '__main__':
    main()
