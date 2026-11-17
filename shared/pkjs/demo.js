// Offline demo backend: answers the Pebble Home API from memory so the
// emulator (and store screenshots) work without a Home Assistant.
var LABELS = { disarmed: 'Disarmed', armed_away: 'Armed away', armed_home: 'Armed home', armed_night: 'Armed night', pending: 'Entry delay', triggered: 'TRIGGERED' };

function Demo(cfg) {
  this.images = cfg.images || {};
  this.cams = cfg.cams || [];
  this.pinOn = !!cfg.pin;
  this.seq = Math.floor(Date.now() / 1000);
  this.s = {
    v: 1, alarm: 'disarmed', front: 'locked', open: ['Kitchen Bifolds'], events: [],
    media: [
      { id: 'media_player.kitchen', n: 'Kitchen', s: 'playing', t: 'Penny Lane', a: 'The Beatles', vol: 22, mute: 0, lead: 1, art: 'a1' },
      { id: 'media_player.lounge', n: 'Lounge', s: 'paused', t: 'Gold (Acoustic)', a: 'Kiiara', vol: 30, mute: 0, lead: 1, art: 'b2' },
      { id: 'media_player.garden', n: 'Garden', s: 'idle', t: '', a: '', vol: 60, mute: 0, lead: 1, art: '' },
      { id: 'media_player.bedroom', n: 'Bedroom', s: 'idle', t: '', a: '', vol: 20, mute: 0, lead: 1, art: '' },
      { id: 'media_player.study', n: 'Study', s: 'idle', t: '', a: '', vol: 20, mute: 0, lead: 1, art: '' },
      { id: 'media_player.loft', n: 'Loft', s: 'idle', t: '', a: '', vol: 20, mute: 0, lead: 1, art: '' },
      { id: 'media_player.bathroom', n: 'Bathroom', s: 'idle', t: '', a: '', vol: 20, mute: 0, lead: 1, art: '' },
      { id: 'media_player.shower', n: 'Shower', s: 'idle', t: '', a: '', vol: 20, mute: 0, lead: 1, art: '' },
      { id: 'media_player.utility_room', n: 'Utility Room', s: 'idle', t: '', a: '', vol: 20, mute: 0, lead: 1, art: '' },
      { id: 'media_player.studio', n: 'Studio', s: 'idle', t: '', a: '', vol: 20, mute: 0, lead: 1, art: '' },
      { id: 'media_player.garage', n: 'Garage', s: 'idle', t: '', a: '', vol: 20, mute: 0, lead: 1, art: '' }
    ]
  };
  var self = this;
  (cfg.script || []).forEach(function (step) {
    setTimeout(function () {
      if (step.arm) self.request('POST', '/action', { a: 'alarm', m: 'arm_' + step.arm }, function () {});
      else self.event(step.k, step.t, step.m, step.c, step.v);
    }, step.after * 1000);
  });
}

Demo.prototype.event = function (k, t, m, c, v) {
  this.s.events.push({ id: ++this.seq, ts: Math.floor(Date.now() / 1000), k: k, t: t, m: m, c: c, v: v, i: c ? 1 : 0 });
  this.s.v++;
};

Demo.prototype.state = function () {
  var s = this.s, cams = [];
  this.cams.forEach(function (n) { if (n !== 'art') cams.push({ id: 'camera.' + n, n: n.charAt(0).toUpperCase() + n.slice(1) }); });
  return {
    v: s.v, now: Math.floor(Date.now() / 1000),
    alarm: { s: s.alarm, l: s.alarm === 'arming' ? 'Arming ' + s.armTo : (LABELS[s.alarm] || s.alarm), r: true, by: 'Alex', t: 0,
             until: s.alarm === 'arming' ? s.armUntil : 0, to: s.armTo || '', f: 11 },
    ppl: [{ n: 'Alex', h: true }, { n: 'Sam', h: true }, { n: 'Jo', h: true }, { n: 'Max', h: false }],
    locks: [{ id: 'lock.front_door', n: 'Front door', s: s.front }],
    open: s.open, open_n: s.open.length,
    wx: { c: 'partlycloudy', t: 5, u: 'C' },
    st: [{ n: 'Solar', s: '0W' }, { n: 'Lounge', s: '20.5C' }, { n: 'Gas today', s: '4.2kWh' }],
    cams: cams,
    acts: [{ id: 'script.good_night', n: 'Good night' }, { id: 'scene.movie', n: 'Movie time' }],
    media: s.media, pin_lock: 0, pin: this.pinOn ? 1 : 0
  };
};

