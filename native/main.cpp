#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <d3d11.h>
#include <dxgi.h>
#include <windowsx.h>

#include "model.hpp"
#include "renderer.hpp"
#include "simulation.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>
#include <memory>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr wchar_t MainClass[] = L"ReynoldsBoidsNativeWindow";
constexpr wchar_t KeeperClass[] = L"ReynoldsBoidsKeeperWindow";
constexpr wchar_t SettingsClass[] = L"ReynoldsBoidsNativeSettings";
constexpr wchar_t SettingsContentClass[] = L"ReynoldsBoidsSettingsContent";
constexpr UINT TrayMessage = WM_APP + 10;
constexpr UINT AppUpdateLayout = WM_APP + 11;
constexpr UINT AppUpdateSettings = WM_APP + 12;
constexpr UINT AppQuit = WM_APP + 13;
constexpr UINT_PTR TrayId = 1;
constexpr GUID GuidMonitorPowerOn{0x02731015, 0x4510, 0x4526, {0x99, 0xe6, 0xe5, 0xa1, 0x7e, 0xbd, 0x1a, 0xea}};
constexpr GUID GuidConsoleDisplayState{0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
constexpr GUID GuidSessionDisplayStatus{0x2b84c20e, 0xad23, 0x4ddf, {0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5}};
constexpr int IdTraySettings = 6101;
constexpr int IdTrayPause = 6102;
constexpr int IdTrayPreview = 6103;
constexpr int IdTrayDesktop = 6104;
constexpr int IdTrayQuit = 6105;
constexpr int IdButtonPause = 5001;
constexpr int IdButtonReset = 5002;
constexpr int IdButton60 = 5003;
constexpr int IdButton30 = 5004;
constexpr int IdButtonWallpaper = 5005;
constexpr int IdButtonPreview = 5006;
constexpr int IdButtonExport = 5007;
constexpr int IdButtonQuit = 5008;
constexpr int IdMonitor = 4001;
constexpr int IdColorText = 3001;
constexpr int IdColorPicker = 3002;
constexpr int IdTrails = 2001;
constexpr int IdRandomColor = 2002;
constexpr int IdAvoidTaskbar = 2003;
constexpr int IdStatus = 7001;

struct CommandLine {
    bool help = false;
    bool wallpaper = false;
    bool span = false;
    bool spanSpecified = false;
    bool noSettings = false;
    bool smoke = false;
    bool monitorSpecified = false;
    int monitor = 0;
    unsigned threads = 0;
    std::optional<int> count;
    std::optional<double> seconds;
    std::optional<std::wstring> screenshot;
    std::optional<std::wstring> log;
};

struct MonitorInfo {
    HMONITOR handle = nullptr;
    RECT monitor{};
    RECT work{};
    std::wstring device;
};

enum class NumberField {
    Count, Size, Speed, SpeedError, Separation, Alignment, Cohesion, Edge,
    TrailDuration, Brightness, Contrast, MarginLeft, MarginRight, MarginTop,
    MarginBottom, RenderScale, Fps, TrailHz
};
struct NumberSpec {
    int id;
    NumberField field;
    const wchar_t* label;
    double minimum;
    double maximum;
    double step;
    double displayScale;
    int decimals;
};
constexpr std::array<NumberSpec, 18> NumberSpecs{{
    {1001, NumberField::Count, L"鸟群数量", 0, flock::MaxBirds, 1, 1, 0},
    {1002, NumberField::Size, L"鸟体大小 (1–16 px)", 1, 16, .01, 1, 2},
    {1003, NumberField::Speed, L"飞行速度 (20–240 px/s)", 20, 240, 1, 1, 0},
    {1004, NumberField::SpeedError, L"个体速度差异 (0–50 %)", 0, 50, 1, 100, 0},
    {1005, NumberField::Separation, L"分离权重 (0–4 ×)", 0, 4, .01, 1, 2},
    {1006, NumberField::Alignment, L"对齐权重 (0–4 ×)", 0, 4, .01, 1, 2},
    {1007, NumberField::Cohesion, L"聚合权重 (0–4 ×)", 0, 4, .01, 1, 2},
    {1008, NumberField::Edge, L"边缘回转区 (0–35 %)", 0, 35, .1, 100, 1},
    {1009, NumberField::TrailDuration, L"拖尾时长 (0.1–0.5 s)", .1, .5, .01, 1, 2},
    {1010, NumberField::Brightness, L"亮度 (0.1–2 ×)", .1, 2, .01, 1, 2},
    {1011, NumberField::Contrast, L"对比度 (0.5–2 ×)", .5, 2, .01, 1, 2},
    {1012, NumberField::MarginLeft, L"左边距 (0–200 px)", 0, 200, 1, 1, 0},
    {1013, NumberField::MarginRight, L"右边距 (0–200 px)", 0, 200, 1, 1, 0},
    {1014, NumberField::MarginTop, L"上边距 (0–200 px)", 0, 200, 1, 1, 0},
    {1015, NumberField::MarginBottom, L"下边距 (0–200 px)", 0, 200, 1, 1, 0},
    {1016, NumberField::RenderScale, L"渲染比例 (0.5–1 ×)", .5, 1, .01, 1, 2},
    {1017, NumberField::Fps, L"目标帧率 (15–120 FPS)", 15, 120, 1, 1, 0},
    {1018, NumberField::TrailHz, L"拖尾绘制采样 (10–60 Hz)", 10, 60, 1, 1, 0},
}};
std::wstring NumberLabel(const NumberSpec& spec) {
    if (spec.field == NumberField::Count)
        return L"鸟群数量 (0–" + std::to_wstring(flock::MaxBirds) + L" 只)";
    return spec.label;
}

struct App;
struct SettingsWindow {
    HWND hwnd = nullptr;
    HWND content = nullptr;
    HWND status = nullptr;
    int scrollOffset = 0;
    int contentHeight = 900;
    std::unordered_map<int, HWND> fields;
    std::unordered_map<int, HWND> checks;
};

std::wstring ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"(encoding error)";
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
std::string ToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count, nullptr, nullptr);
    return result;
}
std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else out += static_cast<char>(c);
        }
    }
    return out;
}
std::wstring FormatNumber(double value, int decimals) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}
std::wstring ColorHex(const flock::Color& color) {
    const auto channel = [](float v) { return static_cast<unsigned>(std::clamp(std::lround(v * 255.f), 0l, 255l)); };
    wchar_t buffer[8]{};
    std::swprintf(buffer, std::size(buffer), L"#%02X%02X%02X", channel(color.r), channel(color.g), channel(color.b));
    return buffer;
}
bool ParseColor(const std::wstring& text, flock::Color& color) {
    if (text.size() != 7 || text[0] != L'#') return false;
    auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    int values[6];
    for (int i = 0; i < 6; ++i) if ((values[i] = nibble(text[i + 1])) < 0) return false;
    color.r = static_cast<float>((values[0] * 16 + values[1]) / 255.0);
    color.g = static_cast<float>((values[2] * 16 + values[3]) / 255.0);
    color.b = static_cast<float>((values[4] * 16 + values[5]) / 255.0);
    return true;
}
void ShowError(HWND owner, const std::wstring& title, const std::wstring& message) {
    MessageBoxW(owner, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}
void ShowInfo(HWND owner, const std::wstring& title, const std::wstring& message) {
    MessageBoxW(owner, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}
std::wstring LastErrorText(DWORD code = GetLastError()) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring result = size && buffer ? std::wstring(buffer, size) : L"Windows error " + std::to_wstring(code);
    if (buffer) LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    return result;
}

std::wstring HelpText() {
    return L"ReynoldsBoids.exe [选项]\n\n"
           L"  --help                 输出本说明；无控制台时写到 exe 同目录 ReynoldsBoids-help.txt\n"
           L"  --wallpaper            启动并尝试挂载到桌面图标下方\n"
           L"  --count N              覆盖鸟群数量 (0–" + std::to_wstring(flock::MaxBirds) + L")\n"
           L"  --threads N            模拟线程数 (0 自动硬件并发至多 8，1 强制串行，2–8 显式上限)\n"
           L"  --seconds N            N 秒后干净退出；与 --screenshot 合用时在第 N 秒捕获并退出\n"
           L"  --screenshot PATH      默认预热 3 秒后 GPU backbuffer PNG 捕获并退出\n"
           L"  --log PATH             写入实测帧提交间隔/耗时、模拟步数、adapter 和尺寸 CSV\n"
           L"  --no-settings          不读取或写入 LOCALAPPDATA 配置\n"
           L"  --smoke                自动操作真实窗口并输出步骤记录；不读写个人配置\n"
           L"  --monitor N            选择从 0 开始的显示器序号\n"
           L"  --span                 跨越整个虚拟桌面\n\n"
           L"预览：Space 暂停，F11 全屏/窗口，S 设置，Esc 离开全屏或隐藏预览。壁纸模式的 Esc 不退出。\n"
           L"托盘菜单可打开设置、暂停、预览、桌面挂载或退出。设置位于 %LOCALAPPDATA%\\ReynoldsBoids\\settings.ini。\n"
           L"日志记录应用的实际帧提交间隔与调用耗时，不声称测到了显示器实际呈现帧率。";
}
bool WriteStandardOutput(const std::wstring& text) {
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!output || output == INVALID_HANDLE_VALUE) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        output = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!output || output == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    DWORD consoleMode = 0;
    if (GetConsoleMode(output, &consoleMode))
        return WriteConsoleW(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE;
    const std::string utf8 = ToUtf8(text);
    return WriteFile(output, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) != FALSE;
}
std::filesystem::path ExecutableDirectory() {
    wchar_t executable[MAX_PATH]{};
    const DWORD size = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (!size || size >= std::size(executable)) return {};
    return std::filesystem::path(executable).parent_path();
}
void WriteHelpOutput() {
    const std::wstring text = HelpText() + L"\n";
    if (WriteStandardOutput(text)) return;
    const auto directory = ExecutableDirectory();
    if (directory.empty()) return;
    std::ofstream file(directory / L"ReynoldsBoids-help.txt", std::ios::binary | std::ios::trunc);
    if (!file) return;
    const std::string utf8 = ToUtf8(text);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}
void WriteCommandLineError(const std::wstring& error) {
    const std::wstring text = L"命令行参数错误：\n" + error + L"\n";
    if (WriteStandardOutput(text)) return;
    const auto directory = ExecutableDirectory();
    if (directory.empty()) return;
    std::ofstream file(directory / L"ReynoldsBoids-error.txt", std::ios::binary | std::ios::trunc);
    if (!file) return;
    const std::string utf8 = ToUtf8(text);
    file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
}

bool ParseInt(const std::wstring& text, int& value) {
    wchar_t* end = nullptr;
    const long parsed = wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end || parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) return false;
    value = static_cast<int>(parsed);
    return true;
}
bool ParseDouble(const std::wstring& text, double& value) {
    wchar_t* end = nullptr;
    const double parsed = wcstod(text.c_str(), &end);
    if (end == text.c_str()) return false;
    while (*end == L' ' || *end == L'\t') ++end;
    if (*end || !std::isfinite(parsed)) return false;
    value = parsed;
    return true;
}
bool ParseCommandLine(CommandLine& options, std::wstring& error) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) { error = L"无法解析命令行：" + LastErrorText(); return false; }
    auto needValue = [&](int& i, const wchar_t* name) -> const wchar_t* {
        if (i + 1 >= argc) { error = std::wstring(L"缺少参数：") + name; return nullptr; }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--help" || arg == L"-h" || arg == L"/?") options.help = true;
        else if (arg == L"--wallpaper") options.wallpaper = true;
        else if (arg == L"--span") { options.span = true; options.spanSpecified = true; }
        else if (arg == L"--no-settings") options.noSettings = true;
        else if (arg == L"--smoke") options.smoke = true;
        else if (arg == L"--count") {
            const wchar_t* value = needValue(i, L"--count"); int parsed = 0;
            if (!value || !ParseInt(value, parsed) || parsed < 0 || parsed > flock::MaxBirds) {
                if (error.empty())
                    error = L"--count 必须是 0–" + std::to_wstring(flock::MaxBirds) + L" 的整数。";
                LocalFree(argv);
                return false;
            }
            options.count = parsed;
        } else if (arg == L"--threads") {
            const wchar_t* value = needValue(i, L"--threads"); int parsed = 0;
            if (!value || !ParseInt(value, parsed) || parsed < 0 || parsed > 8) {
                if (error.empty())
                    error = L"--threads 必须是 0–8 的整数 (0 自动，1 串行，2–8 显式指定上限)。";
                LocalFree(argv);
                return false;
            }
            options.threads = static_cast<unsigned>(parsed);
        } else if (arg == L"--monitor") {
            const wchar_t* value = needValue(i, L"--monitor"); int parsed = 0;
            if (!value || !ParseInt(value, parsed) || parsed < 0) { if (error.empty()) error = L"--monitor 必须是非负显示器序号。"; LocalFree(argv); return false; }
            options.monitor = parsed; options.monitorSpecified = true;
        } else if (arg == L"--seconds") {
            const wchar_t* value = needValue(i, L"--seconds"); double parsed = 0;
            if (!value || !ParseDouble(value, parsed) || parsed <= 0) { if (error.empty()) error = L"--seconds 必须是大于 0 的有限数字。"; LocalFree(argv); return false; }
            options.seconds = parsed;
        } else if (arg == L"--screenshot") {
            const wchar_t* value = needValue(i, L"--screenshot"); if (!value) { LocalFree(argv); return false; } options.screenshot = value;
        } else if (arg == L"--log") {
            const wchar_t* value = needValue(i, L"--log"); if (!value) { LocalFree(argv); return false; } options.log = value;
        } else {
            error = L"未知选项：" + arg + L"\n使用 --help 查看支持的选项。";
            LocalFree(argv);
            return false;
        }
    }
    LocalFree(argv);
    return true;
}

