/* UTF-8 browser primitive for the bounded QuickJS realm. No DOM or I/O. */
const instances=new WeakSet();
const arrayTag=Object.getOwnPropertyDescriptor(Object.getPrototypeOf(Uint8Array.prototype),Symbol.toStringTag).get;
function check(instance) {
  if (!instances.has(instance)) throw new TypeError('Invalid TextEncoder receiver');
}
function text(value) {
  if (typeof value==='symbol') throw new TypeError('Cannot encode a Symbol');
  return String(value);
}
function length(input) {
  let bytes=0;
  for(let i=0;i<input.length;) {
    let cp=input.codePointAt(i); i+=cp>0xffff?2:1;
    if(cp>=0xd800 && cp<=0xdfff)cp=0xfffd;
    bytes+=cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;
  }
  return bytes;
}
function write(input,output) {
  let read=0,written=0;
  while(read<input.length) {
    let cp=input.codePointAt(read),units=cp>0xffff?2:1;
    if(cp>=0xd800 && cp<=0xdfff)cp=0xfffd;
    const bytes=cp<0x80?1:cp<0x800?2:cp<0x10000?3:4;
    if(output.length-written<bytes)break;
    if(bytes===1)output[written++]=cp;
    else {
      if(bytes===2)output[written++]=0xc0|(cp>>6);
      else {
        if(bytes===3)output[written++]=0xe0|(cp>>12);
        else {output[written++]=0xf0|(cp>>18);output[written++]=0x80|((cp>>12)&63);}
        output[written++]=0x80|((cp>>6)&63);
      }
      output[written++]=0x80|(cp&63);
    }
    read+=units;
  }
  return {read,written};
}
export class TextEncoder {
  constructor(){instances.add(this);}
  get encoding(){check(this);return 'utf-8';}
  encode(input='') {
    check(this); input=text(input);
    const output=new Uint8Array(length(input)); write(input,output); return output;
  }
  encodeInto(input,destination) {
    check(this);
    if(arguments.length<2)throw new TypeError('TextEncoder.encodeInto requires two arguments');
    input=text(input);
    if(arrayTag.call(destination)!=='Uint8Array')throw new TypeError('Destination must be a Uint8Array');
    return write(input,destination);
  }
}
Object.defineProperty(TextEncoder.prototype,Symbol.toStringTag,{value:'TextEncoder',configurable:true});
