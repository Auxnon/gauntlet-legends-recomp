// Gauntlet Legends: Recompiled - feasibility spike entry point.
//
// Usage: GauntletRecompiled <path to Gauntlet Legends (US) .z64>
//
// No launcher UI: the ROM path comes from the command line and the game starts
// immediately. Keyboard: arrows = stick, X = A, C = B, Z = Z, Enter = Start,
// A/S = L/R, IJKL = C buttons. The first SDL game controller also works.

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"
#include "SDL2/SDL_syswm.h"
#undef None
#undef Status
#undef LockMask
#undef ControlMask
#undef Success
#undef Always

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/rsp.hpp"

#include "rt64_context.h"
#include "../generated/segments.h"
#include "../RecompiledFuncs/recomp_overlays.inl"

namespace gauntlet {
    void init_tlb(uint8_t* rdram);
}

extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_8020010C(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

static const std::u8string game_id = u8"gauntlet.n64.us";

[[noreturn]] static void exit_error(const std::string& message) {
    fprintf(stderr, "%s\n", message.c_str());
    std::exit(EXIT_FAILURE);
}

// ---- Graphics -----------------------------------------------------------------

static SDL_Window* window = nullptr;

// Launched from an environment without WAYLAND_DISPLAY (ssh, some IDE terminals), SDL
// can't find the compositor even though one is running. Use the first wayland-N socket
// in XDG_RUNTIME_DIR in that case.
static void find_wayland_display() {
    const char* runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (getenv("WAYLAND_DISPLAY") != nullptr || runtime_dir == nullptr) {
        return;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(runtime_dir, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("wayland-", 0) == 0 && name.find('.') == std::string::npos && entry.is_socket(ec)) {
            fprintf(stderr, "[gauntlet] WAYLAND_DISPLAY unset, using %s\n", name.c_str());
            setenv("WAYLAND_DISPLAY", name.c_str(), 0);
            return;
        }
    }
}

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    find_wayland_display();
    // Prefer native Wayland and fall back to X11. SDL_VIDEODRIVER overrides this.
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::string msg = std::string("Failed to initialize SDL: ") + SDL_GetError();
        SDL_version linked;
        SDL_GetVersion(&linked);
        msg += "\n  SDL " + std::to_string(linked.major) + "." + std::to_string(linked.minor) + "." + std::to_string(linked.patch) + ", video drivers:";
        for (int i = 0; i < SDL_GetNumVideoDrivers(); i++) {
            msg += std::string(" ") + SDL_GetVideoDriver(i);
        }
        for (const char* var : { "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DISPLAY", "SDL_VIDEODRIVER" }) {
            const char* value = getenv(var);
            msg += std::string("\n  ") + var + "=" + (value ? value : "(unset)");
        }
        exit_error(msg);
    }
    return {};
}

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    window = SDL_CreateWindow("Gauntlet Legends: Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 960, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (window == nullptr) {
        exit_error(std::string("Failed to create window: ") + SDL_GetError());
    }
    return ultramodern::renderer::WindowHandle{ window };
}

static void update_gfx(void*) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            ultramodern::quit();
        }
    }
}

// Start the game only after the VI thread has run with its placeholder video mode.
// Starting earlier leaves the VI thread with no mode until the game sets one.
static void on_vi() {
    static int vi_count = 0;
    if (++vi_count == 2) {
        recomp::start_game(game_id);
    }
}

// ---- Input ----------------------------------------------------------------------

// N64 controller button bits (libultra's os_cont.h).
enum : uint16_t {
    A_BUTTON = 0x8000, B_BUTTON = 0x4000, Z_TRIG = 0x2000, START_BUTTON = 0x1000,
    U_JPAD = 0x0800, D_JPAD = 0x0400, L_JPAD = 0x0200, R_JPAD = 0x0100,
    L_TRIG = 0x0020, R_TRIG = 0x0010,
    U_CBUTTONS = 0x0008, D_CBUTTONS = 0x0004, L_CBUTTONS = 0x0002, R_CBUTTONS = 0x0001,
};

static SDL_GameController* controller = nullptr;

static void poll_input() {
    if (controller == nullptr) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                controller = SDL_GameControllerOpen(i);
                break;
            }
        }
    }
}

static bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num != 0) {
        return false;
    }

    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint16_t pressed = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;

    struct KeyBinding { SDL_Scancode key; uint16_t button; };
    static const KeyBinding key_bindings[] = {
        { SDL_SCANCODE_X, A_BUTTON }, { SDL_SCANCODE_C, B_BUTTON }, { SDL_SCANCODE_Z, Z_TRIG },
        { SDL_SCANCODE_RETURN, START_BUTTON }, { SDL_SCANCODE_A, L_TRIG }, { SDL_SCANCODE_S, R_TRIG },
        { SDL_SCANCODE_I, U_CBUTTONS }, { SDL_SCANCODE_K, D_CBUTTONS },
        { SDL_SCANCODE_J, L_CBUTTONS }, { SDL_SCANCODE_L, R_CBUTTONS },
    };
    for (const KeyBinding& binding : key_bindings) {
        if (keys[binding.key]) {
            pressed |= binding.button;
        }
    }
    stick_x += keys[SDL_SCANCODE_RIGHT] - keys[SDL_SCANCODE_LEFT];
    stick_y += keys[SDL_SCANCODE_UP] - keys[SDL_SCANCODE_DOWN];

    if (controller != nullptr) {
        struct PadBinding { SDL_GameControllerButton pad; uint16_t button; };
        static const PadBinding pad_bindings[] = {
            { SDL_CONTROLLER_BUTTON_A, A_BUTTON }, { SDL_CONTROLLER_BUTTON_X, B_BUTTON },
            { SDL_CONTROLLER_BUTTON_START, START_BUTTON }, { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, Z_TRIG },
            { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, R_TRIG },
            { SDL_CONTROLLER_BUTTON_DPAD_UP, U_JPAD }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, D_JPAD },
            { SDL_CONTROLLER_BUTTON_DPAD_LEFT, L_JPAD }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, R_JPAD },
        };
        for (const PadBinding& binding : pad_bindings) {
            if (SDL_GameControllerGetButton(controller, binding.pad)) {
                pressed |= binding.button;
            }
        }
        stick_x += SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX) / 32768.0f;
        stick_y -= SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY) / 32768.0f;
    }

    *buttons = pressed;
    *x = std::clamp(stick_x, -1.0f, 1.0f);
    *y = std::clamp(stick_y, -1.0f, 1.0f);
    return true;
}

static void set_rumble(int, bool) {}

static ultramodern::input::connected_device_info_t get_connected_device_info(int controller_num) {
    if (controller_num == 0) {
        return { ultramodern::input::Device::Controller, ultramodern::input::Pak::None };
    }
    return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
}

// ---- Audio ----------------------------------------------------------------------

static SDL_AudioDeviceID audio_device = 0;
static uint32_t sample_rate = 44100;

static void open_audio(uint32_t frequency) {
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
    }
    SDL_AudioSpec spec{};
    spec.freq = (int)frequency;
    spec.format = AUDIO_S16SYS;
    spec.channels = 2;
    spec.samples = 0x100;
    audio_device = SDL_OpenAudioDevice(nullptr, false, &spec, nullptr, 0);
    if (audio_device != 0) {
        SDL_PauseAudioDevice(audio_device, 0);
    }
    sample_rate = frequency;
}

static void queue_samples(int16_t* audio_data, size_t sample_count) {
    if (audio_device == 0) {
        return;
    }
    // The game's buffer is word-swapped by the runtime's endianness handling.
    std::vector<int16_t> swapped(sample_count);
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        swapped[i] = audio_data[i + 1];
        swapped[i + 1] = audio_data[i];
    }
    SDL_QueueAudio(audio_device, swapped.data(), (Uint32)(swapped.size() * sizeof(int16_t)));
}

static size_t get_frames_remaining() {
    return audio_device == 0 ? 0 : SDL_GetQueuedAudioSize(audio_device) / (2 * sizeof(int16_t));
}

static void set_frequency(uint32_t frequency) {
    open_audio(frequency);
}

// ---- RSP --------------------------------------------------------------------------

// The audio microcode isn't recompiled yet, so audio tasks complete without output.
static RspExitReason skip_audio_task(uint8_t*, uint32_t) {
    return RspExitReason::Broke;
}

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task->t.type == M_AUDTASK) {
        return skip_audio_task;
    }
    fprintf(stderr, "Unknown RSP task type: %" PRIu32 "\n", task->t.type);
    return nullptr;
}

