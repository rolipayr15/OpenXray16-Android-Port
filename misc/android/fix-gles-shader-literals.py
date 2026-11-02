#!/usr/bin/env python3
"""Compiler-guided int->float literal fixer for the OpenXRay GL shader tree.

GLSL ES has no implicit int->float conversion, while the shaders (ported from
HLSL) use integer literals in float arithmetic everywhere. This tool expands
each shader exactly like validate-gles-shaders.py, compiles it as GLSL ES 3.20
(every skinning variant of model shaders), and on each line the compiler
reports for an int/float mismatch turns integer literals into float literals.
Literals that must stay integral are left alone: array indices, int/uint/bool
constructors, bit operations, loop headers, preprocessor lines and lines
mentioning sample indices. It repeats until no line changes.

Always re-check desktop GLSL afterwards:
    python misc/android/validate-gles-shaders.py --desktop --preset minimal

Usage:
    python misc/android/fix-gles-shader-literals.py [--preset minimal|mobile]
"""
import argparse
import importlib.util
import os
import re
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
_spec = importlib.util.spec_from_file_location('validator', os.path.join(HERE, 'validate-gles-shaders.py'))
validator = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(validator)

ROOT = os.path.join(REPO, 'res', 'gamedata', 'shaders', 'gl')
CONVERSION = re.compile(
    r"wrong operand types|no matching overloaded function|cannot convert from ' const (?:u?int)'|"
    r"non-convertible constant type")
LITERAL = re.compile(r'(?<![\w.])(\d+)(?![\w.])')
KEEP_LINE = re.compile(
    r'\bfor\s*\(|<<|>>|(?<![&|])[&|](?![&|])|\^|%|\bint\s+\w|\buint\s+\w|\bcase\b|\bswitch\b|'
    r'texelFetch|ISAMPLE|gl_SampleID')
INTEGER_CALL = re.compile(r'\b(int|uint|ivec[234]|uvec[234]|int[234]|uint[234]|bool)\s*\(')


def expand_lines(path, stack):
    """Textual include expansion that remembers (text, file, line) per line."""
    with open(path, encoding='utf-8', errors='replace') as f:
        raw = f.read().split('\n')
    out = []
    for number, line in enumerate(raw, 1):
        match = validator.INCLUDE.search(line)
        if not match:
            out.append((line, path, number))
            continue
        include = os.path.join(ROOT, match.group(1).replace('\\', '/'))
        if include in stack:
            raise RecursionError(include)
        before, after = line[:match.start()], line[match.end():]
        if before.strip():
            out.append((before, path, number))
        out.extend(expand_lines(include, stack + [include]))
        if after.strip():
            out.append((after, path, number))
    return out


def protected_columns(line):
    """Columns inside [...] or integer constructors keep integer literals."""
    keep = [False] * len(line)
    opened = []
    for column, character in enumerate(line):
        if character == '[':
            opened.append(column)
        elif character == ']' and opened:
            for index in range(opened.pop(), column + 1):
                keep[index] = True
    for match in INTEGER_CALL.finditer(line):
        depth = 0
        for column in range(match.end() - 1, len(line)):
            depth += line[column] == '('
            depth -= line[column] == ')'
            keep[column] = True
            if depth == 0:
                break
    return keep


def fix_line(line):
    stripped = line.lstrip()
    if stripped.startswith(('#', '//')) or KEEP_LINE.search(line):
        return line
    keep = protected_columns(line)
    comment = line.find('//')
    pieces = []
    position = 0
    for match in LITERAL.finditer(line):
        start, end = match.span(1)
        if comment != -1 and start > comment:
            break
        if keep[start]:
            continue
        pieces.append(line[position:start])
        pieces.append(match.group(1) + '.0')
        position = end
    pieces.append(line[position:])
    return ''.join(pieces)


def compile_shader(path, preamble, glslc, scratch):
    """Returns (passes, [(file, line, message)]) over every variant."""
    stage = validator.STAGES[os.path.splitext(path)[1]]
    lines = []
    for text, file, number in expand_lines(path, [path]):
        # Rewrite line by line so every output line keeps its origin.
        for part in validator.rewrite(text, stage).split('\n'):
            lines.append((part, file, number))
    body = '\n'.join(text for text, _, _ in lines)
    passes = True
    origins = []
    for variant in validator.variants_for(body):
        extra = f'#define {variant}\t1\n' if variant else ''
        offset = preamble.count('\n') + extra.count('\n')
        temp = os.path.join(scratch, 'shader.' + stage)
        with open(temp, 'w', encoding='utf-8') as f:
            f.write(preamble + extra + body)
        result = subprocess.run(
            [glslc, '-std=320es', '--target-env=opengl', '-fauto-map-locations', '-fauto-bind-uniforms',
             '-fshader-stage=' + stage, '-c', temp, '-o', os.devnull], capture_output=True, text=True)
        if result.returncode == 0:
            continue
        passes = False
        for message in result.stderr.splitlines():
            match = re.search(r':(\d+): error: (.*)', message)
            if match:
                index = int(match.group(1)) - 1 - offset
                if 0 <= index < len(lines):
                    origins.append((lines[index][1], lines[index][2], match.group(2)))
    return passes, origins


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--preset', choices=sorted(validator.PRESETS), default='mobile')
    parser.add_argument('--glslc', default=validator.find_glslc())
    parser.add_argument('--iterations', type=int, default=12)
    args = parser.parse_args()

    preamble = validator.load_preamble(REPO) + ''.join(
        f'#define {name}\t{value}\n' for name, value in validator.PRESETS[args.preset].items())
    shaders = sorted(os.path.join(ROOT, name) for name in os.listdir(ROOT)
                     if os.path.splitext(name)[1] in validator.STAGES)
    with tempfile.TemporaryDirectory() as scratch:
        for iteration in range(args.iterations):
            edits = {}
            passing = 0
            for shader in shaders:
                passes, origins = compile_shader(shader, preamble, args.glslc, scratch)
                passing += passes
                for file, line, message in origins:
                    if CONVERSION.search(message):
                        edits.setdefault(file, set()).add(line)
            changed = 0
            for file, numbers in edits.items():
                with open(file, encoding='utf-8', errors='replace', newline='') as f:
                    content = f.read()
                crlf = '\r\n' in content
                rows = content.replace('\r\n', '\n').split('\n')
                for number in numbers:
                    fixed = fix_line(rows[number - 1])
                    if fixed != rows[number - 1]:
                        rows[number - 1] = fixed
                        changed += 1
                text = '\n'.join(rows)
                with open(file, 'w', encoding='utf-8', newline='') as f:
                    f.write(text.replace('\n', '\r\n') if crlf else text)
            print(f'iteration {iteration}: {passing}/{len(shaders)} pass, {changed} lines changed', flush=True)
            if not changed:
                break


if __name__ == '__main__':
    main()
