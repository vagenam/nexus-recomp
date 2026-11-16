// RecompBuilder.cpp — Native Windows GUI Builder & Launcher for NexusRecomp v0.3
// Zero-dependency static Win32 GUI:
//   - Picks PS3_GAME folder & RPCS3-decrypted EBOOT.ELF
//   - Auto-detects or auto-bootstraps portable Python + CMake + Ninja + Zig (no manual installs!)
//   - Runs build_pipeline.py / CMake build in background thread with live GUI log & progress bar
//   - Writes stock 720p + unlocked FPS settings.txt, creates Desktop shortcut, and launches NexusRecomp.exe
//
// Build:
//   clang++ -O2 -std=c++17 -static -DUNICODE -D_UNICODE -Wl,--subsystem,windows \
//     tools/RecompBuilder.cpp -o dist/RecompBuilder.exe \
//     -lcomctl32 -lole32 -lshell32 -luuid -lgdi32 -luser32 -lbcrypt

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

// ---- Control IDs & Custom Messages -----------------------------------------
enum {
    ID_EDIT_GAME      = 101,
    ID_BTN_GAME       = 102,
    ID_EDIT_ELF       = 103,
    ID_BTN_ELF        = 104,
    ID_CHK_SHORTCUT   = 105,
    ID_BTN_BUILD      = 106,
    ID_BTN_PLAY       = 107,
    ID_PROGRESS       = 108,
    ID_LOG            = 109,
    ID_STATUS_CPU     = 110,
    ID_STATUS_STATE   = 111,
};

enum {
    WM_APP_LOG      = WM_APP + 1,
    WM_APP_PROGRESS = WM_APP + 2,
    WM_APP_DONE     = WM_APP + 3,
};

// ---- Global State ----------------------------------------------------------
static HWND g_hwnd = nullptr;
static HWND g_edit_game = nullptr, g_edit_elf = nullptr;
static HWND g_btn_game = nullptr, g_btn_elf = nullptr;
static HWND g_chk_shortcut = nullptr;
static HWND g_btn_build = nullptr, g_btn_play = nullptr;
static HWND g_progress = nullptr, g_log = nullptr;
static HWND g_lbl_cpu = nullptr, g_lbl_state = nullptr;
static HFONT g_font_ui = nullptr, g_font_title = nullptr, g_font_mono = nullptr;
static HBRUSH g_br_bg = nullptr, g_br_card = nullptr;

static fs::path g_self_dir, g_dist_dir, g_repo_root;
static std::atomic<bool> g_busy{false};
static std::thread g_worker;

// ---- Helpers ---------------------------------------------------------------
static bool check_avx2() {
#if defined(__clang__) || defined(__GNUC__)
    return __builtin_cpu_supports("avx2");
#else
    return true;
#endif
}

static std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

static std::wstring get_win_text(HWND h) {
    int len = GetWindowTextLengthW(h);
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(h, s.data(), len + 1);
    s.resize(len);
    return s;
}

static void post_log(const std::string& msg) {
    if (!g_hwnd) return;
    auto* copy = new std::wstring(utf8_to_wide(msg));
    PostMessageW(g_hwnd, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(copy));
}

static void post_progress(int pct, const std::string& state = "") {
    if (!g_hwnd) return;
    auto* copy = state.empty() ? nullptr : new std::wstring(utf8_to_wide(state));
    PostMessageW(g_hwnd, WM_APP_PROGRESS, (WPARAM)pct, reinterpret_cast<LPARAM>(copy));
}

static std::wstring pick_folder_dialog(HWND owner, const wchar_t* title) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        dlg->SetTitle(title);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

static std::wstring pick_file_dialog(HWND owner, const wchar_t* title) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                   IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        COMDLG_FILTERSPEC filters[] = {
            { L"Decrypted PS3 ELF (*.elf;*.ELF)", L"*.elf;*.ELF" },
            { L"All Files (*.*)", L"*.*" }
        };
        dlg->SetFileTypes(2, filters);
        dlg->SetTitle(title);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

