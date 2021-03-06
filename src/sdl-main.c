#include <SDL.h>
#include <getopt.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "risc.h"
#include "risc-io.h"
#include "disk.h"
#include "pclink.h"
#include "raw-serial.h"
#include "sdl-ps2.h"
#include "sdl-clipboard.h"

#define CPU_HZ 25000000
#define FPS 60

static uint32_t BLACK = 0x657b83, WHITE = 0xfdf6e3;
//static uint32_t BLACK = 0x000000, WHITE = 0xFFFFFF;
//static uint32_t BLACK = 0x0000FF, WHITE = 0xFFFF00;
//static uint32_t BLACK = 0x000000, WHITE = 0x00FF00;

#define MAX_HEIGHT 2048
#define MAX_WIDTH  2048

static int best_display(const SDL_Rect *rect);
static int clamp(int x, int min, int max);
static enum Action map_keyboard_event(SDL_KeyboardEvent *event);
static void show_leds(const struct RISC_LED *leds, uint32_t value);
static double scale_display(SDL_Window *window, const SDL_Rect *risc_rect, SDL_Rect *display_rect);
static void update_texture(struct RISC *risc, SDL_Texture *texture, const SDL_Rect *risc_rect);

enum Action {
  ACTION_OBERON_INPUT,
  ACTION_QUIT,
  ACTION_RESET,
  ACTION_TOGGLE_FULLSCREEN,
  ACTION_FAKE_MOUSE1,
  ACTION_FAKE_MOUSE2,
  ACTION_FAKE_MOUSE3
};

struct KeyMapping {
  int state;
  SDL_Keycode sym;
  SDL_Keymod mod1, mod2;
  enum Action action;
};

struct KeyMapping key_map[] = {
  { SDL_PRESSED,  SDLK_F4,     KMOD_ALT, 0,           ACTION_QUIT },
  { SDL_PRESSED,  SDLK_F12,    0, 0,                  ACTION_RESET },
  { SDL_PRESSED,  SDLK_DELETE, KMOD_CTRL, KMOD_SHIFT, ACTION_RESET },
  { SDL_PRESSED,  SDLK_F11,    0, 0,                  ACTION_TOGGLE_FULLSCREEN },
  { SDL_PRESSED,  SDLK_RETURN, KMOD_ALT, 0,           ACTION_TOGGLE_FULLSCREEN },
  { SDL_PRESSED,  SDLK_f,      KMOD_GUI, KMOD_SHIFT,  ACTION_TOGGLE_FULLSCREEN },  // Mac?
  { SDL_PRESSED,  SDLK_LALT,   0, 0,                  ACTION_FAKE_MOUSE2 },
  { SDL_RELEASED, SDLK_LALT,   0, 0,                  ACTION_FAKE_MOUSE2 },
};

static struct option long_options[] = {
  { "fullscreen", no_argument,       NULL, 'f' },
  { "leds",       no_argument,       NULL, 'L' },
  { "size",       required_argument, NULL, 's' },
  { "serial-fd",  required_argument, NULL, 'F' },
  { NULL }
};

static void fail(int code, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(code);
}

static void usage() {
  fail(1, "Usage: risc [--fullscreen] [--size <width>x<height>] [--leds] disk-file-name");
}

