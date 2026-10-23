#pragma once
#include "pch.h"
#include <wil/com.h>
#include <WebView2.h>
#include <functional>
#include <mutex>
#include <set>
#include <map>
#include "webview/EnvironmentRecycleState.h"

class WebViewHost;

// ============================================
// WebViewEnvironment - WebView2 环境预热与共享
// 
// 优化策略：
// 1. 在 foobar2000 启动时预创建 WebView2 环境
// 2. 所有面板/窗口共享同一个环境实例
// 3. 大幅减少后续 WebView 创建时间
// ============================================

class WebViewEnvironment {
public:
    static WebViewEnvironment& GetInstance();
    
    // 禁止拷贝
    WebViewEnvironment(const WebViewEnvironment&) = delete;
    WebViewEnvironment& operator=(const WebViewEnvironment&) = delete;
    
    // 预热环境（在 foobar2000 启动时调用）
    // 这个函数会异步创建 WebView2 环境
    void Preheat();
    
    // 释放全局 COM 引用并重置就绪状态（在 foobar2000 关闭时调用）
    // UI/STA thread only. Terminal: subsequent GetEnvironment returns an error.
    void Shutdown();
    
    // 作废缓存环境（WebView2 浏览器进程崩溃退出时调用）。
    // 浏览器进程退出后 environment_ COM 指针仍非空，但它绑定的进程已死，
    // 下一次 GetEnvironment 在创建前关闭旧 host、泵消息并释放旧环境。
    void Invalidate(const char* reason);
    
    // 获取共享环境（如果未就绪则创建）
    // callback 会在环境就绪时调用
    using EnvironmentCallback = std::function<void(ICoreWebView2Environment*)>;
    HRESULT GetEnvironment(const EnvironmentCallback& callback);
    
    // 环境是否已就绪
    bool IsReady() const;
    void RegisterHost(WebViewHost* host);
    void UnregisterHost(WebViewHost* host);
    void TrackBrowserProcess(DWORD pid);
    
    // WebView2 用户数据目录，不存在时创建：便携版是 <profile>\foo_ui_webview2，
    // 安装版是 %LOCALAPPDATA%\foobar2000\foo_ui_webview2。都取不到时为空串，
    // 由 WebView2 用它的缺省位置。
    static std::wstring GetUserDataPath();

private:
    WebViewEnvironment() = default;

    // 便携版首次使用自己的目录时从共用目录复制网页数据；在主线程、创建环境之前调用
    static void MigrateSharedUserData();
    
    // Internal method - assumes creating_ flag is already set
    void CreateEnvironmentInternal(std::uint64_t generation);
    void ReleaseEnvironment(bool shutdown);
    
    // 失败时 drain 等待队列并以 nullptr 回调
    void DrainPendingCallbacksOnFailure(std::uint64_t generation);
    
    wil::com_ptr<ICoreWebView2Environment> environment_;
    mutable std::mutex mutex_;
    webview_recycle::State lifecycle_;
    std::set<WebViewHost*> hosts_;
    // Handles distinguish the original process from later PID reuse.
    std::map<DWORD, wil::unique_handle> browserProcesses_;
    
    // 等待环境就绪的回调队列
    std::vector<EnvironmentCallback> pendingCallbacks_;
};

// ============================================
// initquit 服务声明 - 在启动时预热环境
// ============================================
void InitWebViewEnvironment();
void ShutdownWebViewEnvironment();
