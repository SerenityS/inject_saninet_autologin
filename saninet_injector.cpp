#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct ProcessInfo {
    DWORD pid = 0;
    ULONGLONG created = 0;
};

std::filesystem::path ModuleDir() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (len == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    buffer.resize(len);
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path BaseDir() {
    const auto cwd = std::filesystem::current_path();
    if (std::filesystem::exists(cwd / L"XSanity.exe")) {
        return cwd;
    }

    const auto moduleDir = ModuleDir();
    if (std::filesystem::exists(moduleDir / L"XSanity.exe")) {
        return moduleDir;
    }

    return cwd;
}

std::filesystem::path GameExePath() {
    return BaseDir() / L"XSanity.exe";
}

std::filesystem::path PinPath() {
    return BaseDir() / L"pin.txt";
}

std::filesystem::path GetDefaultDllPath() {
    return BaseDir() / L"saninet_autologin.dll";
}

std::optional<ULONGLONG> GetProcessCreateTime(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return std::nullopt;
    }

    FILETIME createTime{}, exitTime{}, kernelTime{}, userTime{};
    const BOOL ok = GetProcessTimes(process, &createTime, &exitTime, &kernelTime, &userTime);
    CloseHandle(process);
    if (!ok) {
        return std::nullopt;
    }

    ULARGE_INTEGER value{};
    value.LowPart = createTime.dwLowDateTime;
    value.HighPart = createTime.dwHighDateTime;
    return value.QuadPart;
}

std::vector<ProcessInfo> FindProcesses(const wchar_t* exeName) {
    std::vector<ProcessInfo> processes;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, exeName) == 0) {
                if (auto created = GetProcessCreateTime(entry.th32ProcessID)) {
                    processes.push_back(ProcessInfo{entry.th32ProcessID, *created});
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processes;
}

DWORD FindNewestProcessId(const wchar_t* exeName) {
    const auto processes = FindProcesses(exeName);
    if (processes.empty()) {
        return 0;
    }

    const auto newest = std::max_element(
        processes.begin(),
        processes.end(),
        [](const ProcessInfo& a, const ProcessInfo& b) { return a.created < b.created; });
    return newest->pid;
}

DWORD WaitForStableProcessId(const wchar_t* exeName, DWORD timeoutMs, DWORD stableMs = 2000) {
    const DWORD sleepMs = 250;
    DWORD currentPid = 0;
    DWORD stableFor = 0;

    for (DWORD waited = 0; waited < timeoutMs; waited += sleepMs) {
        const DWORD newestPid = FindNewestProcessId(exeName);
        if (newestPid == 0) {
            currentPid = 0;
            stableFor = 0;
            Sleep(sleepMs);
            continue;
        }

        if (newestPid == currentPid) {
            stableFor += sleepMs;
        } else {
            currentPid = newestPid;
            stableFor = 0;
        }

        if (stableFor >= stableMs) {
            return currentPid;
        }

        Sleep(sleepMs);
    }

    return currentPid;
}

bool IsValidPin(const std::wstring& pin) {
    return pin.size() == 4 && std::all_of(pin.begin(), pin.end(), iswdigit);
}

std::optional<std::wstring> LoadSavedPin() {
    std::ifstream in(PinPath());
    if (!in) {
        return std::nullopt;
    }

    std::string raw;
    std::getline(in, raw);

    std::wstring pin;
    pin.reserve(raw.size());
    for (char ch : raw) {
        pin.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
    }

    if (IsValidPin(pin)) {
        return pin;
    }
    return std::nullopt;
}

bool SavePin(const std::wstring& pin) {
    std::ofstream out(PinPath());
    if (!out) {
        return false;
    }

    for (wchar_t ch : pin) {
        out << static_cast<char>(ch);
    }
    return true;
}

std::wstring PromptPin() {
    while (true) {
        std::wcout << L"Enter 4-digit SaniNet PIN: ";
        std::wstring pin;
        std::getline(std::wcin, pin);
        if (IsValidPin(pin)) {
            return pin;
        }
        std::wcout << L"PIN must be exactly 4 digits.\n";
    }
}

bool InjectDll(DWORD pid, const std::filesystem::path& dllPath) {
    HANDLE process = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
            PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        pid);
    if (!process) {
        std::wcerr << L"OpenProcess failed: " << GetLastError() << L"\n";
        return false;
    }

    const std::wstring dllWide = dllPath.wstring();
    const size_t bytes = (dllWide.size() + 1) * sizeof(wchar_t);

    void* remoteBuf = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!remoteBuf) {
        std::wcerr << L"VirtualAllocEx failed: " << GetLastError() << L"\n";
        CloseHandle(process);
        return false;
    }

    if (!WriteProcessMemory(process, remoteBuf, dllWide.c_str(), bytes, nullptr)) {
        std::wcerr << L"WriteProcessMemory failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    auto loadLibraryW = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
    if (!loadLibraryW) {
        std::wcerr << L"GetProcAddress(LoadLibraryW) failed\n";
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibraryW, remoteBuf, 0, nullptr);
    if (!thread) {
        std::wcerr << L"CreateRemoteThread failed: " << GetLastError() << L"\n";
        VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
        CloseHandle(process);
        return false;
    }

    WaitForSingleObject(thread, 5000);

    DWORD remoteResult = 0;
    GetExitCodeThread(thread, &remoteResult);
    CloseHandle(thread);
    VirtualFreeEx(process, remoteBuf, 0, MEM_RELEASE);
    CloseHandle(process);

    if (remoteResult == 0) {
        std::wcerr << L"Remote LoadLibraryW failed\n";
        return false;
    }

    return true;
}

