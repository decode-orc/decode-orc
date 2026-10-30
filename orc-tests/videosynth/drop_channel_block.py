#!/usr/bin/env python3
"""Write a videosynth project without its top-level `channel:` block.

    drop_channel_block.py <in.yaml> <out.yaml> <old stem> <new stem>

The pinned videosynth's generate_test_projects.py removes blocks from
sections only, so the truth of a case whose pressing carries a channel
effect (the FM-beat herringbone) is derived here: every line of the
`channel:` block goes, and the output's video_path stem changes from
<old stem> to <new stem>.
"""
import sys


def main() -> int:
    source, target, old_stem, new_stem = sys.argv[1:5]
    with open(source, encoding='utf-8') as f:
        lines = f.read().split('\n')
    kept = []
    in_channel = False
    for line in lines:
        top_level = line[:1] not in ('', ' ', '\t', '#')
        if top_level:
            in_channel = line.startswith('channel:')
        if not in_channel:
            kept.append(line)
    text = '\n'.join(kept)
    if old_stem not in text:
        print(f'drop_channel_block: {source} does not name {old_stem}',
              file=sys.stderr)
        return 1
    with open(target, 'w', encoding='utf-8') as f:
        f.write(text.replace(old_stem, new_stem))
    return 0


if __name__ == '__main__':
    sys.exit(main())