BOOL CALLBACK EnumMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM parameter) {
    auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(parameter);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) monitors->push_back({monitor, info.rcMonitor, info.rcWork, info.szDevice});
    return TRUE;
}
DWORD WindowProcessId(HWND window) {
    DWORD processId = 0;
    if (window) GetWindowThreadProcessId(window, &processId);
    return processId;
}
bool IsWindowClass(HWND window, const wchar_t* expectedClass) {
    wchar_t className[64]{};
    return window && GetClassNameW(window, className, static_cast<int>(std::size(className))) > 0 &&
           wcscmp(className, expectedClass) == 0;
}
BOOL CALLBACK FindShellViewProc(HWND top, LPARAM parameter) {
    auto* result = reinterpret_cast<HWND*>(parameter);
    HWND shellView = FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr);
    const DWORD shellProcessId = WindowProcessId(shellView);
    if (!shellView || !shellProcessId) return TRUE;

    HWND childAfter = shellView;
    while (HWND worker = FindWindowExW(top, childAfter, L"WorkerW", nullptr)) {
        if (WindowProcessId(worker) == shellProcessId) {
            *result = worker;
            return FALSE;
        }
        childAfter = worker;
    }

    for (HWND sibling = GetWindow(top, GW_HWNDNEXT); sibling; sibling = GetWindow(sibling, GW_HWNDNEXT)) {
        if (IsWindowClass(sibling, L"WorkerW") && WindowProcessId(sibling) == shellProcessId) {
            *result = sibling;
            return FALSE;
        }
    }
    return TRUE;
}
HWND FindDesktopWorker() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &ignored);
    }
    HWND worker = nullptr;
    EnumWindows(FindShellViewProc, reinterpret_cast<LPARAM>(&worker));
    return worker;
}
bool IsExplorerOrShell(HWND window) {
    if (!window) return true;
    wchar_t className[96]{};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    return wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0 ||
           wcscmp(className, L"Shell_TrayWnd") == 0 || wcscmp(className, L"Shell_SecondaryTrayWnd") == 0;
}
bool IsLeftMouseButtonDown() {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}
template <size_t N>
bool ReadDesktopObjectName(HDESK desktop, wchar_t (&name)[N]) {
    if (!desktop) return false;
    DWORD copied = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, name, static_cast<DWORD>(sizeof(name)), &copied)) return false;
    name[N - 1] = L'\0';
    return name[0] != L'\0';
}
bool IsDrawingOnInputDesktop() {
    wchar_t drawingName[260]{};
    if (!ReadDesktopObjectName(GetThreadDesktop(GetCurrentThreadId()), drawingName)) return false;
    const HDESK inputDesktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!inputDesktop) return false;
    wchar_t inputName[260]{};
    const bool gotInputName = ReadDesktopObjectName(inputDesktop, inputName);
    CloseDesktop(inputDesktop);
    return gotInputName &&
        CompareStringOrdinal(drawingName, -1, inputName, -1, TRUE) == CSTR_EQUAL;
}
bool ForegroundWindowCoversTarget(HWND ownWindow, const RECT& target, const std::vector<MonitorInfo>& monitors) {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    HWND root = GetAncestor(foreground, GA_ROOT);
    if (!root || root == GetAncestor(ownWindow, GA_ROOT) || IsExplorerOrShell(root) || !IsWindowVisible(root) || IsIconic(root)) return false;
    RECT windowRect{};
    if (!GetWindowRect(root, &windowRect)) return false;
    bool sawTargetMonitor = false;
    for (const MonitorInfo& monitor : monitors) {
        RECT intersection{};
        if (!IntersectRect(&intersection, &target, &monitor.monitor)) continue;
        sawTargetMonitor = true;
        if (windowRect.left > monitor.monitor.left || windowRect.top > monitor.monitor.top ||
            windowRect.right < monitor.monitor.right || windowRect.bottom < monitor.monitor.bottom) return false;
    }
    return sawTargetMonitor;
}

class App {
public:
    explicit App(CommandLine options) : options_(std::move(options)), simulation_(20261022, options_.threads) {}
    int run();
    LRESULT windowMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT keeperMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT settingsMessage(HWND, UINT, WPARAM, LPARAM);
    void ShowSettings();
    void HideSettings();
    void SetPaused(bool paused);
    void ResetSimulation();
    void ChangeFps(int fps);
    void SetDesktop(bool desktop, bool notify = true);
    void SetPreview(bool fullScreen, bool save = true);
    void UpdateLayout(bool rebuildMonitors = false);
    void ConfigureSimulation();
    void SaveConfiguration(bool showFailure = true);
    void ExportDiagnostics();
    void RequestQuit();
    bool OpenLog();
    void LogEvent(const std::string& event);
    bool RenderFrame(Clock::time_point now);
    bool IsRenderable() const;
    void AdvanceFrameDeadline(Clock::time_point now);
    void ScheduleNextFrame(Clock::time_point now);
    void RunSmokeStep(Clock::time_point now);
    bool BeginDesktopMount(bool notifyFailure);
    void ReturnToPreview(bool fullScreen, bool notifyFailure = false);
    void UpdateTray();
    void UpdateSettingsStatus();
    void UpdateCoveragePause();
    bool CreateKeeperWindow();
    bool CreateRenderWindow();
    bool InitializeRenderer();
    void HandleShellRestart();
private:
    CommandLine options_;
    HWND hwnd_ = nullptr;
    HANDLE frameTimer_ = nullptr;
    HWND keeper_ = nullptr;
    HWND desktopParent_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    bool rendererReady_ = false;
    bool running_ = true;
    bool paused_ = false;
    bool desktopMode_ = false;
    bool previewFullScreen_ = true;
    bool minimized_ = false;
    bool suspended_ = false;
    bool sessionLocked_ = false;
    bool displayOff_ = false;
    bool coveragePaused_ = false;
    bool wtsRegistered_ = false;
    bool inputDesktopActive_ = false;
    bool mouseTracking_ = false;
    bool monitorDisplayOff_ = false;
    bool consoleDisplayOff_ = false;
    bool sessionDisplayOff_ = false;
    bool pointerActive_ = false;
    bool clickPending_ = false;
    bool desktopLeftButtonDown_ = false;
    bool trayAdded_ = false;
    bool noSave_ = false;
    bool configFailureReported_ = false;
    bool screenshotComplete_ = false;
    bool screenshotDeadlineReached_ = false;
    bool screenshotFailed_ = false;
    bool smokeFailed_ = false;
    bool runtimeFailed_ = false;
    bool wasRenderable_ = false;
    bool renderDirty_ = true;
    bool quitting_ = false;
    bool savedTopLevelStyles_ = false;
    bool hadDesktopParent_ = false;
    DWORD_PTR originalStyle_ = 0;
    DWORD_PTR originalExStyle_ = 0;
    HPOWERNOTIFY monitorPowerNotify_ = nullptr;
    HPOWERNOTIFY sessionDisplayNotify_ = nullptr;
    HPOWERNOTIFY consoleDisplayNotify_ = nullptr;
    std::string autoPauseReason_;
    float logicalWidth_ = 1;
    float logicalHeight_ = 1;
    POINT pointer_{};
    flock::Settings settings_{};
    flock::Simulation simulation_;
    std::unique_ptr<flock::Renderer> renderer_;
    SettingsWindow settingsWindow_{};
    std::vector<MonitorInfo> monitors_;
    std::vector<flock::Bounds> taskbarExclusions_;
    std::ofstream log_;
    int selectedMonitor_ = 0;
    int lastWidth_ = 0, lastHeight_ = 0, cachedDpi_ = 96;
    bool span_ = false;
    std::filesystem::path configPath_;
    Clock::time_point startedAt_{};
    Clock::time_point lastFrameAt_{};
    Clock::time_point lastSubmitAt_{};
    Clock::time_point nextFrameAt_{};
    Clock::time_point nextStatusAt_{};
    Clock::time_point nextSmokeAt_{};
    Clock::time_point nextCoverageCheck_{};
    Clock::time_point nextInputDesktopCheck_{};
    Clock::time_point shutdownAt_{};
    bool shutdownScheduled_ = false;
    double accumulator_ = 0;
    double frameTotalMs_ = 0;
    double lastFrameMs_ = 0;
    double lastFrameIntervalMs_ = 0;
    double lastDrawMs_ = 0;
    double lastSimulationMs_ = 0;
    std::uint64_t simulationSteps_ = 0;
    std::uint64_t renderedFrames_ = 0;
    std::uint64_t droppedSimulationSteps_ = 0;
    std::uint64_t droppedFrameDeadlines_ = 0;
    std::uint64_t pendingSteps_ = 0;
    std::vector<std::string> smokeSteps_;
    std::filesystem::path smokeDirectory_;
    enum class SmokeStage { Pause, Resume, CountZero, CountRestore, Resize, Desktop, Preview, Capture, Finish };
    SmokeStage smokeStage_ = SmokeStage::Pause;
    flock::Settings preSmokeSettings_{};
    bool preSmokeDesktop_ = false;
    bool preSmokeFullscreen_ = true;
    RECT preSmokeRect_{};

    std::filesystem::path ResolveConfigPath();
    void LoadConfiguration();
    void WriteSettingsFile(const std::filesystem::path& path);
    void ReadSettingLine(const std::string& key, const std::string& value, bool& malformed);
    std::vector<MonitorInfo> EnumerateMonitors() const;
    RECT TargetRectangle() const;
    RECT EffectiveWorkRectangle() const;
    float DpiScale() const;
    void UpdateClientDimensions();
    void PlacePreviewWindow(bool fullScreen);
    void DetachDesktop();
    bool ReattachDesktop();
    void UpdateMonitorList();
    void CreateTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    void CreateSettingsWindow();
    void RefreshSettingsControls();
    void CommitNumber(int id);
    void CommitColor();
    void SetCheckbox(int id, bool checked);
    void SaveOnChange();
    void PersistModeSettings();
    void HandleSmokeDeadline(Clock::time_point now);
    void FinishSmoke();
    void HandleFatalGraphicsError(const std::string& error);
    std::wstring MonitorLabel(size_t index) const;
    flock::Input CurrentInput() const;
    void UpdateAutomaticPause();
    void UpdateInputDesktopState();
    void UpdateDesktopInput();
    void ResetDesktopInputEdge();
};

LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->windowMessage(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}
LRESULT CALLBACK KeeperWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->keeperMessage(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}
LRESULT CALLBACK SettingsWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->settingsMessage(hwnd, message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}
LRESULT CALLBACK SettingsContentProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_COMMAND || message == WM_MOUSEWHEEL) {
        return SendMessageW(GetParent(hwnd), message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}
bool App::CreateKeeperWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = KeeperWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = KeeperClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    keeper_ = CreateWindowExW(WS_EX_TOOLWINDOW, KeeperClass, L"", WS_POPUP, 0, 0, 1, 1,
                              nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!keeper_) return false;
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");
    return taskbarCreatedMessage_ != 0;
}
bool App::CreateRenderWindow() {
    if (hwnd_ && IsWindow(hwnd_)) return true;
    const RECT initial = TargetRectangle();
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, MainClass, L"Reynolds Boids",
        WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, initial.left, initial.top,
        initial.right - initial.left, initial.bottom - initial.top, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;
    minimized_ = false;
    rendererReady_ = false;
    return true;
}
bool App::InitializeRenderer() {
    if (!hwnd_ || !IsWindow(hwnd_)) return false;
    UpdateClientDimensions();
    renderer_ = std::make_unique<flock::Renderer>();
    std::string error;
    if (!renderer_->initialize(hwnd_, lastWidth_, lastHeight_, error)) {
        renderer_.reset();
        rendererReady_ = false;
        ShowError(hwnd_, L"Direct3D 初始化失败", L"GPU 返回的错误信息：\n" + ToWide(error));
        return false;
    }
    rendererReady_ = true;
    UpdateClientDimensions();
    ConfigureSimulation();
    return true;
}
void App::HandleShellRestart() {
    RemoveTrayIcon();
    CreateTrayIcon();
    if (desktopMode_) {
        if (!hwnd_ || !IsWindow(hwnd_)) {
            rendererReady_ = false;
            renderer_.reset();
            if (!CreateRenderWindow()) {
                ShowError(keeper_, L"桌面窗口恢复失败", L"Explorer 已重启，但无法重新创建渲染窗口：\n" + LastErrorText());
                runtimeFailed_ = true;
                RequestQuit();
                return;
            }
        }
        if (!ReattachDesktop()) {
            desktopMode_ = false;
            PlacePreviewWindow(true);
            ShowError(hwnd_, L"桌面重新挂载失败", L"Explorer 已重启，但未能重新挂载 WorkerW，应用已退回预览模式。可稍后从托盘重试。");
        }
        if (!rendererReady_ && !InitializeRenderer()) { runtimeFailed_ = true; RequestQuit(); return; }
    }
    UpdateLayout(true);
    nextCoverageCheck_ = Clock::now();
    LogEvent("explorer_restarted desktop=" + std::string(desktopMode_ ? "reattached" : "preview"));
}
LRESULT App::keeperMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreatedMessage_ && message == taskbarCreatedMessage_) {
        HandleShellRestart();
        return 0;
    }
    if (message == TrayMessage) {
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu();
        else if (lParam == WM_LBUTTONUP || lParam == NIN_SELECT || lParam == NIN_KEYSELECT) ShowSettings();
        return 0;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IdTraySettings: ShowSettings(); return 0;
        case IdTrayPause: SetPaused(!paused_); return 0;
        case IdTrayPreview: SetPreview(true); return 0;
        case IdTrayDesktop: SetDesktop(true); return 0;
        case IdTrayQuit: RequestQuit(); return 0;
        default: break;
        }
    }
    if (message == WM_DISPLAYCHANGE) {
        UpdateLayout(true);
        return 0;
    }
    if (message == WM_SETTINGCHANGE && wParam == SPI_SETWORKAREA) {
        UpdateLayout(true);
        return 0;
    }
    if (message == WM_POWERBROADCAST || message == WM_WTSSESSION_CHANGE)
        return windowMessage(hwnd_, message, wParam, lParam);
    if (message == WM_DESTROY) {
        if (!quitting_) running_ = false;
        keeper_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}


HWND MakeControl(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style,
                 int x, int y, int width, int height, int id) {
    HWND control = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
}

void App::CreateSettingsWindow() {
    if (settingsWindow_.hwnd && IsWindow(settingsWindow_.hwnd)) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = SettingsWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = SettingsClass;
    RegisterClassW(&wc);
    WNDCLASSW contentClass{};
    contentClass.lpfnWndProc = SettingsContentProc;
    contentClass.hInstance = GetModuleHandleW(nullptr);
    contentClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    contentClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    contentClass.lpszClassName = SettingsContentClass;
    RegisterClassW(&contentClass);
    settingsWindow_.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, SettingsClass, L"Reynolds Boids — 设置与诊断",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 755, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!settingsWindow_.hwnd) ShowError(hwnd_, L"设置窗口创建失败", LastErrorText());
}

