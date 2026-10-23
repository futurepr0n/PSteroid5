#!/usr/bin/env bash
set -euo pipefail
work=$1 vulkan=$2 sdk=$3
shift 3
objects=("$@")
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
tool=$vulkan/build/host/ps5-native-tool
mkdir -p "$work/link/obj" "$work/link/stubs"
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -c "$vulkan/tooling/native/app_crt.cpp" -o "$work/link/obj/app_crt.o"
for library in libSceAgc libSceAgcDriver; do
    source=vendor/ps5/sdk/stubs/agc_canary_link_stub.c
    [[ $library == libSceAgcDriver ]] && source=vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c
    cc -O2 -fPIC -c "$vulkan/$source" -o "$work/link/obj/$library.o"
    "$sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$work/link/stubs/$library.so" "$work/link/obj/$library.o"
done
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a"
radv_link_flags+=(-z nodynamic-undefined-weak)
for name in getenv setenv unsetenv putenv; do radv_link_flags+=("--defsym=$name=rpcs3ps5_$name"); done
printf '{ local: getenv; setenv; unsetenv; putenv; };\n' > "$work/link/environment.map"
"$sdk/bin/prospero-lld" --error-limit=0 "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
    --version-script "$work/link/environment.map" --version-script "$vulkan/tooling/native/app-symbols.map" \
    --exclude-libs=ALL -e _start -o "$work/link/llvm-pie.elf" "$work/link/obj/app_crt.o" "${objects[@]}" \
    "$work/link/stubs/libSceAgc.so" "$work/link/stubs/libSceAgcDriver.so" "${radv_link_inputs[@]}" --as-needed "$sdk"/target/lib/*.so
"$tool" link --in "$work/link/llvm-pie.elf" --out "$work/eboot.elf" --stub-dir "$sdk/target/lib" \
    --stub "$work/link/stubs/libSceAgc.so" --stub "$work/link/stubs/libSceAgcDriver.so" \
    --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/sce_sys/param.json")
out=$root/dist/$title_id
mkdir -p "$out/sce_sys" "$out/sce_module" "$out/shaders/glsl"
"$tool" self --sign --in "$work/eboot.elf" --out "$out/eboot.bin" --magic 0x1D3D154F
"$tool" self --inspect --file "$out/eboot.bin"
cp "$root/ps5/sce_sys/param.json" "$out/sce_sys/param.json"
test -s "$root/ps5/sce_sys/icon0.png"
cp "$root/ps5/sce_sys/icon0.png" "$out/sce_sys/icon0.png"
(cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
cp "$vulkan/runtime/libc.prx" "$out/sce_module/"
cp -a "$root/shaders/glsl/base" "$out/shaders/glsl/"
python3 "$root/ps5/tools/port-notices.py" "$out"
