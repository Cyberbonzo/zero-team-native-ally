#include "zeroteam_board.h"
#include "progress.hpp"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
constexpr const char *title = "Zero Team USA";
static volatile std::sig_atomic_t interrupted = 0;
static void stop(int) { interrupted = 1; }

struct Options {
    fs::path assets, saves, capture;
    unsigned frames = 0;
    unsigned present_hz = 60;
    bool headless = false;
    bool stretch = false;
    bool windowed = false;
    bool help = false;
};

static const char *usage =
    "Usage: zeroteam [--assets DIR] [--saves DIR] [--headless --frames N]\n"
    "                [--capture FILE.bmp] [--present-hz N] [--stretch] [--windowed]\n"
    "  --assets DIR     game data (default: ./assets next to the executable)\n"
    "  --saves DIR      save-state folder (default: ./saves next to the executable)\n"
    "  --headless       no video/audio device; requires --frames N\n"
    "  --frames N       stop after N board frames\n"
    "  --capture FILE   write the last board frame as a 320x256 BMP\n"
    "  --present-hz N   display cadence in Hz (default 60)\n"
    "  --stretch        fill the panel, ignoring the 4:3 aspect (default off)\n"
    "  --windowed       run in a 1280x960 window instead of fullscreen\n";

/** Directory holding the executable, so the defaults work from any cwd. */
static fs::path executable_dir() {
    std::error_code error;
    fs::path self = fs::read_symlink("/proc/self/exe", error);
    if (error || self.empty()) return fs::current_path(error);
    return self.parent_path();
}

static unsigned number(const std::string &key, const std::string &argument, unsigned low, unsigned high) {
    size_t used = 0;
    unsigned long value = std::stoul(argument, &used);
    if (used != argument.size() || value < low || value > high)
        throw std::runtime_error("Invalid value for " + key + ": " + argument);
    return static_cast<unsigned>(value);
}

static Options parse(int argc, char **argv) {
    const fs::path base = executable_dir();
    Options opt;
    opt.assets = base / "assets";
    opt.saves = base / "saves";
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        auto value = [&]() {
            if (++i == argc) throw std::runtime_error("Missing value for " + key);
            return std::string(argv[i]);
        };
        if (key == "--assets") opt.assets = value();
        else if (key == "--saves") opt.saves = value();
        else if (key == "--capture") opt.capture = value();
        else if (key == "--present-hz") opt.present_hz = number(key, value(), 1, 1000);
        else if (key == "--frames") opt.frames = number(key, value(), 1, UINT32_MAX);
        else if (key == "--headless") opt.headless = true;
        else if (key == "--stretch") opt.stretch = true;
        else if (key == "--windowed") opt.windowed = true;
        else if (key == "--help" || key == "-h") opt.help = true;
        else throw std::runtime_error("Unknown option: " + key + "\n" + usage);
    }
    if (!opt.help && opt.headless && !opt.frames)
        throw std::runtime_error(std::string("Headless runs need a frame limit\n") + usage);
    return opt;
}

static void sdl_error(const char *where) {
    throw std::runtime_error(std::string(where) + ": " + SDL_GetError());
}

struct Devices {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    SDL_AudioDeviceID audio = 0;
    std::array<SDL_GameController *, 2> pads{};

    ~Devices() {
        for (auto *pad : pads) if (pad) SDL_GameControllerClose(pad);
        if (audio) SDL_CloseAudioDevice(audio);
        if (texture) SDL_DestroyTexture(texture);
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
    }

    void add_pad(int index) {
        if (!SDL_IsGameController(index)) return;
        auto id = SDL_JoystickGetDeviceInstanceID(index);
        for (auto *pad : pads)
            if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) return;
        for (auto &pad : pads) if (!pad) {
            pad = SDL_GameControllerOpen(index);
            if (pad) std::fprintf(stderr, "Gamepad: %s\n", SDL_GameControllerName(pad));
            break;
        }
    }

    void remove_pad(SDL_JoystickID id) {
        for (auto &pad : pads) if (pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) {
            SDL_GameControllerClose(pad);
            pad = nullptr;
        }
    }

    void open(bool windowed) {
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        SDL_DisplayMode mode{};
        if (SDL_GetDesktopDisplayMode(0, &mode) != 0) {
            mode.w = 1920;
            mode.h = 1080;
        }
        if (windowed) {
            window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      1280, 960, SDL_WINDOW_RESIZABLE);
        } else {
            // On the handheld the Gamescope XWayland desktop ignores the
            // fullscreen flag while the window is being created, so the window
            // starts at the panel size and is promoted afterwards. If the
            // compositor still refuses, it stays borderless over the panel.
            window = SDL_CreateWindow(title, 0, 0, mode.w, mode.h,
                                      SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALLOW_HIGHDPI);
            if (!window) sdl_error("Window");
            if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
                std::fprintf(stderr, "Fullscreen request refused: %s\n", SDL_GetError());
        }
        if (!window) sdl_error("Window");
        SDL_ShowCursor(SDL_DISABLE);
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) sdl_error("Renderer");
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING,
                                    ZT_WIDTH, ZT_HEIGHT);
        if (!texture) sdl_error("Game surface");