int main (int argc, char *argv[]) {
  struct RISC *risc = risc_new();
  risc_set_serial(risc, &pclink);
  risc_set_clipboard(risc, &sdl_clipboard);

  struct RISC_LED leds = {
    .write = show_leds
  };

  bool fullscreen = false;
  SDL_Rect risc_rect = {
    .w = RISC_FRAMEBUFFER_WIDTH,
    .h = RISC_FRAMEBUFFER_HEIGHT
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "fLS:F:", long_options, NULL)) != -1) {
    switch (opt) {
      case 'f': {
        fullscreen = true;
        break;
      }
      case 'L': {
        risc_set_leds(risc, &leds);
        break;
      }
      case 's': {
        int w, h;
        if (sscanf(optarg, "%dx%d", &w, &h) != 2) {
          usage();
        }
        risc_rect.w = clamp(w, 32, MAX_WIDTH) & ~31;
        risc_rect.h = clamp(h, 32, MAX_HEIGHT);
        risc_screen_size_hack(risc, risc_rect.w, risc_rect.h);
        break;
      }
      case 'F': {
        risc_set_serial(risc, raw_serial_new(atoi(optarg), atoi(optarg) + 1));
        break;
      }
      default: {
        usage();
      }
    }
  }
  if (optind != argc - 1) {
    usage();
  }
  risc_set_spi(risc, 1, disk_new(argv[optind]));

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fail(1, "Unable to initialize SDL: %s", SDL_GetError());
  }
  atexit(SDL_Quit);
  SDL_EnableScreenSaver();
  SDL_ShowCursor(false);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");

  int window_flags = SDL_WINDOW_HIDDEN;
  int window_pos = SDL_WINDOWPOS_UNDEFINED;
  if (fullscreen) {
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window_pos = best_display(&risc_rect);
  }
  SDL_Window *window = SDL_CreateWindow("Project Oberon",
                                        window_pos, window_pos,
                                        risc_rect.w, risc_rect.h,
                                        window_flags);
  if (window == NULL) {
    fail(1, "Could not create window: %s", SDL_GetError());
  }

  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, 0);
  if (renderer == NULL) {
    fail(1, "Could not create renderer: %s", SDL_GetError());
  }

  SDL_Texture *texture = SDL_CreateTexture(renderer,
                                           SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING,
                                           risc_rect.w,
                                           risc_rect.h);
  if (texture == NULL) {
    fail(1, "Could not create texture: %s", SDL_GetError());
  }

  SDL_Rect display_rect;
  double display_scale = scale_display(window, &risc_rect, &display_rect);
  update_texture(risc, texture, &risc_rect);
  SDL_ShowWindow(window);
  SDL_RenderClear(renderer);
  SDL_RenderCopy(renderer, texture, &risc_rect, &display_rect);
  SDL_RenderPresent(renderer);

  bool done = false;
  bool mouse_was_offscreen = false;
  while (!done) {
    uint32_t frame_start = SDL_GetTicks();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      switch (event.type) {
        case SDL_QUIT: {
          done = true;
        }

        case SDL_WINDOWEVENT: {
          if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
            display_scale = scale_display(window, &risc_rect, &display_rect);
          }
          break;
        }

        case SDL_MOUSEMOTION: {
          int scaled_x = (int)round((event.motion.x - display_rect.x) / display_scale);
          int scaled_y = (int)round((event.motion.y - display_rect.y) / display_scale);
          int x = clamp(scaled_x, 0, risc_rect.w - 1);
          int y = clamp(scaled_y, 0, risc_rect.h - 1);
          bool mouse_is_offscreen = x != scaled_x || y != scaled_y;
          if (mouse_is_offscreen != mouse_was_offscreen) {
            SDL_ShowCursor(mouse_is_offscreen);
            mouse_was_offscreen = mouse_is_offscreen;
          }
          risc_mouse_moved(risc, x, risc_rect.h - y - 1);
          break;
        }

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
          bool down = event.button.state == SDL_PRESSED;
          risc_mouse_button(risc, event.button.button, down);
          break;
        }

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          bool down = event.key.state == SDL_PRESSED;
          switch (map_keyboard_event(&event.key)) {
            case ACTION_RESET: {
              risc_reset(risc);
              break;
            }
            case ACTION_TOGGLE_FULLSCREEN: {
              fullscreen ^= true;
              if (fullscreen) {
                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
              } else {
                SDL_SetWindowFullscreen(window, 0);
              }
              break;
            }
            case ACTION_QUIT: {
              SDL_PushEvent(&(SDL_Event){ .type=SDL_QUIT });
              break;
            }
            case ACTION_FAKE_MOUSE1: {
              risc_mouse_button(risc, 1, down);
              break;
            }
            case ACTION_FAKE_MOUSE2: {
              risc_mouse_button(risc, 2, down);
              break;
            }
            case ACTION_FAKE_MOUSE3: {
              risc_mouse_button(risc, 3, down);
              break;
            }
            case ACTION_OBERON_INPUT: {
              uint8_t ps2_bytes[MAX_PS2_CODE_LEN];
              int len = ps2_encode(event.key.keysym.scancode, down, ps2_bytes);
              risc_keyboard_input(risc, ps2_bytes, len);
              break;
            }
          }
        }
      }
    }

    risc_set_time(risc, frame_start);
    risc_run(risc, CPU_HZ / FPS);

    update_texture(risc, texture, &risc_rect);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, &risc_rect, &display_rect);
    SDL_RenderPresent(renderer);

    uint32_t frame_end = SDL_GetTicks();
    int delay = frame_start + 1000/FPS - frame_end;
    if (delay > 0) {
      SDL_Delay(delay);
    }
  }
  return 0;
}


