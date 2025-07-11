// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// $Log:$
//
// DESCRIPTION:
//	DOOM graphics stuff for X11, UNIX.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include "config.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_event.h"
#include "d_main.h"
#include "i_video.h"
#include "z_zone.h"

#include "tables.h"
#include "doomkeys.h"

#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdarg.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

//#define CMAP256

struct fb_var_screeninfo fb = {};
int fb_scaling = 1;
int usemouse = 0;
int do_mmap = 0;

static uint32_t colors[256];

// The screen buffer; this is modified to draw things to the screen

byte *I_VideoBuffer = NULL;
byte *I_VideoBuffer_FB = NULL;
byte *I_VideoBuffer_Line = NULL;

/* framebuffer file descriptor */
int fd_fb = 0;

int	X_width;
int X_height;

// If true, game is running as a screensaver

boolean screensaver_mode = false;

// Flag indicating whether the screen is currently visible:
// when the screen isnt visible, don't render the screen

boolean screenvisible;

// Mouse acceleration
//
// This emulates some of the behavior of DOS mouse drivers by increasing
// the speed when the mouse is moved fast.
//
// The mouse input values are input directly to the game, but when
// the values exceed the value of mouse_threshold, they are multiplied
// by mouse_acceleration to increase the speed.

float mouse_acceleration = 2.0;
int mouse_threshold = 10;

// Gamma correction level to use

int usegamma = 0;

typedef struct
{
	byte r;
	byte g;
	byte b;
} col_t;

// Palette converted to RGB565

static uint16_t rgb565_palette[256];

// The actual colormap to fb function is decided at runtime based on a few
// parameters, to select the optimal one. (Of course, the fastest versions are
// those I care about!). Numbers are provided for an Allwinner F1C100s at 3x
// scaling, targetting 720p output.
//
// The first optimization is to perform regular memory accesses when possible,
// that is, when a pixel neatly maps to a 8, 16, or 32-bit word. This skips a
// call to memcpy, and a few arithmetic operations.
// - generic: ~12.0 FPS average
// - 32bpp:   ~28.5 FPS average, +16.5fps
//
// The second optimization is to manually unroll the loop for the scaling
// factor. It has been done for the few factors likely to be found on
// underpowered devices (because, yes, a 3GHz laptop can run this at 4K 60fps
// for a factor of 10x with no optimizations).
// - generic:        ~12.0 FPS average
// - 32bpp:          ~28.5 FPS average, +16.5fps
// - 32bpp unrolled: ~34.8 FPS average,  +6.3fps
//
// Lastly, we can use the fact our input line will always be 320px wide,
// enabling us to load 4 colors at a time in a register by loading 32-bit words.
// Bitshifts will be much faster than memory loads, with again a hand-unrolled
// loop.
// - generic:           ~12.0 FPS average
// - 32bpp:             ~28.5 FPS average, +16.5fps
// - 32bpp unrolled:    ~34.8 FPS average,  +6.3fps
// - 32bpp unrolled 4x: ~40.2 FPS average,  +5.4fps
//
// Overall, this now runs at 40fps, up from the base 12fps, freeing CPU time for
// other work.
static void (*cmap_to_fb)(uint8_t* out, uint8_t* in, int in_pixels);

static void cmap_to_fb_generic(uint8_t *out, uint8_t *in, int in_pixels) {
    uint32_t pix;

    for (int i = 0; i < in_pixels; i++) {
        pix = colors[*(in++)];  /* R:8 G:8 B:8 format! */
        for (int k = 0; k < fb_scaling; k++) {
            memcpy(out, &pix, fb.bits_per_pixel / 8);
            out += fb.bits_per_pixel / 8;
        }
    }
}

static void cmap_to_fb_32bpp_generic(uint8_t *out, uint8_t *in, int in_pixels) {
    uint32_t *out32 = (uint32_t*)out;
    // Side note: this CPU is so bad with branches that making the scaling loop
    // outside provides a measurable performance improvement.
    for (int k = 0; k < fb_scaling; k++)
        for (int i = 0; i < in_pixels; i++)
            out32[i * fb_scaling + k] = colors[in[i]];
}

static void cmap_to_fb_32bpp_1x(uint8_t *out, uint8_t *in, int in_pixels) {
    uint32_t *in32 = (uint32_t*)in, *out32 = (uint32_t*)out;

    for (int i = 0; i < in_pixels / 4; i++) {
        uint32_t pixels = *(in32++);

        *(out32++) = colors[pixels & 0xff];
        *(out32++) = colors[(pixels >>= 8) & 0xff];
        *(out32++) = colors[(pixels >>= 8) & 0xff];
        *(out32++) = colors[(pixels >>= 8) & 0xff];
    }
}

