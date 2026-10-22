// Standalone experiment: no foobar2000 instance, CDP port or test bench.
// Build with scripts/run-webview-bounds-unit.ps1. Temporary profile lives in obj/.
#define NOMINMAX
#include <windows.h>
#include <shellscalingapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <dcomp.h>
#include <cstdio>
#include <string>
#include "../../src/window/FullscreenGeometry.h"
using namespace Microsoft::WRL;
static HWND host;
static ComPtr<ICoreWebView2Controller> controller;
static ComPtr<ICoreWebView2> page;
static int phase = 0;
static bool failed = false;
static bool composition = false;
static ComPtr<IDCompositionDevice> device;
static ComPtr<IDCompositionTarget> target;
static ComPtr<IDCompositionVisual> visual;
static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (msg == WM_TIMER && wp == 2) { failed=true; std::puts("FAIL: experiment timed out"); DestroyWindow(hwnd); return 0; }
    if (msg == WM_TIMER && page) {
        KillTimer(hwnd, 1);
        page->ExecuteScript(L"JSON.stringify({innerWidth,innerHeight,dpr:devicePixelRatio,visualWidth:visualViewport.width,clientWidth:document.documentElement.clientWidth,scrollWidth:document.documentElement.scrollWidth})",
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>([](HRESULT hr, LPCWSTR json) -> HRESULT {
                RECT client{}, bounds{}; GetClientRect(host, &client); controller->get_Bounds(&bounds);
                std::wprintf(L"phase=%d dpi=%u client=%ldx%ld bounds=%ldx%ld hr=%08lx page=%s\n", phase, GetDpiForWindow(host), client.right, client.bottom, bounds.right, bounds.bottom, hr, json);
                const wchar_t* expectedWidth = phase == 1 ? L"innerWidth\\\":400" : L"innerWidth\\\":600";
                if (FAILED(hr) || !json || !wcsstr(json, expectedWidth) || !wcsstr(json,L"dpr\\\":1.5")) failed = true;
                if (phase++ == 0) {
                    const UINT dpi = GetDpiForWindow(host);
                    RECT divided{0,0,MulDiv(client.right,96,dpi),MulDiv(client.bottom,96,dpi)};
                    controller->put_Bounds(divided); SetTimer(host,1,800,nullptr);
                } else if (phase == 2) {
                    ComPtr<ICoreWebView2Controller3> c3;
                    controller.As(&c3);
                    if (!c3) { failed=true; controller->Close(); DestroyWindow(host); return S_OK; }
                    c3->put_BoundsMode(COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
                    c3->put_ShouldDetectMonitorScaleChanges(TRUE);
                    c3->put_RasterizationScale(GetDpiForWindow(host)/96.0);
                    const auto converted = fullscreen_geometry::ControllerBoundsFromPhysical(
                        {client.left,client.top,client.right,client.bottom},GetDpiForWindow(host));
                    RECT physical{converted.left,converted.top,converted.right,converted.bottom};
                    controller->put_Bounds(physical); SetTimer(host,1,800,nullptr);
                } else { controller->Close(); DestroyWindow(host); }
                return S_OK;
            }).Get());
        return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int wmain(int argc, wchar_t** argv) {
    composition = argc > 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    LPWSTR version = nullptr;
    if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version))) {
        std::wprintf(L"runtime=%s hosting=%s\n", version, composition ? L"composition" : L"windowed");
        CoTaskMemFree(version);
    }
    POINT origin{100,100};
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR mon,HDC,LPRECT,LPARAM arg) -> BOOL {
        UINT x=0,y=0; GetDpiForMonitor(mon,MDT_EFFECTIVE_DPI,&x,&y);
        MONITORINFO mi{sizeof(mi)}; GetMonitorInfo(mon,&mi);
        std::printf("monitor=(%ld,%ld,%ld,%ld) dpi=%u\n",mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right,mi.rcMonitor.bottom,x);
        if(x==144) { auto* p=reinterpret_cast<POINT*>(arg); *p={mi.rcWork.left+40,mi.rcWork.top+40}; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&origin));
    WNDCLASSW wc{}; wc.lpfnWndProc=Proc; wc.hInstance=GetModuleHandle(nullptr); wc.lpszClassName=L"SolBoundsUnitProbe"; RegisterClassW(&wc);
    host=CreateWindowExW(0,wc.lpszClassName,L"Sol isolated WebView2 bounds unit experiment",WS_OVERLAPPEDWINDOW,origin.x,origin.y,1000,700,nullptr,nullptr,wc.hInstance,nullptr);
    if(GetDpiForWindow(host)!=144) { std::puts("FAIL: no 150% DPI window; experiment not valid"); return 2; }
    RECT size{0,0,900,600}; AdjustWindowRectExForDpi(&size,WS_OVERLAPPEDWINDOW,FALSE,0,144);
    SetWindowPos(host,nullptr,0,0,size.right-size.left,size.bottom-size.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    ShowWindow(host,SW_SHOWNOACTIVATE);
    SetTimer(host,2,30000,nullptr);
    HRESULT hr=CreateCoreWebView2EnvironmentWithOptions(nullptr,argc>1?argv[1]:L"obj\\bounds-unit-profile",nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT hr,ICoreWebView2Environment* env) -> HRESULT {
            if(FAILED(hr)||!env) { failed=true; DestroyWindow(host); return S_OK; }
            auto handler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([](HRESULT hr,ICoreWebView2Controller* c) -> HRESULT {
                if(FAILED(hr)||!c) { failed=true; DestroyWindow(host); return S_OK; }
                controller=c; c->get_CoreWebView2(&page); c->put_ZoomFactor(1.0);
                RECT client{}; GetClientRect(host,&client); c->put_Bounds(client);
                EventRegistrationToken token{};
                page->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>([](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT { SetTimer(host,1,800,nullptr); return S_OK; }).Get(),&token);
                page->NavigateToString(L"<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'><style>body{margin:0}</style><p>Isolated 150% bounds experiment</p>");
                return S_OK;
            });
            if (!composition) return env->CreateCoreWebView2Controller(host, handler.Get());
            ComPtr<ICoreWebView2Environment3> env3;
            if (FAILED(env->QueryInterface(IID_PPV_ARGS(&env3))) ||
                FAILED(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&device))) ||
                FAILED(device->CreateTargetForHwnd(host, TRUE, &target)) ||
                FAILED(device->CreateVisual(&visual))) { failed=true; DestroyWindow(host); return S_OK; }
            target->SetRoot(visual.Get()); device->Commit();
            return env3->CreateCoreWebView2CompositionController(host,
                Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                    [handler](HRESULT hr, ICoreWebView2CompositionController* c) -> HRESULT {
                        ComPtr<ICoreWebView2Controller> base;
                        if (SUCCEEDED(hr) && c) {
                            hr=c->QueryInterface(IID_PPV_ARGS(&base));
                            c->put_RootVisualTarget(visual.Get()); device->Commit();
                        }
                        return handler->Invoke(hr,base.Get());
                    }).Get());
        }).Get());
    if(FAILED(hr)) return 3;
    MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    page.Reset(); controller.Reset(); CoUninitialize(); return failed?1:0;
}
