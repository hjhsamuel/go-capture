#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <vector>

#ifdef CAPTURELIB_EXPORTS
#define CAPTURELIB_API __declspec(dllexport)
#else
#define CAPTURELIB_API 
#endif

namespace CaptureLib {

    struct MonitorInfo {
        HMONITOR handle;
        std::wstring name;
    };

    typedef std::function<void(const uint8_t* data, size_t size, uint64_t timestamp)> DataCallback;

    class CAPTURELIB_API DesktopCapture {
    public:
        static std::vector<MonitorInfo> EnumerateMonitors();

        DesktopCapture();
        ~DesktopCapture();

        bool Initialize(HMONITOR monitor, int bitrate, int fps);
        void Start(DataCallback callback);
        void Stop();

    private:
        class Impl;
        Impl* m_impl;
    };

}
