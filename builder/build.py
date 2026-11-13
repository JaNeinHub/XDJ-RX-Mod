#!/usr/bin/env python3
"""Assemble the XDJRX-MOD web builder: one self-contained HTML page.

    python3 build.py [--test] [--render-screens] [--updater <path>] [<out dir>]      (default out dir: ./build)

Inputs, all next to this file unless given:
  release/release.json   the release: name, texts, screens, checksums of the tested archive, the start line
  release/libxdjmod.so   the library (its md5 must match release.json)
  release/screens.json   the RX screens, already rendered (they go into the update files, so their pixels are part
                         of the published MD5s); --render-screens draws them again with Pillow and rewrites the file
  the updater script     ../device/update (public repo) or ../appupdate/update (development tree), or --updater
  core/, ui/, vendor/    the page's code; expected.json (published MD5s of the outputs, optional)

Writes <out>/xdjrx-mod-builder.html (with --test: xdjrx-mod-builder.test.html, which adds tests/browser_selftest.js)
and <out>/assets.js (the generated assets alone, for the jsc tests). Nothing of Pioneer's goes into either.
Needs Python 3 only. --render-screens also needs Pillow (its built-in Aileron font, public domain).
"""
import base64
import hashlib
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SCREEN_W, SCREEN_H = 640, 120	# the picture format of the update mode's `image` tool (covers Pioneer's dialog)
BG, FG = (8, 72, 120), (255, 255, 255)	# Pioneer's dialog blue, white text


def md5(b):
	return hashlib.md5(b).hexdigest()


def screen_bin(lines, preview=None):
	"""u16 width, u16 height, RGB565 little-endian; two centred lines."""
	from PIL import Image, ImageDraw, ImageFont
	img = Image.new('RGB', (SCREEN_W, SCREEN_H), BG)
	d = ImageDraw.Draw(img)
	font = ImageFont.load_default(30)
	for i, text in enumerate(lines):
		tw = d.textlength(text, font=font)
		if tw > SCREEN_W - 20:
			raise SystemExit('screen text too wide: %r' % text)
		d.text(((SCREEN_W - tw) / 2, 16 + i * 46), text, fill=FG, font=font)
	px = bytearray(struct.pack('<HH', SCREEN_W, SCREEN_H))
	raw = img.tobytes()
	for i in range(0, len(raw), 3):
		r, g, b = raw[i:i + 3]
		px += struct.pack('<H', (r >> 3) << 11 | (g >> 2) << 5 | b >> 3)
	if preview:
		img.save(preview)
	return bytes(px)


def deflate_b64(data):
	c = zlib.compressobj(9, zlib.DEFLATED, -15)
	return base64.b64encode(c.compress(data) + c.flush()).decode()


def csp_hash(text):
	return "'sha256-%s'" % base64.b64encode(hashlib.sha256(text.encode('utf-8')).digest()).decode()


def read(*parts, mode='r'):
	with open(os.path.join(HERE, *parts), mode) as f:
		return f.read()


def find_updater():
	for p in (os.path.join(HERE, '..', 'device', 'update'), os.path.join(HERE, '..', 'appupdate', 'update')):
		if os.path.exists(p):
			return p
	raise SystemExit('updater script not found; pass --updater <path>')


