// Phone-side bridge between the watch and the Pebble Home integration.
// Shared by the watchface and the watchapp; each passes its own options.
//
// Delivery model, shaped by iOS:
//  * The watch drives a short sync on a timer. Each AppMessage wakes the phone
//    app over Bluetooth, so this keeps working while the phone is locked.
//  * While the JS happens to be alive (phone app awake, emulator) a long-poll
//    gives instant updates on top.
//  * Each new event becomes a local timeline pin whose action reopens the app
//    on that snapshot; pins live on the watch, so history survives a sleeping
//    phone.

var CMD = {
  SYNC: 1, HELLO: 2, IMAGE: 3, ACTION: 4, EVENTS: 5,
  STATE: 10, EVENT: 11, IMG_BEGIN: 12, IMG_CHUNK: 13, IMG_END: 14,
  RESULT: 15, EVITEM: 16, ERROR: 17, NOCONFIG: 18, IMG_FAIL: 19
};

var CHUNK = 6000;
var FRESH_SECONDS = 180; // older events are history, not alerts
var DOOR_LIVE_SECONDS = 45; // a doorbell stays "ringing" this long

var opts = {};
var queue = [];
var sending = false;
var lastVersion = -1;
var lastEventId = 0;
var primed = false; // set after the first successful sync this session
var longPollOn = false;
var longPollGen = 0;
var imageToken = 0;

function log() {
  var a = Array.prototype.slice.call(arguments);
  console.log('[home] ' + a.join(' '));
}

// ------------------------------------------------------------------ config
function trimUrl(u) { return String(u || '').trim().replace(/\/+$/, ''); }

function config() {
  var url = trimUrl(localStorage.getItem('ha_url') || opts.devUrl);
  var remote = trimUrl(localStorage.getItem('ha_remote') || opts.devRemote);
  var key = localStorage.getItem('ha_key') || opts.devKey || '';
  return { url: url, remote: remote, key: key };
}

// Home first, Nabu Casa (or any remote URL) when home doesn't answer. The
// working base is sticky; while on remote, home is re-tried every 5 minutes.
var activeBase = null;
var remoteSince = 0;

function bases() {
  var c = config();
  var list = [];
  if (c.url) list.push(c.url);
  if (c.remote && c.remote !== c.url) list.push(c.remote);
  if (activeBase && list.indexOf(activeBase) > 0 && Date.now() - remoteSince < 300000) {
    list.splice(list.indexOf(activeBase), 1);
    list.unshift(activeBase);
  }
  return list;
}

function configured() {
  if (demo) return true;
  var c = config();
  return !!((c.url || c.remote) && c.key);
}

// --------------------------------------------------------------- transport
function send(dict, done) {
  queue.push({ d: dict, cb: done, tries: 0 });
  pump();
}

function pump() {
  if (sending || !queue.length) return;
  sending = true;
  var item = queue[0];
  Pebble.sendAppMessage(item.d, function () {
    queue.shift();
    sending = false;
    if (item.cb) item.cb(true);
    pump();
  }, function () {
    item.tries++;
    sending = false;
    if (item.tries >= 3) {
      queue.shift();
      if (item.cb) item.cb(false);
    }
    setTimeout(pump, 150 * item.tries);
  });
}

var demo = null;

