# Overview

A *stage* is a discrete processing unit with defined inputs and outputs. Stages are connected together to form a directed processing graph, where data flows along connections between stage outputs and stage inputs.  There are 4 stage types in decode-orc:

- [Source stages](source-stages.md) are the **starting point of every decode-orc pipeline**. They load captured video (and any associated audio or data) from disk, or from a pipe when run from the CLI, and make it available for further processing.
- [Transform stages](transform-stages.md) are where **most of the real work in decode-orc happens**. They take video from a source stage (or another transform stage) and modify, combine, or reinterpret it to improve quality or prepare it for output.
- [Media sink stages](sink-core-stages.md) terminate a pipeline by writing decoded or processed media and data to disk (or to a pipe when run from the CLI). The [Chroma decoder guide](chroma-decoder-guide.md) covers the Video Sink's decoder settings in depth.
- [Analysis sink stages](sink-analysis-stages.md) compute diagnostics and measurements, display them in a dialog and write the results to a file. They do not produce media streams.

![](assets/main-window.png)

Right-clicking an empty area of the graph opens the Add Stage menu, which groups stages by these types:

* Source - Stages that load captured video (and any associated audio or data)
* Transform - Stages that modify, combine, or reinterpret video
* Analysis - Analysis sinks that compute statistics and display charts
* Sink - The media/data output sinks (TBC, CVBS, encoded video, audio, EFM, raw EFM, AC3 RF, closed captions, teletext, NABTS, Daphne VBI)

Only stages compatible with the project's video system are listed, and source stages are further filtered by the project's source type (composite or Y/C). Within each group, stages are sorted by name.

Stages provided by externally installed plugins appear under a separate **Plugins** entry, organised into the same four groups. Empty groups are hidden.

Each stage's own help (right-click the node, then **Help...**, or `orc-cli stages help <stage>`) describes its parameters and tools; the pages linked above repeat that information for every stage that ships with decode-orc.
