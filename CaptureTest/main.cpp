#include "CaptureLib.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <winrt/Windows.Foundation.h>

#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

using namespace CaptureLib;

int main() {
    // 1. 初始化 COM
    winrt::init_apartment();

    // 2. 枚举显示器
    auto monitors = DesktopCapture::EnumerateMonitors();
    if (monitors.empty()) {
        std::cerr << "No monitors found!" << std::endl;
        return 1;
    }

    std::cout << "Found " << monitors.size() << " monitors:" << std::endl;
    for (size_t i = 0; i < monitors.size(); ++i) {
        std::wcout << i << ": " << monitors[i].name << std::endl;
    }

    // 3. 初始化采集器 (使用第一个显示器)
    DesktopCapture capture;
    int bitrate = 4000000; // 4 Mbps
    int fps = 60;
    int gop = 120;
    int width = 1280;
    int height = 720;
    bool borderRequired = false;
    if (!capture.Initialize(monitors[0].handle, bitrate, fps, gop, width, height, borderRequired)) {
        std::cerr << "Failed to initialize capture!" << std::endl;
        return 1;
    }

    // 4. 开始采集并写入文件

    std::ofstream outFile("capture_test.h264", std::ios::binary);
    if (!outFile.is_open()) {
        std::cerr << "Failed to open output file!" << std::endl;
        return 1;
    }

    std::cout << "Starting capture for 10 seconds..." << std::endl;
    std::cout << "Target: 60 FPS, Bitrate: " << bitrate / 1000 << " kbps" << std::endl;
    std::atomic<int> frameCount(0);
    auto startTime = std::chrono::steady_clock::now();

    capture.Start([&](const uint8_t* data, size_t size, uint64_t timestamp) {
        outFile.write(reinterpret_cast<const char*>(data), size);
        frameCount++;
        if (frameCount % 60 == 0) {
            std::cout << "\rCaptured " << frameCount << " frames..." << std::flush;
        }
    });

    // 录制 10 秒
    std::cout << "Capturing for 10 seconds... Note: You should see a yellow border around the screen." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));

    capture.Stop();
    outFile.close();

    auto endTime = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    std::cout << "\nCapture finished!" << std::endl;
    std::cout << "Total frames: " << frameCount << std::endl;
    std::cout << "Actual duration: " << elapsed / 1000.0 << "s" << std::endl;
    std::cout << "Average FPS: " << (frameCount * 1000.0) / elapsed << std::endl;
    std::cout << "Output saved to capture_test.h264" << std::endl;
    std::cout << "\nNote: The output is a raw H.264 stream. You can wrap it into MP4 using ffmpeg:" << std::endl;
    std::cout << "ffmpeg -i capture_test.h264 -c copy capture_test.mp4" << std::endl;

    return 0;
}
