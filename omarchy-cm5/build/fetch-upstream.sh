#!/bin/bash
# Clone upstream omarchy at the ref pinned in upstream.lock into build/upstream.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=../upstream.lock
source "$here/../upstream.lock"

dest="$here/upstream"

if [[ -d $dest/.git ]]; then
  git -C "$dest" fetch origin "$OMARCHY_BRANCH"
else
  git clone --branch "$OMARCHY_BRANCH" "$OMARCHY_REPO" "$dest"
fi

git -C "$dest" checkout --detach "$OMARCHY_REF"
echo "upstream omarchy at $(git -C "$dest" rev-parse --short HEAD) ($OMARCHY_VERSION)"
