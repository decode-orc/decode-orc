#!/usr/bin/env python3
"""Write a videosynth project without its `laserdisc.channel` block.

    drop_channel_block.py <in.yaml> <out.yaml> <old stem> <new stem>

The pinned videosynth's generate_test_projects.py removes blocks from
sections only, so the truth of a case whose pressing carries a channel
effect (the FM-beat herringbone) is derived here: every line of the
`channel:` block inside the top-level `laserdisc:` block goes, and the
output's video_path stem changes from <old stem> to <new stem>. The rest of
the `laserdisc:` block stays, because the sections' disc areas and codes
need it; a `laserdisc:` block holding nothing but `channel:` is an error.
"""
import sys


def indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(' '))


def main() -> int:
    source, target, old_stem, new_stem = sys.argv[1:5]
    with open(source, encoding='utf-8') as f:
        lines = f.read().split('\n')
    kept = []
    in_laserdisc = False
    channel_indent = None
    dropped = False
    laserdisc_keys = 0
    for line in lines:
        content = line.strip()
        if line[:1] not in ('', ' ', '\t', '#'):
            in_laserdisc = line.startswith('laserdisc:')
            channel_indent = None
        elif in_laserdisc and content and not content.startswith('#'):
            if channel_indent is not None and indent_of(line) > channel_indent:
                continue
            channel_indent = None
            if content.startswith('channel:'):
                channel_indent = indent_of(line)
                dropped = True
                continue
            if indent_of(line) == 2:
                laserdisc_keys += 1
        kept.append(line)
    text = '\n'.join(kept)
    if not dropped:
        print(f'drop_channel_block: {source} has no laserdisc.channel block',
              file=sys.stderr)
        return 1
    if laserdisc_keys == 0:
        print(f'drop_channel_block: {source} has nothing in its laserdisc '
              'block but channel', file=sys.stderr)
        return 1
    if old_stem not in text:
        print(f'drop_channel_block: {source} does not name {old_stem}',
              file=sys.stderr)
        return 1
    with open(target, 'w', encoding='utf-8') as f:
        f.write(text.replace(old_stem, new_stem))
    return 0


if __name__ == '__main__':
    sys.exit(main())