void App::ShowSettings() {
    CreateSettingsWindow();
    if (!settingsWindow_.hwnd) return;
    RefreshSettingsControls();
    ShowWindow(settingsWindow_.hwnd, SW_SHOW);
    SetWindowPos(settingsWindow_.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(settingsWindow_.hwnd);
}
void App::HideSettings() {
    if (settingsWindow_.hwnd) ShowWindow(settingsWindow_.hwnd, SW_HIDE);
}

void App::RefreshSettingsControls() {
    if (!settingsWindow_.hwnd || !IsWindow(settingsWindow_.hwnd)) return;
    for (const NumberSpec& spec : NumberSpecs) {
        auto found = settingsWindow_.fields.find(spec.id);
        if (found == settingsWindow_.fields.end()) continue;
        double value = 0;
        switch (spec.field) {
        case NumberField::Count: value = settings_.count; break;
        case NumberField::Size: value = settings_.size; break;
        case NumberField::Speed: value = settings_.speed; break;
        case NumberField::SpeedError: value = settings_.speedError; break;
        case NumberField::Separation: value = settings_.separation; break;
        case NumberField::Alignment: value = settings_.alignment; break;
        case NumberField::Cohesion: value = settings_.cohesion; break;
        case NumberField::Edge: value = settings_.edge; break;
        case NumberField::TrailDuration: value = settings_.trailDuration; break;
        case NumberField::Brightness: value = settings_.brightness; break;
        case NumberField::Contrast: value = settings_.contrast; break;
        case NumberField::MarginLeft: value = settings_.marginLeft; break;
        case NumberField::MarginRight: value = settings_.marginRight; break;
        case NumberField::MarginTop: value = settings_.marginTop; break;
        case NumberField::MarginBottom: value = settings_.marginBottom; break;
        case NumberField::RenderScale: value = settings_.renderScale; break;
        case NumberField::Fps: value = settings_.fps; break;
        case NumberField::TrailHz: value = settings_.trailHz; break;
        }
        SetWindowTextW(found->second, FormatNumber(value * spec.displayScale, spec.decimals).c_str());
    }
    if (auto found = settingsWindow_.fields.find(IdColorText); found != settingsWindow_.fields.end())
        SetWindowTextW(found->second, ColorHex(settings_.color).c_str());
    if (auto found = settingsWindow_.checks.find(IdTrails); found != settingsWindow_.checks.end())
        SendMessageW(found->second, BM_SETCHECK, settings_.trails ? BST_CHECKED : BST_UNCHECKED, 0);
    if (auto found = settingsWindow_.checks.find(IdRandomColor); found != settingsWindow_.checks.end())
        SendMessageW(found->second, BM_SETCHECK, settings_.randomColor ? BST_CHECKED : BST_UNCHECKED, 0);
    if (auto found = settingsWindow_.checks.find(IdAvoidTaskbar); found != settingsWindow_.checks.end())
        SendMessageW(found->second, BM_SETCHECK, settings_.avoidTaskbar ? BST_CHECKED : BST_UNCHECKED, 0);
    HWND combo = GetDlgItem(settingsWindow_.content, IdMonitor);
    if (combo) {
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"跨虚拟桌面"));
        for (size_t i = 0; i < monitors_.size(); ++i) {
            const std::wstring label = MonitorLabel(i);
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(combo, CB_SETCURSEL, span_ ? 0 : static_cast<WPARAM>(std::min(selectedMonitor_, static_cast<int>(monitors_.size() ? monitors_.size() - 1 : 0)) + 1), 0);
    }
    if (auto found = settingsWindow_.fields.find(IdButtonPause); found != settingsWindow_.fields.end())
        SetWindowTextW(found->second, paused_ ? L"继续" : L"暂停");
    UpdateSettingsStatus();
}

std::wstring App::MonitorLabel(size_t index) const {
    if (index >= monitors_.size()) return L"显示器不可用";
    const MonitorInfo& monitor = monitors_[index];
    return L"显示器 " + std::to_wstring(index) + L" — " + monitor.device + L" (" +
           std::to_wstring(monitor.monitor.right - monitor.monitor.left) + L" × " +
           std::to_wstring(monitor.monitor.bottom - monitor.monitor.top) + L")";
}