bool LaunchGame() {
    const auto gameExePath = GameExePath();
    if (!std::filesystem::exists(gameExePath)) {
        std::wcerr << L"Game executable not found: " << gameExePath << L"\n";
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + gameExePath.wstring() + L"\"";

    BOOL ok = CreateProcessW(
        gameExePath.c_str(),
        cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        gameExePath.parent_path().c_str(),
        &si,
        &pi);
    if (!ok) {
        std::wcerr << L"Failed to launch game: " << GetLastError() << L"\n";
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

struct Options {
    std::filesystem::path dllPath = GetDefaultDllPath();
    std::optional<std::wstring> pin;
};

std::optional<Options> ParseArgs(int argc, wchar_t** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if ((arg == L"--pin" || arg == L"--set-pin") && i + 1 < argc) {
            opts.pin = argv[++i];
            continue;
        }
        if (arg == L"--help" || arg == L"-h") {
            std::wcout << L"Usage: saninet_injector.exe [dll_path] [--pin 1234]\n";
            return std::nullopt;
        }
        opts.dllPath = arg;
    }
    return opts;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    auto parsed = ParseArgs(argc, argv);
    if (!parsed) {
        return 0;
    }
    auto opts = *parsed;

    if (!std::filesystem::exists(opts.dllPath)) {
        std::wcerr << L"DLL not found: " << opts.dllPath << L"\n";
        return 1;
    }

    std::wstring pin;
    if (opts.pin && IsValidPin(*opts.pin)) {
        pin = *opts.pin;
    } else if (auto savedPin = LoadSavedPin()) {
        pin = *savedPin;
        std::wcout << L"Using saved PIN\n";
    } else {
        pin = PromptPin();
    }

    if (!IsValidPin(pin)) {
        std::wcerr << L"PIN must be exactly 4 digits.\n";
        return 1;
    }

    if (!SavePin(pin)) {
        std::wcerr << L"Failed to write PIN file.\n";
        return 1;
    }

    DWORD pid = FindNewestProcessId(L"XSanity.exe");
    if (pid == 0) {
        std::wcout << L"XSanity.exe is not running. Launching it now.\n";
        if (!LaunchGame()) {
            return 1;
        }
        pid = WaitForStableProcessId(L"XSanity.exe", 45000);
        if (pid == 0) {
            std::wcerr << L"Timed out waiting for a stable XSanity.exe process.\n";
            return 1;
        }
    }

    std::wcout << L"Target PID: " << pid << L"\n";
    std::wcout << L"DLL: " << opts.dllPath << L"\n";

    if (!InjectDll(pid, std::filesystem::absolute(opts.dllPath))) {
        return 1;
    }

    std::wcout << L"Injection complete\n";
    return 0;
}
