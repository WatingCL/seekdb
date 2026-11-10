#!/usr/bin/env python3
"""Build and run the IK differential test using an existing CMake seekdb build.

Reuses production include paths and link libraries, replacing only the
program entry point. Does not rebuild or modify the server binary or build tree.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--english", action="store_true", help="run the English normalizer differential test")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    repo = Path(__file__).resolve().parents[3]
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = next(item for item in commands if "/src/storage/" in item["file"])
    command = entry.get("arguments") or shlex.split(entry["command"])
    includes = ["-I" + str(repo / path) for path in (
        ".", "src", "src/query/api", "src/data_plane/api", "src/oblib", "src/oblib/common")]
    link_dir = build / "src/observer"
    link = shlex.split((link_dir / "CMakeFiles/seekdb.dir/link.txt").read_text())
    link = [item for item in link if not item.startswith("CMakeFiles/ob_main.dir/")]
    with tempfile.TemporaryDirectory(prefix="ik-character-test-") as directory:
        temp = Path(directory)
        objects = []
        sources = (
            repo / "unittest/storage/fts/test_english_normalizer.cpp",
            repo / "src/share/text_analysis/ob_token_stream.cpp",
        ) if args.english else (
            repo / "unittest/storage/fts/test_ik_char_classification.cpp",
            repo / "src/storage/fts/utils/unicode_utils.cpp",
        )
        for index, source in enumerate(sources):
            obj = temp / (str(index) + ".o")
            # This test exercises pure character helpers, with a standard main
            # rather than Observer's allocator/bootstrap entry point.
            production_includes = [item for item in command if item.startswith("-I")]
            compile_command = command[:1] + ["-std=gnu++20", "-O2", "-DNDEBUG",
                "-Wno-format", "-Wno-deprecated-declarations",
                "-Wno-inconsistent-missing-override"] + includes + production_includes
            if args.english:
                compile_command += [item for item in command if item.startswith("-DOB_HAVE_BUNDLED_JEMALLOC")]
            compile_command += ["-c", str(source), "-o", str(obj)]
            subprocess.run(compile_command, cwd=entry["directory"], check=True)
            objects.append(str(obj))
        output_index = link.index("-o")
        binary = temp / "test_ik_char_classification"
        link[output_index + 1] = str(binary)
        link[output_index:output_index] = objects
        subprocess.run(link, cwd=link_dir, check=True)
        subprocess.run([str(binary)] + (["--benchmark"] if args.benchmark and not args.english else []), check=True)


if __name__ == "__main__":
    main()
