/* Keep the pinned implementation for parsing and unusual values. Ordinary
   clock clones can avoid repeating its JavaScript BigInt normalization. */
import OriginalFraction from './node_modules/fraction.js/dist/fraction.mjs';

function Fraction(value, denominator) {
  if (new.target && new.target !== Fraction) {
    return Reflect.construct(OriginalFraction, [value, denominator], new.target);
  }
  if (denominator === undefined && typeof globalThis.__hostFractionCreate === 'function' &&
      ((typeof value === 'object' && value !== null) ||
       (typeof value === 'number' && Number.isSafeInteger(value)))) {
    const native = globalThis.__hostFractionCreate(value);
    if (native !== null) return native;
  }
  return OriginalFraction(value, denominator);
}

Fraction.prototype = OriginalFraction.prototype;
Object.setPrototypeOf(Fraction, OriginalFraction);
export default Fraction;
