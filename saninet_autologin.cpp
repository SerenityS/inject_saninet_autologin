#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace {

constexpr uintptr_t kRvaHandleMessage = 0x006D8F80;
constexpr uintptr_t kRvaHandleInput = 0x006DA000;
constexpr uintptr_t kRvaSaniNetGlobal = 0x00B91900;
constexpr uintptr_t kRvaAssignString = 0x001F6880;
constexpr uintptr_t kRvaSubmitPin = 0x00740490;
constexpr uintptr_t kRvaIsAlive = 0x0073AC20;
constexpr uintptr_t kRvaIsLoginReady = 0x0073AD80;
constexpr size_t kHookPatchSize = 12;

using FnHandleMessage = void(__fastcall*)(void* self, void* message);
using FnHandleInput = std::uint64_t(__fastcall*)(void* self, void* input);
using FnAssignString = void(__fastcall*)(void* dst, const char* src, size_t len);
using FnSubmitPin = void(__fastcall*)(void* sani, void* pinString, void* unused3, void* unused4);
using FnSaniBool = char(__fastcall*)(void* sani);

struct Config {
    std::string pin;
};

struct DetourSlot {
    void* trampoline = nullptr;
    std::array<std::byte, kHookPatchSize> saved{};
};

#pragma pack(push, 1)
struct SyntheticInputEvent {
    std::uint8_t pad0[4];
    std::int32_t code;
    std::uint8_t pad1[0x20 - 8];
    std::int32_t device;
    std::uint8_t pad2[0x28 - 0x24];
    std::int32_t type;
    std::uint8_t pad3[0x30 - 0x2C];
    std::int32_t player;
};
#pragma pack(pop)
static_assert(sizeof(SyntheticInputEvent) == 0x34);

FnHandleMessage g_originalHandleMessage = nullptr;
DetourSlot g_messageDetour;
std::atomic<bool> g_shutdown = false;
std::atomic<DWORD> g_lastAutomationTick = 0;
std::atomic<bool> g_lastReady = false;
std::atomic<int> g_automationStage = 0; // 0 idle, 1 waiting menu, 2 submitted
Config g_config;
void* g_screenSelf = nullptr;
HMODULE g_module = nullptr;

std::filesystem::path ModuleDir(HMODULE module) {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (len == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        len = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    buffer.resize(len);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path DataDir() {
    return ModuleDir(g_module);
}

std::filesystem::path LogPath() {
    return DataDir() / L"saninet_trace.log";
}

std::filesystem::path PinPath() {
    return DataDir() / L"pin.txt";
}

std::string TryReadGameString(void* maybeString) {
    if (maybeString == nullptr) {
        return "<null>";
    }

    auto* obj = static_cast<std::uint64_t*>(maybeString);
    const std::uint64_t len = obj[2];
    const std::uint64_t cap = obj[3];
    if (len > 256) {
        return "<len-too-large>";
    }

    const char* data = reinterpret_cast<const char*>(maybeString);
    if (cap > 0xF) {
        data = reinterpret_cast<const char*>(obj[0]);
    }
    if (data == nullptr) {
        return "<null-data>";
    }

    return std::string(data, data + len);
}

void Log(const std::string& line) {
    std::ofstream out(LogPath(), std::ios::app);
    if (out) {
        out << line << "\n";
    }
}

std::optional<std::string> ReadIniValue(const std::filesystem::path& path, const std::string& key) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind(key + "=", 0) == 0) {
            return line.substr(key.size() + 1);
        }
    }
    return std::nullopt;
}

std::optional<Config> LoadConfig() {
    auto pin = ReadIniValue(PinPath(), "pin");
    if (!pin) {
        std::ifstream pinIn(PinPath());
        if (pinIn) {
            std::string raw;
            std::getline(pinIn, raw);
            pin = raw;
        }
    }

    if (!pin || pin->size() != 4) {
        return std::nullopt;
    }
    if (!std::all_of(pin->begin(), pin->end(), ::isdigit)) {
        return std::nullopt;
    }

    Config cfg;
    cfg.pin = *pin;
    return cfg;
}

uintptr_t ModuleBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

template <typename T>
T Resolve(uintptr_t rva) {
    return reinterpret_cast<T>(ModuleBase() + rva);
}

void* ResolveSaniNetObject() {
    auto ptr = *reinterpret_cast<void**>(ModuleBase() + kRvaSaniNetGlobal);
    if (ptr != nullptr) {
        return ptr;
    }
    return reinterpret_cast<void*>(ModuleBase() + kRvaSaniNetGlobal);
}

void FailMessage(const wchar_t* text) {
    MessageBoxW(nullptr, text, L"Saninet Auto Login", MB_OK | MB_ICONERROR);
}

void RemoveDetour(void* target, const DetourSlot& slot) {
    if (target == nullptr || slot.trampoline == nullptr) {
        return;
    }

    auto* dst = static_cast<std::byte*>(target);
    DWORD oldProtect = 0;
    if (!VirtualProtect(dst, kHookPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }

    std::memcpy(dst, slot.saved.data(), kHookPatchSize);
    DWORD dummy = 0;
    VirtualProtect(dst, kHookPatchSize, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), dst, kHookPatchSize);
}