// ---- Input verification ----------------------------------------------------
// Only BCUS99245 v1.00 is supported: the pipeline and the game harness use
// fixed guest addresses from that exact EBOOT. Keep in sync with build_pipeline.py.
static const char* kExpectedTitleId   = "BCUS99245";
static const char* kExpectedElfSha256 = "ca1a1a293d211095fc834eaaa10cd5fa9248b0e0328919fdeadeed1675c52c3f";

static std::string sha256_file_hex(const fs::path& p) {
    std::string result;
    std::ifstream f(p, std::ios::binary);
    if (!f) return result;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return result;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        bool ok = true;
        while (f) {
            f.read(buf.data(), (std::streamsize)buf.size());
            std::streamsize n = f.gcount();
            if (n > 0 && BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), (ULONG)n, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
            static const char* hex = "0123456789abcdef";
            for (unsigned char b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

// Reads TITLE_ID from a PARAM.SFO. Returns empty string on any parse error.
static std::string read_sfo_title_id(const fs::path& sfo) {
    std::ifstream f(sfo, std::ios::binary);
    if (!f) return {};
    std::vector<unsigned char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= d.size() ? (uint32_t)(d[o] | (d[o + 1] << 8)) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= d.size() ? (uint32_t)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24)) : 0; };
    if (d.size() < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') return {};
    uint32_t key_tab = u32(8), data_tab = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + (size_t)i * 16 + 16 <= d.size(); ++i) {
        size_t e = 20 + (size_t)i * 16;
        size_t k = key_tab + u16(e);
        std::string key;
        while (k < d.size() && d[k]) key += (char)d[k++];
        if (key != "TITLE_ID") continue;
        size_t off = data_tab + u32(e + 12), len = u32(e + 4);
        std::string val;
        for (size_t j = 0; j < len && off + j < d.size() && d[off + j]; ++j) val += (char)d[off + j];
        return val;
    }
    return {};
}

static bool allow_unknown_elf() {
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(L"NEXUS_ALLOW_UNKNOWN_ELF", v, 8) > 0 && v[0] == L'1';
}

// A PS3_GAME folder must contain BOTH USRDIR\ and PARAM.SFO (extracted, not .iso/.pkg).
static bool resolve_ps3_game_dir(const fs::path& input, fs::path& resolved) {
    auto is_valid = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_directory(p / "USRDIR", ec) && fs::is_regular_file(p / "PARAM.SFO", ec);
    };
    if (is_valid(input)) { resolved = input; return true; }
    if (is_valid(input / "PS3_GAME")) { resolved = input / "PS3_GAME"; return true; }
    return false;
}

static bool is_valid_elf(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    unsigned char hdr[4] = {};
    f.read(reinterpret_cast<char*>(hdr), 4);
    return f.gcount() == 4 && hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F';
}

static fs::path find_game_in_drop_dir(const fs::path& drop_dir) {
    fs::path resolved;
    if (resolve_ps3_game_dir(drop_dir, resolved)) return resolved;
    std::error_code ec;
    if (fs::exists(drop_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(drop_dir, ec)) {
            if (entry.is_directory(ec) && resolve_ps3_game_dir(entry.path(), resolved)) {
                return resolved;
            }
        }
    }
    return {};
}

static fs::path find_elf_in_drop_dir(const fs::path& drop_dir) {
    if (is_valid_elf(drop_dir / "EBOOT.ELF")) return drop_dir / "EBOOT.ELF";
    if (is_valid_elf(drop_dir / "EBOOT.elf")) return drop_dir / "EBOOT.elf";
    std::error_code ec;
    if (fs::exists(drop_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(drop_dir, ec)) {
            if (entry.is_regular_file(ec) && is_valid_elf(entry.path())) {
                return entry.path();
            }
        }
    }
    return {};
}

