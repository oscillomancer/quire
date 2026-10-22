#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the pinned development-only CLAP validator in a user-space cache.
# Prints the binary path. Once built, this works without network access.
set -euo pipefail

tag=0.4.1
cache_base="${XDG_CACHE_HOME:-${HOME}/.cache}/quire-tools/clap-validator/${tag}"
source_dir="${cache_base}/source"
exe=""
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*)
  exe=.exe
  # Git's bash puts its coreutils link.exe ahead of MSVC's on the PATH. In a
  # Visual Studio environment, name MSVC's linker for cargo.
  if [[ -n "${VCToolsInstallDir:-}" ]]; then
    msvc_link="${VCToolsInstallDir%\\}\\bin\\Hostx64\\x64\\link.exe"
    if [[ -f "$(cygpath -u "${msvc_link}")" ]]; then
      export CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_LINKER="${msvc_link}"
    fi
  fi ;;
esac
binary="${cache_base}/target/release/clap-validator${exe}"

if [[ -x "${binary}" ]]; then
  printf '%s\n' "${binary}"
  exit 0
fi

command -v git >/dev/null || { echo 'clap-validator: git is required' >&2; exit 1; }
command -v cargo >/dev/null || { echo 'clap-validator: cargo is required' >&2; exit 1; }
mkdir -p "${cache_base}"
if [[ ! -f "${source_dir}/Cargo.toml" ]]; then
  git clone --depth 1 --branch "${tag}" \
    https://github.com/free-audio/clap-validator.git "${source_dir}" >&2
fi

if [[ "$(git -C "${source_dir}" describe --tags --exact-match)" != "${tag}" ]]; then
  echo "clap-validator: source cache is not at release ${tag}" >&2
  exit 1
fi

CARGO_TARGET_DIR="${cache_base}/target" \
  cargo build --locked --release --jobs 6 --bin clap-validator \
    --manifest-path "${source_dir}/Cargo.toml" >&2
printf '%s\n' "${binary}"
