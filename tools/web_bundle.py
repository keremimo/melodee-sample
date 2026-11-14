#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Builds the web page's bundle (src/melodee/web/web_bundle.h).

The device page, the quickstart and the user guide are written as HTML pages
in the shell of src/melodee/web/static/shell.html, beside the style sheet,
the script and every image the docs show. The docs are converted from their
Markdown here: headings, paragraphs, lists, tables, images, links, bold and
code, which is all the user docs use. Links between them become the page's
own; links to other files go to the repository on GitHub.

Usage: web_bundle.py OUT.bin
"""
import html
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
STATIC = ROOT / "src/melodee/web/static"
REPO = "https://github.com/keremimo/melodee-sample/blob/main/"
TYPES = {".html": "text/html; charset=utf-8", ".css": "text/css; charset=utf-8",
         ".js": "text/javascript; charset=utf-8", ".png": "image/png", ".svg": "image/svg+xml"}


def slug(text):
    """A heading's anchor as GitHub makes it."""
    text = re.sub(r"<[^>]+>", "", text).strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def page_of(path):
    """The page a docs file is served as, or None."""
    rel = path.relative_to(DOCS).as_posix() if path.is_relative_to(DOCS) else None
    if rel == "QUICKSTART.md":
        return "/docs/quickstart"
    if rel == "GUIDE.md":
        return "/docs/manual"
    if rel and rel.startswith("guide/") and rel.endswith(".md"):
        return "/docs/manual/" + rel[len("guide/"):-3]
    return None


class Converter:
    def __init__(self, source):
        self.source = source
        self.images = set()
        self.headings = []  # (level, text, id)
        self.ids = {}

    def href(self, target):
        if re.match(r"[a-z]+:", target) or target.startswith("#"):
            return target
        path, _, anchor = target.partition("#")
        resolved = (self.source.parent / path).resolve()
        page = page_of(resolved)
        if page:
            return page + ("#" + anchor if anchor else "")
        return REPO + resolved.relative_to(ROOT).as_posix() + ("#" + anchor if anchor else "")

    def image(self, target):
        resolved = (self.source.parent / target).resolve()
        rel = resolved.relative_to(DOCS / "images").as_posix()
        self.images.add(rel)
        return "/img/" + rel

    def inline(self, text):
        codes = []

        def keep(match):
            codes.append("<code>" + html.escape(match.group(1)) + "</code>")
            return "\x00%d\x00" % (len(codes) - 1)

        text = re.sub(r"`([^`]+)`", keep, text)
        text = html.escape(text, quote=False)
        text = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)",
                      lambda m: '<img src="%s" alt="%s" loading="lazy">' % (self.image(m.group(2)), html.escape(m.group(1))), text)
        text = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", lambda m: self.link(m.group(1), m.group(2)), text)
        text = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", text)
        # A button or knob printed on the MPC (SHIFT, K1, PAD BANK): drawn as a key.
        text = re.sub(r"<strong>((?=[^<]*[A-Z])[A-Z0-9][A-Z0-9 +\-/.]*)</strong>", r'<strong class="key">\1</strong>', text)
        text = re.sub(r"(?<![\w*])\*(\S(?:.*?\S)?)\*(?![\w*])", r"<em>\1</em>", text)
        return re.sub("\x00(\\d+)\x00", lambda m: codes[int(m.group(1))], text)

    def link(self, label, target):
        target = self.href(html.unescape(target))
        outside = target.startswith("http")
        return '<a href="%s"%s>%s</a>' % (html.escape(target), ' target="_blank" rel="noopener"' if outside else "", label)

    def heading(self, level, text):
        base = slug(text)
        n = self.ids.get(base, 0)
        self.ids[base] = n + 1
        anchor = base if n == 0 else "%s-%d" % (base, n)
        body = self.inline(text)
        self.headings.append((level, body, anchor))
        return '<h%d id="%s">%s</h%d>' % (level, anchor, body, level)

    def table(self, rows):
        def cells(line):
            line = line.strip().strip("|")
            return [c.strip().replace("\\|", "|") for c in re.split(r"(?<!\\)\|", line)]

        head, aligns = cells(rows[0]), cells(rows[1])
        align = ["right" if a.endswith(":") and not a.startswith(":") else "center" if a.startswith(":") and a.endswith(":") else ""
                 for a in aligns]

        def row(tag, values):
            out = []
            for i, v in enumerate(values):
                a = ' style="text-align:%s"' % align[i] if i < len(align) and align[i] else ""
                out.append("<%s%s>%s</%s>" % (tag, a, self.inline(v), tag))
            return "<tr>" + "".join(out) + "</tr>"

        body = "".join(row("td", cells(r)) for r in rows[2:])
        return '<div class="table"><table><thead>%s</thead><tbody>%s</tbody></table></div>' % (row("th", head), body)

    def lists(self, items):
        """items: (indent, ordered, text). Nested by indent."""
        out, stack = [], []  # stack of (indent, tag)
        for indent, ordered, text in items:
            tag = "ol" if ordered else "ul"
            while stack and indent < stack[-1][0]:
                out.append("</li></%s>" % stack.pop()[1])
            if not stack or indent > stack[-1][0]:
                out.append("<%s>" % tag)
                stack.append((indent, tag))
            else:
                out.append("</li>")
            out.append("<li>" + self.inline(text))
        while stack:
            out.append("</li></%s>" % stack.pop()[1])
        return "".join(out)

    def convert(self, text):
        lines = text.split("\n")
        out, i = [], 0
        item = re.compile(r"^( *)([-*]|\d+\.) +(.*)$")
        while i < len(lines):
            line = lines[i]
            if not line.strip():
                i += 1
                continue
            m = re.match(r"^(#{1,6}) +(.*)$", line)
            if m:
                out.append(self.heading(len(m.group(1)), m.group(2).strip()))
                i += 1
                continue
            if re.match(r"^-{3,}\s*$", line):
                out.append("<hr>")
                i += 1
                continue
            if line.startswith("|") and i + 1 < len(lines) and re.match(r"^\|?\s*:?-+", lines[i + 1]):
                rows = []
                while i < len(lines) and lines[i].startswith("|"):
                    rows.append(lines[i])
                    i += 1
                out.append(self.table(rows))
                continue
            if item.match(line):
                items = []
                while i < len(lines):
                    line = lines[i]
                    m = item.match(line)
                    if m:
                        items.append([len(m.group(1)), m.group(2)[0].isdigit(), m.group(3)])
                    elif line.strip() and (line.startswith(" ") or lines[i - 1].strip()):
                        # an item's next line: indented, or straight after it (no blank line between)
                        if lines[i - 1].strip() or line.startswith(" "):
                            items[-1][2] += " " + line.strip()
                    elif not line.strip():
                        nxt = next((l for l in lines[i + 1:] if l.strip()), "")
                        if not (item.match(nxt) or nxt.startswith("  ")):
                            break
                    else:
                        break
                    i += 1
                out.append(self.lists(items))
                continue
            para = []
            while i < len(lines) and lines[i].strip() and not re.match(r"^(#{1,6} |\||-{3,}\s*$)", lines[i]) and not item.match(lines[i]):
                para.append(lines[i].strip())
                i += 1
            body = self.inline(" ".join(para))
            if re.fullmatch(r'<img [^>]+>', body):
                alt = re.search(r'alt="([^"]*)"', body).group(1)
                out.append("<figure>%s%s</figure>" % (body, "<figcaption>%s</figcaption>" % alt if alt else ""))
            else:
                out.append("<p>%s</p>" % body)
        return "\n".join(out)


