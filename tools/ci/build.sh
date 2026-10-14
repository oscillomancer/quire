#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One-command build from a clean clone: configure, build and test a preset.
# Usage: tools/ci/build.sh [preset]   (default: dev)
set -euo pipefail
cd "$(dirname "$0")/../.."
preset="${1:-dev}"
cmake --workflow --preset "$preset"