LRESULT App::settingsMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        settingsWindow_.hwnd = hwnd;
        settingsWindow_.content = CreateWindowExW(0, SettingsContentClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, 660, 900, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
        HWND panel = settingsWindow_.content;
        MakeControl(panel, L"BUTTON", L"鸟群与外观", BS_GROUPBOX, 10, 8, 640, 286, 0);
        MakeControl(panel, L"BUTTON", L"拖尾", BS_GROUPBOX, 10, 300, 640, 100, 0);
        MakeControl(panel, L"BUTTON", L"屏幕与渲染", BS_GROUPBOX, 10, 406, 640, 230, 0);
        MakeControl(panel, L"BUTTON", L"控制与导出", BS_GROUPBOX, 10, 642, 640, 202, 0);
        const int leftLabelX = 24, leftEditX = 178, rightLabelX = 328, rightEditX = 492;
        const int labelW = 150, editW = 100;
        const std::array<int, 5> rows{36, 70, 104, 138, 172};
        const std::array<std::array<size_t, 2>, 5> appearanceFields{{{{0, 1}}, {{2, 3}}, {{4, 5}}, {{6, 7}}, {{9, 10}}}};
        for (size_t i = 0; i < appearanceFields.size(); ++i) {
            const NumberSpec& a = NumberSpecs[appearanceFields[i][0]];
            const NumberSpec& b = NumberSpecs[appearanceFields[i][1]];
            const std::wstring aLabel = NumberLabel(a);
            MakeControl(panel, L"STATIC", aLabel.c_str(), 0, leftLabelX, rows[i], labelW, 22, 0);
            settingsWindow_.fields[a.id] = MakeControl(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, leftEditX, rows[i] - 2, editW, 24, a.id);
            const std::wstring bLabel = NumberLabel(b);
            MakeControl(panel, L"STATIC", bLabel.c_str(), 0, rightLabelX, rows[i], labelW, 22, 0);
            settingsWindow_.fields[b.id] = MakeControl(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, rightEditX, rows[i] - 2, editW, 24, b.id);
        }
        MakeControl(panel, L"STATIC", L"鸟群颜色 (#RRGGBB)", 0, leftLabelX, 206, 145, 22, 0);
        settingsWindow_.fields[IdColorText] = MakeControl(panel, L"EDIT", L"#84D8E0", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, leftEditX, 204, 112, 24, IdColorText);
        MakeControl(panel, L"BUTTON", L"选择颜色…", BS_PUSHBUTTON | WS_TABSTOP, 300, 204, 100, 25, IdColorPicker);
        settingsWindow_.checks[IdRandomColor] = MakeControl(panel, L"BUTTON", L"启用鸟群渐变配色", BS_AUTOCHECKBOX | WS_TABSTOP, 418, 204, 210, 25, IdRandomColor);

        settingsWindow_.checks[IdTrails] = MakeControl(panel, L"BUTTON", L"显示拖尾", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 326, 140, 24, IdTrails);
        const std::wstring trailDurationLabel = NumberLabel(NumberSpecs[8]);
        MakeControl(panel, L"STATIC", trailDurationLabel.c_str(), 0, 178, 326, 228, 22, 0);
        settingsWindow_.fields[NumberSpecs[8].id] = MakeControl(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 400, 324, 84, 24, NumberSpecs[8].id);
        const std::wstring trailHzLabel = NumberLabel(NumberSpecs[17]);
        MakeControl(panel, L"STATIC", trailHzLabel.c_str(), 0, 328, 358, 160, 22, 0);
        settingsWindow_.fields[NumberSpecs[17].id] = MakeControl(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 492, 356, 100, 24, NumberSpecs[17].id);

        MakeControl(panel, L"STATIC", L"显示器选择", 0, 24, 438, 120, 22, 0);
        MakeControl(panel, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 150, 434, 460, 240, IdMonitor);
        settingsWindow_.checks[IdAvoidTaskbar] = MakeControl(panel, L"BUTTON", L"避开任务栏工作区", BS_AUTOCHECKBOX | WS_TABSTOP, 24, 470, 210, 24, IdAvoidTaskbar);
        const std::array<int, 2> marginRows{504, 538};
        const std::array<size_t, 4> marginFields{11, 12, 13, 14};
        for (size_t i = 0; i < 2; ++i) {
            const NumberSpec& a = NumberSpecs[marginFields[i * 2]];
            const NumberSpec& b = NumberSpecs[marginFields[i * 2 + 1]];
            const std::wstring aLabel = NumberLabel(a);
            MakeControl(panel, L"STATIC", aLabel.c_str(), 0, leftLabelX, marginRows[i], labelW, 22, 0);
            settingsWindow_.fields[a.id] = MakeControl(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, leftEditX, marginRows[i] - 2, editW, 24, a.id);
            const std::wstring bLabel = NumberLabel(b);
            MakeControl(panel, L"STATIC", bLabel.c_str(), 0, rightLabelX, marginRows[i], labelW, 22, 0);
            settingsWindow_.fields[b.id] = MakeControl(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, rightEditX, marginRows[i] - 2, editW, 24, b.id);
        }
        const std::wstring renderScaleLabel = NumberLabel(NumberSpecs[15]);
        MakeControl(panel, L"STATIC", renderScaleLabel.c_str(), 0, leftLabelX, 572, labelW, 22, 0);
        settingsWindow_.fields[NumberSpecs[15].id] = MakeControl(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, leftEditX, 570, editW, 24, NumberSpecs[15].id);
        const std::wstring fpsLabel = NumberLabel(NumberSpecs[16]);
        MakeControl(panel, L"STATIC", fpsLabel.c_str(), 0, rightLabelX, 572, labelW, 22, 0);
        settingsWindow_.fields[NumberSpecs[16].id] = MakeControl(panel, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, rightEditX, 570, editW, 24, NumberSpecs[16].id);


        settingsWindow_.fields[IdButtonPause] = MakeControl(panel, L"BUTTON", L"暂停", BS_PUSHBUTTON | WS_TABSTOP, 24, 672, 132, 29, IdButtonPause);
        MakeControl(panel, L"BUTTON", L"重置鸟群", BS_PUSHBUTTON | WS_TABSTOP, 166, 672, 132, 29, IdButtonReset);
        MakeControl(panel, L"BUTTON", L"标准 60 FPS", BS_PUSHBUTTON | WS_TABSTOP, 308, 672, 132, 29, IdButton60);
        MakeControl(panel, L"BUTTON", L"节能 30 FPS", BS_PUSHBUTTON | WS_TABSTOP, 450, 672, 132, 29, IdButton30);
        MakeControl(panel, L"BUTTON", L"挂载桌面", BS_PUSHBUTTON | WS_TABSTOP, 24, 710, 132, 29, IdButtonWallpaper);
        MakeControl(panel, L"BUTTON", L"打开预览", BS_PUSHBUTTON | WS_TABSTOP, 166, 710, 132, 29, IdButtonPreview);
        MakeControl(panel, L"BUTTON", L"导出诊断 JSON…", BS_PUSHBUTTON | WS_TABSTOP, 308, 710, 180, 29, IdButtonExport);
        MakeControl(panel, L"BUTTON", L"退出", BS_PUSHBUTTON | WS_TABSTOP, 496, 710, 86, 29, IdButtonQuit);
        settingsWindow_.status = MakeControl(panel, L"STATIC", L"", SS_LEFT, 24, 748, 600, 76, IdStatus);
        settingsWindow_.contentHeight = 850;
        RefreshSettingsControls();
        return 0;
    }
    case WM_SIZE: {
        if (settingsWindow_.content) {
            RECT client{}; GetClientRect(hwnd, &client);
            const int width = std::max(620, static_cast<int>(client.right - GetSystemMetrics(SM_CXVSCROLL)));
            const int height = settingsWindow_.contentHeight;
            SetWindowPos(settingsWindow_.content, nullptr, 0, -settingsWindow_.scrollOffset, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
            info.nMin = 0; info.nMax = height; info.nPage = static_cast<UINT>(std::max(1, static_cast<int>(client.bottom))); info.nPos = settingsWindow_.scrollOffset;
            SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
        }
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(hwnd, SB_VERT, &info);
        int position = settingsWindow_.scrollOffset;
        switch (LOWORD(wParam)) {
        case SB_LINEUP: position -= 28; break;
        case SB_LINEDOWN: position += 28; break;
        case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
        case SB_THUMBPOSITION: case SB_THUMBTRACK: position = info.nTrackPos; break;
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = settingsWindow_.contentHeight; break;
        default: return 0;
        }
        RECT client{}; GetClientRect(hwnd, &client);
        settingsWindow_.scrollOffset = std::clamp(position, 0, std::max(0, settingsWindow_.contentHeight - static_cast<int>(client.bottom)));
        SetWindowPos(settingsWindow_.content, nullptr, 0, -settingsWindow_.scrollOffset, 660, settingsWindow_.contentHeight,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        info.fMask = SIF_POS; info.nPos = settingsWindow_.scrollOffset; SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int lines = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
        for (int i = 0; i < std::max(1, std::abs(lines)); ++i)
            SendMessageW(hwnd, WM_VSCROLL, MAKEWPARAM(lines > 0 ? SB_LINEUP : SB_LINEDOWN, 0), 0);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam), code = HIWORD(wParam);
        if (code == EN_KILLFOCUS && settingsWindow_.fields.contains(id)) {
            if (id == IdColorText) CommitColor(); else CommitNumber(id);
            return 0;
        }
        if (code == BN_CLICKED) {
            if (id == IdTrails || id == IdRandomColor || id == IdAvoidTaskbar) {
                const bool checked = SendMessageW(reinterpret_cast<HWND>(lParam), BM_GETCHECK, 0, 0) == BST_CHECKED;
                SetCheckbox(id, checked); return 0;
            }
            switch (id) {
            case IdColorPicker: {
                COLORREF custom[16]{};
                CHOOSECOLORW choose{}; choose.lStructSize = sizeof(choose); choose.hwndOwner = hwnd;
                choose.rgbResult = RGB(static_cast<int>(settings_.color.r * 255), static_cast<int>(settings_.color.g * 255), static_cast<int>(settings_.color.b * 255));
                choose.lpCustColors = custom; choose.Flags = CC_FULLOPEN | CC_RGBINIT;
                if (ChooseColorW(&choose)) {
                    settings_.color.r = GetRValue(choose.rgbResult) / 255.f;
                    settings_.color.g = GetGValue(choose.rgbResult) / 255.f;
                    settings_.color.b = GetBValue(choose.rgbResult) / 255.f;
                    RefreshSettingsControls(); ConfigureSimulation(); SaveOnChange();
                }
                return 0;
            }
            case IdButtonPause: SetPaused(!paused_); return 0;
            case IdButtonReset: ResetSimulation(); return 0;
            case IdButton60: ChangeFps(60); return 0;
            case IdButton30: ChangeFps(30); return 0;
            case IdButtonWallpaper: SetDesktop(true); return 0;
            case IdButtonPreview: SetPreview(true); return 0;
            case IdButtonExport: ExportDiagnostics(); return 0;
            case IdButtonQuit: RequestQuit(); return 0;
            default: break;
            }
        }
        if (id == IdMonitor && code == CBN_SELCHANGE) {
            const int selected = static_cast<int>(SendMessageW(reinterpret_cast<HWND>(lParam), CB_GETCURSEL, 0, 0));
            if (selected == 0) span_ = true;
            else { span_ = false; selectedMonitor_ = selected - 1; }
            UpdateLayout(); SaveOnChange();
            return 0;
        }
        return 0;
    }
    case WM_CLOSE: ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_DESTROY: settingsWindow_.hwnd = nullptr; settingsWindow_.content = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

std::filesystem::path App::ResolveConfigPath() {
    DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!required) return {};
    std::wstring root(static_cast<size_t>(required), L'\0');
    DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), required);
    if (!copied || copied >= required) return {};
    root.resize(copied);
    return std::filesystem::path(root) / L"ReynoldsBoids" / L"settings.ini";
}
void App::ReadSettingLine(const std::string& key, const std::string& value, bool& malformed) {
    char* end = nullptr;
    auto number = [&]() -> double {
        const double parsed = std::strtod(value.c_str(), &end);
        if (end == value.c_str() || *end || !std::isfinite(parsed)) { malformed = true; return std::numeric_limits<double>::quiet_NaN(); }
        return parsed;
    };
    auto boolean = [&]() -> bool {
        if (value == "1" || value == "true") return true;
        if (value == "0" || value == "false") return false;
        malformed = true; return false;
    };
    auto setFloat = [&](float& target, double minimum, double maximum) {
        const double parsed = number(); if (std::isfinite(parsed)) target = static_cast<float>(std::clamp(parsed, minimum, maximum));
    };
    if (key == "count") { const double v = number(); if (std::isfinite(v)) settings_.count = static_cast<int>(std::clamp(std::round(v), 0.0, static_cast<double>(flock::MaxBirds))); }
    else if (key == "size") setFloat(settings_.size, 1, 16);
    else if (key == "speed") setFloat(settings_.speed, 20, 240);
    else if (key == "speedError") setFloat(settings_.speedError, 0, .5);
    else if (key == "separation") setFloat(settings_.separation, 0, 4);
    else if (key == "alignment") setFloat(settings_.alignment, 0, 4);
    else if (key == "cohesion") setFloat(settings_.cohesion, 0, 4);
    else if (key == "edge") setFloat(settings_.edge, 0, .35);
    else if (key == "trails") settings_.trails = boolean();
    else if (key == "trailDuration") setFloat(settings_.trailDuration, .1, .5);
    else if (key == "color") { flock::Color color{}; if (ParseColor(ToWide(value), color)) settings_.color = color; else malformed = true; }
    else if (key == "brightness") setFloat(settings_.brightness, .1, 2);
    else if (key == "contrast") setFloat(settings_.contrast, .5, 2);
    else if (key == "randomColor") settings_.randomColor = boolean();
    else if (key == "avoidTaskbar") settings_.avoidTaskbar = boolean();
    else if (key == "marginLeft") setFloat(settings_.marginLeft, 0, 200);
    else if (key == "marginRight") setFloat(settings_.marginRight, 0, 200);
    else if (key == "marginTop") setFloat(settings_.marginTop, 0, 200);
    else if (key == "marginBottom") setFloat(settings_.marginBottom, 0, 200);
    else if (key == "renderScale") setFloat(settings_.renderScale, .5, 1);
    else if (key == "fps") { const double v = number(); if (std::isfinite(v)) settings_.fps = static_cast<int>(std::clamp(std::round(v), 15.0, 120.0)); }
    else if (key == "trailHz") { const double v = number(); if (std::isfinite(v)) settings_.trailHz = static_cast<int>(std::clamp(std::round(v), 10.0, 60.0)); }
    else if (key == "monitor") { const double v = number(); if (std::isfinite(v)) selectedMonitor_ = std::max(0, static_cast<int>(std::round(v))); }
    else if (key == "span") span_ = boolean();
}
void App::LoadConfiguration() {
    if (options_.noSettings || options_.smoke) return;
    configPath_ = ResolveConfigPath();
    if (configPath_.empty()) {
        ShowError(nullptr, L"设置存储不可用", L"无法读取 LOCALAPPDATA 环境变量。应用将使用默认设置；本次修改无法保存。\n可使用 --no-settings 明确禁用个人配置读写。");
        noSave_ = true;
        return;
    }
    std::error_code ec;
    const bool exists = std::filesystem::exists(configPath_, ec);
    if (ec) {
        ShowError(nullptr, L"设置读取失败", L"无法检查设置文件：\n" + configPath_.wstring() + L"\n\n" + ToWide(ec.message()));
        return;
    }
    if (!exists) return;
    std::ifstream file(configPath_);
    if (!file) {
        ShowError(nullptr, L"设置读取失败", L"无法读取设置文件：\n" + configPath_.wstring());
        return;
    }
    bool malformed = false;
    std::string line;
    while (std::getline(file, line)) {
        const size_t delimiter = line.find('=');
        if (delimiter == std::string::npos || delimiter == 0) { if (!line.empty() && line[0] != '#') malformed = true; continue; }
        ReadSettingLine(line.substr(0, delimiter), line.substr(delimiter + 1), malformed);
    }
    if (file.bad()) {
        ShowError(nullptr, L"设置读取失败", L"读取配置时发生 I/O 错误：\n" + configPath_.wstring());
    } else if (malformed) {
        ShowError(nullptr, L"设置文件含有无效值", L"部分无效或未知格式的配置值已保留为默认值/忽略。修改设置后会以有效设置重写。\n\n" + configPath_.wstring());
    }
}
void App::WriteSettingsFile(const std::filesystem::path& path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("open failed");
    file << std::setprecision(9)
         << "# Reynolds Boids Native settings v1\n"
         << "count=" << settings_.count << '\n'
         << "size=" << settings_.size << '\n'
         << "speed=" << settings_.speed << '\n'
         << "speedError=" << settings_.speedError << '\n'
         << "separation=" << settings_.separation << '\n'
         << "alignment=" << settings_.alignment << '\n'
         << "cohesion=" << settings_.cohesion << '\n'
         << "edge=" << settings_.edge << '\n'
         << "trails=" << (settings_.trails ? 1 : 0) << '\n'
         << "trailDuration=" << settings_.trailDuration << '\n'
         << "color=" << ToUtf8(ColorHex(settings_.color)) << '\n'
         << "brightness=" << settings_.brightness << '\n'
         << "contrast=" << settings_.contrast << '\n'
         << "randomColor=" << (settings_.randomColor ? 1 : 0) << '\n'
         << "avoidTaskbar=" << (settings_.avoidTaskbar ? 1 : 0) << '\n'
         << "marginLeft=" << settings_.marginLeft << '\n'
         << "marginRight=" << settings_.marginRight << '\n'
         << "marginTop=" << settings_.marginTop << '\n'
         << "marginBottom=" << settings_.marginBottom << '\n'
         << "renderScale=" << settings_.renderScale << '\n'
         << "fps=" << settings_.fps << '\n'
         << "trailHz=" << settings_.trailHz << '\n'
         << "monitor=" << selectedMonitor_ << '\n'
         << "span=" << (span_ ? 1 : 0) << '\n';
    file.flush();
    if (!file) throw std::runtime_error("write failed");
}
void App::SaveConfiguration(bool showFailure) {
    if (noSave_ || options_.noSettings || options_.smoke) return;
    if (configPath_.empty()) configPath_ = ResolveConfigPath();
    if (configPath_.empty()) {
        if (showFailure) ShowError(hwnd_, L"设置保存失败", L"无法访问 LOCALAPPDATA；设置仅在本次运行有效。");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(configPath_.parent_path(), ec);
    if (ec) {
        if (showFailure && !configFailureReported_) ShowError(hwnd_, L"设置保存失败", L"无法创建配置目录：\n" + configPath_.parent_path().wstring() + L"\n\n" + ToWide(ec.message()));
        configFailureReported_ = true;
        return;
    }
    const std::filesystem::path temporary = configPath_.wstring() + L".tmp";
    try {
        WriteSettingsFile(temporary);
        if (!MoveFileExW(temporary.c_str(), configPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error(ToUtf8(LastErrorText()));
        configFailureReported_ = false;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        if (showFailure && !configFailureReported_) ShowError(hwnd_, L"设置保存失败",
            L"无法写入设置文件：\n" + configPath_.wstring() + L"\n\n" + ToWide(error.what()) + L"\n当前修改仅在本次运行有效。");
        configFailureReported_ = true;
    }
}
void App::SaveOnChange() { SaveConfiguration(); }
void App::PersistModeSettings() { SaveConfiguration(); }

std::vector<MonitorInfo> App::EnumerateMonitors() const {
    std::vector<MonitorInfo> result;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorProc, reinterpret_cast<LPARAM>(&result));
    std::sort(result.begin(), result.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.monitor.left != b.monitor.left) return a.monitor.left < b.monitor.left;
        return a.monitor.top < b.monitor.top;
    });
    return result;
}
void App::UpdateMonitorList() {
    std::wstring previous;
    if (selectedMonitor_ >= 0 && selectedMonitor_ < static_cast<int>(monitors_.size())) previous = monitors_[selectedMonitor_].device;
    monitors_ = EnumerateMonitors();
    if (monitors_.empty()) {
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        if (primary && GetMonitorInfoW(primary, &mi)) monitors_.push_back({primary, mi.rcMonitor, mi.rcWork, L"Primary display"});
    }
    if (!previous.empty()) {
        for (size_t i = 0; i < monitors_.size(); ++i) if (monitors_[i].device == previous) { selectedMonitor_ = static_cast<int>(i); break; }
    }
    if (monitors_.empty()) selectedMonitor_ = 0;
    else selectedMonitor_ = std::clamp(selectedMonitor_, 0, static_cast<int>(monitors_.size() - 1));
}
RECT App::TargetRectangle() const {
    if (span_) {
        RECT rect{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                  GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                  GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
        if (rect.right <= rect.left || rect.bottom <= rect.top) return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        return rect;
    }
    if (!monitors_.empty()) return monitors_[std::clamp(selectedMonitor_, 0, static_cast<int>(monitors_.size() - 1))].monitor;
    return {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
}
RECT App::EffectiveWorkRectangle() const {
    const RECT target = TargetRectangle();
    if (!settings_.avoidTaskbar) return target;
    bool any = false;
    RECT result{};
    for (const MonitorInfo& monitor : monitors_) {
        RECT intersection{};
        if (!IntersectRect(&intersection, &target, &monitor.monitor)) continue;
        RECT work{};
        if (!IntersectRect(&work, &target, &monitor.work)) continue;
        if (!any) { result = work; any = true; }
        else { result.left = std::min(result.left, work.left); result.top = std::min(result.top, work.top); result.right = std::max(result.right, work.right); result.bottom = std::max(result.bottom, work.bottom); }
    }
    return any ? result : target;
}
float App::DpiScale() const {
    UINT dpi = 96;
    if (hwnd_ && IsWindow(hwnd_)) dpi = GetDpiForWindow(hwnd_);
    return std::max(1u, dpi) / 96.0f;
}
void App::UpdateClientDimensions() {
    RECT client{};
    if (hwnd_ && GetClientRect(hwnd_, &client)) {
        lastWidth_ = std::max(0, static_cast<int>(client.right - client.left));
        lastHeight_ = std::max(0, static_cast<int>(client.bottom - client.top));
    }
    const float scale = DpiScale();
    logicalWidth_ = std::max(1.f, lastWidth_ / scale);
    logicalHeight_ = std::max(1.f, lastHeight_ / scale);
    if (hwnd_) cachedDpi_ = static_cast<int>(std::lround(scale * 96.f));
}
void App::ConfigureSimulation() {
    UpdateClientDimensions();
    const float scale = DpiScale();
    const RECT target = TargetRectangle();
    const RECT work = span_ && settings_.avoidTaskbar ? target : EffectiveWorkRectangle();
    POINT clientOrigin{};
    if (!hwnd_ || !ClientToScreen(hwnd_, &clientOrigin)) {
        clientOrigin.x = target.left;
        clientOrigin.y = target.top;
    }
    const float maxX = logicalWidth_;
    const float maxY = logicalHeight_;
    flock::Bounds bounds{};
    bounds.left = std::clamp(static_cast<float>(work.left - clientOrigin.x) / scale + settings_.marginLeft, 0.f, maxX);
    bounds.right = std::clamp(static_cast<float>(work.right - clientOrigin.x) / scale - settings_.marginRight, 0.f, maxX);
    bounds.top = std::clamp(static_cast<float>(work.top - clientOrigin.y) / scale + settings_.marginTop, 0.f, maxY);
    bounds.bottom = std::clamp(static_cast<float>(work.bottom - clientOrigin.y) / scale - settings_.marginBottom, 0.f, maxY);
    if (bounds.right < bounds.left) bounds.right = bounds.left;
    if (bounds.bottom < bounds.top) bounds.bottom = bounds.top;

    taskbarExclusions_.clear();
    if (span_ && settings_.avoidTaskbar) {
        const size_t requiredCapacity = monitors_.size() * 4;
        if (taskbarExclusions_.capacity() < requiredCapacity) taskbarExclusions_.reserve(requiredCapacity);
        const auto appendExclusion = [&](const RECT& physicalRect) {
            RECT clipped{};
            if (!IntersectRect(&clipped, &physicalRect, &target)) return;
            float left = static_cast<float>(clipped.left - clientOrigin.x) / scale;
            float top = static_cast<float>(clipped.top - clientOrigin.y) / scale;
            float right = static_cast<float>(clipped.right - clientOrigin.x) / scale;
            float bottom = static_cast<float>(clipped.bottom - clientOrigin.y) / scale;
            left = std::max(left, bounds.left);
            top = std::max(top, bounds.top);
            right = std::min(right, bounds.right);
            bottom = std::min(bottom, bounds.bottom);
            if (right > left && bottom > top) taskbarExclusions_.push_back({left, top, right, bottom});
        };
        for (const MonitorInfo& monitor : monitors_) {
            RECT monitorIntersection{};
            if (!IntersectRect(&monitorIntersection, &target, &monitor.monitor)) continue;
            RECT monitorWork{};
            if (!IntersectRect(&monitorWork, &monitor.work, &monitor.monitor)) continue;
            if (monitorWork.top > monitor.monitor.top)
                appendExclusion({monitor.monitor.left, monitor.monitor.top, monitor.monitor.right, monitorWork.top});
            if (monitorWork.bottom < monitor.monitor.bottom)
                appendExclusion({monitor.monitor.left, monitorWork.bottom, monitor.monitor.right, monitor.monitor.bottom});
            if (monitorWork.left > monitor.monitor.left)
                appendExclusion({monitor.monitor.left, monitorWork.top, monitorWork.left, monitorWork.bottom});
            if (monitorWork.right < monitor.monitor.right)
                appendExclusion({monitorWork.right, monitorWork.top, monitor.monitor.right, monitorWork.bottom});
        }
    }
    simulation_.configure(settings_, bounds, std::span<const flock::Bounds>(taskbarExclusions_));
    renderDirty_ = true;
    UpdateSettingsStatus();
}

void App::PlacePreviewWindow(bool fullScreen) {
    if (!hwnd_) return;
    DetachDesktop();
    ResetDesktopInputEdge();
    desktopMode_ = false;
    previewFullScreen_ = fullScreen;
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, (exStyle | WS_EX_APPWINDOW) & ~WS_EX_TOOLWINDOW);
    if (fullScreen) {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
        const RECT target = TargetRectangle();
        SetWindowPos(hwnd_, HWND_TOPMOST, target.left, target.top, target.right - target.left, target.bottom - target.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN);
        const RECT target = TargetRectangle();
        const int width = std::min(1280, std::max(720, static_cast<int>(target.right - target.left - 80)));
        const int height = std::min(800, std::max(480, static_cast<int>(target.bottom - target.top - 100)));
        const int x = static_cast<int>(target.left) + std::max(0, static_cast<int>(((target.right - target.left) - width) / 2));
        const int y = static_cast<int>(target.top) + std::max(0, static_cast<int>(((target.bottom - target.top) - height) / 2));
        SetWindowPos(hwnd_, HWND_NOTOPMOST, x, y, width, height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
#if defined(FLOCK_CAPACITY_MEASUREMENT)
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
#else
    ShowWindow(hwnd_, SW_SHOW);
#endif
    ConfigureSimulation();
    ScheduleNextFrame(Clock::now());
    savedTopLevelStyles_ = false;
    UpdateTray();
}
void App::DetachDesktop() {
    if (!hwnd_ || !hadDesktopParent_) return;
    ShowWindow(hwnd_, SW_HIDE);
    SetParent(hwnd_, nullptr);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(originalStyle_));
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, static_cast<LONG_PTR>(originalExStyle_));
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    hadDesktopParent_ = false;
    desktopParent_ = nullptr;
    savedTopLevelStyles_ = false;
}
bool App::BeginDesktopMount(bool notifyFailure) {
    if (!hwnd_) return false;
    HWND worker = FindDesktopWorker();
    if (!worker) {
        if (notifyFailure) ShowError(hwnd_, L"无法挂载到桌面", L"未能找到包含桌面图标的 WorkerW 窗口。请在预览模式继续使用；Explorer 可能正在重启或当前桌面不支持该挂载方式。");
        return false;
    }
    RECT target = TargetRectangle();
    RECT parentRect{};
    if (!GetWindowRect(worker, &parentRect)) {
        if (notifyFailure) ShowError(hwnd_, L"桌面挂载失败", L"无法读取 WorkerW 的屏幕位置：" + LastErrorText());
        return false;
    }
    if (!savedTopLevelStyles_) {
        originalStyle_ = static_cast<DWORD_PTR>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        originalExStyle_ = static_cast<DWORD_PTR>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        savedTopLevelStyles_ = true;
    }
    ShowWindow(hwnd_, SW_HIDE);
    SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    SetLastError(ERROR_SUCCESS);
    SetParent(hwnd_, worker);
    if (GetParent(hwnd_) != worker) {
        const DWORD err = GetLastError();
        SetParent(hwnd_, nullptr);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, static_cast<LONG_PTR>(originalStyle_));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, static_cast<LONG_PTR>(originalExStyle_));
        savedTopLevelStyles_ = false;
        if (notifyFailure) ShowError(hwnd_, L"桌面挂载失败", L"Windows 未能将窗口挂入 WorkerW：" + LastErrorText(err));
        return false;
    }
    hadDesktopParent_ = true;
    desktopParent_ = worker;
    desktopMode_ = true;
    previewFullScreen_ = false;
    const int x = target.left - parentRect.left;
    const int y = target.top - parentRect.top;
    SetLastError(ERROR_SUCCESS);
    if (!SetWindowPos(hwnd_, HWND_TOP, x, y, target.right - target.left, target.bottom - target.top,
                      SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE) ||
        !IsWindow(worker) || GetParent(hwnd_) != worker) {
        DWORD err = GetLastError();
        if (err == ERROR_SUCCESS) err = ERROR_INVALID_WINDOW_HANDLE;
        DetachDesktop();
        desktopMode_ = false;
        if (notifyFailure) ShowError(hwnd_, L"桌面挂载失败", L"WorkerW 销毁或无法定位壁纸窗口：" + LastErrorText(err));
        return false;
    }
    ShowWindow(hwnd_, SW_SHOWNA);
    ResetDesktopInputEdge();
    ConfigureSimulation();
    ScheduleNextFrame(Clock::now());
    UpdateTray();
    return true;
}
bool App::ReattachDesktop() {
    if (!desktopMode_) return true;
    if (desktopParent_ && IsWindow(desktopParent_) && GetParent(hwnd_) == desktopParent_) return true;
    hadDesktopParent_ = false;
    desktopParent_ = nullptr;
    return BeginDesktopMount(false);
}
void App::ReturnToPreview(bool fullScreen, bool notifyFailure) {
    (void)notifyFailure;
    PlacePreviewWindow(fullScreen);
}
void App::SetDesktop(bool desktop, bool notify) {
    if (desktop) {
        if (!BeginDesktopMount(notify)) {
            ReturnToPreview(true);
            if (notify && !options_.wallpaper) ShowInfo(hwnd_, L"已切换到安全预览", L"桌面挂载失败，应用仍可在预览窗口中运行。托盘中的“挂载桌面”可以再次尝试。");
        }
    } else SetPreview(true);
    UpdateTray();
    PersistModeSettings();
}
void App::SetPreview(bool fullScreen, bool save) {
    PlacePreviewWindow(fullScreen);
    if (save) PersistModeSettings();
}
void App::UpdateLayout(bool rebuildMonitors) {
    if (rebuildMonitors || monitors_.empty()) UpdateMonitorList();
    if (desktopMode_) {
        if (!ReattachDesktop()) {
            desktopMode_ = false;
            PlacePreviewWindow(true);
            ShowError(hwnd_, L"桌面窗口已失效", L"Explorer/WorkerW 窗口不可用，已退回预览模式。可以稍后从托盘再次挂载。");
        } else {
            RECT target = TargetRectangle(), parentRect{};
            GetWindowRect(desktopParent_, &parentRect);
            SetWindowPos(hwnd_, HWND_TOP, target.left - parentRect.left, target.top - parentRect.top,
                         target.right - target.left, target.bottom - target.top, SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    } else if (IsWindowVisible(hwnd_)) PlacePreviewWindow(previewFullScreen_);
    ConfigureSimulation();
    if (settingsWindow_.hwnd) RefreshSettingsControls();
}

void App::SetPaused(bool value) {
    if (paused_ == value) return;
    paused_ = value;
    lastSubmitAt_ = Clock::time_point{};
    clickPending_ = false;
    if (desktopMode_) ResetDesktopInputEdge();
    if (paused_) {
        accumulator_ = 0;
        lastFrameAt_ = Clock::now();
        nextFrameAt_ = lastFrameAt_;
    } else {
        lastFrameAt_ = Clock::now();
        nextFrameAt_ = lastFrameAt_;
        accumulator_ = 0;
    }
    UpdateTray();
    if (settingsWindow_.hwnd) RefreshSettingsControls();
    UpdateSettingsStatus();
    LogEvent(paused_ ? "pause=true" : "pause=false");
}
void App::ResetSimulation() {
    simulation_.reset(20261022);
    ConfigureSimulation();
    LogEvent("simulation_reset seed=20261022");
}
void App::ChangeFps(int fps) {
    settings_.fps = std::clamp(fps, 15, 120);
    ConfigureSimulation();
    RefreshSettingsControls();
    SaveOnChange();
    ScheduleNextFrame(Clock::now());
    LogEvent("fps_preset=" + std::to_string(settings_.fps));
}
void App::SetCheckbox(int id, bool checked) {
    if (id == IdTrails) settings_.trails = checked;
    else if (id == IdRandomColor) settings_.randomColor = checked;
    else if (id == IdAvoidTaskbar) settings_.avoidTaskbar = checked;
    ConfigureSimulation();
    SaveOnChange();
    if (id == IdAvoidTaskbar) UpdateLayout();
}

void App::CommitNumber(int id) {
    auto field = settingsWindow_.fields.find(id);
    auto spec = std::find_if(NumberSpecs.begin(), NumberSpecs.end(), [id](const NumberSpec& candidate) { return candidate.id == id; });
    if (field == settingsWindow_.fields.end() || spec == NumberSpecs.end()) return;
    const int length = GetWindowTextLengthW(field->second);
    std::wstring text(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(field->second, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    double displayValue = 0;
    if (!ParseDouble(text, displayValue)) {
        ShowError(settingsWindow_.hwnd, L"无效数值", std::wstring(spec->label) + L"请输入有效数字。已恢复原值。");
        RefreshSettingsControls();
        return;
    }
    displayValue = std::clamp(displayValue, spec->minimum, spec->maximum);
    displayValue = spec->minimum + std::round((displayValue - spec->minimum) / spec->step) * spec->step;
    displayValue = std::clamp(displayValue, spec->minimum, spec->maximum);
    const float value = static_cast<float>(displayValue / spec->displayScale);
    switch (spec->field) {
    case NumberField::Count: settings_.count = static_cast<int>(displayValue); break;
    case NumberField::Size: settings_.size = value; break;
    case NumberField::Speed: settings_.speed = value; break;
    case NumberField::SpeedError: settings_.speedError = value; break;
    case NumberField::Separation: settings_.separation = value; break;
    case NumberField::Alignment: settings_.alignment = value; break;
    case NumberField::Cohesion: settings_.cohesion = value; break;
    case NumberField::Edge: settings_.edge = value; break;
    case NumberField::TrailDuration: settings_.trailDuration = value; break;
    case NumberField::Brightness: settings_.brightness = value; break;
    case NumberField::Contrast: settings_.contrast = value; break;
    case NumberField::MarginLeft: settings_.marginLeft = value; break;
    case NumberField::MarginRight: settings_.marginRight = value; break;
    case NumberField::MarginTop: settings_.marginTop = value; break;
    case NumberField::MarginBottom: settings_.marginBottom = value; break;
    case NumberField::RenderScale: settings_.renderScale = value; break;
    case NumberField::Fps: settings_.fps = static_cast<int>(displayValue); break;
    case NumberField::TrailHz: settings_.trailHz = static_cast<int>(displayValue); break;
    }
    RefreshSettingsControls();
    ConfigureSimulation();
    SaveOnChange();
    if (spec->field == NumberField::Fps) ScheduleNextFrame(Clock::now());
    LogEvent("setting_changed id=" + std::to_string(id));
}
void App::CommitColor() {
    auto field = settingsWindow_.fields.find(IdColorText);
    if (field == settingsWindow_.fields.end()) return;
    int length = GetWindowTextLengthW(field->second);
    std::wstring text(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(field->second, text.data(), length + 1); text.resize(static_cast<size_t>(length));
    flock::Color color{};
    if (!ParseColor(text, color)) {
        ShowError(settingsWindow_.hwnd, L"无效颜色", L"颜色请输入 #RRGGBB 格式，例如 #84D8E0。已恢复原值。");
        RefreshSettingsControls(); return;
    }
    settings_.color = color;
    RefreshSettingsControls(); ConfigureSimulation(); SaveOnChange();
    LogEvent("setting_changed color");
}

bool App::OpenLog() {
    if (!options_.log) return true;
    log_.open(std::filesystem::path(*options_.log), std::ios::binary | std::ios::trunc);
    if (!log_) {
        ShowError(nullptr, L"性能日志创建失败", L"无法创建日志文件：\n" + *options_.log);
        return false;
    }
    log_ << "# Reynolds Boids Native actual timing log\n"
         << "# adapter=" << (renderer_ ? renderer_->adapterName() : std::string("unavailable")) << "\n"
         << "# initial_physical_width=" << lastWidth_ << "\n"
         << "# initial_physical_height=" << lastHeight_ << "\n"
         << "# max_capacity=" << flock::MaxBirds << " birds\n"
         << "# requested_workers=" << options_.threads << "\n"
         << "# configured_workers=" << simulation_.workerCount() << "\n"
         << "# fps_cap_fps=" << settings_.fps << " fps; configured application cap, not a display presentation guarantee\n"
         << "# perception_radius_px=" << flock::PerceptionRadius << "\n"
         << "# separation_radius_px=" << flock::SeparationRadius << "\n"
         << "# settings.count_birds=" << settings_.count << "\n"
         << "# settings.size_px=" << settings_.size << "\n"
         << "# settings.speed_px_per_s=" << settings_.speed << "\n"
         << "# settings.speed_error_fraction=" << settings_.speedError << "\n"
         << "# settings.separation_weight=" << settings_.separation << "\n"
         << "# settings.alignment_weight=" << settings_.alignment << "\n"
         << "# settings.cohesion_weight=" << settings_.cohesion << "\n"
         << "# settings.edge_turn_fraction=" << settings_.edge << "\n"
         << "# settings.trails_enabled=" << std::boolalpha << settings_.trails << "\n"
         << "# settings.trail_duration_s=" << settings_.trailDuration << "\n"
         << "# settings.color_rgb_hex=" << ToUtf8(ColorHex(settings_.color)) << "\n"
         << "# settings.brightness_multiplier=" << settings_.brightness << "\n"
         << "# settings.contrast_multiplier=" << settings_.contrast << "\n"
         << "# settings.random_color_enabled=" << settings_.randomColor << "\n"
         << "# settings.avoid_taskbar=" << settings_.avoidTaskbar << "\n"
         << "# settings.margin_left_px=" << settings_.marginLeft << "\n"
         << "# settings.margin_right_px=" << settings_.marginRight << "\n"
         << "# settings.margin_top_px=" << settings_.marginTop << "\n"
         << "# settings.margin_bottom_px=" << settings_.marginBottom << "\n"
         << "# settings.render_scale=" << settings_.renderScale << "\n"
         << "# settings.target_fps=" << settings_.fps << "\n"
         << "# settings.trail_sample_hz=" << settings_.trailHz << "\n"
         << "# settings.monitor_index=" << selectedMonitor_ << "\n"
         << "# settings.span_virtual_desktop=" << std::noboolalpha << span_ << "\n";
#if defined(FLOCK_CAPACITY_MEASUREMENT)
    log_ << "# log_flush=buffered_capacity_measurement\n"
         << "# log_flush_note=frame rows and events are buffered; stream formatting and buffered I/O still add measurement overhead\n";
#else
    log_ << "# log_flush=per_frame_event_flush\n";
#endif
    log_ << "# measured submission intervals and call durations only; Present feedback for actual display presentation is unavailable\n"
         << "# present_feedback_available=false; submission counters are not display presentation counters\n"
         << "elapsed_seconds,physical_width,physical_height,logical_width,logical_height,simulation_steps_since_previous,simulation_ms,submit_interval_ms,frame_cpu_ms,draw_call_ms,submitted_frame_count,total_simulation_steps,birds_active,paused,dropped_simulation_steps,dropped_frame_deadlines,entering,leaving,dying,total_birds,target_birds\n";
    log_.flush();
    return static_cast<bool>(log_);
}
void App::LogEvent(const std::string& event) {
    if (options_.smoke) smokeSteps_.push_back(event);
    if (log_) {
        const double elapsed = std::chrono::duration<double>(Clock::now() - startedAt_).count();
        log_ << "# event elapsed_seconds=" << std::fixed << std::setprecision(6) << elapsed << " " << event << '\n';
#if !defined(FLOCK_CAPACITY_MEASUREMENT)
        log_.flush();
#endif
    }
}
bool App::IsRenderable() const {
    return hwnd_ && rendererReady_ && IsWindowVisible(hwnd_) && !minimized_ && !suspended_ &&
           !sessionLocked_ && !displayOff_ && !coveragePaused_ && inputDesktopActive_;
}
void App::UpdateCoveragePause() {
    coveragePaused_ = desktopMode_ && ForegroundWindowCoversTarget(hwnd_, TargetRectangle(), monitors_);
}
void App::UpdateInputDesktopState() {
    inputDesktopActive_ = IsDrawingOnInputDesktop();
}
void App::ResetDesktopInputEdge() {
    pointerActive_ = false;
    clickPending_ = false;
    desktopLeftButtonDown_ = IsLeftMouseButtonDown();
}
void App::UpdateDesktopInput() {
    if (!desktopMode_) return;
    const bool leftDown = IsLeftMouseButtonDown();
    if (paused_ || !IsRenderable()) {
        pointerActive_ = false;
        clickPending_ = false;
        desktopLeftButtonDown_ = leftDown;
        return;
    }

    POINT screen{};
    if (!GetCursorPos(&screen)) {
        pointerActive_ = false;
        desktopLeftButtonDown_ = leftDown;
        return;
    }
    const HWND hitWindow = WindowFromPoint(screen);
    const HWND root = hitWindow ? GetAncestor(hitWindow, GA_ROOT) : nullptr;
    const DWORD shellProcessId = WindowProcessId(desktopParent_);
    const bool settingsHit = settingsWindow_.hwnd &&
        (hitWindow == settingsWindow_.hwnd || IsChild(settingsWindow_.hwnd, hitWindow));
    const bool shellHit = !settingsHit && shellProcessId &&
        (IsWindowClass(root, L"Progman") || IsWindowClass(root, L"WorkerW")) &&
        WindowProcessId(root) == shellProcessId && WindowProcessId(hitWindow) == shellProcessId;
    const bool ownHit = !settingsHit && (hitWindow == hwnd_ || (hitWindow && IsChild(hwnd_, hitWindow)));
    if (!shellHit && !ownHit) {
        pointerActive_ = false;
        desktopLeftButtonDown_ = leftDown;
        return;
    }

    POINT client = screen;
    if (!ScreenToClient(hwnd_, &client)) {
        pointerActive_ = false;
        desktopLeftButtonDown_ = leftDown;
        return;
    }
    pointer_.x = client.x;
    pointer_.y = client.y;
    pointerActive_ = true;
    if (leftDown && !desktopLeftButtonDown_) {
        clickPending_ = true;
        std::ostringstream event;
        event << "desktop_click screen_x=" << screen.x << " screen_y=" << screen.y
              << " client_x=" << client.x << " client_y=" << client.y;
        LogEvent(event.str());
    }
    desktopLeftButtonDown_ = leftDown;
}
void App::UpdateAutomaticPause() {
    std::string reason;
    if (suspended_) reason = "system_suspend";
    else if (sessionLocked_) reason = "session_locked";
    else if (!inputDesktopActive_) reason = "inactive_desktop";
    else if (displayOff_) reason = "display_off";
    else if (minimized_) reason = "window_minimized";
    else if (hwnd_ && !IsWindowVisible(hwnd_) && !desktopMode_) reason = "preview_hidden";
    else if (coveragePaused_) reason = "fullscreen_foreground_cover";
    if (reason == autoPauseReason_) return;
    autoPauseReason_ = reason;
    accumulator_ = 0;
    if (desktopMode_) ResetDesktopInputEdge();
    lastFrameAt_ = Clock::now();
    nextFrameAt_ = lastFrameAt_;
    lastSubmitAt_ = Clock::time_point{};
    if (reason.empty()) renderDirty_ = true;
    LogEvent("automatic_pause_reason=" + (reason.empty() ? std::string("none") : reason));
}
void App::ScheduleNextFrame(Clock::time_point now) {
    const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / std::max(15, settings_.fps)));
    nextFrameAt_ = now + std::max(interval, Clock::duration(1));
}
void App::AdvanceFrameDeadline(Clock::time_point now) {
    const auto interval = std::max(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / std::max(15, settings_.fps))), Clock::duration(1));
    if (paused_) {
        nextFrameAt_ = now + interval;
        return;
    }
    if (nextFrameAt_ == Clock::time_point{}) nextFrameAt_ = now + interval;
    else nextFrameAt_ += interval;
    if (nextFrameAt_ <= now) {
        const auto late = now - nextFrameAt_;
        const auto skipped = late / interval + 1;
        droppedFrameDeadlines_ += static_cast<std::uint64_t>(skipped);
        // 帧超时 (overrun) 时重置为当前时刻立即就绪，不额外等待一整格栅格，亦不回放过期的绘制帧
        nextFrameAt_ = now;
        LogEvent("dropped_frame_deadlines=" + std::to_string(skipped));
    }
}
flock::Input App::CurrentInput() const {
    flock::Input input{};
    input.pointerActive = pointerActive_;
    const float scale = DpiScale();
    input.pointer.x = pointer_.x / scale;
    input.pointer.y = pointer_.y / scale;
    input.click = clickPending_;
    return input;
}
bool App::RenderFrame(Clock::time_point now) {
    UpdateClientDimensions();
    if (!rendererReady_ || !renderer_) return false;
    if (lastWidth_ <= 0 || lastHeight_ <= 0) {
        lastFrameAt_ = Clock::now();
        accumulator_ = 0;
        AdvanceFrameDeadline(lastFrameAt_);
        return true;
    }
    const Clock::time_point frameBegin = Clock::now();
    const double raw = std::chrono::duration<double>(frameBegin - lastFrameAt_).count();
    lastFrameAt_ = frameBegin;
    const double elapsed = std::clamp(raw, 0.0, 0.1);
    constexpr double fixedStep = 1.0 / 60.0;
    int steps = 0;
    int droppedSteps = 0;
    double simMs = 0;
    if (!paused_) {
        accumulator_ += elapsed;
        if (accumulator_ > .1) {
            droppedSteps += static_cast<int>((accumulator_ - .1) / fixedStep);
            accumulator_ = .1;
        }
        while (accumulator_ + 1e-9 >= fixedStep && steps < 4) {
            const auto before = Clock::now();
            const flock::Input input = CurrentInput();
            try {
                simulation_.step(static_cast<float>(fixedStep), input, false);
            } catch (const std::exception& error) {
                LogEvent("simulation_failed=" + std::string(error.what()));
                WriteCommandLineError(L"模拟计算失败：" + ToWide(error.what()));
                runtimeFailed_ = true;
                return false;
            }
            simMs += std::chrono::duration<double, std::milli>(Clock::now() - before).count();
            accumulator_ -= fixedStep;
            ++steps;
            ++simulationSteps_;
            clickPending_ = false;
        }
        if (steps == 4 && accumulator_ >= fixedStep) {
            droppedSteps += static_cast<int>(accumulator_ / fixedStep);
            accumulator_ = std::fmod(accumulator_, fixedStep);
        }
        if (droppedSteps > 0) {
            droppedSimulationSteps_ += static_cast<std::uint64_t>(droppedSteps);
            LogEvent("dropped_simulation_steps=" + std::to_string(droppedSteps));
        }
    } else {
        accumulator_ = 0;
        clickPending_ = false;
    }
    std::string error;
    const auto drawStart = Clock::now();
    if (!renderer_->draw(simulation_.birds(), simulation_.ghosts(), settings_, simulation_.time(), logicalWidth_, logicalHeight_, error)) {
        HandleFatalGraphicsError(error);
        return false;
    }
    const Clock::time_point submittedAt = Clock::now();
    lastFrameIntervalMs_ = lastSubmitAt_ == Clock::time_point{} ? 0.0 :
        std::chrono::duration<double, std::milli>(submittedAt - lastSubmitAt_).count();
    lastSubmitAt_ = submittedAt;
    lastDrawMs_ = std::chrono::duration<double, std::milli>(submittedAt - drawStart).count();
    lastFrameMs_ = std::chrono::duration<double, std::milli>(submittedAt - frameBegin).count();
    lastSimulationMs_ = simMs;
    renderDirty_ = false;
    ++renderedFrames_;
    pendingSteps_ = static_cast<std::uint64_t>(steps);
    frameTotalMs_ += lastFrameMs_;
    if (log_) {
        const flock::SimStats stats = simulation_.stats();
        const int totalBirds = stats.active + stats.entering + stats.leaving + stats.dying;
        const double elapsedWall = std::chrono::duration<double>(Clock::now() - startedAt_).count();
        log_ << std::fixed << std::setprecision(6) << elapsedWall << ',' << lastWidth_ << ',' << lastHeight_ << ','
             << logicalWidth_ << ',' << logicalHeight_ << ',' << pendingSteps_ << ',' << lastSimulationMs_ << ','
             << lastFrameIntervalMs_ << ',' << lastFrameMs_ << ',' << lastDrawMs_ << ','
             << renderedFrames_ << ',' << simulationSteps_ << ',' << stats.active << ',' << (paused_ ? 1 : 0) << ','
             << droppedSimulationSteps_ << ',' << droppedFrameDeadlines_ << ',' << stats.entering << ',' << stats.leaving
             << ',' << stats.dying << ',' << totalBirds << ',' << settings_.count << '\n';
#if !defined(FLOCK_CAPACITY_MEASUREMENT)
        log_.flush();
#endif
    }
    pendingSteps_ = 0;
    if (options_.screenshot && screenshotDeadlineReached_ && !screenshotComplete_) {
        if (!renderer_->capture(*options_.screenshot, error)) {
            screenshotFailed_ = true;
            HandleFatalGraphicsError(error);
            return false;
        }
        screenshotComplete_ = true;
        LogEvent("gpu_screenshot=" + ToUtf8(*options_.screenshot));
        running_ = false;
    }
    if (options_.smoke && smokeStage_ == SmokeStage::Capture && !screenshotComplete_) {
        const auto path = smokeDirectory_ / L"smoke-capture.png";
        if (!renderer_->capture(path.wstring(), error)) {
            smokeFailed_ = true;
            LogEvent("capture_failed " + error);
            FinishSmoke();
            return true;
        }
        screenshotComplete_ = true;
        LogEvent("gpu_capture=" + ToUtf8(path.wstring()));
        smokeStage_ = SmokeStage::Finish;
        nextSmokeAt_ = Clock::now() + std::chrono::milliseconds(300);
    }
    if (Clock::now() >= nextStatusAt_) {
        UpdateSettingsStatus();
        nextStatusAt_ = Clock::now() + std::chrono::milliseconds(500);
    }
    AdvanceFrameDeadline(Clock::now());
    (void)now;
    return true;
}
void App::HandleFatalGraphicsError(const std::string& error) {
    ShowError(hwnd_, L"Direct3D 渲染失败", L"GPU 初始化/绘制/截图返回错误：\n" + ToWide(error) + L"\n\n应用将干净退出。未尝试静默的软件回退。");
    runtimeFailed_ = true;
    running_ = false;
}

void App::UpdateTray() {
    if (!trayAdded_ || !keeper_) return;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = keeper_; data.uID = TrayId;
    data.uFlags = NIF_TIP;
    const wchar_t* mode = desktopMode_ ? L"桌面壁纸" : (IsWindowVisible(hwnd_) ? L"预览" : L"预览已隐藏");
    std::swprintf(data.szTip, std::size(data.szTip), L"Reynolds Boids — %s%s", mode, paused_ ? L"（已暂停）" : L"");
    Shell_NotifyIconW(NIM_MODIFY, &data);
}
void App::CreateTrayIcon() {
    if (!keeper_) return;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = keeper_; data.uID = TrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; data.uCallbackMessage = TrayMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(data.szTip, L"Reynolds Boids", static_cast<int>(std::size(data.szTip)));
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    if (trayAdded_) { data.uVersion = NOTIFYICON_VERSION; Shell_NotifyIconW(NIM_SETVERSION, &data); }
}
void App::RemoveTrayIcon() {
    if (!trayAdded_ || !keeper_) return;
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = keeper_; data.uID = TrayId;
    Shell_NotifyIconW(NIM_DELETE, &data); trayAdded_ = false;
}
void App::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, IdTraySettings, L"打开设置");
    AppendMenuW(menu, MF_STRING, IdTrayPause, paused_ ? L"继续" : L"暂停");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IdTrayPreview, L"打开预览");
    AppendMenuW(menu, MF_STRING, IdTrayDesktop, L"挂载到桌面");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IdTrayQuit, L"退出");
    POINT point{}; GetCursorPos(&point);
    SetForegroundWindow(keeper_);
    const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, keeper_, nullptr);
    DestroyMenu(menu);
    switch (selected) {
    case IdTraySettings: ShowSettings(); break;
    case IdTrayPause: SetPaused(!paused_); break;
    case IdTrayPreview: SetPreview(true); break;
    case IdTrayDesktop: SetDesktop(true); break;
    case IdTrayQuit: RequestQuit(); break;
    default: break;
    }
}
void App::RequestQuit() {
    if (quitting_) return;
    quitting_ = true;
    running_ = false;
    if (settingsWindow_.hwnd && IsWindow(settingsWindow_.hwnd)) DestroyWindow(settingsWindow_.hwnd);
    if (wtsRegistered_ && keeper_) { WTSUnRegisterSessionNotification(keeper_); wtsRegistered_ = false; }
    if (monitorPowerNotify_) { UnregisterPowerSettingNotification(monitorPowerNotify_); monitorPowerNotify_ = nullptr; }
    if (sessionDisplayNotify_) { UnregisterPowerSettingNotification(sessionDisplayNotify_); sessionDisplayNotify_ = nullptr; }
    if (consoleDisplayNotify_) { UnregisterPowerSettingNotification(consoleDisplayNotify_); consoleDisplayNotify_ = nullptr; }
    RemoveTrayIcon();
    DetachDesktop();
    if (frameTimer_) { CancelWaitableTimer(frameTimer_); CloseHandle(frameTimer_); frameTimer_ = nullptr; }
    if (hwnd_ && IsWindow(hwnd_)) DestroyWindow(hwnd_);
    if (keeper_ && IsWindow(keeper_)) DestroyWindow(keeper_);
}

