#include <stdint.h>
#include "doomgeneric.h"
#include "doomkeys.h"
#include "d_event.h"
#define SYS_EXIT 0u
#define SYS_TICKS 7u
#define SYS_DRAW 9u
#define SYS_KEY_EVENT 10u
#define SYS_APP_READY 11u
#define SYS_MOUSE_EVENT 12u
#define SYS_SHOULD_CLOSE 13u
static uint32_t mouse_buttons;
static inline uint64_t vex_syscall0(uint64_t n){uint64_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n):"memory","rcx","r11");return r;}
static inline uint64_t vex_syscall3(uint64_t n,uint64_t a,uint64_t b,uint64_t c){uint64_t r;__asm__ volatile("int $0x80":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"memory","rcx","r11");return r;}
void DG_Init(void){mouse_buttons=0;(void)vex_syscall0(SYS_APP_READY);}
void DG_SetWindowTitle(const char *title){(void)title;}
void DG_DrawFrame(void){if(DG_ScreenBuffer)(void)vex_syscall3(SYS_DRAW,(uint64_t)(uintptr_t)DG_ScreenBuffer,DOOMGENERIC_RESX,DOOMGENERIC_RESY);}
uint32_t DG_GetTicksMs(void){return (uint32_t)vex_syscall0(SYS_TICKS);}
void DG_SleepMs(uint32_t ms){uint32_t start=DG_GetTicksMs();while((uint32_t)(DG_GetTicksMs()-start)<ms)__asm__ volatile("pause");}
int DG_ShouldClose(void){return vex_syscall0(SYS_SHOULD_CLOSE)!=0;}
__attribute__((noreturn)) void DG_Exit(void){(void)vex_syscall0(SYS_EXIT);for(;;)__asm__ volatile("pause");}
static void poll_mouse(void){
 uint64_t v=vex_syscall0(SYS_MOUSE_EVENT);
 int16_t dx=(int16_t)(uint16_t)(v&0xffffu),dy=(int16_t)(uint16_t)((v>>16)&0xffffu);
 uint32_t buttons=(uint32_t)((v>>32)&7u);
 if(dx||dy||buttons!=mouse_buttons){event_t ev;ev.type=ev_mouse;ev.data1=(int)buttons;ev.data2=(int)dx;ev.data3=-(int)dy;ev.data4=0;D_PostEvent(&ev);mouse_buttons=buttons;}
}
static unsigned char doom_key_from_hid(uint8_t u){
 if(u>=0x04&&u<=0x1d)return (unsigned char)('a'+u-0x04);
 if(u>=0x1e&&u<=0x26)return (unsigned char)('1'+u-0x1e);
 if(u==0x27)return '0';if(u==0x28)return KEY_ENTER;if(u==0x29)return KEY_ESCAPE;
 if(u==0x2a)return KEY_BACKSPACE;if(u==0x2b)return KEY_TAB;if(u==0x2c)return KEY_USE;
 if(u==0x2d)return KEY_MINUS;if(u==0x2e)return KEY_EQUALS;
 if(u>=0x3a&&u<=0x45)return (unsigned char)(KEY_F1+u-0x3a);
 if(u==0x4f)return KEY_RIGHTARROW;if(u==0x50)return KEY_LEFTARROW;
 if(u==0x51)return KEY_DOWNARROW;if(u==0x52)return KEY_UPARROW;
 if(u==0xe0||u==0xe4)return KEY_FIRE;if(u==0xe2||u==0xe6)return KEY_RALT;
 if(u==0xe1||u==0xe5)return KEY_RSHIFT;return 0;
}
int DG_GetKey(int *pressed,unsigned char *key){
 poll_mouse();uint64_t ev=vex_syscall0(SYS_KEY_EVENT);if(!(ev&0x10000u))return 0;
 unsigned char mapped=doom_key_from_hid((uint8_t)(ev&0xffu));if(!mapped)return 0;
 if(pressed)*pressed=(int)((ev>>8)&1u);if(key)*key=mapped;return 1;
}
