#pragma once

#include "zeroteam_board.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

class Progress {
    std::filesystem::path folder;
    size_t bytes = 0;
    int lock = -1;

    struct Header {
        char magic[8];
        uint32_t format;
        uint32_t checksum;
        uint64_t length;
    };

    static uint32_t checksum(const std::vector<uint8_t> &data) {
        uint32_t value = 2166136261u;
        for (auto byte : data)
            value = (value ^ byte) * 16777619u;
        return value;
    }

public:
    ~Progress() { if (lock >= 0) close(lock); }

    void open(const std::filesystem::path &directory) {
        size_t reported = zt_state_size();
        if (!reported || directory.empty()) {
            // The board may be built without a verified save-state (size 0).
            // That is a supported configuration: play without persistence.
            if (!reported) std::fprintf(stderr, "Board has no save-state support; progress disabled.\n");
            return;
        }
        if (reported > 128u * 1024 * 1024) {
            std::fprintf(stderr, "Unexpected board state size: %zu\n", reported);
            return;
        }
        folder = directory;
        std::error_code error;
        std::filesystem::create_directories(folder, error);
        if (!error)
            lock = ::open((folder / "progress.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
            std::fprintf(stderr, "Could not lock progress in %s\n", folder.c_str());
            return;
        }
        bytes = reported;
        FILE *f = std::fopen((folder / "progress.zt").c_str(), "rb");
        if (!f) {
            if (errno != ENOENT) {
                std::perror("Reading existing progress");
                bytes = 0;
            }
            return;
        }
        Header header{};
        std::vector<uint8_t> data(bytes);
        bool ok = std::fread(&header, 1, sizeof(header), f) == sizeof(header)
            && std::memcmp(header.magic, "ZEROTEAM", 8) == 0 && header.format == 1
            && header.length == bytes && std::fread(data.data(), 1, bytes, f) == bytes
            && std::fgetc(f) == EOF && !std::ferror(f) && header.checksum == checksum(data);
        std::fclose(f);
        if (!ok || zt_load_state(data.data(), bytes) != 0) {
            std::fprintf(stderr, "Existing save rejected; preserving it on disk.\n");
            bytes = 0;
        } else {
            std::fprintf(stderr, "Loaded saved game.\n");
        }
    }

    void save() {
        if (!bytes) return;
        std::vector<uint8_t> data(bytes);
        if (zt_save_state(data.data(), bytes) != 0) {
            std::fprintf(stderr, "Could not serialize game progress.\n");
            return;
        }
        Header header{{'Z','E','R','O','T','E','A','M'}, 1, checksum(data), bytes};
        auto temporary = folder / "progress.tmp";
        FILE *f = std::fopen(temporary.c_str(), "wb");
        if (!f) { std::perror("Opening save"); return; }
        bool ok = std::fwrite(&header, 1, sizeof(header), f) == sizeof(header)
            && std::fwrite(data.data(), 1, bytes, f) == bytes
            && std::fflush(f) == 0 && fsync(fileno(f)) == 0;
        if (std::fclose(f) != 0) ok = false;
        if (ok && std::rename(temporary.c_str(), (folder / "progress.zt").c_str()) == 0) {
            int fd = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY);
            if (fd >= 0) { fsync(fd); close(fd); }
        } else {
            std::perror("Writing save");
            std::remove(temporary.c_str());
        }
    }
};