void App::UpdateSettingsStatus() {
    if (!settingsWindow_.status || !IsWindow(settingsWindow_.status)) return;
    const flock::SimStats stats = simulation_.stats();
    const std::wstring adapter = renderer_ ? ToWide(renderer_->adapterName()) : L"不可用";
    const std::wstring status = L"状态：" + std::wstring(paused_ ? L"已暂停" : L"运行中") +
        L" | 模式：" + (desktopMode_ ? L"桌面壁纸" : (IsWindowVisible(hwnd_) ? L"预览" : L"预览已隐藏")) +
        L" | 鸟数：" + std::to_wstring(stats.active) + L" 活动 / " + std::to_wstring(settings_.count) + L" 目标" +
        L" | 显示：" + std::to_wstring(lastWidth_) + L" × " + std::to_wstring(lastHeight_) + L" px" +
        L" | GPU：" + adapter + L"\n应用提交间隔：" + FormatNumber(lastFrameIntervalMs_, 2) +
        L" ms；帧处理：" + FormatNumber(lastFrameMs_, 2) + L" ms；绘制：" + FormatNumber(lastDrawMs_, 2) +
        L" ms；模拟步：" + FormatNumber(lastSimulationMs_, 2) + L" ms；累计步数：" + std::to_wstring(simulationSteps_);
    SetWindowTextW(settingsWindow_.status, status.c_str());
}

