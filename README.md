# Windows Desktop Capture & H264 Encoder Library

这是一个使用 Windows Graphics Capture (WGC) 采集桌面并使用 Media Foundation (MF) 进行 H.264 硬件编码的 C++ 库。

## 项目结构

- `CaptureLib/`: 核心库代码。
  - `CaptureLib.h`: 导出接口。
  - `CaptureLib.cpp`: 采集与编码实现。
  - `CMakeLists.txt`: CMake 构建配置文件。
- `Win32CaptureSample/`: 参考的官方示例代码。

## 编译要求

- Windows 10/11
- Visual Studio 2022 (或支持 C++20 的版本)
- **Windows SDK 10.0.22621.0 或更高版本** (由于旧版 SDK 的 C++/WinRT 与 C++20 存在兼容性问题，建议使用此版本或更新版本)

## 编译步骤

推荐在 **Developer PowerShell for VS 2022** 或 **x64 Native Tools Command Prompt for VS 2022** 中执行：

1. 进入项目根目录执行：
   ```powershell
   mkdir build
   cd build
   cmake ..\CaptureLib
   cmake --build . --config Release
   ```
2. 编译完成后，`CaptureLib.lib` 将生成在 `build\Release` 目录下。

**注意**：如果遇到 `wait_for` 相关的 C++/WinRT 编译错误，请确保已安装并选择了较新版本的 Windows SDK。

## 接口说明

```cpp
#include "CaptureLib.h"

// 1. 枚举显示器
auto monitors = CaptureLib::DesktopCapture::EnumerateMonitors();

// 2. 初始化采集器
CaptureLib::DesktopCapture capture;
capture.Initialize(monitors[0].handle, 4000000, 60); // 4Mbps, 60fps

// 3. 开始采集并输出 H264 数据
capture.Start([](const uint8_t* data, size_t size, uint64_t timestamp) {
    // 处理编码后的 H264 数据包 (NALUs)
});

// 4. 停止采集
capture.Stop();
```

## 实现细节

- **采集**: 使用 `Windows.Graphics.Capture` API，支持高性能桌面抓取。
- **编码**: 使用 Media Foundation 硬件加速编码器 (`CLSID_CMSH264EncoderMFT`)。
- **颜色转换**: 从 WGC 的 `B8G8R8A8` 格式转换为编码器通用的 `NV12` 格式（实现中建议使用 GPU Video Processor）。

## 测试

### C++

C++ 的测试文件在 `CaptureTest` 目录下，同样，在项目根目录下进行编译：

```powershell
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

在编译完成后，在 `build/CaptureTest/Release` 目录下将生成 `CaptureTest.exe` 文件


### Golang

golang 通过 CGO 加载 dll 文件，因此需要先完成 dll 文件的编译，具体操作 [参考](https://github.com/hjhsamuel/go-capture#编译步骤)

在编译完成后，请将 dll 文件拷贝至项目根目录下，然后执行：

```powershell
go run main.go
```

## h264

### 检查 h264 文件编码是否正确

- ffprobe

    ```powershell
    ffprobe ./go_capture.h264
    ```
    
    关键输出信息：
    
    ```powershell
    Input #0, h264, from '.\go_capture.h264':
      Duration: N/A, bitrate: N/A
      Stream #0:0: Video: h264 (Main), yuv420p(progressive), 2560x1440, 60 fps, 60 tbr, 1200k tbn
    ```
  
- ffmpeg

    ```powershell
    ffmpeg -v error -i go_capture.h264 -f null -
    ```
    
    如果没有任何输出，说明码流可以被正确解码

### 播放视频文件

无法直接播放 h264 文件，可以尝试将其转换为 mp4 文件

```powershell
ffmpeg -i go_capture.h264 -c copy go_capture.mp4
```