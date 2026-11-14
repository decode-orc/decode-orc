#!/usr/bin/env bash
# File:        check_sdk_header_hygiene.sh
# Purpose:     SDK self-hygiene gate. Every #include in an SDK header
#              (orc/sdk/include) must resolve to one of:
#                - an SDK tier: <orc/abi/...>, <orc/stage/...>,
#                  <orc/support/...>, or the (transitional) <orc/plugin/...>
#                  plugin-API surface;
#                - a C or C++ standard-library header;
#                - the spdlog / fmt logging surface;
#                - a platform / OS header — permitted ONLY in orc/support/
#                  (the support tier owns the single file-I/O dependency).
#              Any include of a private host tree (orc/core, orc/common,
#              orc/presenters, orc/view-types, Qt, ...) or a platform header
#              outside orc/support/ fails the gate (CTest label "sdk").
#
# Usage:       check_sdk_header_hygiene.sh [<repo-root>]
#
# SPDX-License-Identifier: GPL-3.0-or-later

set -u

REPO_ROOT="${1:-.}"
INC="${REPO_ROOT}/orc/sdk/include"

# Platform / OS headers: allowed only inside the support tier.
PLATFORM_HEADERS="dlfcn.h windows.h unistd.h fcntl.h io.h share.h"

# C standard-library headers (C++ headers are matched structurally: an angle
# name with no '.' and no '/').
C_STD_HEADERS="assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h \
iso646.h limits.h locale.h math.h setjmp.h signal.h stdalign.h stdarg.h \
stdatomic.h stdbool.h stddef.h stdint.h stdio.h stdlib.h stdnoreturn.h \
string.h tgmath.h threads.h time.h uchar.h wchar.h wctype.h"

in_words() {  # $1 needle, $2 space-separated list
    local w
    for w in $2; do [ "$1" = "$w" ] && return 0; done
    return 1
}

VIOLATIONS=0

while IFS= read -r hdr; do
    rel="${hdr#"$REPO_ROOT"/}"
    is_support=0
    case "$hdr" in
        "$INC"/orc/support/*) is_support=1 ;;
    esac

    while IFS= read -r inc; do
        [ -n "$inc" ] || continue
        case "$inc" in
            orc/abi/*|orc/stage/*|orc/support/*|orc/plugin/*|orc/view/*)
                continue ;;              # SDK tiers
            orc/*)
                echo "❌ ${rel}: includes <${inc}> — private host header (not an SDK tier)"
                VIOLATIONS=$((VIOLATIONS + 1)); continue ;;
            fmt/*|spdlog/*)
                continue ;;              # logging surface
        esac
        # C++ stdlib: angle name with no '.' and no '/'.
        case "$inc" in
            */*) : ;;
            *.*) : ;;
            *) continue ;;
        esac
        if in_words "$inc" "$C_STD_HEADERS"; then
            continue
        fi
        # Sibling SDK header included by bare/relative name (e.g.
        # "orc_stage_tooling.h" from orc_stage_runtime.h).
        if [ -f "$(dirname "$hdr")/${inc}" ]; then
            continue
        fi
        if in_words "$inc" "$PLATFORM_HEADERS"; then
            if [ "$is_support" -eq 1 ]; then
                continue
            fi
            echo "❌ ${rel}: includes platform header <${inc}> outside the support tier"
            VIOLATIONS=$((VIOLATIONS + 1)); continue
        fi
        echo "❌ ${rel}: includes <${inc}> — not an SDK tier, stdlib, spdlog/fmt, or (support-only) platform header"
        VIOLATIONS=$((VIOLATIONS + 1))
    done < <(grep -hoE '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^">]+[">]' \
        "$hdr" 2>/dev/null \
        | sed -E 's/^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]([^">]+)[">]/\1/')
done < <(find "$INC" -type f -name '*.h' | sort)

echo
if [ "$VIOLATIONS" -eq 0 ]; then
    echo "✓ SDK headers are hygiene-clean (no private-host or stray includes)"
    exit 0
fi
echo "❌ HARD GATE FAILURE: $VIOLATIONS SDK header hygiene violation(s)"
exit 1
