#!/usr/bin/env bash
# builds the tree-sitter grammar in a scratch folder and parses every .embr file in the repo with it. a syntax error in any of
# them means the grammar has drifted from include/embr/core/parser.h. needs node and npm, and network the first time (it
# installs tree-sitter-cli).
#
#   tools/editor/tree-sitter/check.sh
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
cp "$here/grammar.js" "$here/package.json" "$work/"
cd "$work"
npm install --silent >/dev/null 2>&1 || { echo "npm install failed (node, npm and network are needed)"; exit 77; }
npx tree-sitter generate >/dev/null 2>&1 || { npx tree-sitter generate; exit 1; }
bad=0; total=0
while IFS= read -r f; do
    total=$((total + 1))
    if npx tree-sitter parse "$f" 2>/dev/null | grep -q "ERROR\|MISSING"; then echo "syntax error: $f"; bad=$((bad + 1)); fi
done < <(find "$root" -name '*.embr' -not -path '*/build/*' -not -path '*/bin/*' -not -path '*/node_modules/*' -not -path '*/.git/*' | sort)
echo "tree-sitter grammar: $((total - bad)) of $total files parse"
exit $((bad ? 1 : 0))