static bool update_settings(const fs::path& settings_path, const fs::path& template_path, const std::string& game_path) {
    std::vector<std::string> lines;
    fs::path src = fs::exists(settings_path) ? settings_path : template_path;
    if (fs::exists(src)) {
        std::ifstream in(src);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    bool replaced = false;
    for (auto& l : lines) {
        if (l.rfind("GAME=", 0) == 0) {
            l = "GAME=" + game_path;
            replaced = true;
            break;
        }
    }
    if (!replaced) lines.insert(lines.begin(), "GAME=" + game_path);

    std::ofstream out(settings_path, std::ios::trunc);
    if (!out) return false;
    for (const auto& l : lines) out << l << "\n";
    return true;
}

static std::string read_game_from_settings(const fs::path& settings_path) {
    std::ifstream in(settings_path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("GAME=", 0) == 0) return line.substr(5);
    }
    return {};
}

static bool create_desktop_shortcut(const fs::path& exe_path, const fs::path& work_dir, fs::path& out_lnk) {
    wchar_t desktop_buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop_buf))) return false;
    out_lnk = fs::path(desktop_buf) / L"Ratchet & Clank - Into the Nexus.lnk";

    bool ok = false;
    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**>(&psl)))) {
        psl->SetPath(exe_path.wstring().c_str());
        psl->SetWorkingDirectory(work_dir.wstring().c_str());
        psl->SetDescription(L"Ratchet & Clank: Into the Nexus (PC Recomp)");
        psl->SetIconLocation(exe_path.wstring().c_str(), 0);

        IPersistFile* ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf)))) {
            if (SUCCEEDED(ppf->Save(out_lnk.wstring().c_str(), TRUE))) ok = true;
            ppf->Release();
        }
        psl->Release();
    }
    return ok;
}

// Run command with redirected stdout/stderr streamed live to the GUI log
static int run_cmd_logged(const std::wstring& cmd, const fs::path& cwd) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    std::wstring wcmd = L"cmd.exe /C " + cmd;
    std::wstring wcwd = cwd.wstring();
    BOOL ok = CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, wcwd.c_str(), &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        return -1;
    }

    char buf[1024];
    std::string pending;
    DWORD read = 0;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
        buf[read] = '\0';
        pending += buf;
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) post_log(line);
            pending.erase(0, pos + 1);
        }
    }
    if (!pending.empty()) post_log(pending);
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static void prepend_env_path(const fs::path& dir) {
    if (!fs::exists(dir)) return;
    DWORD len = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring cur(len, L'\0');
    if (len > 0) GetEnvironmentVariableW(L"PATH", cur.data(), len);
    if (!cur.empty() && cur.back() == L'\0') cur.pop_back();
    std::wstring updated = dir.wstring() + L";" + cur;
    SetEnvironmentVariableW(L"PATH", updated.c_str());
}

// Pinned, hash-verified portable toolchain in tools/.portable/:
//   Python 3.12.8 embeddable + pip 26.3 + cmake 4.4.5 + ninja 1.13.2 + ziglang 0.17.1
// Every download is checked against a SHA-256 below, and the pip packages are
// installed with --require-hashes from tools/toolchain-requirements.txt, so a
// zig cc / cmake release months from now cannot silently change the build.
// The Releases zip ships this folder pre-populated; a git clone bootstraps it.
static const wchar_t* kPythonUrl    = L"https://www.python.org/ftp/python/3.12.8/python-3.12.8-embed-amd64.zip";
static const char*    kPythonSha256 = "8d3f33be9eb810f23c102f08475af2854e50484b8e4e06275e937be61ce3d2fb";
static const wchar_t* kPipWheel     = L"pip-26.3-py3-none-any.whl";
static const wchar_t* kPipUrl       = L"https://files.pythonhosted.org/packages/py3/p/pip/pip-26.3-py3-none-any.whl";
static const char*    kPipSha256    = "f3b60b20ffaadd4415ed7957212a38ac63fa4c5fa375bd0f4a797777940fad13";

static bool download_verified(const std::wstring& url, const fs::path& dst, const char* sha256, const fs::path& cwd) {
    if (run_cmd_logged(L"curl.exe -fL --retry 3 \"" + url + L"\" -o \"" + dst.wstring() + L"\"", cwd) != 0) {
        post_log("[ERROR] Download failed: " + wide_to_utf8(url) + " (check internet connection).");
        return false;
    }
    std::string got = sha256_file_hex(dst);
    if (got != sha256) {
        post_log("[ERROR] SHA-256 mismatch for " + wide_to_utf8(url) + ": got " + got + ", expected " + sha256);
        std::error_code ec;
        fs::remove(dst, ec);
        return false;
    }
    post_log("[Toolchain] Verified SHA-256 of " + dst.filename().string());
    return true;
}

