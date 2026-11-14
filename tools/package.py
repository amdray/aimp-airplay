"""Create the same complete Windows package locally and in GitHub Actions."""
import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
PLUGINS = ('aimp_airplay', 'dsp_airplay')


def package(build, runtime, runtime_licenses, objdump, output):
    version = re.search(r'project\(aimp_airplay VERSION ([0-9.]+)', (ROOT / 'CMakeLists.txt').read_text()).group(1)
    files = {}
    for folder in PLUGINS:
        library = build / (folder + '.dll')
        if not library.is_file():
            raise FileNotFoundError(library)
        files[folder + '/x64/' + library.name] = library
    for name in ('README.md', 'THIRD-PARTY.md'):
        files[name] = ROOT / name
    for directory in ('docs/images', 'licenses'):
        for file in sorted((ROOT / directory).rglob('*')):
            if file.is_file():
                files[file.relative_to(ROOT).as_posix()] = file

    imports, included = {}, set()

    def add_dependencies(library, destination):
        key = destination, library.name.lower()
        if key in included:
            return
        included.add(key)
        if library not in imports:
            result = subprocess.run([str(objdump), '-p', str(library)], check=True, capture_output=True, text=True)
            imports[library] = re.findall(r'DLL Name:\s*(\S+)', result.stdout)
        for name in imports[library]:
            dependency = runtime / name
            if dependency.is_file():
                files[destination + '/' + name] = dependency
                add_dependencies(dependency, destination)
            elif name.lower().startswith(('api-ms-', 'ext-ms-')) or (Path(os.environ['SystemRoot']) / 'System32' / name).is_file():
                continue
            else:
                raise FileNotFoundError(f'{library.name} requires {name}, missing from {runtime}')

    for folder in PLUGINS:
        add_dependencies(build / (folder + '.dll'), folder + '/x64')
    for name in ('libcrypto-3-x64.dll', 'libssl-3-x64.dll'):
        library = runtime / name
        if not library.is_file():
            raise FileNotFoundError(library)
        files['aimp_airplay/x64/' + name] = library
        add_dependencies(library, 'aimp_airplay/x64')
    if runtime_licenses:
        for directory in ('openssl', 'gcc-libs', 'winpthreads', 'libwinpthread', 'mingw-w64-libraries'):
            target = runtime_licenses / directory
            for file in sorted(target.rglob('*')):
                if file.is_file():
                    files['licenses/runtime/' + file.relative_to(runtime_licenses).as_posix()] = file

    output.mkdir(parents=True, exist_ok=True)
    filename = output / f'aimp_airplay-{version}-x64.zip'
    notices = files['THIRD-PARTY.md'].read_text(encoding='utf-8')
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        source_url = f"{os.environ['GITHUB_SERVER_URL']}/{os.environ['GITHUB_REPOSITORY']}/tree/{os.environ['GITHUB_SHA']}"
        notices += f'\n[Source and build instructions for this package]({source_url}).\n'

    def write_file(archive, name, file):
        if file == files['THIRD-PARTY.md']:
            archive.writestr(name, notices)
        else:
            archive.write(file, name)
    addons = {}
    for folder in PLUGINS:
        addon_filename = output / f'{folder}-{version}-x64.aimppack'
        addon_files = {name: file for name, file in files.items() if name.startswith(folder + '/')}
        for name, file in files.items():
            if name == 'THIRD-PARTY.md' or name.startswith('licenses/'):
                addon_files[folder + '/' + name] = file
        addon = io.BytesIO()
        with zipfile.ZipFile(addon, 'w', zipfile.ZIP_DEFLATED) as archive:
            directories = { '/'.join(name.split('/')[:depth]) + '/'
                            for name in addon_files for depth in range(1, len(name.split('/'))) }
            for directory in sorted(directories):
                archive.writestr(directory, b'')
            for name, file in sorted(addon_files.items()):
                write_file(archive, name, file)
        addon_bytes = addon.getvalue()
        addon_filename.write_bytes(addon_bytes)
        addon_digest = hashlib.sha256(addon_bytes).hexdigest()
        addon_filename.with_suffix('.aimppack.sha256').write_text(f'{addon_digest}  {addon_filename.name}\n', encoding='ascii')
        addons[addon_filename.name] = addon_bytes
    # Assemble before opening the output, so missing dependencies leave no partial zip.
    with zipfile.ZipFile(filename, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name, file in sorted(files.items()):
            if name.split('/')[0] not in PLUGINS:
                write_file(archive, name, file)
        for name, content in addons.items():
            archive.writestr(name, content)
    digest = hashlib.sha256(filename.read_bytes()).hexdigest()
    filename.with_suffix(filename.suffix + '.sha256').write_text(f'{digest}  {filename.name}\n', encoding='ascii')
    print(filename)
    for name in addons:
        print(output / name)
    return filename


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--runtime-dir', type=Path, required=True)
    parser.add_argument('--runtime-licenses', type=Path)
    parser.add_argument('--objdump', type=Path, default=Path('objdump'))
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'dist')
    args = parser.parse_args()
    package(args.build_dir.resolve(), args.runtime_dir.resolve(), args.runtime_licenses, args.objdump, args.output_dir.resolve())
