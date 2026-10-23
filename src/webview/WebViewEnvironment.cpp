#include "pch.h"
#include "webview/WebViewEnvironment.h"
#include "webview/WebViewHost.h"
#include "core/WebViewContext.h"
#include "settings/SecurityConfig.h"
#include "prefs/PreferencesPage.h"   // webview_prefs::GetPreheatEnabled / GetCdpPort
#include "utils/PathExpansion.h"
#include "webview/UserDataMigration.h"
#include <Shlobj.h>
#include <chrono>
#include <filesystem>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2EnvironmentOptions.h>

using namespace Microsoft::WRL;

// ============================================
// WebViewEnvironment 实现
// ============================================

WebViewEnvironment& WebViewEnvironment::GetInstance() {
    static WebViewEnvironment instance;
    return instance;
}

// 便携版的用户数据目录放在实例自己的 profile 下。同一个用户数据目录由同一组浏览器
// 进程服务：几个实例共用一个目录时，后开的那个要么因为环境选项不同（比如启动参数里的
// CDP 端口）创建环境失败，要么页面跑在先开的实例的浏览器进程里、随它退出一起没了。
// 安装版仍用 %LOCALAPPDATA%：它的 profile 在漫游的 %APPDATA% 下，缓存不宜跟着漫游，
// 而且一个 Windows 用户只有一份安装版 profile。
namespace {

// 安装版的用户数据目录，也是便携版以前共用的那个；取不到时为空串
std::wstring SharedUserDataPath() {
    wchar_t path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) return L"";
    return std::wstring(path) + L"\\foobar2000\\foo_ui_webview2";
}

// 便携版自己的用户数据目录；不是便携版或取不到 profile 时为空串
std::wstring PortableUserDataPath() {
    if (!core_api::is_portable_mode_enabled()) return L"";
    // 带结尾反斜杠
    const std::wstring profileDir = PathExpansion::GetProfileDirectory();
    return profileDir.size() > 1 ? profileDir + L"foo_ui_webview2" : L"";
}

// 共用目录被占着时最多等这么久再放弃迁移。只在首次迁移时等，之后目标目录已存在。
constexpr auto kSharedFolderReleaseWait = std::chrono::seconds(3);

}  // namespace

std::wstring WebViewEnvironment::GetUserDataPath() {
    std::wstring userDataPath = PortableUserDataPath();
    if (userDataPath.empty()) {
        userDataPath = SharedUserDataPath();
        if (userDataPath.empty()) return L"";
        // 已存在是常态，返回值不看
        (void)CreateDirectoryW(std::filesystem::path(userDataPath).parent_path().c_str(), nullptr);
    }
    (void)CreateDirectoryW(userDataPath.c_str(), nullptr);
    return userDataPath;
}

// 便携版第一次用自己的目录时，把共用目录里的数据复制过来，见 UserDataMigration.h。
// 自己的目录一旦存在就不再迁移；共用目录被占用时就不迁移，并在控制台写明手动做法。
void WebViewEnvironment::MigrateSharedUserData() {
    namespace fs = std::filesystem;
    const fs::path target = PortableUserDataPath();
    std::error_code ec;
    if (target.empty() || fs::exists(target, ec)) return;
    const fs::path source = SharedUserDataPath();
    if (source.empty() || !fs::is_directory(source, ec)) return;

    const auto utf8 = [](const fs::path& path) {
        return std::string(pfc::stringcvt::string_utf8_from_wide(path.c_str()).get_ptr());
    };
    if (webview_udf::WaitWhileInUse(source, kSharedFolderReleaseWait)) {
        FB2K_console_formatter()
            << "[WebView2 UI] Web storage was not copied from " << utf8(source).c_str()
            << ": another foobar2000 instance is using it. This instance starts with empty web"
               " storage in " << utf8(target).c_str()
            << ". To copy it later, close every foobar2000 instance, delete that folder and start"
               " this instance again; whatever pages saved there meanwhile is lost.";
        return;
    }

    const auto started = std::chrono::steady_clock::now();
    std::wstring error;
    const webview_udf::CopyOutcome outcome = webview_udf::CopyUserDataFolder(source, target, &error);
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    if (outcome == webview_udf::CopyOutcome::Copied) {
        FB2K_console_formatter()
            << "[WebView2 UI] Copied web storage from " << utf8(source).c_str() << " to "
            << utf8(target).c_str() << " in " << static_cast<int>(elapsedMs)
            << " ms; the original folder is kept.";
    } else if (outcome == webview_udf::CopyOutcome::Failed) {
        FB2K_console_formatter()
            << "[WebView2 UI] Could not copy web storage from " << utf8(source).c_str()
            << " (" << utf8(error).c_str() << "); this instance starts with empty web storage.";
    }
}

