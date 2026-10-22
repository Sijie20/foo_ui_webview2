# P0-7 全屏 / DPI 人工回归

本轮（2026-10-23）只完成源码、隔离单位实验、C++ / Vitest / build；未启动、停止或连接测试台，未运行 accept / soak，未部署生产。下面步骤由 Lead 释放测试台后在隔离实例执行。

## 单位证据与重现

在组件 fork 根目录运行（自建短生命周期窗口、独立 `obj/bounds-unit/profile`，不使用 foobar2000、CDP 或 9222）：

```powershell
.\scripts\run-webview-bounds-unit.ps1
.\scripts\run-webview-bounds-unit.ps1 -Composition
```

要求桌面存在真正的 150% 显示器；无 144 DPI 窗口时实验退出 2，不伪造 scale。采样时运行时 157.0.4341.10，显示器物理矩形 `(0,0,3840,2160)`，`GetDpiForWindow=144`，客户区 900×600。标准 / 合成两种 hosting 均实测了前两个阶段；显式 helper 阶段已在合成 hosting 实测：

| 输入 bounds | 页面 innerWidth × innerHeight | DPR | visualViewport.width |
| --- | --- | --- | --- |
| 原始物理 900×600 | 600×400 | 1.5 | 600 |
| 物理 / scale = 600×400 | 400×267 | 1.5 | 400 |
| 统一 helper + 显式 RAW_PIXELS + scale 同步，900×600 | 600×400 | 1.5 | 600 |

默认及 RAW_PIXELS 模式使用物理 px。不能把物理客户区再除以 DPI；controller rect 与 DComp clip 分开保存，即使当前数值相同。探针三个阶段分别验证输出，失败返回非零。后续标准 hosting 三阶段复跑时桌面已变成 `(0,0,2880,1800)` / 192 DPI，探针拒绝采样；没有调整系统设置，标准 hosting 的新增 helper 阶段留待 150% 环境恢复后补验。

## 三档 DPI、最大化与显示器切换

1. 使用独立实例和本轮组件 / 主题构建。准备 100%（96 DPI）、125%（120 DPI）、150%（144 DPI）显示器；单屏则依次调整系统缩放，等待 DPI 消息完成。记录 Windows 缩放、运行时、组件版本及截图时间。
2. 每档依次做：普通窗口 → 最大化 → 还原 → 打开 NPFS → 进入宿主全屏 → 切换黑胶 → 切换歌词 → 退出全屏。每一步等 resize 稳定，截图包含页面右边界、搜索框和退出按钮，命名 `p0-7-dpi-{100|125|150}-{normal|maximized|restored|fullscreen|vinyl|exit}.png`。
3. 每一步读取下面对象，保留原始 JSON，勿用 DOM 倒推客户区物理宽度：

```js
await (async () => {
  const native = await window.fb2k.invoke('window.getBounds', {});
  const root = document.documentElement;
  return {
    native, // clientWidth/clientHeight = 原生 GetClientRect；dpi = GetDpiForWindow
    innerWidth, innerHeight, devicePixelRatio,
    visualViewportWidth: visualViewport.width,
    clientWidth: root.clientWidth, scrollWidth: root.scrollWidth,
    widthDelta: Math.round(innerWidth * devicePixelRatio) - native.clientWidth,
    heightDelta: Math.round(innerHeight * devicePixelRatio) - native.clientHeight,
    npfs: document.querySelector('[data-npfs]')?.dataset.viewport,
  };
})();
```

要求宽高差绝对值 ≤1 物理 px；`visualViewport.width` 与 `innerWidth` 差 ≤1 CSS px；`scrollWidth <= clientWidth` 且 `scrollWidth <= innerWidth`。右侧搜索框、控制按钮必须可点。旧组件缺少独立客户区字段时标为缺证，不得标通过。

4. 普通 / 最大化 / 全屏各跨 100%→125%→150%→100% 移动，重复采样。检查按 WM_DPICHANGED 建议矩形更新后没有旧视口；CUI 与 DUI 各做一次跨屏及父窗口布局调整，DUI 编辑模式须保持 WebView 隐藏。
5. 外接显示器位于主屏左边或上方（含负坐标）时，在外屏进入全屏。拔插 HDMI、关闭再打开外屏、改分辨率、改缩放、切换主屏和任务栏工作区；每次窗口应在当前最近有效显示器内，可点击、聚焦、Alt-Tab 切出及切回，不能在外部应用上强制置顶。
6. 退出全屏后，显示器仍在时恢复原窗口矩形 / 原显示器及原最大化状态；原显示器消失或分辨率缩小时，恢复矩形应 clamp 到最近有效工作区，标题栏关闭位置及右边界可达。分别从普通窗口和最大化窗口进入，记录进入前与退出后的 `window.getBounds`。

## Esc / 关闭优先级

1. NPFS 在宿主全屏中打开：可见、可 Tab 聚焦的 `data-npfs-action="exit-fullscreen"` 按钮常驻，黑胶 / 歌词模式均保留；点击只退全屏，详情面板与播放继续。
2. Esc 第一次退宿主全屏，NPFS 保留；第二次关 NPFS。长按 / autorepeat 不得连续退出并关面板。子弹层若 `preventDefault()` 消费 Esc，先由子弹层处理，不退出宿主。
3. 普通页面未消费 Esc：原生退出全屏。页面脚本阻塞 / 无响应：原生 750ms 兜底（需宿主 UI 线程可调度）；页面恢复后不得二次切换或进入全屏。
4. 焦点在宿主窗口时也测试 Esc；F11 在全屏中退出；Alt-F4、系统菜单 / 任务栏关闭、主题窗口关闭按钮第一次退全屏，之后按正常关闭策略处理（含关闭到托盘）。关闭过程不停止正在播放的曲目，除非用户第二次确实关闭应用。
5. Alt-Tab 切到另一个应用并操作其关闭按钮，再切回 foobar2000；无抢焦、无鼠标捕获残留。

## 后续自动闸

`tools/window-viewport-probe.mjs` 将浏览器读数与新增原生客户区字段比较；默认 accept 的 `p0-7-viewport` core 闸引用同一判据，soak 的 baseline / 每轮 before / after / final 都取 viewport，违规记 `viewport-mismatch`。只检查语法及纯函数测试不连接测试台；活体运行须等 Lead 释放 9222 并部署本轮组件后进行。

物理显示器拔插、三档 DPI 截图、CUI / DUI 活体、播放连续性及原生 Esc 兜底均尚未实际验收，不能用几何单测或 DOM 单测替代。