#if SDL_VERSION_ATLEAST(2, 0, 12)
        if (SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest) != 0) sdl_error("Texture filter");
#endif
        int window_w = 0, window_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        std::fprintf(stderr, "Window: %dx%d, desktop %dx%d, video driver %s\n", window_w,
                     window_h, mode.w, mode.h, SDL_GetCurrentVideoDriver());
        for (int i = 0; i < SDL_NumJoysticks(); ++i) add_pad(i);
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec want{};
            want.freq = ZT_AUDIO_RATE;
            want.format = AUDIO_S16SYS;
            want.channels = 2;
            want.samples = 512;
            audio = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
        }
        if (audio) SDL_PauseAudioDevice(audio, 0);
        else std::fprintf(stderr, "Audio unavailable: %s\n", SDL_GetError());
    }

    unsigned input(size_t player) const {
        unsigned result = 0;
        auto *p = pads[player];
        if (p) {
            auto b = [p](SDL_GameControllerButton button) { return SDL_GameControllerGetButton(p, button); };
            int x = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTX);
            int y = SDL_GameControllerGetAxis(p, SDL_CONTROLLER_AXIS_LEFTY);
            constexpr int deadzone = 10000;
            if (b(SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -deadzone) result |= ZT_UP;
            if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > deadzone) result |= ZT_DOWN;
            if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -deadzone) result |= ZT_LEFT;
            if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > deadzone) result |= ZT_RIGHT;
            if (b(SDL_CONTROLLER_BUTTON_A)) result |= ZT_ATTACK;
            if (b(SDL_CONTROLLER_BUTTON_B)) result |= ZT_JUMP;
            if (b(SDL_CONTROLLER_BUTTON_BACK)) result |= ZT_COIN;
            if (b(SDL_CONTROLLER_BUTTON_START)) result |= ZT_START;
        }
        if (player == 0) {
            const Uint8 *key = SDL_GetKeyboardState(nullptr);
            if (key[SDL_SCANCODE_UP]) result |= ZT_UP;
            if (key[SDL_SCANCODE_DOWN]) result |= ZT_DOWN;
            if (key[SDL_SCANCODE_LEFT]) result |= ZT_LEFT;
            if (key[SDL_SCANCODE_RIGHT]) result |= ZT_RIGHT;
            if (key[SDL_SCANCODE_Z]) result |= ZT_ATTACK;
            if (key[SDL_SCANCODE_X]) result |= ZT_JUMP;
            if (key[SDL_SCANCODE_5]) result |= ZT_COIN;
            if (key[SDL_SCANCODE_1] || key[SDL_SCANCODE_RETURN]) result |= ZT_START;
        }
        return result;
    }

    void upload(const uint32_t *pixels) {
        if (SDL_UpdateTexture(texture, nullptr, pixels, ZT_WIDTH * 4) < 0) sdl_error("Texture update");
    }

    void present(bool paused, bool stretch) {
        int width, height;
        if (SDL_GetRendererOutputSize(renderer, &width, &height) < 0) sdl_error("Display size");
        // The 320x256 raster was shown as a 4:3 picture on the cabinet monitor.
        // Fit the largest 4:3 rectangle inside the panel; the remaining side
        // columns are the black pillarbox bars. --stretch fills the panel.
        SDL_Rect dest{0, 0, width, height};
        if (!stretch) {
            int h = std::min(height, width * 3 / 4);
            int w = h * 4 / 3;
            dest = SDL_Rect{(width - w) / 2, (height - h) / 2, w, h};
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderSetClipRect(renderer, &dest);
        SDL_RenderCopy(renderer, texture, nullptr, &dest);
        SDL_RenderSetClipRect(renderer, nullptr);
        if (paused) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 155);
            SDL_RenderFillRect(renderer, &dest);
            int size = std::max(12, dest.h / 12);
            SDL_Rect left{width / 2 - size / 3, height / 2 - size / 2, size / 4, size};
            SDL_Rect right{width / 2 + size / 12, left.y, left.w, size};
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer, &left);
            SDL_RenderFillRect(renderer, &right);
        }
        SDL_RenderPresent(renderer);
    }
};

