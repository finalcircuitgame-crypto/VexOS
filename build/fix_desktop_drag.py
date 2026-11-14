from pathlib import Path
p=Path('src/kernel/core/desktop.c');s=p.read_text();s=s.replace('static uint32_t *g_bg;', 'static uint32_t *g_bg;\nstatic uint8_t g_clip_enabled;\nstatic uint32_t g_clip_x, g_clip_y, g_clip_x2, g_clip_y2;')
a=s.index('static void blit_rect(')
s=s[:a]+'''static uint8_t app_visible(app_id_t app) {
  switch (app) {
  case APP_SHELL: return g_show_shell;
  case APP_EXPLORER: return g_explorer_open;
  case APP_VIEWER: return g_viewer_open;
  case APP_NOTEPAD: return g_app_open[app];
  case APP_SETUP: return g_setup_open;
  default: return 0;
  }
}
static wm_window_t *app_window(app_id_t app) {
  wm_window_t *windows[APP_COUNT] = {&g_shell_win,&g_explorer_win,&g_viewer_win,&g_notepad_win,&g_setup_win};
  return app < APP_COUNT ? windows[app] : NULL;
}
static int pointer_window(int32_t x, int32_t y) {
  if (app_visible(g_app_focus)) {
    wm_window_t *w=app_window(g_app_focus);
    if (x>=w->x && x<w->x+w->w && y>=w->y && y<w->y+w->h) return g_app_focus;
  }
  for (int i=APP_COUNT-1;i>=0;i--) {
    if ((app_id_t)i==g_app_focus || !app_visible((app_id_t)i)) continue;
    wm_window_t *w=app_window((app_id_t)i);
    if (x>=w->x && x<w->x+w->w && y>=w->y && y<w->y+w->h) return i;
  }
  return -1;
}
static void draw_app(app_id_t app) {
  switch(app) {
  case APP_SHELL: draw_shell_window(&g_shell_win); break;
  case APP_EXPLORER: draw_explorer_window(&g_explorer_win); break;
  case APP_VIEWER: draw_viewer_window(&g_viewer_win); break;
  case APP_NOTEPAD: draw_notepad_window(&g_notepad_win); break;
  case APP_SETUP: draw_setup_window(&g_setup_win); break;
  default: break;
  }
}
static void draw_open_windows(void) {
  for (uint32_t i=0;i<APP_COUNT;i++)
    if ((app_id_t)i!=g_app_focus && app_visible((app_id_t)i)) draw_app((app_id_t)i);
  if (app_visible(g_app_focus)) draw_app(g_app_focus);
}
static void clamp_window(wm_window_t *w) {
  int32_t usable_h=g_bi->height>56 ? (int32_t)g_bi->height-56 : (int32_t)g_bi->height;
  if (w->w>(int32_t)g_bi->width) w->w=g_bi->width;
  if (w->h>usable_h) w->h=usable_h;
  int32_t max_x=(int32_t)g_bi->width-w->w, max_y=usable_h-w->h;
  if(w->x<0) w->x=0; if(w->x>max_x) w->x=max_x;
  if(w->y<0) w->y=0; if(w->y>max_y) w->y=max_y;
}
static int move_window(wm_window_t *w, int32_t x, int32_t y, wm_window_t *damage, uint8_t *valid) {
  wm_window_t old=*w; w->x=x; w->y=y; clamp_window(w);
  if (w->x==old.x && w->y==old.y) return 0;
  int32_t left=old.x<w->x ? old.x : w->x, top=old.y<w->y ? old.y : w->y;
  int32_t right=(old.x>w->x?old.x:w->x)+w->w+4;
  int32_t bottom=(old.y>w->y?old.y:w->y)+w->h+4;
  if (*valid) {
    if (damage->x<left) left=damage->x;
    if (damage->y<top) top=damage->y;
    if (damage->x+damage->w>right) right=damage->x+damage->w;
    if (damage->y+damage->h>bottom) bottom=damage->y+damage->h;
  }
  *damage=(wm_window_t){left,top,right-left,bottom-top}; *valid=1;
  return 1;
}

'''+s[a:]
# Clip all bulk primitive loops and blits to the damage rectangle.
s=s.replace('  for (uint32_t yy = 0; yy < h; yy++) {\n    uint32_t *drow', '''  if (g_clip_enabled) {
    uint32_t end_x=x+w, end_y=y+h;
    if (x<g_clip_x) x=g_clip_x; if (y<g_clip_y) y=g_clip_y;
    if (end_x>g_clip_x2) end_x=g_clip_x2; if (end_y>g_clip_y2) end_y=g_clip_y2;
    if (x>=end_x || y>=end_y) return;
    w=end_x-x; h=end_y-y;
  }
  for (uint32_t yy = 0; yy < h; yy++) {
    uint32_t *drow''',1)
