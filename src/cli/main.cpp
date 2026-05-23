#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string WtoU8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// ─── Pipe ────────────────────────────────────────────────────────────────────

static HANDLE TryOpenPipe() {
    const wchar_t* name = L"\\\\.\\pipe\\TotalControl";
    HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) return h;
    if (GetLastError() == ERROR_PIPE_BUSY) {
        WaitNamedPipeW(name, 2000);
        h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, OPEN_EXISTING, 0, nullptr);
    }
    return h;
}

static bool Talk(HANDLE pipe, const std::string& req, std::string& resp) {
    std::string line = req + "\n";
    DWORD wr;
    if (!WriteFile(pipe, line.c_str(), static_cast<DWORD>(line.size()), &wr, nullptr))
        return false;
    resp.clear();
    char ch; DWORD rd;
    while (ReadFile(pipe, &ch, 1, &rd, nullptr) && rd == 1) {
        if (ch == '\n') break;
        if (ch != '\r') resp += ch;
    }
    return true;
}

// ─── Minimal JSON readers ─────────────────────────────────────────────────────

static std::string JStr(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    auto pos = j.find(k);
    if (pos == std::string::npos) return "";
    pos += k.size();
    auto end = j.find('"', pos);
    return (end != std::string::npos) ? j.substr(pos, end - pos) : "";
}

static std::string JNum(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    auto pos = j.find(k);
    if (pos == std::string::npos) return "";
    pos += k.size();
    auto end = j.find_first_of(",}", pos);
    return (end != std::string::npos) ? j.substr(pos, end - pos) : j.substr(pos);
}

static bool JBool(const std::string& j, const char* key, bool def = false) {
    std::string v = JNum(j, key);
    if (v == "true")  return true;
    if (v == "false") return false;
    return def;
}

// ─── JSON builders ────────────────────────────────────────────────────────────

static std::string Q(const std::string& s) { return "\"" + s + "\""; }

// ─── argv helpers ─────────────────────────────────────────────────────────────

static std::string Arg(const std::vector<std::string>& args, const std::string& flag,
                       const std::string& def = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag) return args[i + 1];
    return def;
}

static bool HasFlag(const std::vector<std::string>& args, const std::string& flag) {
    for (const auto& a : args) if (a == flag) return true;
    return false;
}

// ─── Daemon auto-start ────────────────────────────────────────────────────────

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (auto* s = wcsrchr(buf, L'\\')) *(s + 1) = L'\0';
    return buf;
}