def main():
	args = sys.argv[1:]
	test = '--test' in args
	if test:
		args.remove('--test')
	render = '--render-screens' in args
	if render:
		args.remove('--render-screens')
	updater = None
	if '--updater' in args:
		i = args.index('--updater')
		updater = args[i + 1]
		del args[i:i + 2]
	out = os.path.abspath(args[0]) if args else os.path.join(HERE, 'build')
	os.makedirs(out, exist_ok=True)

	rel = json.loads(read('release', 'release.json'))
	lib = read('release', 'libxdjmod.so', mode='rb')
	if len(lib) != rel['lib']['size'] or md5(lib) != rel['lib']['md5']:
		raise SystemExit('release/libxdjmod.so does not match release.json (md5 %s)' % md5(lib))
	with open(updater or find_updater(), 'rb') as f:
		update_script = f.read()

	screens_path = os.path.join(HERE, 'release', 'screens.json')
	if render:
		os.makedirs(os.path.join(out, 'screens-preview'), exist_ok=True)
		stored = {'lines': rel['screens'], 'screens': {}}
		for group, items in rel['screens'].items():
			stored['screens'][group] = {}
			for name, lines in items.items():
				preview = os.path.join(out, 'screens-preview', '%s-%s.png' % (group, name))
				stored['screens'][group][name] = deflate_b64(screen_bin(lines, preview))
		with open(screens_path, 'w') as f:
			json.dump(stored, f, indent='\t', sort_keys=True)
			f.write('\n')
	with open(screens_path) as f:
		stored = json.load(f)
	if stored['lines'] != rel['screens']:
		raise SystemExit('release/screens.json was rendered from other texts than release.json; run with --render-screens')
	screens = stored['screens']

	exp = os.path.join(HERE, 'expected.json')
	expected = json.loads(read('expected.json')) if os.path.exists(exp) else {}
	release = {k: rel[k] for k in ('name', 'title', 'libName', 'testedAs', 'date', 'zipName', 'lib', 'modTarMd5',
								   'modArchive')}
	assets = {
		'release': release,
		'lib': base64.b64encode(lib).decode(),
		'updateScript': base64.b64encode(update_script).decode(),
		'aplOld': rel['aplOld'],
		'aplNew': rel['aplNew'],
		'screens': screens,
		'readme': read('ui', 'README-FIRST.txt'),
		'expected': expected,
	}
	assets_js = ('// generated by build.py: do not edit\n'
				 'var XRM = typeof XRM !== \'undefined\' ? XRM : {};\nXRM.assets = ' +
				 json.dumps(assets, separators=(',', ':')) + ';\n')
	with open(os.path.join(out, 'assets.js'), 'w') as f:
		f.write(assets_js)

	parts = [read('vendor', 'pako-2.1.0', 'pako.min.js')]
	parts += [read('core', f) for f in sorted(os.listdir(os.path.join(HERE, 'core'))) if f.endswith('.js')]
	parts += [assets_js, read('ui', 'app.js')]
	if test:
		parts.append(read('tests', 'browser_selftest.js'))
	script = '\n'.join(parts).replace('</script', '<\\/script')
	style = read('ui', 'style.css')
	csp = ("default-src 'none'; script-src %s; style-src %s; img-src data:; connect-src 'none'; "
		   "form-action 'none'; base-uri 'none'; object-src 'none'" % (csp_hash(script), csp_hash(style)))
	build_id = hashlib.sha256((script + style).encode()).hexdigest()[:12]
	src = rel.get('sourceUrl')
	source = ('Source code: <a class="link" href="%s" target="_blank" rel="noopener noreferrer">%s</a>.' % (src, src)
			  if src else 'The source code is in the release download.')
	page = read('ui', 'page.html')
	for k, v in {'{{CSP}}': csp, '{{NAME}}': rel['name'], '{{FW_URL}}': rel['fwUrl'], '{{GPL_URL}}': rel['gplUrl'],
				 '{{SOURCE}}': source, '{{BUILD_ID}}': build_id, '{{STYLE}}': style}.items():
		page = page.replace(k, v)
	page = page.replace('{{SCRIPT}}', script)	# last: the script itself may contain braces
	path = os.path.join(out, 'xdjrx-mod-builder.test.html' if test else 'xdjrx-mod-builder.html')
	with open(path, 'w', encoding='utf-8') as f:
		f.write(page)
	print('wrote %s (%d bytes, build %s, %s)' % (path, len(page.encode()), build_id,
		  'with published MD5s' if expected else 'no published MD5s yet'))


if __name__ == '__main__':
	main()
