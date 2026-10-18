/*!
 * Farey conversion adapted from fraction.js, Copyright (c) 2025 Robert Eisele.
 * MIT License
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/* Match fraction.js 5.x's number conversion without walking millions of
   adjacent Farey mediants for floating-point clock noise near a simple ratio.
   Binary-search consecutive identical branches, using the same Number
   division/comparison and denominator limit as the pinned dependency. */
export function queryFraction(Fraction, value) {
  if (!Number.isFinite(value) || Number.isInteger(value)) return Fraction(value);
  const sign=value<0?-1:1;
  let x=Math.abs(value), scale=1;
  if (x>=1) {scale=10**Math.floor(1+Math.log10(x)); x/=scale;}
  const limit=10000000;
  let a=0,b=1,c=1,d=1,n=0,den=1;
  while (b<=limit && d<=limit) {
    const middle=(a+c)/(b+d);
    if (x===middle) {
      if (b+d<=limit) {n=a+c;den=b+d;}
      else if (d>b) {n=c;den=d;}
      else {n=a;den=b;}
      break;
    }
    const lower=x>middle;
    const maximum=Math.floor((limit-(lower?b:d))/(lower?d:b))+1;
    let lo=1,hi=maximum;
    while (lo<hi) {
      const k=Math.ceil((lo+hi)/2);
      const ratio=lower?(a+k*c)/(b+k*d):(c+k*a)/(d+k*b);
      if (lower?x>ratio:x<ratio) lo=k;
      else hi=k-1;
    }
    if (lower) {a+=lo*c;b+=lo*d;}
    else {c+=lo*a;d+=lo*b;}
    if (b>limit) {n=c;den=d;}
    else {n=a;den=b;}
  }
  return Fraction({n:BigInt(n)*BigInt(scale)*BigInt(sign),d:BigInt(den)});
}
