# FM Beat Cancel

Removes the FM-beat herringbone that some PAL LaserDisc masters carry: a faint diagonal pattern in flat saturated colour, strongest in reds and magentas. It was recorded into the master, so every capture of a pressing carries the same beat and stacking does not remove it. This stage predicts the beat from each field's own signal, fits its strength line by line, and subtracts it. Its output is composite, like its input.

## When to use

Use it on PAL LaserDisc sources whose bars, menus or captions show a herringbone in flat colour. Place one on each source, **before** the Stacker: the beats of different masters have independent phase, so once captures of several masters are blended, one fit removes much less. Run the **FM Beat Calibration** tool on each node first: it measures the master's carrier and sets `carrier_black_mhz`.

## What it does

The beat is a tone at the difference between the FM video carrier and the colour subcarrier, so its frequency follows the colour's brightness (about 2.7 MHz on blue to 3.2 MHz on yellow) and its strength follows the colour's saturation. Its phase is set by the carrier's own phase, which is the running integral of the carrier frequency, and the carrier frequency is set by the pre-emphasised composite. The stage therefore builds the beat's waveform from the field: the carrier from the signal level (`carrier_black_mhz` at black plus 8 kHz per IRE, after the 400/100 ns pre-emphasis the disc was cut with), its phase integrated along the field, mixed with the field's own chroma, de-emphasised and band-passed to 2.2–3.9 MHz.

One complex amplitude per line is then fitted to the field in that band, over flat parts of the picture only: the fit's weight falls away at luma and colour edges, and where the band holds far more energy than any beat (gratings, fine texture). The beat's phase steps by a near-constant amount from line to line and wanders slowly; the step is tracked over 9 lines and the amplitude fitted over 15. A line is corrected only where its amplitude is well above its standard error (4 times, fully from 8) and below the largest a disc carries. The fitted beat is subtracted across the line's active picture, except where the band's energy is well above any beat.

A field with too little flat saturated colour (fewer than 1500 samples of chroma above about 10 IRE where the luma is flat) is not fitted, and a frame neither of whose fields is corrected passes **bit-identical**. Blanking, sync, the colour burst and the VBI lines are never changed. Dropout hints pass through.

Only PAL composite sources are processed; any other source passes through unchanged with a warning.

How well it works, measured:

- On a videosynth pressing whose master carries a −24 dB beat at 7.085 MHz, with the Domesday capture noise, the stage at that carrier removes 23 dB of the beat on bars and on a flat red field. At the nominal 7.1 MHz, 15 kHz off, it removes under 2 dB. On the same pictures without a beat it changes nothing on bars, red or tartan, and Test Card F by 0.006 IRE RMS.
- On single captures of the Community South Domesday side, the beat on the bars (the part two captures of one master share, less what captures of different masters share) comes down by 11.5 dB on one master and 12.3 dB on another; on the flat field below the bars by 11.7 and 12.6 dB, and on the narrow bars themselves by 5–10 dB, where the noise the captures of one stamper share limits what can be measured.
- What it removes is the same on two captures of one master (correlation 0.95–0.98 on bars and maps, 0.84–0.95 on photographs) and unrelated between masters (0.00). The picture is the same on every master, so none of what it removes is picture: it is the master's beat. Most map pictures are corrected too, their seas being flat colour (0.6 IRE RMS removed); photographs carry less (0.2 IRE).

## Parameters

### carrier_black_mhz (double)
The master's FM video carrier at black (blanking), in MHz. The beat's phase along a line follows this closely: 10 kHz off turns it by about 3 radians across the picture, which leaves most of the beat in place. Set it with the FM Beat Calibration tool. Range: 6.9–7.3. Default: `7.1`, the PAL LaserDisc standard's. The Community South captures measure 7.088–7.106 MHz, within 1–3 kHz of one another on captures of one master.

## Tools

### FM Beat Calibration
Finds the master's carrier: it reads 300 frames spread over the stage's input, takes the 24 fields with the most flat saturated colour, and searches the carrier from 6.9 to 7.3 MHz (2 kHz steps, then 0.25 kHz around the best) for the value at which the stage removes the most beat from them. **Apply to Stage** sets `carrier_black_mhz`. A source with no flat colour, or whose best carrier corrects no line, is reported as having no beat to remove; the stage can then be left out. It takes under a minute on one capture.

## Status Indicator

The coloured dot in the top-right corner of the node shows its configuration status.

| Colour | Meaning |
|--------|---------|
| Green | Fully configured and ready to run. All required parameters are set. |
| Yellow | Partially configured. The stage can run but will use default or reduced behaviour — for example, pass-through mode or console-only output. Review the parameters for optional settings. |
| Red | Not configured. One or more required parameters are missing and the stage cannot run. |

Parameters can be set via **Edit Parameters...** in the node context menu. Some stages also provide interactive stage tools (listed under **Tools** above) that set parameters directly from within the tool.