static void cmap_to_fb_32bpp_2x(uint8_t *out, uint8_t *in, int in_pixels) {
    uint32_t *in32 = (uint32_t*)in, *out32 = (uint32_t*)out;

    for (int i = 0; i < in_pixels / 4; i++) {
        uint32_t pix, pixels = *(in32++);

        pix = colors[pixels & 0xff];
        *(out32++) = pix;
        *(out32++) = pix;
        pix = colors[(pixels >>= 8) & 0xff];
        *(out32++) = pix;
        *(out32++) = pix;
        pix = colors[(pixels >>= 8) & 0xff];
        *(out32++) = pix;
        *(out32++) = pix;
        pix = colors[(pixels >>= 8) & 0xff];
        *(out32++) = pix;
        *(out32++) = pix;
    }
}

static void cmap_to_fb_32bpp_3x(uint8_t *out, uint8_t *in, int in_pixels) {
    uint32_t *in32 = (uint32_t*)in, *out32 = (uint32_t*)out;
    for (int i = 0; i < in_pixels / 4; i++) {
        uint32_t pix, pixels = *(in32++);

        pix = colors[pixels & 0xff];
        *(out32++) = pix;
        *(out32++) = pix;
        *(out32++) = pix;
        pix = colors[(pixels >>= 8) & 0xff];
        *(out32++) = pix;
        *(out32++) = pix;
        *(out32++) = pix;
        pix = colors[(pixels >>= 8) & 0xff];
        *(out32++) = pix;
        *(out32++) = pix;
        *(out32++) = pix;
        pix = colors[(pixels >>= 8) & 0xff];
        *(out32++) = pix;
        *(out32++) = pix;
        *(out32++) = pix;
    }
}

void I_InitGraphics (void)
{
    int i;

    /* Open fbdev file descriptor */
    fd_fb = open("/dev/fb0", O_RDWR);
    if (fd_fb < 0)
    {
        printf("Could not open /dev/fb0");
        exit(-1);
    }

    /* fetch framebuffer info */
    ioctl(fd_fb, FBIOGET_VSCREENINFO, &fb);
    /* change params if needed */
    //ioctl(fd_fb, FBIOPUT_VSCREENINFO, &fb);
    printf("I_InitGraphics: framebuffer: x_res: %d, y_res: %d, x_virtual: %d, y_virtual: %d, bpp: %d, grayscale: %d\n",
            fb.xres, fb.yres, fb.xres_virtual, fb.yres_virtual, fb.bits_per_pixel, fb.grayscale);

    i = M_CheckParmWithArgs("-bgra", 0);
    if (i > 0) {
        fb.red.offset = 0;
        fb.blue.offset = 16;
    }

    printf("I_InitGraphics: framebuffer: %s: %d%d%d%d, red_off: %d, green_off: %d, blue_off: %d, transp_off: %d\n",
            i > 0 ? "BGRA" : "RGBA", fb.red.length, fb.green.length, fb.blue.length, fb.transp.length, fb.red.offset, fb.green.offset, fb.blue.offset, fb.transp.offset);

    printf("I_InitGraphics: DOOM screen size: w x h: %d x %d\n", SCREENWIDTH, SCREENHEIGHT);


    i = M_CheckParmWithArgs("-scaling", 1);
    if (i > 0) {
        i = atoi(myargv[i + 1]);
        fb_scaling = i;
        printf("I_InitGraphics: Scaling factor: %d\n", fb_scaling);
    } else {
        fb_scaling = fb.xres / SCREENWIDTH;
        if (fb.yres / SCREENHEIGHT < fb_scaling)
            fb_scaling = fb.yres / SCREENHEIGHT;
        printf("I_InitGraphics: Auto-scaling factor: %d\n", fb_scaling);
    }

    do_mmap = !M_CheckParm("-nommap");

    /* Allocate screen to draw to */
    I_VideoBuffer = (byte*)Z_Malloc (SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);  // For DOOM to draw on
    if (do_mmap) {
        I_VideoBuffer_FB = mmap(NULL,
                                fb.xres * fb.yres * fb.bits_per_pixel / 8,
                                PROT_WRITE,
                                MAP_SHARED | MAP_NORESERVE,
                                fd_fb,
                                0);
        memset(I_VideoBuffer_FB, 0, fb.xres * fb.yres * fb.bits_per_pixel / 8);
        I_VideoBuffer_Line = (byte*)malloc(SCREENWIDTH * fb_scaling * fb.bits_per_pixel / 8);
    } else {
        // For a single write() syscall to fbdev
        I_VideoBuffer_FB = (byte*)malloc(fb.xres * fb.yres * (fb.bits_per_pixel/8));
    }

    if (fb.bits_per_pixel == 32)
        switch (fb_scaling) {
        case 1:  cmap_to_fb = cmap_to_fb_32bpp_1x; break;
        case 2:  cmap_to_fb = cmap_to_fb_32bpp_2x; break;
        case 3:  cmap_to_fb = cmap_to_fb_32bpp_3x; break;
        default: cmap_to_fb = cmap_to_fb_32bpp_generic;
        }
    else
        cmap_to_fb = cmap_to_fb_generic;

    screenvisible = true;

    extern int I_InitInput(void);
    I_InitInput();
}

