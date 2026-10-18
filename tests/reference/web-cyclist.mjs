/*
cyclist.mjs - Strudel web REPL scheduler reference, captured 2026-10-18.
Source: https://codeberg.org/uzu/strudel/src/branch/main/packages/core/cyclist.mjs
Copyright (C) 2022 Strudel contributors.
This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License as published by the Free
Software Foundation, either version 3 of the License, or (at your option) any
later version. This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
for more details. You should have received a copy of the GNU Affero General
Public License along with this program. If not, see https://www.gnu.org/licenses/.

Test-only snapshot: imports point at the existing pinned package. The bundled
pattern runtime and its established event baselines are not upgraded. The clock
is unchanged upstream; tempo-onset handling matches the deployed web scheduler.
*/
import createClock from '../../js/node_modules/@strudel/core/zyklus.mjs';
import { errorLogger, logger } from '../../js/node_modules/@strudel/core/logger.mjs';

export class Cyclist {
  constructor({ interval, onSchedule, onTrigger, onToggle, onError, getTime,
    latency = 0.1, setInterval, clearInterval, beforeStart }) {
    if (onTrigger) throw new Error('onTrigger has been dropped. use onSchedule(hap, time, cps) instead');
    this.started = false;
    this.beforeStart = beforeStart;
    this.cps = 0.5;
    this.checkpoint = { cycles: 0, seconds: 0 };
    this.num_ticks_since_checkpoint = 0;
    this.lastTick = 0;
    this.lastBegin = 0;
    this.lastEnd = 0;
    this.getTime = getTime;
    this.onToggle = onToggle;
    this.latency = latency;
    this.clock = createClock(getTime, (phase, duration, _, t) => {
      if (this.num_ticks_since_checkpoint === 0)
        this.checkpoint = { cycles: this.lastEnd, seconds: phase };
      this.num_ticks_since_checkpoint++;
      try {
        var begin = this.lastEnd;
        this.lastBegin = begin;
        var end = this.checkpoint.cycles + this.num_ticks_since_checkpoint * duration * this.cps;
        this.lastEnd = end;
        this.lastTick = phase;
        if (phase < t) { console.log('skip query: too late'); return; }
        const tickEndTime = phase + duration;
        var cpsChanged = false;
        do {
          cpsChanged = false;
          const haps = this.pattern.queryArc(begin, end, { _cps: this.cps, cyclist: 'cyclist' })
            .filter(hap => hap.hasOnset()).sort((a, b) => {
              const aBegin = a.whole.begin, bBegin = b.whole.begin;
              if (aBegin.eq(bBegin)) {
                if (a.value.cps !== undefined) return b.value.cps !== undefined ? 0 : -1;
                if (b.value.cps !== undefined) return 1;
                return 0;
              }
              return aBegin.sub(bBegin);
            });
          haps.forEach(hap => {
            const targetTime = (hap.whole.begin - this.checkpoint.cycles) / this.cps + this.checkpoint.seconds;
            if (tickEndTime < targetTime) return;
            onSchedule?.(hap, targetTime + this.latency, hap.value.cps ?? this.cps);
            if (hap.value.cps !== undefined && this.cps != hap.value.cps) {
              cpsChanged = true;
              this.cps = hap.value.cps;
              this.num_ticks_since_checkpoint = 0;
              this.checkpoint = { cycles: hap.whole.begin, seconds: targetTime };
            }
          });
          if (cpsChanged) {
            begin = end;
            const remainingSecondsSinceCheckpoint = tickEndTime - this.checkpoint.seconds;
            const remainingCycles = remainingSecondsSinceCheckpoint * this.cps;
            end = this.checkpoint.cycles + remainingCycles;
            this.lastEnd = end;
            if (end < begin) break;
          }
        } while (cpsChanged);
      } catch (e) { errorLogger(e); onError?.(e); }
    }, interval, 0.1, 0.1, setInterval, clearInterval);
  }
  now() {
    if (!this.started) return 0;
    const secondsSinceLastTick = this.getTime() - this.lastTick - this.clock.duration;
    return this.lastBegin + secondsSinceLastTick * this.cps;
  }
  setStarted(v) { this.started = v; this.onToggle?.(v); }
  async start() {
    await this.beforeStart?.();
    this.checkpoint = { cycles: 0, seconds: 0 };
    this.num_ticks_since_checkpoint = 0;
    if (!this.pattern) throw new Error('Scheduler: no pattern set! call .setPattern first.');
    logger('[cyclist] start'); this.clock.start(); this.setStarted(true);
  }
  pause() { logger('[cyclist] pause'); this.clock.pause(); this.setStarted(false); }
  stop() { logger('[cyclist] stop'); this.clock.stop(); this.lastEnd = 0; this.setStarted(false); }
  async setPattern(pat, autostart = false) {
    this.pattern = pat;
    if (autostart && !this.started) await this.start();
  }
  setCps(cps = 0.5) {
    if (this.cps === cps) return;
    this.cps = cps; this.num_ticks_since_checkpoint = 0;
  }
  log(begin, end, haps) {
    const onsets = haps.filter(h => h.hasOnset());
    console.log(`${begin.toFixed(4)} - ${end.toFixed(4)} ${Array(onsets.length).fill('I').join('')}`);
  }
}
