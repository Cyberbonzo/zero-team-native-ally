// Single-title Seibu V30 board adapter for Zero Team (USA).
//
// The board reuses FBNeo's Zero Team driver for the memory map, the Seibu COP
// (SEI252) sprite device, the YM2151 + 2x MSM6295 sound section and the video
// mixer. Only the V30 program execution is replaced by the static translation,
// which is why the CPU core is linked from the generated sources.
#include "burnint.h"
#include "timer.h"
#pragma GCC visibility push(default)
#include "zeroteam_board.h"
#pragma GCC visibility pop
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <vector>
#include <zlib.h>

extern BurnDriver BurnDrvZeroteam;

// Exposed by the private copy of the driver so the build can compare the
// program image it translated against the one the driver assembles.
extern "C" UINT8 *zt_main_rom_ptr(void);
extern "C" UINT8 *zt_main_ram_ptr(void);

// Provided by the generated V30 translation, or stubbed for reference builds.
#ifndef ZEROTEAM_AOT
unsigned zt_aot_sites(void) { return 0; }
unsigned zt_aot_fallback(void) { return 0; }
#endif

static std::unordered_map<uint32_t, std::vector<unsigned char>> chips;
static bool active;

namespace {
enum Field { P1_COIN, P1_START, P1_UP, P1_DOWN, P1_LEFT, P1_RIGHT, P1_B1, P1_B2,
             P2_COIN, P2_START, P2_UP, P2_DOWN, P2_LEFT, P2_RIGHT, P2_B1, P2_B2,
             DIP_A, DIP_B, DIP_C, FIELD_COUNT };

const char *field_names[FIELD_COUNT] = {
    "p1 coin", "p1 start", "p1 up", "p1 down", "p1 left", "p1 right",
    "p1 fire 1", "p1 fire 2",
    "p2 coin", "p2 start", "p2 up", "p2 down", "p2 left", "p2 right",
    "p2 fire 1", "p2 fire 2",
    "Dip A", "Dip B", "Dip C",
};

const char *field_labels[FIELD_COUNT] = {
    "P1 Coin", "P1 Start", "P1 Up", "P1 Down", "P1 Left", "P1 Right",
    "P1 Button 1", "P1 Button 2",
    "P2 Coin", "P2 Start", "P2 Up", "P2 Down", "P2 Left", "P2 Right",
    "P2 Button 1", "P2 Button 2",
    "Dip A", "Dip B", "Dip C",
};

int field_index[FIELD_COUNT] = {};

// Arcade defaults of the USA set: coins 1/1, difficulty normal, three lives,
// demo sound on, two-player cabinet, service mode off.
const unsigned char dip_defaults[3] = {0xff, 0xff, 0xfd};
} // namespace

static INT32 load_chip(UINT8 *dst, INT32 *written, INT32 index) {
    BurnRomInfo rom{};
    if (BurnDrvZeroteam.GetRomInfo(&rom, index)) return 1;
    auto chip = chips.find(rom.nCrc);
    if (chip == chips.end() || chip->second.size() != rom.nLen) {
        std::fprintf(stderr, "zeroteam: missing chip %08x (index %d)\n", rom.nCrc, index);
        return 1;
    }
    std::memcpy(dst, chip->second.data(), rom.nLen);
    if (written) *written = rom.nLen;
    return 0;
}

static UINT32 xrgb(INT32 r, INT32 g, INT32 b, INT32) {
    return (UINT32(r) << 16) | (UINT32(g) << 8) | UINT32(b);
}

static void resolve_inputs() {
    for (int &slot : field_index) slot = -1;
    for (INT32 i = 0;; ++i) {
        BurnInputInfo input{};
        if (BurnDrvZeroteam.GetInputInfo(&input, i)) break;
        for (int f = 0; f < FIELD_COUNT; ++f) {
            // Digital inputs are matched by their short info string; the three
            // DIP switches all share "dip" and are matched by their label.
            const bool dip = f >= DIP_A;
            const char *key = dip ? input.szName : input.szInfo;
            if (std::strcmp(key, dip ? field_labels[f] : field_names[f]) != 0) continue;
            if (dip && input.nType != BIT_DIPSWITCH) continue;
            if (!dip && input.nType == BIT_DIPSWITCH) continue;
            field_index[f] = i;
            break;
        }
    }
    for (int f = 0; f < FIELD_COUNT; ++f) {
        if (field_index[f] < 0)
            std::fprintf(stderr, "zeroteam: input %s not exposed by the driver\n",
                         field_labels[f]);
    }
}

