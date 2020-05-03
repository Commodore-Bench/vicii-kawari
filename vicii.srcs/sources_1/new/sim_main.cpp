#include <SDL2/SDL.h>

#include <iostream>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <verilated.h>
#include <regex.h> 

#include "Vvicii.h"
#include "constants.h"

extern "C" {
#include "vicii_ipc.h"
}

// Current simulation time (64-bit unsigned). See
// constants.h for how much each tick represents.
static vluint64_t ticks = 0;
static vluint64_t half4XDotPS;
static vluint64_t half4XColorPS;
static vluint64_t startTicks;
static vluint64_t endTicks;
static vluint64_t nextClk1;
static vluint64_t nextClk2;
static int maxDotX;
static int maxDotY;

// Add new input/output here
#define OUT_PHI 0
#define OUT_COLREF 1
#define IN_RST 2
#define OUT_R0 3
#define OUT_R1 4
#define OUT_G0 5
#define OUT_G1 6
#define OUT_B0 7
#define OUT_B1 8
#define OUT_DOT 9
#define OUT_CSYNC 10
#define INOUT_A0 11
#define INOUT_A1 12
#define INOUT_A2 13
#define INOUT_A3 14
#define INOUT_A4 15
#define INOUT_A5 16
#define INOUT_A6 17
#define INOUT_A7 18
#define INOUT_A8 19
#define INOUT_A9 20
#define INOUT_A10 21
#define INOUT_A11 22
#define OUT_D0 23
#define OUT_D1 24
#define OUT_D2 25
#define OUT_D3 26
#define OUT_D4 27
#define OUT_D5 28
#define OUT_D6 29
#define OUT_D7 30
#define OUT_D8 31
#define OUT_D9 32
#define OUT_D10 33
#define OUT_D11 34
#define IN_D0 35
#define IN_D1 36
#define IN_D2 37
#define IN_D3 38
#define IN_D4 39
#define IN_D5 40
#define IN_D6 41
#define IN_D7 42
#define IN_D8 43
#define IN_D9 44
#define IN_D10 45
#define IN_D11 46
#define IN_CE 47
#define IN_RW 48
#define IN_BA 49
#define IN_AEC 50
#define IN_IRQ 51
#define NUM_SIGNALS 52

// Add new input/output here
const char *signal_labels[] = {
   "phi", "col", "rst", "r0", "r1", "g0", "g1", "b0", "b1" , "dot", "csync",
   "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9", "a10", "a11",
   "do0", "do1", "do2", "do3", "do4", "do5", "do6", "do7", "do8", "do9", "do10", "do11",
   "di0", "di1", "di2", "di3", "di4", "di5", "di6", "di7", "di8", "di9", "di10", "di11",
   "ce", "rw", "ba", "aec", "irq"
};
const char *signal_ids[] = {
   "p", "c", "r" ,  "r0", "r1", "g0", "g1", "b0", "b1" , "dot", "s",
   "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9", "a10", "a11",
   "do0", "do1", "do2", "do3", "do4", "do5", "do6", "do7", "do8", "do9", "do10", "do11",
   "di0", "di1", "di2", "di3", "di4", "di5", "di6", "di7", "di8", "di9", "di10", "di11",
   "ce", "rw", "ba", "aec", "irq"
};

static unsigned int signal_width[NUM_SIGNALS];
static unsigned char *signal_src8[NUM_SIGNALS];
static unsigned short *signal_src16[NUM_SIGNALS];
static unsigned int signal_bit[NUM_SIGNALS];
static bool signal_monitor[NUM_SIGNALS];
static unsigned char prev_signal_values[NUM_SIGNALS];

// Some utility macros
// Use RISING/FALLING in combination with HASCHANGED

static int SGETVAL(int signum) {
  if (signal_width[signum] <= 8) {
     return (*signal_src8[signum] & signal_bit[signum] ? 1 : 0);
  } else if (signal_width[signum] > 8 && signal_width[signum] < 16) {
     return (*signal_src16[signum] & signal_bit[signum] ? 1 : 0);
  } else {
    abort();
  }
}

#define HASCHANGED(signum) \
   ( signal_monitor[signum] && SGETVAL(signum) != prev_signal_values[signum] )
#define RISING(signum) \
   ( signal_monitor[signum] && SGETVAL(signum))
