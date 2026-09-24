# Plugin Architecture

Decode-Orc uses a **plugin-first stage system** where every processing stage —
including the stages shipped with Decode-Orc itself — is loaded at runtime through
the same plugin framework. There is no privileged path for built-in stages; all
stages are self-contained shared libraries that are discovered, loaded, and
registered through a common host runtime.

## Core Goals

- **Uniform loading:** Decode-Orc-supplied stages and third-party stages follow
  identical contracts, loader paths, and registry entries.
- **Independent development:** A third-party plugin can be developed, built, and
  distributed without any changes to the Decode-Orc source repository.
- **Stable binary interface:** Explicit ABI and API version numbers govern
  compatibility, and the host refuses to load mismatched plugins with a clear
  diagnostic.
- **Registry-based distribution:** Plugins are declared in a persistent YAML
  registry and can be fetched automatically from GitHub release assets at
  startup. Non-core registry entries must be marked trusted before they are
  downloaded or loaded — both front ends grant trust as part of the action
  that lets a binary run (adding, installing, updating, enabling), each time
  behind the same explicit confirmation, while entries that arrive from
  outside the application default to untrusted — and
  downloaded artifacts are verified against a recorded SHA-256 checksum (see
  [Distribution integrity](#distribution-integrity)). Plugin binaries are
  **not** code-signed.

## Runtime Flow

```
Host startup
   │
  ├─ Read user plugin registry YAML
  │      ($XDG_CONFIG_HOME/decode-orc/stage-plugins.yaml or
  │       ~/.config/decode-orc/stage-plugins.yaml)
  │
  ├─ Collect default plugin search paths
  │      (development build dir or executable-relative install dir)
   │
   ├─ For each registry entry
   │      ├─ Check trust_state (untrusted non-core entries are skipped
   │      │      with a warning diagnostic; nothing is downloaded or loaded)
   │      ├─ Resolve local path (download from GitHub releases if absent;
   │      │      verify sha256 checksum, quarantining mismatches)
   │      ├─ dlopen / LoadLibrary the shared library
   │      ├─ Resolve entrypoints:
   │      │      orc_get_stage_plugin_descriptor
   │      │      orc_register_stage_plugin
   │      ├─ Validate host_abi_version, plugin_api_version, toolchain_tag
   │      └─ Call orc_register_stage_plugin → StageRegistry::register_stage
   │
  ├─ Merge registry paths, default search paths, and ORC_STAGE_PLUGIN_PATHS
  │
  └─ Consumer flows (GUI / CLI) query the same StageRegistry via presenters
```

Loading happens once at startup. Hot-reload is not supported in the current
version.

Plugins are loaded with `RTLD_LOCAL` (each plugin's symbols stay private to
that plugin), and plugin libraries are reference-counted: every registered
stage factory and every live stage instance holds a keep-alive reference, so
a plugin's code is never unmapped while one of its stages can still run.

`orc-gui` and `orc-cli` share the same persistent registry file. The difference
between development and packaged installs is the default plugin search path, not
a separate per-application registry.

### Front-end parity

Every plugin and stage action is available from both front ends, in the same
words, and both derive an entry's state from one presenter-computed value
rather than re-deriving it. The capability set is recorded in
[`orc/plugin_ux_capabilities.yaml`](https://github.com/decode-orc/decode-orc/blob/main/orc/plugin_ux_capabilities.yaml){target="_blank"},
which names the command and the control for each capability; the
`CLI.PluginUxCapabilityParity` CTest (label `unit;cli`) fails when a recorded
command disappears from `orc-cli plugins --help` / `orc-cli stages --help`, or
when a subcommand exists that the manifest does not record. Add a capability to
either front end and the manifest is where both sides are declared.

Three asymmetries are intended and recorded as such:

- **`plugins untrust` has no GUI control.** The GUI has no standalone trust
  control to withdraw from — adding, installing and ticking **Enabled** are its
  trust-granting actions — so untrusting stays a CLI primitive.
- **`plugins update --all` has no GUI control.** The Plugin Manager's
  **Update** button acts on the selected row.
- **The restart prompt is GUI-only.** Only a running GUI has a session to
  restart; the CLI prints the same registry-change note instead.

The `--json` and paste-ready (`--yaml`, `--filtergraph`) output modes are
likewise CLI-only: they are scripting projections of the same presenter
structures the GUI renders as widgets.

## Plugin Binary Format

Plugins are native shared libraries:

| Platform | Extension |
|----------|-----------|
| Linux    | `.so`     |
| macOS    | `.dylib`  |
| Windows  | `.dll`    |

Each plugin must export two C-linkage entrypoints (C++ types cross the
boundary — see Binary Compatibility Model below):

```cpp
// Returns the plugin descriptor (ABI version, API version, toolchain tag,
// metadata).
// All descriptor pointer fields must reference static storage.
const orc::StagePluginDescriptor* orc_get_stage_plugin_descriptor();

// Receives the host service table and registers one or more stage types
// with the host by invoking register_stage once per exported stage.
// Returns true when every stage registered successfully.
bool orc_register_stage_plugin(
    const orc::OrcPluginServices* services, void* context,
    bool (*register_stage)(void* context, const char* stage_name,
                           orc::OrcStageFactoryFn factory),
    const char** error_message);
```

Both symbols are declared with `ORC_STAGE_PLUGIN_EXPORT` (C linkage, default
visibility). Stage factories match `orc::OrcStageFactoryFn` and return
`std::shared_ptr<orc::DAGStage>`.

## Binary Compatibility Model

The plugin boundary is a **C++ ABI**, not a C ABI. `std::shared_ptr`,
`std::string`, STL containers, exceptions, and virtual-function tables all
cross the host/plugin boundary. Matching version numbers are therefore
necessary but not sufficient: a plugin must be built with the same compiler
family, the same C++ standard library, and a compatible build configuration
as the host. On Windows this includes the CRT flavour — a Debug-CRT plugin
cannot be loaded by a Release-CRT host.

Since ABI v5 the toolchain requirement is enforced at load time: the
descriptor's `toolchain_tag` field (populated by the `ORC_SDK_TOOLCHAIN_TAG`
macro) encodes the compiler family and major version, the C++ standard
library, and — on Windows — the CRT flavour, e.g. `gcc14/libstdc++`,
`clang17/libc++`, `msvc19/msvc-stl/release-crt`. The host requires the
plugin's tag to equal its own tag exactly and rejects the plugin with a
diagnostic naming both tags otherwise.

The host requires **exact equality** for both `host_abi_version` and
`plugin_api_version`; a mismatch in either causes the plugin to be rejected
with a logged diagnostic. The `services_size` field in `OrcPluginServices`
is an intra-version safety net only: it guards access to service-table
fields appended within the current ABI version. It is not a cross-version
compatibility mechanism.

## Compatibility Gating

Two version numbers plus the toolchain tag govern compatibility. All three
are checked before a plugin is accepted; a mismatch causes the plugin to be
skipped with a logged diagnostic. For guidance on which changes force a
`host_abi_version` bump, see the
[ABI impact decision table](plugin-sdk.md#abi-impact-decision-table).

### `host_abi_version`

Controls the binary ABI: the layout of `StagePluginDescriptor`, the entrypoint
signatures, and the `register_stage` callback contract.

**Current value:** `18` (`<orc/stage/video_frame_representation.h>`:
`VideoFrameRepresentation` gains four appended virtuals —
`is_exhausted()`, `stream_error()`, `has_unbounded_frame_range()` and
`max_concurrent_frame_requests()` — so a sink can consume a sequential,
possibly open-ended source such as a stdin-fed stream source; the appended
virtuals change the vtable layout).
The authoritative per-version change log is `orc/sdk/abi_history.yaml`, rendered as
the version-history table in [plugin-sdk.md](plugin-sdk.md#version-history).

Bumped when any of the following change:
- `StagePluginDescriptor` field order or alignment
- Entrypoint function signatures
- Callback calling convention
- `OrcPluginServices` gains or loses a field
- `IStageServices` gains or loses methods
- A public SDK contract header or class is removed
- The vtable layout of a contract type crossing the boundary (e.g.
  `VideoFrameRepresentation`) changes

### `plugin_api_version`

Controls the stage contract: the `DAGStage` virtual interface,
`ParameterizedStage`, `TriggerableStage`, `ArtifactPtr`, `ObservationContext`,
and `NodeTypeInfo` semantics.

**Current value:** `3`

Bumped when any of the following change:
- A `DAGStage` virtual method is added, removed, or reordered
- `ParameterValue` variant types change
- `NodeTypeInfo` struct layout changes
- `execute()` or `trigger()` lifecycle semantics change incompatibly
- The primary frame-data type changes (e.g. field → frame representation)

### `toolchain_tag`

Identifies the build environment the plugin binary was produced with (ABI
v5+). Compared as an exact string against the host's own tag; see the
Binary Compatibility Model above for the encoding.

## Plugin Registry

The registry is a YAML file that tracks the installed plugin set. The current
runtime stores it at:

- Linux: `$XDG_CONFIG_HOME/decode-orc/stage-plugins.yaml` or `~/.config/decode-orc/stage-plugins.yaml`
- macOS: `~/.config/decode-orc/stage-plugins.yaml` unless `XDG_CONFIG_HOME` is set
- Windows: `%APPDATA%/decode-orc/stage-plugins.yaml`

If none of those platform defaults are available, the host falls back to
`.decode-orc/stage-plugins.yaml` under the current working directory.

Both `orc-gui` and `orc-cli` read and write this same registry file.

Each entry records:

| Field | Description |
|-------|-------------|
| `plugin_id` | Unique string identifier |
| `plugin_version` | Plugin release version |
| `path` | Resolved local path to the plugin binary |
| `source_repo_url` | Repository URL for the plugin source or release |
| `artifact_source` | `local_path` or `github_release_asset` |
| `release_asset_url` | Direct URL to the GitHub release asset |
| `release_tag` | Release tag associated with the asset |
| `release_asset_name` | Expected artifact filename |
| `target_platform` | Optional platform hint for cache selection |
| `local_dev_path` | Optional development override used before remote download |
| `enabled` | Whether the plugin is loaded at startup |
| `trust_state` | Trust level, enforced before loading: entries other than `trusted` are neither downloaded nor `dlopen`ed unless `is_core_plugin` is set. Trust is always granted through an explicit warning ("plugins execute code locally on your computer") that defaults to refusing. Both front ends grant it the same way: the action that lets a binary run — `add`, `install`, `update`, `enable` — shows that warning up front, and confirming records the entry trusted while declining records nothing. The GUI asks with a dialog (OK/Cancel, Cancel default); the CLI asks on the terminal (`[y/N]`), takes `--yes` for scripted use, and refuses rather than blocking when stdin is not a terminal, exiting `4`. Untrusted entries still occur (updates reset trust, and hand-edited registry files default to `untrusted`); the Plugin Manager shows them with an unticked **Enabled** box, and ticking it shows the same warning before recording trust — the GUI has no separate trust control, by design. `orc-cli plugins trust <selector>` / `untrust <selector>` remain as explicit scripting primitives, `trust` asking the same question; withdrawing trust grants nothing, so `untrust` never asks |
| `license_spdx` | SPDX license identifier |
| `is_core_plugin` | Marks entries supplied by Decode-Orc itself; implicitly trusted |
| `required_host_abi` | Host ABI the plugin was built for. Enforced before download and load: a non-zero value that does not equal the host's `host_abi_version` means the entry is neither downloaded nor `dlopen`ed — it stays visible with a "needs a rebuild for Orc ABI N" message in `orc-cli plugins list` and the GUI Plugin Manager. `0` means unspecified (not gated); `is_core_plugin` entries are exempt |
| `sha256` | Optional SHA-256 digest (64 hex chars) of the plugin binary for `github_release_asset` entries; verified after download and on cache hits |

Entries with `artifact_source: github_release_asset` and an absent or empty
`path` are resolved automatically: the host downloads the binary from the
declared GitHub release and caches it to
`~/.config/decode-orc/plugin-cache/<platform>/` before loading. The download
only happens for trusted entries. When the entry records a `sha256`, the
digest is checked both after a fresh download and on every cache hit; a
mismatching file is quarantined (renamed with a `.quarantined` suffix) and
reported as an error, and a mismatching cache hit triggers one fresh
download attempt. When no `sha256` is recorded, the host loads the artifact
but emits a warning that its integrity could not be verified.

### Update checks and updating

For every non-core registry entry with a GitHub `source_repo_url`, the host
can query the repository's **latest published release**
(`api.github.com/repos/<owner>/<repo>/releases/latest`) and compare its tag
against the installed `plugin_version` (falling back to `release_tag`). This
means a newer plugin release is reported without the curated index needing an
update. The per-plugin outcome is one of:

- **Up to date** — installed version ≥ latest release tag.
- **Update available (x.y.z)** — a newer release is published upstream.
- **Unreachable** — the release information could not be fetched (offline,
  rate-limited, repository removed, or no releases published).
- **Unknown** — the latest release is known but no installed version is
  recorded to compare against.
- Entries without a GitHub repository URL (e.g. local plugins) are not
  checked.

The GUI Plugin Manager runs the check on a worker thread when it opens and
shows the outcome in its **Update** column; `orc-cli plugins updates` and
`orc-cli plugins list --check-updates` print the same information in the same
words. One API request is made per distinct repository per sweep. No other
listing path goes to the network: plain `plugins list` and the Plugin
Manager's table are registry-only until the check reports.

Updating (the Plugin Manager's **Update** button, or
`orc-cli plugins update <selector>`, or `orc-cli plugins update --all` for
every entry reporting an update) re-resolves the release asset from the
entry's `source_repo_url` — the same resolution as adding by URL and
installing from the index — and rewrites the registry entry to point at the
latest release. Any recorded `sha256` is cleared (release artifacts carry no
reviewed digest), the cached `path` is dropped so the next launch downloads
the new asset, and the entry's recorded trust is reset: a new binary is a new
decision, so both front ends re-confirm trust as part of the update — the GUI
with its post-update prompt, the CLI with the same confirmation it applies to
`add` and `install` (once for the whole batch under `--all`). Declining leaves
the entry untrusted, and it is neither downloaded nor loaded.

## Curated plugin index

Alongside manual URL entry, the host offers a **curated index** of third-party
plugins for discovery and one-click install. The index is a versioned YAML
document read from a configurable URL (default: the `orc-plugin-registry/`
`index.yaml` on the Decode-Orc default branch; override with the
`ORC_PLUGIN_INDEX_URL` environment variable). Because the host reads the branch
head over plain HTTPS, a merged registry change publishes immediately — no host
release and no registry tag are required.

The host refreshes the index on demand — when the Plugin Manager's **Browse
Plugins…** dialog opens or a `orc-cli plugins search / info / install` command
runs — asynchronously, falling back to the last-good cached copy
(`<config>/plugin-index-cache.yaml`) when offline. `plugins info` also
describes plugins that are only in the local registry, and skips the index
entirely for a `path:`/`url:` selector, which the index never uses.

The index curates **which plugins are offered, not their versions**: once a
plugin is accepted, its current release and all subsequent releases are
available without further index changes. The host resolves each entry's
**latest published GitHub release** at browse and install time (the same
resolution used when adding a plugin by URL). Asset selection is driven
entirely by the mandatory [release manifest](#release-manifest)
(`orc-plugin-manifest.yaml`): the host picks the artifact the manifest
declares for this platform and pre-checks its declared ABI and toolchain tag
against its own. There is exactly one notion of compatible — the manifest
declares an artifact for this platform whose ABI and toolchain match the
host. A release without a manifest, with an invalid manifest, or whose
manifest declares a mismatch is **incompatible** and cannot be installed;
there is no name-based fallback and no "unknown compatibility" middle ground.
Entries whose release information cannot be fetched are shown as
**unreachable**.

Schema (`registry_schema: 2`):

| Field | Description |
|-------|-------------|
| `registry_schema` | Index schema **major** version. Hosts ignore unknown fields, so additions within a major are non-breaking; a newer major is parsed best-effort with a warning. Schema 1 pinned per-release `artifacts` lists; schema 2 removed them — a pin list in an older index is ignored |
| `plugins[].id` | Unique plugin identifier |
| `plugins[].display_name` | Human-readable name |
| `plugins[].description` | Short description |
| `plugins[].tags` | Search tags |
| `plugins[].maintainer` | Maintainer name |
| `plugins[].license_spdx` | SPDX license identifier (mandatory) |
| `plugins[].source_repo_url` | GitHub repository the host resolves releases from (mandatory) |

Installing from the index records a registry entry pointing at the resolved
latest release asset. Installing runs a binary, so both the browse dialog's
**Install…** and `orc-cli plugins install <id>` ask for trust confirmation
first and record the entry trusted; declining records nothing at all. The
manifest's per-artifact `sha256` and `abi` are recorded into the registry
entry, arming the existing download verify-and-quarantine path and the early
`required_host_abi` check. After install, the host keeps checking the
repository for newer releases — see
[Update checks and updating](#update-checks-and-updating); updating re-reads
the new release's manifest the same way.

### Release manifest

Every plugin release **must** upload a YAML asset named
`orc-plugin-manifest.yaml` alongside its binaries — a release without one
(or with an invalid one) is not installable:

```yaml
manifest_schema: 1
plugin_id: org.example.stage.demo
plugin_version: 1.2.3
artifacts:
  - file: orc-plugin_demo_linux.so
    platform: linux
    abi: 12
    toolchain_tag: gcc14/libstdc++
    sha256: 91ba329876d3df13772f051878ef7071721ed8f3e547ff6bfe6e4bd36c088c68
  - file: orc-plugin_demo_macos.dylib
    platform: macos
    abi: 12
    toolchain_tag: clang17/libc++
    sha256: 16dbdebc7ee615ac7d759972f38a8e92479f1a985dcc93ee355e7a36c32cf12c
```

Schema (`manifest_schema: 1`; unknown fields are ignored, a newer major is
parsed best-effort with a warning):

| Field | Required | Description |
|-------|----------|-------------|
| `manifest_schema` | yes | Manifest schema **major** version |
| `plugin_id` | recommended | Plugin identifier; should match the index entry and the binary's descriptor |
| `plugin_version` | no | Informational; the release tag remains the version of record |
| `artifacts[].file` | yes | Exact release asset filename |
| `artifacts[].platform` | yes | `linux`, `macos` or `windows` |
| `artifacts[].abi` | yes | `kStagePluginHostAbiVersion` the binary was built against |
| `artifacts[].toolchain_tag` | recommended | `ORC_SDK_TOOLCHAIN_TAG` of the build; without it only the ABI number can be pre-checked |
| `artifacts[].sha256` | recommended | Hex SHA-256 of the asset; recorded into the registry so downloads are verified and quarantined on mismatch |

Declared `file` entries must follow the
[artifact naming convention](#artifact-naming-convention) and must exist in
the release's asset list; either violation fails resolution.

The manifest is a **declaration by the release's CI, not proof**: the
load-time ABI/toolchain gate remains the enforcement point, and a manifest
that misdeclares its binaries fails there. What the manifest provides is a
definitive compatibility verdict *before* anything is downloaded (browse
shows "compatible — declared by the release manifest", or the precise
mismatch), hard refusal of installs and updates that are guaranteed to fail
at load, and a digest for the download integrity check. Because the manifest
travels with the release, the index stays unpinned while each release still
pins its own artifacts.

Contribution model: plugin authors open a pull request adding their entry;
repository CI validates every PR (schema conformance, a present SPDX license,
a GitHub `source_repo_url`, and — online — that the repository's latest
release publishes an asset following the plugin naming convention), and a
maintainer's merge is the curation decision. **The merge endorses the
repository, not a reviewed binary**: every future release the repository
publishes becomes installable immediately. See
[`orc-plugin-registry/README.md`](https://github.com/decode-orc/decode-orc/blob/main/orc-plugin-registry/README.md){target="_blank"}.

### Distribution integrity

What the host verifies before running plugin code, and what it does not:

**Verified:**

- `trust_state` — untrusted non-core registry entries are never downloaded
  or `dlopen`ed (`stage_plugin_registry.cpp`, `stage_registry.cpp`).
- `sha256` — downloaded artifacts (and cached copies) are checked against
  the registry digest when one is recorded (`plugin_remote_loader.cpp`).
- `host_abi_version` and `plugin_api_version` — exact-match at load time
  (`stage_plugin_loader.cpp`).
- `required_host_abi` — a non-zero registry value that does not match the
  host ABI blocks download and load early, before any bytes are fetched
  (`stage_plugin_registry.cpp`).
- `toolchain_tag` — exact-match against the host's compiler/stdlib/build
  configuration tag (ABI v5).

**What the curated index adds:**

- **Curated acceptance.** A human maintainer's merge admits a plugin
  repository to the list; the PR workflow verifies the repository publishes a
  release with a conforming plugin asset before the entry can go live.
- **No version pinning (by design).** The index does not pin release URLs or
  digests: installs and updates always resolve the repository's latest
  release, so a new upstream release is available with no index change. The
  flip side is that the maintainer's merge endorses the repository and its
  future releases, not one reviewed binary — a compromised plugin repository
  can publish a malicious release that is immediately installable. The
  per-user trust confirmation (required on first install and again after
  every update) is the remaining human checkpoint.

**Not verified (future work):**

- **Signed index.** The index itself is fetched over HTTPS but is not
  cryptographically signed, so its authenticity rests on transport security and
  repository access control. Signing the index (e.g. minisign/sigstore) is a
  documented follow-on.
- **Code signing.** Plugin binaries carry no cryptographic signature. A
  manifest-supplied (or hand-entered) `sha256` in the local registry protects
  download integrity, but the manifest is served from the same release as the
  binary, so it authenticates an artifact only as strongly as the repository
  that published both.
- **Unsigned local registry.** The on-disk registry
  (`stage-plugins.yaml`) and the cached index copy are plain files with no
  signature. An attacker who can write to the user's config directory could
  flip `trust_state` or point an entry at a malicious binary with a matching
  digest; the host trusts the local registry as much as the filesystem it lives
  on. Confining that directory's permissions is the user's responsibility until
  registry signing lands.

## Project-Level Plugin Metadata

Project files may also include a root-level `required_plugins` block. This is a
snapshot of the subset of third-party plugin registry metadata that is actually
required by the current project DAG.

Each `required_plugins` entry stores:

| Field | Description |
|-------|-------------|
| `plugin_id` | Plugin identifier expected by the project |
| `plugin_version` | Last known plugin version used when saved |
| `source_repo_url` | Repository URL for the plugin source or release |
| `artifact_source` | `local_path` or `github_release_asset` |
| `release_asset_url` | Direct release asset URL when known |
| `release_tag` | Release tag associated with the saved metadata |
| `release_asset_name` | Expected artifact filename |
| `target_platform` | Optional platform hint |
| `local_dev_path` | Optional development override path |
| `license_spdx` | SPDX license identifier |
| `is_core_plugin` | Whether the plugin is Decode-Orc supplied |
| `required_host_abi` | Host ABI version expected by the plugin |
| `stage_names` | Stage names from that plugin that are still referenced by the project |

The host rewrites this block on every save. It only keeps entries whose
`stage_names` are still referenced by the project, so stale third-party plugin
references are removed automatically when plugin-backed stages are deleted.

When a project references a missing runtime stage, the loader consults this
saved block to enrich the error message with the expected plugin id and, when
available, a repository or release URL.

In addition to registry entries, the host also searches the standard plugin
install/build locations relative to the executable:

- Linux install: `../lib/orc-stage-plugins`
- macOS install: `../PlugIns/orc-stage-plugins`
- Windows install: `orc-stage-plugins`
- Development builds: the compiled-in build plugin directory when the
  executable-relative install path does not exist

## Artifact Naming Convention

All plugin release artifacts follow a consistent naming scheme:

```
orc-plugin_<stage-name>_<platform>[_abi<N>].<ext>
```

The optional `_abi<N>` token records the host ABI version the binary targets,
so one release can carry builds for several host ABIs side by side. The name
is purely a validity/recognisability convention: artifact **selection** and
compatibility checking are driven entirely by the mandatory
[release manifest](#release-manifest), whose `file` entries must conform to
this scheme (the pattern is also enforced when a download is requested and
when a registry entry is parsed).

Examples:
- `orc-plugin_skeleton_passthrough_linux.so` (untagged)
- `orc-plugin_skeleton_passthrough_linux_abi12.so` (ABI-tagged)
- `orc-plugin_skeleton_passthrough_macos_abi12.dylib`
- `orc-plugin_skeleton_passthrough_windows_abi12.dll`

External plugin repository names follow the same prefix convention
(`orc-plugin_<name>`), both for official decode-orc organization repositories
and as the recommended standard for third-party authors.

## Stdio Piping Convention

A `FILE_PATH` parameter (`orc::ParameterType::FILE_PATH`) may be set to the
literal value `"-"` instead of a real path. This is a convention every stage
is free to opt into, not a new ABI contract:

- On an input-side parameter, `"-"` means the CLI process's own standard
  input. On an output-side parameter, its own standard output.
- Direction comes from the parameter's `ParameterDescriptor::output_path`
  flag when set, OR from the node being a `SINK`/`ANALYSIS_SINK` otherwise
  (`ProjectPresenter`'s `find_pipe_parameter_direction()`,
  `project_presenter.cpp`) — most sinks never set the flag explicitly and
  rely on the node-type fallback; only set it yourself for an output-only
  path on a non-sink stage (e.g. a report file written by a transform).

**CLI-only to execute; settable in the GUI.** A GUI process has no
meaningful stdin/stdout to redirect a stage's I/O to, so `"-"` can never
actually run there — but the officially supported workflow is to build a
project in the GUI and run it via `orc-cli ... --process`, so the GUI's
`FILE_PATH` parameter editor accepts and saves the value instead of
blocking that workflow at the editing step. What refuses it is execution:
every GUI-side code path capable of running a real stage instance —
`ProjectPresenter::getNodeConfigurationStatus()` (shows the node as
unconfigured rather than ready), `RenderPresenter::triggerStage()`,
`PreviewRenderer::ensure_node_executed()` (both automatic and
manually-requested preview), and the background observation pool's two
entry points (`RenderPresenter::sweepNodeForObservation()` and
`::scheduleObservationsForPreview()`) — checks
`orc::dag_subgraph_targets_pipe_or_network()`
(`<orc/core/include/project_to_dag.h>`, a backward walk over the node and
everything it transitively depends on, since executing a node also
executes its whole upstream chain) and refuses rather than let a stage
attempt real stdin/stdout I/O against the GUI process itself. A stage does
not need to guard against any of this itself: by the time its parameters
reach `execute()`/`trigger()`, either the CLI's own collision/reachability
check below has already passed, or the GUI call never happened.
`ProjectPresenter::triggerNode()`/`triggerAllSinks()` are the one
exception — shared with the CLI's own use of the same methods, they carry
a `SAFETY` docstring instead of a hard refusal; see their declarations
before wiring either to a GUI action.

**Collision and compatibility checks are the host's responsibility**, not
each stage's: the host is what knows a project's whole node graph, so it is
where "does more than one node claim stdin/stdout" and "can every node
between a piped source and a piped sink actually run in a single forward
pass" get validated before a run starts. A stage only has to (a) recognise
`"-"` on its own parameters and (b) declare, from its own current
configuration, whether it can honour it — by implementing the stage-tier
[`IStreamingCompatibility`](../../orc/sdk/include/orc/stage/streaming_capability.h)
interface alongside its other `DAGStage`-derived interfaces. Not
implementing it means "not streaming-safe"; the host walks every node
reachable from a pipe endpoint and refuses the run unless each one both
implements the interface and currently returns `true` from
`supports_streaming_execution()`.

These checks, and the GUI refusal above, apply to every non-seekable
stream: `"-"`, a network stream URL, or a named pipe (a POSIX FIFO on disk;
see `orc::is_stream_target()` in `project_to_dag.h`).

A piped or network input can be read only once, so by default it may feed
only one sink; the host refuses a project where more than one sink depends
on it. The exception is stdin read by a source that declares the reserved
`orc::kStreamReaderCountParameter` (`<orc/stage/params/parameter_types.h>`,
UINT32, default 1): the DAG builder sets it to the number of sinks
downstream, `triggerAllSinks()` runs those sinks side by side, each in its
own execution graph with its own instance of the source, and each instance
reads stdin through `orc::pipe_io::open_stdin_reader(count)`. That splits
stdin between the instances — read once, every instance gets the whole
stream, bounded buffering, pace of the slowest reader — like `tee`. A reader
that stops early must detach (destroying its stream does), so the others do
not wait on it.

A stream that stops on a read error rather than a clean end reports it
through `VideoFrameRepresentation::stream_error()`; a sink that stops on
`is_exhausted()` must check it and fail rather than report a truncated
output as a success.

The `support`-tier header
[`<orc/support/pipe_io.h>`](../../orc/sdk/include/orc/support/pipe_io.h)
gives a stage everything it needs for (b) without any host coordination:

- `orc::pipe_io::is_pipe_path(path)` — true for `"-"` or an actual POSIX
  named pipe already on disk (a real `mkfifo`, recognised so the same
  streaming-safe choices apply to it as to `"-"`; Windows named pipes are not
  detected and fall back to being treated like a regular path).
- `orc::pipe_io::to_libav_io_url(path, direction)` — translates `"-"` into
  libav's own `pipe:0`/`pipe:1` URL for a backend built on `avio_open()` /
  `avformat_alloc_output_context2()`. A literal `"-"` handed straight to
  libav would try to open a file actually named `-` in the working
  directory; `pipe:` is the portable way to mean stdin/stdout on both POSIX
  and Windows.
- `orc::pipe_io::BoundedPipeQueue<T>` — a bounded producer/consumer queue for
  a stage that encodes in one thread and writes in another, so a slow
  consumer on the other end of the pipe (e.g. `| ffplay -`) throttles the
  writer without stalling the encoder arbitrarily far ahead of it.
- `orc::pipe_io::open_stdin_reader(readers)` / `InputSplitter` — a reader of
  stdin for a source that may be one of several sharing it (see
  `kStreamReaderCountParameter` above); with one reader it is plain stdin.

A stage that reads a `VideoFrameRepresentation` and returns `true` from
`supports_streaming_execution()` must also check
`VideoFrameRepresentation::has_unbounded_frame_range()` before assuming
`frame_range()` is the input's real length (reserving a buffer sized from
it, running a pre-count pass, etc.) — a piped/live upstream source with no
declared `frame_count` reports a placeholder there. Detect the real end via
`is_exhausted()` instead, or refuse cleanly with a diagnostic if the stage
has no per-frame signal to detect it from. See
`streaming_capability.h`'s `supports_streaming_execution()` docstring and
`video_sink_stage.cpp`'s `run_streaming_export()` for the fullest example.

A sink using these still has to pick its own pipe-safe format: whichever
container needs to seek back and rewrite a header/index at the end (a plain
MP4, for instance) cannot be produced on a pipe at all, so `is_pipe_path()`
on the current `output_path` should steer format selection (or parameter
validation) accordingly, the same way it would for any other
configuration-dependent constraint.

### Network stream URLs

A live network destination — `udp://`, `rtmp(s)://`, `rtp://`, `srt://`,
`tcp://` — is exactly as non-seekable as a `"-"` pipe, and gets the same
treatment throughout: `orc::pipe_io::is_network_stream_url(path)` recognises
one, `to_libav_io_url()` passes it straight through unchanged (libav's own
protocol handlers already understand these URL schemes directly), and the
host's collision/reachability check (see below) treats a stage targeting one
the same way it treats a stage targeting `"-"` for the
`IStreamingCompatibility` question — and, like `"-"`, it is accepted by the
GUI's `FILE_PATH` parameter editor (same officially-supported
build-in-the-GUI-run-via-the-CLI workflow) and refused instead at every
GUI-side execution path via the same `dag_subgraph_targets_pipe_or_network()`
check described above.

One thing does NOT carry over: **collision detection is scoped to `"-"`
alone.** `"-"` names one real, OS-level singleton stream — the whole
process has exactly one stdin and one stdout — so two nodes both targeting
`"-"` on the same side really are fighting over the same destination. Two
nodes each targeting their *own* distinct network URL are not; there is no
process-wide singleton to collide over, so the host only ever flags
`"More than one node targets standard input/output"` for genuine `"-"`
duplicates, never for two different network URLs (or a `"-"` and a network
URL) coexisting in the same project.

Only a libav-backed backend (one using `to_libav_io_url()`) can actually
open a network URL — an iostream-based backend (`raw_output_backend`,
`cvbs_stream_source`'s stdin reader) has no way to write or read a network
socket and keeps checking for the literal `"-"` token via `is_pipe_path()`
only, exactly as before.

## Stage Services

Plugins interact with the host through explicit service interfaces rather than
direct calls into host internals. The host builds an `OrcPluginServices` table
(declared in `<orc/plugin/orc_plugin_services.h>`) and passes it as the first
argument to `orc_register_stage_plugin()`. The table provides:

- `log` — pre-formatted message logging routed to the host logger (plugins use
  the `ORC_PLUGIN_LOG_*` macros)
- `render_colour_preview` — converts a decoded `ColourFrameCarrier` to a
  display-ready `PreviewImage`
- `stage_services` — optional pointer to the consolidated `IStageServices`
  interface (may be `nullptr` when the capability is unavailable)
- `observation_service` — optional pointer to `IObservationService` (appended
  in ABI 9, guarded by `services_size`); runs the standard observers by stable
  string id so plugins no longer link the concrete observer classes. `nullptr`
  on any host older than ABI 9. Obtained via
  `orc::plugin::get_observation_service()`. See the
  [Plugin SDK Developer Guide](plugin-sdk.md#observation-service-abi-9)

The `IStageServices` contract (declared in `<orc/plugin/orc_stage_services.h>`)
currently exposes buffered file-output factories used by sink stages:
`create_buffered_file_writer_uint8()`, `create_buffered_file_writer_uint16()`,
and `create_buffered_file_writer_int16()`. It does not currently provide
artifact delivery, logging, or progress reporting.

Plugins store the table with `orc::plugin::set_services()` at the start of
`orc_register_stage_plugin`, and obtain the `IStageServices` pointer later via
`orc::plugin::get_stage_services()` (which returns `nullptr` when the host does
not provide it).

## Stage Tools

Stages that expose interactive tooling (custom editors, analysis views) publish
optional `StageToolDescriptor` records through the `StageToolProvider` mixin
(declared in `<orc/plugin/orc_stage_tooling.h>`). The host discovers and routes
these descriptors through the presenter layer — no hardcoded stage-name branches
exist in host tool dispatch.

Analysis tools (dropout analysis, SNR analysis, burst-level analysis) follow the
same pattern via `AnalysisToolDescriptor` and `AnalysisToolProvider`.

## SDK Boundary Enforcement

The source-level plugin boundary is an **allowlist** of SDK contract
headers: the `<orc/plugin/...>` family (ABI, entrypoints, services) and the
`<orc/stage/...>` family (stage, frame, observation, preview, and utility
contracts). The complete permitted set is listed in the
[Plugin SDK Developer Guide](plugin-sdk.md) (SDK Headers section); anything
else in the host tree is private.

Enforcement happens at two levels:

1. **Compile time** — the `orc-plugin-sdk` / `orc::plugin-sdk` target
   propagates only the SDK include tree, the spdlog/fmt usage requirements the
   SDK headers themselves need, and the `orc-sdk-support` static library
   (support-tier helper symbols). The host libraries are **not** linked — the
   former `$<LINK_ONLY:orc-core>` tether was removed so plugins compile and link
   against the SDK alone. Private host include directories and third-party
   dependencies are therefore invisible to plugin translation units; including a
   private host header fails the plugin's compile. Third-party libraries a
   plugin uses directly must be declared by the plugin's own CMake target.
2. **Scan gates** — two hard-fail CI gates (`ctest -L sdk`) run on the
   in-tree plugin tree `orc/plugins/stages/`:
   `check_plugin_private_includes.sh` fails on any include that is not an
   allowlisted SDK header, a plugin-local header, a standard-library or
   platform header, or a permitted third-party header;
   `check_plugin_private_links.sh` fails on plugin build files that link
   private host targets directly. Third-party authors run the same scripts
   in standalone mode against their own repository (see
   [Publishing a plugin](../technical/plugin-publishing.md)).

## Third-Party Plugin Repositories

An official skeleton template lives at
[decode-orc/orc-plugin_skeleton](https://github.com/decode-orc/orc-plugin_skeleton).
It provides:

- Minimal buildable plugin scaffold (CMake + SDK-only includes + sample stage)
- Unit tests
- Linux / macOS / Windows CI workflows
- Packaging conventions
- SPDX / licensing guidance

Use it as the starting point for any new out-of-tree plugin. For a
step-by-step walkthrough from an empty directory to a loaded plugin, see the
[Plugin Author Guide](../technical/plugin-author-guide.md); for release and
registry submission, see the
[Plugin Publishing Guide](../technical/plugin-publishing.md).
