import json, socket, subprocess, time, shutil
from pathlib import Path
root=Path(__file__).resolve().parents[1]
import sys
version=int(sys.argv[1]) if len(sys.argv)>1 else 1
out=root/('build/xhci-review-'+str(version));out.mkdir(exist_ok=True)
shutil.copyfile(root/'dist/tiny64.img',out/'boot.img')
shutil.copyfile(root/'dist/storage.img',out/'storage.img')
shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',out/'vars.fd')
subprocess.run(['mcopy','-o','-i',str(out/'boot.img'),str(root/'dist/kernel.elf'),'::/kernel.elf'],check=True)
subprocess.run(['mcopy','-o','-i',str(out/'boot.img'),str(root/'dist/BOOTX64.EFI'),'::/EFI/BOOT/BOOTX64.EFI'],check=True)
sockpath='/tmp/vexos-xhci-'+str(version)+'.sock'
Path(sockpath).unlink(missing_ok=True)
(out/'flash.img').write_bytes(bytes(4*1024*1024))
p=subprocess.Popen(['qemu-system-x86_64','-machine','pc','-m','1G','-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,file={out}/vars.fd','-drive',f'format=raw,file={out}/boot.img','-drive',f'if=ide,index=1,format=raw,file={out}/storage.img','-device','qemu-xhci,id=xhci','-drive',f'if=none,id=flash,format=raw,file={out}/flash.img','-device',f'usb-kbd,id=kbd,usb_version={version},bus=xhci.0,port=1','-device',f'usb-mouse,id=mouse,usb_version={version},bus=xhci.0,port=2','-device','usb-tablet,id=tablet,bus=xhci.0,port=3','-device','usb-storage,id=flashusb,drive=flash,bus=xhci.0,port=4','-nic','none','-display','none','-serial',f'file:{out}/serial.log','-qmp',f'unix:{sockpath},server=on,wait=off'],stderr=open(out/'qemu.log','w'))
try:
    for _ in range(50):
        if Path(sockpath).exists():break
        time.sleep(.1)
    q=socket.socket(socket.AF_UNIX);q.connect(sockpath);f=q.makefile('rwb',buffering=0)
    f.readline()
    def cmd(name,args=None):
        f.write((json.dumps({'execute':name,'arguments':args or {}})+'\n').encode())
        while True:
            r=json.loads(f.readline())
            if 'return' in r:return r['return']
            if 'error' in r:raise RuntimeError(r)
    cmd('qmp_capabilities')
    start=time.time()
    while time.time()-start<45:
        if (out/'serial.log').exists() and 'Starting Fullscreen Setup Wizard' in (out/'serial.log').read_text(): break
        time.sleep(.25)
    else: raise AssertionError('Boot never reached setup')
    time.sleep(1)
    cmd('screendump',{'filename':str(out/'boot.ppm')})
    def move(x,y):
        cmd('input-send-event',{'events':[{'type':'abs','data':{'axis':'x','value':int(x*32767/1280)}},{'type':'abs','data':{'axis':'y','value':int(y*32767/800)}}]})
        time.sleep(.15)
    def click(x,y):
        move(x,y)
        cmd('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
        time.sleep(.25)
        cmd('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
        time.sleep(.5)
    for i in range(6): click(1190,744)
    time.sleep(2)
    cmd('screendump',{'filename':str(out/'desktop.ppm')})
    click(40,765)
    cmd('screendump',{'filename':str(out/'menu.ppm')})
    click(60,612)
    time.sleep(.5)
    cmd('screendump',{'filename':str(out/'terminal.ppm')})
    move(900,600)
    cmd('screendump',{'filename':str(out/'cursor-before.ppm')})
    move(1000,600)
    cmd('screendump',{'filename':str(out/'cursor-after.ppm')})
    # Open Notepad from the dock, type through USB keyboard, and save.
    click(604,766)
    for key in ['h','e','l','l','o']:
        cmd('send-key',{'keys':[{'type':'qcode','data':key}]})
        time.sleep(.12)
    cmd('screendump',{'filename':str(out/'notepad.ppm')})
    click(582,180)
    cmd('screendump',{'filename':str(out/'saved.ppm')})
    mice=cmd('query-mice'); print('Input devices:',mice,flush=True)
    mouse=next(m for m in mice if 'Mouse' in m['name'] and 'PS/2' not in m['name'] and not m['absolute'])
    cmd('human-monitor-command',{'command-line':'mouse_set '+str(mouse['index'])})
    for i in range(320):
        cmd('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':1}}]})
        time.sleep(.015)
    cmd('device_del',{'id':'mouse'})
    time.sleep(.6)
    cmd('device_add',{'driver':'usb-mouse','id':'mouse2','usb_version':version,'bus':'xhci.0','port':'2'})
    time.sleep(1)
    mice=cmd('query-mice'); print('Input devices:',mice,flush=True); mouse=next(m for m in mice if 'Mouse' in m['name'] and 'PS/2' not in m['name'] and not m['absolute'])
    cmd('human-monitor-command',{'command-line':'mouse_set '+str(mouse['index'])})
    cmd('input-send-event',{'events':[{'type':'rel','data':{'axis':'x','value':4}}]})
    time.sleep(.3)
    log=(out/'serial.log').read_text()
    speed=1 if version==1 else 3
    assert any('enumeration complete port=5 ' in line and f'speed={speed}' in line for line in log.splitlines())
    assert 'speed=4 VID:PID=' in log and 'EP0_MPS=512' in log, 'SuperSpeed descriptor missing'
    assert log.count('enumeration complete port=6')>=2, 'Mouse reconnect failed'
    assert 'count=256' in log, 'Interrupt ring did not cross wrap'
    assert 'HID report port=5' in log and 'HID report port=6' in log
    assert 'timeout' not in log.lower(), 'Unexpected timeout'
    print('PASS: USB speeds, SuperSpeed storage enumeration, HID reports, ring wrap and reconnect')
    print((out/'serial.log').read_text()[-2200:])
    print('Screenshots:',out)

finally:
    p.terminate();p.wait(timeout=5)

# Inspect the persisted test disk after QEMU has flushed and exited.
import struct
with open(out/'storage.img','rb') as disk:
    header = struct.unpack('<8I', disk.read(32))
    assert header[0] == 0x31465353, 'Expected SimpleFS test storage'
    disk.seek(header[2]*512)
    table = disk.read(header[3]*512)
    for i in range(64):
        entry = table[i*45:(i+1)*45]
        if entry[0] and entry[1:33].split(bytes([0]),1)[0] == b'note.txt':
            start, size = struct.unpack('<II',entry[33:41])
            disk.seek(start*512)
            assert disk.read(size) == b'hello', 'Saved contents differ'
            print('PASS: USB typing and Notepad save persisted hello in note.txt')
            break
    else: raise AssertionError('Notepad save did not create note.txt')
assert '[KERNEL] Starting main loop' in (out/'serial.log').read_text()
assert '[EXCEPTION]' not in (out/'serial.log').read_text()
print('PASS: desktop reached main loop without an exception log')