void App::ExportDiagnostics() {
    OPENFILENAMEW dialog{};
    wchar_t path[MAX_PATH] = L"ReynoldsBoids-diagnostics.json";
    dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = settingsWindow_.hwnd;
    dialog.lpstrFilter = L"JSON 诊断文件 (*.json)\0*.json\0所有文件 (*.*)\0*.*\0\0";
    dialog.lpstrFile = path; dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"json"; dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&dialog)) return;
    const SYSTEMTIME utc = [] { SYSTEMTIME value{}; GetSystemTime(&value); return value; }();
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const flock::SimStats stats = simulation_.stats();
    const double elapsed = std::chrono::duration<double>(Clock::now() - startedAt_).count();
    const double averageFrame = renderedFrames_ ? frameTotalMs_ / renderedFrames_ : 0;
    std::ostringstream json;
    json << std::setprecision(9)
         << "{\n  \"schemaVersion\": 1,\n  \"exportedAtUtc\": \""
         << utc.wYear << '-' << std::setw(2) << std::setfill('0') << utc.wMonth << '-' << std::setw(2) << utc.wDay << 'T'
         << std::setw(2) << utc.wHour << ':' << std::setw(2) << utc.wMinute << ':' << std::setw(2) << utc.wSecond << "Z\",\n"
         << "  \"settings\": {\n"
         << "    \"count\": " << settings_.count << ", \"size\": " << settings_.size << ", \"speed\": " << settings_.speed << ", \"speedError\": " << settings_.speedError << ",\n"
         << "    \"separation\": " << settings_.separation << ", \"alignment\": " << settings_.alignment << ", \"cohesion\": " << settings_.cohesion << ", \"edge\": " << settings_.edge << ",\n"
         << "    \"trails\": " << (settings_.trails ? "true" : "false") << ", \"trailDuration\": " << settings_.trailDuration << ", \"color\": \"" << ToUtf8(ColorHex(settings_.color)) << "\",\n"
         << "    \"brightness\": " << settings_.brightness << ", \"contrast\": " << settings_.contrast << ", \"randomColor\": " << (settings_.randomColor ? "true" : "false") << ",\n"
         << "    \"avoidTaskbar\": " << (settings_.avoidTaskbar ? "true" : "false") << ", \"marginLeft\": " << settings_.marginLeft << ", \"marginRight\": " << settings_.marginRight << ",\n"
         << "    \"marginTop\": " << settings_.marginTop << ", \"marginBottom\": " << settings_.marginBottom << ", \"renderScale\": " << settings_.renderScale << ",\n"
         << "    \"fps\": " << settings_.fps << ", \"trailHz\": " << settings_.trailHz << "\n  },\n"
         << "  \"runtime\": {\n"
         << "    \"adapter\": \"" << JsonEscape(renderer_ ? renderer_->adapterName() : std::string("unavailable")) << "\", \"mode\": \"" << (desktopMode_ ? "wallpaper" : "preview") << "\",\n"
         << "    \"requestedWorkers\": " << options_.threads << ", \"configuredWorkers\": " << simulation_.workerCount() << ",\n"
         << "    \"span\": " << (span_ ? "true" : "false") << ", \"selectedMonitorIndex\": " << selectedMonitor_ << ", \"monitorCount\": " << monitors_.size() << ",\n"
         << "    \"physicalWidth\": " << lastWidth_ << ", \"physicalHeight\": " << lastHeight_ << ", \"logicalWidth\": " << logicalWidth_ << ", \"logicalHeight\": " << logicalHeight_ << ", \"dpi\": " << cachedDpi_ << ",\n"
         << "    \"elapsedSeconds\": " << elapsed << ", \"submittedFrameCalls\": " << renderedFrames_ << ", \"simulationSteps\": " << simulationSteps_ << ",\n"
         << "    \"presentFeedbackAvailable\": false, \"droppedSimulationSteps\": " << droppedSimulationSteps_ << ", \"droppedFrameDeadlines\": " << droppedFrameDeadlines_ << ",\n"
         << "    \"lastSubmitIntervalMs\": " << lastFrameIntervalMs_ << ", \"lastFrameMs\": " << lastFrameMs_ << ", \"meanFrameMs\": " << averageFrame << ",\n"
         << "    \"lastDrawMs\": " << lastDrawMs_ << ", \"lastSimulationMs\": " << lastSimulationMs_ << ",\n"
         << "    \"automaticPauseReason\": \"" << JsonEscape(autoPauseReason_) << "\", \"activeBirds\": " << stats.active << ", \"paused\": " << (paused_ ? "true" : "false") << ", \"visible\": " << (IsWindowVisible(hwnd_) ? "true" : "false") << "\n  },\n"
         << "  \"system\": {\"logicalProcessors\": " << system.dwNumberOfProcessors << ", \"pageSize\": " << system.dwPageSize
         << ", \"virtualScreenWidth\": " << GetSystemMetrics(SM_CXVIRTUALSCREEN) << ", \"virtualScreenHeight\": " << GetSystemMetrics(SM_CYVIRTUALSCREEN) << "},\n"
         << "  \"monitors\": [\n";
    for (size_t i = 0; i < monitors_.size(); ++i) {
        const auto& m = monitors_[i];
        json << "    {\"index\": " << i << ", \"device\": \"" << JsonEscape(ToUtf8(m.device)) << "\", \"left\": " << m.monitor.left
             << ", \"top\": " << m.monitor.top << ", \"width\": " << m.monitor.right - m.monitor.left << ", \"height\": " << m.monitor.bottom - m.monitor.top
             << ", \"workLeft\": " << m.work.left << ", \"workTop\": " << m.work.top << ", \"workRight\": " << m.work.right << ", \"workBottom\": " << m.work.bottom << "}"
             << (i + 1 == monitors_.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!file) { ShowError(settingsWindow_.hwnd, L"诊断导出失败", L"无法创建文件：\n" + std::wstring(path)); return; }
    const std::string body = json.str();
    file.write(body.data(), static_cast<std::streamsize>(body.size()));
    file.close();
    if (!file) { ShowError(settingsWindow_.hwnd, L"诊断导出失败", L"写入文件时发生错误：\n" + std::wstring(path)); return; }
    LogEvent("diagnostics_export=" + ToUtf8(path));
    ShowInfo(settingsWindow_.hwnd, L"诊断已导出", L"设置、显示器环境、真实 adapter、渲染尺寸与时序数据已导出：\n" + std::wstring(path));
}