static int best_display(const SDL_Rect *rect) {
  int result = SDL_WINDOWPOS_UNDEFINED;
  int display_cnt = SDL_GetNumVideoDisplays();
  for (int i = 0; i < display_cnt; i++) {
    SDL_Rect bounds;
    SDL_GetDisplayBounds(i, &bounds);
    if (bounds.h == rect->h && bounds.w >= rect->w) {
      result = SDL_WINDOWPOS_UNDEFINED_DISPLAY(i);
      if (bounds.w == rect->w)
        break;  // exact match
    }
  }
  return result;
}

static int clamp(int x, int min, int max) {
  if (x < min) return min;
  if (x > max) return max;
  return x;
}

static enum Action map_keyboard_event(SDL_KeyboardEvent *event) {
  for (size_t i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
    if ((event->state == key_map[i].state) &&
        (event->keysym.sym == key_map[i].sym) &&
        ((key_map[i].mod1 == 0) || (event->keysym.mod & key_map[i].mod1)) &&
        ((key_map[i].mod2 == 0) || (event->keysym.mod & key_map[i].mod2))) {
      return key_map[i].action;
    }
  }
  return ACTION_OBERON_INPUT;
}

static void show_leds(const struct RISC_LED *leds, uint32_t value) {
  printf("LEDs: ");
  for (int i = 7; i >= 0; i--) {
    if (value & (1 << i)) {
      printf("%d", i);
    } else {
      printf("-");
    }
  }
  printf("\n");
}

static double scale_display(SDL_Window *window, const SDL_Rect *risc_rect, SDL_Rect *display_rect) {
  int win_w, win_h;
  SDL_GetWindowSize(window, &win_w, &win_h);
  double oberon_aspect = (double)risc_rect->w / risc_rect->h;
  double window_aspect = (double)win_w / win_h;

  double scale;
  if (oberon_aspect > window_aspect) {
    scale = (double)win_w / risc_rect->w;
  } else {
    scale = (double)win_h / risc_rect->h;
  }

  int w = (int)ceil(risc_rect->w * scale);
  int h = (int)ceil(risc_rect->h * scale);
  *display_rect = (SDL_Rect){
    .w = w, .h = h,
    .x = (win_w - w) / 2,
    .y = (win_h - h) / 2
  };
  return scale;
}

// Only used in update_texture(), but some systems complain if you
// allocate three megabyte on the stack.
static uint32_t pixel_buf[MAX_WIDTH * MAX_HEIGHT];

static void update_texture(struct RISC *risc, SDL_Texture *texture, const SDL_Rect *risc_rect) {
  struct Damage damage = risc_get_framebuffer_damage(risc);
  if (damage.y1 <= damage.y2) {
    uint32_t *in = risc_get_framebuffer_ptr(risc);
    uint32_t out_idx = 0;

    for (int line = damage.y2; line >= damage.y1; line--) {
      int line_start = line * (risc_rect->w / 32);
      for (int col = damage.x1; col <= damage.x2; col++) {
        uint32_t pixels = in[line_start + col];
        for (int b = 0; b < 32; b++) {
          pixel_buf[out_idx] = (pixels & 1) ? WHITE : BLACK;
          pixels >>= 1;
          out_idx++;
        }
      }
    }

    SDL_Rect rect = {
      .x = damage.x1 * 32,
      .y = risc_rect->h - damage.y2 - 1,
      .w = (damage.x2 - damage.x1 + 1) * 32,
      .h = (damage.y2 - damage.y1 + 1)
    };
    SDL_UpdateTexture(texture, &rect, pixel_buf, rect.w * 4);
  }
}
