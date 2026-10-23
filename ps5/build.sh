#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export PS5_CLANG=/usr/bin/clang LLVM_CONFIG=/usr/lib/llvm-22/bin/llvm-config
export PS5_VULKAN_DIR=$HOME/work/PS5_Vulkan
python3 "$repo/ps5/prepare_build.py" "$repo"
app=$HOME/work/psteroids5-app
validator=$repo/../PSPGameCPP/ps5/validate_package.py
tls=(-DMBEDTLS_USER_CONFIG_FILE='"tls_config.h"' -I"$repo/ps5" -I"$repo/third_party/mbedtls/include")

# Host mbedTLS for the network tests; rebuilt only when its inputs change.
host=$app/host-mbedtls
stamp=$(cat "$repo/ps5/tls_config.h" "$repo/ps5/tls_entropy.c" "$repo/third_party/mbedtls/VENDORED.txt" | sha256sum | cut -c1-16)
if [[ ! -f $host/$stamp ]]; then
    rm -rf "$host"; mkdir -p "$host/obj"
    for f in "$repo"/third_party/mbedtls/library/*.c "$repo/ps5/tls_entropy.c"; do
        gcc -O2 -w "${tls[@]}" -I"$repo/third_party/mbedtls/library" -c "$f" -o "$host/obj/$(basename "$f" .c).o" &
        while (( $(jobs -r | wc -l) >= 8 )); do wait -n; done
    done
    wait
    ar rcs "$host/libmbedtls.a" "$host"/obj/*.o
    touch "$host/$stamp"
fi

# Portable tests: simulation, then JSON/HTTP/TLS against loopback fixtures.
g++ -std=c++20 -O2 -Wall -Wextra -pedantic "$repo/ps5/test_core.cpp" -o "$app/test-core"
"$app/test-core"
g++ -std=c++20 -O2 -Wall -Wextra -pedantic -pthread "${tls[@]}" "$repo/ps5/test_net.cpp" "$host/libmbedtls.a" -o "$app/test-net"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost -keyout "$app/fixture.key" -out "$app/fixture.crt" 2>/dev/null
rm -f "$app/fixture.port" "$app/fixture-tls.port"
python3 "$repo/ps5/fixture_server.py" > "$app/fixture.port" & fixture=$!
python3 "$repo/ps5/fixture_server.py" "$app/fixture.crt" "$app/fixture.key" > "$app/fixture-tls.port" & fixture_tls=$!
trap 'kill $fixture $fixture_tls 2>/dev/null || true' EXIT
for _ in $(seq 50); do [[ -s $app/fixture.port && -s $app/fixture-tls.port ]] && break; sleep .1; done
# PSTEROIDS_LIVE (e.g. https://asteroids.futurepr0n.com) adds read-only GETs to a real server.
PSTEROIDS_LIVE=${PSTEROIDS_LIVE:-} "$app/test-net" "$(cat "$app/fixture.port")" "$(cat "$app/fixture-tls.port")"

# Relink and repackage every time: link-title.sh also copies param.json and the icon,
# and ninja would skip it when only metadata changed.
rm -rf "$app/dist/PPSA99305" "$app/build/ps5/eboot.elf"
bash "$app/ps5/tools/build.sh"
# Imports from a module the loader does not map stay at address 0 (-z
# nodynamic-undefined-weak) and crash when called: libSceRandom did exactly that.
# Allow only modules the console was seen loading for this title (klog, 2026-10-23).
loaded='libkernel libSceLibcInternal libSceNet libSceVideoOut libSceAgc libSceAgcDriver libSceAudioOut libScePad libSceSystemService libSceUserService'
for lib in $(readelf -d "$app/build/ps5/link/llvm-pie.elf" | sed -n 's/.*Shared library: \[\(.*\)\.s\?prx\]/\1/p'); do
    [[ " $loaded " == *" $lib "* ]] || { echo "Needed module $lib has not been seen loading on the console" >&2; exit 1; }
done
python3 "$validator" "$app/dist/PPSA99305" --generic --title-id PPSA99305 --write-report
echo "Built $app/dist/PPSA99305"
