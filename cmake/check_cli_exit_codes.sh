#!/usr/bin/env bash
# File:        check_cli_exit_codes.sh
# Purpose:     Gate: the `plugins` and `stages` commands return the documented
#              exit code for every outcome, and none of them blocks on input
#              that cannot arrive.
#
# Usage:       check_cli_exit_codes.sh <path-to-orc-cli>
#
# The codes are defined in orc/cli/cli_exit_codes.h and documented in
# docs/cli-user-guide/overview.md; a script tells "does not exist" from "the
# network was down" from "you declined to trust a binary" by them, so each is
# pinned here rather than left to WILL_FAIL, which any non-zero code satisfies.
#
# Registry-only: the fixture registry is built under a redirected
# HOME/XDG_CONFIG_HOME, the curated index is seeded as an empty last-good cache
# and its fetch pointed at a closed port. Every invocation reads stdin from
# /dev/null, so a command that decided to wait for an answer fails the gate
# instead of hanging it. Kept bash-3.2 compatible.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

set -u

ORC_CLI="${1:-}"
if [[ -z "$ORC_CLI" || ! -x "$ORC_CLI" ]]; then
    echo "Usage: $0 <path-to-orc-cli>" >&2
    exit 1
fi

readonly EXIT_SUCCESS=0
readonly EXIT_USAGE=1
readonly EXIT_NOT_FOUND=2
readonly EXIT_INDEX_UNAVAILABLE=3
readonly EXIT_TRUST_DECLINED=4

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

export HOME="$WORK_DIR/home"
export XDG_CONFIG_HOME="$WORK_DIR/home"
export APPDATA="$WORK_DIR/home"
mkdir -p "$HOME/decode-orc"

# A reachable index holding one fixture entry: 'info' and 'install' can report
# that an id is in neither place (2) rather than that they could not look (3),
# and 'install' can get as far as the trust prompt for the listed id. The
# fetch itself points at a closed port, so nothing leaves the machine.
export ORC_PLUGIN_INDEX_URL="http://127.0.0.1:9/index.yaml"
CACHE="$HOME/decode-orc/plugin-index-cache.yaml"
seed_cache() {
    cat > "$CACHE" <<'CACHE_EOF'
registry_schema: 2
plugins:
  - id: com.example.indexed
    display_name: Indexed Example
    license_spdx: MIT
    source_repo_url: https://example.invalid/releases
CACHE_EOF
}
seed_cache

case "$(uname -s)" in
    Darwin) PLUGIN_EXT=".dylib" ;;
    MINGW*|MSYS*|CYGWIN*) PLUGIN_EXT=".dll" ;;
    *) PLUGIN_EXT=".so" ;;
esac

FAILURES=0

fail() {
    echo "FAIL: $*" >&2
    FAILURES=$((FAILURES + 1))
}

# Run orc-cli with stdin closed and check the exit code.
# Usage: expect_code <expected> <description> <args...>
expect_code() {
    local expected="$1"
    local description="$2"
    shift 2
    "$ORC_CLI" "$@" </dev/null >/dev/null 2>&1
    local status=$?
    if [[ "$status" -ne "$expected" ]]; then
        fail "$description: exited $status, expected $expected"
    fi
}

PLUGIN="$WORK_DIR/exit_fixture${PLUGIN_EXT}"
touch "$PLUGIN"
SELECTOR="com.example.exitcodes"

# --- 4: trust was required and not granted ----------------------------------
#
# There is no terminal to confirm on, so each of these must decline rather than
# wait. Nothing may be recorded by a declined command.

expect_code "$EXIT_TRUST_DECLINED" "'plugins add' with no confirmation" \
    plugins add "$PLUGIN" --id "$SELECTOR"
expect_code "$EXIT_TRUST_DECLINED" "'plugins install' with no confirmation" \
    plugins install com.example.indexed
if "$ORC_CLI" plugins list </dev/null 2>/dev/null | grep -q "$SELECTOR"; then
    fail "a declined 'plugins add' recorded an entry anyway"
fi

# The registry now gets its fixture, with the confirmation given up front.
if ! "$ORC_CLI" plugins add "$PLUGIN" --id "$SELECTOR" --version 1.0.0 --yes \
        </dev/null >/dev/null 2>&1; then
    fail "'plugins add --yes' did not add the fixture plugin"
fi

expect_code "$EXIT_TRUST_DECLINED" "'plugins trust' with no confirmation" \
    plugins trust "$SELECTOR"
expect_code "$EXIT_TRUST_DECLINED" "'plugins update' with no confirmation" \
    plugins update "$SELECTOR"

# --- 2: the thing named does not exist --------------------------------------

expect_code "$EXIT_NOT_FOUND" "'plugins info' on an unknown id" \
    plugins info com.example.absent
expect_code "$EXIT_NOT_FOUND" "'plugins remove' on an unknown selector" \
    plugins remove com.example.absent
