#!/usr/bin/env python3
"""Check the relative links in Markdown files: that each file linked to
exists, and each #anchor is a heading there (as GitHub makes them:
lowercase, spaces to hyphens, punctuation dropped; a repeated heading's
anchor gets -1, -2 and so on) or an id="..." in its HTML.

  tools/check-links.py [FILE.md ...]

Without files, it checks README.md, THIRD-PARTY-NOTICES.md and docs/*.md.
Links in code (fenced blocks, `spans`) aren't links, and aren't checked;
nor are links to other sites. Exits 1 if a link doesn't resolve.
"""
import glob
import os
import re
import sys
import unicodedata
from urllib.parse import unquote

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FENCE = re.compile(r'^\s*(```|~~~)')
CODE_SPAN = re.compile(r'(`+)(.+?)\1')
# [text](target "title"), ![alt](target); the text may hold one level of [brackets]
INLINE = re.compile(r'!?\[(?:[^\[\]]|\[[^\]]*\])*\]\(\s*<?([^)\s>]*)>?(?:\s+"[^"]*")?\s*\)')
REFERENCE = re.compile(r'^\s{0,3}\[[^\]]+\]:\s*<?(\S+?)>?(?:\s+"[^"]*")?\s*$')
HTML_LINK = re.compile(r'<(?:a|img)\b[^>]*?\b(?:href|src)\s*=\s*"([^"]*)"', re.I)
HTML_ID = re.compile(r'\b(?:id|name)\s*=\s*"([^"]+)"', re.I)
HEADING = re.compile(r'^\s{0,3}(#{1,6})\s+(.*?)\s*#*\s*$')


def prose_lines(text):
    """The lines outside fenced code blocks, with their numbers"""
    fenced = False
    for n, line in enumerate(text.split('\n'), 1):
        if FENCE.match(line):
            fenced = not fenced
            continue
        if not fenced:
            yield n, line


def slug(heading):
    """A heading's anchor, as GitHub makes it"""
    text = CODE_SPAN.sub(lambda m: m.group(2), heading)
    text = re.sub(r'!?\[([^\]]*)\]\([^)]*\)', r'\1', text)     # (a link's text)
    text = re.sub(r'<[^>]+>', '', text)                         # (HTML tags)
    text = text.strip().lower()
    out = []
    for ch in text:
        cat = unicodedata.category(ch)
        if ch in '-_' or cat[0] in 'LN' or cat == 'Mn':
            out.append(ch)
        elif ch == ' ':
            out.append('-')
    return ''.join(out)


anchor_cache = {}


def anchors(path):
    if path not in anchor_cache:
        found = set()
        seen = {}
        with open(path, encoding='utf-8') as f:
            text = f.read()
        for _, line in prose_lines(text):
            m = HEADING.match(line)
            if m:
                base = slug(m.group(2))
                k = seen.get(base, 0)
                seen[base] = k + 1
                found.add(base if k == 0 else f'{base}-{k}')
            found.update(HTML_ID.findall(line))
        anchor_cache[path] = found
    return anchor_cache[path]


def links(text):
    for n, line in prose_lines(text):
        bare = CODE_SPAN.sub('', line)
        for pattern in (INLINE, HTML_LINK):
            for m in pattern.finditer(bare):
                yield n, m.group(1)
        m = REFERENCE.match(bare)
        if m:
            yield n, m.group(1)


def check(path):
    bad = 0
    with open(path, encoding='utf-8') as f:
        text = f.read()
    here = os.path.dirname(path)
    for n, target in links(text):
        if not target or re.match(r'[a-z][a-z0-9+.-]*:', target, re.I) or target.startswith('//'):
            continue        # (another site, or mailto:)
        file_part, _, anchor = target.partition('#')
        dest = os.path.normpath(os.path.join(here, unquote(file_part))) if file_part else path
        why = None
        if not os.path.exists(dest):
            why = 'no such file'
        elif anchor and dest.endswith('.md') and unquote(anchor) not in anchors(dest):
            why = f'no heading #{anchor} in {os.path.relpath(dest, ROOT)}'
        if why:
            print(f'{os.path.relpath(path, ROOT)}:{n}: {target}: {why}')
            bad += 1
    return bad


def main():
    files = sys.argv[1:] or [os.path.join(ROOT, 'README.md'), os.path.join(ROOT, 'THIRD-PARTY-NOTICES.md')] + \
        sorted(glob.glob(os.path.join(ROOT, 'docs', '*.md')))
    bad = sum(check(os.path.abspath(f)) for f in files)
    print(f'links: {len(files)} files, {bad} broken' if bad else f'links: {len(files)} files, all resolve')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