def nav_lines_out(text):
    """A chapter's "Previous · Contents · Next" lines and the rule before the last: the page draws its own."""
    lines = [l for l in text.split("\n") if not re.match(r"^(Previous:|Next:|\[Contents\])", l.strip())]
    while lines and (not lines[-1].strip() or re.match(r"^-{3,}\s*$", lines[-1])):
        lines.pop()
    return "\n".join(lines)


def chapters():
    """The guide's chapters in GUIDE.md's order: (slug, title, sections)."""
    out = []
    for line in (DOCS / "GUIDE.md").read_text().split("\n"):
        m = re.match(r"^\d+\. \[([^\]]+)\]\(guide/([\w-]+)\.md\)", line)
        if m:
            out.append([m.group(2), m.group(1), []])
        m = re.match(r"^ +- \[([^\]]+)\]\(guide/[\w-]+\.md#([\w-]+)\)", line)
        if m and out:
            out[-1][2].append((m.group(1), m.group(2)))
    return out


def shell(title, active, main, description=""):
    page = (STATIC / "shell.html").read_text()
    current = {k: ' aria-current="page"' if k == active else "" for k in ("device", "files", "quickstart", "manual")}
    return (page.replace("{{title}}", html.escape(title))
                .replace("{{device}}", current["device"])
                .replace("{{files}}", current["files"])
                .replace("{{quickstart}}", current["quickstart"])
                .replace("{{manual}}", current["manual"])
                .replace("{{main}}", main))


def toc(headings, here=""):
    items = "".join('<li><a href="%s#%s">%s</a></li>' % (here, a, re.sub(r"<[^>]+>", "", t)) for lvl, t, a in headings if lvl == 2)
    return '<nav class="toc" aria-label="On this page"><p class="label">On this page</p><ol>%s</ol></nav>' % items


