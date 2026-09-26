#!/usr/bin/env bash
# Regenerates the committed generator output under skill/generated/ from the
# dev build (checked by the sandbox.registry-drift test).
set -euo pipefail
cd "$(dirname "$0")/../.."
slug=$(sed -n 's/^set(PRODUCT_SLUG "\(.*\)").*/\1/p' cmake/Product.cmake)
tool=build/dev/host/sandbox/${slug}-sandbox
cmake --build --preset dev --target "${slug}-sandbox" >/dev/null
"$tool" registry --markdown > skill/generated/sandbox-commands.md
"$tool" registry --mcp-tools > skill/generated/sandbox-mcp-tools.json
engine=build/dev/host/engine/${slug}-engine
cmake --build --preset dev --target "${slug}-engine" >/dev/null
"$engine" registry --markdown > skill/generated/engine-commands.md
"$engine" registry --mcp-tools > skill/generated/engine-mcp-tools.json
cmake --build --preset dev --target notation-ref >/dev/null
build/dev/libs/notation/notation-ref > skill/generated/notation-reference.md
echo "updated skill/generated/{sandbox,engine}-{commands.md,mcp-tools.json} and notation-reference.md"
