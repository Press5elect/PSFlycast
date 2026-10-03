#!/usr/bin/env python3
"""
PSFlyCast - makes patches.txt from the Flycast widescreen and 60 FPS chart.

    make_patches.py "<chart>/Dreamcast Widescreen.md" "<libretro-database>/metadat/redump/Sega - Dreamcast.dat" > patches.txt

The chart (github.com/nexus382/Flycast-Widescreen-Compatability-And-Cheat-Chart)
lists, for a game and a region, CodeBreaker codes that make it draw a 16:9
picture and codes that make it run at 60 frames a second. Each becomes a patch:
the codes turned into the writes Flycast's cheat manager would make of them
(core/cheats.cpp, addGameSharkCheat), and the names a game file may have - the
Redump names of that game in that region, by which the title recognises a disc
image, and the chart's own title for a file named otherwise.

Left out, and listed on stderr: a cell with a code that is not eight digits and
eight digits (the patch would be half of what was meant), a code type other
than the writes and conditions below, a game the Redump list does not have
under a name that could be matched, and a revision its names do not tell apart.
Demos, betas and limited editions are other builds and get no patch.
"""
import collections
import re
import sys

CODE = re.compile(r'(?<![0-9A-Fa-f])([0-9A-Fa-f]{8}) ([0-9A-Fa-f]{8})(?![0-9A-Fa-f])')
HEXISH = re.compile(r'(?<![0-9A-Za-z])[0-9A-Fa-f]{5,}(?![0-9A-Za-z])')

# The chart's title -> the Redump title, where they differ by more than punctuation.
ALIASES = {
    'berserk': 'berserk millennium falcon hen wasurebana no shou',
    'cool boarders': 'cool boarders burrrn',
    'donald duck quack attack': 'disneys donald duck quack attack',
    'gun spike': 'gunspike',
    'metropolis street racer': 'msr metropolis street racer',
    'outrigger': 'outtrigger',
    'pen pen triicelon': 'penpen triicelon',
    'pen pen': 'penpen',
    'ring terrors realm': 'the ring terrors realm',
    'rippin riders': 'rippin riders snowboarding',
    'segagaga': 'sggg segagaga',
    'soul calibur': 'soulcalibur',
    'tennis 2k2 virtua tennis 2': 'tennis 2k2',
    'virtua tennis 2 tennis 2k2': 'virtua tennis 2 sega professional tennis',
    'virtua tennis power smash': 'virtua tennis',
    'toyota doricatch series land cruiser 100 cygnus': 'doricatch land cruiser 100 cygnus',
    'whats shenmue': 'whats shenmue yukawa',
    # Released under another name in one region.
    ('d2', 'JP'): 'd2 d no shokutaku 2',
    ('daytona usa', 'EU'): 'daytona usa 2001',
    ('dynamite cop', 'JP'): 'dynamite deka 2',
    ('f355 challenge passione rossa', 'JP'): 'f355 challenge',
    ('marvel vs capcom 2 new age of heroes', 'NA'): 'marvel vs capcom 2',
    ('project justice', 'EU'): 'project justice rival schools 2',
    ('shenmue', 'JP'): 'shenmue isshou yokosuka',
    ('tony hawks pro skater', 'EU'): 'tony hawks skateboarding',
    ('virtua tennis power smash', 'EU'): 'virtua tennis sega professional tennis',
}

# The tags a name may have after its region: languages and a revision. Any
# other (a demo, a beta, a limited edition, a bonus disc) is another build of
# the game, with addresses of its own.
PLAIN_TAG = re.compile(r'[A-Z][a-z](,[A-Z][a-z])*|Rev [0-9A-Z]+')

REGIONS = {
    'NA': ('USA',),
    'EU': ('Europe', 'UK', 'Australia'),      # a release in one language (France, Germany...) is a build of its own
    'JP': ('Japan',),
}


def norm(s):
    s = s.lower().replace('&', ' and ').replace("'", '').replace('’', '')
    return re.sub(r'[^a-z0-9]+', ' ', s).strip()


def base(name):
    b = re.split(r'\s*[\(\[]', name, 1)[0]
    m = re.match(r'^(.*?), (The|A|An)( - .*)?$', b)
    if m:
        b = m.group(2) + ' ' + m.group(1) + (m.group(3) or '')
    return b


def without_disc(name):
    return re.sub(r'\s*\((Disc|Disk|GD-ROM) [0-9A-D]+( of [0-9]+)?\)', '', name)


def ops_of(codes):
    """The cheat manager's entries for a list of (first, second) code words; None for a type not taken."""
    ops = []
    for first, second in codes:
        kind, address = first >> 24, first & 0x00ffffff
        if kind in (0, 1, 2):
            ops.append('set:%d:%X:%X' % ((8, 16, 32)[kind], address, second))
        elif kind == 0x0d:
            test = {0: 'ifeq', 1: 'ifne', 2: 'iflt', 3: 'ifgt'}.get(second >> 16)
            if test is None:
                return None
            ops.append('%s:16:%X:%X' % (test, address, second & 0xffff))
        else:
            return None
    return ops