void I_ShutdownGraphics (void)
{
	Z_Free (I_VideoBuffer);
	free(I_VideoBuffer_FB);
}

void I_StartFrame (void)
{

}

__attribute__ ((weak)) void I_GetEvent (void)
{
//	event_t event;
//	bool button_state;
//
//	button_state = button_read ();
//
//	if (last_button_state != button_state)
//	{
//		last_button_state = button_state;
//
//		event.type = last_button_state ? ev_keydown : ev_keyup;
//		event.data1 = KEY_FIRE;
//		event.data2 = -1;
//		event.data3 = -1;
//
//		D_PostEvent (&event);
//	}
//
//	touch_main ();
//
//	if ((touch_state.x != last_touch_state.x) || (touch_state.y != last_touch_state.y) || (touch_state.status != last_touch_state.status))
//	{
//		last_touch_state = touch_state;
//
//		event.type = (touch_state.status == TOUCH_PRESSED) ? ev_keydown : ev_keyup;
//		event.data1 = -1;
//		event.data2 = -1;
//		event.data3 = -1;
//
//		if ((touch_state.x > 49)
//		 && (touch_state.x < 72)
//		 && (touch_state.y > 104)
//		 && (touch_state.y < 143))
//		{
//			// select weapon
//			if (touch_state.x < 60)
//			{
//				// lower row (5-7)
//				if (touch_state.y < 119)
//				{
//					event.data1 = '5';
//				}
//				else if (touch_state.y < 131)
//				{
//					event.data1 = '6';
//				}
//				else
//				{
//					event.data1 = '1';
//				}
//			}
//			else
//			{
//				// upper row (2-4)
//				if (touch_state.y < 119)
//				{
//					event.data1 = '2';
//				}
//				else if (touch_state.y < 131)
//				{
//					event.data1 = '3';
//				}
//				else
//				{
//					event.data1 = '4';
//				}
//			}
//		}
//		else if (touch_state.x < 40)
//		{
//			// button bar at bottom screen
//			if (touch_state.y < 40)
//			{
//				// enter
//				event.data1 = KEY_ENTER;
//			}
//			else if (touch_state.y < 80)
//			{
//				// escape
//				event.data1 = KEY_ESCAPE;
//			}
//			else if (touch_state.y < 120)
//			{
//				// use
//				event.data1 = KEY_USE;
//			}
//			else if (touch_state.y < 160)
//			{
//				// map
//				event.data1 = KEY_TAB;
//			}
//			else if (touch_state.y < 200)
//			{
//				// pause
//				event.data1 = KEY_PAUSE;
//			}
//			else if (touch_state.y < 240)
//			{
//				// toggle run
//				if (touch_state.status == TOUCH_PRESSED)
//				{
//					run = !run;
//
//					event.data1 = KEY_RSHIFT;
//
//					if (run)
//					{
//						event.type = ev_keydown;
//					}
//					else
//					{
//						event.type = ev_keyup;
//					}
//				}
//				else
//				{
//					return;
//				}
//			}
//			else if (touch_state.y < 280)
//			{
//				// save
//				event.data1 = KEY_F2;
//			}
//			else if (touch_state.y < 320)
//			{
//				// load
//				event.data1 = KEY_F3;
//			}
//		}
//		else
//		{
//			// movement/menu navigation
//			if (touch_state.x < 100)
//			{
//				if (touch_state.y < 100)
//				{
//					event.data1 = KEY_STRAFE_L;
//				}
//				else if (touch_state.y < 220)
//				{
//					event.data1 = KEY_DOWNARROW;
//				}
//				else
//				{
//					event.data1 = KEY_STRAFE_R;
//				}
//			}
//			else if (touch_state.x < 180)
//			{
//				if (touch_state.y < 160)
//				{
//					event.data1 = KEY_LEFTARROW;
//				}
//				else
//				{
//					event.data1 = KEY_RIGHTARROW;
//				}
//			}
//			else
//			{
//				event.data1 = KEY_UPARROW;
//			}
//		}
//
//		D_PostEvent (&event);
//	}
}

__attribute__ ((weak)) void I_StartTic (void)
{
	I_GetEvent();
}

