#!/usr/bin/env python3
# The user manual (HTML) as Markdown to edit: html2md.py Benutzerhandbuch.html Benutzerhandbuch.md
# Menu names in backticks, notes as quotes, figures as images. The HTML stays the
# source of the PDF; edits of the Markdown are carried back into it by hand.
#
# Copyright (C) 2026 bond Software Entwicklung GmbH
# SPDX-License-Identifier: LGPL-2.1-or-later
import sys, re, html
from html.parser import HTMLParser

class Conv(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out = []          # finished blocks
        self.buf = ''          # current inline text
        self.lists = []        # stack of 'ul'/'ol' with counters
        self.skip = 0          # inside nav/style/head/title
        self.pre = False
        self.table = None      # list of rows
        self.row = None
        self.cell = None
        self.quote = 0
        self.href = []
        self.in_body = False
        self.heading = None
    def flush(self, prefix=''):
        t = re.sub(r'[ \t\r\n]+', ' ', self.buf).strip()
        self.buf = ''
        if t:
            if self.quote:
                t = '> ' + t
            self.out.append(prefix + t)
    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag in ('style', 'head', 'nav', 'title', 'figcaption'):
            self.skip += 1; return
        if self.skip: return
        if tag == 'body': self.in_body = True
        if tag == 'img':
            self.flush(); self.out.append('![%s](%s)' % (a.get('alt', ''), a.get('src', ''))); return
        if tag == 'figure':
            self.flush(); return
        cls = a.get('class', '')
        if tag in ('h0', 'h1', 'h2', 'h3'):
            self.flush(); self.heading = tag
        elif tag == 'p':
            self.flush()
        elif tag in ('ul', 'ol'):
            self.flush(); self.lists.append([tag, 0])
        elif tag == 'li':
            self.flush()
            self.lists[-1][1] += 1
        elif tag == 'b': self.buf += '**'
        elif tag == 'i': self.buf += '*'
        elif tag == 'code': self.buf += '`'
        elif tag == 'span' and cls in ('menu', 'path'): self.buf += '`'
        elif tag == 'span': pass
        elif tag == 'a':
            self.href.append(a.get('href', '')); self.buf += '['
        elif tag == 'br': self.buf += '  \n'
        elif tag == 'div' and cls == 'note':
            self.flush(); self.quote += 1
        elif tag == 'div' and cls in ('title', 'sub', 'meta', 'desc'):
            self.flush()
        elif tag == 'pre':
            self.flush(); self.pre = True; self.buf = ''
        elif tag == 'table':
            self.flush(); self.table = []
        elif tag == 'tr': self.row = []
        elif tag in ('td', 'th'): self.cell = ''; self.buf = ''
    def handle_endtag(self, tag):
        if tag in ('style', 'head', 'nav', 'title', 'figcaption'):
            self.skip -= 1; return
        if self.skip: return
        if tag in ('h0', 'h1', 'h2', 'h3'):
            level = {'h0': '#', 'h1': '#', 'h2': '##', 'h3': '###'}[tag]
            t = re.sub(r'\s+', ' ', self.buf).strip(); self.buf = ''
            self.out.append(level + ' ' + t); self.heading = None
        elif tag == 'p':
            self.flush()
        elif tag == 'li':
            depth = len(self.lists) - 1
            kind, n = self.lists[-1]
            mark = '%d.' % n if kind == 'ol' else '-'
            self.flush('   ' * depth + mark + ' ')
        elif tag in ('ul', 'ol'):
            self.flush(); self.lists.pop()
        elif tag == 'b': self.buf += '**'
        elif tag == 'i': self.buf += '*'
        elif tag == 'code': self.buf += '`'
        elif tag == 'span' and self.buf.count('`') % 2 == 1: self.buf += '`'
        elif tag == 'a':
            self.buf += '](%s)' % self.href.pop()
        elif tag == 'figure':
            self.flush()
        elif tag == 'div' and self.quote:
            self.flush(); self.quote -= 1
        elif tag == 'div':
            self.flush()
        elif tag == 'pre':
            self.out.append('```\n' + self.buf.strip('\n') + '\n```'); self.buf = ''; self.pre = False
        elif tag in ('td', 'th'):
            self.row.append(re.sub(r'\s+', ' ', self.buf).strip()); self.buf = ''
        elif tag == 'tr':
            self.table.append(self.row)
        elif tag == 'table':
            rows = self.table; self.table = None
            if rows:
                lines = ['| ' + ' | '.join(rows[0]) + ' |', '|' + '---|' * len(rows[0])]
                lines += ['| ' + ' | '.join(r) + ' |' for r in rows[1:]]
                self.out.append('\n'.join(lines))
    def handle_data(self, data):
        if self.skip: return
        if not self.in_body: return
        if self.pre:
            self.buf += data
        else:
            self.buf += data.replace('\xa0', ' ')

src = open(sys.argv[1], encoding='utf-8').read()
c = Conv(); c.feed(src); c.flush()
head = ('<!--\n  Benutzerhandbuch von evolution-groupwise als Markdown zum Bearbeiten.\n'
        '  Quelle für das PDF bleibt Benutzerhandbuch.html; Claude überträgt die\n'
        '  Änderungen dorthin. Menünamen stehen in `Backticks`, **fett** und *kursiv*\n'
        '  wie gewohnt, Hinweiskästen als Zitat (> ...), Bilder als ![Beschreibung](Datei.png)\n  (die Beschreibung wird zur Bildunterschrift). Das Inhaltsverzeichnis\n'
        '  entsteht automatisch aus den Überschriften.\n-->\n\n')
text = head + '\n\n'.join(c.out) + '\n'
text = re.sub(r'\*\*\s+\*\*', ' ', text)
open(sys.argv[2], 'w', encoding='utf-8').write(text)
