# AC3 RF Sink

Decodes AC3 RF (Dolby Digital) audio samples from the pipeline and writes the resulting AC3 frames to a standard `.ac3` file. Use this stage when your LaserDisc source carries AC3 RF surround sound data alongside the video signal.

## When to use

Add this sink when processing later North American NTSC LaserDiscs that used AC3 RF encoding for 5.1 surround sound. Run it in parallel with the TBC Sink stage so you obtain both the video output and the extracted AC3 audio in a single pipeline trigger. The resulting `.ac3` file can be played back directly or muxed into a video container alongside the decoded video.

## What it does

The stage reads AC3 RF samples from the VideoFieldRepresentation, which are populated by the upstream source stage. It decodes the RF-modulated Dolby Digital bitstream frame by frame and writes the resulting AC3 audio frames sequentially to the output file. The output is a raw AC3 elementary stream with no container wrapping.

## Parameters

### output_path (string)
Path to the output AC3 file. Required. The conventional extension is `.ac3`. The file will be created or overwritten at trigger time.

`-` writes the stream to the CLI process's standard output instead of a file — CLI only; the GUI rejects this value.

## Notes

The upstream source stage must supply AC3 RF data — `has_ac3_rf()` must return true on the VideoFieldRepresentation. If no AC3 RF data is present, the pipeline will abort at trigger time. This stage is specific to AC3 RF as found on LaserDiscs; it does not handle AC3 carried in other formats or containers.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