def main():
    out_path = Path(sys.argv[1])
    files, images = {}, set()

    files["/files"] = shell("Files · Melodee", "files", (STATIC / "files.html").read_text())
    files["/app.css"] = (STATIC / "app.css").read_text()
    files["/app.js"] = (STATIC / "app.js").read_text()

    # Quickstart: one page, its sections at the side.
    qs = Converter(DOCS / "QUICKSTART.md")
    body = qs.convert((DOCS / "QUICKSTART.md").read_text())
    body = re.sub(r"^<h1[^>]*>.*?</h1>\n<p>.*?</p>\n<ol>.*?</ol>", "", body, count=1, flags=re.S)  # its own contents list
    files["/docs/quickstart"] = shell("Quickstart · Melodee", "quickstart",
        '<div class="doc-layout"><aside>%s</aside><article class="doc"><p class="kicker">Quickstart</p>'
        '<h1>From the zip to a saved beat</h1>%s</article></div>' % (toc(qs.headings), body))
    images |= qs.images

    # The manual: a contents page of chapter cards, then a page per chapter.
    parts = chapters()
    guide = Converter(DOCS / "GUIDE.md")
    intro = (DOCS / "GUIDE.md").read_text().split("## Contents")[0]
    intro_html = guide.convert("\n".join(l for l in intro.split("\n") if not l.startswith("# ")))
    cards = "".join(
        '<a class="chapter" href="/docs/manual/%s"><span class="num">%02d</span><span class="name">%s</span>'
        '<span class="sections">%s</span></a>' % (s, n + 1, html.escape(t), html.escape(" · ".join(x for x, _ in secs[:4]) + (" …" if len(secs) > 4 else "")))
        for n, (s, t, secs) in enumerate(parts))
    files["/docs/manual"] = shell("Manual · Melodee", "manual",
        '<div class="manual-index"><p class="kicker">Manual</p><h1>Melodee user guide</h1><div class="lead">%s</div>'
        '<div class="chapters">%s</div></div>' % (intro_html, cards))
    for n, (s, title, _) in enumerate(parts):
        conv = Converter(DOCS / "guide" / (s + ".md"))
        body = conv.convert(nav_lines_out((DOCS / "guide" / (s + ".md")).read_text()))
        body = re.sub(r"^<h1[^>]*>.*?</h1>", "", body, count=1, flags=re.S)
        images |= conv.images
        side = "".join(
            '<li%s><a href="/docs/manual/%s"><span class="num">%02d</span>%s</a>%s</li>' % (
                ' class="here"' if k == n else "", ps, k + 1, html.escape(pt),
                toc(conv.headings).replace('<nav class="toc" aria-label="On this page"><p class="label">On this page</p>', '<nav class="toc">') if k == n else "")
            for k, (ps, pt, _) in enumerate(parts))
        prev_next = '<nav class="pager">'
        if n > 0:
            prev_next += '<a class="prev" href="/docs/manual/%s"><span class="label">Previous</span>%s</a>' % (parts[n - 1][0], html.escape(parts[n - 1][1]))
        if n + 1 < len(parts):
            prev_next += '<a class="next" href="/docs/manual/%s"><span class="label">Next</span>%s</a>' % (parts[n + 1][0], html.escape(parts[n + 1][1]))
        prev_next += "</nav>"
        files["/docs/manual/" + s] = shell(title + " · Melodee", "manual",
            '<div class="doc-layout"><aside><details class="chapters-menu" open><summary>Chapters</summary><ol class="chapter-list">%s</ol>'
            '</details></aside><article class="doc"><p class="kicker">Chapter %d</p><h1>%s</h1>%s%s</article></div>'
            % (side, n + 1, html.escape(title), body, prev_next))

    # The device page, with the docs' contents on its Learn cards.
    steps = "".join("<li>%s</li>" % re.sub(r"^\d+\.\s*", "", re.sub(r"<[^>]+>", "", t)) for lvl, t, a in qs.headings if lvl == 2 and re.match(r"\d+\.", t))
    names = "".join("<li>%s</li>" % html.escape(t) for _, t, _ in parts)
    files["/"] = shell("Melodee", "device", (STATIC / "device.html").read_text()
                       .replace("{{quickstart_steps}}", steps).replace("{{chapter_names}}", names)
                       .replace("{{chapter_count}}", str(len(parts))))

    for rel in sorted(images):
        files["/img/" + rel] = (DOCS / "images" / rel).read_bytes()

    blob = bytearray(b"MLWB")
    for path, data in files.items():
        if isinstance(data, str):
            data = data.encode()
        suffix = Path(path).suffix
        kind = TYPES.get(suffix, TYPES[".html"])
        for part in (path.encode(), kind.encode(), data):
            blob += struct.pack("<I", len(part)) + part
    out_path.write_bytes(blob)
    print("web bundle: %d files, %.1f MB" % (len(files), len(blob) / 1e6))


if __name__ == "__main__":
    main()
