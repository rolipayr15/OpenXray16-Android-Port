#!/usr/bin/env python3
"""Compile every OpenXRay GL shader as GLSL ES 3.20 with the NDK's glslc.

The Android OpenGL ES renderer feeds the desktop GL shaders through a GLES
preamble and a small source rewrite (see glShaderSourceES.cpp). This script
reproduces that pipeline offline: it expands #include directives the way the
engine does (plain textual inclusion relative to the shader root), applies the
same rewrite, and validates the result under the strict ES grammar, which
rejects things desktop drivers accept (implicit int->float conversion, missing
precision, unlocated MRT outputs, ...). No device is needed.

Usage:
    python misc/android/validate-gles-shaders.py [--glslc PATH] [--shaders DIR]
                                                 [--define NAME=VALUE ...] [--only PATTERN]
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter

STAGES = {'.vs': 'vert', '.ps': 'frag', '.gs': 'geom', '.cs': 'comp', '.hs': 'tesc', '.ds': 'tese'}

# Options the engine typically sets for an R3-class mobile configuration.
DEFAULT_DEFINES = {
    'SMAP_size': '2048',
    'FP16_FILTER': '1',
    'FP16_BLEND': '1',
    'USE_HWSMAP': '1',
    'USE_HWSMAP_PCF': '1',
    'USE_BRANCHING': '1',
    'USE_VTF': '1',
    'SM_4_1': '1',
}

INCLUDE = re.compile(r'#include\s*"([^"]+)"')


def find_glslc():
    roots = [os.environ.get('ANDROID_NDK_ROOT', ''), os.environ.get('ANDROID_NDK_HOME', '')]
    sdk = os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    if sdk:
        roots += sorted(glob.glob(os.path.join(sdk, 'ndk', '*')), reverse=True)
    for root in filter(None, roots):
        for candidate in glob.glob(os.path.join(root, 'shader-tools', '*', 'glslc*')):
            return candidate
    return 'glslc'


def expand(path, root, seen_stack):
    """Textual include expansion matching shader_sources_manager::load_includes."""
    with open(path, encoding='utf-8', errors='replace') as source:
        text = source.read()
    out = []
    position = 0
    for match in INCLUDE.finditer(text):
        out.append(text[position:match.start()])
        name = match.group(1).replace('\\', '/')
        include_path = os.path.join(root, name)
        if not os.path.isfile(include_path):
            raise FileNotFoundError(f'{path}: missing include {name}')
        if include_path in seen_stack:
            raise RecursionError(f'{path}: recursive include {name}')
        out.append(expand(include_path, root, seen_stack + [include_path]))
        position = match.end()
    out.append(text[position:])
    return ''.join(out)


def load_preamble(repo_root):
    """Reads the ES preamble and rewrite rules from the engine source so the
    validator can never drift from what the renderer actually compiles."""
    source_path = os.path.join(repo_root, 'src', 'Layers', 'xrRenderGL', 'glShaderSourceES.cpp')
    with open(source_path, encoding='utf-8') as f:
        text = f.read()
    block = re.search(r'// BEGIN ES PREAMBLE\n(.*?)// END ES PREAMBLE', text, re.S)
    if not block:
        raise SystemExit(f'{source_path}: preamble markers not found')
    lines = re.findall(r'^\s*"(.*)\\n"', block.group(1), re.M)
    return '\n'.join(line.replace('\\"', '"') for line in lines) + '\n'


MRT_OUTPUT = re.compile(r'^(\s*)out(\s+\w+\s+SV_Target)(\d)\s*;', re.M)


def rewrite(source):
    # Must match glShaderSourceES.cpp: GLSL ES needs explicit locations for
    # multiple fragment outputs (desktop used glBindFragDataLocation).
    return MRT_OUTPUT.sub(lambda m: f'{m.group(1)}layout(location = {m.group(3)}) out{m.group(2)}{m.group(3)};', source)


def main():
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--glslc', default=find_glslc())
    parser.add_argument('--shaders', default=os.path.join(repo_root, 'res', 'gamedata', 'shaders', 'gl'))
    parser.add_argument('--define', action='append', default=[])
    parser.add_argument('--only', default='')
    parser.add_argument('--verbose', action='store_true')
    parser.add_argument('--desktop', action='store_true',
                        help='compile as desktop GLSL 4.10 instead (regression check for the PC renderer)')
    args = parser.parse_args()

    defines = dict(DEFAULT_DEFINES)
    for item in args.define:
        name, _, value = item.partition('=')
        defines[name] = value or '1'

    if args.desktop:
        # Matches CRender::shader_compile on desktop.
        preamble = '#version 410\n#extension GL_ARB_separate_shader_objects : enable\n'
        profile = '-std=410core'
    else:
        preamble = load_preamble(repo_root)
        profile = '-std=320es'
    options = ''.join(f'#define {name}\t{value}\n' for name, value in defines.items())

    shaders = sorted(p for p in glob.glob(os.path.join(args.shaders, '*'))
                     if os.path.splitext(p)[1] in STAGES and args.only in os.path.basename(p))
    failures = 0
    categories = Counter()
    with tempfile.TemporaryDirectory() as scratch:
        for shader in shaders:
            stage = STAGES[os.path.splitext(shader)[1]]
            try:
                body = expand(shader, args.shaders, [shader])
            except (FileNotFoundError, RecursionError) as error:
                print(f'FAIL {os.path.basename(shader)}: {error}')
                failures += 1
                continue
            source = preamble + options + (body if args.desktop else rewrite(body))
            temp = os.path.join(scratch, os.path.basename(shader) + '.' + stage)
            with open(temp, 'w', encoding='utf-8') as f:
                f.write(source)
            result = subprocess.run(
                [args.glslc, profile, '--target-env=opengl', '-fauto-map-locations',
                 '-fauto-bind-uniforms', '-fshader-stage=' + stage, '-c', temp, '-o', os.devnull],
                capture_output=True, text=True)
            if result.returncode != 0:
                failures += 1
                errors = [line for line in result.stderr.splitlines() if 'error:' in line]
                for line in errors:
                    categories[re.sub(r".*error: '?[^']*'? ?:?", '', line).strip()[:70]] += 1
                print(f'FAIL {os.path.basename(shader)}: {errors[0] if errors else result.stderr.strip()[:200]}')
                if args.verbose:
                    print(result.stderr)
            elif args.verbose:
                print(f'ok   {os.path.basename(shader)}')

    language = 'desktop GLSL 4.10' if args.desktop else 'GLSL ES 3.20'
    print(f'\n{len(shaders) - failures}/{len(shaders)} shaders compile as {language}')
    if categories:
        print('\nMost common errors:')
        for message, count in categories.most_common(15):
            print(f'{count:5}  {message}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