function http(method, path, body, cb, asText) {
  if (demo) {
    demo.request(method, path, body, cb);
    return;
  }
  var c = config();
  var list = bases();
  if (!list.length || !c.key) {
    cb(new Error('noconfig'));
    return;
  }
  var longPollReq = path.indexOf('wait=') >= 0;
  attempt(0);

  function attempt(i) {
    var base = list[i];
    var last = i === list.length - 1;
    var done = false;
    var xhr = new XMLHttpRequest();
    // There is no xhr.timeout on the new phone app, so time out by hand. Home
    // gets a short fuse so leaving the house fails over quickly.
    // An action waits on the real outcome (a lock can take 20 s to give up),
    // and must not time out onto the next URL and run twice: on the URL that
    // is answering syncs it gets the long fuse.
    var slowAction = method === 'POST' && path === '/action' && (base === activeBase || last);
    var fuse = longPollReq || slowAction ? 40000 : (last ? 15000 : 4000);
    var timer = setTimeout(function () {
      if (done) return;
      done = true;
      try { xhr.abort(); } catch (e) { /* ignore */ }
      next(new Error('timeout'));
    }, fuse);
    function next(err) {
      clearTimeout(timer);
      if (!last) return attempt(i + 1);
      cb(err);
    }
    xhr.open(method, base + '/api/pebble_home/v1' + path, true);
    xhr.setRequestHeader('X-Pebble-Key', c.key);
    if (body) xhr.setRequestHeader('Content-Type', 'application/json');
    xhr.onload = function () {
      if (done) return;
      done = true;
      clearTimeout(timer);
      if (xhr.status === 200) {
        if (base !== activeBase) {
          log('using', base === c.url ? 'home' : 'remote', 'URL');
          if (base !== c.url) remoteSince = Date.now();
        }
        activeBase = base;
        if (asText) return cb(null, xhr.responseText);
        try { cb(null, JSON.parse(xhr.responseText)); } catch (e) { cb(e); }
      } else if (xhr.status === 401 || xhr.status === 403 || xhr.status === 404 && path.indexOf('/image') === 0) {
        cb(new Error('HTTP ' + xhr.status)); // a real answer: don't fail over
      } else {
        next(new Error('HTTP ' + xhr.status));
      }
    };
    xhr.onerror = function () {
      if (done) return;
      done = true;
      next(new Error('network'));
    };
    xhr.send(body ? JSON.stringify(body) : null);
  }
}

// ------------------------------------------------------------- formatting
function clean(s, max) {
  s = String(s === undefined || s === null ? '' : s).replace(/[|:\n]/g, ' ');
  return max ? s.substr(0, max) : s;
}

function packState(s) {
  var d = { cmd: CMD.STATE, ver: s.v, online: 1 };
  if (s.alarm) {
    d.aS = s.alarm.s;
    d.aL = clean(s.alarm.l, 18);
    d.aR = s.alarm.r ? 1 : 0;
    d.aBy = clean(s.alarm.by, 20);
    d.aT = s.alarm.t;
    // Seconds left on the exit/entry delay, worked out from HA's own clock so
    // the watch's clock doesn't matter.
    d.aRem = s.alarm.until ? Math.max(0, s.alarm.until - (s.now || Math.floor(Date.now() / 1000))) : 0;
    d.aTo = s.alarm.to || '';
    d.aF = s.alarm.f || 3;
  }
  d.ppl = (s.ppl || []).map(function (p) { return clean(p.n, 10) + ':' + (p.h ? 1 : 0); }).join('|');
  d.locks = (s.locks || []).map(function (l) { return l.id + ':' + clean(l.n, 16) + ':' + l.s; }).join('|');
  d.openN = s.open_n || 0;
  d.openL = (s.open || []).map(function (n) { return clean(n, 20); }).join('|');
  if (s.wx && s.wx.t !== null && s.wx.t !== undefined) {
    d.wxT = s.wx.t;
    d.wxC = clean(WX[s.wx.c] || s.wx.c, 20);
  }
  d.st = (s.st || []).map(function (x) { return clean(x.n, 18) + ':' + clean(x.s, 12); }).join('|');
  d.cams = (s.cams || []).map(function (c) { return c.id + ':' + clean(c.n, 16); }).join('|');
  d.acts = (s.acts || []).map(function (a) { return a.id + ':' + clean(a.n, 28); }).join('|');
  var media = s.media || [];
  d.media = media.map(function (m) {
    return m.id + ':' + clean(m.n, 14) + ':' + m.s + ':' + m.vol + ':' + m.mute + ':' + m.lead + ':' + clean(m.art || '-', 8);
  }).join('|');
  d.mediaT = media.map(function (m) { return clean(m.t, 40) + ':' + clean(m.a, 30); }).join('|');
  d.pinLock = s.pin_lock || 0;
  d.pinReq = (s.pin || localPin()) ? 1 : 0;
  // The watch keeps the Quick Launch choices so it can act on them before we're up.
  if (opts.name === 'app') d.ql = quickLaunch();
  return d;
}

function packEvent(e, cmd, alert) {
  return {
    cmd: cmd,
    eId: e.id,
    eTs: e.ts,
    eK: e.k,
    eT: clean(e.t, 32),
    eM: clean(e.m, 80),
    eC: e.c || '',
    eV: e.v || 'none',
    eI: e.i ? 1 : 0,
    eA: String(e.a || '').replace(/[\n:]/g, ' ').substr(0, 90),
    eAf: e.af || 0,
    eAlert: alert ? 1 : 0
  };
}

