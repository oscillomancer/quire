#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Run the pinned validator headlessly and retain its complete JSON and text summary.
set -euo pipefail

# --exclude REGEX skips the tests it matches (clap-validator's own option).
exclude=()
if [[ "${1:-}" == --exclude && $# -ge 2 ]]; then
  exclude=(--exclude "$2")
  shift 2
fi
if [[ $# -eq 0 ]]; then
  echo 'usage: tools/validator/run.sh [--exclude REGEX] <plugin.clap>...' >&2
  exit 2
fi

root="$(cd "$(dirname "$0")/../.." && pwd)"
slug=$(sed -n 's/^set(PRODUCT_SLUG "\(.*\)").*/\1/p' "$root/cmake/Product.cmake")
# Windows (Git Bash) names the binary .exe and has `python`, not `python3`.
exe=""
python=python3
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) exe=.exe python=python ;; esac
binary="${CLAP_VALIDATOR_BIN:-${XDG_CACHE_HOME:-${HOME}/.cache}/${slug}-tools/clap-validator/0.4.1/target/release/clap-validator${exe}}"
if [[ ! -x "${binary}" ]]; then
  echo "SKIP: clap-validator 0.4.1 is absent; run ${root}/tools/validator/build.sh" >&2
  exit 77
fi

mkdir -p "${root}/build/validator"
result=0
for input in "$@"; do
  if [[ ! -e "${input}" ]]; then  # a macOS .clap is a bundle, a folder
    echo "clap-validator: plugin does not exist: ${input}" >&2
    result=1
    continue
  fi
  plugin="$(realpath "${input}")"
  name="$(basename "${plugin}" .clap)"
  json="${root}/build/validator/${name}.json"
  report="${root}/build/validator/${name}.txt"
  stderr="${root}/build/validator/${name}.stderr"

  # The validator's subprocess timeout helper is blocked by the Codex sandbox.
  # In-process mode still runs every headless validation check.
  code=0
  "${binary}" validate --json --in-process "${exclude[@]}" "${plugin}" >"${json}" 2>"${stderr}" || code=$?
  summary=0
  "${python}" - "${json}" "${report}" "${plugin}" "$("${binary}" --version)" "${code}" <<'PY' || summary=$?
import collections
import json
import sys

json_path, report_path, plugin, version, code = sys.argv[1:]
with open(report_path, "w", encoding="utf-8") as report:
    print(version, file=report)
    print(f"plugin: {plugin}", file=report)
    print(f"validator exit: {code}", file=report)
    try:
        with open(json_path, encoding="utf-8") as source:
            results = json.load(source)["results"]
    except (OSError, ValueError, KeyError) as error:
        print(f"could not read validator JSON: {error}", file=report)
        raise SystemExit(1)
    else:
        counts = collections.Counter(item["status"]["code"] for item in results)
        print("results: " + ", ".join(f"{key}={counts[key]}" for key in sorted(counts)), file=report)
        for item in results:
            status = item["status"]
            if status["code"] in ("success", "skipped"):
                continue
            test = next(iter(item["test"].values()))["test"]
            print(f"{status['code'].upper()}: {test}: {status.get('details')}", file=report)
PY
  cat "${report}"
  if (( code != 0 || summary != 0 )); then result=1; fi
done
exit "${result}"
