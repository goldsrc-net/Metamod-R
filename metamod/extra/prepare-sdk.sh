#!/usr/bin/env bash
#
# prepare-sdk.sh — stage the per-arch publish/ tree with the upstream
# Metamod-R release shape: addons/metamod/{config.ini,plugins.ini},
# sdk/<headers>, example_plugin/{Makefile,sources,include/{hlsdk,metamod}}.
#
# Called from each per-arch build job before the arch-specific binary
# is moved into publish/addons/metamod/.  Cross-platform (uses cp only,
# no rsync) so it runs on github-hosted Linux and Windows runners.
#
# Usage:  bash metamod/extra/prepare-sdk.sh [publish_dir]
#         (default: publish/)

set -euo pipefail
PUB="${1:-publish}"

# 1. SDK headers (the public Metamod-R API).  Selective copy from
#    metamod/src/ — only the seven headers a plugin author needs.
mkdir -p "$PUB/sdk" "$PUB/addons/metamod"
for h in dllapi.h engine_api.h enginecallbacks.h h_export.h meta_api.h mutil.h plinfo.h; do
	cp "metamod/src/$h" "$PUB/sdk/"
done

# 2. Metamod-R runtime config files (drop-in alongside metamod_<arch>.{so,dll}).
cp metamod/extra/config.ini  "$PUB/addons/metamod/"
cp metamod/extra/plugins.ini "$PUB/addons/metamod/"

# 3. example_plugin source tree (Makefile + .cpp/.h + bundled hlsdk).
#    The trailing `.` on the src copies *contents*, not the dir itself.
mkdir -p "$PUB/example_plugin"
cp -R metamod/extra/example/. "$PUB/example_plugin/"
# Strip MSBuild output that may have landed in the source tree before
# this step ran.  The windows job builds the main solution first, and
# example_plugin.vcxproj is a member project — its compile/link output
# goes to msvc/{Release,Debug,x64}/, none of which are part of
# upstream's release-zip shape.
rm -rf "$PUB/example_plugin/msvc/Release" \
       "$PUB/example_plugin/msvc/Debug"   \
       "$PUB/example_plugin/msvc/x64"     \
       "$PUB/example_plugin/Release"

# 4. Refresh example_plugin's bundled include/metamod/ headers from the
#    just-staged SDK so the template builds against current API surface.
mkdir -p "$PUB/example_plugin/include/metamod"
cp "$PUB"/sdk/*.h "$PUB/example_plugin/include/metamod/"
