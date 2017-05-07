/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "video.h"
#include "egl.h"
#include "ffmpeg.h"
#include "ffmpeg_vdpau.h"

#include "../input/x11.h"
#include "../loop.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#define DECODER_BUFFER_SIZE 92*1024

static char* ffmpeg_buffer = NULL;

static Display *display;

static int pipefd[2];

static int frame_handle(int pipefd) {
  const unsigned char** data = NULL;
  while (read(pipefd, &data, sizeof(void*)) > 0);
  if (data)
    egl_draw(data);

  return LOOP_OK;
}

int x11_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  int avc_flags = SLICE_THREADING;
  if (drFlags & FORCE_HARDWARE_ACCELERATION)
    avc_flags |= HARDWARE_ACCELERATION;

  ffmpeg_buffer = malloc(DECODER_BUFFER_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
  if (ffmpeg_buffer == NULL) {
    fprintf(stderr, "Not enough memory\n");
    ffmpeg_destroy();
    return -1;
  }

  XInitThreads();
  display = XOpenDisplay(NULL);
  if (!display) {
    fprintf(stderr, "Error: failed to open X display.\n");
    return -2;
  }

  Window root = DefaultRootWindow(display);
  XSetWindowAttributes winattr = { .event_mask = PointerMotionMask | ButtonPressMask | ButtonReleaseMask | KeyPressMask | KeyReleaseMask };
  Window window = XCreateWindow(display, root, 0, 0, width, height, 0, CopyFromParent, InputOutput, CopyFromParent, CWEventMask, &winattr);
  XMapWindow(display, window);
  XStoreName(display, window, "Moonlight");

  if (drFlags & DISPLAY_FULLSCREEN) {
    Atom wm_state = XInternAtom(display, "_NET_WM_STATE", False);
    Atom fullscreen = XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);

    XEvent xev = {0};
    xev.type = ClientMessage;
    xev.xclient.window = window;
    xev.xclient.message_type = wm_state;
    xev.xclient.format = 32;
    xev.xclient.data.l[0] = 1;
    xev.xclient.data.l[1] = fullscreen;
    xev.xclient.data.l[2] = 0;

    XSendEvent(display, DefaultRootWindow(display), False, SubstructureRedirectMask | SubstructureNotifyMask, &xev);
  }

  if (ffmpeg_init(videoFormat, width, height, avc_flags, 2, 2, display) < 0) {
    fprintf(stderr, "Couldn't initialize video decoding\n");
    return -1;
  }

  if (ffmpeg_decoder == SOFTWARE)
    egl_init(display, window, width, height);
  else
    vdpau_init_queue(window);

  x11_input_init(display, window);

  if (pipe(pipefd) == -1) {
    fprintf(stderr, "Can't create communication channel between threads\n");
    return -2;
  }
  loop_add_fd(pipefd[0], &frame_handle, POLLIN);
  fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

  return 0;
}

void x11_cleanup() {
  ffmpeg_destroy();
  egl_destroy();
}

int x11_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  if (decodeUnit->fullLength < DECODER_BUFFER_SIZE) {
    PLENTRY entry = decodeUnit->bufferList;
    int length = 0;
    while (entry != NULL) {
      memcpy(ffmpeg_buffer+length, entry->data, entry->length);
      length += entry->length;
      entry = entry->next;
    }
    ffmpeg_decode(ffmpeg_buffer, length);
    AVFrame* frame = ffmpeg_get_frame(true);
    if (frame != NULL) {
      if (ffmpeg_decoder == SOFTWARE) {
        void* pointer = frame->data;
        write(pipefd[1], &pointer, sizeof(void*));
      } else
        vdpau_queue(frame);
    }
  }

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_x11 = {
  .setup = x11_setup,
  .cleanup = x11_cleanup,
  .submitDecodeUnit = x11_submit_decode_unit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(4) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_DIRECT_SUBMIT,
};
