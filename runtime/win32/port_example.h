/* port.h: what a port tells the AntiStatic runtime about its game. Copy this file into your
   port's runtime folder as port.h and fill it in; the runtime core includes it (runtime.h).

   Every value here describes the game as the program sees it under Windows, or names the port
   on the host. None of it is the game's code or data. */
#pragma once

/* the game's name: the window title, message boxes, the log's first line */
#define PORT_TITLE "My Game"
/* the port's short name: the user folder (~/.local/share/<slug>), the desktop app id */
#define PORT_SLUG "mygame-native"
/* environment variables for tests and power users are <PORT_ENV>_NAME (MYGAME_KEYS, ...) */
#define PORT_ENV "MYGAME"

/* The game's folder as the program sees it ("C:\\My Game"), and the folder inside it the
   program starts in ("" for the game folder itself, or e.g. "Binaries"). The program file and
   its DLLs are found there. */
#define PORT_VROOT "C:\\My Game"
#define PORT_START_DIR ""
/* the program's file name, and what follows it on its command line */
#define PORT_EXE "GAME.EXE"
#define PORT_CMDLINE_ARGS ""
/* #define PORT_CMDLINE_EXE "Game.exe" */   /* the name the command line shows, if not PORT_EXE (li2-native's runs) */
/* #define PORT_CMDLINE_EXE_W "LARush.exe" */ /* GetCommandLineW's name, if not PORT_CMDLINE_EXE's (L.A. Rush's runs) */
/* the volume label GetVolumeInformation reports for drive C: (some games check their CD's) */
#define PORT_VOLUME_LABEL "MYGAME"

/* Program memory: allocations start above the translated modules (the program's image is at
   0x400000; set this above the highest module's end, 64 KB aligned). */
#define PORT_ARENA_START 0x01000000u

/* The screen size the program sees (optional; default: the host's desktop):
#define PORT_SCREEN_W 640
#define PORT_SCREEN_H 480 */

/* The game's CD (optional): define PORT_CD_LABEL for a CD drive (PORT_CD_DRIVE, "D:" by default)
   holding the files in <game>/CD (or --cd DIR), read only, with that volume label. */
/* #define PORT_CD_LABEL "MYGAMECD" */

/* --test runs: milliseconds of the virtual clock an empty message wait lets pass (default: none;
   the C++-line ports' baselines were recorded with 1) */
/* #define PORT_TEST_IDLE_MS 1.0 */
/* --test runs: the thread turn order. 1 (default): the D3D9 line's (larush, li2, stranglehold);
   2: the C++ line's (tdor3, lixs, etm, penguins: a finished thread's place is taken
   again, the main thread's Sleep moves the virtual clock, an empty PeekMessage gives the others
   their turn, and a timed-out wait moves the clock while the main thread waits untimed) */
/* #define PORT_TEST_SCHEDULER 2 */
/* ... with scheduler 2, the empty-PeekMessage turn can be left out (for a port whose own scheduler
   never had it); default 1 with scheduler 2 (lixs, penguins), 0 otherwise */
/* #define PORT_TEST_PEEK_YIELD 0 */
/* --test: the sound mixer's clock moves only at presented frames (the virtual clock as it stood
   at the last one), not with the waits between them (Off-Road) */
/* #define PORT_TEST_SOUND_AT_FRAME 1 */
/* --test: only the main thread's clock readings move the virtual clock; other threads see it plus
   their own readings (Hard Truck 2: how often they read depends on real scheduling) */
/* #define PORT_TEST_CLOCK_PER_THREAD 1 */
/* The D3D7 renderer's defaults when options.txt doesn't say: fog (1, the default: drawn as
   shipped; 0: left out, LEGO Island 2) and the render resolution ("original", the default: the
   game's own, scaled to the window; "native": the window's, Hard Truck 2 and Off-Road) */
/* #define PORT_DEFAULT_FOG 0 */
/* #define PORT_DEFAULT_RESOLUTION "native" */
/* ... and "textures" ("original"; "smooth": trilinear + anisotropic) and "colour_precision"
   ("original"; "high": the 16-bit scene), Off-Road's being smooth and high */
