// tests/test_launcher_music.py: the launcher page's music (launcher/ui/app.js, from "const TRACKS" to the end of
// audioTick) run in node with stand-ins for what a page has: Web Audio, fetch and <audio> record what they are asked and
// make no sound. The WAV files are written here, laid out as launcher/Core/GameFiles/Bwav.cs ToWav writes them; no game
// file is read. The page's code goes where the marker below is.
'use strict';
const assert = require('assert');

// ---- stand-ins ----
const FAKE = { files: {}, ctxs: [], audios: [], fetched: [], decodeFails: false, noWebAudio: false, oneShots: [] };
class Param { constructor() { this.value = 0; this.set = []; } setTargetAtTime(v, t, c) { assert.ok(c > 0 && c < .1, 'a short smoothing'); this.set.push(v); this.value = v; } }
class Ctx {
  constructor(opts) { this.opts = opts; this.state = 'running'; this.currentTime = 0; this.destination = { name: 'speakers' }; this.sources = []; this.resumes = 0; FAKE.ctxs.push(this); }
  resume() { this.resumes++; this.state = 'running'; return Promise.resolve(); }
  createGain() { return { gain: new Param(), to: [], connect(n) { this.to.push(n); return n; }, disconnect() { this.cut = true; } }; }
  createBufferSource() {
    const s = { loop: false, loopStart: 0, loopEnd: 0, starts: 0, stopAt: null, onended: null,
      connect(n) { this.to = n; return n; }, start() { this.starts++; }, stop(t) { this.stopAt = t; } };
    this.sources.push(s);
    return s;
  }
  decodeAudioData(ab, ok, no) {
    if (FAKE.decodeFails) { const e = new Error('cannot decode'); if (no) no(e); return Promise.reject(e); }
    const buf = { sampleRate: 48000, length: FAKE.frames, duration: FAKE.frames / 48000, numberOfChannels: 2 };
    if (ok) ok(buf);
    return Promise.resolve(buf);
  }
}
class FakeAudio {
  constructor(src) { this.src = src; this.loop = false; this.volume = 1; this.playing = false; FAKE.audios.push(this); }
  play() { this.playing = true; return Promise.resolve(); }
  pause() { this.playing = false; }
  removeAttribute(a) { if (a === 'src') this.src = ''; }
}
const window = { get AudioContext() { return FAKE.noWebAudio ? undefined : Ctx; } };
const Audio = FakeAudio;
const listeners = {};
function addEventListener(type, fn) { (listeners[type] = listeners[type] || []).push(fn); }
function fetch(url) {
  FAKE.fetched.push(url);
  const bytes = FAKE.files[url];
  return Promise.resolve({ ok: !!bytes, status: bytes ? 200 : 404, arrayBuffer: () => Promise.resolve(bytes.slice(0)) });
}
const HOST = 'win';
const S = { media: { music: true, v: 1 }, sound: { music: .4, sfx: .7, muted: false } };
function nowPlaying() {}
function afterOneShot(name) { FAKE.oneShots.push(name); }

/*@@APP_SOUND@@*/

// ---- WAV files as Bwav.ToWav writes them: 16-bit stereo, the samples at 44, a smpl chunk after them for a loop ----
function makeWav(frames, loop, rate = 48000) {
  const data = frames * 4, smpl = loop ? 68 : 0, b = new ArrayBuffer(44 + data + smpl), d = new DataView(b);
  const tag = (at, s) => { for (let i = 0; i < 4; i++) d.setUint8(at + i, s.charCodeAt(i)); };
  tag(0, 'RIFF'); d.setUint32(4, 36 + data + smpl, true); tag(8, 'WAVE');
  tag(12, 'fmt '); d.setUint32(16, 16, true); d.setUint16(20, 1, true); d.setUint16(22, 2, true); d.setUint32(24, rate, true);
  d.setUint32(28, rate * 4, true); d.setUint16(32, 4, true); d.setUint16(34, 16, true);
  tag(36, 'data'); d.setUint32(40, data, true);
  for (let i = 0; i < frames * 2; i++) d.setInt16(44 + 2 * i, (i * 37) % 2000 - 1000, true);
  if (loop) {
    const at = 44 + data;
    tag(at, 'smpl'); d.setUint32(at + 4, 60, true); d.setUint32(at + 16, Math.floor(1e9 / rate), true); d.setUint32(at + 20, 60, true);
    d.setUint32(at + 36, 1, true); d.setUint32(at + 52, loop[0], true); d.setUint32(at + 56, loop[1] - 1, true);
  }
  return b;
}