void I_UpdateNoBlit (void)
{
}

//
// I_FinishUpdate
//

void I_FinishUpdate (void)
{
    int y, dy;
    int x_offset, y_offset, x_offset_end, bpp, line_w;
    unsigned char *line_in, *line_out;

    bpp = fb.bits_per_pixel / 8;
    /* Offsets in case FB is bigger than DOOM */
    /* 600 = fb heigt, 200 screenheight */
    /* 600 = fb heigt, 200 screenheight */
    /* 2048 =fb width, 320 screenwidth */
    y_offset     = (fb.yres - SCREENHEIGHT * fb_scaling) * bpp / 2;
    // XXX: siglent FB hack: /4 instead of /2, since it seems to handle the resolution in a funny way
    x_offset     = (fb.xres - SCREENWIDTH  * fb_scaling) * bpp / 2;
    //x_offset     = 0;
    x_offset_end = (fb.xres - SCREENWIDTH  * fb_scaling) * bpp - x_offset;
    line_w = SCREENWIDTH * fb_scaling * bpp;

    /* DRAW SCREEN */
    line_in  = (unsigned char *) I_VideoBuffer;
    line_out = (unsigned char *) I_VideoBuffer_FB;

    y = SCREENHEIGHT;

    if (do_mmap)
        line_out += y_offset * fb.xres;

    while (y--) {
        uint8_t *line = do_mmap ? I_VideoBuffer_Line : line_out + x_offset;
        cmap_to_fb((void*)line, (void*)line_in, SCREENWIDTH);

        dy = 0;
        if (!do_mmap) {
            line_out += x_offset + line_w + x_offset_end;
            dy = 1;
        }

        for (; dy < fb_scaling; dy++) {
            line_out += x_offset;
            memcpy(line_out, line, line_w);
            line_out += line_w + x_offset_end;
        }
        line_in += SCREENWIDTH;
    }

    /* Start drawing from y-offset */
    if (!do_mmap) {
        lseek(fd_fb, y_offset * fb.xres, SEEK_SET);
        // draw only portion used by doom + x-offsets
        write(fd_fb, I_VideoBuffer_FB, (SCREENHEIGHT * fb_scaling * bpp) * fb.xres);
    }
}

//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

//
// I_SetPalette
//
#define GFX_RGB565(r, g, b)			((((r & 0xF8) >> 3) << 11) | (((g & 0xFC) >> 2) << 5) | ((b & 0xF8) >> 3))
#define GFX_RGB565_R(color)			((0xF800 & color) >> 11)
#define GFX_RGB565_G(color)			((0x07E0 & color) >> 5)
#define GFX_RGB565_B(color)			(0x001F & color)

void I_SetPalette (byte* palette)
{
    uint16_t r, g, b;
    uint32_t pix;
    int i;

    for (i = 0; i < 256; i++) {
        r = (uint16_t)gammatable[usegamma][*palette++];
        g = (uint16_t)gammatable[usegamma][*palette++];
        b = (uint16_t)gammatable[usegamma][*palette++];
        r = (r >> (8 - fb.red.length));
        g = (g >> (8 - fb.green.length));
        b = (b >> (8 - fb.blue.length));
        pix = (r << fb.red.offset | g << fb.green.offset | b << fb.blue.offset);
        colors[i] = pix;
    }

    /* Set new color map in kernel framebuffer driver */
    //XXX FIXME ioctl(fd_fb, IOCTL_FB_PUTCMAP, colors);
}

// Given an RGB value, find the closest matching palette index.

int I_GetPaletteIndex (int r, int g, int b)
{
    int best, best_diff, diff;
    int i;
    col_t color;

    printf("I_GetPaletteIndex\n");

    best = 0;
    best_diff = INT_MAX;

    for (i = 0; i < 256; ++i)
    {
    	color.r = GFX_RGB565_R(rgb565_palette[i]);
    	color.g = GFX_RGB565_G(rgb565_palette[i]);
    	color.b = GFX_RGB565_B(rgb565_palette[i]);

        diff = (r - color.r) * (r - color.r)
             + (g - color.g) * (g - color.g)
             + (b - color.b) * (b - color.b);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
        }

        if (diff == 0)
        {
            break;
        }
    }

    return best;
}

void I_BeginRead (void)
{
}

void I_EndRead (void)
{
}

void I_SetWindowTitle (char *title)
{
}

void I_GraphicsCheckCommandLine (void)
{
}

void I_SetGrabMouseCallback (grabmouse_callback_t func)
{
}

void I_EnableLoadingDisk(void)
{
}

void I_BindVideoVariables (void)
{
}

void I_DisplayFPSDots (boolean dots_on)
{
}

void I_CheckIsScreensaver (void)
{
}
//}
