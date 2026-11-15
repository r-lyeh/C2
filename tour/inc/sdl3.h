// inc/sdl3.h: the part of SDL3's API that demo_49_ffi_sdl3.c2 uses, declared as SDL3's own headers do
// (the real <SDL3/SDL.h> works too, with -I pointing at it).
#pragma once
#define SDL_INIT_VIDEO       0x20u
#define SDL_WINDOW_RESIZABLE 0x20ull
#define SDL_EVENT_QUIT       0x100
#define SDL_EVENT_KEY_DOWN   0x300
#define SDLK_ESCAPE          0x1bu

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef union SDL_Event {
    uint32_t type;
    struct { uint32_t type, reserved; uint64_t timestamp; uint32_t window, which, scancode, key; } key;
    uint8_t padding[128];
} SDL_Event;

bool          SDL_Init(uint32_t flags);
void          SDL_Quit(void);
const char   *SDL_GetError(void);
SDL_Window   *SDL_CreateWindow(const char *title, int w, int h, uint64_t flags);
void          SDL_DestroyWindow(SDL_Window *window);
SDL_Renderer *SDL_CreateRenderer(SDL_Window *window, const char *name);
void          SDL_DestroyRenderer(SDL_Renderer *renderer);
bool          SDL_PollEvent(SDL_Event *event);
bool          SDL_SetRenderDrawColor(SDL_Renderer *r, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha);
bool          SDL_RenderClear(SDL_Renderer *r);
bool          SDL_RenderDebugText(SDL_Renderer *r, float x, float y, const char *text);
bool          SDL_SetRenderScale(SDL_Renderer *r, float sx, float sy);
bool          SDL_RenderPresent(SDL_Renderer *r);
uint64_t      SDL_GetTicks(void);
void          SDL_Delay(uint32_t ms);