s=s.replace('    for (uint32_t xx = 0; xx < w; xx++) {\n      drow[xx] = srow[xx];\n    }','    size_t pixels=w;\n    __asm__ volatile("cld; rep movsl" : "+D"(drow), "+S"(srow), "+c"(pixels) :: "memory");',1)
# put_px checks signed-cast bounds first, then the active repaint bounds.
pos=s.index('static inline void put_px(',s.index('void desktop_draw_frame'))
z=s.index('\n}',pos);part=s[pos:z]
part=part.replace('  uint32_t *dst', '  if (g_clip_enabled && (x<g_clip_x || x>=g_clip_x2 || y<g_clip_y || y>=g_clip_y2)) return;\n  uint32_t *dst');s=s[:pos]+part+s[z:]
# Clip loops while retaining rounded-corner geometry based on original edges.
for name in ['draw_rect','blend_rect','blend_round_rect']:
 a=s.index('static void '+name+'(',s.index('static void draw_widgets(void) {')); z=s.index('\n}\n',a);part=s[a:z]
 part=part.replace('  uint32_t *dst', '''  uint32_t start_x=x, start_y=y, end_x=x2, end_y=y2;
  if (g_clip_enabled) {
    if (start_x<g_clip_x) start_x=g_clip_x; if (start_y<g_clip_y) start_y=g_clip_y;
    if (end_x>g_clip_x2) end_x=g_clip_x2; if (end_y>g_clip_y2) end_y=g_clip_y2;
  }
  uint32_t *dst''',1)
 part=part.replace('yy = y; yy < y2', 'yy = start_y; yy < end_y').replace('xx = x; xx < x2','xx = start_x; xx < end_x')
 if name!='draw_rect': part=part.replace('blend_rgb(row[xx], c, a)', 'a==255 ? c : blend_rgb(row[xx], c, a)')
 s=s[:a]+part+s[z:]
# Route a press to the visible topmost window only.
s=s.replace('  // Click handling\n', '''  int clicked_window = pointer_window(p0.x,p0.y);
  if ((pressed&1u) && clicked_window>=0) {
    if (g_app_focus!=(app_id_t)clicked_window) { g_app_focus=(app_id_t)clicked_window; ui_changed=1; }
    g_shell_dragging=g_explorer_dragging=g_viewer_dragging=g_notepad_dragging=g_setup_dragging=0;
  }
  // Click handling
''',1)
a=s.index('    // Explorer window interactions');z=s.index('  if (!(buttons & 1u))',a);part=s[a:z]
for cond,app in [('g_explorer_open','APP_EXPLORER'),('g_viewer_open','APP_VIEWER'),('g_app_open[APP_NOTEPAD]','APP_NOTEPAD'),('g_setup_open','APP_SETUP'),('g_show_shell','APP_SHELL')]:
 part=part.replace('if ('+cond+') {', 'if ('+cond+' && clicked_window=='+app+') {')
for state,app in [('g_explorer_open','APP_EXPLORER'),('g_viewer_open','APP_VIEWER'),('g_show_shell','APP_SHELL')]:
 part=part.replace(state+' = 0;',state+' = 0;\n        g_app_open['+app+'] = 0;')