/* #define PORT_DEFAULT_TEXTURES "smooth" */
/* #define PORT_DEFAULT_COLOUR_PRECISION "high" */
/* Off-Road's D3D7 renderer features (runtime/d3d7, all off unless set; vk.c's head says more) */
/* #define PORT_D3D7_SCREEN_WORLD 1 */    /* the game draws its world as screen-space vertices: the renderer tells it
                                              from the HUD (needed by SSAO and BAKED_LIGHT; widescreen past 4:3;
                                              weak hook port_ws_hud_centre) */
/* #define PORT_D3D7_SSAO 1 */            /* ambient occlusion from that world's depth ("ambient_occlusion", default on) */
/* #define PORT_D3D7_BAKED_LIGHT 1 */     /* baked lighting and the car lights (vk_light_load / _view / _cars) */
/* #define PORT_D3D7_WATER 1 */           /* simulated water's surface and spray (vk_water_step / _draw; needs BAKED_LIGHT) */
/* #define PORT_D3D7_SCENE_OBJECTS 1 */   /* motion vectors and FSR 2's camera for a screen-space world (VkScene.g, .rt,
                                              vk_scene_object) */
/* #define PORT_D3D7_DEPTHBACK 1 */       /* the scene's depth copied back to the back buffer's (needs SCENE_OBJECTS) */
/* #define PORT_D3D7_STENCIL 1 */         /* Direct3D's stencil states (a depth format with stencil first) */
/* #define PORT_D3D7_AA_MODES 1 */        /* "antialiasing = 2x" and "fxaa" too */
/* #define PORT_D3D7_TAA_HP_HISTORY 1 */  /* TAA history at the high-precision scene format (Off-Road; GPU resets seen on an RDNA 3.5 laptop GPU with float16) */
/* #define PORT_D3D7_SMOKE_STEADY 1 */    /* the volumetric smoke's budget 4x and its coarsening eased in (no flicker) */
/* #define PORT_D3D7_SMOKE_DENSITY 3.0f */   /* the puffs' density scale (default 1; Off-Road 3) */
/* #define PORT_D3D7_UNITS_PER_METRE 100.0f */   /* world units a metre, for FSR 2 (default 66.67; Off-Road's cm 100) */
/* #define PORT_D3D7_FAST_SCREEN 1 */     /* XYZRHW|DIFFUSE|SPECULAR|TEX2 draws taken without unpacking (CPU time) */
/* The Direct3D 9 bridge to DXVK (runtime/d3d9; its README lists the port_d3d9_* hooks) */
/* #define PORT_D3D9_ANTIALIASING "4x" */     /* option antialiasing's default (default "off"; d3d9_msaa.c) */
/* #define PORT_D3D9_ANISOTROPIC "16" */      /* option anisotropic's default ("off", 2, 4, 8, 16; default "16") */
/* #define PORT_D3D9_SHADOW_FILTER "5" */     /* option shadow_filter's default ("original", 3, 5, 7; default
                                                 "original"; DXVK's soft-shadows patch) */
/* #define PORT_D3D9_KEEP_ASPECT 1 */         /* the back buffer shown at its own shape, black bars (Stranglehold) */
/* #define PORT_D3D9_KEEP_LOCK_COPIES 1 */    /* a DYNAMIC / SYSTEMMEM texture's lock gives the same copy each time
                                                 (Bink decoding into old frames: Stranglehold); moves allocations */
/* --test, scheduler 1: the main thread's Sleep(ms) moves the virtual clock by ms (scheduler 2
   always does) */
/* #define PORT_TEST_SLEEP_MOVES_CLOCK 1 */

/* The machine the program sees (defaults: a 2006 Pentium III-class PC) */
/* #define PORT_CPU_COUNT 1 */                 /* processors (default the host's, at most 4; 4 in --test).
                                                  A game may behave differently with more than one: TDOR3's
                                                  sr.dll re-measured its timer (a 5 s Sleep) on every start */
/* #define PORT_CPU_REVISION 0x0806 */         /* GetSystemInfo's wProcessorRevision */
/* #define PORT_CPUID_SIGNATURE 0x686 */       /* CPUID leaf 1's eax (family, model, stepping) */
/* #define PORT_CPUID_MAX_LEAF 2 */
/* #define PORT_CPUID_FEATURES 0x0383f9ffu */   /* CPUID leaf 1's edx */
/* #define PORT_X87_PORTABLE 1 */   /* x87 sin/cos/tan/atan/log/2^x by basic arithmetic: the same results on
                                       every x86 CPU (the host's instructions differ between Intel and AMD) */