expect_code "$EXIT_NOT_FOUND" "'plugins remove --dry-run' on an unknown selector" \
    plugins remove --dry-run "path:$WORK_DIR/absent${PLUGIN_EXT}"
expect_code "$EXIT_NOT_FOUND" "'plugins enable' on an unknown selector" \
    plugins enable com.example.absent --yes
expect_code "$EXIT_NOT_FOUND" "'plugins disable' on an unknown selector" \
    plugins disable com.example.absent
expect_code "$EXIT_NOT_FOUND" "'plugins install' on an id not in the index" \
    plugins install com.example.absent --yes
expect_code "$EXIT_NOT_FOUND" "'plugins update' on an unknown selector" \
    plugins update com.example.absent --yes
expect_code "$EXIT_NOT_FOUND" "'stages info' on an unknown stage" \
    stages info definitely_not_a_stage
expect_code "$EXIT_NOT_FOUND" "'stages help' on an unknown stage" \
    stages help definitely_not_a_stage
expect_code "$EXIT_NOT_FOUND" "'stages list --plugin' on an unknown plugin" \
    stages list --plugin com.example.absent

# --- 3: the index could not be reached --------------------------------------
#
# Without the cache there is no listing to fall back on, and the fetch has
# nowhere to go.

rm -f "$CACHE"
expect_code "$EXIT_INDEX_UNAVAILABLE" "'plugins search' with no index" \
    plugins search
expect_code "$EXIT_INDEX_UNAVAILABLE" "'plugins search --json' with no index" \
    plugins search --json
expect_code "$EXIT_INDEX_UNAVAILABLE" "'plugins install' with no index" \
    plugins install com.example.indexed --yes
seed_cache

# A listed id whose release fetch has nowhere to go: the install resolves and
# is confirmed, and the failure is the network's, not the arguments'.
expect_code "$EXIT_INDEX_UNAVAILABLE" \
    "'plugins install --yes' when the release cannot be fetched" \
    plugins install com.example.indexed --yes

# --- 1: bad arguments -------------------------------------------------------

expect_code "$EXIT_USAGE" "'plugins list' with an unknown option" \
    plugins list --frobnicate
expect_code "$EXIT_USAGE" "'plugins' with an unknown subcommand" \
    plugins frobnicate
expect_code "$EXIT_USAGE" "'stages' with an unknown subcommand" \
    stages frobnicate
expect_code "$EXIT_USAGE" "'stages info' with an unknown option" \
    stages info tbc_source --frobnicate
# The options removed with the command-line pipelines (issue #378) are refused
# as bad arguments, with a pointer to the migration notes.
for removed_option in --source -i --filters -f --sink -o --export-project \
        --video-format --source-type; do
    expect_code "$EXIT_USAGE" "removed option '$removed_option'" \
        "$removed_option" value
done
# The fixture resolves and is confirmed, but a local file has no releases to
# update from: a plain failure, neither not-found nor a network problem.
expect_code "$EXIT_USAGE" "'plugins update --yes' on a local-file plugin" \
    plugins update "$SELECTOR" --yes

# --- 0: the commands that need no confirmation, and the ones that got it ----
#
# One read-only command per family, and every mutating plugin command with
# --yes: none of them may block, so a redirected stdin must still exit 0.

expect_code "$EXIT_SUCCESS" "'plugins list' with stdin redirected" plugins list
expect_code "$EXIT_SUCCESS" "'plugins list --json' with stdin redirected" \
    plugins list --json
expect_code "$EXIT_SUCCESS" "'plugins doctor' with stdin redirected" \
    plugins doctor
# The id 'plugins search' prints is accepted by 'plugins info' unchanged.
expect_code "$EXIT_SUCCESS" "'plugins info' on an id the index lists" \
    plugins info com.example.indexed
expect_code "$EXIT_SUCCESS" "'stages list' with stdin redirected" stages list
expect_code "$EXIT_SUCCESS" "'stages info' with stdin redirected" \
    stages info tbc_source
expect_code "$EXIT_SUCCESS" "'plugins trust --yes'" \
    plugins trust "$SELECTOR" --yes
expect_code "$EXIT_SUCCESS" "'plugins untrust'" plugins untrust "$SELECTOR"
expect_code "$EXIT_SUCCESS" "'plugins enable --yes' on an untrusted entry" \
    plugins enable "$SELECTOR" --yes
expect_code "$EXIT_SUCCESS" "'plugins disable'" plugins disable "$SELECTOR"
expect_code "$EXIT_SUCCESS" "'plugins remove --dry-run'" \
    plugins remove --dry-run "$SELECTOR"
# Removal never prompts, but --yes is accepted so a script can pass the flag
# uniformly to every mutating command.
expect_code "$EXIT_SUCCESS" "'plugins remove --yes'" \
    plugins remove "$SELECTOR" --yes

if [[ "$FAILURES" -eq 0 ]]; then
    echo "CLI exit-code contract: passed"
    exit 0
fi

echo "CLI exit-code contract: failed ($FAILURES violation(s))" >&2
exit 1