s=s[:a]+part+s[z:]
# Replace repetitive draw blocks with ordering matching hit testing.
for indent in ['    ','      ']:
 block=''
 for cond,fn,w in [('g_show_shell','draw_shell_window','g_shell_win'),('g_explorer_open','draw_explorer_window','g_explorer_win'),('g_viewer_open','draw_viewer_window','g_viewer_win'),('g_app_open[APP_NOTEPAD]','draw_notepad_window','g_notepad_win'),('g_setup_open','draw_setup_window','g_setup_win')]:
  block+=indent+'if ('+cond+') {\n'+indent+'  '+fn+'(&'+w+');\n'+indent+'}\n'
 s=s.replace(block,indent+'draw_open_windows();\n')
a=s.index('  if (g_shell_dragging &&');z=s.index('  if (!g_back || !g_bg)',a)
s=s[:a]+'''  uint8_t full_redraw=ui_changed;
  wm_window_t damage={0}; uint8_t damage_valid=0;
'''+''.join('  if ('+drag+') ui_changed |= move_window(&'+w+', p0.x - '+off+'_x, p0.y - '+off+'_y, &damage, &damage_valid);\n' for drag,w,off in [('g_shell_dragging','g_shell_win','g_shell_drag_off'),('g_explorer_dragging','g_explorer_win','g_explorer_drag_off'),('g_viewer_dragging','g_viewer_win','g_viewer_drag_off'),('g_notepad_dragging','g_notepad_win','g_notepad_drag_off'),('g_setup_dragging','g_setup_win','g_setup_drag_off')])+'''  if (!app_visible(g_app_focus)) {
    for(int i=APP_COUNT-1;i>=0;i--) if(app_visible((app_id_t)i)) { g_app_focus=(app_id_t)i; break; }
  }

'''+s[z:]
s=s.replace('    if (scene_changed) {\n      blit_rect(g_back', '''    uint8_t partial=scene_changed && damage_valid && !full_redraw && g_tick>=2;
    if (scene_changed) {
      if (partial) {
        g_clip_enabled=1; g_clip_x=damage.x; g_clip_y=damage.y;
        g_clip_x2=damage.x+damage.w; g_clip_y2=damage.y+damage.h;
      }
      blit_rect(g_back''',1)
s=s.replace('      g_desktop_dirty = 0;', '      g_clip_enabled=0;\n      g_desktop_dirty = 0;',1)
s=s.replace('        blit_rect(g_bi->framebuffer, g_back, 0, 0, g_bi->width, g_bi->height);\n      } else {', '''        if (partial) blit_rect(g_bi->framebuffer,g_back,damage.x,damage.y,damage.w,damage.h);
        else blit_rect(g_bi->framebuffer, g_back, 0, 0, g_bi->width, g_bi->height);
      }
      if (!scene_changed || partial) {''',1)
s=s.replace('  g_bg = 0;', '  g_bg = 0; g_clip_enabled=0;',1)
s=s.replace('  g_setup_open = 0; // Closed', '  for(uint32_t i=0;i<APP_COUNT;i++) clamp_window(app_window((app_id_t)i));\n  g_setup_open = 0; // Closed',1)
p.write_text(s)
p=Path('src/kernel/core/main.c');s=p.read_text().replace('xhci-portscan-v2','vexos-xhci-usb2-desktop-v4');p.write_text(s)
p=Path('run.sh');s=p.read_text().replace('set -e\n', 'set -e\n\n# Resolve images/builds relative to this script, regardless of caller cwd.\ncd -- "$(dirname -- "${BASH_SOURCE[0]}")"\necho "Launching VexOS from $(pwd)"\n',1);p.write_text(s)
p=Path('Makefile');s=p.read_text().replace('$(OBJDIR)/%.o: $(KERNELDIR)/%.c','$(OBJDIR)/%.o: $(KERNELDIR)/%.c Makefile').replace('$(CC) $(CFLAGS_KERNEL) -c $< -o $@','$(CC) $(CFLAGS_KERNEL) -MMD -MP -c $< -o $@');s+='\n# Track header changes so a successful make cannot silently reuse old headers.\n-include $(KERNEL_OBJS:.o=.d)\n';p.write_text(s)