// ---- Game -----------------------------------------------------------------------

// Runs after librecomp has loaded boot and before any game code runs.
static void on_init(uint8_t* rdram, recomp_context*) {
    gauntlet::init_tlb(rdram);

    // Native replacements that recompiled code reaches through indirect jumps.
    recomp::overlays::add_loaded_function((int32_t)0x8020010C, func_8020010C);

    // The game code and level overlays are decompressed by the game itself, never
    // DMA'd straight from ROM, so register their recompiled functions up front. Their
    // virtual address ranges don't overlap, so they can all be resident at once.
    load_overlays(SEGMENT_GAME_ENTRY_KSEG0_ROM, (int32_t)SEGMENT_GAME_ENTRY_KSEG0_VRAM, SEGMENT_GAME_ENTRY_KSEG0_SIZE);
    load_overlays(SEGMENT_GAME_ROM, (int32_t)SEGMENT_GAME_VRAM, SEGMENT_GAME_SIZE);
    load_overlays(SEGMENT_TRANSITIONS_ROM, (int32_t)SEGMENT_TRANSITIONS_VRAM, SEGMENT_TRANSITIONS_SIZE);
    load_overlays(SEGMENT_TOWER_ROM, (int32_t)SEGMENT_TOWER_VRAM, SEGMENT_TOWER_SIZE);
    load_overlays(SEGMENT_BOSS_ROM, (int32_t)SEGMENT_BOSS_VRAM, SEGMENT_BOSS_SIZE);
}

static void register_overlays() {
    recomp::overlays::overlay_section_table_data_t sections{
        .code_sections = section_table,
        .num_code_sections = ARRLEN(section_table),
        .total_num_sections = num_sections,
    };
    recomp::overlays::overlays_by_index_t overlays{
        .table = overlay_sections_by_index,
        .len = ARRLEN(overlay_sections_by_index),
    };
    recomp::overlays::register_overlays(sections, overlays);
}

static void message_box(const char* message) {
    fprintf(stderr, "%s\n", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Gauntlet Legends: Recompiled", message, window);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        exit_error("Usage: GauntletRecompiled <path to Gauntlet Legends (US) .z64>");
    }

    recomp::Version version{};
    recomp::Version::from_string("0.1.0", version);

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    open_audio(sample_rate);

    std::filesystem::path config_path = std::filesystem::path(SDL_GetPrefPath("gauntlet-recomp", "GauntletLegendsRecompiled"));
    recomp::register_config_path(config_path);

    recomp::register_game({
        .rom_hash = 0x88845DB2CBC34EE7ULL,
        .internal_name = "GAUNTLET LEGENDS",
        .game_id = game_id,
        .mod_game_id = "",
        .save_type = recomp::SaveType::AllowAll,
        .is_enabled = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = recomp_entrypoint,
        .on_init_callback = on_init,
    });
    register_overlays();

    std::u8string selected_game = game_id;
    recomp::RomValidationError rom_error = recomp::select_rom(argv[1], selected_game);
    if (rom_error != recomp::RomValidationError::Good) {
        exit_error("ROM rejected (error " + std::to_string((int)rom_error) + "). Expected Gauntlet Legends (US), sha1 0489dcce749c6a5102681d288ed0616a4b94e99d.");
    }

    recomp::start(
        version,
        {},
        recomp::rsp::callbacks_t{ .get_rsp_microcode = get_rsp_microcode },
        ultramodern::renderer::callbacks_t{ .create_render_context = gauntlet::create_render_context },
        ultramodern::audio_callbacks_t{
            .queue_samples = queue_samples,
            .get_frames_remaining = get_frames_remaining,
            .set_frequency = set_frequency,
        },
        ultramodern::input::callbacks_t{
            .poll_input = poll_input,
            .get_input = get_input,
            .set_rumble = set_rumble,
            .get_connected_device_info = get_connected_device_info,
        },
        ultramodern::gfx_callbacks_t{
            .create_gfx = create_gfx,
            .create_window = create_window,
            .update_gfx = update_gfx,
        },
        ultramodern::events::callbacks_t{ .vi_callback = on_vi },
        ultramodern::error_handling::callbacks_t{ .message_box = message_box },
        ultramodern::threads::callbacks_t{}
    );

    return EXIT_SUCCESS;
}
