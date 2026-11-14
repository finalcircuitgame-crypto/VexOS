from pathlib import Path
s=Path('tools/ui_smoke.py').read_text().replace("out=root/'build/ui-review'", "out=root/'build/desktop-drag-review'").replace("sockpath='/tmp/vexos-ui-review.sock'", "sockpath='/tmp/vexos-desktop-drag.sock'")
a=s.index("    click(40,765)")
s=s[:a]+'''    # Repeat the reported sequence, crossing both edges and USB ring wraps.
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
'''+s[a:]
s=s.replace("assert '[KERNEL] Starting main loop'", "assert 'vexos-xhci-usb2-desktop-v4' in (out/'serial.log').read_text()\nassert 'count=512' in (out/'serial.log').read_text(), 'Pointer did not cross two ring wraps'\nassert '[KERNEL] Starting main loop'")
Path('tools/desktop_drag_test.py').write_text(s)