int zt_init(const char *directory) {
    if (active || !directory) return -1;
    chips.clear();
    try {
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file() || !entry.file_size()) continue;
            std::ifstream stream(entry.path(), std::ios::binary);
            std::vector<unsigned char> bytes(entry.file_size());
            if (!stream.read(reinterpret_cast<char *>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size())))
                return -1;
            const auto crc = crc32(0, bytes.data(), static_cast<uInt>(bytes.size()));
            chips.emplace(crc, std::move(bytes));
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "zeroteam: reading chip directory: %s\n", e.what());
        return -1;
    }
    if (chips.empty()) {
        std::fprintf(stderr, "zeroteam: no chips in %s\n", directory);
        return -1;
    }

    BurnLibInit();
    nBurnDrvActive = 0;
    nBurnBpp = 4;
    nBurnPitch = ZT_WIDTH * nBurnBpp;
    nBurnSoundRate = ZT_AUDIO_RATE;
    BurnHighCol = xrgb;
    BurnExtLoadRom = load_chip;
    BurnInitMemoryManager();
    BurnTimerPreInit();

    resolve_inputs();
    for (int i = 0; i < 3; ++i) {
        const int field = DIP_A + i;
        if (field_index[field] < 0) continue;
        BurnInputInfo input{};
        BurnDrvZeroteam.GetInputInfo(&input, field_index[field]);
        // Diagnostics can override a switch bank (for example to reach the
        // board's I/O test); gameplay always starts from the arcade defaults.
        static const char *env_names[3] = {"ZEROTEAM_DIP_A", "ZEROTEAM_DIP_B",
                                           "ZEROTEAM_DIP_C"};
        const char *override = std::getenv(env_names[i]);
        *input.pVal = override ? static_cast<unsigned>(std::strtoul(override, nullptr, 16))
                               : dip_defaults[i];
    }

    const auto result = BurnDrvZeroteam.Init();
    chips.clear();
    if (result != 0) {
        std::fprintf(stderr, "zeroteam: driver init failed (%d)\n", result);
        BurnExitMemoryManager();
        BurnLibExit();
        return -1;
    }
    if (std::getenv("ZEROTEAM_DUMP_ROM")) {
        UINT8 *rom = zt_main_rom_ptr();
        std::ofstream out(std::getenv("ZEROTEAM_DUMP_ROM"), std::ios::binary);
        out.write(reinterpret_cast<const char *>(rom), 0x100000);
    }
    active = true;
    return 0;
}

double zt_fps(void) { return nBurnFPS / 100.0; }

int zt_frame(uint32_t *pixels, int16_t *stereo, unsigned p1, unsigned p2) {
    if (!active || !pixels || !stereo) return -1;
    // Diagnostic: hold words in main RAM at caller-chosen values, which is how
    // the investigation located the game's own screen-width constant.
    struct Poke { unsigned address; unsigned value; };
    static std::vector<Poke> pokes;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *spec = std::getenv("ZEROTEAM_POKE_RAM")) {
            const char *at = spec;
            while (*at) {
                unsigned address = 0, value = 0;
                if (std::sscanf(at, "%x:%x", &address, &value) != 2) break;
                pokes.push_back({address, value});
                const char *next = std::strchr(at, ';');
                if (!next) break;
                at = next + 1;
            }
        }
    }
    if (!pokes.empty()) {
        UINT8 *ram = zt_main_ram_ptr();
        for (const Poke &poke : pokes) {
            if (poke.address + 1 >= 0x20000) continue;
            ram[poke.address] = poke.value & 0xff;
            ram[poke.address + 1] = (poke.value >> 8) & 0xff;
        }
    }
    const unsigned players[2] = {p1, p2};
    // Ordered exactly like the field table above: coin, start, four directions,
    // then the two action buttons.
    static const std::array<unsigned, 8> bits = {
        ZT_COIN, ZT_START, ZT_UP, ZT_DOWN, ZT_LEFT, ZT_RIGHT, ZT_ATTACK, ZT_JUMP};
    for (int p = 0; p < 2; ++p) {
        const int base = p == 0 ? P1_COIN : P2_COIN;
        for (unsigned i = 0; i < 8; ++i) {
            const int field = base + static_cast<int>(i);
            if (field_index[field] < 0) continue;
            BurnInputInfo input{};
            BurnDrvZeroteam.GetInputInfo(&input, field_index[field]);
            *input.pVal = (players[p] & bits[i]) != 0;
        }
    }
    pBurnDraw = reinterpret_cast<UINT8 *>(pixels);
    pBurnSoundOut = stereo;
    std::memset(stereo, 0, sizeof(*stereo) * 2 * nBurnSoundLen);
    ++nCurrentFrame;
    return BurnDrvZeroteam.Frame() == 0 ? nBurnSoundLen : -1;
}

void zt_shutdown(void) {
    if (!active) return;
    BurnDrvZeroteam.Exit();
    BurnExitMemoryManager();
    BurnLibExit();
    active = false;
}

// Save states are not exposed yet: the driver's scan covers the board, but the
// generated V30 state has to be added and verified before shipping them.
size_t zt_state_size(void) { return 0; }
int zt_save_state(void *, size_t) { return -1; }
int zt_load_state(const void *, size_t) { return -1; }
