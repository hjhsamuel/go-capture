package main

/*
#cgo CFLAGS: -I.
#cgo LDFLAGS: -L. -lCaptureLib -lwindowsapp -ld3d11 -lmfplat -lmfuuid -lmfreadwrite -lwmcodecdspuuid -lole32 -luser32

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

typedef void(*CaptureDataCallback)(const uint8_t* data, size_t size, uint64_t timestamp);

void* CreateDesktopCapture();
void DestroyDesktopCapture(void* capture);
bool InitializeCapture(void* capture, HMONITOR monitor, int bitrate, int fps);
void StartCapture(void* capture, CaptureDataCallback callback);
void StopCapture(void* capture);

struct CMonitorInfo {
    HMONITOR handle;
    wchar_t name[256];
};
int GetMonitors(struct CMonitorInfo* monitors, int maxCount);

// Gateway for CGO callback
extern void goCaptureCallback(uint8_t* data, size_t size, uint64_t timestamp);

static void startCaptureWithGateway(void* capture) {
    StartCapture(capture, (CaptureDataCallback)goCaptureCallback);
}
*/
import "C"
import (
	"fmt"
	"os"
	"runtime"
	"time"
	"unsafe"
)

var globalCallback func([]byte, uint64)

//export goCaptureCallback
func goCaptureCallback(data *C.uint8_t, size C.size_t, timestamp C.uint64_t) {
	if globalCallback != nil {
		goData := C.GoBytes(unsafe.Pointer(data), C.int(size))
		globalCallback(goData, uint64(timestamp))
	}
}

type Monitor struct {
	Handle C.HMONITOR
	Name   string
}

func EnumerateMonitors() []Monitor {
	var cMonitors [16]C.struct_CMonitorInfo
	count := int(C.GetMonitors(&cMonitors[0], 16))

	monitors := make([]Monitor, 0, count)
	for i := 0; i < count; i++ {
		// Convert wchar_t[256] to string
		// This is a bit tricky in Go/CGO on Windows because wchar_t is 2 bytes (UTF-16)
		name := ""
		for j := 0; j < 256; j++ {
			if cMonitors[i].name[j] == 0 {
				break
			}
			name += string(rune(cMonitors[i].name[j]))
		}

		monitors = append(monitors, Monitor{
			Handle: cMonitors[i].handle,
			Name:   name,
		})
	}
	return monitors
}

type DesktopCapture struct {
	ptr unsafe.Pointer
}

func NewDesktopCapture() *DesktopCapture {
	ptr := C.CreateDesktopCapture()
	return &DesktopCapture{ptr: ptr}
}

func (c *DesktopCapture) Close() {
	if c.ptr != nil {
		C.DestroyDesktopCapture(c.ptr)
		c.ptr = nil
	}
}

func (c *DesktopCapture) Initialize(monitor C.HMONITOR, bitrate int, fps int) bool {
	return bool(C.InitializeCapture(c.ptr, monitor, C.int(bitrate), C.int(fps)))
}

func (c *DesktopCapture) Start(callback func([]byte, uint64)) {
	globalCallback = callback
	C.startCaptureWithGateway(c.ptr)
}

func (c *DesktopCapture) Stop() {
	C.StopCapture(c.ptr)
	globalCallback = nil
}

func main() {
	// Lock OS thread for GUI/WinRT stuff if necessary,
	// though our lib creates its own dispatcher thread.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	fmt.Println("Golang Desktop Capture Test")

	monitors := EnumerateMonitors()
	if len(monitors) == 0 {
		fmt.Println("No monitors found")
		return
	}

	fmt.Printf("Found %d monitors:\n", len(monitors))
	for i, m := range monitors {
		fmt.Printf("%d: %s\n", i, m.Name)
	}

	capture := NewDesktopCapture()
	defer capture.Close()

	bitrate := 4000000
	fps := 60

	if !capture.Initialize(monitors[0].Handle, bitrate, fps) {
		fmt.Println("Failed to initialize capture")
		return
	}

	f, err := os.Create("go_capture.h264")
	if err != nil {
		fmt.Printf("Failed to create file: %v\n", err)
		return
	}
	defer f.Close()

	frameCount := 0
	capture.Start(func(data []byte, timestamp uint64) {
		f.Write(data)
		frameCount++
		if frameCount%60 == 0 {
			fmt.Printf("\rCaptured %d frames...", frameCount)
		}
	})

	fmt.Println("Capturing for 5 seconds...")
	time.Sleep(5 * time.Second)

	capture.Stop()
	fmt.Printf("\nCapture finished. Total frames: %d\n", frameCount)
	fmt.Println("Output saved to go_capture.h264")
}
