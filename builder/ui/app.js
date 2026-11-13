// XDJRX-MOD web builder: the page. Everything happens in this tab; the page's Content-Security-Policy
// forbids any network connection.
// SPDX-License-Identifier: GPL-3.0-only
(function () {
	'use strict';
	const $ = (id) => document.getElementById(id);
	const B = XRM.build;
	const tick = () => new Promise((r) => setTimeout(r, 20));	// lets the page redraw between steps
	const have = { fw: null, gpl: null };	// { upd } / { keyFile }
	let state = null;

	function status(slot, cls, msg) {
		const li = $('st-' + slot);
		li.className = cls;
		li.querySelector('.msg').textContent = msg;
	}

	function other(msg) {
		const p = $('other');
		p.hidden = !msg;
		p.textContent = msg || '';
	}

	const mb = (n) => (n / 1048576).toFixed(1) + ' MB';

	async function addFiles(list) {
		other('');
		const notes = [];
		for (const file of list) {
			const head = new Uint8Array(await file.slice(0, B.HEAD_BYTES).arrayBuffer());
			const kind = B.identify(file.name, file.size, head);
			if (kind === 'fw-zip' || kind === 'fw-upd') {
				status('fw', 'busy', 'checking ' + file.name + ' …');
				await tick();
				try {
					const bytes = new Uint8Array(await file.arrayBuffer());
					have.fw = { upd: B.firmware(kind, bytes) };
					status('fw', 'ok', 'Pioneer\'s 2.21 update ✓');
				} catch (e) {
					have.fw = null;
					status('fw', 'bad', e.message);
				}
			} else if (kind === 'gpl-zip' || kind === 'gpl-raw') {
				status('gpl', 'busy', 'reading the key from ' + file.name + ' …');
				await tick();
				try {
					const prefix = new Uint8Array(await file.slice(0, B.GPL_READ).arrayBuffer());
					have.gpl = { keyFile: B.key(kind, prefix) };
					status('gpl', 'ok', 'update key found ✓');
				} catch (e) {
					have.gpl = null;
					status('gpl', 'bad', e.message);
				}
			} else if (kind === 'gpl-bsp') {
				notes.push(file.name + ' is the chip maker\'s Linux kit from the same list (L3.0.35_…_source), not the part with ' +
					'the key. On Pioneer\'s page, take pioneer_xdj_rx.tar.gz.00 (209,700 KB), near the end of the XDJ-RX ' +
					'Version 1.50 list.');
			} else if (kind === 'gpl-part01') {
				notes.push(file.name + ' is part .01 of the GPL package. Only part .00 is needed.');
			} else {
				notes.push(file.name + ' (' + mb(file.size) + ') is neither of the two files.');
			}
		}
		other(notes.join(' '));
		$('build').disabled = !(have.fw && have.gpl);
	}

	function download(bytes, name, type) {
		const url = URL.createObjectURL(new Blob([bytes], { type: type || 'application/octet-stream' }));
		const a = document.createElement('a');
		a.href = url;
		a.download = name;
		document.body.appendChild(a);
		a.click();
		a.remove();
		setTimeout(() => URL.revokeObjectURL(url), 60000);
	}

	async function saveToStick(bytes) {
		try {
			const h = await window.showSaveFilePicker({ suggestedName: 'XDJRX.UPD' });
			const w = await h.createWritable();
			await w.write(bytes);
			await w.close();
			return true;
		} catch (e) {
			if (e && e.name === 'AbortError') return false;
			alert('Saving failed: ' + (e && e.message ? e.message : e));
			return false;
		}
	}

	function showResult(key) {
		const card = document.querySelector('[data-out="' + key + '"]');
		const actions = card.querySelector('.actions');
		actions.textContent = '';
		const dl = document.createElement('button');
		dl.className = 'small';
		dl.textContent = 'Download XDJRX.UPD';
		dl.addEventListener('click', () => download(state.out[key], 'XDJRX.UPD'));
		actions.appendChild(dl);
		if (window.showSaveFilePicker) {
			const sv = document.createElement('button');
			sv.className = 'small';
			sv.textContent = 'Save to USB stick …';
			sv.title = 'Choose the stick\'s top level; the name stays XDJRX.UPD';
			sv.addEventListener('click', async () => {
				if (await saveToStick(state.out[key])) sv.textContent = 'Saved ✓';
			});
			actions.appendChild(sv);
		}
		const sum = card.querySelector('.sum');
		sum.textContent = 'MD5 ' + XRM.hash.md5(state.out[key]) + ' · ' + state.out[key].length.toLocaleString('en') + ' bytes ';
		const m = state.match[key];
		if (m !== null) {
			const span = document.createElement('span');
			span.className = m ? 'yes' : 'no';
			span.textContent = m ? '· same as the published build ✓' : '· differs from the published build';
			sum.appendChild(span);
		}
	}

	async function build() {
		$('build').disabled = true;
		$('build-error').hidden = true;
		$('results').hidden = true;
		const ol = $('steps');
		ol.textContent = '';
		const list = B.steps({ keyFile: have.gpl.keyFile, upd: have.fw.upd }, { r2Release: $('r2rel').value.trim() || '2.22' });
		for (const st of list) {
			const li = document.createElement('li');
			li.textContent = st.label + ' …';
			ol.appendChild(li);
			await tick();
			try {
				st.run();
			} catch (e) {
				li.className = 'failed';
				li.textContent = st.label + ': failed';
				$('build-error').textContent = e.message;
				$('build-error').hidden = false;
				$('build').disabled = false;
				return;
			}
			li.className = 'done';
			li.textContent = st.label;
		}
		state = list[0].state;
		for (const k of ['install', 'r1', 'r2']) showResult(k);
		$('results').hidden = false;
		$('build').disabled = false;
		$('results').scrollIntoView({ behavior: 'smooth', block: 'start' });
	}

	function wire() {
		const drop = $('drop');
		$('pick').addEventListener('change', (e) => { addFiles(Array.from(e.target.files)); e.target.value = ''; });
		['dragenter', 'dragover'].forEach((t) => drop.addEventListener(t, (e) => { e.preventDefault(); drop.classList.add('over'); }));
		['dragleave', 'drop'].forEach((t) => drop.addEventListener(t, () => drop.classList.remove('over')));
		drop.addEventListener('drop', (e) => { e.preventDefault(); addFiles(Array.from(e.dataTransfer.files)); });
		// a file dropped next to the drop zone would make the browser open it instead
		window.addEventListener('dragover', (e) => e.preventDefault());
		window.addEventListener('drop', (e) => e.preventDefault());
		$('build').addEventListener('click', build);
		$('zip').addEventListener('click', () => download(B.zipAll(state), XRM.assets.release.zipName, 'application/zip'));
		$('r2go').addEventListener('click', () => {
			if (!state) return;
			try {
				B.rebuildR2(state, $('r2rel').value.trim());
				showResult('r2');
			} catch (e) {
				alert(e.message);
			}
		});
	}

	wire();
})();