var WX = {
  'clear-night': 'Clear', cloudy: 'Cloudy', exceptional: 'Extreme', fog: 'Fog', hail: 'Hail',
  lightning: 'Lightning', 'lightning-rainy': 'Storms', partlycloudy: 'Partly cloudy', pouring: 'Pouring',
  rainy: 'Rain', snowy: 'Snow', 'snowy-rainy': 'Sleet', sunny: 'Sunny', windy: 'Windy', 'windy-variant': 'Windy'
};

// ---------------------------------------------------------------- timeline
var PIN_ICON = {
  doorbell: 'system://images/NOTIFICATION_FLAG',
  person: 'system://images/GENERIC_WARNING',
  package: 'system://images/GENERIC_CONFIRMATION',
  alarm: 'system://images/GENERIC_WARNING',
  alert: 'system://images/NOTIFICATION_GENERIC'
};
var PIN_COLOR = { doorbell: '#FFAA00', person: '#FF5500', package: '#55AAFF', alarm: '#FF0000', alert: '#00AAFF' };

function pinFor(e) {
  if (typeof Pebble.insertTimelinePin !== 'function') return;
  var icon = PIN_ICON[e.k] || PIN_ICON.alert;
  var pin = {
    id: 'home-' + e.id,
    time: new Date(e.ts * 1000).toISOString(),
    layout: {
      type: 'genericPin',
      title: e.t,
      subtitle: e.m || 'Home',
      body: e.m,
      tinyIcon: icon,
      backgroundColor: PIN_COLOR[e.k] || '#00AAFF'
    },
    actions: [{ title: e.i ? 'View snapshot' : 'Open Home', type: 'openWatchApp', launchCode: e.id % 2147483647 }]
  };
  // Notification buttons ride along as pin actions. A launch code is 32 bits:
  // top bit set, the event id's low 28 bits, then the button number.
  (e.a ? e.a.split('|') : []).slice(0, 4).forEach(function (title, i) {
    pin.actions.push({
      title: title,
      type: 'openWatchApp',
      launchCode: ((0x80000000 | ((e.id & 0x0FFFFFFF) << 3) | (i + 1)) >>> 0)
    });
  });
  try {
    Pebble.insertTimelinePin(pin);
  } catch (err) {
    log('pin failed', err);
  }
}

// --------------------------------------------------------------------- sync
function handleSync(res, fromWatch) {
  var s = res.state;
  if (s && (s.v !== lastVersion || fromWatch === 'force')) {
    lastVersion = s.v;
    send(packState(s));
  }
  var evs = res.events || [];
  var now = Date.now() / 1000;
  // First contact of a fresh install: take only the newest event, silently.
  var first = !primed && lastEventId === 0;
  var sessionFirst = !primed;
  primed = true;
  // The app opened (say by Quick Launch) while the doorbell is still ringing,
  // but this app's phone side already saw that event: hand it over again so the
  // watch can show the door and unlock. Quietly: it already buzzed.
  if (sessionFirst && opts.replayFresh && evs.length) {
    var newest = evs[evs.length - 1];
    var age = Date.now() / 1000 - newest.ts;
    if (newest.id <= lastEventId && newest.k === 'doorbell' && age < DOOR_LIVE_SECONDS) {
      var again = packEvent(newest, CMD.EVENT, true);
      again.eV = 'none';
      send(again);
    }
  }
  if (first && evs.length) evs = evs.slice(-1);
  for (var i = 0; i < evs.length; i++) {
    var e = evs[i];
    if (e.id <= lastEventId) continue;
    lastEventId = e.id;
    localStorage.setItem('last_event', String(e.id));
    var fresh = now - e.ts < FRESH_SECONDS;
    // On first contact replay only the newest event, silently.
    // A doorbell is over 45 s after the ring, wherever the watch was meanwhile.
    var rung = e.k === 'doorbell' && now - e.ts >= DOOR_LIVE_SECONDS;
    var alert = !first && fresh && !rung && e.v !== 'none';
    send(packEvent(e, CMD.EVENT, alert));
    if (!first && opts.pins !== false) pinFor(e);
  }
  if (!evs.length && fromWatch && s && s.v === lastVersion) {
    send({ cmd: CMD.STATE, online: 1, ver: s.v }); // heartbeat: still connected
  }
}

