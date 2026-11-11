"""Keep SQL assertions intact across mysqltest client command dialects."""
from pathlib import Path
import re


def normalize_regex_flags(text):
    """Both clients replace globally; MariaDB rejects an explicit /g modifier.

    Only recognized modifiers after complete --replace_regex expressions are
    normalized. SQL, patterns, replacements and malformed directives are kept.
    """
    output = []
    for line in text.splitlines(keepends=True):
        match = re.match(r'^(\s*--replace_regex\s+)(.*?)(\r?\n)?$', line)
        if not match:
            output.append(line)
            continue
        expr = match[2]
        result, pos, valid = [], 0, True
        while pos < len(expr):
            if expr[pos].isspace():
                result.append(expr[pos]); pos += 1
                continue
            if expr[pos] != '/':
                valid = False
                break
            start = pos
            pos += 1
            for _ in range(2):
                while pos < len(expr):
                    if expr[pos] == '\\' and pos + 1 < len(expr):
                        pos += 2
                    elif expr[pos] == '/':
                        pos += 1
                        break
                    else:
                        pos += 1
                else:
                    valid = False
                    break
            if not valid:
                break
            result.append(expr[start:pos])
            start = pos
            while pos < len(expr) and not expr[pos].isspace():
                pos += 1
            flags = expr[start:pos]
            if len(set(flags)) != len(flags) or any(flag not in 'ig' for flag in flags):
                valid = False
                break
            result.append(flags.replace('g', ''))
        output.append(match[1] + ''.join(result) + (match[3] or '') if valid else line)
    return ''.join(output)


def portable_test_file(source, directory):
    source = Path(source)
    original = source.read_text()
    normalized = normalize_regex_flags(original)
    if normalized == original:
        return source
    target = Path(directory) / (source.stem + '.portable.test')
    target.write_text(normalized)
    return target
