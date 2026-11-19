/* Minimal Hydra-compatible visual functions for the bounded /hydra page.
   Generators produce a render function on a 2D canvas; effects wrap it. No
   network, WebGL or engine control is involved. */
(function (global) {
  'use strict';
  function clamp(value, low, high) { return Math.min(high, Math.max(low, value)); }
  function number(value, fallback) {
    const n = Number(value);
    return Number.isFinite(n) ? n : fallback;
  }

  function generator(paint) {
    const api = {
      _paint: paint,
      rotate(angle) {
        const a = number(angle, 0);
        return generator((ctx, w, h, scope) => {
          ctx.save(); ctx.translate(w / 2, h / 2); ctx.rotate(a * Math.PI * 2);
          ctx.translate(-w / 2, -h / 2); paint(ctx, w, h, scope); ctx.restore();
        });
      },
      scale(x, y) {
        const sx = number(x, 1), sy = number(y === undefined ? x : y, 1);
        return generator((ctx, w, h, scope) => {
          ctx.save(); ctx.translate(w / 2, h / 2);
          ctx.scale(clamp(sx, .05, 20), clamp(sy, .05, 20));
          ctx.translate(-w / 2, -h / 2); paint(ctx, w, h, scope); ctx.restore();
        });
      },
      repeatX(count) {
        const n = clamp(Math.floor(number(count, 2)), 1, 16);
        return generator((ctx, w, h, scope) => {
          for (let i = 0; i < n; i++) {
            ctx.save(); ctx.beginPath(); ctx.rect(i * w / n, 0, w / n, h); ctx.clip();
            ctx.translate(-i * w / n, 0); paint(ctx, w, h, scope); ctx.restore();
          }
        });
      },
      repeatY(count) {
        const n = clamp(Math.floor(number(count, 2)), 1, 16);
        return generator((ctx, w, h, scope) => {
          for (let i = 0; i < n; i++) {
            ctx.save(); ctx.beginPath(); ctx.rect(0, i * h / n, w, h / n); ctx.clip();
            ctx.translate(0, -i * h / n); paint(ctx, w, h, scope); ctx.restore();
          }
        });
      },
      color(r, g, b) {
        const red = clamp(number(r, 1) * 255, 0, 255);
        const green = clamp(number(g, 1) * 255, 0, 255);
        const blue = clamp(number(b, 1) * 255, 0, 255);
        return generator((ctx, w, h, scope) => {
          ctx.save(); ctx.globalCompositeOperation = 'source-in';
          ctx.fillStyle = `rgb(${red},${green},${blue})`;
          ctx.fillRect(0, 0, w, h); ctx.restore();
          paint(ctx, w, h, scope);
        });
      },
      out() { global.__hydraOutput = api; return api; },
    };
    return api;
  }

  const api = {
    osc(frequency) {
      const f = clamp(number(frequency, 4), 1, 256);
      return generator((ctx, w, h) => {
        ctx.fillStyle = '#121a2e'; ctx.fillRect(0, 0, w, h);
        ctx.strokeStyle = '#7fd4ff'; ctx.lineWidth = 2; ctx.beginPath();
        for (let x = 0; x <= w; x += 4) {
          const y = h / 2 + Math.sin(x / w * Math.PI * 2 * f) * h * .3;
          if (x === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.stroke();
      });
    },
    shape(sides) {
      const n = clamp(Math.floor(number(sides, 3)), 3, 32);
      return generator((ctx, w, h) => {
        ctx.fillStyle = '#121a2e'; ctx.fillRect(0, 0, w, h);
        const radius = Math.min(w, h) * .35;
        ctx.strokeStyle = '#b9ff7f'; ctx.lineWidth = 2; ctx.beginPath();
        for (let i = 0; i <= n; i++) {
          const angle = i / n * Math.PI * 2;
          const x = w / 2 + Math.cos(angle) * radius;
          const y = h / 2 + Math.sin(angle) * radius;
          if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
        }
        ctx.stroke();
      });
    },
    solid(r, g, b) {
      const red = clamp(number(r, 1) * 255, 0, 255);
      const green = clamp(number(g, 1) * 255, 0, 255);
      const blue = clamp(number(b, 1) * 255, 0, 255);
      return generator((ctx, w, h) => {
        ctx.fillStyle = `rgb(${red},${green},${blue})`; ctx.fillRect(0, 0, w, h);
      });
    },
    noise() {
      return generator((ctx, w, h, scope) => {
        ctx.fillStyle = '#121a2e'; ctx.fillRect(0, 0, w, h);
        ctx.fillStyle = '#ffffff';
        for (let i = 0; i < (scope && scope.scope ? scope.scope.length : 0); i++) {
          const value = scope.scope[i];
          ctx.fillRect(i / scope.scope.length * w, h / 2 - value * h * .4, 2, 2);
        }
      });
    },
    src() { return api.solid(.07, .1, .18); },
  };
  global.Hydra = api;
  Object.assign(global, api);
})(typeof globalThis !== 'undefined' ? globalThis : this);
