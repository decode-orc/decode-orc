# CLI template projects

Template `.orcprj` files used by the `orc-cli` functional checks
(`cmake/check_cli_nodes.sh`, `cmake/check_cli_tools.sh`), by the `--process`
tests in `orc/cli/CMakeLists.txt`, and by the non-ASCII path smoke test in
`.github/workflows/package-windows.yml`.

Each check copies a template into its own temporary directory and repoints it
with `orc-cli nodes set`. File paths in the templates are relative, so they
resolve against wherever the copy is written.

| File | Graph |
| --- | --- |
| `tbc_frame_map_video.orcprj` | TBC Source → Frame Map → Video Sink (PAL composite, source unconfigured) |
| `cvbs_video_params_video.orcprj` | CVBS Source (`blank.cvbs`, `CVBS_U10_4FSC`) → Video Parameters (black 300) → Video Parameters 2 → Video Sink |
| `vbi_video.orcprj` | VBI Capture Source (`capture.vbi`, bt8x8 WST) → Video Sink (`output.mkv`) |
| `no_sinks.orcprj` | VBI Capture Source alone: loads, but `--process` has nothing to trigger |
| `legacy_teletext.orcprj` | VBI Capture Source → Teletext Sink under its pre-plugin name `teletext_analysis_sink`, with no `required_plugins` entry: loads, but `--process` names the teletext plugin to install |
