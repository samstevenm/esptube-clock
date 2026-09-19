// Unit tests for the helper's pure / mockable modules. Extracts every `/* @module name */ … /* @end */`
// region from tools/helper.html and runs it under node — no browser, no clock.   node test/helper-unit.mjs
import fs from 'node:fs'; import path from 'node:path'; import { fileURLToPath } from 'node:url'; import vm from 'node:vm';
const here = path.dirname(fileURLToPath(import.meta.url));
const html = fs.readFileSync(path.join(here, '..', 'tools', 'helper.html'), 'utf8');
const mods = {}; for (const m of html.matchAll(/\/\* @module (\w+)[\s\S]*?\*\/([\s\S]*?)\/\* @end \*\//g)) mods[m[1]] = m[2];
let fails = 0, checks = 0; const ok = (c, name, detail = '') => { checks++; if (!c) { fails++; console.log('  FAIL ', name, detail); } };
const load = (names, sandbox) => { const ctx = vm.createContext(sandbox); const exportsOf = { wire: 'WIRE', outbox: 'Outbox', layout: 'Layout', zip: 'ZIP', usd: 'USD', dock: 'DockModel' };
  for (const n of names) vm.runInContext(mods[n] + `\n;globalThis.${exportsOf[n]}=${exportsOf[n]};`, ctx, { filename: n + '.js' }); return ctx; };
console.log('modules found:', Object.keys(mods).join(', '));

// ---------------------------------------------------------------- wire
{ const { WIRE } = load(['wire'], { console });
  ok(WIRE.crc16(new TextEncoder().encode('123456789')) === 0x29B1, 'crc16 check value');
  ok(WIRE.succ(255) === 1 && WIRE.succ(1) === 2, 'epoch successor wraps to 1');
  let rng = 12345; const rnd = () => (rng = (rng * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
  for (let t = 0; t < 1500; t++) { const n = Math.floor(rnd() * 1400); const a = new Uint8Array(n).map(() => (rnd() < 0.3 ? 0 : Math.floor(rnd() * 256))); if (t % 7 === 0) a.fill(0x11);
    const e = WIRE.cobsEnc(a), d = WIRE.cobsDec(e); ok(!e.includes(0) && d && d.length === n && d.every((x, i) => x === a[i]), 'cobs round trip', 'n=' + n); }
  const fr = WIRE.frame(WIRE.OP.BLIT, 3, { y: 32, h: 16, fmt: 1, seq: 9, gen: 7, flags: WIRE.F_V11 | WIRE.F_NOACK }, Uint8Array.of(1, 2, 3));
  ok(fr.length === 23 && fr[0] === 0xE5 && fr[1] === 0x7B && fr[2] === 1 && fr[3] === 3 && fr[6] === 32 && fr[8] === 135 && fr[10] === 16 && fr[12] === 1 && fr[13] === 9 && fr[14] === 7 && fr[15] === 0x81 && fr[16] === 3, 'frame header layout');
  const dg = WIRE.datagram(fr); ok(dg[0] === 0 && dg[dg.length - 1] === 0 && !dg.subarray(1, dg.length - 1).includes(0), 'datagram is 00 <cobs> 00');
  // replies interleaved with text, split across chunks, one corrupted, one opener lost
  const ack = (seq, st, ep) => { const r = Uint8Array.of(0xA5, seq, st, ep), c = WIRE.crc16(r), raw = Uint8Array.of(...r, c >> 8, c & 255), e = WIRE.cobsEnc(raw); return Uint8Array.of(0, ...e, 0); };
  const te = s => new TextEncoder().encode(s); const cat = (...p) => Uint8Array.from(p.flatMap(x => [...x]));
  const bad = ack(3, 0, 5); bad[3] ^= 0x40;
  const stream = cat(te('boot log\n'), ack(1, 0, 5), te('OK cleared\n'), ack(2, 4, 6), bad, te('tail\n'), ack(4, 0, 6).subarray(1), ack(5, 0, 6));
  const st = { inDg: false, buf: [] }; const got = [], text = [];
  for (let i = 0; i < stream.length; i += 5) { const r = WIRE.parseReplies(st, stream.subarray(i, i + 5)); got.push(...r.replies.map(x => [...x])); text.push(...r.text); }
  ok(JSON.stringify(got.map(g => g[1])) === JSON.stringify([1, 2, 5]), 'framed acks found across chunk boundaries; corrupt + opener-less ones dropped', JSON.stringify(got));
  ok(new TextDecoder().decode(Uint8Array.from(text)).includes('boot log') && new TextDecoder().decode(Uint8Array.from(text)).includes('OK cleared'), 'text survives around replies');
  // THE console bug: one stray 0x00 (reset glitch) or a legacy ack (A5 seq 00 00) must never swallow the text after it
  { const st2 = { inDg: false, buf: [] }; const dec = b => new TextDecoder().decode(b); let shown = '';
    for (const chunk of [Uint8Array.of(0), te('OK preset 0\n'), Uint8Array.of(0xA5, 7, 0, 0), te('fps=0.0 kbps=0\n'), Uint8Array.of(0, 0, 0), te('status ok\n'), ack(9, 0, 3), te('after a real ack\n')]) { const r = WIRE.parseReplies(st2, chunk); shown += dec(r.text); if (r.replies.length) shown += '<ack ' + r.replies[0][1] + '>'; }
    ok(shown.includes('OK preset 0\n') && shown.includes('fps=0.0 kbps=0\n') && shown.includes('status ok\n') && shown.includes('<ack 9>') && shown.includes('after a real ack\n'), 'stray zeros / legacy acks never hold text back', JSON.stringify(shown)); }
  // dirty blocks + lossless band encoding
  const W = 135, H = 240, A = new Uint8Array(W * H * 2), B = new Uint8Array(W * H * 2); B[(40 * W + 3) * 2] = 9; B[(200 * W) * 2 + 1] = 7;
  ok(JSON.stringify(WIRE.dirtyBands(A, A)) === '[]', 'identical frames: nothing to send');
  ok(JSON.stringify(WIRE.dirtyBands(A, B)) === '[[32,47],[192,207]]', 'two dirty 16-row blocks', JSON.stringify(WIRE.dirtyBands(A, B)));
  ok(JSON.stringify(WIRE.dirtyBands(null, B)) === '[[0,239]]', 'no baseline: the whole tile');
  B.fill(1, 48 * W * 2, 64 * W * 2); ok(JSON.stringify(WIRE.dirtyBands(A, B)) === '[[32,63],[192,207]]', 'adjacent dirty blocks merge');
  const e1 = WIRE.encodeBand(A, 0, 239); ok(e1.fmt === 1 && e1.payload.length < 2000, 'flat tile -> RLE565', e1.payload.length);
  const N = new Uint8Array(W * H * 2).map(() => Math.floor(rnd() * 256)); const e2 = WIRE.encodeBand(N, 16, 31); ok(e2.fmt === 0 && e2.payload.length === 16 * W * 2 && e2.y === 16 && e2.h === 16, 'noise -> RAW565 band');
  // band splitting keeps every piece a valid self-contained BLIT
  const lenOk = (f, w, h, n) => f === 0 ? n === w * h * 2 : f === 1 ? (n % 3 === 0 && n >= 3) : f === 2 ? n === 32 + ((w + 1) >> 1) * h : n === 32 + ((w + 1) >> 1) * ((h + 1) >> 1);
  const rawParts = WIRE.splitBands(0, N, 0, H); ok(rawParts.every(p => lenOk(0, W, p.h, p.payload.length) && p.payload.length <= 1400) && rawParts.reduce((a, p) => a + p.h, 0) === H && rawParts[1].y === rawParts[0].h, 'RAW split by rows', rawParts.length);
  const pal = new Uint8Array(32 + 68 * 120).map((_, i) => i & 255); const pp = WIRE.splitBands(3, pal, 0, 240);
  ok(pp.length === 8 && pp.every(p => lenOk(3, W, p.h, p.payload.length)) && pp[7].y === 224 && pp[7].h === 16 && pp.reduce((a, p) => a + p.h, 0) === 240, 'PAL4x2 split: 16 payload rows + palette each', pp.map(p => p.h).join());
  const rle = WIRE.rle565(N); const rp = WIRE.splitBands(1, rle, 8, H); let px = 0; for (const p of rp) { let c = 0; for (let i = 0; i < p.payload.length; i += 3) c += p.payload[i]; ok(c === p.h * W, 'RLE band covers exactly its rows', c + ' vs ' + p.h * W); px += c; }
  ok(px === W * H && rp[0].y === 8 && rp.every(p => p.payload.length < 1700), 'RLE split covers the tile', rp.length); }

// ---------------------------------------------------------------- outbox (mock links)
{ const sent = []; let failNext = null, staleNext = false; const sleep = ms => new Promise(r => setTimeout(r, ms));
  const sandbox = { console, performance, setTimeout, clearTimeout, Promise, Map, Set, Object, Math, Date, Uint8Array, DataView, Error,
    state: { paused: false }, _pausedToastAt: 0, toast() {}, LIVE: { last: { 1: 'x' } }, SER: { dgram: true }, base: () => 'http://clock',
    serialUsable: () => true, linkPolicy: () => 'serial', postBare: async p => { sent.push(['rest', p]); }, Transport: { full: async () => {}, rect: async () => {} },
    serAckWait: () => Promise.resolve(), serWrite: async b => { sent.push(['cancel', b.length]); },
    serSendBand: async (tube, e, isStale) => { await sleep(20); if (isStale()) throw Object.assign(new Error('superseded'), { superseded: true });
      if (staleNext) { staleNext = false; throw Object.assign(new Error('stale'), { stale: true }); } if (failNext) { const f = failNext; failNext = null; throw new Error(f); } sent.push([tube, e.y, e.h, e.fmt]); return e.payload.length; } };
  const { Outbox } = load(['wire', 'outbox'], sandbox); const W = 135, H = 240; const tile = v => new Uint8Array(W * H * 2).fill(v);
  const full = (list, tube) => { const b = list.filter(x => x[0] === tube); return b.length && b[0][1] === 0 && b.reduce((a, x) => a + x[2], 0) === H && b.every(x => x[2] <= 64); };   // a whole tile = bands of <= 64 rows covering all 240
  let r = await Outbox.submit(1, tile(1)); ok(!r.skipped && full(sent, 1), 'first push: a full tile, in bands of <= 64 rows', JSON.stringify(sent));
  r = await Outbox.submit(1, tile(1)); ok(r.skipped, 'same content again: skipped');
  const t2 = tile(1); t2.fill(9, 100 * W * 2, 101 * W * 2); sent.length = 0; r = await Outbox.submit(1, t2); ok(r.rect && sent.length === 1 && sent[0][1] === 96 && sent[0][2] === 16, 'one changed row -> its 16-row block only', JSON.stringify(sent));
  // latest-wins: three submits while one is in flight -> the middle one is dropped, the last one wins
  sent.length = 0; const a = Outbox.submit(2, tile(1)), b = Outbox.submit(2, tile(2)), c = Outbox.submit(2, tile(3)); const [ra, rb, rc] = await Promise.all([a, b, c]);
  ok(!ra.superseded && rb.superseded && !rc.superseded && Outbox.shown[2][0] === 3, 'latest-wins per tube', JSON.stringify([ra, rb, rc].map(x => !!x.superseded)));
  // a failed send deletes the baseline so the next push is a full tile
  failNext = 'link down'; let threw = false; try { await Outbox.submit(1, tile(5)); } catch (e) { threw = true; } ok(threw && !Outbox.shown[1], 'error: rejected and the baseline is forgotten');
  sent.length = 0; await Outbox.submit(1, tile(5)); ok(full(sent, 1), 'after an error the next push is a full tile (keyframe)');
  // status 4 (the node's epoch moved on): adopt + resend once as a full tile
  staleNext = true; sent.length = 0; r = await Outbox.submit(1, tile(6)); ok(!r.superseded && full(sent, 1) && Outbox.shown[1][0] === 6, 'stale epoch: one automatic full-tile retry', JSON.stringify(sent));
  // supersede: queued + in-flight work resolves as superseded, baselines cleared, CANCEL sent first
  sent.length = 0; const big = tile(7); for (let y = 0; y < H; y += 32) big.fill(y, y * W * 2, (y + 16) * W * 2);   // many separate bands -> many sends
  const p1 = Outbox.submit(3, big), p2 = Outbox.submit(4, tile(8)); await sleep(5); const via = await Outbox.supersede({ clear: true }); const [s1, s2] = await Promise.all([p1, p2]);
  ok(via === 'serial' && sent.some(x => x[0] === 'cancel'), 'supersede sends CANCEL on the cable', JSON.stringify(sent.slice(0, 3)));
  ok(s1.superseded && s2.superseded && !Outbox.shown[3] && !Outbox.shown[4] && Object.keys(sandbox.LIVE.last).length === 0, 'in-flight pushes stop; baselines (and Live\'s) are cleared');
  ok(sent.filter(x => x[0] === 3).length < 8, 'the superseded tile stopped being fed mid-way', sent.filter(x => x[0] === 3).length + ' of 8 bands');
  sandbox.state.paused = true; threw = false; try { await Outbox.submit(1, tile(1)); } catch (e) { threw = e.message === 'paused'; } ok(threw, 'pause gate lives in the Outbox'); }

// ---------------------------------------------------------------- layout / zip / usd / dock
{ const { Layout, ZIP, USD, DockModel } = load(['layout', 'zip', 'usd', 'dock'], { console, TextEncoder, TextDecoder, Uint8Array, Uint32Array, DataView, Math, JSON, Object, Map, Set, Array });
  const L0 = Layout.fresh(35); const ds = Layout.expand(L0);
  ok(ds.length === 6 && ds[0].out === 5 && ds[5].out === 0 && ds[0].pos[0] === -87.5 && ds[5].pos[0] === 87.5, 'fresh row: t5 far left … t0 far right, centred', JSON.stringify(ds.map(d => [d.out, d.pos[0]])));
  ok(Layout.pitch(L0) === 35 && Layout.bytes(L0) < 140, 'a uniform row is stored as one generator', Layout.bytes(L0) + ' bytes: ' + JSON.stringify(Layout.compact(L0)));
  const w0 = Layout.wall(L0, [5, 4, 3, 2, 1]); const gap = Layout.pitchToGap(35);
  ok(w0.n === 5 && w0.xs[0] === 0 && Math.abs(w0.xs[1] - (135 + gap)) <= 1 && Math.abs(w0.W - (5 * 135 + 4 * gap)) <= 2 && w0.H === 240, 'wall(): panel-pixel x from real positions', JSON.stringify(w0));
  ok(Math.abs(Layout.gapToPitch(0) - 14.864) < 1e-3 && Layout.pitchToGap(14.864) === 0 && Math.abs(Layout.gapToPitch(gap) - 35) < 0.06, 'seam px <-> pitch mm');
  // a hole never collapses: skipping t3 keeps everyone else where they are
  const wh = Layout.wall(L0, [5, 4, 2, 1]); ok(wh.xs[2] === w0.xs[3] && wh.xs[1] === w0.xs[1], 'a missing display leaves a hole, the rest stay put', JSON.stringify(wh.xs));
  // move one display: the generator no longer fits -> explicit list; round-trips through compact/normalize
  const L1 = Layout.set(L0, 3, { pos: [-20, 4, 7.5], rot: [0, 12, 0] }); const c1 = Layout.compact(L1);
  ok(!c1.row && c1.displays.length === 6 && JSON.stringify(c1.displays.find(d => d.out === 3)) === JSON.stringify({ id: 't3', out: 3, pos: [-20, 4, 7.5], rot: [0, 12, 0] }), 'moved display -> explicit poses, defaults omitted', JSON.stringify(c1.displays[2]));
  const L2 = Layout.normalize(JSON.parse(JSON.stringify(c1))); ok(JSON.stringify(Layout.expand(L2).map(d => d.pos)) === JSON.stringify(Layout.expand(L1).map(d => d.pos)), 'layout JSON round trip');
  const w1 = Layout.wall(L1, [5, 4, 3, 2, 1]); ok(w1.ys[2] === 0 && w1.ys[0] > 30 && w1.H > 240, 'a raised display shifts the others down in wall pixels', JSON.stringify(w1.ys));
  ok(Layout.pitch(Layout.setPitch(L0, 40)) === 40 && Layout.compact(Layout.setPitch(L0, 40)).row.pitch === 40, 'setPitch re-spaces the row');
  ok(Layout.normalize(null).row.n === 6 && Layout.normalize({ v: 9 }).row.n === 6, 'garbage in -> a fresh layout');
  // the measured model = the known physical condition; rev + model survive compact/normalize; an edit is no longer "measured"
  const M = Layout.fresh(); ok(Layout.pitch(M) === 30.5 && M.model === 'sihai-6' && Layout.isMeasured(M) && Layout.model(M).base.size[0] === 216 && Layout.model(M).base.size[2] === 63.5, 'fresh() is the measured SI HAI model: 30.5 mm pitch on a 216 x 63.5 mm base');
  const span = Layout.expand(M); ok(Math.abs((span[5].pos[0] - span[0].pos[0]) - 152.5) < 1e-6 && Math.abs((216 - 152.5) / 2 - 31.75) < 1e-6, 'six tubes span 152.5 mm, 31.75 mm in from each end of the base (photo: ~31.6 / 32)');
  const E = Object.assign(Layout.set(M, 5, { pos: [-81, -6, 0] }), { rev: 1234 }); ok(!Layout.isMeasured(E) && Layout.normalize(JSON.parse(JSON.stringify(Layout.compact(E)))).rev === 1234 && Layout.compact(E).model === 'sihai-6', 'an edited layout is not "measured"; rev and model round-trip');
  ok(Layout.pitchToGap(30.5) === 142, 'physical seam for the measured pitch: 142 panel px hidden between tubes', String(Layout.pitchToGap(30.5)));
  // zip: store-only, every payload on a 64-byte boundary, valid CRCs, parsable central directory
  ok(ZIP.crc32(new TextEncoder().encode('123456789')) === 0xCBF43926, 'crc32 check value');
  const files = [{ name: 'esptube.usda', data: new TextEncoder().encode('#usda 1.0\n') }, { name: 'tex/t5.png', data: new Uint8Array(1000).fill(7) }, { name: 'tex/t4.png', data: new Uint8Array(33).fill(9) }];
  const z = ZIP.store(files, 64), dv = new DataView(z.buffer); let off = 0, aligned = true, n = 0;
  while (dv.getUint32(off, true) === 0x04034b50) { const nl = dv.getUint16(off + 26, true), el = dv.getUint16(off + 28, true), sz = dv.getUint32(off + 18, true), data = off + 30 + nl + el; if (data % 64) aligned = false;
    if (ZIP.crc32(z.subarray(data, data + sz)) !== dv.getUint32(off + 14, true)) aligned = false; off = data + sz; n++; }
  ok(n === 3 && aligned && dv.getUint32(off, true) === 0x02014b50, 'usdz container: 3 entries, 64-byte aligned payloads, CRCs match');
  const eocd = z.length - 22; ok(dv.getUint32(eocd, true) === 0x06054b50 && dv.getUint16(eocd + 10, true) === 3 && dv.getUint32(eocd + 16, true) === off, 'end-of-central-directory points at the directory');
  // usd text: one Xform per display in mm, and the layout rides along for a round trip
  const usda = USD.usda(L1, { textures: { 3: 'tex/t3.png' } });
  ok(usda.startsWith('#usda 1.0') && usda.includes('metersPerUnit = 0.001') && usda.includes('upAxis = "Y"') && (usda.match(/def Xform "t\d"/g) || []).length === 6 && usda.includes('def Cube "Base"') && usda.includes('xformOp:scale = (216, 14, 63.5)'), 'usda: stage metadata + six displays + the measured base plate');
  ok(usda.includes('xformOp:translate = (-20, 4, 7.5)') && usda.includes('xformOp:rotateXYZ = (0, 12, 0)') && usda.includes('@tex/t3.png@'), 'usda carries the pose and the texture');
  ok((usda.match(/\{/g) || []).length === (usda.match(/\}/g) || []).length && (usda.match(/\(/g) || []).length === (usda.match(/\)/g) || []).length, 'usda brackets balance');
  ok(JSON.stringify(USD.layoutFromUsda(usda)) === JSON.stringify(Layout.compact(L1)), 'layout round-trips through the .usda');
  const uz = USD.usdz(L0, { 5: new Uint8Array(10) }); ok(new TextDecoder().decode(uz.subarray(30, 42)) === 'esptube.usda', 'usdz: the .usda is the first entry');
  // dock model
  const known = ['clock', 'live', 'bridge', 'serial', 'scenes', 'tubes', 'stage', 'span', 'anim', 'tools'];
  let d = DockModel.normalize(null, known); ok(d.order[0] === 'tubes' && d.order.length === known.length && d.collapsed.serial && d.preset === 'everyday', 'dock: default arrangement');
  d = DockModel.move(d, 'serial', 'tubes'); ok(d.order[0] === 'serial' && d.order[1] === 'tubes' && d.preset === 'custom', 'dock: move before');
  d = DockModel.move(d, 'serial', null); ok(d.order[d.order.length - 1] === 'serial', 'dock: move to the end');
  d = DockModel.toggle(d, 'hidden', 'tools', true); ok(d.hidden.tools === 1 && !DockModel.toggle(d, 'hidden', 'tools', false).hidden.tools, 'dock: hide / show');
  const old = DockModel.normalize({ preset: 'custom', order: ['gone', 'span', 'tubes'], collapsed: { gone: 1, span: 1 }, half: {}, hidden: {} }, known);
  ok(old.order.indexOf('span') < old.order.indexOf('tubes') && old.order.length === known.length && !old.order.includes('gone') && !('gone' in old.collapsed) && old.collapsed.span, 'dock: a saved arrangement from another build is repaired (saved order kept, unknown ids dropped, every panel present)', JSON.stringify(old.order));
  const one = DockModel.normalize({ preset: 'custom', order: ['tubes', 'clock', 'scenes', 'span', 'anim', 'live', 'bridge', 'serial', 'tools'], collapsed: {}, half: {}, hidden: {} }, known);
  ok(one.order[1] === 'stage', 'dock: a brand-new panel lands next to its preset neighbour, not at the bottom', JSON.stringify(one.order)); }

console.log(`${checks} checks, ${fails} failed\n${fails ? 'FAILED' : 'ALL PASS'}`); process.exit(fails ? 1 : 0);