bool WebViewEnvironment::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lifecycle_.ready && SUCCEEDED(lifecycle_.AccessStatus());
}

void WebViewEnvironment::RegisterHost(WebViewHost* host) {
    std::lock_guard<std::mutex> lock(mutex_);
    hosts_.insert(host);
}

void WebViewEnvironment::UnregisterHost(WebViewHost* host) {
    std::lock_guard<std::mutex> lock(mutex_);
    hosts_.erase(host);
}

void WebViewEnvironment::TrackBrowserProcess(DWORD pid) {
    if (!pid) return;
    wil::unique_handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
    if (!process) return;
    std::lock_guard<std::mutex> lock(mutex_);
    browserProcesses_.insert_or_assign(pid, std::move(process));
}

void WebViewEnvironment::Preheat() {
    (void)GetEnvironment({});
}

HRESULT WebViewEnvironment::GetEnvironment(const EnvironmentCallback& callback) {
    if (!core_api::is_main_thread()) return RPC_E_WRONG_THREAD;
    bool recycle = false;
    HRESULT status;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status = lifecycle_.AccessStatus();
        recycle = SUCCEEDED(status) && lifecycle_.invalidated;
    }
    if (recycle) ReleaseEnvironment(false);

    wil::com_ptr<ICoreWebView2Environment> snapshot;
    bool create = false;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status = lifecycle_.GetEnvironment(environment_, snapshot);
        if (status == E_PENDING) {
            if (callback) pendingCallbacks_.push_back(callback);
            if (!lifecycle_.creating) {
                lifecycle_.creating = true;
                create = true;
            }
            generation = lifecycle_.generation;
        }
    }
    if (FAILED(status) && status != E_PENDING) {
        if (callback) callback(nullptr);
        return status;
    }
    if (snapshot) {
        if (callback) callback(snapshot.get());
    } else if (create) {
        CreateEnvironmentInternal(generation);
    }
    return S_OK;
}