def main():
    chart, dat = sys.argv[1], sys.argv[2]
    redump = collections.OrderedDict()
    for m in re.finditer(r'game \(\n\tname "([^"]+)"\n\tregion "([^"]*)"', open(dat, encoding='utf-8').read()):
        redump.setdefault(without_disc(m.group(1)), m.group(2))
    by_base = collections.defaultdict(list)
    for name in redump:
        by_base[norm(base(name))].append(name)

    out, skipped, nameless = [], [], []
    for line in open(chart, encoding='utf-8'):
        if not line.startswith('|') or line.startswith('|--') or 'Game Title' in line:
            continue
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) < 7:
            continue
        title, region = cells[0], cells[1]
        for kind, cell in (('60fps', cells[5]), ('widescreen', cells[6])):
            cell = re.sub(r'[0-9A-Fa-f]+ offset track\d+\.bin', '', cell)   # a patch of the disc image, not of memory
            cell = cell.split(' or ')[0]                                     # alternatives: the first
            codes = [(int(a, 16), int(b, 16)) for a, b in CODE.findall(cell)]
            if not codes and not re.search(r'(?<![0-9A-Za-z])[0-9A-Fa-f]{6,8} [0-9A-Fa-f]{6,9}(?![0-9A-Za-z])', cell):
                continue
            if not codes or HEXISH.search(CODE.sub('', cell)):
                skipped.append('%s [%s] %s: a code that is not eight digits and eight digits' % (title, region, kind))
                continue
            ops = ops_of(codes)
            if ops is None:
                skipped.append('%s [%s] %s: a code type that is not taken' % (title, region, kind))
                continue
            # The Redump names of this game in this region.
            key = norm(title)
            area = region.split()[0]
            key = ALIASES.get((key, area), ALIASES.get(key, key))
            qualifier = region[len(area):].strip()
            country = {'Fr': 'France', 'De': 'Germany', 'Es': 'Spain', 'It': 'Italy'}
            wanted = REGIONS.get(area, ())
            if qualifier.startswith('('):
                # "EU (Fr)": the release of that country, or the European one in just those languages.
                wanted = wanted + tuple(country[l.strip()] for l in qualifier.strip('()').split(',') if l.strip() in country)
            names = []
            for name in by_base.get(key, []):
                tags = re.findall(r'\(([^)]*)\)', name)
                places = tags[0].split(', ') if tags else []
                if not any(p in wanted for p in places):
                    continue
                if not all(PLAIN_TAG.fullmatch(t) for t in tags[1:]):
                    continue
                rest = qualifier
                if rest.startswith('('):
                    langs = [l.strip() for l in rest.strip('()').split(',')]
                    if len(langs) == 1 and country.get(langs[0]) in places:
                        names.append(name)
                        continue
                    listed = [t.split(',') for t in tags[1:] if re.fullmatch(r'[A-Z][a-z](,[A-Z][a-z])*', t)]
                    english = langs == ['En'] and places == ['Europe'] and not listed
                    if not (english or (listed and sorted(listed[0]) == sorted(langs))):
                        continue
                elif rest:
                    continue        # "EU v1.001": a revision Redump's names do not tell apart
                names.append(name)
            if not names and qualifier:
                skipped.append('%s [%s] %s: no Redump name for that release' % (title, region, kind))
                continue
            if not names:
                nameless.append('%s [%s]' % (title, region))
            out.append((title, region, kind, names, ops))

    print('# PSFlyCast - game patches: a 16:9 picture, 60 frames a second.')
    print('# From the Flycast widescreen and 60 FPS chart, by nexus382 and its contributors:')
    print('#   https://github.com/nexus382/Flycast-Widescreen-Compatability-And-Cheat-Chart')
    print('# Made by shell/ps5/patches/make_patches.py; one patch a line, tab-separated:')
    print('#   kind, region (NA, EU, JP; "EU (Fr)" for one release), the game, the file names it goes by (| between them), the writes.')
    print('# A game file is found by its name (without its extension and disc number), else by the')
    print("# game's title when its name carries no more than (USA), (Europe) or (Japan); a patch for one release is found by name only.")
    print('# A write is type:bits:address:value in hexadecimal; ifeq, ifne, iflt and ifgt guard the write after them.')
    for title, area, kind, names, ops in out:
        print('\t'.join([kind, area, title, '|'.join(names), ' '.join(ops)]))
    sys.stderr.write('%d patches (%d widescreen, %d 60 FPS); left out:\n' % (
        len(out), sum(1 for o in out if o[2] == 'widescreen'), sum(1 for o in out if o[2] == '60fps')))
    for s in skipped:
        sys.stderr.write('  ' + s + '\n')
    sys.stderr.write('not in the Redump list, found by the chart\'s title alone: %s\n' % ', '.join(nameless))


if __name__ == '__main__':
    main()
