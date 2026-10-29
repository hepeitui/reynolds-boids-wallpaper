# Reynolds Boids · C++ 原生核心与 WebAssembly 桥接源码

本目录包含 Reynolds Boids（雷诺兹鸟群）模拟算法的核心 C++20 实现，以及用于构建 Wallpaper Engine WebAssembly 单文件模块和 Windows 独立原生桌面壁纸的工程源码。

---

## 源码地图

| 路径 | 模块职责 |
| :--- | :--- |
| `model.hpp` | 坐标/速度、设置、鸟/历史/残影的共享数据契约 |
| `simulation.hpp` / `simulation.cpp` | 完整半径空间哈希、Reynolds三项群飞力、触边死亡、进出场、点击增速、历史与内部紧凑近邻缓存 |
| `renderer.hpp` / `renderer.cpp` | Windows 原生 Direct3D 11 实例化渲染管线（30 顶点双翼网格、拖尾分段光栅化、设备上下文与 GPU 截图） |
| `main.cpp` | Windows 独立桌面壁纸宿主：Win32 窗口生命周期、WorkerW 桌面嵌入、系统托盘管理及命令行参数解析 |
| `verify.cpp` | 确定性物理与数值不变式自动化验证套件（暴力真值对照、哈希网格碰撞、生命周期边界检查） |
| `benchmark.cpp` | 确定性 CPU 模拟负载基准测试工具 |
| `build.ps1` | Windows 原生 Direct3D 11 壁纸及验证工具构建脚本（基于 MinGW-w64 GCC C++20） |
| `build-web.ps1` | Emscripten WebAssembly 跨平台单文件构建脚本（将核心物理编译为内联 `boids-wasm.js`） |
| `build-capacity.ps1` / `run-capacity.ps1` | 高数量负载容量测试与副屏自动化评测脚本 |
| `analyze-capacity.py` / `test_capacity_analysis.py` | 容量测试指标统计与断言检验工具 |
| `web/wasm_bridge.cpp` | C API与实例平铺内存指针；JS可通过TypedArray视图读取而不做JSON序列化，向GPU上传仍有传输成本 |

---

## 构建指南

### 1. 构建 WebAssembly 模块 (`boids-wasm.js`)

WebAssembly 内联在单文件胶水脚本中，不发起额外 `.wasm` 请求；本次Emscripten 6.0.13直接嵌入二进制字节，已实际验证本地文件加载。

- **默认构建（输出至仓库根目录）**：
  在仓库根目录直接执行：
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\native\build-web.ps1
  ```
- **自定义输出目录或编译器路径**：
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\native\build-web.ps1 -OutputDirectory "." -EmscriptenPath "C:\path\to\emsdk\upstream\emscripten"
  ```

构建要求：已安装 [Emscripten SDK](https://emscripten.org/)（可通过 `emsdk_env.bat` 激活环境或通过 `-EmscriptenPath` 指定路径）。

---

### 2. 构建 Windows 原生 Direct3D 11 壁纸

原生版不依赖浏览器或 Wallpaper Engine，可作为独立的 Windows 桌面背景进程运行。

- **构建独立应用程序**：
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\native\build.ps1
  ```
- **构建应用程序及验证工具并执行自检**：
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\native\build.ps1 -Tools -Verify
  ```

构建要求：支持 C++20 的 MinGW-w64 GCC（如 `g++.exe`），Windows SDK 兼容系统头文件及 Direct3D 11 导入库。

---

## 核心设计特性与约束

1. **同构动力学与网格模型**：
   - WebAssembly 与原生 Windows 版本共享严格一致的物理动力学规则（58 px 感知半径、15 px 分离半径、40 px 空间哈希网格）。
   - 鸟体均使用 30 顶点双翼分面网格与分段实例化拖尾技术。
2. **纯串行 WebAssembly**：
   - WebAssembly 构建强制单线程运行（不使用 `SharedArrayBuffer` 与 `pthread`），避免现代浏览器和 Chromium/CEF 的跨源隔离（COOP/COEP）限制，在本地 `file://` 环境下即可直接运行。
3. **紧凑近邻位置缓存**：
   - 构建网格时在同次遍历填充位置缓存，候选循环不再跨越含40条历史的Bird读取坐标；3000容量增加24,000字节固定缓存。单线程收益与多核波动见上层 `docs/验证记录.md`，没有测量硬件缓存未命中计数。
