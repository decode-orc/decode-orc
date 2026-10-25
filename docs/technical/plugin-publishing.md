# Plugin Publishing Guide

How to package, release, and distribute a third-party Decode-Orc stage plugin so
users can install it — either from a hand-written registry entry or, preferably,
from the curated plugin index.

This is the release-side companion to the
[Plugin Author Guide](plugin-author-guide.md) (which covers building and loading
a plugin locally). Reference material lives in
[Plugin SDK Developer Guide → Distribution](plugin-sdk.md#distribution) and
[Plugin Architecture](plugin-architecture.md).

---

## 1. Repository naming (recommended)

Prefer hosting your plugin in its own repository named with the `orc-plugin_`
prefix:

```
orc-plugin_<stage-name>     e.g.  orc-plugin_deinterlace
```

This convention matches the release-asset naming below and makes plugin
repositories easy to recognise, but it is not enforced — a plugin may live in
a repository with any name (or a subdirectory of a larger project) as long as
its releases publish conforming assets and the release manifest in §3.

---

## 2. Release-asset naming

Publish platform binaries as GitHub release assets using:

```
orc-plugin_<stage-name>_<platform>[_abi<N>].<ext>
```

where `<platform>` is `linux` / `macos` / `windows`, `<ext>` is
`so` / `dylib` / `dll`, and the optional `_abi<N>` token records the **host ABI**
the binary was built against:

```
orc-plugin_deinterlace_linux_abi18.so
orc-plugin_deinterlace_macos_abi18.dylib
orc-plugin_deinterlace_windows_abi18.dll
```

The name is a validity convention (the host rejects downloads whose names do
not match it); which asset a host actually installs is decided by the release
manifest below, not by parsing names. Find the ABI number you built against
in the [SDK version history](plugin-sdk.md#version-history)
(`host_abi_version`).

The skeleton CI workflows produce and upload these artifacts automatically on
tagged releases.

---

## 3. Release manifest (required)

Every release **must** include an `orc-plugin-manifest.yaml` asset declaring
each binary's platform, host ABI, toolchain tag, and SHA-256 digest — a
release without one cannot be browsed, installed, or updated to:

```yaml
manifest_schema: 1
plugin_id: org.example.stage.deinterlace
plugin_version: 1.0.0
artifacts:
  - file: orc-plugin_deinterlace_linux_abi18.so
    platform: linux
    abi: 18
    toolchain_tag: gcc14/libstdc++
    sha256: 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
```

`file`, `platform`, and a non-zero `abi` are required per artifact;
`toolchain_tag` (the `ORC_SDK_TOOLCHAIN_TAG` your build produced) and `sha256`
are strongly recommended. List **one artifact per platform**: the host considers
only the first entry for its own platform. The host uses the declared values to refuse installs that are
guaranteed to fail at load, and to verify (and quarantine on mismatch) every
download and cache hit. Compute digests with `sha256sum <file>` (macOS:
`shasum -a 256 <file>`). See
[Release manifest](plugin-architecture.md#release-manifest) for the full
schema and matching rules.

> Plugin binaries are **not code-signed**. See
> [Distribution integrity](plugin-architecture.md#distribution-integrity) for
> exactly what the digest does and does not guarantee, and the roadmap toward a
> signed index.

---

## 4. Rebuild expectations on host ABI bumps

The plugin boundary is a **C++ ABI**, not a C ABI — vtables, `std::shared_ptr`,
`std::string`, and STL containers cross it. Matching version numbers are
necessary but not sufficient: the plugin must be built with the same compiler
family, C++ standard library, and build configuration as the host (encoded in
the descriptor's `toolchain_tag` and checked for exact equality at load).

Consequences for publishing:

- **When `host_abi_version` increases**, you must rebuild against the new SDK
  and publish a new release whose assets are tagged `_abi<new>` and whose
  manifest declares the new `abi`. The host refuses to load binaries built
  against an older ABI. Because the host resolves only the latest release, and
  only the first manifest artifact for its platform, a release serves one host
  ABI per platform.
- **When `plugin_api_version` increases**, update your stage implementation to
  the new contract and recompile.
- Neither increment happens without a corresponding Decode-Orc release and
  migration notes. Watch the
  [version history](plugin-sdk.md#version-history) and the
  [ABI impact decision table](plugin-sdk.md#abi-impact-decision-table).

Before tagging a release, run the SDK enforcement gates in standalone mode
against your repository (see
[Plugin Author Guide §6](plugin-author-guide.md#6-run-the-sdk-enforcement-gates-standalone)).

---

## 5. Distribute via a hand-written registry entry

Users can register your plugin directly by adding an entry to their plugin
registry YAML:

```yaml
- plugin_id:          com.example.deinterlace
  plugin_version:     "1.0.0"
  source_repo_url:    https://github.com/example/orc-plugin_deinterlace
  artifact_source:    github_release_asset
  release_asset_url:  https://github.com/example/orc-plugin_deinterlace/releases/download/v1.0.0/orc-plugin_deinterlace_linux_abi18.so
  release_tag:        v1.0.0
  release_asset_name: orc-plugin_deinterlace_linux_abi18.so
  target_platform:    linux
  required_host_abi:  18
  enabled:            true
  trust_state:        untrusted
  license_spdx:       GPL-3.0-or-later
  sha256:             9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
```

Set `required_host_abi` so an incompatible entry is reported (and neither
downloaded nor loaded) instead of failing late at `dlopen`. Hand-written entries
default to `trust_state: untrusted`; the host downloads and loads only after the
user grants trust (ticking **Enabled** in the Plugin Manager, or
`orc-cli plugins enable <id>` / `orc-cli plugins trust <id>`, each of which asks
for confirmation). Users who add your plugin by its releases URL instead
(`orc-cli plugins add --url <releases-url>`, or the Plugin Manager) get the
entry resolved from your release manifest and confirm trust as part of adding
it. See [Registry entry](plugin-sdk.md#registry-entry).

---

## 6. Distribute via the curated index (recommended)

The curated index is the list users browse under **Plugin Manager → Browse
Plugins…** and query with `orc-cli plugins search / info / install`. It lives in
[`orc-plugin-registry/`](https://github.com/decode-orc/decode-orc/tree/main/orc-plugin-registry){target="_blank"} and the host fetches it from
the default-branch head over HTTPS — **a merged pull request publishes
immediately**, with no Decode-Orc release and no registry tag.

The index curates **which plugins are offered, not their versions**: the host
resolves your repository's **latest published release** at browse and install
time, and offers each new release to installed users as an update. Publishing
a new release therefore requires **no** registry change — keep your latest
release working and correctly named, because it is what users get.

Submit an entry:

1. Fork the registry and append your plugin under `plugins:` in `index.yaml`,
   following [`registry_schema: 2`](https://github.com/decode-orc/decode-orc/blob/main/orc-plugin-registry/README.md){target="_blank"}:

   ```yaml
   - id: com.example.deinterlace
     display_name: Example Deinterlacer
     description: Motion-adaptive deinterlacing for PAL/NTSC
     maintainer: Example Author
     license_spdx: GPL-3.0-or-later
     source_repo_url: https://github.com/example/orc-plugin_deinterlace
     tags: [transform, video]
   ```

2. Make sure your **latest GitHub release** publishes one conforming binary
   per platform you support **and the `orc-plugin-manifest.yaml` from §3** —
   without the manifest the host refuses to browse, install, or update to the
   release. The host installs exactly what the manifest declares for the
   user's platform, and a user on an unsupported platform, ABI, or toolchain
   is told precisely why rather than downloading an incompatible binary. Tag
   releases `v<version>` so the host can report and compare versions.

3. Validate locally before opening the PR:

   ```bash
   # Offline: schema, license, repository URL
   python3 orc-plugin-registry/tools/validate_index.py orc-plugin-registry/index.yaml

   # Online as well: latest release publishes a conforming plugin asset
   python3 orc-plugin-registry/tools/validate_index.py --online orc-plugin-registry/index.yaml
   ```

4. Open the pull request. CI validates schema conformance, a valid SPDX
   `license_spdx`, a GitHub `source_repo_url`, and that your latest release
   publishes a conforming plugin asset. Only GPLv3-compatible licenses are
   accepted.

**A maintainer's merge is the curation decision** and publishes the entry —
it endorses your repository, so every release you publish afterwards is
offered to users automatically. Users then install with:

```console
$ orc-cli plugins search deinterlace
$ orc-cli plugins info com.example.deinterlace
$ orc-cli plugins install com.example.deinterlace   # asks to confirm trust
```

Installing asks the user to confirm trust up front (`[y/N]`, or `--yes` for
scripted use) — the same confirmation the GUI **Browse Plugins…** dialog shows —
and records an entry pointing at your latest release, trusted, with the
`sha256` and `abi` your manifest declares; declining records nothing. Users are
re-asked to confirm trust after every update, since each release is a new
binary.

---

## 7. Checklist

- [ ] Repository named `orc-plugin_<stage-name>` (recommended).
- [ ] Release assets named `orc-plugin_<stage>_<platform>_abi<N>.<ext>`.
- [ ] `orc-plugin-manifest.yaml` uploaded with the release, declaring every
      binary's `file`/`platform`/`abi` plus `toolchain_tag` and `sha256`.
- [ ] Latest release is the one users should get (the index resolves it at
      install time); releases tagged `v<version>`.
- [ ] Rebuilt (and manifest updated) for each host ABI bump.
- [ ] `instructions.md` shipped beside each binary and up to date.
- [ ] Enforcement gates pass in standalone mode.
- [ ] Curated-index PR validated locally.