void WebViewEnvironment::CreateEnvironmentInternal(std::uint64_t generation) {
    // Note: lifecycle_.creating flag should already be set by caller
    
    // 必须在 GetUserDataPath 创建目录之前：迁移以目标目录不存在为前提
    MigrateSharedUserData();
    auto userDataPath = GetUserDataPath();
    
    // Check WebView2 Runtime availability
    wil::unique_cotaskmem_string version;
    HRESULT checkHr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &version);
    if (FAILED(checkHr) || !version) {
        console::print("[WebView2 UI] WebView2 Runtime not available");
        DrainPendingCallbacksOnFailure(generation);
        return;
    }
    
    FB2K_console_formatter() << "[WebView2 UI] WebView2 Runtime version: " 
                             << pfc::stringcvt::string_utf8_from_wide(version.get()).get_ptr();
    
    // Create environment options with custom schemes
    auto envOptions = Make<CoreWebView2EnvironmentOptions>();
    
    // Register fb2k:// and artwork:// protocols
    auto schemeRegistrationFb2k = Make<CoreWebView2CustomSchemeRegistration>(L"fb2k");
    schemeRegistrationFb2k->put_TreatAsSecure(TRUE);
    schemeRegistrationFb2k->put_HasAuthorityComponent(TRUE);
    LPCWSTR allowedOriginsFb2k[] = { L"*" };
    schemeRegistrationFb2k->SetAllowedOrigins(1, allowedOriginsFb2k);
    
    auto schemeRegistrationArtwork = Make<CoreWebView2CustomSchemeRegistration>(L"artwork");
    schemeRegistrationArtwork->put_TreatAsSecure(TRUE);
    schemeRegistrationArtwork->put_HasAuthorityComponent(FALSE);
    LPCWSTR allowedOriginsArtwork[] = { L"*" };
    schemeRegistrationArtwork->SetAllowedOrigins(1, allowedOriginsArtwork);
    
    ICoreWebView2CustomSchemeRegistration* schemeRegistrations[] = {
        schemeRegistrationFb2k.Get(),
        schemeRegistrationArtwork.Get()
    };
    envOptions->SetCustomSchemeRegistrations(2, schemeRegistrations);

    // ============================================
    // 环境选项减法（关闭运行期纯开销特性）
    // SDK 辅助类 CoreWebView2EnvironmentOptions 已实现 Options1-8 全链，
    // QI 恒成功；保留 SUCCEEDED 判空仅为防御（勿改用 WRL ComPtr::As 搭配
    // wil::com_ptr，模板不匹配）。
    // ============================================

    // Options5: 关闭跟踪防护 — 本项目导航面受控（fb2k://、虚拟主机、dev server），
    // 过滤引擎对每个资源请求都是纯开销（官方文档明确关闭可提升运行时性能）
    wil::com_ptr<ICoreWebView2EnvironmentOptions5> options5;
    if (SUCCEEDED(envOptions->QueryInterface(IID_PPV_ARGS(&options5)))) {
        options5->put_EnableTrackingPrevention(FALSE);
    }

    // Options3: 崩溃转储仅本地保留（用户数据目录 CrashDumps），不向微软上报，
    // 消除崩溃上报的后台 IO/网络开销；本地转储仍可配合 webview_crash.log 诊断
    wil::com_ptr<ICoreWebView2EnvironmentOptions3> options3;
    if (SUCCEEDED(envOptions->QueryInterface(IID_PPV_ARGS(&options3)))) {
        options3->put_IsCustomCrashReportingEnabled(TRUE);
    }
    
    // ============================================
    // 浏览器命令行参数
    // ============================================
    // Visual Hosting (CompositionController + DirectComposition) 模式下，
    // Chromium 的 native window occlusion 计算会在窗口最小化/被完全遮挡时
    // 把 WebView 判定为 occluded 并挂起呈现；恢复时在 Composition 模式下
    // 运行时不一定能可靠收到“重新可见”信号，导致 DComp 表面停留空白
    // （现象：最小化→恢复后只剩空 Win32 窗口壳，且不触发 ProcessFailed）。
    // 关闭该特性可让 WebView 始终保持呈现，规避恢复空白。
    // 代价：窗口最小化时后台仍持续渲染，CPU/GPU 占用略增（音乐播放器常驻后台，可接受）。
    // --disable-features 必须保持单一 switch（逗号分隔值）：WebView2 对重复
    // switch 的语义是 last-instance-wins、仅 features 类做 union 合并，
    // 为规避运行时版本差异，一律在此处自行合并。
    //
    // SpareRendererForSitePerProcess: 防御性禁用 Chromium "备用渲染进程"预热。
    // 本组件是单源单页应用，备用 renderer 纯属内存浪费；本机基线未观测到该进程
    // （官方口径 only consulted in site-per-process mode），显式禁用以挡住任意
    // Runtime 版本/导航时机下的创建。
    std::wstring disableFeatures = L"CalculateNativeWinOcclusion,SpareRendererForSitePerProcess";
    std::wstring extraArgs;

    // CDP remote debugging port — 仅在 CDP 远程调试开关启用时开放
    // 允许 MCP 工具集 / AI 智能体通过 CDP 操控 WebView2
    if (security_config::IsCdpRemoteEnabled()) {
        // 端口来自 Developer 子页（默认 9222）；读出时已钳到 1024–65535。环境只创建一次，
        // 运行中改端口不影响本进程已开的端口。
        const int cdpPort = webview_prefs::GetCdpPort();
        // CDP 模式追加后台节流禁用，消除 hidden ≥5min 后定时器分钟级对齐的长尾
        // （IntensiveWakeUpThrottling 是 blink feature、没有独立 switch，只能并入
        // --disable-features）。
        extraArgs += L" --remote-debugging-port=" + std::to_wstring(cdpPort) +
                     L" --disable-background-timer-throttling"
                     L" --disable-renderer-backgrounding";
        disableFeatures += L",IntensiveWakeUpThrottling";
        // keep-alive 判定绑定"本进程真实开了端口"的快照，而非配置实时值
        // （运行中勾/取消 CDP 不影响已创建环境的端口状态）。
        security_config::NoteCdpPortOpenedThisProcess();
        FB2K_console_formatter() << "[WebView2 UI] CDP remote debugging enabled on port " << cdpPort;
    }

    std::wstring browserArgs = L"--disable-features=" + disableFeatures + extraArgs;

    envOptions->put_AdditionalBrowserArguments(browserArgs.c_str());
    
    // Create environment
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,                    // Browser executable path
        userDataPath.c_str(),       // User data directory
        envOptions.Get(),           // Environment options
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this, generation](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                std::vector<EnvironmentCallback> callbacksToInvoke;
                wil::com_ptr<ICoreWebView2Environment> envToReturn;
                bool failed = false;
                
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    
                    if (!lifecycle_.AcceptCallback(generation)) return S_OK;
                    if (FAILED(result) || !env) {
                        FB2K_console_formatter() << "[WebView2 UI] Failed to create environment, HRESULT: 0x" 
                                                 << pfc::format_hex((uint32_t)result);
                        // 提取等待者，在锁外以 nullptr 回调
                        callbacksToInvoke = std::move(pendingCallbacks_);
                        pendingCallbacks_.clear();
                        lifecycle_.creating = false;
                        failed = true;
                    } else {
                        environment_ = env;
                        lifecycle_.ready = true;
                        lifecycle_.creating = false;
                        envToReturn = environment_;
                        
                        // Move callbacks to local vector to call outside lock
                        callbacksToInvoke = std::move(pendingCallbacks_);
                        pendingCallbacks_.clear();
                    }
                }
                
                if (failed) {
                    // 通知所有等待者环境创建失败
                    for (auto& cb : callbacksToInvoke) {
                        {
                            std::lock_guard<std::mutex> lock(mutex_);
                            if (!lifecycle_.AcceptCallback(generation)) break;
                        }
                        if (cb) cb(nullptr);
                    }
                    return result;
                }
                
                console::print("[WebView2 UI] WebView2 environment preheated successfully");
                
                // Call all pending callbacks outside lock to avoid deadlock
                for (auto& cb : callbacksToInvoke) {
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (!lifecycle_.AcceptCallback(generation)) break;
                    }
                    if (cb) cb(envToReturn.get());
                }
                
                return S_OK;
            }
        ).Get()
    );
    
    if (FAILED(hr)) {
        FB2K_console_formatter() << "[WebView2 UI] CreateCoreWebView2EnvironmentWithOptions failed: 0x" 
                                 << pfc::format_hex((uint32_t)hr);
        DrainPendingCallbacksOnFailure(generation);
    }
}