bool InstallDetour(void* target, void* hook, DetourSlot& slot, void** originalOut) {
    auto* src = static_cast<std::byte*>(target);
    std::memcpy(slot.saved.data(), src, kHookPatchSize);

    slot.trampoline = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!slot.trampoline) {
        return false;
    }

    auto* tramp = static_cast<std::byte*>(slot.trampoline);
    std::memcpy(tramp, src, kHookPatchSize);
    tramp += kHookPatchSize;
    tramp[0] = std::byte{0x48};
    tramp[1] = std::byte{0xB8};
    *reinterpret_cast<uintptr_t*>(tramp + 2) = reinterpret_cast<uintptr_t>(src + kHookPatchSize);
    tramp[10] = std::byte{0xFF};
    tramp[11] = std::byte{0xE0};

    DWORD oldProtect = 0;
    if (!VirtualProtect(src, kHookPatchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    src[0] = std::byte{0x48};
    src[1] = std::byte{0xB8};
    *reinterpret_cast<uintptr_t*>(src + 2) = reinterpret_cast<uintptr_t>(hook);
    src[10] = std::byte{0xFF};
    src[11] = std::byte{0xE0};

    DWORD dummy = 0;
    VirtualProtect(src, kHookPatchSize, oldProtect, &dummy);
    FlushInstructionCache(GetCurrentProcess(), src, kHookPatchSize);

    *originalOut = slot.trampoline;
    return true;
}

bool IsReadyNow() {
    auto sani = ResolveSaniNetObject();
    if (sani == nullptr) {
        return false;
    }

    auto isAlive = Resolve<FnSaniBool>(kRvaIsAlive);
    auto isLoginReady = Resolve<FnSaniBool>(kRvaIsLoginReady);
    return isAlive(sani) != 0 && isLoginReady(sani) != 0;
}

void SendInternalKey(void* self, std::int32_t code) {
    if (self == nullptr) {
        return;
    }

    SyntheticInputEvent ev{};
    ev.code = code;
    ev.device = 0;
    ev.type = 0;
    ev.player = 0;

    auto handleInput = Resolve<FnHandleInput>(kRvaHandleInput);
    handleInput(self, &ev);
}

void TrySubmitPin(void* self) {
    if (g_shutdown.load() || self == nullptr) {
        return;
    }

    auto assignString = Resolve<FnAssignString>(kRvaAssignString);
    auto submitPin = Resolve<FnSubmitPin>(kRvaSubmitPin);
    auto sani = ResolveSaniNetObject();
    if (!sani) {
        return;
    }

    auto* pinString = static_cast<void*>(static_cast<std::byte*>(self) + 0x3270);
    assignString(pinString, g_config.pin.c_str(), g_config.pin.size());
    submitPin(sani, pinString, nullptr, nullptr);
}

void TryStartAutomation(void* self) {
    const DWORD now = GetTickCount();
    const DWORD last = g_lastAutomationTick.load();
    if (last != 0 && (now - last) < 1500) {
        return;
    }

    g_lastAutomationTick.store(now);
    g_automationStage.store(1);
    Log("TryStartAutomation: ready edge");
    SendInternalKey(self, 0x90);
}

DWORD WINAPI PollThread(void*) {
    while (!g_shutdown.load()) {
        const bool ready = IsReadyNow();
        const bool previousReady = g_lastReady.exchange(ready);
        if (!previousReady && ready) {
            void* self = g_screenSelf;
            if (self != nullptr) {
                TryStartAutomation(self);
            }
        }
        if (!ready) {
            g_automationStage.store(0);
        }
        Sleep(200);
    }
    return 0;
}

void __fastcall HookHandleMessage(void* self, void* message) {
    if (g_shutdown.load()) {
        if (g_originalHandleMessage) {
            g_originalHandleMessage(self, message);
        }
        return;
    }

    g_screenSelf = self;
    const std::string msg = TryReadGameString(message);

    g_originalHandleMessage(self, message);

    if (g_automationStage.load() == 1 && msg == "SaniNetLoginMenu") {
        const int loginActive = static_cast<int>(
            *reinterpret_cast<unsigned char*>(static_cast<std::byte*>(self) + 0x3269));
        if (loginActive != 0) {
            TrySubmitPin(self);
            g_automationStage.store(2);
        }
    }
}

DWORD WINAPI WorkerThread(void*) {
    std::error_code ec;
    std::filesystem::remove(LogPath(), ec);
    Log("WorkerThread: start");

    auto cfg = LoadConfig();
    if (!cfg) {
        Log("WorkerThread: config load failed");
        FailMessage(L"Failed to read PIN file.");
        return 0;
    }
    g_config = *cfg;
    Log("WorkerThread: config loaded");

    auto handleMessage = Resolve<void*>(kRvaHandleMessage);
    if (!InstallDetour(handleMessage, reinterpret_cast<void*>(&HookHandleMessage), g_messageDetour,
                       reinterpret_cast<void**>(&g_originalHandleMessage))) {
        Log("WorkerThread: message detour failed");
        FailMessage(L"Failed to install message hook.");
        return 0;
    }

    HANDLE poll = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
    if (poll != nullptr) {
        CloseHandle(poll);
    } else {
        Log("WorkerThread: poll start failed");
    }

    Log("WorkerThread: detours installed");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        g_shutdown.store(false);
        CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_shutdown.store(true);
        RemoveDetour(Resolve<void*>(kRvaHandleMessage), g_messageDetour);
    }
    return TRUE;
}





