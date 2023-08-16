//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
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
// DESCRIPTION:
//	DOOM keyboard input via linux tty
//


#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <linux/keyboard.h>
#include <linux/kd.h>

#include "config.h"
#include "deh_str.h"
#include "doomtype.h"
#include "doomkeys.h"
#include "i_input_at.h"
#include "i_joystick.h"
#include "i_system.h"
#include "i_swap.h"
#include "i_timer.h"
#include "i_video.h"
#include "i_scale.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "tables.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

int vanilla_keyboard_mapping = 1;

// Should we take no keyboard input?
static int no_kb = 0;

/* Checks whether or not the given file descriptor is associated
   with a local keyboard.
   Returns 1 if it is, 0 if not (or if something prevented us from
   checking). */

int tty_is_kbd(int fd)
{
    int data = 0;

    if (ioctl(fd, KDGKBTYPE, &data) != 0)
        return 0;

    if (data == KB_84) {
        printf("84-key keyboard found.\n");
        return 1;
    } else if (data == KB_101) {
        printf("101-key keyboard found.\n");
        return 1;
    } else {
        printf("KDGKBTYPE = 0x%x.\n", data);
        return 0;
    }
}

static int old_mode = -1;
static struct termios old_term;
static int kb = -1; /* keyboard file descriptor */

void kbd_shutdown(void)
{
    /* Shut down nicely. */

    printf("Cleaning up.\n");
    fflush(stdout);

    printf("Exiting normally.\n");
    if (old_mode != -1) {
        ioctl(kb, KDSKBMODE, old_mode);
        tcsetattr(kb, 0, &old_term);
    }

    if (kb > 3)
        close(kb);

    exit(0);
}

static int kbd_init(void)
{
    struct termios new_term;
    char *files_to_try[] = {"/dev/tty", "/dev/tty0", "/dev/console", NULL};
    int i;
    int flags;
    int found = 0;

    /* First we need to find a file descriptor that represents the
       system's keyboard. This should be /dev/tty, /dev/console,
       stdin, stdout, or stderr. We'll try them in that order.
       If none are acceptable, we're probably not being run
       from a VT. */
    for (i = 0; files_to_try[i] != NULL; i++) {
        /* Try to open the file. */
        kb = open(files_to_try[i], O_RDONLY);
        if (kb < 0) continue;
        /* See if this is valid for our purposes. */
        if (tty_is_kbd(kb)) {
            printf("Using keyboard on %s.\n", files_to_try[i]);
            found = 1;
            break;
        }
        close(kb);
    }

    /* If those didn't work, not all is lost. We can try the
       3 standard file descriptors, in hopes that one of them
       might point to a console. This is not especially likely. */
    if (files_to_try[i] == NULL) {
        for (kb = 0; kb < 3; kb++) {
            if (tty_is_kbd(i)) {
                found = 1;
                break;
            }
        }
    }

    if (!found) {
        printf("Unable to find a file descriptor associated with "\
                "the keyboard.\n" \
                "Perhaps you're not using a virtual terminal?\n");
        return 1;
    }

    /* Find the keyboard's mode so we can restore it later. */
    if (ioctl(kb, KDGKBMODE, &old_mode) != 0) {
        printf("Unable to query keyboard mode.\n");
        kbd_shutdown();
    }

    /* Adjust the terminal's settings. In particular, disable
       echoing, signal generation, and line buffering. Any of
       these could cause trouble. Save the old settings first. */
    if (tcgetattr(kb, &old_term) != 0) {
        printf("Unable to query terminal settings.\n");
        kbd_shutdown();
    }

    new_term = old_term;
    new_term.c_iflag = 0;
    new_term.c_lflag &= ~(ECHO | ICANON | ISIG);

    /* TCSAFLUSH discards unread input before making the change.
       A good idea. */
    if (tcsetattr(kb, TCSAFLUSH, &new_term) != 0) {
        printf("Unable to change terminal settings.\n");
    }
    
    /* Put the keyboard in mediumraw mode. */
    if (ioctl(kb, KDSKBMODE, K_MEDIUMRAW) != 0) {
        printf("Unable to set mediumraw mode.\n");
        kbd_shutdown();
    }

    /* Put in non-blocking mode */
    flags = fcntl(kb, F_GETFL, 0);
    fcntl(kb, F_SETFL, flags | O_NONBLOCK);

    printf("Ready to read keycodes. Press Backspace to exit.\n");

    return 0;
}

int kbd_read(int *pressed, unsigned char *key)
{
    unsigned char data;

    if (read(kb, &data, 1) < 1) {
        return 0;
    }

    *pressed = (data & 0x80) == 0x80;
    *key = data & 0x7F;

    /* Print the keycode. The top bit is the pressed/released
       flag, and the lower seven are the keycode. */
    //printf("%s: 0x%2X (%i)\n", *pressed ? "Released" : " Pressed", (unsigned int)*key, (unsigned int)*key);

    return 1;
}

void I_GetEvent(void)
{
    if (no_kb)
        return;

    event_t event;
    int pressed;
    unsigned char key;

    // put event-grabbing stuff in here
    
    while (kbd_read(&pressed, &key))
    {
        if (key == 0x0E) {
            kbd_shutdown();
            I_Quit();
        }

        UpdateShiftStatus(pressed, key);

        // process event
        
        if (!pressed)
        {
            // data1 has the key pressed, data2 has the character
            // (shift-translated, etc)
            event.type = ev_keydown;
            event.data1 = TranslateKey(key);
            event.data2 = GetTypedChar(key);

            if (event.data1 != 0)
            {
                D_PostEvent(&event);
            }
        }
        else
        {
            event.type = ev_keyup;
            event.data1 = TranslateKey(key);

            // data2 is just initialized to zero for ev_keyup.
            // For ev_keydown it's the shifted Unicode character
            // that was typed, but if something wants to detect
            // key releases it should do so based on data1
            // (key ID), not the printable char.

            event.data2 = 0;

            if (event.data1 != 0)
            {
                D_PostEvent(&event);
            }
            break;
        }
    }


                /*
            case SDL_MOUSEMOTION:
                event.type = ev_mouse;
                event.data1 = mouse_button_state;
                event.data2 = AccelerateMouse(sdlevent.motion.xrel);
                event.data3 = -AccelerateMouse(sdlevent.motion.yrel);
                D_PostEvent(&event);
                break;
                */
}

void I_InitInput(void)
{
    no_kb = M_CheckParm("-nokb");
    if (no_kb)
        return;

    kbd_init();

    //UpdateFocus();
}

