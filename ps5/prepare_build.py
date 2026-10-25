"""Stage PSteroids5 against the validated local PS5 platform sources.

Adapted from PSPGameCPP/ps5/prepare_build.py. Uses its own staging tree
(~/work/psteroids5-app) and title identity so BattleStar is never touched.
"""
from pathlib import Path
import json, shutil, subprocess, sys, hashlib

PORT_REVISION = 'm3-https-server-api-2026-10-23'
HEADERS = ['powerups.hpp', 'asteroids_core.hpp', 'json_lite.hpp', 'ca_bundle.hpp', 'net_http.hpp', 'server_api.hpp', 'ship_design.hpp', 'score_store.hpp', 'socketio.hpp', 'mmo_world.hpp', 'coop_lobby.hpp', 'ship_render.hpp', 'sound.hpp', 'ship_editor.hpp']

repo = Path(sys.argv[1]).resolve()
# ps5/sce_sys/param.json is the title's identity (the catalog reads it from the repository).
param = json.loads((repo / 'ps5/sce_sys/param.json').read_text(encoding='utf-8'))
TITLE_ID, CONTENT_ID, VERSION = param['titleId'], param['contentId'], param['contentVersion']
assert param['conceptId'] == TITLE_ID[4:] and CONTENT_ID[7:16] == TITLE_ID and len(CONTENT_ID) == 36
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
# DualSense touchpad for the ship editor (staged copy only; the shared template is untouched).
# The 120-byte pad sample follows the SDK pad-data layout whose anchors platform.c already
# asserts (connected @0x4c, timestamp @0x50): touch count @52, touch[i] @60+8i = {u16 x, u16 y,
# u8 id}. Implausible values read as "no touch", so a wrong layout can only disable the feature.
def patch(path, pairs):
    text = path.read_text().replace('\r\n', '\n')
    for old, new in pairs:
        if text.count(old) != 1:
            raise SystemExit(f'{path.name}: platform layout changed near {old[:40]!r}')
        text = text.replace(old, new)
    path.write_text(text)
patch(app / 'ps5/src/platform.h', [(
    '   float l2, r2;                           /* 0..1 */\n};',
    '   float l2, r2;                           /* 0..1 */\n'
    '   int touch_count;                        /* PSteroid5: fingers on the touchpad, 0..2 */\n'
    '   float touch_x, touch_y;                 /* first finger, raw pad units */\n'
    '   int touch_id;                           /* changes when a new finger lands */\n};')])
patch(app / 'ps5/src/platform.c', [(
    '   pad->r2 = pad_last.r2 / 255.0f;\n}',
    '   pad->r2 = pad_last.r2 / 255.0f;\n'
    '   {\n'
    '      const uint8_t *raw = (const uint8_t *)&pad_last;\n'
    '      uint16_t tx, ty;\n'
    '      memcpy(&tx, raw + 60, 2);\n'
    '      memcpy(&ty, raw + 62, 2);\n'
    '      const int fingers = raw[52];\n'
    '      pad->touch_count = (fingers >= 0 && fingers <= 2 && tx < 4096 && ty < 4096) ? fingers : 0;\n'
    '      pad->touch_x = tx;\n'
    '      pad->touch_y = ty;\n'
    '      pad->touch_id = raw[64];\n'
    '   }\n}')])
# The thruster loop (ps5/sounds/thrust_loop.wav, 48 kHz mono 16-bit) compiled in as PCM.
import wave
with wave.open(str(repo / 'ps5/sounds/thrust_loop.wav')) as w:
    assert (w.getframerate(), w.getnchannels(), w.getsampwidth()) == (48000, 1, 2), 'thrust_loop.wav must be 48 kHz mono 16-bit'
    pcm = w.readframes(w.getnframes())
samples = [int.from_bytes(pcm[i:i + 2], 'little', signed=True) for i in range(0, len(pcm), 2)]
rows = ',\n'.join(','.join(map(str, samples[i:i + 32])) for i in range(0, len(samples), 32))
(app / 'ps5/src/thrust_loop_pcm.hpp').write_text(
    '#pragma once\n// Generated by prepare_build.py from ps5/sounds/thrust_loop.wav (Kenney, CC0).\n#include <cstddef>\n#include <cstdint>\n'
    f'namespace cssteroids::sound {{\ninline constexpr size_t thrust_loop_frames = {len(samples)};\n'
    f'inline const int16_t thrust_loop_pcm[thrust_loop_frames] = {{\n{rows}}};\n}}\n')
prelude = app / 'ps5/src/prelude.h'
code = '\n'.join(line for line in prelude.read_text().splitlines() if '../../examples/rpcs3ps5/' not in line)
prelude.write_text(code + '\n' + ''.join(f'#include "{h}"\n' for h in HEADERS + ['thrust_loop_pcm.hpp']))
samples = app / 'ps5/src/samples.cpp'
samples.write_text(samples.read_text().replace('rpcs3ps5', 'cssteroids')
                   .replace('KB-RPCS3 Alpha 0.1.0', 'Native PS5 Asteroids').replace('KB-RPCS3', 'PSteroid5'))
copies = [('main.cpp', 'ps5/src/main.cpp'), ('cssteroids.cpp', 'examples/cssteroids/cssteroids.cpp'),
          ('link-title.sh', 'ps5/tools/link-title.sh'),
          ('tls_entropy.c', 'ps5/src/tls_entropy.c'), ('tls_config.h', 'ps5/src/tls_config.h')] + [(h, 'ps5/src/' + h) for h in HEADERS]
for src, dst in copies:
    target = app / dst; target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes((repo / 'ps5' / src).read_bytes().replace(b'\r\n', b'\n'))
for p in (app / 'ps5').rglob('*.sh'):
    p.write_bytes(p.read_bytes().replace(b'\r\n', b'\n'))

shutil.copy2(repo / 'ps5/sce_sys/param.json', app / 'ps5/sce_sys/param.json')
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