function sync(force) {
  if (!configured()) {
    send({ cmd: CMD.NOCONFIG });
    return;
  }
  var since = (!primed && opts.replayFresh) ? 1 : (lastEventId || 1);
  http('GET', '/sync?v=' + (force ? -1 : lastVersion) + '&e=' + since, null, function (err, res) {
    if (err) {
      log('sync failed', err.message);
      send({ cmd: CMD.ERROR, msg: err.message === 'noconfig' ? 'Set up in phone app' : 'HA offline' });
      return;
    }
    handleSync(res, force ? 'force' : true);
  });
}

function longPoll(gen) {
  if (!longPollOn || gen !== longPollGen || !configured()) return;
  http('GET', '/sync?wait=25&v=' + lastVersion + '&e=' + (lastEventId || 1), null, function (err, res) {
    if (gen !== longPollGen) return;
    if (err) {
      setTimeout(function () { longPoll(gen); }, 5000);
      return;
    }
    handleSync(res, false);
    setTimeout(function () { longPoll(gen); }, demo ? 2000 : 200);
  });
}

function startLongPoll() {
  longPollOn = true;
  longPollGen++;
  longPoll(longPollGen);
}

// -------------------------------------------------------------------- image
function b64ToBytes(b64) {
  var map = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  var lut = {};
  for (var i = 0; i < map.length; i++) lut[map.charAt(i)] = i;
  b64 = b64.replace(/[^A-Za-z0-9+/]/g, '');
  var out = [];
  for (var j = 0; j + 3 < b64.length + 3; j += 4) {
    var a = lut[b64.charAt(j)], b = lut[b64.charAt(j + 1)];
    var c = lut[b64.charAt(j + 2)], d = lut[b64.charAt(j + 3)];
    if (a === undefined || b === undefined) break;
    out.push((a << 2) | (b >> 4));
    if (c !== undefined) out.push(((b & 15) << 4) | (c >> 2));
    if (d !== undefined) out.push(((c & 3) << 6) | d);
  }
  return out;
}

// Live camera: while one frame is being sent over Bluetooth, fetch the next one,
// so the watch's next request is answered from memory instead of a round trip.
var nextFrame = null; // { q, at, text, err, waiters }

function fetchFrame(q, cb) {
  if (nextFrame && nextFrame.q === q && Date.now() - nextFrame.at < 10000) {
    var f = nextFrame;
    nextFrame = null;
    if (f.done) return cb(f.err, f.text);
    f.waiters.push(cb);
    nextFrame = null;
    return;
  }
  http('GET', '/image' + q, null, cb, true);
}

function prefetchFrame(q) {
  var f = { q: q, at: Date.now(), done: false, waiters: [] };
  nextFrame = f;
  http('GET', '/image' + q, null, function (err, text) {
    f.done = true;
    f.err = err;
    f.text = text;
    f.waiters.forEach(function (w) { w(err, text); });
    f.waiters = [];
  }, true);
}

function sendImage(p) {
  var token = ++imageToken;
  var q = '?w=' + (p.rW || 260) + '&h=' + (p.rH || 260) + '&bpp=' + (p.rBpp || 4) + '&b64=1&fit=' + (p.rFit || 'auto');
  var live = !p.rEv && !p.rMedia;
  if (p.rEv) q += '&event=' + p.rEv;
  else if (p.rMedia) q += '&media=' + encodeURIComponent(p.rMedia);
  else q += '&camera=' + encodeURIComponent(p.rCam);
  (live ? fetchFrame : function (qq, cb) { http('GET', '/image' + qq, null, cb, true); })(q, function (err, text) {
    if (token !== imageToken) return; // superseded by a newer request
    if (err) {
      // Say what's actually wrong rather than an HTTP code.
      var why = err.message;
      if (why === 'HTTP 404') why = p.rEv ? 'Snapshot expired' : p.rMedia ? 'No artwork' : 'Camera offline';
      else if (why === 'timeout' || why === 'network') why = 'HA unreachable';
      send({ cmd: CMD.IMG_FAIL, iTok: p.iTok || 0, msg: why });
      return;
    }
    if (live) prefetchFrame(q);
    var blob = b64ToBytes(text);
    if (blob[0] !== 80 || blob[1] !== 66) {
      send({ cmd: CMD.IMG_FAIL, iTok: p.iTok || 0, msg: 'bad image' });
      return;
    }
    var bpp = blob[3];
    var w = blob[4] | (blob[5] << 8);
    var h = blob[6] | (blob[7] << 8);
    var stride = blob[8] | (blob[9] << 8);
    var n = blob[10];
    var pal = blob.slice(11, 11 + n);
    var data = blob.slice(11 + n);
    var tok = p.iTok || 0;
    send({ cmd: CMD.IMG_BEGIN, iTok: tok, iW: w, iH: h, iBpp: bpp, iStride: stride, iPal: pal.length ? pal : [0], iTotal: data.length });
    for (var off = 0; off < data.length; off += CHUNK) {
      if (token !== imageToken) return;
      send({ cmd: CMD.IMG_CHUNK, iTok: tok, iOff: off, iData: data.slice(off, off + CHUNK) });
    }
    send({ cmd: CMD.IMG_END, iTok: tok });
  });
}

