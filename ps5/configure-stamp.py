#!/usr/bin/env python3
"""Reuse Wine configure output, cleaning objects when compiler flags change."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


ENVIRONMENT = frozenset((
    'PATH', 'CC', 'CXX', 'CPP', 'AR', 'AS', 'LD', 'NM', 'RANLIB', 'STRIP',
    'CFLAGS', 'CXXFLAGS', 'CROSSCFLAGS', 'CROSSLDFLAGS', 'CPPFLAGS', 'LDFLAGS',
    'LIBS', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
    'PKG_CONFIG', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR', 'PKG_CONFIG_SYSROOT_DIR',
    'SOURCE_DATE_EPOCH', 'LANG', 'LC_ALL', 'CONFIG_SITE',
))


def compiler_flags(command, environment):
    values = {key: environment.get(key, '-g -O2') for key in ('CFLAGS', 'CXXFLAGS')}
    assignments = dict(item.split('=', 1) for item in command[1:]
                       if '=' in item and not item.startswith('-'))
    values.update({key: assignments[key] for key in values if key in assignments})
    cross = assignments.get('CROSSCFLAGS', environment.get('CROSSCFLAGS'))
    if cross is not None:
        values.update(i386_CFLAGS=cross, x86_64_CFLAGS=cross)
    return {key: ' '.join(value.split()) for key, value in values.items()}


def existing_flags(makefile, names):
    values = {}
    if makefile.is_file():
        with makefile.open() as source:
            for line in source:
                key, separator, value = line.partition(' = ')
                if separator and key in names:
                    values[key] = ' '.join(value.split())
                    if values.keys() == names:
                        break
    return values


def generated_identity(build):
    result = {}
    for name in ('Makefile', 'config.status', 'include/config.h'):
        path = build / name
        if not path.is_file():
            return None
        with path.open('rb') as source:
            result[name] = hashlib.file_digest(source, 'sha256').hexdigest()
    return result


def configure(build, command, environment=None):
    environment = dict(os.environ if environment is None else environment)
    script = Path(command[0]).resolve()
    inputs = {
        'format': 1,
        'command': command,
        'environment': {key: value for key, value in environment.items()
                        if key in ENVIRONMENT or key.startswith('ac_cv_') or
                        key.endswith(('_CFLAGS', '_LIBS'))},
        'scripts': {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in (script, script.with_name('configure.ac')) if path.is_file()},
    }
    key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
    flags = compiler_flags(command, environment)
    stamp = build / '.configure-stamp.json'
    try:
        previous = json.loads(stamp.read_text())
    except (OSError, ValueError):
        previous = {}
    generated = generated_identity(build)
    if previous.get('key') == key and generated is not None and previous.get('generated') == generated:
        print(f'CONFIGURE_HIT {build}', flush=True)
        return
    # Trust recorded input flags only while the generated files still match.
    # A legacy/private writer may have reconfigured this shared tree; then
    # compare its actual Makefile instead (configure can append its own flags).
    old_flags = (previous.get('compiler_flags')
                 if generated is not None and previous.get('generated') == generated
                 else existing_flags(build / 'Makefile', flags.keys()))
    stamp.unlink(missing_ok=True)
    build.mkdir(parents=True, exist_ok=True)
    if (build / 'Makefile').is_file() and old_flags != flags:
        print(f'CONFIGURE_CLEAN {build}: compiler flags changed', flush=True)
        with (build / 'configure-clean.log').open('w') as log:
            subprocess.run(['make', 'clean'], cwd=build, env=environment,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    with (build / 'configure.log').open('w') as log:
        subprocess.run(command, cwd=build, env=environment,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    temporary = stamp.with_suffix('.tmp')
    temporary.write_text(json.dumps({'key': key, 'compiler_flags': flags, 'inputs': inputs,
                                    'generated': generated_identity(build)},
                                    indent=2, sort_keys=True) + '\n')
    temporary.replace(stamp)
    print(f'CONFIGURE_MISS {build}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a configure command is required')
    configure(args.build.resolve(), command)


if __name__ == '__main__':
    main()