void WebViewEnvironment::DrainPendingCallbacksOnFailure(std::uint64_t generation) {
    std::vector<EnvironmentCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!lifecycle_.AcceptCallback(generation)) return;
        lifecycle_.creating = false;
        callbacks.swap(pendingCallbacks_);
    }
    for (auto& cb : callbacks) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!lifecycle_.AcceptCallback(generation)) break;
        }
        if (cb) cb(nullptr);
    }
}

void WebViewEnvironment::Invalidate(const char* reason) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!lifecycle_.Invalidate()) return;
        pendingCallbacks_.clear();
    }
    WebViewHost::WriteLifecycleLog(std::string("event=environment.invalidate reason=") +
        (reason && reason[0] ? reason : "unspecified"));
}

namespace {
// Bound even with a constantly busy queue. Preserve WM_QUIT for the outer loop.
void PumpCleanupMessages(DWORD milliseconds, const std::function<bool()>& complete) {
    const ULONGLONG deadline = GetTickCount64() + milliseconds;
    bool quit = false;
    int quitCode = 0;
    do {
        MSG msg{};
        for (unsigned count = 0; count < 64 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (msg.message == WM_QUIT) {
                quit = true;
                quitCode = static_cast<int>(msg.wParam);
            } else {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (GetTickCount64() >= deadline) break;
        }
        if (complete() || GetTickCount64() >= deadline) break;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (GetTickCount64() < deadline);
    if (quit) PostQuitMessage(quitCode);
}
}

void WebViewEnvironment::ReleaseEnvironment(bool shutdown) {
    if (!core_api::is_main_thread()) {
        WebViewHost::WriteLifecycleLog("event=environment.release.error reason=wrongThread");
        return;
    }
    std::vector<WebViewHost*> hosts;
    wil::com_ptr<ICoreWebView2Environment5> environment5;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (lifecycle_.terminal && !lifecycle_.shuttingDown_) return;
        if (!lifecycle_.BeginRelease(shutdown)) return;
        pendingCallbacks_.clear();
        hosts.assign(hosts_.begin(), hosts_.end());
        if (environment_) environment_.try_query_to(&environment5);
    }
    // WebView2 has no controller Closed event. BrowserProcessExited is the
    // runtime cleanup signal; old runtimes take the bounded timeout path.
    const auto exited = std::make_shared<std::atomic<bool>>(false);
    EventRegistrationToken token{};
    const bool subscribed = environment5 && SUCCEEDED(environment5->add_BrowserProcessExited(
        Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
            [exited](ICoreWebView2Environment*, ICoreWebView2BrowserProcessExitedEventArgs*) -> HRESULT {
                exited->store(true, std::memory_order_release);
                return S_OK;
            }).Get(), &token));
    webview_recycle::ReleaseSequence(
        [&] {
            for (auto* host : hosts) {
                bool registered;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    registered = hosts_.contains(host);
                }
                if (registered) host->CloseForEnvironmentRecycle();
            }
        },
        [&] { PumpCleanupMessages(2000, [&] { return exited->load(std::memory_order_acquire); }); },
        [&] {
            if (subscribed) environment5->remove_BrowserProcessExited(token);
            environment5.reset(); // QI reference must not keep the old environment alive
            wil::com_ptr<ICoreWebView2Environment> old;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                old.swap(environment_);
            }
            old.reset(); // never release COM while holding the state lock
        },
        [&] { PumpCleanupMessages(500, [] { return false; }); });
    std::string pendingPids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lifecycle_.EndRelease();
        for (auto it = browserProcesses_.begin(); it != browserProcesses_.end();) {
            if (WaitForSingleObject(it->second.get(), 0) == WAIT_OBJECT_0) {
                it = browserProcesses_.erase(it);
            } else {
                if (!pendingPids.empty()) pendingPids += ",";
                pendingPids += std::to_string(it->first);
                ++it;
            }
        }
    }
    WebViewHost::WriteLifecycleLog(std::string("event=environment.released envReleased=1 reason=") +
        (shutdown ? "shutdown" : "invalidate") + " browserExited=" +
        (exited->load(std::memory_order_acquire) ? "1" : "0") +
        " waitTimedOut=" + (exited->load(std::memory_order_acquire) ? "0" : "1") +
        " pendingBrowserPids=" + (pendingPids.empty() ? "none" : pendingPids));
}

