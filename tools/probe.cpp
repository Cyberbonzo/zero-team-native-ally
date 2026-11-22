// Headless board self-test: runs N frames of the game without SDL, then writes
// the last frame as a 32-bit BMP and reports the translation counters.
#include "zeroteam_board.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static void write_bmp(const char *path, const uint32_t *pixels, int w, int h) {
    const int row = w * 4;
    const int image = row * h;
    unsigned char header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t size = 54 + image;
    std::memcpy(header + 2, &size, 4);
    const uint32_t offset = 54;
    std::memcpy(header + 10, &offset, 4);
    const uint32_t header_size = 40;
    std::memcpy(header + 14, &header_size, 4);
    std::memcpy(header + 18, &w, 4);
    std::memcpy(header + 22, &h, 4);
    const uint16_t planes = 1, bits = 32;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bits, 2);
    std::memcpy(header + 34, &image, 4);
    std::FILE *f = std::fopen(path, "wb");
    if (!f) return;
    std::fwrite(header, 1, sizeof(header), f);
    std::vector<unsigned char> line(row);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t p = pixels[y * w + x];
            line[x * 4 + 0] = p & 0xff;
            line[x * 4 + 1] = (p >> 8) & 0xff;
            line[x * 4 + 2] = (p >> 16) & 0xff;
            line[x * 4 + 3] = 0;
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}

int main(int argc, char **argv) {
    const char *assets = argc > 1 ? argv[1] : "assets";
    const int frames = argc > 2 ? std::atoi(argv[2]) : 120;
    const char *capture = argc > 3 ? argv[3] : nullptr;
    // With --hold every control stays pressed from frame 20, which drives the
    // board's own I/O test screen for an end-to-end input check.
    const bool hold_all = std::getenv("PROBE_HOLD") != nullptr;
    if (zt_init(assets) != 0) {
        std::fprintf(stderr, "probe: board init failed\n");
        return 1;
    }
    std::printf("probe: %dx%d, %.5f Hz\n", ZT_WIDTH, ZT_HEIGHT, zt_fps());
    std::vector<uint32_t> pixels(static_cast<size_t>(ZT_WIDTH) * ZT_HEIGHT);
    std::vector<int16_t> sound(static_cast<size_t>(ZT_MAX_AUDIO_FRAMES) * 2);
    // FNV-1a over every rendered frame and every audio block, so a single
    // divergent instruction anywhere in the run changes the result.
    uint64_t video = 1469598103934665603ull, audio = 1469598103934665603ull;
    for (int i = 0; i < frames; ++i) {
        // Deterministic input script: one credit, one start press, then a
        // repeated attack/jump cadence, so both builds receive the same input.
        // The driver's coin filter wants the line held for four frames.
        unsigned p1 = 0;
        if (hold_all) {
            if (i >= 20) p1 = ZT_COIN | ZT_START | ZT_RIGHT | ZT_ATTACK | ZT_JUMP;
        } else if (i >= 30 && i < 45) p1 |= ZT_COIN;
        if (!hold_all && i >= 70 && i < 78) p1 |= ZT_START;
        if (i >= 140) {
            if ((i / 15) % 3 == 0) p1 |= ZT_ATTACK;
            if ((i / 40) % 4 == 0) p1 |= ZT_JUMP;
            if ((i / 60) % 2 == 0) p1 |= ZT_RIGHT;
        }
        const int samples = zt_frame(pixels.data(), sound.data(), p1, 0);
        if (samples < 0) {
            std::fprintf(stderr, "probe: frame %d failed\n", i);
            return 1;
        }
        const auto *video_bytes = reinterpret_cast<const unsigned char *>(pixels.data());
        for (size_t b = 0; b < pixels.size() * 4; ++b) {
            video = (video ^ video_bytes[b]) * 1099511628211ull;
        }
        for (int s = 0; s < samples * 2; ++s) {
            const unsigned v = static_cast<uint16_t>(sound[s]);
            audio = (audio ^ (v & 0xff)) * 1099511628211ull;
            audio = (audio ^ (v >> 8)) * 1099511628211ull;
        }
    }
    if (capture) write_bmp(capture, pixels.data(), ZT_WIDTH, ZT_HEIGHT);
    std::printf("probe: frames=%d video=%016llx audio=%016llx sites=%u fallback=%u\n", frames,
                static_cast<unsigned long long>(video), static_cast<unsigned long long>(audio),
                zt_aot_sites(), zt_aot_fallback());
    zt_shutdown();
    return 0;
}
