#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

if [[ "$PLATFORMS" != all ]]; then
  echo 'Refusing a partial-platform draft release. Run all platforms from one commit.' >&2
  exit 1
fi

tag="v$VERSION"
if release=$(gh release view "$tag" --json isDraft,targetCommitish); then
  if [[ $(jq -r .isDraft <<< "$release") != true ]]; then
    echo 'Refusing to modify a published release.' >&2
    exit 1
  fi
  if [[ $(jq -r .targetCommitish <<< "$release") != "$GITHUB_SHA" ]]; then
    echo 'Refusing to update a draft from another revision. Delete the draft (or bump the version), then run all platforms from one commit.' >&2
    exit 1
  fi
else
  gh release create "$tag" --draft --target "$GITHUB_SHA" --title "$tag" \
    --notes 'Unsigned packages; see docs/guides/installing.md for first-launch steps.'
fi
gh release upload "$tag" dist/* --clobber