// ------------------------------------------------------------------ actions
function localPin() { return localStorage.getItem('pin') || ''; }

// Quick Launch: one digit per slot (Tap Up, Tap Down, Hold Up, Hold Center,
// Hold Down, Hold Back) naming the screen it opens; 0 is the main menu.
var QL_SLOTS = 6;
function quickLaunch() {
  var q = String(localStorage.getItem('quick_launch') || '').replace(/[^0-9]/g, '');
  while (q.length < QL_SLOTS) q += '0';
  return q.substr(0, QL_SLOTS);
}
function saveQuickLaunch(q) {
  if (typeof q === 'string' && /^[0-9]{1,6}$/.test(q)) localStorage.setItem('quick_launch', q);
}

// A PIN set in the phone settings guards disarm and unlock on the phone side,
// with its own escalating lockout. A PIN set in Home Assistant is checked there.
function localPinOk(code) {
  var pin = localPin();
  if (!pin) return true;
  var until = parseInt(localStorage.getItem('pin_until') || '0', 10);
  if (Date.now() < until) return 'Locked, try later';
  if (code === pin) {
    localStorage.setItem('pin_fail', '0');
    return true;
  }
  var n = parseInt(localStorage.getItem('pin_fail') || '0', 10) + 1;
  localStorage.setItem('pin_fail', String(n));
  if (n >= 3) localStorage.setItem('pin_until', String(Date.now() + 30000 * Math.pow(2, n - 3)));
  return 'Wrong PIN';
}

function action(p) {
  var guarded = (p.xK === 'alarm' && p.xM === 'disarm') || (p.xK === 'lock' && p.xM !== 'lock') ||
    (p.xK === 'notify' && p.xText === 'guard');
  if (guarded) {
    var ok = localPinOk(p.xCode || '');
    if (ok !== true) {
      send({ cmd: CMD.RESULT, ok: 0, msg: ok });
      return;
    }
  }
  var body = { a: p.xK };
  if (p.xM) body.m = p.xM;
  if (p.xId) body.id = p.xId;
  if (p.xCode) body.code = p.xCode;
  if (p.xText && p.xK !== 'notify') body.text = p.xText;
  if (p.xK === 'notify') { body.id = parseInt(p.xId, 10); body.m = parseInt(p.xM, 10); }
  var cid = localStorage.getItem('assist_cid');
  if (p.xK === 'assist' && cid) body.cid = cid;
  http('POST', '/action', body, function (err, res) {
    if (err) {
      send({ cmd: CMD.RESULT, ok: 0, msg: 'Failed: ' + err.message });
      return;
    }
    if (res.cid) localStorage.setItem('assist_cid', res.cid);
    if (res.quiet && res.ok) { setTimeout(function () { sync(false); }, 400); return; }
    send({ cmd: CMD.RESULT, ok: res.ok ? 1 : 0, msg: clean(res.speech || res.err || (res.ok ? 'Done' : 'Failed'), 400) });
    setTimeout(function () { sync(false); }, 800);
    setTimeout(function () { sync(false); }, 2500);
  });
}