void App::RunSmokeStep(Clock::time_point now) {
    if (smokeStage_ == SmokeStage::Finish) { FinishSmoke(); return; }
    switch (smokeStage_) {
    case SmokeStage::Pause:
        SetPaused(true); LogEvent("smoke_step pause"); smokeStage_ = SmokeStage::Resume; break;
    case SmokeStage::Resume:
        SetPaused(false); LogEvent("smoke_step resume"); smokeStage_ = SmokeStage::CountZero; break;
    case SmokeStage::CountZero:
        settings_.count = 0; ConfigureSimulation(); LogEvent("smoke_step count_zero"); smokeStage_ = SmokeStage::CountRestore; break;
    case SmokeStage::CountRestore:
        settings_.count = preSmokeSettings_.count;
        ConfigureSimulation();
        LogEvent("smoke_step count_restore=" + std::to_string(settings_.count));
        smokeStage_ = SmokeStage::Resize;
        break;
    case SmokeStage::Resize: {
        if (desktopMode_) PlacePreviewWindow(true);
        GetWindowRect(hwnd_, &preSmokeRect_);
        const int w = std::max(320, static_cast<int>(preSmokeRect_.right - preSmokeRect_.left - 160));
        const int h = std::max(240, static_cast<int>(preSmokeRect_.bottom - preSmokeRect_.top - 100));
        SetWindowPos(hwnd_, HWND_TOPMOST, preSmokeRect_.left, preSmokeRect_.top, w, h, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        LogEvent("smoke_step resize_applied"); smokeStage_ = SmokeStage::Desktop; break;
    }
    case SmokeStage::Desktop:
        if (BeginDesktopMount(false)) LogEvent("smoke_step desktop_mount=success");
        else {
            smokeFailed_ = true;
            ReturnToPreview(true);
            LogEvent("smoke_step desktop_mount=failed_fell_back_to_preview");
        }
        smokeStage_ = SmokeStage::Preview; break;
    case SmokeStage::Preview:
        PlacePreviewWindow(true); LogEvent("smoke_step return_to_preview");
        smokeStage_ = SmokeStage::Capture; break;
    case SmokeStage::Capture:
        if (smokeDirectory_.empty()) {
            smokeFailed_ = true;
            LogEvent("smoke_step capture_skipped_no_temp_directory");
            smokeStage_ = SmokeStage::Finish;
        }
        break;
    case SmokeStage::Finish:
        FinishSmoke(); return;
    }
    nextSmokeAt_ = now + std::chrono::milliseconds(300);
}
void App::HandleSmokeDeadline(Clock::time_point now) {
    if (!options_.smoke || now < nextSmokeAt_) return;
    RunSmokeStep(now);
}
void App::FinishSmoke() {
    if (smokeStage_ == SmokeStage::Finish && !running_) return;
    if (options_.smoke) {
        settings_ = preSmokeSettings_;
        ConfigureSimulation();
        if (preSmokeDesktop_) SetDesktop(true, false); else SetPreview(preSmokeFullscreen_, false);
        LogEvent("smoke_step complete");
        if (!smokeDirectory_.empty()) {
            std::ofstream report(smokeDirectory_ / L"smoke-steps.txt", std::ios::binary | std::ios::trunc);
            if (!report) smokeFailed_ = true;
            else {
                for (const auto& step : smokeSteps_) report << step << '\n';
                report.flush();
                if (!report) smokeFailed_ = true;
            }
        }
        smokeStage_ = SmokeStage::Finish;
    }
    running_ = false;
}

LRESULT App::windowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        hwnd_ = hwnd;
        return 0;
    case WM_SIZE:
        minimized_ = wParam == SIZE_MINIMIZED;
        if (rendererReady_ && !minimized_) {
            const int width = LOWORD(lParam), height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                lastWidth_ = width; lastHeight_ = height;
                std::string error;
                if (!renderer_ || !renderer_->resize(width, height, error))
                    HandleFatalGraphicsError(error.empty() ? "renderer unavailable during resize" : error);
                else ConfigureSimulation();
            }
        }
        return 0;
    case WM_MOVE:
        if (!desktopMode_ && rendererReady_) ConfigureSimulation();
        return 0;
    case WM_DPICHANGED: {
        cachedDpi_ = HIWORD(wParam);
        if (!desktopMode_ && !previewFullScreen_) {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        } else UpdateLayout();
        ConfigureSimulation();
        return 0;
    }
    case WM_DISPLAYCHANGE:
        UpdateLayout(true); return 0;
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETWORKAREA) UpdateLayout(true);
        return 0;
    case WM_POWERBROADCAST:
        if (wParam == PBT_APMSUSPEND) {
            suspended_ = true; accumulator_ = 0; LogEvent("power_suspend");
        } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            suspended_ = false; accumulator_ = 0; lastFrameAt_ = Clock::now(); nextFrameAt_ = lastFrameAt_; nextInputDesktopCheck_ = lastFrameAt_; renderDirty_ = true; LogEvent("power_resume");
        } else if (wParam == PBT_POWERSETTINGCHANGE) {
            const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
            if (setting && setting->DataLength >= sizeof(DWORD)) {
                DWORD state = 0;
                std::memcpy(&state, setting->Data, sizeof(state));
                if (IsEqualGUID(setting->PowerSetting, GuidMonitorPowerOn)) monitorDisplayOff_ = state == 0;
                else if (IsEqualGUID(setting->PowerSetting, GuidConsoleDisplayState)) consoleDisplayOff_ = state == 0;
                else if (IsEqualGUID(setting->PowerSetting, GuidSessionDisplayStatus)) sessionDisplayOff_ = state == 0;
                else return TRUE;
                const bool off = monitorDisplayOff_ || consoleDisplayOff_ || sessionDisplayOff_;
                if (displayOff_ != off) {
                    displayOff_ = off;
                    accumulator_ = 0;
                    if (!off) { lastFrameAt_ = Clock::now(); nextFrameAt_ = lastFrameAt_; renderDirty_ = true; }
                    LogEvent(std::string("display_power_off=") + (off ? "true" : "false"));
                }
            }
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) { sessionLocked_ = true; accumulator_ = 0; LogEvent("session_locked"); }
        else if (wParam == WTS_SESSION_UNLOCK) { sessionLocked_ = false; accumulator_ = 0; lastFrameAt_ = Clock::now(); nextFrameAt_ = lastFrameAt_; nextInputDesktopCheck_ = lastFrameAt_; renderDirty_ = true; LogEvent("session_unlocked"); }
        return 0;
    case WM_SHOWWINDOW:
        renderDirty_ = true;
        accumulator_ = 0;
        lastFrameAt_ = Clock::now();
        nextFrameAt_ = lastFrameAt_;
        return 0;
    case WM_MOUSEMOVE: {
        if (!desktopMode_) {
            pointerActive_ = true; pointer_.x = GET_X_LPARAM(lParam); pointer_.y = GET_Y_LPARAM(lParam);
            if (!mouseTracking_) { TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0}; TrackMouseEvent(&tracking); mouseTracking_ = true; }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!desktopMode_) pointerActive_ = false;
        mouseTracking_ = false;
        return 0;
    case WM_LBUTTONDOWN:
        if (!desktopMode_) {
            pointerActive_ = true; pointer_.x = GET_X_LPARAM(lParam); pointer_.y = GET_Y_LPARAM(lParam); clickPending_ = true;
        }
        return 0;
    case WM_KEYDOWN:
        if ((lParam & (1ll << 30)) != 0) return 0;
        if (!desktopMode_) {
            if (wParam == VK_SPACE) { SetPaused(!paused_); return 0; }
            if (wParam == VK_F11) { SetPreview(!previewFullScreen_); return 0; }
            if (wParam == 'S') { ShowSettings(); return 0; }
            if (wParam == VK_ESCAPE) {
                if (previewFullScreen_) SetPreview(false);
                else { ShowWindow(hwnd_, SW_HIDE); UpdateTray(); }
                return 0;
            }
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_CLOSE:
        if (desktopMode_) return 0;
        ShowWindow(hwnd_, SW_HIDE); UpdateTray(); return 0;
    case TrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) ShowTrayMenu();
        else if (lParam == WM_LBUTTONUP || lParam == NIN_SELECT || lParam == NIN_KEYSELECT) ShowSettings();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IdTraySettings: ShowSettings(); return 0;
        case IdTrayPause: SetPaused(!paused_); return 0;
        case IdTrayPreview: SetPreview(true); return 0;
        case IdTrayDesktop: SetDesktop(true); return 0;
        case IdTrayQuit: RequestQuit(); return 0;
        default: break;
        }
        break;
    case WM_APP + 14:
        RequestQuit(); return 0;
    case WM_DESTROY:
        if (desktopMode_ && hadDesktopParent_ && !quitting_) {
            rendererReady_ = false;
            renderer_.reset();
            hwnd_ = nullptr;
            hadDesktopParent_ = false;
            desktopParent_ = nullptr;
            minimized_ = false;
            return 0;
        }
        if (hadDesktopParent_) DetachDesktop();
        rendererReady_ = false;
        renderer_.reset();
        if (!quitting_) running_ = false;
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