#define FALLING(signum) \
   ( signal_monitor[signum] && !SGETVAL(signum))

// We can drive our simulated clock gen every pico second but that would
// be a waste since nothing happens between clock edges. This function
// will determine how many ticks(picoseconds) to advance our clock.
static vluint64_t nextTick(Vvicii* top) {
   vluint64_t diff1 = nextClk1 - ticks;

   nextClk1 += half4XDotPS;
   top->clk_dot4x = ~top->clk_dot4x;
   return ticks + diff1;
}

static void vcd_header(Vvicii* top, FILE* fp) {
   fprintf (fp, "$date\n");
   fprintf (fp, "   January 1, 1979.\n");
   fprintf (fp, "$end\n");
   fprintf (fp,"$version\n");
   fprintf (fp,"   1.0\n");
   fprintf (fp,"$end\n");
   fprintf (fp,"$comment\n");
   fprintf (fp,"   VCD vicii\n");
   fprintf (fp,"$end\n");

   fprintf (fp,VCD_TIMESCALE);
   fprintf (fp,"$scope module logic $end\n");

   for (int i=0;i<NUM_SIGNALS;i++)
      if (signal_monitor[i])
         fprintf (fp,"$var wire 1 %s %s $end\n", signal_ids[i], signal_labels[i]);
   fprintf (fp,"$upscope $end\n");

   fprintf (fp,"$enddefinitions $end\n");
   fprintf (fp,"$dumpvars\n");

   for (int i=0;i<NUM_SIGNALS;i++)
      if (signal_monitor[i])
         fprintf (fp,"x%s\n",signal_ids[i]);
   fprintf (fp,"$end\n");

   // Start time
   fprintf (fp,"#%" VL_PRI64 "d\n", startTicks/TICKS_TO_TIMESCALE);
   for (int i=0;i<NUM_SIGNALS;i++) {
      if (signal_monitor[i])
         fprintf (fp,"%x%s\n",SGETVAL(i), signal_ids[i]);
   }
   fflush(fp);
}

static void drawPixel(SDL_Renderer* ren, int x,int y) {
   SDL_RenderDrawPoint(ren, x*2,y*2);
   SDL_RenderDrawPoint(ren, x*2+1,y*2);
   SDL_RenderDrawPoint(ren, x*2,y*2+1);
   SDL_RenderDrawPoint(ren, x*2+1,y*2+1);
}

