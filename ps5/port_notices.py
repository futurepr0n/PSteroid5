from pathlib import Path
import shutil, subprocess, sys
app=Path(sys.argv[1]); root=Path(__file__).resolve().parents[2]; work=Path.home()/'work'
out=app/'licenses'; out.mkdir(exist_ok=True)
for source,name in [(root/'LICENSE.md','platform-template-MIT.txt'),(root/'ps5/licenses/GPL-3.0.txt','GPL-3.0.txt'),
                    (work/'PS5_PayloadSDK/LICENSE','payload-sdk-LICENSE.txt'),
                    (work/'PS5_Vulkan/LICENSE','PS5-Vulkan-LICENSE.txt'),
                    (work/'PS5_Vulkan/.deps/work/radv-src/docs/license.rst','Mesa-license.rst')]:
    shutil.copy2(source,out/name)
kit_license=subprocess.check_output(['git','-C',str(work/'PS5_VKHomebrewUI'),'show','4451c261ac8631c698291ee765a37456f25bc22c:LICENSE'])
(out/'PS5-VKHomebrewUI-LICENSE.txt').write_bytes(kit_license)
for name in ['build-provenance.json','GAME_README.md','server.txt.example']:
    shutil.copy2(root/name,app/name)
(app/'THIRD_PARTY_NOTICES.txt').write_text('CSSteroids native PS5 edition\n\nGame design and rules: futurepr0n/CSSteroids-2 (browser game, public/js). All graphics are vector shapes drawn at run time; no original image or audio assets are shipped.\nNew native simulation, networking and rendering: PSteroids5 project (separate from CSSteroids-2, which is used read-only as the reference).\nPS5 platform: KB-RPCS3 app, derived from Mihawk PS5 Vulkan Template and Sascha Willems Vulkan examples.\nUI/Vulkan renderer: BlackBearReloaded and Mihawk PS5_VKHomebrewUI (GPL-3.0-or-later).\nGraphics driver: Mesa/RADV PS5 port, Mihawk and Mesa contributors.\nToolchain and platform library: ps5-payload-dev and Mihawk PS5_PayloadSDK.\nThe original notices in the source files continue to apply. See licenses/ and assets/NOTICES.txt.\n')
