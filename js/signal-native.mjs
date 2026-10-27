/* Fastcat triangle controls sampled in C. One-sample waveforms stay in JavaScript. */

const SAW = 0, ISAW = 1;
const PIECE_LIMIT = 8192;

function isFraction(value) {
  return !!value && typeof value.mul === 'function' && typeof value.lte === 'function' && typeof value.sam === 'function';
}

function factorOf(Fraction, value) {
  if (typeof value === 'number') {
    if (!Number.isFinite(value) || value <= 0) return null;
    return Fraction(value);
  }
  if (isFraction(value)) return value.lte(0) ? null : value;
  return null;
}

function offsetOf(Fraction, value) {
  if (typeof value === 'number') return Number.isFinite(value) ? Fraction(value) : null;
  if (isFraction(value)) return value;
  return null;
}

function finiteNumber(value) {
  return typeof value === 'number' && Number.isFinite(value);
}

/**
 * Install native queries for tri/itri and their numeric fast/slow/early/late/range chain.
 * Uniform waveforms and pattern arguments keep the original Strudel query.
 * @param {object} core pinned Strudel core
 * @returns {void}
 */
export function installNativeSignals(core) {
  /* Reference runners and other JS-only hosts must retain the pinned query
     path, including untouched prototypes and signal objects. */
  if (typeof globalThis.__hostSignal !== 'function') return;
  const { Pattern, Hap, TimeSpan, Fraction } = core;
  const couple = (waveA, waveB) => ({ kind: 'cat', waveA, waveB, segments: 2, rate: Fraction(1), phase: Fraction(0), ops: [] });
  const warp = (sig, factor, offset) => ({
    ...sig, rate: sig.rate.mul(factor), phase: sig.phase.add(offset.mul(sig.rate)),
  });
  const withOps = (sig, extra) => ({ ...sig, ops: sig.ops.concat(extra) });
  const applyOps = (raw, ops) => {
    let value = raw;
    for (const op of ops) {
      if (op.t === 'mul') value *= op.k;
      else if (op.t === 'add') value += op.k;
      else if (op.t === 'bipolar') value = value * 2 - 1;
      else value = (value + 1) / 2;
    }
    return value;
  };
  const pieces = (begin, end) => {
    if (begin.equals(end)) return [[begin, end]];
    if (!end.gt(begin)) return null;
    const out = [];
    const endSam = end.sam();
    let cursor = begin;
    while (end.gt(cursor)) {
      if (out.length >= PIECE_LIMIT) return null;
      if (cursor.sam().equals(endSam)) {
        out.push([cursor, end]);
        break;
      }
      const next = cursor.nextSam();
      if (!next.gt(cursor)) return null;
      out.push([cursor, next]);
      cursor = next;
    }
    return out;
  };
  const seedOf = (state) => {
    const seed = state.controls && state.controls.randSeed;
    return typeof seed === 'number' && Number.isFinite(seed) ? seed : 0;
  };
  const samples = (kinds, times, seed) => {
    try {
      const raw = globalThis.__hostSignal(kinds, times, seed);
      return raw && raw.length === times.length ? raw : null;
    } catch {
      return null;
    }
  };
  const querySig = (sig, state) => {
    const span = state && state.span;
    if (!span) return null;
    if (typeof globalThis.__hostTriangle === 'function') {
      const native = globalThis.__hostTriangle(span, sig.rate, sig.phase, sig.waveA, sig.waveB, sig.ops);
      if (native !== null) return native;
    }
    const domainBegin = span.begin.mul(sig.rate).add(sig.phase);
    const domainEnd = span.end.mul(sig.rate).add(sig.phase);
    const seed = seedOf(state);
    const segments = Fraction(sig.segments);
    const cuts = pieces(domainBegin.mul(segments), domainEnd.mul(segments));
    if (!cuts) return null;
    const kinds = [];
    const times = [];
    for (const [begin] of cuts) {
      const index = begin.sam().valueOf();
      if (!Number.isSafeInteger(index)) return null;
      const offset = begin.floor().sub(begin.div(segments).floor());
      kinds.push(core._mod(index, sig.segments) === 0 ? sig.waveA : sig.waveB);
      times.push(begin.sub(offset).valueOf());
    }
    const raw = samples(kinds, times, seed);
    if (!raw) return null;
    const undo = (time) => time.div(segments).sub(sig.phase).div(sig.rate);
    return cuts.map(([begin, end], i) => new Hap(undefined, new TimeSpan(undo(begin), undo(end)), applyOps(raw[i], sig.ops), { locations: [] }));
  };
  const lift = (sig, slow) => {
    const pat = new Pattern((state) => querySig(sig, state) || slow.query(state));
    pat.__sig = sig;
    if (slow.hasSteps) pat.setSteps(slow._steps);
    return pat;
  };
  const wrap = (name, adapt) => {
    const previous = Pattern.prototype[name];
    if (typeof previous !== 'function') return;
    Pattern.prototype[name] = function (...args) {
      const slow = previous.apply(this, args);
      if (!this.__sig) return slow;
      const sig = adapt(this.__sig, args);
      return sig ? lift(sig, slow) : slow;
    };
  };

  const speedSig = (sig, args) => {
    if (args.length !== 1) return null;
    const amount = factorOf(Fraction, args[0]);
    return amount ? warp(sig, amount, Fraction(0)) : null;
  };
  const shiftSig = (sig, args) => {
    if (args.length !== 1) return null;
    const amount = offsetOf(Fraction, args[0]);
    return amount ? warp(sig, Fraction(1), amount) : null;
  };
  wrap('fast', speedSig);
  wrap('density', speedSig);
  wrap('_fast', speedSig);
  wrap('early', shiftSig);
  wrap('_early', shiftSig);
  const rangeSig = (sig, min, max) => finiteNumber(min) && finiteNumber(max)
    ? withOps(sig, [{ t: 'mul', k: max - min }, { t: 'add', k: min }]) : null;
  wrap('range', (sig, [min, max]) => rangeSig(sig, min, max));
  wrap('_range', (sig, [min, max]) => rangeSig(sig, min, max));
  wrap('toBipolar', (sig) => withOps(sig, [{ t: 'bipolar' }]));
  wrap('fromBipolar', (sig) => withOps(sig, [{ t: 'unipolar' }]));

  /* Uniform signals are one sample per query. A host call is slower than the
     original function there, so only the fastcat waveforms take this path. */
  const triSig = couple(SAW, ISAW);
  core.tri.__sig = triSig;
  core.itri.__sig = couple(ISAW, SAW);
  core.tri2.__sig = withOps(triSig, [{ t: 'bipolar' }]);
  core.itri2.__sig = withOps(core.itri.__sig, [{ t: 'bipolar' }]);
}
