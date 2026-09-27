#!/usr/bin/env bash
#   Copyright (C) 2026 Mihawk
#
# This file is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not see
# <http://www.gnu.org/licenses/>.
#
# Installs this fork of the payload SDK into a project's SDK directory: the
# upstream release this fork is based on, whose binaries (crt, libc, stubs,
# libc++, tools) are the ones validated on the console, with this tree's
# headers and platform layer installed over it.
#
#   platform/tools/setup-sdk.sh <sdk-dir> <revision> [download-cache]
#
# Run it from an export of the pinned revision, never a working tree:
#
#   git -C <fork> archive <revision> | tar -x -C <tree>
#   bash <tree>/platform/tools/setup-sdk.sh <sdk-dir> <revision> <cache>
#
# <sdk-dir> is replaced as a whole and records <revision> in
# .ps5-sdk-revision, which consumers compare against their pin.
set -euo pipefail

release_url="https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip"
release_hash="8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da"

[[ $# == 2 || $# == 3 ]] || {
    echo "usage: ${0##*/} <sdk-dir> <revision> [download-cache]" >&2
    exit 2
}
tree=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
sdk=$1
revision=$2
cache=${3:-$(dirname -- "$sdk")}
[[ $revision =~ ^[0-9a-f]{40}$ ]] || { echo "revision must be a full commit id" >&2; exit 2; }
for command in flock wget unzip sha256sum make; do
    command -v "$command" >/dev/null || { echo "missing required command: $command" >&2; exit 2; }
done

mkdir -p -- "$cache"
# One install at a time: a project's make runs its targets in parallel, and
# each asks for the SDK. Whoever waited finds it installed and stops.
exec 9>"$cache/.ps5-sdk.lock"
flock 9
if [[ -f $sdk/.ps5-sdk-revision && $(<"$sdk/.ps5-sdk-revision") == "$revision" ]]; then
    exit 0
fi
archive="$cache/ps5-payload-sdk.zip"
if [[ -f $archive ]] &&
    ! printf '%s  %s\n' "$release_hash" "$archive" | sha256sum --check --strict >/dev/null 2>&1; then
    rm -f -- "$archive"
fi
if [[ ! -f $archive ]]; then
    wget -q "$release_url" -O "$archive.download"
    mv -- "$archive.download" "$archive"
fi
printf '%s  %s\n' "$release_hash" "$archive" | sha256sum --check --strict >/dev/null

staging=$(mktemp -d "$cache/.ps5-sdk.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT
unzip -q "$archive" -d "$staging"
[[ -x $staging/ps5-payload-sdk/bin/prospero-lld ]] || {
    echo "the release archive has no ps5-payload-sdk/bin/prospero-lld" >&2
    exit 2
}
# The release archive carries target/include_common as an empty directory,
# where an install makes a link to user/homebrew/include; the install makes it.
rmdir -- "$staging/ps5-payload-sdk/target/include_common"
# The fork's compiler wrappers, which differ from the release's only in what the
# fork changed (IEEE denormals, host/bin/prospero-clang).
install -m 0755 "$tree/host/bin/prospero-clang" "$tree/host/bin/prospero-clang++" "$staging/ps5-payload-sdk/bin/"
make -s -C "$tree/include" install DESTDIR="$staging/ps5-payload-sdk"
make -s -C "$tree/platform" clean
make -s -C "$tree/platform" install DESTDIR="$staging/ps5-payload-sdk"
printf '%s\n' "$revision" >"$staging/ps5-payload-sdk/.ps5-sdk-revision"

# Into place by content: a file the new revision did not change keeps its
# time, so what builds against the SDK (the CTS, RADV) rebuilds only what the
# change touched instead of everything.
if command -v rsync >/dev/null; then
    mkdir -p -- "$sdk"
    rsync -rlp --checksum --delete -- "$staging/ps5-payload-sdk/" "$sdk/"
else
    rm -rf -- "$sdk"
    mv -- "$staging/ps5-payload-sdk" "$sdk"
fi
echo "==> [sdk] $sdk: payload SDK v0.42 with the PS5 fork at $revision"
