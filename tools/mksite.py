#!/usr/bin/env python3
"""Render STATUS.md + NOTES.md + git log into site/index.html. No deps."""
import html, re, subprocess, sys, datetime, os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def md(text):
    out, i, lines = [], 0, text.split('\n')
    inlist = None
    def close():
        nonlocal inlist
        if inlist: out.append(f'</{inlist}>'); inlist = None
    while i < len(lines):
        l = lines[i]
        if l.startswith('```'):
            close(); j = i + 1; buf = []
            while j < len(lines) and not lines[j].startswith('```'): buf.append(lines[j]); j += 1
            out.append('<pre>' + html.escape('\n'.join(buf)) + '</pre>'); i = j + 1; continue
        if l.startswith('|') and i + 1 < len(lines) and re.match(r'^\|[\s:|-]+\|$', lines[i+1]):
            close(); rows = []
            while i < len(lines) and lines[i].startswith('|'):
                rows.append([c.strip() for c in lines[i].strip('|').split('|')]); i += 1
            hdr, body = rows[0], rows[2:]
            t = '<table><tr>' + ''.join(f'<th>{inline(c)}</th>' for c in hdr) + '</tr>'
            for r in body: t += '<tr>' + ''.join(f'<td>{inline(c)}</td>' for c in r) + '</tr>'
            out.append(t + '</table>'); continue
        m = re.match(r'^(#{1,4})\s+(.*)', l)
        if m:
            close(); n = len(m.group(1)); out.append(f'<h{n}>{inline(m.group(2))}</h{n}>'); i += 1; continue
        m = re.match(r'^\s*[-*]\s+(.*)', l)
        if m:
            if inlist != 'ul': close(); out.append('<ul>'); inlist = 'ul'
            out.append(f'<li>{inline(m.group(1))}</li>'); i += 1; continue
        m = re.match(r'^\s*\d+\.\s+(.*)', l)
        if m:
            if inlist != 'ol': close(); out.append('<ol>'); inlist = 'ol'
            out.append(f'<li>{inline(m.group(1))}</li>'); i += 1; continue
        if l.strip() == '': close(); i += 1; continue
        close(); out.append(f'<p>{inline(l)}</p>'); i += 1
    close(); return '\n'.join(out)

def inline(s):
    s = html.escape(s)
    s = re.sub(r'`([^`]+)`', r'<code>\1</code>', s)
    s = re.sub(r'\*\*([^*]+)\*\*', r'<b>\1</b>', s)
    s = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'<a href="\2">\1</a>', s)
    return s

def read(p):
    try: return open(os.path.join(ROOT, p)).read()
    except FileNotFoundError: return f'(no {p} yet)'

log = subprocess.run(['git', '-C', ROOT, 'log', '--date=format:%H:%M', '--format=%h %ad %s', '-n', '40'],
                     capture_output=True, text=True).stdout
CSS = """body{font:15px/1.5 system-ui,sans-serif;max-width:960px;margin:2em auto;padding:0 1em;color:#222;background:#fafafa}
pre{background:#f0f0f0;padding:.8em;overflow-x:auto;font-size:13px}code{background:#eee;padding:0 3px}
table{border-collapse:collapse;margin:1em 0}th,td{border:1px solid #ccc;padding:3px 8px;text-align:left}th{background:#eee}
h1{border-bottom:2px solid #444}h2{margin-top:1.6em;border-bottom:1px solid #bbb}.meta{color:#666;font-size:13px}
nav a{margin-right:1em}details{margin:1em 0}summary{cursor:pointer;font-weight:bold}"""
now = datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')
page = f"""<!doctype html><html><head><meta charset=utf-8><meta http-equiv=refresh content=60>
<title>sandbox-stack progress</title><style>{CSS}</style></head><body>
<p class=meta>rendered {now} · auto-refreshes every 60s · <a href="NOTES.md">NOTES.md raw</a> · <a href="STATUS.md">STATUS.md raw</a> · <a href="tree/">browse files</a></p>
{md(read('STATUS.md'))}
<h2>Recent commits</h2><pre>{html.escape(log) or '(none yet)'}</pre>
<details open><summary>NOTES.md (full lab notebook)</summary>{md(read('NOTES.md'))}</details>
</body></html>"""
os.makedirs(os.path.join(ROOT, 'site'), exist_ok=True)
open(os.path.join(ROOT, 'site', 'index.html'), 'w').write(page)
for f in ('NOTES.md', 'STATUS.md'):
    if os.path.exists(os.path.join(ROOT, f)):
        open(os.path.join(ROOT, 'site', f), 'w').write(read(f))
