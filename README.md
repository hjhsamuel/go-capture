# go-capture: Windows 桌面视频采集库

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
![Language](https://img.shields.io/badge/Language-C%2B%2B%20%26%20Go-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Windows-blue.svg)

一个基于 **Windows Graphics Capture (WGC)** 和 **Media Foundation** 的高性能桌面视频采集库。使用 CGO 将 C++ 核心库封装为 Go 接口，提供简洁易用的 API，支持硬件加速 H.264 编码。

## ✨ 核心特性

- **🎬 高性能采集**：基于 Windows.Graphics.Capture API，支持多监视器、高分辨率、高帧率采集
- **⚡ 硬件加速编码**：使用 Media Foundation 硬件 H.264 编码器，大幅降低 CPU 占用
- **🔄 支持多格式**：自动处理色彩空间转换 (BGRA → NV12)，输出标准 H.264 NALUs
- **🌐 跨语言支持**：通过 CGO 同时提供 C++ 和 Go 接口
- **📊 灵活配置**：支持自定义比特率、帧率和监视器选择

## 📋 系统要求

| 要求 | 版本 |
|------|------|
| **操作系统** | Windows 10/11 |
| **编译器** | Visual Studio 2022 或更高版本（支持 C++20） |
| **Windows SDK** | 10.0.22621.0 或更高版本 |
| **Go 版本** | 1.18 或更高版本（仅用于 Go 示例） |

> **⚠️ 注意**：由于旧版 Windows SDK 的 C++/WinRT 与 C++20 存在兼容性问题，强烈建议使用 Windows SDK 10.0.22621.0 或更新版本。

## 🏗️ 项目结构

```
go-capture/
├── CaptureLib/              # C++ 核心库
│   ├── CaptureLib.h         # C++ 和 C 接口定义
│   ├── CaptureLib.cpp       # 采集与编码实现
│   └── CMakeLists.txt       # CMake 构建配置
├── CaptureTest/             # C++ 测试程序
│   ├── main.cpp
│   └── CMakeLists.txt
├── examples/                # Go 示例程序
│   └── main.go
├── LICENSE                  # MIT 许可证
└── README.md               # 本文件
```

## 🔧 编译与构建

### C++ 库编译

推荐在 **Developer PowerShell for VS 2022** 或 **x64 Native Tools Command Prompt for VS 2022** 中执行：

```powershell
# 1. 创建构建目录
mkdir build
cd build

# 2. 生成 Visual Studio 项目
cmake ..\CaptureLib

# 3. 编译 Release 版本
cmake --build . --config Release
```

编译完成后，`CaptureLib.dll` 和 `CaptureLib.lib` 将生成在 `build\CaptureLib\Release` 目录下。

**故障排除**：
- 若遇到 `wait_for` 相关的 C++/WinRT 编译错误，请确保已安装最新版本的 Windows SDK
- 使用 VS 2022 的"工作负载"安装程序确保选中了 C++ 开发工具

### 完整项目编译（包含测试）

```powershell
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## 📖 API 文档

### C++ 接口

#### `MonitorInfo` 结构体

```cpp
struct MonitorInfo {
    HMONITOR handle;        // 监视器句柄
    std::wstring name;      // 监视器名称
    bool isPrimary;         // 是否为主监视器
};
```

#### `DesktopCapture` 类

```cpp
class DesktopCapture {
public:
    // 枚举所有连接的监视器
    static std::vector<MonitorInfo> EnumerateMonitors();
    
    // 初始化采集器
    // @param monitor: 目标监视器句柄
    // @param bitrate: 编码比特率（单位：bps，如 4000000 = 4Mbps）
    // @param fps: 帧率（如 30、60）
    // @return: 初始化是否成功
    bool Initialize(HMONITOR monitor, int bitrate, int fps);
    
    // 启动采集
    // @param callback: 数据回调函数，返回 H.264 编码数据
    void Start(DataCallback callback);
    
    // 停止采集
    void Stop();
};

// 回调函数类型定义
typedef std::function<void(const uint8_t* data, size_t size, uint64_t timestamp)> DataCallback;
```

#### C 接口（供 CGO 调用）

```cpp
// 创建采集器实例
void* CreateDesktopCapture();

// 销毁采集器实例
void DestroyDesktopCapture(void* capture);

// 初始化采集器
bool InitializeCapture(void* capture, HMONITOR monitor, int bitrate, int fps);

// 启动采集
void StartCapture(void* capture, CaptureDataCallback callback);

// 停止采集
void StopCapture(void* capture);

// 获取监视器列表
int GetMonitors(CMonitorInfo* monitors, int maxCount);

// C 接口回调类型
typedef void(*CaptureDataCallback)(const uint8_t* data, size_t size, uint64_t timestamp);
```

### Go 接口

```go
// 监视器信息
type Monitor struct {
    Handle    C.HMONITOR  // 监视器句柄
    Name      string      // 监视器名称
    IsPrimary bool        // 是否为主监视器
}

// 枚举所有监视器
func EnumerateMonitors() []Monitor

// 创建采集器
func NewDesktopCapture() *DesktopCapture

// 初始化采集器
// bitrate: 比特率（单位：bps）
// fps: 帧率
func (c *DesktopCapture) Initialize(monitor C.HMONITOR, bitrate int, fps int) bool

// 启动采集（阻塞式，直到 Stop() 被调用）
func (c *DesktopCapture) Start(callback func([]byte, uint64))

// 停止采集
func (c *DesktopCapture) Stop()

// 关闭采集器并释放资源
func (c *DesktopCapture) Close()
```

## 💻 使用示例

### Go 示例

```go
package main

import (
    "fmt"
    "os"
    "time"
)

func main() {
    // 枚举监视器
    monitors := EnumerateMonitors()
    if len(monitors) == 0 {
        fmt.Println("No monitors found")
        return
    }
    
    fmt.Printf("Found %d monitors:\n", len(monitors))
    for i, m := range monitors {
        fmt.Printf("%d: %s", i, m.Name)
        if m.IsPrimary {
            fmt.Print(" (Primary)")
        }
        fmt.Println()
    }
    
    // 创建采集器
    capture := NewDesktopCapture()
    defer capture.Close()
    
    // 初始化：4Mbps, 60fps
    if !capture.Initialize(monitors[0].Handle, 4000000, 60) {
        fmt.Println("Failed to initialize capture")
        return
    }
    
    // 创建输出文件
    f, err := os.Create("capture.h264")
    if err != nil {
        fmt.Printf("Failed to create file: %v\n", err)
        return
    }
    defer f.Close()
    
    frameCount := 0
    
    // 启动采集并处理数据
    capture.Start(func(data []byte, timestamp uint64) {
        f.Write(data)  // 写入 H.264 编码数据
        frameCount++
        if frameCount%60 == 0 {
            fmt.Printf("Captured %d frames...\n", frameCount)
        }
    })
    
    fmt.Println("Capturing for 10 seconds...")
    time.Sleep(10 * time.Second)
    
    capture.Stop()
    fmt.Printf("Capture finished. Total frames: %d\n", frameCount)
    fmt.Println("Output saved to capture.h264")
}
```

### C++ 示例

```cpp
#include "CaptureLib.h"
#include <iostream>
#include <fstream>

int main() {
    using namespace CaptureLib;
    
    // 枚举监视器
    auto monitors = DesktopCapture::EnumerateMonitors();
    if (monitors.empty()) {
        std::cout << "No monitors found" << std::endl;
        return 1;
    }
    
    // 显示监视器信息
    for (size_t i = 0; i < monitors.size(); ++i) {
        std::wcout << i << ": " << monitors[i].name;
        if (monitors[i].isPrimary) std::wcout << " (Primary)";
        std::wcout << std::endl;
    }
    
    // 创建采集器
    DesktopCapture capture;
    
    // 初始化：4Mbps, 60fps
    if (!capture.Initialize(monitors[0].handle, 4000000, 60)) {
        std::cout << "Failed to initialize capture" << std::endl;
        return 1;
    }
    
    // 打开输出文件
    std::ofstream outFile("capture.h264", std::ios::binary);
    
    // 启动采集
    capture.Start([&outFile](const uint8_t* data, size_t size, uint64_t timestamp) {
        outFile.write((const char*)data, size);
    });
    
    std::cout << "Capturing for 10 seconds..." << std::endl;
    Sleep(10000);
    
    capture.Stop();
    outFile.close();
    
    std::cout << "Capture finished. Output saved to capture.h264" << std::endl;
    return 0;
}
```

## 🚀 运行 Go 示例

### 前置条件

首先需要编译 C++ 库生成 `CaptureLib.dll`：

```powershell
# 从项目根目录执行
mkdir build
cd build
cmake ..\CaptureLib
cmake --build . --config Release
```

### 配置和运行

```powershell
# 切换到 examples 目录
cd examples

# 复制必要的文件
copy ..\build\CaptureLib\Release\CaptureLib.dll .
copy ..\CaptureLib\CaptureLib.h .

# 编译并运行
go run main.go
```

运行成功后，会在 `examples` 目录下生成 `go_capture.h264` 文件。

## 📹 H.264 视频文件处理

### 验证编码正确性

**使用 ffprobe 检查**：
```powershell
ffprobe go_capture.h264
```

预期输出示例：
```
Input #0, h264, from 'go_capture.h264':
  Duration: 00:00:10.00, bitrate: 4000k
  Stream #0:0: Video: h264 (Main), yuv420p(progressive), 2560x1440, 60 fps, 60 tbr
```

**使用 ffmpeg 验证可解码性**：
```powershell
ffmpeg -v error -i go_capture.h264 -f null -
```
无任何输出表示码流可以正确解码。

### 转换为 MP4 格式

H.264 文件无法直接播放，需要转换为标准容器格式：

```powershell
ffmpeg -i go_capture.h264 -c copy go_capture.mp4
```

然后可以使用任何支持 H.264 的播放器播放 `go_capture.mp4`。

## 🔍 技术实现细节

### 采集流程

1. **监视器枚举**：使用 Windows API 获取所有连接的显示设备
2. **WGC 初始化**：创建 GraphicsCapture 会话和帧池
3. **帧读取**：异步接收每一帧的 BGRA8 格式数据
4. **色彩空间转换**：将 BGRA8 转换为 Media Foundation 编码器需要的 NV12 格式
5. **硬件编码**：使用 GPU 上的 H.264 编码器进行编码
6. **数据输出**：通过回调函数返回编码后的 H.264 NALUs

### 关键技术栈

| 组件 | 作用 |
|------|------|
| **Windows.Graphics.Capture** | 高效的桌面屏幕采集 |
| **Direct3D 11** | GPU 色彩空间转换 |
| **Media Foundation** | 硬件 H.264 编码 |
| **C++20 Coroutines** | 异步任务管理 |
| **CGO** | Go 与 C++ 互操作 |

## 📊 性能指标

在 RTX 3070 GPU 上的典型性能（2560×1440 分辨率）：

| 指标 | 值 |
|------|-----|
| **CPU 占用** | < 5% |
| **GPU 占用** | 10-20% |
| **内存使用** | ~100MB |
| **编码延迟** | < 50ms |
| **4Mbps 60fps 支持** | ✅ 稳定 |

> 实际性能取决于硬件配置、系统负载和分辨率设置。

## 🛠️ 故障排除

### 编译错误：`wait_for` 相关错误

**原因**：Windows SDK 版本过旧，C++/WinRT 不兼容 C++20

**解决**：
1. 升级 Windows SDK 到 10.0.22621.0 或更新
2. 打开 Visual Studio Installer，修改安装
3. 在"个别组件"中选择最新的 Windows SDK

### 运行时错误：`Failed to initialize capture`

**可能原因**：
- 不支持的监视器配置
- 硬件不支持 H.264 编码
- 权限不足

**解决**：
- 检查硬件是否支持 Media Foundation H.264 编码（大多数现代 GPU 都支持）
- 尝试更换监视器或分辨率
- 以管理员身份运行程序

### Go 编译错误：`undefined reference`

**原因**：缺少 `CaptureLib.dll` 或 `CaptureLib.h`

**解决**：
- 确保 `CaptureLib.dll` 和 `CaptureLib.h` 在 `examples` 目录中
- 检查 CGO 编译标志是否正确
- 使用 `go env` 验证 CGO 设置

## 📄 许可证

本项目采用 MIT 许可证。详见 [LICENSE](LICENSE) 文件。

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

## 📚 参考资源

- [Windows.Graphics.Capture API](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture)
- [Media Foundation Encoder](https://learn.microsoft.com/en-us/windows/win32/medfound/media-foundation-encoder)
- [C++ 与 Go 的 CGO 互操作](https://pkg.go.dev/cmd/cgo)
- [Visual Studio 2022 C++20 支持](https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance)

---

**作者**：hjhsamuel  
**最后更新**：2026-06-18