static bool LaunchDaemon() {
    std::wstring dir    = ExeDir();
    std::wstring daemon = dir + L"TotalControlSRV.exe";
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(daemon.c_str(), nullptr, nullptr, nullptr,
                        FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

// ─── Usage ────────────────────────────────────────────────────────────────────

static void Usage() {
    fputs(
        "TotalControlCLI — Sony Camera Remote Controller\n"
        "\n"
        "Usage:\n"
        "  tc status\n"
        "  tc shoot [--ss 1/100] [--iso 100] [--f 2.8] [--mode M|A|S|P]\n"
        "           [--focus MF|AF-S|AF-C|AF-A|DMF] [--store card|pc|both]\n"
        "           [--timeout_ms 5000]\n"
        "  tc bracket [--ev 0.3ev|0.5ev|0.7ev|1ev|1.3ev|1.5ev|1.7ev|2ev|2.3ev|2.5ev|2.7ev|3ev]\n"
        "             [--count 3|5|7|9] [--mode single|cont] [--order minus|zero]\n"
        "             [--ss 1/250] [--iso 200] [--f 2.8] [--store card|pc|both]\n"
        "             [--timeout_ms 15000]\n"
        "  tc movie start|stop|toggle\n"
        "  tc af s1|s2|s1+s2|ael|awb|fel [up|down|press]\n"
        "  tc get <prop>\n"
        "  tc set <prop> <val>\n"
        "  tc cmd <id> [--param 1] [--press]\n"
        "  tc quit\n"
        "\n"
        "gPhoto2-compatible aliases:\n"
        "  tc --trigger-capture            (= shoot)\n"
        "  tc --get-config <prop>          (= get)\n"
        "  tc --set-config <prop>=<val>    (= set)\n"
        "\n"
        "Properties (get/set):\n"
        "  shutter-speed  iso  f-number  ev-comp  exposure-mode  focus-mode\n"
        "  focus-area  drive-mode  white-balance  color-temp  image-size\n"
        "  file-type  metering-mode  store-dest  bracket-order  priority-key\n"
        "  battery  battery-level  remaining  slot1-remaining  slot2-remaining\n"
        "  slot1-status  slot2-status  focus-ind  model\n"
        "\n"
        "Exposure mode values:  M  P  A  S\n"
        "Focus mode values:     MF  AF-S  AF-C  AF-A  DMF\n"
        "File type values:      JPEG  RAW  RAW+JPEG  HEIF  RAW+HEIF\n"
        "White balance values:  AWB  daylight  fluorescent  flash  shadow  cloudy\n"
        "                       tungsten  color-temp  custom-1  custom-2  custom-3\n"
        "Metering values:       multi  center  spot  hl\n"
        "Store dest values:     card  pc  both\n"
        "Image size values:     L  M  S\n"
        "\n"
        "Command IDs (for tc cmd):\n"
        "  release  movie-rec  cancel-shoot  media-format  media-quick-fmt\n"
        "  power-off  power-on  standby  sensor-clean  pixel-mapping\n"
        "  nav-up  nav-down  nav-left  nav-right  nav-set  nav-back  nav-menu\n"
        "  movie-toggle  flicker-scan  tracking-af-on  cam-reset\n",
        stderr
    );
}

// ─── wmain ────────────────────────────────────────────────────────────────────

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) { Usage(); return 1; }

    std::vector<std::string> args;
    args.reserve(argc - 1);
    for (int i = 1; i < argc; ++i)
        args.push_back(WtoU8(argv[i]));

    // ── Build JSON request ────────────────────────────────────────────────────
    std::string req;
    const std::string& cmd = args[0];

    if (cmd == "status") {
        req = R"({"cmd":"status"})";
    }
    else if (cmd == "quit") {
        req = R"({"cmd":"quit"})";
    }
    else if (cmd == "shoot" || cmd == "--trigger-capture") {
        req = "{\"cmd\":\"shoot\"";
        auto ss   = Arg(args, "--ss");     if (!ss.empty())    req += ",\"ss\":"    + Q(ss);
        auto iso  = Arg(args, "--iso");    if (!iso.empty())   req += ",\"iso\":"   + iso;
        auto f    = Arg(args, "--f");      if (!f.empty())     req += ",\"f\":"     + f;
        auto mode = Arg(args, "--mode");   if (!mode.empty())  req += ",\"mode\":"  + Q(mode);
        auto foc  = Arg(args, "--focus");  if (!foc.empty())   req += ",\"focus\":" + Q(foc);
        auto store= Arg(args, "--store");  if (!store.empty()) req += ",\"store\":" + Q(store);
        auto tms  = Arg(args, "--timeout_ms"); if (!tms.empty()) req += ",\"timeout_ms\":" + tms;
        req += "}";
    }
    else if (cmd == "bracket") {
        req = "{\"cmd\":\"bracket\"";
        auto ev    = Arg(args, "--ev",    "1ev"); req += ",\"ev\":"    + Q(ev);
        auto cnt   = Arg(args, "--count", "5");   req += ",\"count\":" + cnt;
        auto mode  = Arg(args, "--mode",  "single"); req += ",\"mode\":" + Q(mode);
        auto order = Arg(args, "--order"); if (!order.empty()) req += ",\"order\":" + Q(order);
        auto ss    = Arg(args, "--ss");    if (!ss.empty())    req += ",\"ss\":"    + Q(ss);
        auto iso   = Arg(args, "--iso");   if (!iso.empty())   req += ",\"iso\":"   + iso;
        auto f     = Arg(args, "--f");     if (!f.empty())     req += ",\"f\":"     + f;
        auto store = Arg(args, "--store"); if (!store.empty()) req += ",\"store\":" + Q(store);
        auto tms   = Arg(args, "--timeout_ms"); if (!tms.empty()) req += ",\"timeout_ms\":" + tms;
        req += "}";
    }
    else if (cmd == "movie" && args.size() >= 2) {
        req = "{\"cmd\":\"movie\",\"action\":" + Q(args[1]) + "}";
    }
    else if (cmd == "af" && args.size() >= 2) {
        req = "{\"cmd\":\"af\",\"button\":" + Q(args[1]);
        if (args.size() >= 3) req += ",\"state\":" + Q(args[2]);
        req += "}";
    }
    else if ((cmd == "get" || cmd == "--get-config") && args.size() >= 2) {
        req = "{\"cmd\":\"get\",\"prop\":" + Q(args[1]) + "}";
    }
    else if (cmd == "set" && args.size() >= 3) {
        req = "{\"cmd\":\"set\",\"prop\":" + Q(args[1]) + ",\"val\":" + Q(args[2]) + "}";
    }
    else if (cmd == "--set-config" && args.size() >= 2) {
        auto& kv = args[1];
        auto eq = kv.find('=');
        if (eq == std::string::npos) { Usage(); return 1; }
        req = "{\"cmd\":\"set\",\"prop\":" + Q(kv.substr(0, eq)) +
              ",\"val\":"                  + Q(kv.substr(eq + 1)) + "}";
    }
    else if (cmd == "cmd" && args.size() >= 2) {
        req = "{\"cmd\":\"cmd\",\"id\":" + Q(args[1]);
        auto p = Arg(args, "--param");
        if (!p.empty()) req += ",\"param\":" + p;
        if (HasFlag(args, "--press")) req += ",\"press\":true";
        req += "}";
    }
    else {
        Usage(); return 1;
    }

    // ── Connect (auto-start daemon if needed) ─────────────────────────────────
    HANDLE pipe = TryOpenPipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND) {
            fprintf(stderr, "tc: pipe error %lu\n", err);
            return 1;
        }
        if (!LaunchDaemon()) {
            fprintf(stderr, "tc: cannot start TotalControl.exe (dir: %s)\n",
                    WtoU8(ExeDir()).c_str());
            return 1;
        }
        fprintf(stderr, "tc: starting daemon");
        for (int i = 0; i < 20 && pipe == INVALID_HANDLE_VALUE; ++i) {
            Sleep(500);
            fputc('.', stderr);
            pipe = TryOpenPipe();
        }
        fputc('\n', stderr);
        if (pipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "tc: daemon did not start in time\n");
            return 1;
        }
    }

    // ── Send / receive ────────────────────────────────────────────────────────
    std::string resp;
    bool ok = Talk(pipe, req, resp);
    CloseHandle(pipe);

    if (!ok) { fprintf(stderr, "tc: pipe I/O error\n"); return 1; }

    // ── Interpret response ────────────────────────────────────────────────────
    if (!JBool(resp, "ok")) {
        std::string e = JStr(resp, "err");
        std::string m = JStr(resp, "msg");
        if (m.empty()) fprintf(stderr, "tc: error: %s\n", e.c_str());
        else           fprintf(stderr, "tc: error: %s - %s\n", e.c_str(), m.c_str());
        return 1;
    }

    if (cmd == "get" || cmd == "--get-config") {
        printf("%s\n", JStr(resp, "val").c_str());
    }
    else if (cmd == "shoot" || cmd == "--trigger-capture") {
        std::string lat = JNum(resp, "latency_ms");
        if (!lat.empty()) printf("latency_ms=%s\n", lat.c_str());
    }
    else if (cmd == "bracket") {
        std::string lat  = JNum(resp, "latency_ms");
        std::string caps = JNum(resp, "captures");
        std::string ev   = JStr(resp, "ev");
        if (!lat.empty())  printf("latency_ms=%s\n", lat.c_str());
        if (!caps.empty()) printf("captures=%s\n", caps.c_str());
        if (!ev.empty())   printf("ev=%s\n", ev.c_str());
    }
    else if (cmd == "status") {
        auto ps = [&](const char* key) {
            auto v = JStr(resp, key);
            if (!v.empty()) printf("%s=%s\n", key, v.c_str());
        };
        auto pn = [&](const char* key) {
            auto v = JNum(resp, key);
            if (!v.empty()) printf("%s=%s\n", key, v.c_str());
        };
        pn("connected");
        ps("model");
        pn("battery");
        ps("battery_level");
        pn("remaining");
        pn("slot2_remaining");
        ps("slot1_status");
        ps("slot2_status");
        ps("ss");
        pn("iso");
        pn("f");
        pn("ev");
        ps("mode");
        ps("focus");
        ps("focus_area");
        ps("focus_ind");
        ps("drive");
        ps("wb");
        pn("color_temp");
        ps("img_size");
        ps("file_type");
        ps("metering");
        ps("store");
    }
    else if (cmd == "af") {
        // no output on success
    }
    else if (cmd == "movie") {
        // no output on success
    }
    else if (cmd == "cmd") {
        // no output on success
    }

    return 0;
}
