#include "doom_window.h"
#include "../../../../include/heap.h"
#include "../../window_manager.h"
#include "../../graphics/graphics.h"
#define DOOM_W 640u
#define DOOM_H 400u
extern void serial_print(const char *str);
#define DOOM_PIXELS (DOOM_W*DOOM_H)
static uint32_t screen_w,screen_h;
static desktop_window_t win,restore_win;
static uint32_t *framebuffer;
static uint32_t frame_w,frame_h;
static uint8_t frame_ready,is_open,maximized,dragging,close_requested;
static int32_t drag_x,drag_y;
static int32_t clamp_i32(int32_t v,int32_t lo,int32_t hi){if(v<lo)return lo;if(v>hi)return hi;return v;}
static int inside(int32_t px,int32_t py,int32_t x,int32_t y,int32_t w,int32_t h){return px>=x&&py>=y&&px<x+w&&py<y+h;}
static void make_restore_rect(void){
 int32_t w=(int32_t)(screen_w>760u?720u:(screen_w>48u?screen_w-48u:screen_w));
 int32_t h=(int32_t)(screen_h>560u?500u:(screen_h>80u?screen_h-80u:screen_h));
 if(w<240&&screen_w>=240u)w=240;if(h<160&&screen_h>=160u)h=160;
 if(w>(int32_t)screen_w)w=(int32_t)screen_w;if(h>(int32_t)screen_h)h=(int32_t)screen_h;
 restore_win.w=w;restore_win.h=h;restore_win.x=((int32_t)screen_w-w)/2;restore_win.y=((int32_t)screen_h-h)/2;
 if(restore_win.y<8)restore_win.y=8;
}
void doom_window_init(uint32_t w,uint32_t h){
 screen_w=w?w:DOOM_W;screen_h=h?h:DOOM_H;make_restore_rect();win=restore_win;
 framebuffer=NULL;frame_w=DOOM_W;frame_h=DOOM_H;
 frame_ready=is_open=maximized=dragging=close_requested=0;drag_x=drag_y=0;
}
int doom_window_open(void){
 if(!framebuffer)framebuffer=(uint32_t*)kmalloc(DOOM_PIXELS*sizeof(uint32_t));
 if(!framebuffer){close_requested=1;return 0;}
 make_restore_rect();win.x=0;win.y=0;win.w=(int32_t)screen_w;win.h=(int32_t)screen_h;
 maximized=1;dragging=close_requested=frame_ready=0;is_open=1;return 1;
}
void doom_window_app_exited(void){
 is_open=maximized=dragging=close_requested=frame_ready=0;win=restore_win;
 if(framebuffer){kfree(framebuffer);framebuffer=NULL;}
}
int doom_window_draw_frame(const uint32_t *src,uint32_t w,uint32_t h){
 if(!is_open||!framebuffer||!src||!w||!h||w>DOOM_W||h>DOOM_H)return 0;
 for(uint32_t y=0;y<h;y++)for(uint32_t x=0;x<w;x++)framebuffer[y*DOOM_W+x]=src[y*w+x];
 frame_w=w;frame_h=h;frame_ready=1;return 1;
}
int doom_window_contains(int32_t x,int32_t y){return is_open&&inside(x,y,win.x,win.y,win.w,win.h);}
int doom_window_pointer_in_content(int32_t x,int32_t y){return is_open&&win.w>2&&win.h>32&&inside(x,y,win.x+1,win.y+31,win.w-2,win.h-32);}
int doom_window_is_maximized(void){return is_open&&maximized;}
int doom_window_close_requested(void){return close_requested?1:0;}
static void toggle_maximize(void){
 if(!is_open)return;
 if(maximized){win=restore_win;maximized=0;}
 else{restore_win=win;win.x=win.y=0;win.w=(int32_t)screen_w;win.h=(int32_t)screen_h;maximized=1;dragging=0;}
}
int doom_window_handle_click(int32_t x,int32_t y){
 if(!doom_window_contains(x,y))return 0;
 wm_raise(APP_DOOM);
 if(inside(x,y,win.x+win.w-24,win.y+5,20,20)){
  close_requested=1;dragging=0;
  serial_print("[DOOMWIN] Close button hit\\n");
 }
 else if(inside(x,y,win.x+win.w-46,win.y+5,20,20))toggle_maximize();
 else if(inside(x,y,win.x,win.y,win.w-68,30)){
  if(maximized){win=restore_win;maximized=0;win.x=clamp_i32(x-win.w/2,0,(int32_t)screen_w-win.w);win.y=clamp_i32(y-15,0,(int32_t)screen_h-30);}
  drag_x=x-win.x;drag_y=y-win.y;dragging=1;
 }
 return 1;
}
int doom_window_pointer_update(int32_t x,int32_t y,uint32_t buttons,int32_t dx,int32_t dy){
 if(!(buttons&1u))dragging=0;if(!is_open||!dragging||(!dx&&!dy))return 0;
 int32_t mx=(int32_t)screen_w-win.w,my=(int32_t)screen_h-win.h;if(mx<0)mx=0;if(my<0)my=0;
 int32_t nx=clamp_i32(x-drag_x,0,mx),ny=clamp_i32(y-drag_y,0,my);
 if(nx==win.x&&ny==win.y)return 0;win.x=nx;win.y=ny;return 1;
}
void doom_window_draw(void){
 if(!is_open||win.w<40||win.h<40)return;
 int32_t x=win.x,y=win.y,w=win.w,h=win.h;uint32_t focus=(wm_get_focus()==APP_DOOM);
 gfx_blend_round_rect((uint32_t)(x+4),(uint32_t)(y+6),(uint32_t)w,(uint32_t)h,8u,0x02060Cu,155u);
 gfx_blend_round_rect((uint32_t)x,(uint32_t)y,(uint32_t)w,(uint32_t)h,7u,0x101A28u,255u);
 gfx_draw_rect((uint32_t)x,(uint32_t)y,(uint32_t)w,1u,focus?0x5EA9F7u:0x3B4C62u);
 gfx_draw_rect((uint32_t)x,(uint32_t)y,1u,(uint32_t)h,0x35485Fu);
 gfx_draw_rect((uint32_t)(x+w-1),(uint32_t)y,1u,(uint32_t)h,0x26364Au);
 gfx_draw_rect((uint32_t)(x+1),(uint32_t)(y+1),(uint32_t)(w-2),29u,focus?0x1D3550u:0x1A293Bu);
 gfx_draw_rect((uint32_t)(x+1),(uint32_t)(y+29),(uint32_t)(w-2),1u,0x42688Cu);
 gfx_blend_round_rect((uint32_t)(x+8),(uint32_t)(y+6),20u,20u,5u,0x5B2E3Au,255u);
 gfx_draw_string((uint32_t)(x+14),(uint32_t)(y+12),"D",0xFFFFFFu);
 gfx_draw_string((uint32_t)(x+36),(uint32_t)(y+11),"Doom",focus?0xF5F8FCu:0xC0CAD8u);
 gfx_draw_rect((uint32_t)(x+80),(uint32_t)(y+10),1u,10u,0x59708Au);
 gfx_draw_string((uint32_t)(x+90),(uint32_t)(y+11),"Freedoom Phase 1",0x9EB5CCu);
 uint32_t mx=(uint32_t)(x+w-45);
 gfx_blend_round_rect(mx,(uint32_t)(y+6),17u,17u,5u,0x334B65u,255u);
 gfx_draw_rect(mx+5u,(uint32_t)(y+10),8u,7u,0xE8F1FAu);
 gfx_draw_rect(mx+7u,(uint32_t)(y+12),8u,7u,0x334B65u);
 gfx_draw_rect(mx+8u,(uint32_t)(y+13),6u,5u,0xE8F1FAu);
 gfx_blend_round_rect((uint32_t)(x+w-23),(uint32_t)(y+6),17u,17u,5u,0xB94755u,255u);
 gfx_draw_string((uint32_t)(x+w-18),(uint32_t)(y+10),"x",0xFFFFFFu);
 int32_t cx=x+1,cy=y+31,cw=w-2,ch=h-32;if(cw<=0||ch<=0)return;
 gfx_draw_rect((uint32_t)cx,(uint32_t)cy,(uint32_t)cw,(uint32_t)ch,0x05070Au);
 if(!frame_ready||!framebuffer||!frame_w||!frame_h){gfx_draw_string((uint32_t)(cx+16),(uint32_t)(cy+16),"Starting Doom...",0xB7C7D9u);return;}
 uint32_t dw=(uint32_t)cw,dh=(uint32_t)(((uint64_t)dw*frame_h)/frame_w);
 if(dh>(uint32_t)ch){dh=(uint32_t)ch;dw=(uint32_t)(((uint64_t)dh*frame_w)/frame_h);}
 if(!dw||!dh)return;
 uint32_t dx=(uint32_t)cx+((uint32_t)cw-dw)/2u,dy=(uint32_t)cy+((uint32_t)ch-dh)/2u;
 gfx_blit_scaled(framebuffer,frame_w,frame_h,DOOM_W,dx,dy,dw,dh);
}
