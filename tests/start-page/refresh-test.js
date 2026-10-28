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
let tick = null, reply = null, navigated = false, fails = 0, fetches = 0, timeout = null;
const vc = new VirtualConsole();
vc.on('jsdomError', (e) => { if (/navigation/i.test(e.message)) navigated = true; });
const dom = new JSDOM(a, {
    url: 'http://cams.test/', runScripts: 'dangerously', virtualConsole: vc,
    beforeParse(w) {
        w.setInterval = (f, ms) => { if (ms === 5000) tick = f; return 1; };
        w.fetch = () => { fetches++; return typeof reply === 'function' ? reply() : Promise.resolve(reply); };
        // The 4-second limit of a request: called by the test itself
        const st = w.setTimeout.bind(w);
        w.setTimeout = (f, ms) => { if (ms === 4000) { timeout = f; return 7; } return st(f, ms); };
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
    // The same page again: nothing rebuilt (a selection, the focus stay)
    d.querySelector('.tabs').setAttribute('data-k', 'kept');
    tick(); await flush();
    check(d.querySelector('.tabs').getAttribute('data-k') === 'kept', 'the same page again: the lists are not rebuilt');
    // A request that hangs: no second one meanwhile; after 4 s "no
    // connection", its late answer is dropped once a newer one went out
    let hung = null;
    reply = () => new Promise((res) => { hung = res; });
    fetches = 0;
    tick(); tick(); await flush();
    check(fetches === 1, 'one request at a time');
    timeout(); await flush();
    check(d.body.classList.contains('stale'), 'no answer in 4 s: marked "no connection"');
    reply = { ok: true, text: () => Promise.resolve(b.replace('99 online', '77 online')) };
    tick(); await flush();
    check(fetches === 2 && /77 online/.test(d.querySelector('.sub').textContent) && !d.body.classList.contains('stale'),
          'after the limit a new request goes out, its answer is shown');
    hung({ ok: true, text: () => Promise.resolve(b) }); await flush();
    check(/77 online/.test(d.querySelector('.sub').textContent), 'the late answer of the older request is dropped');
    // Past the limit and before the next tick: its late answer is dropped,
    // "no connection" stays
    reply = () => new Promise((res) => { hung = res; });
    tick(); await flush();
    timeout(); await flush();
    hung({ ok: true, text: () => Promise.resolve(b.replace('99 online', '55 online')) }); await flush();
    check(d.body.classList.contains('stale') && /77 online/.test(d.querySelector('.sub').textContent),
          'an answer after the 4-second limit is dropped, "no connection" stays');
    reply = { ok: true, text: () => Promise.resolve(c) };
    tick(); await flush();
    check(navigated, 'another camex version: a full reload');
    // A link with nginx's HTTP port, the page opened over HTTPS (#141)
    const tlsPage = a.replace(/<\/tbody>/, '<tr><td><a href="http://cam9.cams.test:85/">x</a></td>' +
        '<td><a class="peer" href="http://192.0.2.10:8080/">p</a></td></tr></tbody>');
    const t = new JSDOM(tlsPage, { url: 'https://cams.test/', runScripts: 'dangerously',
        virtualConsole: new VirtualConsole(), beforeParse(w) { w.setInterval = () => 1; } });
    check(t.window.document.querySelector('a[href="https://cam9.cams.test/"]') !== null,
          'over HTTPS, a link with the HTTP port goes to https:// without it');
    check(t.window.document.querySelector('a.peer[href="http://192.0.2.10:8080/"]') !== null,
          'over HTTPS, a peer\'s link to its own web port stays');
    process.exit(fails ? 1 : 0);
})();
