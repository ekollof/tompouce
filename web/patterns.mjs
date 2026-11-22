/*! Browser-side pattern queries for Hydra H() and time-driven visual values.
   Uses the same pinned Strudel packages as playback so mini-notation and
   pattern alignment match the engine. This bundle is visual-only: it never
   schedules audio or evaluates user code beyond the page's existing source
   overlay. Strudel is AGPL-3.0-or-later; see thirdparty/STRUDEL-LICENSE. */
import * as core from '@strudel/core';
import * as mini from '@strudel/mini';

core.setStringParser(mini.mini);

let cycle = 0;

function numeric(value) {
  const n = typeof value === 'number' ? value : Number(value);
  return Number.isFinite(n) ? n : 0;
}

/* Strudel's H() returns a function read by Hydra every frame. Parse the
   pattern once, then query the current cycle so visuals stay aligned with
   playback. Values that cannot be represented numerically stay 0. */
function H(pattern) {
  let parsed = null;
  try { parsed = core.reify(pattern); } catch (error) { parsed = null; }
  return () => {
    if (!parsed) return 0;
    try {
      const haps = parsed.queryArc(cycle, cycle);
      return haps.length ? numeric(haps[0].value) : 0;
    } catch (error) {
      return 0;
    }
  };
}

const api = {
  H,
  setCycle(value) {
    if (Number.isFinite(value)) cycle = value;
  },
  getCycle() {
    return cycle;
  },
};

globalThis.TompouceHydraPatterns = api;
globalThis.H = H;