let T = performance.now();
function play(seconds) { for (let i = 0, n = Math.round(seconds * 60); i < n; i++) { T += 1000 / 60; audioTick(T); } }
const settle = async () => { for (let i = 0; i < 10; i++) await new Promise(r => setImmediate(r)); };
const near = (a, b, what) => assert.ok(Math.abs(a - b) < 1e-9, what + ': ' + a + ' and ' + b);

(async () => {
  FAKE.frames = 1000;
  // ---- the loop a file carries ----
  let lp = wavLoop(makeWav(1000, [300, 1000]));
  assert.deepStrictEqual(lp, { rate: 48000, frames: 1000, start: 300, end: 1000 }, 'the smpl loop, its end the sample after the last');
  assert.deepStrictEqual(wavLoop(makeWav(1000, null)), { rate: 48000, frames: 1000, start: 0, end: 1000 }, 'an older file: the whole file');
  assert.deepStrictEqual(wavLoop(makeWav(1000, [300, 1001])), { rate: 48000, frames: 1000, start: 0, end: 1000 }, 'a loop past the sound: the whole file');
  assert.deepStrictEqual(wavLoop(makeWav(1000, [1000, 1000])), { rate: 48000, frames: 1000, start: 0, end: 1000 }, 'a loop that is none: the whole file');
  assert.deepStrictEqual(wavLoop(makeWav(1000, [300, 1000], 44100)), { rate: 44100, frames: 1000, start: 300, end: 1000 }, 'another rate');
  assert.strictEqual(wavLoop(new ArrayBuffer(8)), null, 'too short');
  assert.strictEqual(wavLoop(new TextEncoder().encode('<html>not found</html>').buffer), null, 'not a WAV');
  const cut = makeWav(1000, [300, 1000]).slice(0, 44 + 4000 + 40);
  assert.deepStrictEqual(wavLoop(cut), { rate: 48000, frames: 1000, start: 0, end: 1000 }, 'a smpl chunk cut off');

  FAKE.files['audio/fairy.wav'] = makeWav(1000, [300, 1000]);
  FAKE.files['audio/temple.wav'] = makeWav(1000, [10, 1000]);
  FAKE.files['audio/saria.wav'] = makeWav(1000, null);
  FAKE.files['audio/itemget.wav'] = makeWav(1000, null);

  // ---- a track loops between its loop points, sample for sample, at the game's rate ----
  music('fairy');
  assert.strictEqual(FAKE.ctxs.length, 1, 'one AudioContext');
  const ctx = FAKE.ctxs[0];
  assert.deepStrictEqual(ctx.opts, { sampleRate: 48000 }, 'the context at the game\'s rate');
  play(.5);   // (still being read: the fade-in waits for it)
  await settle();
  assert.strictEqual(ctx.sources.length, 1, 'one source');
  let src = ctx.sources[0];
  assert.strictEqual(src.starts, 1, 'started once');
  assert.strictEqual(src.loop, true, 'looping');
  near(src.loopStart, 300 / 48000, 'loop start');
  near(src.loopEnd, 1000 / 48000, 'loop end');
  const fairy = live.find(m => m.name === 'fairy');
  assert.strictEqual(fairy.k, 0, 'the fade-in starts when the music does');
  assert.deepStrictEqual(src.to.to, [ctx.destination], 'through a gain to the speakers');
  play(1.5);
  near(fairy.g.gain.value, .4, 'faded in to the music volume (1.4 s)');
  S.sound.muted = true; play(.1); near(fairy.g.gain.value, 0, 'muted');
  S.sound.muted = false; S.sound.music = .8; play(.1); near(fairy.g.gain.value, .8, 'volume follows');
  S.sound.music = .4; play(.1);
  assert.strictEqual(FAKE.audios.length, 0, 'no <audio> element');
  assert.deepStrictEqual(FAKE.fetched, ['audio/fairy.wav'], 'read once');

  // ---- the same track again: nothing restarts; another: the first fades out whole, then stops ----
  music('fairy'); await settle();
  assert.strictEqual(ctx.sources.length, 1, 'the same track does not start again');
  music('temple', true, 1.4); await settle();
  assert.strictEqual(ctx.sources.length, 2, 'the next track');
  const temple = ctx.sources[1];
  near(temple.loopStart, 10 / 48000, 'its own loop');
  play(1.3);
  assert.strictEqual(src.stopAt, null, 'not cut during its fade-out');
  assert.ok(fairy.g.gain.value > 0, 'still sounding');
  play(.2);
  assert.ok(src.stopAt !== null, 'stopped once faded out');
  assert.ok(!live.includes(fairy), 'gone from the live list');
  assert.ok(!decoded.has('fairy') && decoded.has('temple'), 'its samples let go; the playing track kept');

  // ---- the item fanfare: once, at its full level from its first note, then what comes after it ----
  music('itemget', false, .3); await settle();
  const fan = ctx.sources[2];
  assert.strictEqual(fan.loop, false, 'plays once');
  const item = live.find(m => m.name === 'itemget');
  assert.strictEqual(item.k, 1, 'no fade-in on a one-shot');
  // (before any frame: the gain is set as the source starts, not ramped up from 0 on the next frame)
  near(item.g.gain.value, .4, 'full level from its first sample');
  assert.deepStrictEqual(item.g.gain.set, [], 'no ramp to it');
  play(.02);
  near(item.g.gain.value, .4, 'and kept');
  play(.3);
  assert.ok(temple.stopAt !== null, 'the track before it faded in .3 s');
  fan.onended(); play(.02);   // (played to its end: a source fires ended once, not again when stop() comes after)
  assert.deepStrictEqual(FAKE.oneShots, ['itemget'], 'what plays after the fanfare is asked for');
  assert.strictEqual(curTrack, null, 'nothing playing');
  assert.ok(!live.includes(item), 'the fanfare is gone');
  assert.strictEqual(item.g.cut, true, 'its gain let go of the speakers');

  // ---- a track faded out before it was read never starts ----
  music('saria'); music(null); play(.05); await settle();
  assert.strictEqual(ctx.sources.length, 3, 'never started');
  // ---- an older WAV, without a loop, loops whole ----
  music('saria'); await settle();
  const saria = ctx.sources[3];
  assert.strictEqual(saria.loop, true, 'loops');
  near(saria.loopStart, 0, 'from the start');
  near(saria.loopEnd, 1000 / 48000, 'to the end');
  play(1.5);
  music(null, true, 2.2); play(2.1);
  assert.strictEqual(saria.stopAt, null, 'still fading out (2.2 s, as for the game starting)');
  play(.2);
  assert.ok(saria.stopAt !== null && live.length === 0, 'stopped once faded out');

  // ---- a context the browser holds back starts on the first click or key ----
  actx.state = 'suspended';
  listeners.pointerdown.forEach(f => f({}));
  assert.strictEqual(actx.state, 'running', 'resumed by a click');
  actx.state = 'suspended'; const resumes = actx.resumes; play(.1);
  assert.strictEqual(actx.state, 'running', 'and again from the frame loop once a click or key has come');
  assert.strictEqual(actx.resumes, resumes + 1, 'asked once, not every frame');

  // ---- fallback: a file Web Audio cannot decode plays in an <audio> element, as before ----
  FAKE.decodeFails = true;
  music('fairy'); await settle();
  assert.strictEqual(FAKE.audios.length, 1, 'an <audio> element');
  const el = FAKE.audios[0];
  assert.strictEqual(el.src, 'audio/fairy.wav');
  assert.strictEqual(el.loop, true, 'looping whole');
  assert.ok(el.playing, 'playing');
  play(1.5); near(el.volume, .4, 'faded in');
  music(null); play(1.5);
  assert.ok(!el.playing && el.src === '', 'stopped and let go');
  FAKE.decodeFails = false;

  // ---- fallback: no Web Audio at all ----
  actxTried = false; actx = null; FAKE.noWebAudio = true;
  music('temple'); await settle();
  assert.strictEqual(FAKE.audios.length, 2, 'an <audio> element without Web Audio');
  assert.strictEqual(FAKE.audios[1].loop, true);
  music('itemget', false, .3); await settle();
  const once = FAKE.audios[2];
  assert.strictEqual(once.loop, false, 'the fanfare once');
  near(once.volume, .4, 'at its full level from the start');
  once.onended();
  assert.deepStrictEqual(FAKE.oneShots, ['itemget', 'itemget'], 'and what comes after it');

  // ---- music off (no game yet): nothing is read or played ----
  music(null); play(2);
  S.media.music = false; const before = FAKE.fetched.length;
  music('fairy'); await settle();
  assert.strictEqual(curTrack, null); assert.strictEqual(FAKE.fetched.length, before, 'nothing read without a game');

  console.log('launcher music: all checks passed');
})().catch(e => { console.error(e && e.stack || e); process.exit(1); });
