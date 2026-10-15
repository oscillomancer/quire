#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# key=value lines describing the product, for $GITHUB_OUTPUT.
set -euo pipefail
cd "$(dirname "$0")/../.."
cmake -P .github/scripts/product-info.cmake | tee /dev/stderr
