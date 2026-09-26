#!/usr/bin/env bash
# PS5 Platform - build the platform library for the console with a project's
# payload SDK.
#
#   tools/build-ps5.sh <payload-sdk-dir> <output-dir>
#
# Writes <output-dir>/libps5platform.a. Every consumer builds it with its own
# SDK copy, from the revision it pins, so the archive always matches the
# toolchain that links it.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
[[ $# == 2 ]] || { echo "usage: ${0##*/} <payload-sdk-dir> <output-dir>" >&2; exit 2; }
sdk=$(cd -- "$1" && pwd)
out=$(mkdir -p -- "$2" && cd -- "$2" && pwd)
[[ -x $sdk/bin/prospero-clang ]] || { echo "error: no prospero-clang in $sdk" >&2; exit 2; }
export PS5_PAYLOAD_SDK="$sdk"
export PS5_CLANG=${PS5_CLANG:-/usr/bin/clang}
objects=()
for source in "$root"/src/*.c; do
    object="$out/$(basename "${source%.c}").o"
    "$sdk/bin/prospero-clang" -std=c11 -O2 -fPIC -Wall -Wextra -Werror \
        -I "$root/include" -c "$source" -o "$object"
    objects+=("$object")
done
rm -f -- "$out/libps5platform.a"
"$sdk/bin/prospero-ar" rcs "$out/libps5platform.a" "${objects[@]}"
echo "==> [platform] $out/libps5platform.a (${#objects[@]} objects)"
