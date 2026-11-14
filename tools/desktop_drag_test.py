import json, socket, subprocess, time, shutil
from pathlib import Path
root=Path(__file__).resolve().parents[1]
out=root/'build/desktop-drag-review';out.mkdir(exist_ok=True)
shutil.copyfile(root/'dist/tiny64.img',out/'boot.img')
shutil.copyfile(root/'dist/storage.img',out/'storage.img')
shutil.copyfile('/usr/share/OVMF/OVMF_VARS_4M.fd',out/'vars.fd')
subprocess.run(['mcopy','-o','-i',str(out/'boot.img'),str(root/'dist/kernel.elf'),'::/kernel.elf'],check=True)
subprocess.run(['mcopy','-o','-i',str(out/'boot.img'),str(root/'dist/BOOTX64.EFI'),'::/EFI/BOOT/BOOTX64.EFI'],check=True)
sockpath='/tmp/vexos-desktop-drag.sock'
Path(sockpath).unlink(missing_ok=True)
p=subprocess.Popen(['qemu-system-x86_64','-machine','pc','-m','1G','-drive','if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd','-drive',f'if=pflash,format=raw,file={out}/vars.fd','-drive',f'format=raw,file={out}/boot.img','-drive',f'if=ide,index=1,format=raw,file={out}/storage.img','-device','qemu-xhci','-device','usb-kbd','-device','usb-tablet','-nic','none','-display','none','-serial',f'file:{out}/serial.log','-qmp',f'unix:{sockpath},server=on,wait=off'],stderr=open(out/'qemu.log','w'))
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
    time.sleep(25)
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
    # Repeat the reported sequence, crossing both edges and USB ring wraps.
    for cycle in range(24):
        click(460,766) # Viewer
        sx,sy=(150,140) if cycle==0 else (54,12)
        move(sx,sy)
        cmd('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':True}}]})
        time.sleep(.08)
        for step in range(14):
            x=sx+(1250-sx)*(step+1)//14; y=sy+(740-sy)*(step+1)//14
            cmd('input-send-event',{'events':[{'type':'abs','data':{'axis':'x','value':int(x*32767/1280)}},{'type':'abs','data':{'axis':'y','value':int(y*32767/800)}}]})
            time.sleep(.015)
        for step in range(14):
            x=1250-(1250*(step+1)//14); y=740-(740*(step+1)//14)
            cmd('input-send-event',{'events':[{'type':'abs','data':{'axis':'x','value':int(x*32767/1280)}},{'type':'abs','data':{'axis':'y','value':int(y*32767/800)}}]})
            time.sleep(.015)
        time.sleep(.1)
        cmd('input-send-event',{'events':[{'type':'btn','data':{'button':'left','down':False}}]})
        time.sleep(.1)
        click(403,12) # Close the viewer clamped at top left.
        click(222,766) # Reopen/focus Terminal after every close.
    cmd('screendump',{'filename':str(out/'after-drag.ppm')})
    print('PASS: completed 24 viewer drag/close/terminal cycles',flush=True)
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
assert 'vexos-xhci-usb2-desktop-v4' in (out/'serial.log').read_text()
assert 'count=512' in (out/'serial.log').read_text(), 'Pointer did not cross two ring wraps'
assert '[KERNEL] Starting main loop' in (out/'serial.log').read_text()
assert '[EXCEPTION]' not in (out/'serial.log').read_text()
print('PASS: desktop reached main loop without an exception log')