/* #define PORT_TSC_HZ 10000000.0 */           /* RDTSC's rate */
/* #define PORT_QPC_HZ 10000000.0 */           /* QueryPerformanceCounter's rate */
/* #define PORT_MEMORY_MB 1024 */              /* GlobalMemoryStatus' physical memory */
/* #define PORT_PAD_ALWAYS 1 */               /* --test: the game's joystick is there with no scripted controller
                                                  too (for a game that lists its devices once; default: keyboard only) */
/* #define PORT_FILM_SKIP_ANY 1 */            /* any key or controller button skips a film (video_skip_press),
                                                  not only Escape */
/* #define PORT_CURSOR_START_X 0 */           /* where GetCursorPos starts (320, 240 unless set) */
/* #define PORT_CURSOR_START_Y 0 */
/* #define PORT_DEFAULT_DISPLAY "fullscreen" */ /* start full screen unless options.txt says display = window */
/* #define PORT_PAD_JOYS -1 */                /* ONE merged joystick (every controller drives it), made when the
                                                  first controller is plugged in and gone with the last: none with
                                                  none, as the game shipped (1NSANE) */
/* #define PORT_PAD_JOYS 0 */                 /* one joystick per controller, made on plug-in and gone on unplug
                                                  (1NSANE); default 1 always-present merged joystick, up to 4 */
/* #define PORT_POLL_CLOCK_TIMERS_ONLY 1 */   /* --test: an empty PeekMessage reads the clock only with a timer (1NSANE) */
/* #define PORT_KEEP_EXIT_CODE 1 */          /* real runs exit with the game's own code (default 0 for a normal quit) */
/* #define PORT_HASH_STACKS 1 */              /* --hash includes thread stacks, as a port's old runtime did (1NSANE) */
/* #define PORT_NO_BUILTIN_MODS 1 */          /* no <user>/mods overlays (option "mods"): the port has its own (1NSANE) */
/* #define PORT_PAD_AXES 6 */                  /* the joysticks the game sees (TDOR3: 4 axes, 8 buttons) */
/* #define PORT_PAD_BUTTONS 16 */
/* #define PORT_COMPUTER_NAME "PLAYER-PC" */    /* GetComputerName, gethostname */
/* #define PORT_TEST_DATE 1154649600ull */      /* --test runs' date, Unix seconds (2006-08-04) */
                                                /* (moving: use the old runtime's test date; a game seeding rand()
                                                   from time() goes another way with another one) */
/* #define PORT_WINSOCK_STARTS 1 */            /* WSAStartup succeeds (sockets still fail): no network either way */

/* optional, for the per-function check (runtime/rewrite/fncheck.c, docs/REWRITE.md): the game's operator
   new (cdecl, one argument), so the blocks a check makes (new:N, fc_new) can be freed by the game's code
   (default: the process heap); the import slot of MSVCP60's basic_string::assign(const char *, n), for
   std::strings (sstr:, fc_std_string) */
/* #define PORT_FNCHECK_NEW 0x00401230u */
/* #define PORT_FNCHECK_STRING_ASSIGN 0x00402000u */

/* Optional hooks a port may define (weak defaults in the core):
     int port_file_hidden(const char *winpath);   1: CreateFile doesn't see it
                                                  (e.g. intro movies skipped by an option)
     void port_graphics_init(void);               the display layer's set-up, at start (ddraw_init)
     void port_peek_hook(void);                   at each PeekMessage (a movie player's next frame)
     void port_frame_time(double ms);             each presented frame's duration (frame-rate code)
     uint32_t port_dialog_box(CPU *c, uint32_t template_id, uint32_t proc, uint32_t param);
                                                  answer a modal dialog (DialogBoxParamA) itself
     void port_runtime_ready(void);               after game_init: install replacements, late hooks
     void port_idle_hook(void);                   while the program waits for messages
     void game_init(void);                        once, before the program starts
     void game_frame(unsigned long frame);        once per presented frame */
/* optional: 0 turns off the clock-spin guard (pacing.c), for a game whose timing reads the
   millisecond clock many times a millisecond on purpose */
/* #define PORT_CLOCK_SPIN 0 */
/* optional: the joysticks the game sees (pad.c; default 1, up to 4): more for a game with a joystick
   per player in split screen */
/* #define PORT_PAD_JOYS 2 */