int App::run() {
    startedAt_ = Clock::now();
    UpdateMonitorList();
    LoadConfiguration();
    if (options_.monitorSpecified) {
        selectedMonitor_ = options_.monitor;
        if (!options_.spanSpecified) span_ = false;
    }
    if (options_.spanSpecified) span_ = options_.span;
    selectedMonitor_ = std::clamp(selectedMonitor_, 0, std::max(0, static_cast<int>(monitors_.size()) - 1));
    if (options_.count) settings_.count = *options_.count;
    noSave_ = options_.noSettings || options_.smoke;

    WNDCLASSW wc{};
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = MainClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        ShowError(nullptr, L"窗口类注册失败", LastErrorText());
        return 1;
    }
    if (!CreateKeeperWindow()) {
        ShowError(nullptr, L"应用宿主初始化失败", L"无法创建隐藏的 Win32 生命周期窗口或注册 Explorer 通知：\n" + LastErrorText());
        return 1;
    }
    if (!CreateRenderWindow()) {
        ShowError(keeper_, L"渲染窗口创建失败", LastErrorText());
        RequestQuit();
        return 1;
    }

    frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    bool highResolutionTimer = frameTimer_ != nullptr;
    if (!frameTimer_) frameTimer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (!frameTimer_) LogEvent("waitable_timer_unavailable; absolute-deadline MsgWait fallback enabled");

    if (options_.wallpaper) {
        if (!BeginDesktopMount(false)) {
            PlacePreviewWindow(true);
            ShowError(hwnd_, L"桌面挂载失败", L"无法确认安全的 WorkerW 桌面宿主；应用已回退到预览模式。可从托盘稍后重试。");
        }
    } else PlacePreviewWindow(true);

    if (!InitializeRenderer()) {
        RequestQuit();
        return 2;
    }
    CreateTrayIcon();
    if (!trayAdded_) ShowError(hwnd_, L"系统托盘不可用", L"无法创建通知区域图标。应用仍可通过窗口的键盘操作或任务管理器退出。");
    if (!OpenLog()) {
        RequestQuit();
        return 2;
    }
    wtsRegistered_ = WTSRegisterSessionNotification(keeper_, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (!wtsRegistered_) LogEvent("session_notification_registration_failed=" + ToUtf8(LastErrorText()));
    monitorPowerNotify_ = RegisterPowerSettingNotification(keeper_, &GuidMonitorPowerOn, DEVICE_NOTIFY_WINDOW_HANDLE);
    consoleDisplayNotify_ = RegisterPowerSettingNotification(keeper_, &GuidConsoleDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    sessionDisplayNotify_ = RegisterPowerSettingNotification(keeper_, &GuidSessionDisplayStatus, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!monitorPowerNotify_ || !consoleDisplayNotify_ || !sessionDisplayNotify_)
        LogEvent("power_display_notification_registration_partial_failure=" + ToUtf8(LastErrorText()));
    LogEvent(std::string("wait_timer=") + (highResolutionTimer ? "high_resolution" : frameTimer_ ? "standard" : "message_deadline_fallback"));
    LogEvent("desktop_mode=" + std::string(desktopMode_ ? "true" : "false"));

    if (options_.seconds || options_.screenshot) {
        shutdownScheduled_ = true;
        const double delaySeconds = options_.seconds.value_or(3.0);
        shutdownAt_ = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(delaySeconds));
    }
    if (options_.smoke) {
        preSmokeSettings_ = settings_;
        preSmokeDesktop_ = desktopMode_;
        preSmokeFullscreen_ = previewFullScreen_;
        std::error_code ec;
        const auto tempDirectory = std::filesystem::temp_directory_path(ec);
        if (ec) {
            smokeFailed_ = true;
            LogEvent("smoke_temp_directory_failure_code=" + std::to_string(ec.value()));
        } else {
            const auto uniqueId = std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(Clock::now().time_since_epoch().count());
            smokeDirectory_ = tempDirectory / (L"ReynoldsBoids-smoke-" + uniqueId);
            std::filesystem::create_directories(smokeDirectory_, ec);
            if (ec) {
                smokeFailed_ = true;
                LogEvent("smoke_output_directory_failure_code=" + std::to_string(ec.value()));
                smokeDirectory_.clear();
            }
        }
        nextSmokeAt_ = Clock::now() + std::chrono::milliseconds(250);
        LogEvent("smoke_started");
    }
    if (options_.screenshot) {
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
        renderDirty_ = true;
    }
    lastFrameAt_ = Clock::now();
    nextFrameAt_ = lastFrameAt_;
    nextStatusAt_ = lastFrameAt_;
    nextCoverageCheck_ = lastFrameAt_;
    nextInputDesktopCheck_ = lastFrameAt_;
    wasRenderable_ = false;

    while (running_) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                running_ = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (!running_) break;
        }
        if (!running_) break;

        const auto now = Clock::now();
        if (now >= nextInputDesktopCheck_) {
            UpdateInputDesktopState();
            nextInputDesktopCheck_ = now + std::chrono::seconds(1);
        }
        if (desktopMode_ && now >= nextCoverageCheck_) {
            UpdateCoveragePause();
            nextCoverageCheck_ = now + std::chrono::seconds(1);
        } else if (!desktopMode_) {
            coveragePaused_ = false;
        }
        UpdateAutomaticPause();
        if (options_.smoke && !inputDesktopActive_) {
            smokeFailed_ = true;
            LogEvent("smoke_blocked=inactive_desktop; no dynamic or input pass claimed");
            WriteCommandLineError(L"原生窗口检查无法在非活动输入桌面执行。请退出屏保或解锁，回到普通桌面后重试；没有切换或绕过系统桌面。");
            RequestQuit();
            break;
        }
        if (desktopMode_) UpdateDesktopInput();
        if (shutdownScheduled_ && now >= shutdownAt_) {
            shutdownScheduled_ = false;
            if (options_.screenshot) {
                screenshotDeadlineReached_ = true;
                if (minimized_) ShowWindow(hwnd_, SW_RESTORE);
                else if (!IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_SHOW);
                if (!IsRenderable()) {
                    screenshotFailed_ = true;
                    LogEvent("screenshot_unavailable=automatic_pause; bounded exit without a fabricated capture");
                    WriteCommandLineError(L"截图未执行：窗口因屏保、锁屏或其他自动暂停条件不可绘制。请回到普通桌面后重试。");
                    RequestQuit();
                    break;
                }
                lastFrameAt_ = now;
                accumulator_ = 0;
                nextFrameAt_ = now;
                renderDirty_ = true;
                LogEvent("screenshot_capture_deadline_reached");
            } else {
                LogEvent("seconds_deadline_exit");
                RequestQuit();
                break;
            }
        }
        if (options_.smoke && now >= nextSmokeAt_) HandleSmokeDeadline(now);
        if (!running_) break;

        const bool renderable = IsRenderable();
        if (!renderable) {
            if (wasRenderable_) {
                lastFrameAt_ = now;
                accumulator_ = 0;
            }
            wasRenderable_ = false;
        } else if (!wasRenderable_) {
            wasRenderable_ = true;
            lastFrameAt_ = now;
            accumulator_ = 0;
            nextFrameAt_ = now;
            renderDirty_ = true;
        }
        const bool needsFrame = renderable && (!paused_ || renderDirty_);
        if (needsFrame && now >= nextFrameAt_) {
            if (!RenderFrame(now)) break;
            if (!running_) break;
        }

        Clock::time_point deadline = Clock::time_point::max();
        if (renderable && (!paused_ || renderDirty_)) deadline = std::min(deadline, nextFrameAt_);
        if (shutdownScheduled_) deadline = std::min(deadline, shutdownAt_);
        if (options_.smoke) deadline = std::min(deadline, nextSmokeAt_);
        deadline = std::min(deadline, nextInputDesktopCheck_);
        if (desktopMode_) deadline = std::min(deadline, nextCoverageCheck_);

        DWORD fallbackWaitMs = INFINITE;
        bool timerArmed = false;
        if (deadline != Clock::time_point::max()) {
            const auto waitStarted = Clock::now();
            auto delta = deadline - waitStarted;
            if (delta <= Clock::duration::zero()) fallbackWaitMs = 0;
            else {
                auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
                if (milliseconds < delta) ++milliseconds;
                fallbackWaitMs = static_cast<DWORD>(std::clamp<long long>(milliseconds.count(), 0, INFINITE - 1));
                if (frameTimer_) {
                    const auto ticks = std::max<long long>(1, static_cast<long long>(
                        std::ceil(std::chrono::duration<double>(delta).count() * 10000000.0)));
                    LARGE_INTEGER due{};
                    due.QuadPart = -ticks;
                    timerArmed = SetWaitableTimer(frameTimer_, &due, 0, nullptr, nullptr, FALSE) != FALSE;
                }
            }
        }
        if (timerArmed) {
            MsgWaitForMultipleObjectsEx(1, &frameTimer_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        } else {
            if (frameTimer_) CancelWaitableTimer(frameTimer_);
            MsgWaitForMultipleObjectsEx(0, nullptr, fallbackWaitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }

    if (options_.smoke && !smokeSteps_.empty() && log_) {
        for (const std::string& step : smokeSteps_) log_ << "# smoke " << step << '\n';
    }
    RequestQuit();
    if (log_) {
        log_.flush();
        log_.close();
    }
    return screenshotFailed_ || smokeFailed_ || runtimeFailed_ ? 2 : 0;
}

}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CommandLine options;
    std::wstring error;
    if (!ParseCommandLine(options, error)) { WriteCommandLineError(error); return 2; }
    if (options.help) { WriteHelpOutput(); return 0; }
    App app(std::move(options));
    return app.run();
}