static int run(const Options &opt) {
    Uint32 init = opt.headless ? SDL_INIT_TIMER : (SDL_INIT_VIDEO | SDL_INIT_TIMER);
    if (SDL_Init(init) != 0) sdl_error("SDL");
    struct Session { ~Session() { SDL_Quit(); } } session;
    // A missing gamepad subsystem must not take the game down: the keyboard
    // bindings still work and SDL reports the pads it can see.
    if (!opt.headless && SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        std::fprintf(stderr, "Game controllers unavailable: %s\n", SDL_GetError());
    if (zt_init(opt.assets.c_str()) != 0) throw std::runtime_error("Board could not initialize the game");
    struct Board { ~Board() { zt_shutdown(); } } board;
    double fps = zt_fps();
    if (!std::isfinite(fps) || fps < 20 || fps > 240) throw std::runtime_error("Invalid refresh rate from board");
    std::fprintf(stderr, "%s: %dx%d, %.5f Hz logic, %d Hz stereo\n",
                 title, ZT_WIDTH, ZT_HEIGHT, fps, ZT_AUDIO_RATE);
    Devices devices;
    if (!opt.headless) {
        devices.open(opt.windowed);
        std::fprintf(stderr, "Present: %u Hz cadence, %s\n", opt.present_hz,
                     opt.stretch ? "stretched to fill the panel" : "4:3 centred with pillarbox bars");
    }
    Progress progress;
    progress.open(opt.saves);
    std::vector<uint32_t> pixels(ZT_WIDTH * ZT_HEIGHT);
    std::array<int16_t, ZT_MAX_AUDIO_FRAMES * 2> sound{};
    unsigned logic_frames = 0, presents = 0, duplicated = 0, catchups = 0;
    bool paused = false;
    const unsigned save_period = std::max(1u, static_cast<unsigned>(std::round(fps * 30)));
    const double frequency = static_cast<double>(SDL_GetPerformanceFrequency());
    const double started = static_cast<double>(SDL_GetPerformanceCounter());

    auto step = [&]() {
        unsigned p1 = opt.headless ? 0 : devices.input(0);
        unsigned p2 = opt.headless ? 0 : devices.input(1);
        int count = zt_frame(pixels.data(), sound.data(), p1, p2);
        if (count < 0 || count > ZT_MAX_AUDIO_FRAMES)
            throw std::runtime_error("Board failed while running frame " + std::to_string(logic_frames));
        ++logic_frames;
        if (devices.audio && count) {
            if (SDL_GetQueuedAudioSize(devices.audio) > static_cast<Uint32>(ZT_AUDIO_RATE) * 4 / 6)
                SDL_ClearQueuedAudio(devices.audio);
            if (SDL_QueueAudio(devices.audio, sound.data(), static_cast<Uint32>(count) * 4) != 0)
                sdl_error("Queue audio");
        }
        if (logic_frames % save_period == 0) progress.save();
    };

    auto poll = [&]() {
        SDL_Event event;
        while (!opt.headless && SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) interrupted = 1;
            if (event.type == SDL_CONTROLLERDEVICEADDED) devices.add_pad(event.cdevice.which);
            if (event.type == SDL_CONTROLLERDEVICEREMOVED) devices.remove_pad(event.cdevice.which);
            bool key = event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.sym == SDLK_ESCAPE;
            bool pad = event.type == SDL_CONTROLLERBUTTONDOWN && event.cbutton.button == SDL_CONTROLLER_BUTTON_Y;
            if ((key || pad) && !opt.headless) {
                paused = !paused;
                if (paused) {
                    progress.save();
                    if (devices.audio) SDL_ClearQueuedAudio(devices.audio);
                }
            }
        }
    };

    if (opt.headless) {
        // No device and no pacing: the frame limit drives the run.
        while (!interrupted && (!opt.frames || logic_frames < opt.frames)) step();
    } else {
        // Decoupled pacing: the board advances on its own 55.47 Hz clock while
        // the panel is fed on a steady present cadence. A present with no new
        // board frame shows the previous texture again (frame duplication).
        const double logic_period = frequency / fps;
        const double present_period = frequency / static_cast<double>(opt.present_hz);
        constexpr int catchup_limit = 3; // board frames per wake-up, like the reference
        double logic_deadline = started;
        double present_deadline = started;
        bool dirty = false;
        while (!interrupted && (!opt.frames || logic_frames < opt.frames)) {
            poll();
            if (interrupted) break;
            double now = static_cast<double>(SDL_GetPerformanceCounter());
            if (paused) {
                logic_deadline = now + logic_period; // no backlog builds up while paused
            } else {
                int ran = 0;
                while ((!opt.frames || logic_frames < opt.frames) && now >= logic_deadline &&
                       ran < catchup_limit) {
                    step();
                    dirty = true;
                    ++ran;
                    logic_deadline += logic_period;
                    now = static_cast<double>(SDL_GetPerformanceCounter());
                }
                if (now >= logic_deadline + logic_period) {
                    // Scheduler hiccup or suspend: skip the backlog instead of
                    // spiralling into an unbounded catch-up burst.
                    logic_deadline = now;
                    ++catchups;
                }
            }
            now = static_cast<double>(SDL_GetPerformanceCounter());
            if (now >= present_deadline) {
                if (dirty) {
                    devices.upload(pixels.data());
                    dirty = false;
                } else {
                    ++duplicated;
                }
                devices.present(paused, opt.stretch);
                ++presents;
                present_deadline += present_period;
                if (now - present_deadline > present_period) {
                    unsigned missed = static_cast<unsigned>((now - present_deadline) / present_period) + 1;
                    present_deadline += missed * present_period;
                    ++catchups;
                }
            }
            // Sleep to the next deadline in short slices so gamepad input and
            // window events stay responsive between board frames.
            for (;;) {
                now = static_cast<double>(SDL_GetPerformanceCounter());
                double target = paused ? present_deadline : std::min(present_deadline, logic_deadline);
                double remaining_ms = (target - now) * 1000.0 / frequency;
                if (remaining_ms <= 0.5 || interrupted) break;
                SDL_Delay(static_cast<Uint32>(std::min(2.0, remaining_ms - 0.5)));
                poll();
            }
        }
        if (dirty) {
            devices.upload(pixels.data());
            devices.present(paused, opt.stretch);
            ++presents;
        }
    }

    const double ended = static_cast<double>(SDL_GetPerformanceCounter());
    const double elapsed = (ended - started) / frequency;

    if (!opt.capture.empty()) {
        if (!logic_frames) {
            std::fprintf(stderr, "No board frame rendered; capture skipped.\n");
        } else {
            auto *surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), ZT_WIDTH, ZT_HEIGHT,
                                                               32, ZT_WIDTH * 4, SDL_PIXELFORMAT_RGB888);
            if (!surface) sdl_error("Capture");
            int status = SDL_SaveBMP(surface, opt.capture.c_str());
            SDL_FreeSurface(surface);
            if (status != 0) sdl_error("Writing capture");
            std::fprintf(stderr, "Capture: %s (%dx%d)\n", opt.capture.c_str(), ZT_WIDTH, ZT_HEIGHT);
        }
    }
    if (logic_frames) progress.save();

    const double logic_fps = elapsed > 0 ? logic_frames / elapsed : 0.0;
    const double present_fps = elapsed > 0 ? presents / elapsed : 0.0;
    const unsigned shown = presents - duplicated;
    const unsigned dropped = logic_frames > shown ? logic_frames - shown : 0;
    if (opt.headless) {
        std::fprintf(stderr,
                     "Timing: logic %.2f fps average, present 0.00 fps (headless), duplicated 0, dropped 0\n",
                     logic_fps);
    } else {
        std::fprintf(stderr,
                     "Timing: logic %.2f fps average, present %.2f fps average, %u duplicated, %u dropped, "
                     "%u catch-up cap(s)\n",
                     logic_fps, present_fps, duplicated, dropped, catchups);
    }
    return 0;
}

int main(int argc, char **argv) {
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    try {
        const Options opt = parse(argc, argv);
        if (opt.help) {
            std::fputs(usage, stdout);
            return 0;
        }
        return run(opt);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s: %s\n", title, error.what());
        return 1;
    }
}
