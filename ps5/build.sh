#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export PS5_CLANG=/usr/bin/clang LLVM_CONFIG=/usr/lib/llvm-22/bin/llvm-config
export PS5_VULKAN_DIR=$HOME/work/PS5_Vulkan
python3 "$repo/ps5/prepare_build.py" "$repo"
app=$HOME/work/psteroids5-app
validator=$repo/../PSPGameCPP/ps5/validate_package.py

# Portable tests: simulation, then JSON/HTTP including a live loopback fixture.
g++ -std=c++20 -O2 -Wall -Wextra -pedantic "$repo/ps5/test_core.cpp" -o "$app/test-core"
"$app/test-core"
g++ -std=c++20 -O2 -Wall -Wextra -pedantic -pthread "$repo/ps5/test_net.cpp" -o "$app/test-net"
python3 "$repo/ps5/fixture_server.py" > "$app/fixture.port" & fixture=$!
trap 'kill $fixture 2>/dev/null || true' EXIT
for _ in $(seq 50); do [[ -s $app/fixture.port ]] && break; sleep .1; done
"$app/test-net" "$(cat "$app/fixture.port")"

# Relink and repackage every time: link-title.sh also copies param.json and the icon,
# and ninja would skip it when only metadata changed.
rm -rf "$app/dist/PPSA99305" "$app/build/ps5/eboot.elf"
bash "$app/ps5/tools/build.sh"
python3 "$validator" "$app/dist/PPSA99305" --generic --title-id PPSA99305 --write-report
echo "Built $app/dist/PPSA99305"
