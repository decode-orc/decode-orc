#!/usr/bin/env python3
"""Write the still bench with its stills moved through the PAL sequence.

    rotate_stills.py <in.yaml> <out.yaml> <rotation> <old stem> <new stem>

pal_stills.yaml puts each of its twelve stills (sections 1-12, between the
Lead and Tail sections) on one frame, so each is decoded at only one place
in the 4-frame (8-field) PAL sequence: one carrier phase and V-switch
alignment. The output moves each still on by <rotation> frames (1-3), the
last ones wrapping to the front. Twelve is a multiple of four, so a still
moved by r sits r frames later in the sequence whether it wrapped or not.
Each section keeps its own blocks (noise seeds and all), so only its place
in the sequence changes. The output's video_path stem changes from
<old stem> to <new stem>. The input may be the noise-free copy the pinned
videosynth's generate_test_projects.py derives (still-truth-clean): its
sections are moved the same way.

docs-tech/domesday2d-decoder-investigation.md, Phase 8;
tools/domesday2d-bench/phase7_rotation.py is the bench's version.
"""
import sys

STILLS = 12  # sections 1-12; 0 (Lead) and 13 (Tail) stay


def main() -> int:
    source, target, rotation, old_stem, new_stem = sys.argv[1:6]
    rotation = int(rotation)
    if not 1 <= rotation < 4:
        print(f'rotate_stills: rotation {rotation} is not 1-3', file=sys.stderr)
        return 1
    with open(source, encoding='utf-8') as f:
        lines = f.read().split('\n')
    try:
        first = lines.index('sections:') + 1
    except ValueError:
        print(f'rotate_stills: {source} has no sections', file=sys.stderr)
        return 1
    # Each section runs from its "  - " line to the next one's; the lines
    # before the first stay with the header, and the comments that head a
    # section (its frame number) move with the section above them, which
    # is harmless in a derived copy.
    starts = [i for i in range(first, len(lines))
              if lines[i].startswith('  - ')]
    if len(starts) != STILLS + 2:
        print(f'rotate_stills: {source} has {len(starts)} sections, not '
              f'{STILLS + 2}', file=sys.stderr)
        return 1
    bounds = starts + [len(lines)]
    sections = [lines[bounds[i]:bounds[i + 1]] for i in range(len(starts))]
    stills = sections[1:1 + STILLS]
    moved = [None] * STILLS
    for i, section in enumerate(stills):
        moved[(i + rotation) % STILLS] = section
    out = lines[:starts[0]]
    out.append(f'  # Derived: the stills moved on by {rotation} frames '
               '(orc-tests/videosynth/rotate_stills.py).')
    for section in [sections[0]] + moved + [sections[-1]]:
        out.extend(section)
    text = '\n'.join(out)
    if old_stem not in text:
        print(f'rotate_stills: {source} does not name {old_stem}',
              file=sys.stderr)
        return 1
    with open(target, 'w', encoding='utf-8') as f:
        f.write(text.replace(old_stem, new_stem))
    return 0


if __name__ == '__main__':
    sys.exit(main())
