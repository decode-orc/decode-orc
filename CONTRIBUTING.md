# Contributing

Thanks for your interest in improving Decode-Orc.

## How to contribute

- Report bugs and request features via GitHub Issues.
- Submit focused pull requests with clear descriptions.
- Keep changes small and aligned with project architecture.

## Before opening a pull request

- Search existing issues and pull requests first.
- Build the project locally and verify your change.
- Update documentation when behavior or usage changes.

## SDK-Only Plugin Development

All stage plugins (core-supplied and third-party) must use only the public plugin SDK contract. If you are modifying or adding a stage plugin:

- Use only public SDK headers from `orc/sdk/include/orc/plugin/` — no private host headers from `orc/core`, `orc/gui`, `orc/cli`, or `orc/presenters`
- Do not link plugin targets against private host internals (`orc-core`, `orc-presenters`, `orc-gui`, etc.)
- Access host services through `orc::plugin::get_stage_services()`, not direct factory APIs
- Verify SDK enforcement passes locally before opening a PR:
  ```bash
  cmake --build build --parallel
  ctest --test-dir build -L sdk --output-on-failure
  ```
- Verify runtime plugin loading if adding or modifying stages:
  ```bash
  ctest --test-dir build -R "StagePluginLoader" --output-on-failure
  ```

For new plugins, use the canonical skeleton template as a reference: https://github.com/decode-orc/orc-plugin_skeleton

For third-party plugins, the plugin must build against the installed SDK only (no in-tree private headers). Shared helper dependencies must be either packaged or vendored locally.

## Stages in separate plugin repositories

The teletext and NABTS sinks are not in this repository: they are in the
teletext plugin, [orc-plugin_teletext](https://github.com/decode-orc/orc-plugin_teletext), which
users install with the Plugin Manager. Changes to those stages go to that
repository.

A change to the SDK that a plugin uses must keep it building. Build a checkout
of it in with `-DORC_PLUGIN_SOURCE_DIRS=<path>` and run its tests
(`ctest --test-dir build/plugin-source-trees/<name>`) before pushing; see
[BUILD.md](BUILD.md#building-a-plugin-checkout-in-tree). The nightly workflow
builds `main` against each official plugin's `main` as a backstop.

## Development notes

- Build system: CMake
- Main components: `orc-core`, `orc-cli`, `orc-gui`
- Architecture: MVP where possible
- Plugin architecture: SDK-only

## Pull request checklist

- [ ] The change solves one clear problem
- [ ] Code builds successfully
- [ ] Documentation updated if needed
- [ ] Commit messages are clear
- [ ] **For stage/plugin changes**: SDK-only compliance checklist completed (see SDK-Only Plugin Development section above)
- [ ] **For stage/plugin changes**: SDK enforcement checks pass: `ctest --test-dir build -L sdk` ✓

## Questions

If you are unsure about direction or scope, open an issue to discuss first.