//
// Copyright (c) OpenIPC  https://openipc.org  MIT License
//
// refresh-test.js — the start page's script in jsdom (#142): a.html is the
// page, b.html the next one. Run by docker/test-e2e-nginx-map.sh.
//
'use strict';
const fs = require('fs');
const { JSDOM, VirtualConsole } = require('jsdom');

const a = fs.readFileSync('a.html', 'utf8');
const b = fs.readFileSync('b.html', 'utf8');
const c = fs.readFileSync('c.html', 'utf8');   // another camex version
let tick = null, reply = null, navigated = false, fails = 0;
const vc = new VirtualConsole();
vc.on('jsdomError', (e) => { if (/navigation/i.test(e.message)) navigated = true; });
const dom = new JSDOM(a, {
    url: 'http://cams.test/', runScripts: 'dangerously', virtualConsole: vc,
    beforeParse(w) {
        w.setInterval = (f, ms) => { if (ms === 5000) tick = f; return 1; };
        w.fetch = () => Promise.resolve(reply);
    },
});
const d = dom.window.document;
const check = (ok, what) => { console.log((ok ? 'ok: ' : 'FAIL: ') + what); if (!ok) fails++; };
const flush = () => new Promise((r) => setTimeout(r, 50));

(async () => {
    check(typeof tick === 'function', 'the script refreshes every 5 s');
    d.getElementById('tab-auto').checked = true;
    d.getElementById('tab-auto').dispatchEvent(new dom.window.Event('change', { bubbles: true }));
    reply = { ok: true, text: () => Promise.resolve(b) };
    tick(); await flush();
    check(d.querySelector('.tabs').getAttribute('data-t') === 'new' &&
          /99 online/.test(d.querySelector('.sub').textContent),
          'the lists and counts of the new page are in place, no reload');
    check(d.getElementById('tab-auto').checked, 'the chosen tab stays');
    check(!d.body.classList.contains('stale'), 'not marked stale');
    reply = { ok: false, status: 401, text: () => Promise.resolve('') };
    tick(); await flush();
    check(d.body.classList.contains('stale') && /99 online/.test(d.querySelector('.sub').textContent),
          'no answer: the data stays, marked "no connection"');
    reply = { ok: true, text: () => Promise.resolve(b) };
    tick(); await flush();
    check(!d.body.classList.contains('stale'), 'an answer again: the mark goes');
    reply = { ok: true, text: () => Promise.resolve(c) };
    tick(); await flush();
    check(navigated, 'another camex version: a full reload');
    // A link with nginx's HTTP port, the page opened over HTTPS (#141)
    const tlsPage = a.replace(/<\/tbody>/, '<tr><td><a href="http://cam9.cams.test:85/">x</a></td></tr></tbody>');
    const t = new JSDOM(tlsPage, { url: 'https://cams.test/', runScripts: 'dangerously',
        virtualConsole: new VirtualConsole(), beforeParse(w) { w.setInterval = () => 1; } });
    check(t.window.document.querySelector('a[href="https://cam9.cams.test/"]') !== null,
          'over HTTPS, a link with the HTTP port goes to https:// without it');
    process.exit(fails ? 1 : 0);
})();
