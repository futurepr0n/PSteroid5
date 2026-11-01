"""Prepare immutable candidate artifacts and catalog metadata, without publishing."""
from pathlib import Path
import base64
import hashlib
import importlib.util
import json
import subprocess
import zipfile

REPO = Path(__file__).resolve().parents[1]
WORKSPACE = REPO.parent
TITLE = 'PPSA77305'
SOURCE = REPO / 'dist' / TITLE


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def archive(path, files):
    # Never silently replace a candidate whose checksum may already be in use.
    if path.exists():
        raise RuntimeError('Candidate already exists: ' + str(path))
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as output:
        for file, name in sorted(files, key=lambda item: str(item[1])):
            output.write(file, str(name).replace('\\', '/'))


def main():
    spec = importlib.util.spec_from_file_location('validator', WORKSPACE / 'PSPGameCPP/ps5/validate_package.py')
    validator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(validator)
    validator.validate(SOURCE, title_id=TITLE, game_assets=())
    report = json.loads((SOURCE / 'validation.json').read_text())
    for name, sha in report['file_sha256'].items():
        if digest(SOURCE / name) != sha:
            raise RuntimeError('Package changed after validation: ' + name)
    provenance = json.loads((SOURCE / 'build-provenance.json').read_text())
    for name, sha in provenance['port_source_sha256'].items():
        if digest(REPO / name) != sha:
            raise RuntimeError('Source changed after build: ' + name)
    version = json.loads((SOURCE / 'sce_sys/param.json').read_text())['contentVersion']
    target = WORKSPACE / 'catalog-prep/PSteroid5' / version
    target.mkdir(parents=True, exist_ok=True)
    package = target / (TITLE + '.zip')
    archive(package, [(p, Path(TITLE) / p.relative_to(SOURCE)) for p in SOURCE.rglob('*') if p.is_file()])
    # Explicitly selected source trees: no git objects, logs, console reads or saves.
    files = [REPO / name for name in ['README.md', 'Build-PS5.ps1', 'LICENSE', '.gitattributes', '.gitignore']]
    for folder in ['ps5', 'docs', 'tools', 'third_party']:
        files.extend(p for p in (REPO / folder).rglob('*') if p.is_file()
                     and not {'node_modules', '__pycache__'}.intersection(p.parts)
                     and p.suffix not in {'.log', '.pyc'})
    source_zip = target / 'PSteroid5-source.zip'
    archive(source_zip, [(p, Path('PSteroid5') / p.relative_to(REPO)) for p in files])
    # Preserve the authoritative existing catalog fields, then update only this release.
    raw = subprocess.check_output(['gh', 'api', 'repos/darkxex/ps5-homebrew-catalog/contents/apps/PPSA77305.json'])
    current = json.loads(base64.b64decode(json.loads(raw)['content']))
    (target / 'previous-catalog.json').write_text(json.dumps(current, indent=2) + '\n')
    current.update(version=version,
                   artifact_url=f'https://github.com/futurepr0n/PSteroid5/releases/download/{version}/{TITLE}.zip',
                   sha256=digest(package),
                   icon_url=f'https://raw.githubusercontent.com/futurepr0n/PSteroid5/{version}/ps5/sce_sys/icon0.png')
    (target / 'catalog').mkdir(exist_ok=True)
    (target / 'catalog' / (TITLE + '.json')).write_text(json.dumps(current, indent=2) + '\n')
    (target / 'RELEASE-NOTES.md').write_bytes((REPO / f'docs/release-{version}.md').read_bytes())
    (target / 'SHA256SUMS.txt').write_text(''.join(f'{digest(p)}  {p.name}\n' for p in [package, source_zip]))
    (target / 'candidate.json').write_text(json.dumps({
        'title_id': TITLE, 'version': version, 'console_tested': False, 'published': False,
        'eboot_sha256': report['eboot_sha256'], 'package_sha256': digest(package),
        'source_sha256': digest(source_zip), 'source_file_sha256': {
            p.relative_to(REPO).as_posix(): digest(p) for p in files},
        'reference_commit': provenance['reference_project']['commit'],
    }, indent=2) + '\n')
    (target / 'PROMOTION.md').write_text(f'''# Promote the tested candidate

No remote release or catalog change has been made by preparation.
1. Record console acceptance for eboot SHA-256 `{report['eboot_sha256']}`.
2. Review/commit the corresponding source changes in futurepr0n/PSteroid5;
   retain the existing GPL/dependency notices and build provenance. The source
   archive accompanies the prepared native dependency baseline; it is not a
   clean-machine SDK bootstrap.
3. Create a new tag {version} at that reviewed source, and a GitHub release with
   `{TITLE}.zip`, `PSteroid5-source.zip` and `SHA256SUMS.txt`. Keep that asset name.
   Download the published archive and require its SHA-256 to match SHA256SUMS.txt.
4. On a fresh branch from darkxex/ps5-homebrew-catalog main, replace only
   apps/{TITLE}.json with catalog/{TITLE}.json. Run `python -m catalog check` and
   `python -m catalog verify {TITLE}` before submitting the catalog update PR.
5. Record release/PR URLs and the tested hash. If any runtime bytes change,
   rebuild, retest and prepare a new candidate rather than reusing these hashes.
''')
    print('Prepared candidate, not published: ' + str(target))


if __name__ == '__main__':
    main()