static bool ensure_toolchain(const fs::path& repo_root, fs::path& out_python) {
    fs::path pdir = repo_root / "tools" / ".portable";
    fs::path py_exe = pdir / "python.exe";
    fs::path reqs = repo_root / "tools" / "toolchain-requirements.txt";
    fs::path pins_stamp = pdir / ".toolchain-pins";
    fs::create_directories(pdir);

    if (!fs::exists(reqs)) {
        post_log("[ERROR] Missing " + reqs.string());
        return false;
    }
    std::string reqs_hash = sha256_file_hex(reqs);

    if (!fs::exists(py_exe)) {
        post_progress(10, "Downloading portable Python 3.12.8...");
        fs::path py_zip = pdir / "python.zip";
        if (!download_verified(kPythonUrl, py_zip, kPythonSha256, pdir)) return false;
        post_log("[Toolchain] Extracting Python...");
        int rc = run_cmd_logged(L"tar.exe -xf \"" + py_zip.wstring() + L"\" -C \"" + pdir.wstring() + L"\"", pdir);
        std::error_code ec;
        fs::remove(py_zip, ec);
        if (rc != 0 || !fs::exists(py_exe)) return false;

        // Enable site-packages in python312._pth
        fs::path pth = pdir / "python312._pth";
        if (fs::exists(pth)) {
            std::ifstream in(pth);
            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            size_t pos = content.find("#import site");
            if (pos != std::string::npos) content.replace(pos, 12, "import site");
            std::ofstream out(pth, std::ios::trunc);
            out << content << "\nLib\\site-packages\n";
        }
    }

    if (!fs::exists(pdir / "Scripts" / "pip.exe")) {
        post_progress(20, "Bootstrapping pip 26.3...");
        fs::path whl = pdir / kPipWheel;
        if (!download_verified(kPipUrl, whl, kPipSha256, pdir)) return false;
        // Run pip straight from its own verified wheel to install itself (no get-pip.py).
        if (run_cmd_logged(L"\"" + py_exe.wstring() + L"\" \"" + whl.wstring() + L"\\pip\" install --no-index --no-warn-script-location \"" + whl.wstring() + L"\"", pdir) != 0)
            return false;
        std::error_code ec;
        fs::remove(whl, ec);
    }

    std::string have_pins;
    if (fs::exists(pins_stamp)) { std::ifstream in(pins_stamp); std::getline(in, have_pins); }
    if (have_pins != reqs_hash ||
        !fs::exists(pdir / "Scripts" / "ninja.exe") ||
        !fs::exists(pdir / "Lib" / "site-packages" / "cmake" / "data" / "bin" / "cmake.exe") ||
        !fs::exists(pdir / "Lib" / "site-packages" / "ziglang" / "zig.exe")) {
        post_progress(30, "Installing pinned CMake, Ninja & Zig...");
        post_log("[Toolchain] pip install --require-hashes -r tools\\toolchain-requirements.txt");
        if (run_cmd_logged(L"\"" + py_exe.wstring() + L"\" -m pip install --no-warn-script-location --disable-pip-version-check "
                           L"--require-hashes --only-binary=:all: --no-deps -r \"" + reqs.wstring() + L"\"", pdir) != 0)
            return false;
        std::ofstream(pins_stamp, std::ios::trunc) << reqs_hash << "\n";
    }

    prepend_env_path(pdir);
    prepend_env_path(pdir / "Scripts");
    prepend_env_path(pdir / "Lib" / "site-packages" / "cmake" / "data" / "bin");
    prepend_env_path(pdir / "Lib" / "site-packages" / "ziglang");
    out_python = py_exe;
    post_log("[Toolchain] Pinned portable toolchain ready.");
    return true;
}

