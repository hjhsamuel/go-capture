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
        bool isPrimary;
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

extern "C" {
    typedef void(*CaptureDataCallback)(const uint8_t* data, size_t size, uint64_t timestamp);

    CAPTURELIB_API void* CreateDesktopCapture();
    CAPTURELIB_API void DestroyDesktopCapture(void* capture);
    CAPTURELIB_API bool InitializeCapture(void* capture, HMONITOR monitor, int bitrate, int fps);
    CAPTURELIB_API void StartCapture(void* capture, CaptureDataCallback callback);
    CAPTURELIB_API void StopCapture(void* capture);
    
    // Helper to get monitors
    struct CMonitorInfo {
        HMONITOR handle;
        wchar_t name[256];
        bool isPrimary;
    };
    CAPTURELIB_API int GetMonitors(CMonitorInfo* monitors, int maxCount);
}