Demo.prototype.request = function (method, path, body, cb) {
  var self = this, s = this.s;
  var q = {};
  (path.split('?')[1] || '').split('&').forEach(function (kv) { var p = kv.split('='); q[p[0]] = decodeURIComponent(p[1] || ''); });
  var route = path.split('?')[0];
  setTimeout(function () {
    if (route === '/sync' || route === '/hello') {
      var e = parseInt(q.e || '0', 10);
      var evs = e ? s.events.filter(function (x) { return x.id > e; }) : [];
      return cb(null, { state: self.state(), events: evs.slice(-5) });
    }
    if (route === '/events') return cb(null, { events: s.events.slice().reverse() });
    if (route === '/image') {
      var key = q.camera ? q.camera.split('.')[1] : q.media ? 'art' : null;
      if (q.event) {
        var ev = s.events.filter(function (x) { return String(x.id) === q.event; })[0];
        key = ev && ev.c ? ev.c.split('.')[1] : null;
      }
      var img = self.images[key + '_' + q.w + 'x' + q.h] || self.images[key];
      return img ? cb(null, img) : cb(new Error('HTTP 404'));
    }
    if (route === '/action') {
      if (body.a === 'alarm') {
        if (body.m === 'disarm' && (self.pinOn || body.code) && body.code !== '1234') return cb(null, { ok: false, err: 'Wrong PIN' });
        if (body.m === 'disarm') {
          s.alarm = 'disarmed';
          clearTimeout(self.armTimer);
          self.event('alarm', 'Alarm disarmed', 'by Pebble', null, 'short');
        } else {
          var target = body.m.replace('arm_', 'armed_');
          s.alarm = 'arming';
          s.armTo = body.m.replace('arm_', '');
          s.armUntil = Math.floor(Date.now() / 1000) + 30;
          s.v++;
          self.armTimer = setTimeout(function () {
            s.alarm = target;
            s.armUntil = 0;
            self.event('alarm', 'Alarm ' + LABELS[target].toLowerCase(), 'by Pebble', null, 'short');
          }, 30000);
        }
      } else if (body.a === 'lock') {
        if (body.m === 'unlock' && (self.pinOn || body.code) && body.code !== '1234') return cb(null, { ok: false, err: 'Wrong PIN' });
        s.front = body.m === 'lock' ? 'locked' : 'unlocked';
        s.v++;
      } else if (body.a === 'media') {
        var m = s.media.filter(function (x) { return x.id === body.id; })[0];
        if (body.m === 'play_pause') m.s = m.s === 'playing' ? 'paused' : 'playing';
        if (body.m === 'vol_up') m.vol = Math.min(100, m.vol + 4);
        if (body.m === 'vol_down') m.vol = Math.max(0, m.vol - 4);
        if (body.m === 'next') { m.t = 'Strawberry Fields Forever'; m.art = 'c3'; }
        s.v++;
        return cb(null, { ok: true, quiet: true });
      } else if (body.a === 'assist') {
        return cb(null, { ok: true, speech: 'Done. The kitchen lights are on at 60 percent.' });
      }
      return cb(null, { ok: true });
    }
    cb(new Error('HTTP 404'));
  }, 120);
};

module.exports = Demo;