// ---- Worker Thread ---------------------------------------------------------
static void worker_build(fs::path game_input, fs::path elf_input, bool make_shortcut) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    fs::path resolved_game = find_game_in_drop_dir(game_input);
    if (resolved_game.empty()) {
        post_log("[ERROR] Invalid PS3_GAME folder (needs both USRDIR\\ and PARAM.SFO): " + game_input.string());
        PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        CoUninitialize();
        return;
    }
    {
        std::string tid = read_sfo_title_id(resolved_game / "PARAM.SFO");
        if (tid != kExpectedTitleId) {
            post_log("[ERROR] PARAM.SFO TITLE_ID is '" + tid + "', expected " + kExpectedTitleId +
                     " (Ratchet & Clank: Into the Nexus, USA disc v1.00).");
            PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
            CoUninitialize();
            return;
        }
        post_log("[OK] PARAM.SFO TITLE_ID = " + tid);
    }

    fs::path settings_path = g_dist_dir / "settings.txt";
    fs::path template_path = !g_repo_root.empty() ? (g_repo_root / "settings.template.txt") : (g_dist_dir / "settings.template.txt");
    fs::create_directories(g_dist_dir);
    if (update_settings(settings_path, template_path, resolved_game.string())) {
        post_log("[OK] Updated settings.txt -> GAME=" + resolved_game.string());
    }

    fs::path exe_path = g_dist_dir / "NexusRecomp.exe";
    if (!g_repo_root.empty()) {
        fs::path target_elf = g_repo_root / "elf" / "EBOOT.ELF";

        // Pick the ELF the user selected (or dropped), falling back to an already staged one.
        fs::path src_elf = fs::is_directory(elf_input) ? find_elf_in_drop_dir(elf_input) : elf_input;
        if (src_elf.empty() || !is_valid_elf(src_elf)) src_elf = find_elf_in_drop_dir(g_repo_root / "Put_ELF_Here");
        if (src_elf.empty() && is_valid_elf(resolved_game / "USRDIR" / "EBOOT.elf"))
            src_elf = resolved_game / "USRDIR" / "EBOOT.elf";
        if (src_elf.empty() && is_valid_elf(target_elf))
            src_elf = target_elf;

        if (src_elf.empty() || !is_valid_elf(src_elf)) {
            post_log("[ERROR] Valid decrypted EBOOT.ELF not found. Use RPCS3: Utilities -> Decrypt PS3 Binaries on EBOOT.BIN.");
            PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
            CoUninitialize();
            return;
        }
        post_progress(5, "Verifying EBOOT.ELF (SHA-256)...");
        std::string elf_sha = sha256_file_hex(src_elf);
        if (elf_sha != kExpectedElfSha256) {
            if (!allow_unknown_elf()) {
                post_log("[ERROR] EBOOT.ELF SHA-256 is " + elf_sha);
                post_log("        expected " + std::string(kExpectedElfSha256));
                post_log("        Only the RPCS3-decrypted EBOOT.BIN of BCUS99245 v1.00 (disc, no update) is supported.");
                PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
                CoUninitialize();
                return;
            }
            post_log("[WARN] Unknown EBOOT.ELF SHA-256 (NEXUS_ALLOW_UNKNOWN_ELF=1, unsupported): " + elf_sha);
        } else {
            post_log("[OK] EBOOT.ELF SHA-256 matches BCUS99245 v1.00");
        }
        std::error_code ec_eq;
        if (!fs::equivalent(src_elf, target_elf, ec_eq) &&
            (!fs::exists(target_elf) || sha256_file_hex(target_elf) != elf_sha)) {
            fs::create_directories(target_elf.parent_path());
            fs::copy_file(src_elf, target_elf, fs::copy_options::overwrite_existing);
            post_log("[OK] Staged EBOOT.ELF -> " + target_elf.string());
        }

        // Re-lift unless the existing lift was produced from this exact ELF
        // (build_pipeline.py writes out/lift_stamp.txt and also re-checks the recipe).
        bool need_lift = !fs::exists(g_repo_root / "game" / "src" / "recomp" / "ppu_recomp.h");
        {
            std::ifstream st(g_repo_root / "out" / "lift_stamp.txt");
            std::string first;
            std::getline(st, first);
            if (first != "elf_sha256=" + elf_sha) need_lift = true;
        }

        fs::path py_exe;
        if (!ensure_toolchain(g_repo_root, py_exe)) {
            post_log("[ERROR] Toolchain bootstrap failed.");
            PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
            CoUninitialize();
            return;
        }

        int rc = -1;
        SetEnvironmentVariableW(L"NEXUS_NO_SMOKE", L"1");
        SetEnvironmentVariableW(L"NEXUS_PS3_GAME", resolved_game.wstring().c_str());
        if (need_lift) {
            post_progress(45, "Lifting PPU/SPU & building NexusRecomp.exe (this takes a few minutes)...");
            post_log("[Pipeline] Running build_pipeline.py...");
            rc = run_cmd_logged(L"\"" + py_exe.wstring() + L"\" -u build_pipeline.py", g_repo_root);
        } else if (fs::exists(g_repo_root / "build-win-clang-pgo" / "build.ninja")) {
            post_progress(55, "Building NexusRecomp.exe (Clang PGO)...");
            post_log("[Build] cmake --build build-win-clang-pgo...");
            rc = run_cmd_logged(L"cmake --build build-win-clang-pgo", g_repo_root);
            if (rc == 0 && fs::exists(g_repo_root / "build-win-clang-pgo" / "game" / "NexusRecomp.exe")) {
                fs::copy_file(g_repo_root / "build-win-clang-pgo" / "game" / "NexusRecomp.exe",
                              exe_path, fs::copy_options::overwrite_existing);
            }
        } else {
            post_progress(55, "Building NexusRecomp.exe...");
            post_log("[Build] Running build.bat...");
            rc = run_cmd_logged(L"build.bat", g_repo_root);
            if (rc == 0 && fs::exists(g_repo_root / "build-win" / "game" / "NexusRecomp.exe")) {
                fs::copy_file(g_repo_root / "build-win" / "game" / "NexusRecomp.exe",
                              exe_path, fs::copy_options::overwrite_existing);
            }
        }

        if (rc != 0 || !fs::exists(exe_path)) {
            post_log("[ERROR] Build failed with exit code " + std::to_string(rc));
            PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
            CoUninitialize();
            return;
        }
        post_log("[OK] Your recompiled game is in: " + exe_path.string());
    }

    if (make_shortcut && fs::exists(exe_path)) {
        fs::path lnk;
        if (create_desktop_shortcut(exe_path, g_dist_dir, lnk)) {
            post_log("[OK] Desktop shortcut created: " + lnk.string());
        }
    }

    post_progress(100, "Ready! NexusRecomp.exe is in RecompFolder\\.");
    PostMessageW(g_hwnd, WM_APP_DONE, fs::exists(exe_path) ? 1 : 0, 0);
    CoUninitialize();
}

