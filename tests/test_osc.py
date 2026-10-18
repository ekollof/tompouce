"""Native OSC wire format and transport, against disposable silent engines."""
import json
import os
import select
import socket
import struct
import subprocess
import tempfile
import time
import unittest

BINARY=os.environ.get('TOMPOUCE_TEST_BINARY','./build/tompouce')

def decode(packet):
    assert packet[:8]==b'#bundle\0'
    seconds,fraction,length=struct.unpack('>III',packet[8:20])
    assert length==len(packet)-20
    stamp=seconds-2208988800+fraction/2**32
    at=20
    def string():
        nonlocal at
        end=packet.index(0,at)
        text=packet[at:end].decode()
        at=(end+4)&~3
        return text
    assert string()=='/dirt/play'
    types=string()
    assert types.startswith(',')
    values=[]
    for tag in types[1:]:
        if tag=='s': values.append(string())
        else:
            assert tag in 'if'
            values.append(struct.unpack('>'+tag,packet[at:at+4])[0]); at+=4
    assert at==len(packet) and len(values)%2==0
    return stamp,dict(zip(values[::2],values[1::2]))

class OscTest(unittest.TestCase):
    def test_scheduling_wire_format_and_stop(self):
        self.exercise(False)

    def test_without_local_audio(self):
        self.exercise(True)

    def exercise(self,silent):
        with tempfile.TemporaryDirectory(prefix='tompouce-osc-') as root, socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as receiver:
            receiver.bind(('127.0.0.1',0)); receiver.settimeout(8)
            port=receiver.getsockname()[1]
            code=f'note("c4*8").s("superpiano").gain(.3).roomsize(.2).oscport({port}).osc()'
            env=dict(os.environ,XDG_CONFIG_HOME=root,XDG_CACHE_HOME=root,TOMPOUCE_BANKS=root)
            if silent:
                config=os.path.join(root,'alsa.conf')
                with open(config,'w') as file: file.write('pcm.!default { type tompouce_missing_pcm }\n')
                env.update(ALSA_CONFIG_PATH=config,PIPEWIRE_REMOTE='tompouce-missing-audio-server',
                           PULSE_SERVER='unix:'+root+'/missing-pulse',JACK_DEFAULT_SERVER='tompouce-missing-jack-server')
            with subprocess.Popen([BINARY,'--mcp','--code',code],stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True,env=env) as engine:
                def tool(name,arguments=None):
                    request=dict(jsonrpc='2.0',id=1,method='tools/call',params=dict(name=name,arguments=arguments or {}))
                    engine.stdin.write(json.dumps(request)+'\n'); engine.stdin.flush()
                    self.assertTrue(select.select([engine.stdout],[],[],8)[0])
                    response=json.loads(engine.stdout.readline())
                    self.assertNotIn('error',response)
                    return response['result']['content'][0]['text']
                def drain_committed():
                    receiver.setblocking(False)
                    deadline=time.monotonic()+.35
                    while time.monotonic()<deadline:
                        if select.select([receiver],[],[],max(0,deadline-time.monotonic()))[0]:
                            _,retained=decode(receiver.recvfrom(4096)[0])
                            self.assertEqual(retained['s'],'superpiano')
                            self.assertAlmostEqual(retained['gain'],.3,places=6)
                try:
                    packet,_=receiver.recvfrom(4096)
                    stamp,values=decode(packet)
                    self.assertLess(abs(stamp-time.time()),2)
                    self.assertEqual(values['s'],'superpiano')
                    self.assertEqual(values['midinote'],60)
                    self.assertAlmostEqual(values['gain'],.3,places=6)
                    self.assertAlmostEqual(values['size'],.2,places=6)
                    self.assertEqual(values['cps'],.5)
                    self.assertAlmostEqual(values['delta'],.25)
                    self.assertNotIn('_osc',values)
                    health=tool('music_health')
                    self.assertIn('voices=0/128',health)
                    if silent: self.assertIn('stream=down backend=none',health)
                    tool('music_set_bpm',{'bpm':0})
                    self.assertIn('bpm=0',tool('music_health'))
                    drain_committed()
                    self.assertFalse(select.select([receiver],[],[],.4)[0], 'zero tempo kept advancing')
                    tool('music_set_bpm',{'bpm':120}); receiver.settimeout(3)
                    _,resumed=decode(receiver.recvfrom(4096)[0])
                    self.assertEqual(resumed['cps'],.5)
                    self.assertAlmostEqual(resumed['delta'],.25)
                    tool('music_stop')
                    drain_committed()
                    self.assertFalse(select.select([receiver],[],[],.4)[0], 'stopped transport emitted OSC')
                    tool('music_play'); receiver.settimeout(3); receiver.recvfrom(4096)
                    tool('music_set_code',{'code':f'note("c4*8").oscport({port}).oschost("invalid-host").osc()'})
                    # Like the web scheduler, a replacement retains already
                    # submitted notes in its bounded lookahead. Those packets
                    # must still belong to the old, valid pattern. A 50 ms
                    # quiet gap does not drain a quarter-second drum pattern.
                    drain_committed()
                    self.assertFalse(select.select([receiver],[],[],.4)[0], 'invalid OSC address emitted data')
                    self.assertIn('voices=0/128',tool('music_health'))
                    tool('music_stop')
                    self.assertIn('autostart must be boolean',tool('music_set_code',{'code':'note(60).gain(0)','autostart':'true'}))
                    self.assertIn('playing=no',tool('music_health'))
                    tool('music_set_code',{'code':'note(60).gain(0)'})
                    self.assertIn('playing=no',tool('music_health'))
                    tool('music_evaluate',{'autostart':True})
                    self.assertIn('playing=yes',tool('music_health'))
                    tool('music_stop')
                    failed='note(99); throw Error("stopped performance edit")'
                    self.assertIn('Evaluation failed',tool('music_set_code',{'code':failed,'autostart':True}))
                    self.assertIn('playing=no',tool('music_health'))
                    self.assertIn('Evaluation failed',tool('music_evaluate',{'autostart':True}))
                    self.assertIn('playing=no',tool('music_health'))
                    tool('music_set_code',{'code':'note(72).gain(0)','autostart':True})
                    self.assertIn('playing=yes',tool('music_health'))
                finally:
                    engine.stdin.close()
                    try: engine.wait(timeout=8)
                    except subprocess.TimeoutExpired: engine.kill(); engine.wait()
                self.assertEqual(engine.returncode,0)

if __name__=='__main__': unittest.main()
