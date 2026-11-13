#!/usr/bin/env python3
"""Open the web builder in headless Chrome (a throw-away profile) and check it from file://.

    chrome_check.py <xdjrx-mod-builder.html>

1. The real page: loads without console errors, the page's Content-Security-Policy is active (meta tag present
   in the DOM Chrome produced).
2. The test build (same page + tests/browser_selftest.js): the core works in the browser, and a fetch() to the
   internet is blocked by the CSP.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

BUILD_PY = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build.py')
CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'


def run_chrome(path, profile):
	cmd = [CHROME, '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
		   '--disable-extensions', '--user-data-dir=' + profile, '--enable-logging=stderr', '--v=0',
		   '--virtual-time-budget=8000', '--dump-dom', 'file://' + path]
	p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
	try:
		out, err = p.communicate(timeout=60)
	except subprocess.TimeoutExpired:
		p.kill()
		out, err = p.communicate()
	return out.decode('utf-8', 'replace'), err.decode('utf-8', 'replace')


def main():
	page = os.path.abspath(sys.argv[1])
	if not os.path.exists(CHROME):
		print('  skip: Google Chrome not installed')
		return
	failed = 0
	with tempfile.TemporaryDirectory() as tmp:
		dom, log = run_chrome(page, os.path.join(tmp, 'p1'))
		console = [l for l in log.splitlines() if 'CONSOLE' in l or 'Uncaught' in l]
		errors = [l for l in console if re.search(r'Uncaught|Refused to execute|Refused to apply|SyntaxError|TypeError', l)]
		ok = '<h1>' in dom and 'Content-Security-Policy' in dom and not errors
		print('  %s real page loads from file:// without script/CSP errors' % ('ok  ' if ok else 'FAIL'))
		for l in errors[:5]:
			print('       ' + l[:200])
		failed += not ok

		out = os.path.join(tmp, 'web')
		os.makedirs(out)
		subprocess.run([sys.executable, '-I', BUILD_PY, '--test', out],
					   check=True, capture_output=True)
		dom, log = run_chrome(os.path.join(out, 'xdjrx-mod-builder.test.html'), os.path.join(tmp, 'p2'))
		m = re.search(r'data-selftest="([^"]*)"', dom)
		res = m.group(1) if m else 'no result (script did not run?)'
		for item in res.split('; '):
			print('  %s browser %s' % ('ok  ' if item.startswith('ok') else 'FAIL', item))
			failed += not item.startswith('ok')
		csp = 'Refused to connect' in log or 'Content Security Policy' in log
		print('  %s Chrome reports the CSP refusal of the probe request' % ('ok  ' if csp else 'note'))
	print('\nchrome: %s' % ('all passed' if not failed else '%d failed' % failed))
	sys.exit(1 if failed else 0)


if __name__ == '__main__':
	main()
