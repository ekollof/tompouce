import assert from 'node:assert/strict';
import vm from 'node:vm';
import {TextEncoder as Portable} from './text-encoding.mjs';

const native=new TextEncoder(),portable=new Portable();
const values=['','ascii','café 🎵','\0\u007f\u0080\u07ff\u0800\uffff',
  '\ud800','\udc00','A\ud800B\udc00C','\ud800\ud800\udc00',undefined,null,123,false];
for(const value of values) {
  const wanted=native.encode(value),got=portable.encode(value);
  assert.deepEqual(got,wanted); assert.equal(got.buffer.byteLength,wanted.length);
  const input=String(value);
  for(let size=0;size<=native.encode(input).length+2;size++) {
    const a=new Uint8Array(size+4).fill(0xcc),b=a.slice();
    assert.deepEqual(portable.encodeInto(input,a.subarray(2,2+size)),native.encodeInto(input,b.subarray(2,2+size)));
    assert.deepEqual(a,b);
  }
}
assert.equal(portable.encoding,'utf-8');
assert.equal(Object.prototype.toString.call(portable),'[object TextEncoder]');
for(const encoder of [portable,native]) {
  assert.throws(()=>encoder.encode(Symbol('x')),TypeError);
  assert.throws(()=>encoder.encodeInto(),TypeError);
  assert.throws(()=>encoder.encodeInto('x'),TypeError);
  for(const destination of [null,[],new Uint8ClampedArray(4),new DataView(new ArrayBuffer(4)),new Proxy(new Uint8Array(4),{})])
    assert.throws(()=>encoder.encodeInto('x',destination),TypeError);
  assert.throws(()=>encoder.encode.call({},'x'),TypeError);
  assert.throws(()=>Object.getOwnPropertyDescriptor(Object.getPrototypeOf(encoder),'encoding').get.call({}),TypeError);
  const foreign=vm.runInNewContext('new Uint8Array(8)');
  assert.deepEqual(encoder.encodeInto('café',foreign),{read:4,written:5});
  assert.deepEqual(Array.from(foreign).slice(0,5),[99,97,102,195,169]);
}
console.log('UTF-8 encoding matches TextEncoder: scalar replacement, boundaries, destinations and brands');