function eventList() {
  http('GET', '/events?n=15', null, function (err, res) {
    if (err) return send({ cmd: CMD.ERROR, msg: 'HA offline' });
    var evs = res.events || [];
    if (!evs.length) send({ cmd: CMD.EVITEM, eIdx: 0, eCount: 0 });
    evs.forEach(function (e, i) {
      var d = packEvent(e, CMD.EVITEM, false);
      d.eIdx = i;
      d.eCount = evs.length;
      send(d);
    });
  });
}

function hello(p) {
  http('POST', '/hello', {
    bat: p.bat, chg: p.chg ? 1 : 0, app: opts.name,
    model: opts.model, fw: opts.fw
  }, function (err, res) {
    if (!err && res) handleSync(res, 'force');
  });
}

// ------------------------------------------------------------------- wiring
function init(options) {
  opts = options || {};
  lastEventId = parseInt(localStorage.getItem('last_event') || '0', 10) || 0;
  if (opts.demo) {
    var Demo = require('./demo');
    demo = new Demo(opts.demo);
    lastEventId = 0;
  }

  Pebble.addEventListener('ready', function () {
    log('ready', opts.name, configured() ? 'configured' : 'not configured');
    try {
      var info = Pebble.getActiveWatchInfo && Pebble.getActiveWatchInfo();
      if (info) {
        opts.model = info.model;
        opts.fw = info.firmware && (info.firmware.major + '.' + info.firmware.minor + '.' + info.firmware.patch);
      }
    } catch (e) { /* not all runtimes */ }
    if (!configured()) send({ cmd: CMD.NOCONFIG });
    else sync(true);
    if (opts.longPoll !== false) startLongPoll();
  });

  Pebble.addEventListener('appmessage', function (e) {
    var p = e.payload;
    switch (p.cmd) {
      case CMD.SYNC: sync(!!p.seq); break;
      case CMD.HELLO: hello(p); break;
      case CMD.IMAGE: sendImage(p); break;
      case CMD.ACTION: action(p); break;
      case CMD.EVENTS: eventList(); break;
    }
  });

  // Bundled settings page (configPage) talks to us over configmessage.
  Pebble.addEventListener('configmessage', function (e) {
    var m = e.message || e.data || {};
    if (m.type === 'get') {
      var c = config();
      e.respond({ url: c.url, remote: c.remote, hasKey: !!c.key, hasPin: !!localPin(),
                  app: opts.name === 'app', ql: quickLaunch() });
    } else if (m.type === 'save') {
      if (m.url) localStorage.setItem('ha_url', String(m.url).trim());
      if (m.remote !== undefined) localStorage.setItem('ha_remote', String(m.remote || '').trim());
      activeBase = null;
      if (m.key) localStorage.setItem('ha_key', String(m.key).trim());
      saveQuickLaunch(m.ql);
      if (m.pin !== undefined && m.pin !== null && m.pin !== '__keep__') {
        var pin = String(m.pin).replace(/\D/g, '').substr(0, 4);
        if (pin.length === 4 || pin === '') localStorage.setItem('pin', pin);
      }
      lastVersion = -1;
      http('GET', '/sync?v=-1', null, function (err, res) {
        if (err) return e.respond({ ok: false, err: err.message });
        handleSync(res, 'force');
        e.respond({ ok: true, alarm: res.state && res.state.alarm && res.state.alarm.l });
      });
      startLongPoll();
    }
  });

  // Legacy settings flow (older phone apps and the emulator).
  Pebble.addEventListener('showConfiguration', function () {
    var c = config();
    Pebble.openURL(opts.legacyConfigUrl + '#' + encodeURIComponent(JSON.stringify({
      url: c.url, remote: c.remote, app: opts.name === 'app', ql: quickLaunch() })));
  });
  Pebble.addEventListener('webviewclosed', function (e) {
    if (!e || !e.response) return;
    try {
      var r = JSON.parse(decodeURIComponent(e.response));
      if (r.url) localStorage.setItem('ha_url', r.url);
      if (r.remote !== undefined) localStorage.setItem('ha_remote', r.remote || '');
      activeBase = null;
      if (r.key) localStorage.setItem('ha_key', r.key);
      saveQuickLaunch(r.ql);
      if (typeof r.pin === 'string' && r.pin !== '__keep__') localStorage.setItem('pin', r.pin.replace(/\D/g, '').substr(0, 4));
      lastVersion = -1;
      sync(true);
    } catch (err) { log('bad config', err); }
  });
}

module.exports = { init: init, CMD: CMD };