void WebViewEnvironment::Shutdown() {
    ReleaseEnvironment(true);
}

// ============================================
// initquit 服务 - 在启动时预热环境
// ============================================

class WebViewEnvironmentInitQuit : public initquit {
public:
    void on_init() override {
        // 预热开关在 Performance 子页；关闭时不在这里创建环境，首个 GetEnvironment() 调用会按需创建。
        // 开关改动只影响下一次启动。
        if (!webview_prefs::GetPreheatEnabled()) {
            console::print("[WebView2 UI] WebView2 environment preheat disabled; it will be created on first use");
            return;
        }
        // Use main thread callback to ensure proper COM context
        fb2k::inMainThread([]() {
            WebViewEnvironment::GetInstance().Preheat();
        });
    }
    
    void on_quit() override {
        // 显式释放 WebView2 环境的全局 COM 引用
        // 此时所有 Controller 已由 user_interface::shutdown() 关闭
        // 释放环境引用后 msedgewebview2.exe 子进程将自动退出
        WebViewEnvironment::GetInstance().Shutdown();
    }
};

// Register with high priority to start early
static initquit_factory_t<WebViewEnvironmentInitQuit> g_webview_env_factory;

void InitWebViewEnvironment() {
    WebViewEnvironment::GetInstance().Preheat();
}

void ShutdownWebViewEnvironment() {
    WebViewEnvironment::GetInstance().Shutdown();
}
