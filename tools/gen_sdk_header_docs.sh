#!/usr/bin/env bash
# File:        gen_sdk_header_docs.sh
# Purpose:     Emit the tiered SDK header reference (Markdown) from the single
#              source manifest orc/sdk/sdk_headers.yaml, to stdout. The block is
#              spliced into docs/technical/plugin-sdk.md between the
#              GENERATED-SDK-HEADER-TABLES markers; the SdkHeaderDocsSync CTest
#              fails if the committed block and this output diverge.
#
# Usage:       tools/gen_sdk_header_docs.sh [<repo-root>]
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu
REPO_ROOT="${1:-.}"
MANIFEST="${REPO_ROOT}/orc/sdk/sdk_headers.yaml"

# Flatten the manifest to TSV: path<TAB>tier<TAB>domain<TAB>deprecated<TAB>notes
flatten() {
    awk '
        /^[[:space:]]*-[[:space:]]*path:/ {
            if (havePath) print rec();
            path=val($0); tier=""; domain=""; dep=""; notes=""; havePath=1; next
        }
        /^[[:space:]]*tier:/     { tier=val($0);   next }
        /^[[:space:]]*domain:/   { domain=strip(val($0)); next }
        /^[[:space:]]*deprecated:/ { dep=val($0); next }
        /^[[:space:]]*notes:/    { notes=strip(val($0)); next }
        END { if (havePath) print rec() }
        function val(l){ sub(/^[^:]*:[[:space:]]*/,"",l); return l }
        function strip(s){ gsub(/^"|"$/,"",s); return s }
        function rec(){ return path"\t"tier"\t"domain"\t"dep"\t"notes }
    ' "$MANIFEST"
}

DATA="$(flatten)"

emit_group() {  # $1 tier  $2 domain(optional, "" = all in tier)
    local tier="$1" domain="$2"
    printf '%s\n' "$DATA" | awk -F'\t' -v t="$tier" -v d="$domain" '
        $4=="false" && $2==t && (d=="" || $3==d) {
            printf "| `<%s>` | %s |\n", $1, ($5==""?"—":$5)
        }' | sort
}

echo '<!-- BEGIN GENERATED SDK HEADER TABLES (source: orc/sdk/sdk_headers.yaml; regenerate with tools/gen_sdk_header_docs.sh) -->'
echo

echo '#### `orc/abi/` — frozen binary contract'
echo
echo 'Stability: **any change bumps the host ABI version.** Descriptor,'
echo 'entrypoints, registration, and service tables.'
echo
echo '| Header | Provides |'
echo '|--------|----------|'
emit_group abi ""
echo

echo '#### `orc/plugin/` — plugin API surface (transitional)'
echo
echo 'Plugin-facing stage API and host-services headers not yet relocated under'
echo 'a tier subdirectory. Treated as ABI: layout changes bump the host ABI.'
echo
echo '| Header | Provides |'
echo '|--------|----------|'
emit_group plugin-api ""
echo

echo '#### `orc/stage/` — stage contract'
echo
echo 'Stage interfaces and data-contract types that cross the plugin boundary,'
echo 'grouped by domain. A layout change here bumps the host ABI version.'
for dom in foundation audio dropout observation params preview tooling; do
    # Skip a domain with no rows.
    if [ -z "$(emit_group stage "$dom")" ]; then continue; fi
    echo
    echo "**${dom}**"
    echo
    echo '| Header | Provides |'
    echo '|--------|----------|'
    emit_group stage "$dom"
done
echo

echo '#### `orc/support/` — compiled-into-plugin utilities'
echo
echo 'NOT part of the binary ABI. Changes never force an ABI bump; recompile the'
echo 'plugin at the author'"'"'s convenience.'
echo
echo '| Header | Provides |'
echo '|--------|----------|'
emit_group support ""
echo

echo '#### `orc/view/` — view binary contract'
echo
echo 'The contract between `orc-gui` and a plugin'"'"'s view binary. Versioned by'
echo '`kViewPluginAbiVersion`, not the stage host ABI. No Qt header is included;'
echo 'a view binary brings its own.'
echo
echo '| Header | Provides |'
echo '|--------|----------|'
emit_group view ""
echo

echo '#### `orc/view-support/` — widgets for view binaries'
echo
echo 'Qt widgets `orc-gui` ships in the `orc-view-support` shared library, so a'
echo 'view draws with the same plot as `orc-gui`'"'"'s own windows. Link'
echo '`orc::view-support`; at run time the view uses the copy `orc-gui` has'
echo 'loaded. Versioned with the view tier: a layout change bumps'
echo '`kViewPluginAbiVersion`. The only SDK tier that includes Qt.'
echo
echo '| Header | Provides |'
echo '|--------|----------|'
emit_group view-support ""
echo

dep_count="$(printf '%s\n' "$DATA" | awk -F'\t' '$4=="true"{n++} END{print n+0}')"
echo '#### Deprecated pre-tier include paths'
echo
echo "The ${dep_count} flat \`<orc/plugin/...>\` / \`<orc/stage/...>\` paths that"
echo 'predate this layout are retained as forwarding shims for one release, gated'
echo 'by the `ORC_SDK_DEPRECATED_INCLUDE_SHIMS` CMake option (default ON). New'
echo 'code must use the tiered paths above; building with the option OFF turns any'
echo 'remaining pre-tier include into a hard compile error.'
echo
echo '<!-- END GENERATED SDK HEADER TABLES -->'
