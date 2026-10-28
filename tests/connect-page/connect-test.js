//
// Copyright (c) OpenIPC  https://openipc.org  MIT License
//
// connect-test.js — docs/connect/index.html in jsdom (#146): the button's
// intent URL, the table, the desktop command. Run by
// docker/test-connect-page.sh.
//
'use strict';
const fs = require('fs');
const { JSDOM, VirtualConsole } = require('jsdom');

const page = fs.readFileSync('index.html', 'utf8');
let fails = 0;
const check = (ok, what) => { console.log((ok ? 'ok: ' : 'FAIL: ') + what); if (!ok) fails++; };
const open = (hash) => new JSDOM(page, {
    url: 'https://openipc.github.io/camex/connect/' + hash, runScripts: 'dangerously',
    virtualConsole: new VirtualConsole(),
}).window.document;

const KEY = '+v/tp4H5gAJTw3y6yaU1zbS6a6fUDV+e5EI6oh7cTZk=';
const hash = '#h=vpn.example.org&p=5900&t=tcp&k=' + KEY + "&s=it's%20%26%20%2B";
let d = open(hash);
const btn = d.getElementById('open').getAttribute('href');
const enc = "h=vpn.example.org&p=5900&t=tcp&k=" + encodeURIComponent(KEY) + "&s=it's%20%26%20%2B";
check(btn === 'intent://connect?' + enc + '#Intent;scheme=camex;package=org.openipc.camex;' +
      'S.browser_fallback_url=https%3A%2F%2Fgithub.com%2FOpenIPC%2Fcamex%2Freleases%2Flatest%2Fdownload%2Fcamex-android.apk;end',
      'the button: the checked fields %XX-encoded, for camex://connect of the app');
const cells = [].map.call(d.querySelectorAll('#fields td'), (td) => td.textContent);
check(cells.join('|') === 'Server host|vpn.example.org|Port|5900|Transport|tcp|Server public key|' + KEY,
      'the table: host, port, transport, key; not the psk');
check(!d.getElementById('ok').hidden && d.getElementById('bad').hidden, 'the settings are shown');
check(d.getElementById('cli').textContent === "camex --mode client --auto --server-host 'vpn.example.org' " +
      "--port '5900' --transport tcp --server-pubkey '" + KEY + "' --psk '<psk>'",
      'the desktop command shown: the psk left out');
check(d.getElementById('cli-psk').textContent === "camex --mode client --auto --server-host 'vpn.example.org' " +
      "--port '5900' --transport tcp --server-pubkey '" + KEY + "' --psk 'it'\\''s & +'",
      'the command with the psk (behind "details"): decoded and quoted for the shell');
// A repeated field: the last one wins, as in the app
d = open('#h=x&p=1x&p=5900&k=' + KEY + '&s=y');
check(!d.getElementById('ok').hidden && /Port5900Server/.test(d.getElementById('fields').textContent) &&
      !/1x/.test(d.getElementById('open').getAttribute('href')),
      'a repeated field: the last one, the link not refused for the first');
// "#Intent;" in the psk stays inside the query
d = open('#h=x&k=' + KEY + '&s=a%23Intent%3Bscheme%3Dhttp%3Bend');
const b2 = d.getElementById('open').getAttribute('href');
check(b2.split('#Intent;').length === 2 && b2.indexOf('s=a%23Intent%3Bscheme%3Dhttp%3Bend#Intent;') > 0,
      'a psk with #Intent; cannot end the intent\'s query');
d = open('#h=x&k=' + KEY + '&s=y&m=1400&a=0&n=cam-1');
check(d.getElementById('cli-psk').textContent === "camex --mode client --auto --name 'cam-1' --server-host 'x' " +
      "--port '5800' --mtu 1400 --keepalive 0 --server-pubkey '" + KEY + "' --psk 'y'",
      'the desktop command has the name, MTU and keepalive of the link');
d = open('#h=x&k=' + KEY + '&s=y&m=1');
check(d.getElementById('ok').hidden && /malformed: MTU/.test(d.getElementById('malformed').textContent),
      'an MTU out of range: named as malformed, not shown as ready');
d = open('#h=x&k=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA%3D&s=hex%3Abad');
check(d.getElementById('ok').hidden &&
      /Server public key, Pre-shared key/.test(d.getElementById('malformed').textContent),
      'an all-zero key and a bad hex: psk: named as malformed');
d = open('#h=x&p=5800');
check(d.getElementById('ok').hidden && !d.getElementById('bad').hidden, 'no key or psk: "no camex settings"');
d = open('#h=x&k=' + KEY + '&s=%E0%A4');
check(d.getElementById('ok').hidden && !d.getElementById('bad').hidden, 'a malformed escape: "no camex settings"');
check(!/<script[^>]+src=|<link[^>]+href=|<img|fetch\(|XMLHttpRequest/.test(page),
      'the page loads nothing from anywhere');
process.exit(fails ? 1 : 0);