// ---- Win32 Window Procedure ------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_font_title = CreateFontW(-20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_font_ui    = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_font_mono  = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

        auto make = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
            HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                                     reinterpret_cast<HMENU>((INT_PTR)id), nullptr, nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_ui), TRUE);
            return c;
        };

        HWND lbl_title = make(L"STATIC", L"Project Raccoon: Into the Nexus  —  v0.3", 0, 20, 14, 620, 28);
        SendMessageW(lbl_title, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_title), TRUE);

        std::wstring cpu_txt = check_avx2()
            ? L"CPU Check: AVX2 Supported (Ready)"
            : L"CPU Warning: AVX2 not detected (Haswell / Zen or newer required)";
        g_lbl_cpu = make(L"STATIC", cpu_txt.c_str(), 0, 20, 44, 620, 20, ID_STATUS_CPU);

        make(L"STATIC", L"1. Extracted PS3_GAME Folder (NOT .iso or .pkg — BCUS99245 v1.00):", 0, 20, 74, 520, 20);
        g_edit_game = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 20, 96, 520, 26, ID_EDIT_GAME, WS_EX_CLIENTEDGE);
        g_btn_game  = make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 550, 95, 95, 28, ID_BTN_GAME);

        make(L"STATIC", L"2. Decrypted EBOOT.ELF (RPCS3: Utilities -> Decrypt PS3 Binaries):", 0, 20, 132, 520, 20);
        g_edit_elf = make(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 20, 154, 520, 26, ID_EDIT_ELF, WS_EX_CLIENTEDGE);
        g_btn_elf  = make(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 550, 153, 95, 28, ID_BTN_ELF);

        g_chk_shortcut = make(L"BUTTON", L"Create Desktop shortcut", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 192, 220, 22, ID_CHK_SHORTCUT);
        SendMessageW(g_chk_shortcut, BM_SETCHECK, BST_CHECKED, 0);

        g_btn_build = make(L"BUTTON", L"Build & Setup", BS_DEFPUSHBUTTON | WS_TABSTOP, 360, 188, 140, 32, ID_BTN_BUILD);
        g_btn_play  = make(L"BUTTON", L"Play", BS_PUSHBUTTON | WS_TABSTOP, 510, 188, 135, 32, ID_BTN_PLAY);

        g_lbl_state = make(L"STATIC", L"Select your PS3_GAME folder and decrypted EBOOT.ELF, then click Build & Setup.", 0, 20, 228, 625, 20, ID_STATUS_STATE);
        g_progress  = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                      20, 250, 625, 18, hwnd, reinterpret_cast<HMENU>((INT_PTR)ID_PROGRESS), nullptr, nullptr);
        SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

        g_log = make(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                     20, 278, 625, 195, ID_LOG, WS_EX_CLIENTEDGE);
        SendMessageW(g_log, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_mono), TRUE);

        // Pre-fill existing settings / Put_Game_Folder_Here / Put_ELF_Here / exe state
        std::string saved_game = read_game_from_settings(g_dist_dir / "settings.txt");
        fs::path drop_game = !g_repo_root.empty() ? find_game_in_drop_dir(g_repo_root / "Put_Game_Folder_Here") : fs::path();
        if (!drop_game.empty()) {
            SetWindowTextW(g_edit_game, drop_game.wstring().c_str());
        } else if (!saved_game.empty() && saved_game.find("Put_PS3_Game_Here") == std::string::npos && fs::exists(saved_game)) {
            SetWindowTextW(g_edit_game, utf8_to_wide(saved_game).c_str());
        } else if (!g_repo_root.empty() && fs::exists(g_repo_root / "Put_Game_Folder_Here")) {
            SetWindowTextW(g_edit_game, (g_repo_root / "Put_Game_Folder_Here").wstring().c_str());
        }

        fs::path drop_elf = !g_repo_root.empty() ? find_elf_in_drop_dir(g_repo_root / "Put_ELF_Here") : fs::path();
        if (!drop_elf.empty()) {
            SetWindowTextW(g_edit_elf, drop_elf.wstring().c_str());
        } else if (!g_repo_root.empty() && fs::exists(g_repo_root / "elf" / "EBOOT.ELF")) {
            SetWindowTextW(g_edit_elf, (g_repo_root / "elf" / "EBOOT.ELF").wstring().c_str());
        } else if (!g_repo_root.empty() && fs::exists(g_repo_root / "Put_ELF_Here")) {
            SetWindowTextW(g_edit_elf, (g_repo_root / "Put_ELF_Here").wstring().c_str());
        }
        bool has_exe = fs::exists(g_dist_dir / "NexusRecomp.exe");
        EnableWindow(g_btn_play, has_exe ? TRUE : FALSE);
        if (has_exe) {
            SetWindowTextW(g_lbl_state, L"NexusRecomp.exe is ready. Click Play or rebuild.");
            SendMessageW(g_progress, PBM_SETPOS, 100, 0);
        }
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_BTN_GAME && !g_busy) {
            std::wstring dir = pick_folder_dialog(hwnd, L"Select your dumped PS3_GAME folder (BCUS99245)");
            if (!dir.empty()) {
                fs::path resolved;
                if (resolve_ps3_game_dir(dir, resolved)) dir = resolved.wstring();
                SetWindowTextW(g_edit_game, dir.c_str());
                // Auto-fill EBOOT.elf if present inside USRDIR
                if (get_win_text(g_edit_elf).empty()) {
                    if (is_valid_elf(fs::path(dir) / "USRDIR" / "EBOOT.elf"))
                        SetWindowTextW(g_edit_elf, (fs::path(dir) / "USRDIR" / "EBOOT.elf").wstring().c_str());
                    else if (is_valid_elf(fs::path(dir) / "USRDIR" / "EBOOT.ELF"))
                        SetWindowTextW(g_edit_elf, (fs::path(dir) / "USRDIR" / "EBOOT.ELF").wstring().c_str());
                }
            }
        } else if (id == ID_BTN_ELF && !g_busy) {
            std::wstring f = pick_file_dialog(hwnd, L"Select decrypted EBOOT.ELF (from RPCS3)");
            if (!f.empty()) SetWindowTextW(g_edit_elf, f.c_str());
        } else if (id == ID_BTN_BUILD && !g_busy) {
            std::wstring gpath = get_win_text(g_edit_game);
            std::wstring epath = get_win_text(g_edit_elf);
            if (gpath.empty()) {
                MessageBoxW(hwnd, L"Please select your dumped PS3_GAME folder first.", L"NexusRecomp", MB_ICONWARNING);
                return 0;
            }
            bool mk_sc = SendMessageW(g_chk_shortcut, BM_GETCHECK, 0, 0) == BST_CHECKED;
            g_busy = true;
            EnableWindow(g_btn_build, FALSE);
            EnableWindow(g_btn_play, FALSE);
            SetWindowTextW(g_log, L"");
            SendMessageW(g_progress, PBM_SETPOS, 5, 0);
            if (g_worker.joinable()) g_worker.join();
            g_worker = std::thread(worker_build, fs::path(gpath), fs::path(epath), mk_sc);
        } else if (id == ID_BTN_PLAY && !g_busy) {
            fs::path exe = g_dist_dir / "NexusRecomp.exe";
            if (fs::exists(exe)) {
                ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), nullptr, g_dist_dir.wstring().c_str(), SW_SHOWNORMAL);
            }
        }
        return 0;
    }

    case WM_APP_LOG: {
        auto* s = reinterpret_cast<std::wstring*>(lParam);
        if (s) {
            int len = GetWindowTextLengthW(g_log);
            SendMessageW(g_log, EM_SETSEL, len, len);
            std::wstring line = *s + L"\r\n";
            SendMessageW(g_log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
            delete s;
        }
        return 0;
    }

    case WM_APP_PROGRESS: {
        SendMessageW(g_progress, PBM_SETPOS, wParam, 0);
        auto* s = reinterpret_cast<std::wstring*>(lParam);
        if (s) {
            SetWindowTextW(g_lbl_state, s->c_str());
            delete s;
        }
        return 0;
    }

    case WM_APP_DONE: {
        g_busy = false;
        EnableWindow(g_btn_build, TRUE);
        EnableWindow(g_btn_play, wParam ? TRUE : FALSE);
        if (!wParam) {
            SetWindowTextW(g_lbl_state, L"Setup failed. Check the log below.");
        }
        return 0;
    }

    case WM_DESTROY:
        if (g_worker.joinable()) g_worker.join();
        if (g_font_ui) DeleteObject(g_font_ui);
        if (g_font_title) DeleteObject(g_font_title);
        if (g_font_mono) DeleteObject(g_font_mono);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    wchar_t self_buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self_buf, MAX_PATH);
    g_self_dir = fs::path(self_buf).parent_path();
    if (fs::exists(g_self_dir / "game") && fs::exists(g_self_dir / "build_pipeline.py")) {
        g_repo_root = g_self_dir;
        g_dist_dir = g_self_dir / "RecompFolder";
    } else if (fs::exists(g_self_dir.parent_path() / "game")) {
        g_repo_root = g_self_dir.parent_path();
        g_dist_dir = g_repo_root / "RecompFolder";
    } else {
        g_dist_dir = g_self_dir / "RecompFolder";
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NexusRecompBuilderWnd";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, 665, 492 };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Project Raccoon v0.3 - Setup & Builder",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
