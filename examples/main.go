package main

/*
#cgo CFLAGS: -I.
#cgo LDFLAGS: -L. -lCaptureLib -lwindowsapp -ld3d11 -lmfplat -lmfuuid -lmfreadwrite -lwmcodecdspuuid -lole32 -luser32

#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

typedef void(*CaptureDataCallback)(const uint8_t* data, size_t size, uint64_t timestamp);
typedef void(*LogCallback)(int level, const char* message);

void SetLogCallback(LogCallback callback);
void* CreateDesktopCapture();
void DestroyDesktopCapture(void* capture);
bool InitializeCapture(void* capture, HMONITOR monitor, int bitrate, int fps, int gopSize, int width, int height, bool borderRequired);
void StartCapture(void* capture, CaptureDataCallback callback);
void StopCapture(void* capture);
void RequestIDR(void* capture);

struct CMonitorInfo {
    HMONITOR handle;
    wchar_t name[256];
    bool isPrimary;
};
int GetMonitors(struct CMonitorInfo* monitors, int maxCount);

// Gateway for CGO callback
extern void goCaptureCallback(uint8_t* data, size_t size, uint64_t timestamp);
extern void goLogCallback(int level, char* message);

static void startCaptureWithGateway(void* capture) {
    StartCapture(capture, (CaptureDataCallback)goCaptureCallback);
}

static void setLogCallbackWithGateway() {
    SetLogCallback((LogCallback)goLogCallback);
}
*/
import "C"
import (
	"fmt"
	"os"
	"runtime"
	"strings"
	"time"
	"unsafe"
)

type NALUType byte

const (
	NALU_TYPE_UNDEFINED NALUType = 0
	NALU_TYPE_NON_IDR   NALUType = 1
	NALU_TYPE_IDR       NALUType = 5
	NALU_TYPE_SEI       NALUType = 6
	NALU_TYPE_SPS       NALUType = 7
	NALU_TYPE_PPS       NALUType = 8
	NALU_TYPE_AUD       NALUType = 9
)

func (t NALUType) String() string {
	switch t {
	case NALU_TYPE_NON_IDR:
		return "NON-IDR"
	case NALU_TYPE_IDR:
		return "IDR"
	case NALU_TYPE_SEI:
		return "SEI"
	case NALU_TYPE_SPS:
		return "SPS"
	case NALU_TYPE_PPS:
		return "PPS"
	case NALU_TYPE_AUD:
		return "AUD"
	default:
		return fmt.Sprintf("Type-%d", t)
	}
}

func GetNALUTypes(data []byte) []NALUType {
	var types []NALUType
	n := len(data)
	for i := 0; i < n-4; i++ {
		// Check for start code: 00 00 01 or 00 00 00 01
		startCodeLen := 0
		if data[i] == 0 && data[i+1] == 0 && data[i+2] == 1 {
			startCodeLen = 3
		} else if data[i] == 0 && data[i+1] == 0 && data[i+2] == 0 && data[i+3] == 1 {
			startCodeLen = 4
		}

		if startCodeLen > 0 {
			pos := i + startCodeLen
			if pos < n {
				// NALU Header: F(1 bit), NRI(2 bits), Type(5 bits)
				naluType := NALUType(data[pos] & 0x1F)
				types = append(types, naluType)
			}
			i += startCodeLen - 1 // Skip start code
		}
	}
	return types
}

var globalCallback func([]byte, uint64)

//export goCaptureCallback
func goCaptureCallback(data *C.uint8_t, size C.size_t, timestamp C.uint64_t) {
	if globalCallback != nil {
		goData := C.GoBytes(unsafe.Pointer(data), C.int(size))
		globalCallback(goData, uint64(timestamp))
	}
}

//export goLogCallback
func goLogCallback(level C.int, message *C.char) {
	goMsg := C.GoString(message)
	levelStr := "INFO"
	switch level {
	case 1:
		levelStr = "WARN"
	case 2:
		levelStr = "ERROR"
	}
	fmt.Printf("[CPP-%s] %s\n", levelStr, goMsg)
}

type Monitor struct {
	Handle    C.HMONITOR
	Name      string
	IsPrimary bool
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
			Handle:    cMonitors[i].handle,
			Name:      name,
			IsPrimary: bool(cMonitors[i].isPrimary),
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

func (c *DesktopCapture) Initialize(monitor C.HMONITOR, bitrate int, fps int, gop int, width int, height int, borderRequired bool) bool {
	return bool(C.InitializeCapture(c.ptr, monitor, C.int(bitrate), C.int(fps), C.int(gop), C.int(width), C.int(height), C.bool(borderRequired)))
}

func (c *DesktopCapture) Start(callback func([]byte, uint64)) {
	globalCallback = callback
	C.startCaptureWithGateway(c.ptr)
}

func (c *DesktopCapture) Stop() {
	C.StopCapture(c.ptr)
	globalCallback = nil
}

func (c *DesktopCapture) RequestIDR() {
	C.RequestIDR(c.ptr)
}

func main() {
	// Lock OS thread for GUI/WinRT stuff if necessary,
	// though our lib creates its own dispatcher thread.
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	fmt.Println("Golang Desktop Capture Test")

	C.setLogCallbackWithGateway()

	monitors := EnumerateMonitors()
	if len(monitors) == 0 {
		fmt.Println("No monitors found")
		return
	}

	fmt.Printf("Found %d monitors:\n", len(monitors))
	for i, m := range monitors {
		primaryStr := ""
		if m.IsPrimary {
			primaryStr = " (Primary)"
		}
		fmt.Printf("%d: %s%s\n", i, m.Name, primaryStr)
	}

	capture := NewDesktopCapture()
	defer capture.Close()

	bitrate := 4000000
	fps := 60
	gop := fps * 2
	width := 1920
	height := 1080
	borderRequired := true

	if !capture.Initialize(monitors[0].Handle, bitrate, fps, gop, width, height, borderRequired) {
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
	idrCount := 0
	capture.Start(func(data []byte, timestamp uint64) {
		f.Write(data)
		frameCount++

		naluTypes := GetNALUTypes(data)
		isIDR := false
		var typeStrings []string
		for _, t := range naluTypes {
			typeStrings = append(typeStrings, t.String())
			if t == NALU_TYPE_IDR {
				isIDR = true
			}
		}
		typeList := ""
		if len(typeStrings) > 0 {
			typeList = fmt.Sprintf(" [%s]", strings.Join(typeStrings, ", "))
		}

		if isIDR {
			idrCount++
			fmt.Printf("\n[IDR] Frame %d, IDR count: %d%s\n", frameCount, idrCount, typeList)
		} else if frameCount%60 == 0 {
			fmt.Printf("\rCaptured %d frames...%s", frameCount, typeList)
		}
	})

	ticker := time.NewTicker(time.Second)
	go func() {
		for {
			select {
			case <-ticker.C:
				fmt.Println("\nRequesting IDR frame...")
				capture.RequestIDR()
			}
		}
	}()

	fmt.Println("Capturing for 10 seconds...")
	time.Sleep(10 * time.Second)

	ticker.Stop()
	capture.Stop()

	fmt.Printf("\nCapture finished. Total frames: %d\n", frameCount)
	fmt.Println("Output saved to go_capture.h264")
}