int main(int argc, char** argv, char** env) {

    struct vicii_state* state;
    bool capture = false;

    int chip = CHIP6569;
    bool isNtsc = true;

    bool captureByTime = true;
    bool outputVcd = false;
    bool showWindow = false;
    bool shadowVic = false;
    bool renderEachPixel = false;
    int prev_y = -1;
    struct vicii_ipc* ipc;

    // Default to 16.7us starting at 0
    startTicks = US_TO_TICKS(0);
    vluint64_t durationTicks;


    char *cvalue = nullptr;
    char c;
    char *token;
    regex_t regex;
    int reti, reti2;
    char regex_buf[32];
    FILE* outFile = NULL;

    while ((c = getopt (argc, argv, "c:hs:t:vwi:zbo:")) != -1)
    switch (c) {
      case 'c':
        chip = atoi(optarg);
        break;
      case 'i':
        token = strtok(optarg, ",");
        while (token != NULL) {
           strcpy (regex_buf, "^");
           strcat (regex_buf, token);
           strcat (regex_buf, "$");
           reti = regcomp(&regex, regex_buf, 0);
           for (int i = 0; i < NUM_SIGNALS; i++) {
              if (strcmp(signal_labels[i],token) == 0) {
                 signal_monitor[i] = true;
                 break;
              }
              if (!reti) {
                 reti2 = regexec(&regex, signal_labels[i], 0, NULL, 0);
                 if (!reti2) {
                    signal_monitor[i] = true;
                 }
              }
           }
           regfree(&regex);
           token = strtok(NULL, ",");
        }
        break;
      case 'o':
        outFile = fopen(optarg,"w");
        break;
      case 'b':
        // Render after every pixel instead of after every line
        renderEachPixel = true;
        break;
      case 'z':
        // IPC tells us when to start/stop capture
        captureByTime = false;
        shadowVic = true;
        break;
      case 'v':
        outputVcd = true;
        break;
      case 'w':
        showWindow = true;
        break;
      case 's':
        startTicks = US_TO_TICKS(atol(optarg));
        break;
      case 't':
        durationTicks = US_TO_TICKS(atol(optarg));
        break;
      case 'h':
        printf ("Usage\n");
        printf ("  -s [uS]   : start at uS\n");
        printf ("  -t [uS]   : run for uS\n");
        printf ("  -v        : generate vcd to file\n");
        printf ("  -o <file> : specify filename\n");
        printf ("  -w        : show SDL2 window\n");
        printf ("  -z        : single step eval for shadow vic via ipc\n");
        printf ("  -b        : render each pixel instead of each line\n");
        printf ("  -i        : list signals to include (phi, ce, csync, etc.) \n");
        printf ("  -c <chip> : 0=CHIP6567R8, 1=CHIP6567R56A 2=CHIP65669\n");
        
        exit(0);
      case '?':
        if (optopt == 't' || optopt == 's')
          fprintf (stderr, "Option -%c requires an argument.\n", optopt);
        else if (isprint (optopt))
          fprintf (stderr, "Unknown option `-%c'.\n", optopt);
        else
          fprintf (stderr,
                   "Unknown option character `\\x%x'.\n",
                   optopt);
        return 1;
      default:
        exit(-1);
    }

    if (outputVcd && outFile == NULL) {
       printf ("error: need out file with -o\n");
       exit(-1);
    }

    switch (chip) {
       case CHIP6567R8:
       case CHIP6567R56A:
          durationTicks = US_TO_TICKS(16700L);
          break;
       case CHIP6569:
          durationTicks = US_TO_TICKS(20000L);
          break;
       default:
          durationTicks = US_TO_TICKS(20000L);
    }

    switch (chip) {
       case CHIP6567R8:
          isNtsc = true;
          printf ("CHIP: 6567R8\n");
          printf ("VIDEO: NTSC\n");
          break;
       case CHIP6567R56A:
          isNtsc = true;
          printf ("CHIP: 6567R56A\n");
          printf ("VIDEO: NTSC\n");
          break;
       case CHIP6569:
          isNtsc = false;
          printf ("CHIP: 6569\n");
          printf ("VIDEO: PAL\n");
          break;
       default:
          printf ("error: unknown chip\n");
          break;
    }

    if (isNtsc) {
       half4XDotPS = NTSC_HALF_4X_DOT_PS;
       half4XColorPS = NTSC_HALF_4X_COLOR_PS;
       switch (chip) {
          case CHIP6567R56A:
             maxDotX = NTSC_6567R56A_MAX_DOT_X;
             maxDotY = NTSC_6567R56A_MAX_DOT_Y;
             break;
          case CHIP6567R8:
             maxDotX = NTSC_6567R8_MAX_DOT_X;
             maxDotY = NTSC_6567R8_MAX_DOT_Y;
             break;
          default:
             fprintf (stderr, "wrong chip?\n");
             exit(-1);
       }
    } else {
       half4XDotPS = PAL_HALF_4X_DOT_PS;
       half4XColorPS = PAL_HALF_4X_COLOR_PS;
       switch (chip) {
          case CHIP6569:
             maxDotX = PAL_6569_MAX_DOT_X;
             maxDotY = PAL_6569_MAX_DOT_Y;
             break;
          default:
             fprintf (stderr, "wrong chip?\n");
             exit(-1);
       }
    }

    nextClk1 = half4XDotPS;
    nextClk2 = half4XColorPS;
    endTicks = startTicks + durationTicks;

    int sdl_init_mode = SDL_INIT_VIDEO;
    if (SDL_Init(sdl_init_mode) != 0) {
      std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
      return 1;
    }

    SDL_Event event;
    SDL_Renderer* ren = nullptr;
    SDL_Window* win;

    if (showWindow) {
      SDL_DisplayMode current;
      int width = maxDotX*2;
      int height = maxDotY*2;

      win = SDL_CreateWindow("VICII",
                             SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED,
                             width, height, SDL_WINDOW_SHOWN);
      if (win == nullptr) {
        std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
        SDL_Quit();
        return 1;
      }

      ren = SDL_CreateRenderer(
          win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
      if (ren == nullptr) {
        std::cerr << "SDL_CreateRenderer Error: "
           << SDL_GetError() << std::endl;
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
      }
    }

    // Add new input/output here.
    Vvicii* top = new Vvicii;
    top->chip = chip;
    top->rst = 0;
    top->ad = 0;
    top->dbi = 0;
    top->aec = 1; // TODO
    top->vicii__DOT__b0c = 14;
    top->vicii__DOT__ec = 6;

    // Default all signals to bit 1 and include in monitoring.
    for (int i = 0; i < NUM_SIGNALS; i++) {
      signal_width[i] = 1;
      signal_bit[i] = 1;
    }

    signal_monitor[OUT_DOT] = true;

    // Add new input/output here.
    signal_src8[OUT_PHI] = &top->clk_phi;
    signal_src8[OUT_COLREF] = &top->clk_colref;
    signal_src8[IN_RST] = &top->rst;
    signal_src8[OUT_R0] = &top->red;
    signal_src8[OUT_R1] = &top->red;
    signal_bit[OUT_R1] = 2;
    signal_src8[OUT_G0] = &top->green;
    signal_src8[OUT_G1] = &top->green;
    signal_bit[OUT_G1] = 2;
    signal_src8[OUT_B0] = &top->blue;
    signal_src8[OUT_B1] = &top->blue;
    signal_bit[OUT_B1] = 2;
    signal_src8[OUT_DOT] = &top->vicii__DOT__clk_dot;
    signal_src8[OUT_CSYNC] = &top->cSync;
    signal_src8[IN_CE] = &top->ce;
    signal_src8[IN_RW] = &top->rw;
    signal_src8[IN_BA] = &top->ba;
    signal_src8[IN_AEC] = &top->aec;
    signal_src8[IN_IRQ] = &top->irq;

    int bt = 1;
    for (int i=INOUT_A0; i<= INOUT_A11; i++) {
       signal_width[i] = 12;
       signal_bit[i] = bt;
       signal_src16[i] = &top->ad;
       bt = bt * 2;
    }
    bt = 1;
    for (int i=OUT_D0; i<= OUT_D11; i++) {
       signal_width[i] = 12;
       signal_bit[i] = bt;
       signal_src16[i] = &top->dbo;
       bt = bt * 2;
    }
    bt = 1;
    for (int i=IN_D0; i<= IN_D11; i++) {
       signal_width[i] = 12;
       signal_bit[i] = bt;
       signal_src16[i] = &top->dbi;
       bt = bt * 2;
    }

    top->eval();


    if (outputVcd)
       vcd_header(top,outFile);

    // After this loop, the next tick will bring DOT high
    for (int t =0; t < 32; t++) {
       ticks = nextTick(top);
       top->eval();
    }

    if (shadowVic) {
       ipc = ipc_init(IPC_RECEIVER);
       ipc_open(ipc);
       state = ((struct vicii_state*)ipc->dspOutBuf);
    }

    // This lets us iterate the eval loop until we see the
    // dot clock tick forward one half its period.
    bool needDotTick = false;

    for (int i = 0; i < NUM_SIGNALS; i++) {
       prev_signal_values[i] = SGETVAL(i);
    }

    int verifyNextDotXPos = -1;

    // IMPORTANT: Any and all state reads/writes MUST occur between ipc_receive
    // and ipc_send inside this loop.
    while (!Verilated::gotFinish()) {

        // Are we shadowing from VICE? Wait for sync data, then
        // step until next dot clock tick.
        if (shadowVic && !needDotTick) {
           if (ipc_receive(ipc))
              break;

           needDotTick = true;

           capture = (state->flags & VICII_OP_CAPTURE_START);

           if (state->flags & VICII_OP_SYNC_STATE) {
               // We sync state always when phi is high (2nd phase)
               assert(~top->clk_phi);
               // Add 3 because the next dot tick will increment xpos.
               top->vicii__DOT__raster_x = 8 * state->cycle_num + 3;
               top->vicii__DOT__raster_line = state->raster_line;

               verifyNextDotXPos = state->xpos + 4;

               printf ("info: syncing FPGA to cycle=%u, raster_line=%u, xpos=%u\n",
                  state->cycle_num, state->raster_line, verifyNextDotXPos);
           }
           state->flags &= ~VICII_OP_SYNC_STATE;

           if (state->flags & VICII_OP_BUS_ACCESS) {
              assert(top->clk_phi);
           }

           top->ad = state->addr;
           top->ce = state->ce;
           top->rw = state->rw;

           if (top->ce == 0 && top->rw == 0) {
              // chip select and write, set data
              top->dbi = state->data;
           }
        }

#ifdef TEST_RESET
        // Test reset between approx 7 and approx 8 us
        if (ticks >= US_TO_TICKS(7000L) && ticks <= US_TO_TICKS(8000L))
           top->rst = 1;
        else
           top->rst = 0;
#endif

        // Evaluate model
        top->eval();

        if (captureByTime)
           capture = (ticks >= startTicks) && (ticks <= endTicks);

        if (capture) {
          bool anyChanged = false;
          for (int i = 0; i < NUM_SIGNALS; i++) {
             if (HASCHANGED(i)) {
                anyChanged = true;
                break;
             }
          }

          if (anyChanged) {
             if (outputVcd)
                fprintf (outFile, "#%" VL_PRI64 "d\n", ticks/TICKS_TO_TIMESCALE);
             for (int i = 0; i < NUM_SIGNALS; i++) {
                if (HASCHANGED(i)) {
                   if (outputVcd)
                      fprintf (outFile, "%x%s\n", SGETVAL(i), signal_ids[i]);
                }
             }
          }

          // If rendering, draw current color on dot clock
          if (showWindow && HASCHANGED(OUT_DOT) && RISING(OUT_DOT)) {

             if (verifyNextDotXPos >= 0) {
                // This is a check to make sure the next dot is what
                // we expected from the fpga sync step above.
                if (top->xpos != verifyNextDotXPos) {
                   printf ("error: expected next dot to have xpos=%u but got xpos=%u\n",
                       verifyNextDotXPos, top->xpos);
                } else {
                   printf ("info: got expected next dot with xpos=%u\n", top->xpos);
                }
                verifyNextDotXPos = -1;
             }

             SDL_SetRenderDrawColor(ren,
                top->red << 6,
                top->green << 6,
                top->blue << 6,
                255);
             drawPixel(ren,
                top->vicii__DOT__raster_x,
                top->vicii__DOT__raster_line
             );

             // Show updated pixels per raster line
             if (prev_y != top->vicii__DOT__raster_line) {
                SDL_RenderPresent(ren);
                prev_y = top->vicii__DOT__raster_line;

                SDL_PollEvent(&event);
                switch (event.type) {
                   case SDL_QUIT:
                      state->flags |= VICII_OP_CAPTURE_END;
                      break;
                   default:
                      break;
                }
             }
             if (renderEachPixel) {
                SDL_RenderPresent(ren);
             }
          }
        }

        if (shadowVic && HASCHANGED(OUT_DOT) && needDotTick) {
           // TODO : Report back any outputs like data, ba, aec, etc. here

           if (top->ce == 0 && top->rw == 1) {
              // Chip selected and read, set data in state
              state->data = top->dbo;
           }

           bool needQuit = false;
           if (state->flags & VICII_OP_CAPTURE_END) {
              needQuit = true;
           }

           // After we have one full frame, exit the loop.
           if ((state->flags & VICII_OP_CAPTURE_ONE_FRAME) !=0 &&
              top->xpos == 0 && top->vicii__DOT__raster_line == 0) {
              needQuit = true;
           }

           if (ipc_send(ipc))
              break;
           needDotTick = false;

           if (needQuit) {
              // Safe to quit now. We sent our response.
              break;
           }
        }

        // End of eval. Remember current values for previous compares.
        for (int i = 0; i < NUM_SIGNALS; i++) {
           prev_signal_values[i] = SGETVAL(i);
        }

        // Is it time to stop?
        if (captureByTime && ticks >= endTicks)
           break;

        // Advance simulation time. Each tick represents 1 picosecond.
        ticks = nextTick(top);
    }

    if (outputVcd) {
        fclose(outFile);
    }

    if (shadowVic) {
       ipc_close(ipc);
    }

    if (showWindow) {
       bool quit = false;
       while (!quit) {
          while (SDL_PollEvent(&event)) {
             switch (event.type) {
                case SDL_QUIT:
                case SDL_KEYUP:
                   quit=true; break;
                default:
                   break;
             }
           }
       }

       SDL_DestroyRenderer(ren);
       SDL_DestroyWindow(win);
       SDL_Quit();
    }

    // Final model cleanup
    top->final();

    // Destroy model
    delete top;

    // Fin
    exit(0);
}
