#include <tonc.h>

#include "Benchmark.h"
#include "Config.h"
#include "Palette.h"
#include "Protocol.h"
#include "SPISlave.h"

// -----
// STATE
// -----

typedef struct {
  u32 expectedPackets;
  u8 temporalDiffs[TEMPORAL_DIFF_SIZE];
  u8 spatialDiffs[SPATIAL_DIFF_SIZE];
  u8 compressedPixels[TOTAL_PIXELS];
} State;

SPISlave* spiSlave = new SPISlave();
DATA_EWRAM u8 frameBuffer[TOTAL_SCREEN_PIXELS];

// ---------
// BENCHMARK
// ---------

#ifdef BENCHMARK
int main() {
  Benchmark::init();
  Benchmark::mainLoop();
  return 0;
}
#endif

// ------------
// DECLARATIONS
// ------------

void init();
void mainLoop();
void sendKeysAndReceiveTemporalDiffs(State& state);
void receiveSpatialDiffs(State& state);
void receivePixels(State& state);
void draw(State& state);
void decompressImage(State& state);
bool sync(State& state, u32 local, u32 remote);
u32 x(u32 cursor);
u32 y(u32 cursor);

// -----------
// DEFINITIONS
// -----------

#ifndef BENCHMARK
int main() {
  init();
  mainLoop();
  return 0;
}
#endif

inline void init() {
  enableMode4AndBackground2();
  overclockEWRAM();
  enable2xMosaic();
  dma3_cpy(pal_bg_mem, MAIN_PALETTE, sizeof(COLOR) * PALETTE_COLORS);
}

CODE_IWRAM void mainLoop() {
reset:
  State state;
  state.expectedPackets = 0;

  spiSlave->transfer(CMD_RESET);

  while (true) {
    state.expectedPackets = 0;

    if (!sync(state, CMD_FRAME_START_GBA, CMD_FRAME_START_RPI))
      goto reset;

    sendKeysAndReceiveTemporalDiffs(state);

    if (!sync(state, CMD_SPATIAL_DIFFS_START_GBA, CMD_SPATIAL_DIFFS_START_RPI))
      goto reset;

    receiveSpatialDiffs(state);

    if (!sync(state, CMD_PIXELS_START_GBA, CMD_PIXELS_START_RPI))
      goto reset;

    receivePixels(state);

    if (!sync(state, CMD_FRAME_END_GBA, CMD_FRAME_END_RPI))
      goto reset;

    draw(state);
  }
}

inline void sendKeysAndReceiveTemporalDiffs(State& state) {
  state.expectedPackets = spiSlave->transfer(0);

  u16 keys = pressedKeys();
  for (u32 i = 0; i < TEMPORAL_DIFF_SIZE / PACKET_SIZE; i++) {
    ((u32*)state.temporalDiffs)[i] =
        spiSlave->transfer(i < PRESSED_KEYS_REPETITIONS ? keys : i);
  }
}

inline void receiveSpatialDiffs(State& state) {
  for (u32 i = 0; i < SPATIAL_DIFF_SIZE / PACKET_SIZE; i++)
    ((u32*)state.spatialDiffs)[i] = spiSlave->transfer(i);
}

inline void receivePixels(State& state) {
  for (u32 i = 0; i < state.expectedPackets; i++)
    ((u32*)state.compressedPixels)[i] = spiSlave->transfer(i);
}

inline void draw(State& state) {
  decompressImage(state);
  dma3_cpy(vid_mem_front, frameBuffer, TOTAL_SCREEN_PIXELS);
}

inline void decompressImage(State& state) {
  u32 block = 0;
  u32 blockPart = 0;
  u32 decompressedPixels = 0;

  for (u32 cursor = 0; cursor < TOTAL_PIXELS; cursor++) {
    u32 temporalByte = cursor / 8;
    u32 temporalBit = cursor % 8;
    u32 temporalDiff = state.temporalDiffs[temporalByte];

    if (temporalBit == 0) {
      if (((u32*)state.temporalDiffs)[temporalByte / 4] == 0) {
        // (no changes in the next 32 pixels)
        cursor += 31;
        continue;
      } else if (((u16*)state.temporalDiffs)[temporalByte / 2] == 0) {
        // (no changes in the next 16 pixels)
        cursor += 15;
        continue;
      } else if (temporalDiff == 0) {
        // (no changes in the next 8 pixels)
        cursor += 7;
        continue;
      }
    }

    if ((temporalDiff >> temporalBit) & 1) {
      // (a pixel changed)

      u32 spatialByte = block / 8;
      u32 spatialBit = block % 8;
      u32 spatialDiff = state.spatialDiffs[spatialByte];
      bool isRepeatedBlock = !((spatialDiff >> spatialBit) & 1);

      u32 drawCursor = y(cursor) * DRAW_WIDTH + x(cursor);
      frameBuffer[drawCursor] = state.compressedPixels[decompressedPixels];

      blockPart++;
      if (blockPart == SPATIAL_DIFF_BLOCK_SIZE) {
        block++;
        blockPart = 0;
        decompressedPixels++;
      } else if (!isRepeatedBlock)
        decompressedPixels++;
    }
  }
}

inline bool sync(State& state, u32 local, u32 remote) {
  u32 blindFrames = 0;
  bool wasVBlank = IS_VBLANK;

  while (spiSlave->transfer(local) != remote) {
    bool isVBlank = IS_VBLANK;

    if (!wasVBlank && isVBlank) {
      blindFrames++;
      wasVBlank = true;
    } else if (wasVBlank && !isVBlank)
      wasVBlank = false;

    if (blindFrames >= MAX_BLIND_FRAMES)
      return false;
  }

  return true;
}

inline u32 x(u32 cursor) {
  return (cursor % RENDER_WIDTH) * DRAW_SCALE_X;
}

inline u32 y(u32 cursor) {
  return (cursor / RENDER_WIDTH) * DRAW_SCALE_Y;
}
