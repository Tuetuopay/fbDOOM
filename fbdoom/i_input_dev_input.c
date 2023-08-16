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
//   DOOM keyboard input via /dev/input
//

#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "d_event.h"
#include "doomtype.h"
#include "i_input_at.h"
#include "i_system.h"

int vanilla_keyboard_mapping = 1;

static int kb = -1;
static int mouse = -1;

static int find_event(const char *name) {
    char *line = NULL, *event = NULL;
    boolean found = false;
    int evt_no;
    size_t n;

    FILE *f = NULL;
    if (!(f = fopen("/proc/bus/input/devices", "ro"))) {
        printf("Failed to open /proc/bus/input/devices: %s\n", strerror(errno));
        goto fail;
    }
    while (getline(&line, &n, f) != EOF) {
        line[strlen(line) - 1] = '\0'; // remove \n

        if (line[0] == 'N' && strstr(line, name))
            found = true;
        if (found && line[0] == 'H')
            break;
    }
    fclose(f);

    if (!found) {
        printf("Did not find any input device for `%s`.\n", name);
        goto fail;
    }

    // We can skip the first token as it'll be "H: Handler="
    event = strtok(line, "=");
    while ((event = strtok(NULL, " ")) && !strstr(event, "event"));

    if (!event) {
        printf("Did not find event name for the device `%s`.\n", name);
        goto fail;
    }

    event += strlen("event");
    evt_no = atoi(event);
    free(line);

    return evt_no;

fail:
    exit(0);
}

static void init_kbd(void) {
    char path[128] = {};
    int kbd_evt_no;

    kbd_evt_no = find_event("kbdsrv virtual keyboard");
    snprintf(path, 127, "/dev/input/event%d", kbd_evt_no);

    printf("Using keyboard at %s\n", path);
    if (!(kb = open(path, O_RDONLY | O_NONBLOCK))) {
        printf("Failed to open `%s`: %s\n", path, strerror(errno));
        goto fail;
    }

    return;

fail:
    exit(0);
}

static void init_mouse(void) {
    char path[128] = {};
    int mouse_evt_no;

    mouse_evt_no = find_event("kbdsrv virtual mouse");
    snprintf(path, 127, "/dev/input/event%d", mouse_evt_no);

    printf("Using mouse at %s\n", path);
    if (!(mouse = open(path, O_RDONLY | O_NONBLOCK))) {
        printf("Failed to open `%s`: %s\n", path, strerror(errno));
        goto fail;
    }

    return;

fail:
    exit(0);
}

void I_InitInput(void) {
    init_kbd();
    init_mouse();
}

void I_GetEvent() {
    struct input_event in_event;

    while (read(kb, &in_event, sizeof(in_event)) >= 0) {
        event_t out_event = {.data2 = 0};

        if (in_event.type != EV_KEY || in_event.code > 0xff) continue;
        if (in_event.code == 0xe) I_Quit();

        switch (in_event.value) {
            case 0: out_event.type = ev_keyup;   break;
            case 1: out_event.type = ev_keydown; break;
            case 2: // autorepeat
            default: continue;
        }

        I_UpdateShiftStatus(out_event.type == ev_keydown, in_event.code);

        out_event.data1 = I_TranslateKey(in_event.code);
        if (out_event.type != ev_keyup)
            out_event.data2 = I_GetTypedChar(in_event.code);

        if (out_event.data1 != 0) D_PostEvent(&out_event);

        // printf("key%s %c (0x%x)\n", out_event.type == ev_keydown ? "down" : "up", out_event.data2, out_event.data1);
    }

    if (errno != EAGAIN && errno != EWOULDBLOCK)
        printf("Failed to read from the keyboard: %s\n", strerror(errno));

    // Accumulate mouse inputs
    event_t out_event = {.type = ev_mouse, .data1 = 0, .data2 = 0, .data3 = 0};
    while (read(mouse, &in_event, sizeof(in_event)) >= 0) {
        if (in_event.type != EV_REL) continue;

        switch (in_event.code) {
            case 0: out_event.data2 += in_event.value * 5; break; // X axis
            // case 1: out_event.data3 = in_event.value; break; // Y axis
            default: continue;
        }
    }

    if (out_event.data2 != 0)
        D_PostEvent(&out_event);

    if (errno != EAGAIN && errno != EWOULDBLOCK)
        printf("Failed to read from the mouse: %s\n", strerror(errno));
}
