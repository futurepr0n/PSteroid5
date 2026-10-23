"""Stage CSSteroids against the validated local PS5 platform sources.

Adapted from PSPGameCPP/ps5/prepare_build.py. Uses its own staging tree
(~/work/psteroids5-app) and title identity so BattleStar is never touched.
"""
from pathlib import Path
import json, shutil, subprocess, sys, hashlib

TITLE_ID = 'PPSA99305'
CONTENT_ID = 'UP9000-PPSA99305_00-CSSTEROIDSPS5000'
VERSION = '01.000.000'
PORT_REVISION = 'm3-https-server-api-2026-10-23'
HEADERS = ['asteroids_core.hpp', 'json_lite.hpp', 'ca_bundle.hpp', 'net_http.hpp', 'server_api.hpp', 'ship_design.hpp', 'score_store.hpp']

repo = Path(sys.argv[1]).resolve()
work = Path.home() / 'work'
template = work / 'ps5-rpcs3-app'
app = work / 'psteroids5-app'
if not (template / '.deps/native/ps5-payload-sdk/bin/prospero-lld').exists():
    raise SystemExit('The prepared KB-RPCS3 PS5 toolchain is required.')
app.mkdir(exist_ok=True)
for name in ['base', 'external', 'ps5', 'shaders']:
    shutil.copytree(template / name, app / name, dirs_exist_ok=True, ignore=shutil.ignore_patterns('icon0.png'))
for name in ['LICENSE.md', 'LICENSING.md']:
    shutil.copy2(template / name, app / name)
deps = app / '.deps'
if not deps.exists(): deps.symlink_to(template / '.deps', target_is_directory=True)

cmake = app / 'ps5/CMakeLists.txt'
code = cmake.read_text()
for source in ['probe_rpcs3.cpp', 'ps5_dl.c', 'ps5_fastclock.c', 'ps5_tuning.c', 'import_check.c']:
    code = code.replace('\tsrc/' + source + '\n', '')
# mbedTLS (vendored, unmodified) as its own object library, linked into the title.
code = code.replace('# Link, sign and package', '''# PSteroids5: mbedTLS for the HTTPS client (third_party/mbedtls, Apache-2.0)
file(GLOB mbedtls_sources ${ROOT}/mbedtls/library/*.c)
add_library(mbedtls_ps5 OBJECT ${mbedtls_sources} src/tls_entropy.c)
target_include_directories(mbedtls_ps5 PRIVATE ${ROOT}/mbedtls/include ${ROOT}/mbedtls/library src)
target_compile_definitions(mbedtls_ps5 PRIVATE MBEDTLS_USER_CONFIG_FILE="tls_config.h")
target_compile_options(mbedtls_ps5 PRIVATE -O2 -w -ffunction-sections -fdata-sections -fno-omit-frame-pointer)
set_target_properties(mbedtls_ps5 PROPERTIES C_STANDARD 11)
target_include_directories(samples PRIVATE ${ROOT}/mbedtls/include)
target_compile_definitions(samples PRIVATE MBEDTLS_USER_CONFIG_FILE="tls_config.h")

# Link, sign and package''', 1)
objects = '"$<TARGET_OBJECTS:samples>"'
assert code.count(objects) == 2, 'link command layout changed'
code = code.replace(objects, objects + ' "$<TARGET_OBJECTS:mbedtls_ps5>"')
code = code.replace('DEPENDS samples ', 'DEPENDS samples mbedtls_ps5 ', 1)
cmake.write_text(code)
shutil.copytree(repo / 'third_party/mbedtls', app / 'mbedtls', dirs_exist_ok=True)
prelude = app / 'ps5/src/prelude.h'
code = '\n'.join(line for line in prelude.read_text().splitlines() if '../../examples/rpcs3ps5/' not in line)
prelude.write_text(code + '\n' + ''.join(f'#include "{h}"\n' for h in HEADERS))
samples = app / 'ps5/src/samples.cpp'
samples.write_text(samples.read_text().replace('rpcs3ps5', 'cssteroids')
                   .replace('KB-RPCS3 Alpha 0.1.0', 'Native PS5 Asteroids').replace('KB-RPCS3', 'CSSteroids'))
copies = [('main.cpp', 'ps5/src/main.cpp'), ('cssteroids.cpp', 'examples/cssteroids/cssteroids.cpp'),
          ('link-title.sh', 'ps5/tools/link-title.sh'),
          ('tls_entropy.c', 'ps5/src/tls_entropy.c'), ('tls_config.h', 'ps5/src/tls_config.h')] + [(h, 'ps5/src/' + h) for h in HEADERS]
for src, dst in copies:
    target = app / dst; target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes((repo / 'ps5' / src).read_bytes().replace(b'\r\n', b'\n'))
for p in (app / 'ps5').rglob('*.sh'):
    p.write_bytes(p.read_bytes().replace(b'\r\n', b'\n'))

param_path = app / 'ps5/sce_sys/param.json'
param = json.loads(param_path.read_text())
param.update(titleId=TITLE_ID, conceptId=TITLE_ID[4:], contentId=CONTENT_ID, contentVersion=VERSION)
param['localizedParameters']['en-US']['titleName'] = 'CSSteroids'
param_path.write_text(json.dumps(param, indent=2) + '\n')
icon = repo / 'ps5/sce_sys/icon0.png'
if not icon.is_file():
    raise SystemExit('Missing launcher icon. Run ps5/Build-Icon.ps1 first.')
shutil.copy2(icon, app / 'ps5/sce_sys/icon0.png')
manifest_path = app / 'ps5/assets.json'
manifest = json.loads(manifest_path.read_text())
manifest['assets'] = [a for a in manifest['assets'] if '/audio/' not in a['path']]
manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
shutil.copy2(repo / 'ps5/port_notices.py', app / 'ps5/tools/port-notices.py')
for source, target in [('README.md', 'GAME_README.md'), ('server.txt.example', 'server.txt.example')]:
    shutil.copy2(repo / 'ps5' / source, app / target)

def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args]).decode().strip()
reference = repo.parent / 'CSSTeroids-2'
has_head = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--verify', '-q', 'HEAD'], capture_output=True).returncode == 0
sources = sorted(p for p in (repo / 'ps5').rglob('*') if p.is_file() and '__pycache__' not in p.parts
                 and p.name != 'validation.json')
provenance = {
    'reference_project': {'path': 'CSSTeroids-2 (read-only reference)', 'url': 'https://github.com/futurepr0n/CSSteroids-2',
                          'commit': git(reference, 'rev-parse', 'HEAD')} if (reference / '.git').exists() else None,
    'port_commit': git(repo, 'rev-parse', '--verify', '-q', 'HEAD') if has_head else None,
    'port_dirty_paths': git(repo, 'status', '--porcelain').splitlines(),
    'port_revision': PORT_REVISION,
    'port_source_sha256': {p.relative_to(repo).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in sources + [repo / 'Build-PS5.ps1']},
    'platform_source': git(template, 'rev-parse', 'HEAD'),
    'vulkan_source': git(work / 'PS5_Vulkan', 'rev-parse', 'HEAD'),
    'title_id': TITLE_ID, 'content_version': VERSION,
    'logical_board': [1920, 1080], 'render_output': [3840, 2160],
}
(app / 'build-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
print(app)
