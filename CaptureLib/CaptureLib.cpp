#include "CaptureLib.h"
#include <unknwn.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wmcodecdsp.h>
#include <chrono>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <wrl.h>
#include <avrt.h>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <iostream>

#pragma comment(lib, "windowsapp")
#pragma comment(lib, "d3d11")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")
#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "wmcodecdspuuid")
#pragma comment(lib, "avrt")

using namespace Microsoft::WRL;

namespace CaptureLib {

    // Helper to convert winrt device to d3d11 device
    template <typename T>
    auto GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const& object)
    {
        auto access = object.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<T> result;
        winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
        return result;
    }

    class DesktopCapture::Impl {
    public:
        Impl() : m_closed(false), m_running(false) {
            MFStartup(MF_VERSION);
        }

        ~Impl() {
            Stop();
            MFShutdown();
        }

        bool Initialize(HMONITOR monitor, int bitrate, int fps, int gopSize, int width, int height, bool borderRequired) {
            std::cout << "Initializing CaptureLib..." << std::endl;
            m_monitor = monitor;
            m_bitrate = bitrate;
            m_fps = fps;
            m_gopSize = gopSize;
            m_width = width;
            m_height = height;
            m_borderRequired = borderRequired;

            // Ensure DispatcherQueue exists for the thread
            if (!m_dispatcherQueue) {
                m_controller = winrt::Windows::System::DispatcherQueueController::CreateOnDedicatedThread();
                m_dispatcherQueue = m_controller.DispatcherQueue();
            }

            // Initialize D3D11
            UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, m_d3dDevice.put(), nullptr, m_d3dContext.put());
            if (FAILED(hr)) {
                std::cerr << "Failed to create D3D11 Device: " << std::hex << hr << std::endl;
                return false;
            }

            // Enable multithreaded protection for D3D11 context
            winrt::com_ptr<ID3D11Multithread> mt;
            if (SUCCEEDED(m_d3dContext->QueryInterface(IID_PPV_ARGS(mt.put())))) {
                mt->SetMultithreadProtected(TRUE);
            }

            auto dxgiDevice = m_d3dDevice.as<IDXGIDevice>();
            winrt::com_ptr<IInspectable> inspectable;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
            m_device = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

            // Get Capture Item
            auto interop_factory = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            hr = interop_factory->CreateForMonitor(m_monitor, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(), winrt::put_abi(m_item));
            if (FAILED(hr)) {
                std::cerr << "Failed to create GraphicsCaptureItem for monitor: " << std::hex << hr << std::endl;
                return false;
            }

            m_srcWidth = m_item.Size().Width;
            m_srcHeight = m_item.Size().Height;
            std::cout << "Monitor resolution: " << m_srcWidth << "x" << m_srcHeight << std::endl;

            if (m_width <= 0 || m_height <= 0) {
                m_width = m_srcWidth;
                m_height = m_srcHeight;
            }

            // Adjust width/height to be even for H264
            m_width &= ~1;
            m_height &= ~1;

            std::cout << "Output resolution: " << m_width << "x" << m_height << " FPS: " << m_fps << " Bitrate: " << m_bitrate << std::endl;

            return InitEncoder();
        }

        bool InitEncoder() {
            return InitMFT();
        }

        bool InitMFT() {
            std::cout << "Initializing MFT..." << std::endl;
            HRESULT hr = S_OK;
            
            // Find H264 Encoder MFT
            MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_H264 };
            UINT32 count = 0;
            IMFActivate** activates = nullptr;
            
            // Just use FLAG_ALL to avoid filtering issues, but sort and filter is usually better
            hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &outInfo, &activates, &count);
            
            if (FAILED(hr) || count == 0) {
                std::cerr << "No H264 encoders found!" << std::endl;
                return false;
            }

            std::cout << "Found " << count << " encoders." << std::endl;

            for (UINT32 i = 0; i < count; i++) {
                m_encoder = nullptr;
                LPWSTR name = nullptr;
                UINT32 nameLen = 0;
                activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLen);
                std::wstring wname = name ? name : L"Unknown";
                std::wcout << L"Trying Encoder [" << i << L"]: " << wname << std::endl;
                if (name) CoTaskMemFree(name);

                hr = activates[i]->ActivateObject(IID_PPV_ARGS(m_encoder.put()));
                if (FAILED(hr)) {
                    std::cerr << "  Failed to activate: " << std::hex << hr << std::endl;
                    continue;
                }

                // Try to configure this encoder
                winrt::com_ptr<IMFMediaType> outType;
                MFCreateMediaType(outType.put());
                outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
                outType->SetUINT32(MF_MT_AVG_BITRATE, m_bitrate);
                MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
                MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, m_fps, 1);
                outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                outType->SetUINT32(MF_MT_MAX_KEYFRAME_SPACING, 30); // Use a more reasonable default GOP size
                
                hr = m_encoder->SetOutputType(0, outType.get(), 0);
                if (FAILED(hr)) {
                    std::cerr << "  Failed to set output type: " << std::hex << hr << std::endl;
                    continue;
                }

                // Try Input Types
                GUID inputFormats[] = { MFVideoFormat_NV12, MFVideoFormat_YV12, MFVideoFormat_IYUV, MFVideoFormat_RGB32 };
                bool inputSet = false;
                for (auto& fmt : inputFormats) {
                    winrt::com_ptr<IMFMediaType> inType;
                    MFCreateMediaType(inType.put());
                    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                    inType->SetGUID(MF_MT_SUBTYPE, fmt); 
                    MFSetAttributeSize(inType.get(), MF_MT_FRAME_SIZE, m_width, m_height);
                    MFSetAttributeRatio(inType.get(), MF_MT_FRAME_RATE, m_fps, 1);
                    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                    // MF_MT_REAL_TIME might not be available in all SDK versions
                    // inType->SetUINT32(MF_MT_REAL_TIME, TRUE);

                    hr = m_encoder->SetInputType(0, inType.get(), 0);
                    if (SUCCEEDED(hr)) {
                        m_inputFormat = fmt;
                        inputSet = true;
                        std::cout << "  Successfully set input format." << std::endl;
                        break;
                    }
                }

                if (inputSet) {
                    std::cout << "Successfully configured encoder " << i << std::endl;
                    break;
                } else {
                    std::cerr << "  Failed to set any input format." << std::endl;
                    m_encoder = nullptr;
                }
            }
            
            for (UINT32 i = 0; i < count; i++) activates[i]->Release();
            CoTaskMemFree(activates);

            if (!m_encoder) return false;

            // Optional: Set Bitrate via ICodecAPI
            winrt::com_ptr<ICodecAPI> codecApi;
            if (SUCCEEDED(m_encoder->QueryInterface(IID_PPV_ARGS(codecApi.put())))) {
                VARIANT var = {};
                var.vt = VT_UI4;
                var.ulVal = m_bitrate;
                codecApi->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);

                // Enable Low Latency Mode
                VARIANT varLL = {};
                varLL.vt = VT_BOOL;
                varLL.boolVal = VARIANT_TRUE;
                if (FAILED(hr = codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &varLL))) {
                    std::cerr << "  Warning: Failed to set low latency mode: " << std::hex << hr << std::endl;
                }

                // Enable Real Time Mode
                VARIANT varRT = {};
                varRT.vt = VT_BOOL;
                varRT.boolVal = VARIANT_TRUE;
                if (FAILED(hr = codecApi->SetValue(&CODECAPI_AVEncCommonRealTime, &varRT))) {
                    std::cerr << "  Warning: Failed to set real time mode: " << std::hex << hr << std::endl;
                }

                // Set GOP size (Keyframe spacing)
                VARIANT varGOP = {};
                varGOP.vt = VT_UI4;
                varGOP.ulVal = (m_gopSize > 0) ? (UINT32)m_gopSize : ((m_fps > 0) ? m_fps * 2 : 60);
                if (FAILED(hr = codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &varGOP))) {
                    std::cerr << "  Warning: Failed to set GOP size: " << std::hex << hr << std::endl;
                }

                // Disable B-frames for zero-latency reordering
                // CODECAPI_AVEncVideoMaxNumRefFrames: {3091B33B-8C4E-4643-984F-6E941D6837C5}
                VARIANT varB = {};
                varB.vt = VT_UI4;
                varB.ulVal = 0;
                const GUID CLSID_AVEncVideoMaxNumRefFrames = { 0x3091B33B, 0x8C4E, 0x4643, { 0x98, 0x4F, 0x6E, 0x94, 0x1D, 0x68, 0x37, 0xC5 } };
                codecApi->SetValue(&CLSID_AVEncVideoMaxNumRefFrames, &varB);

                // If hardware encoder activation failed with 8000ffff previously, 
                // it might be because of some attributes being set before they are supported.
                // However, the activation happens in activates[i]->ActivateObject.
            }

            // Move this before SetOutputType for some encoders
            winrt::com_ptr<IMFAttributes> encoderAttributes;
            if (SUCCEEDED(m_encoder->GetAttributes(encoderAttributes.put()))) {
                encoderAttributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                encoderAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            }

            m_encoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            m_encoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

            // Set D3D11 Device on Encoder
            // winrt::com_ptr<IMFAttributes> encoderAttributes; // Already declared above
            if (encoderAttributes) {
                // Ensure D3D11 is enabled on encoder attributes as well if needed
                // encoderAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            }

            winrt::com_ptr<IMFDXGIDeviceManager> deviceManager;
            UINT resetToken = 0;
            if (SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken, deviceManager.put()))) {
                if (SUCCEEDED(deviceManager->ResetDevice(m_d3dDevice.get(), resetToken))) {
                    m_encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager.get()));
                }
            }

            return true;
        }

        void Start(DataCallback callback) {
            std::cout << "Starting capture session..." << std::endl;
            m_callback = callback;
            m_running = true;
            m_startTimeNs = 0; // Reset start time

            m_dispatcherQueue.TryEnqueue([this]() {
                // Set MMCSS for the dispatcher thread
                DWORD taskIndex = 0;
                HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
                if (hTask) {
                    AvSetMmThreadPriority(hTask, AVRT_PRIORITY_CRITICAL);
                }

                try {
                    m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                        m_device,
                        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                        1,
                        m_item.Size());

                    m_session = m_framePool.CreateCaptureSession(m_item);

                    // Explicitly set border requirement based on input parameter
                    try {
                        m_session.IsBorderRequired(m_borderRequired);
                        std::cout << "Border required set to " << (m_borderRequired ? "true" : "false") << "." << std::endl;
                    } catch (...) {
                        std::cout << "IsBorderRequired not supported." << std::endl;
                    }

                    m_framePool.FrameArrived({ this, &Impl::OnFrameArrived });
                    m_session.StartCapture();
                    std::cout << "Capture session started on dispatcher thread." << std::endl;
                } catch (winrt::hresult_error const& ex) {
                    std::cerr << "Failed to start capture on dispatcher: " << std::hex << ex.code() << std::endl;
                }
            });

            // Start worker thread for constant FPS encoding
            m_workerThread = std::thread(&Impl::WorkerThread, this);
        }

        void Stop() {
            std::cout << "Stopping capture session..." << std::endl;
            m_running = false;
            
            if (m_workerThread.joinable()) {
                m_workerThread.join();
            }

            if (m_session) {
                m_session.Close();
                m_session = nullptr;
            }
            if (m_framePool) {
                m_framePool.Close();
                m_framePool = nullptr;
            }
            if (m_controller) {
                m_controller.ShutdownQueueAsync();
                m_controller = nullptr;
            }

            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_cacheTexture = nullptr;
                m_lastWgcFrame = nullptr;
            }
        }

        void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const&) {
            if (!m_running) return;
            auto frame = sender.TryGetNextFrame();
            if (!frame) return;

            auto surface = frame.Surface();
            auto texture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(surface);
            
            if (texture) {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                
                // Copy the frame to our cache texture to decouple from WGC frame pool recycling.
                // This prevents flickering caused by the pool reusing the texture while we are still encoding it.
                if (!m_cacheTexture) {
                    D3D11_TEXTURE2D_DESC desc;
                    texture->GetDesc(&desc);
                    // Ensure it's a default usage texture for CopyResource
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = 0;
                    m_d3dDevice->CreateTexture2D(&desc, nullptr, m_cacheTexture.put());
                }

                if (m_cacheTexture) {
                    // Use multithread protection if available for the copy
                    winrt::com_ptr<ID3D11Multithread> mt;
                    if (SUCCEEDED(m_d3dContext->QueryInterface(IID_PPV_ARGS(mt.put())))) {
                        mt->Enter();
                    }
                    m_d3dContext->CopyResource(m_cacheTexture.get(), texture.get());
                    if (mt) {
                        mt->Leave();
                    }
                }
                
                auto systemTime = frame.SystemRelativeTime().count();
                if (m_startTimeNs == 0) {
                    m_startTimeNs = systemTime;
                }
            }
        }

        void WorkerThread() {
            auto frameDuration = std::chrono::nanoseconds(1000000000 / m_fps);
            auto startTIme = std::chrono::steady_clock::now();
            auto nextFrameTime = startTIme;
            uint64_t frameCount = 0;

            // Use high-resolution waitable timer if available (Windows 10 1803+)
            HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!timer) {
                // Fallback to normal waitable timer
                timer = CreateWaitableTimerW(NULL, FALSE, NULL);
            }

            while (m_running) {
                {
                    winrt::com_ptr<ID3D11Texture2D> texture;
                    {
                        std::lock_guard<std::mutex> lock(m_frameMutex);
                        texture = m_cacheTexture;
                    }

                    if (texture) {
                        // Use a relative timestamp for the encoder based on constant FPS.
                        uint64_t ts = (frameCount++) * (10000000ULL / m_fps); // 100ns units
                        ProcessFrame(texture.get(), ts);
                    }
                }

                nextFrameTime += frameDuration;
                auto now = std::chrono::steady_clock::now();
                if (nextFrameTime > now) {
                    auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(nextFrameTime - now);
                    if (timer) {
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -(delay.count() / 100); // 100ns units, negative for relative
                        SetWaitableTimer(timer, &dueTime, 0, NULL, NULL, FALSE);
                        WaitForSingleObject(timer, INFINITE);
                    } else {
                        std::this_thread::sleep_for(delay);
                    }
                }
            }

            if (timer) {
                CloseHandle(timer);
            }
        }

        void ProcessFrame(ID3D11Texture2D* wgcTexture, uint64_t timestamp) {
            if (!m_encoder) return;

            // Protect D3D11 Context
            winrt::com_ptr<ID3D11Multithread> mt;
            if (SUCCEEDED(m_d3dContext->QueryInterface(IID_PPV_ARGS(mt.put())))) {
                mt->Enter();
            }

            ID3D11Texture2D* texture = wgcTexture;

            HRESULT hr = S_OK;

            // Initialize Video Processor if not done
            if (!m_videoProcessorEnumerator) {
                D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
                contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
                contentDesc.InputFrameRate.Numerator = m_fps;
                contentDesc.InputFrameRate.Denominator = 1;
                contentDesc.InputWidth = m_srcWidth;
                contentDesc.InputHeight = m_srcHeight;
                contentDesc.OutputFrameRate.Numerator = m_fps;
                contentDesc.OutputFrameRate.Denominator = 1;
                contentDesc.OutputWidth = m_width;
                contentDesc.OutputHeight = m_height;
                contentDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

                winrt::com_ptr<ID3D11VideoDevice> videoDevice;
                if (SUCCEEDED(m_d3dDevice->QueryInterface(IID_PPV_ARGS(videoDevice.put())))) {
                    videoDevice->CreateVideoProcessorEnumerator(&contentDesc, m_videoProcessorEnumerator.put());
                    if (m_videoProcessorEnumerator) {
                        videoDevice->CreateVideoProcessor(m_videoProcessorEnumerator.get(), 0, m_videoProcessor.put());
                    }
                    m_d3dContext->QueryInterface(IID_PPV_ARGS(m_videoContext.put()));
                }
            }

            // Create Output Texture for Encoder Input
            if (!m_nv12Texture) {
                D3D11_TEXTURE2D_DESC desc = {};
                desc.Width = m_width;
                desc.Height = m_height;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = (m_inputFormat == MFVideoFormat_RGB32) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_NV12;
                desc.SampleDesc.Count = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET;
                hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, m_nv12Texture.put());
                if (FAILED(hr)) {
                    std::cerr << "Failed to create input texture: " << std::hex << hr << std::endl;
                    return;
                }
            }

            if (m_inputFormat == MFVideoFormat_NV12 && m_videoProcessor && m_videoContext && m_nv12Texture) {
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
                inputViewDesc.FourCC = 0;
                inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                inputViewDesc.Texture2D.MipSlice = 0;
                inputViewDesc.Texture2D.ArraySlice = 0;

                winrt::com_ptr<ID3D11VideoProcessorInputView> inputView;
                winrt::com_ptr<ID3D11VideoDevice> videoDevice = m_d3dDevice.as<ID3D11VideoDevice>();
                videoDevice->CreateVideoProcessorInputView(texture, m_videoProcessorEnumerator.get(), &inputViewDesc, inputView.put());

                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
                outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                outputViewDesc.Texture2D.MipSlice = 0;

                winrt::com_ptr<ID3D11VideoProcessorOutputView> outputView;
                videoDevice->CreateVideoProcessorOutputView(m_nv12Texture.get(), m_videoProcessorEnumerator.get(), &outputViewDesc, outputView.put());

                D3D11_VIDEO_PROCESSOR_STREAM stream = {};
                stream.Enable = TRUE;
                stream.pInputSurface = inputView.get();

                m_videoContext->VideoProcessorBlt(m_videoProcessor.get(), outputView.get(), 0, 1, &stream);
            } else if (m_inputFormat == MFVideoFormat_RGB32) {
                m_d3dContext->CopyResource(m_nv12Texture.get(), texture);
            }

            // 2. Wrap NV12 Texture in MF Sample
            winrt::com_ptr<IMFSample> pSample;
            hr = MFCreateSample(pSample.put());
            if (FAILED(hr)) return;

            pSample->SetSampleTime(timestamp);
            pSample->SetSampleDuration(10000000ULL / m_fps);
            // Low latency hint
            pSample->SetUINT32(MF_LOW_LATENCY, TRUE);

            winrt::com_ptr<IMFMediaBuffer> pBuffer;
            hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), m_nv12Texture.get(), 0, FALSE, pBuffer.put());
            if (FAILED(hr)) return;

            pSample->AddBuffer(pBuffer.get());

            // 3. Process Input
            hr = m_encoder->ProcessInput(0, pSample.get(), 0);
            if (hr == MF_E_NOTACCEPTING) {
                // Encoder full, need to drain output first
            } else if (FAILED(hr)) {
                return;
            }

            // 4. Get Output from Encoder
            while (true) {
                MFT_OUTPUT_STREAM_INFO streamInfo = {};
                hr = m_encoder->GetOutputStreamInfo(0, &streamInfo);
                if (FAILED(hr)) break;

                MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
                outputDataBuffer.dwStreamID = 0;
                
                winrt::com_ptr<IMFSample> outSample;
                if (!(streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                    hr = MFCreateSample(outSample.put());
                    if (FAILED(hr)) break;
                    winrt::com_ptr<IMFMediaBuffer> outBuffer;
                    hr = MFCreateMemoryBuffer(streamInfo.cbSize, outBuffer.put());
                    if (FAILED(hr)) break;
                    hr = outSample->AddBuffer(outBuffer.get());
                    if (FAILED(hr)) break;
                    outputDataBuffer.pSample = outSample.get(); // MFT will fill this
                }

                DWORD status = 0;
                hr = m_encoder->ProcessOutput(0, 1, &outputDataBuffer, &status);
                
                if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                    break;
                } else if (hr == S_OK) {
                    if (outputDataBuffer.pSample) {
                        IMFMediaBuffer* pOutBuffer = nullptr;
                        outputDataBuffer.pSample->GetBufferByIndex(0, &pOutBuffer);
                        if (pOutBuffer) {
                            BYTE* pData = nullptr;
                            DWORD cbData = 0;
                            pOutBuffer->Lock(&pData, nullptr, &cbData);
                            if (m_callback) {
                                m_callback(pData, cbData, timestamp);
                            }
                            pOutBuffer->Unlock();
                            pOutBuffer->Release();
                        }
                    }
                    if (outputDataBuffer.pSample && (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                        outputDataBuffer.pSample->Release();
                    }
                    if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
                } else {
                    if (outputDataBuffer.pSample && (streamInfo.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                        outputDataBuffer.pSample->Release();
                    }
                    if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
                    break;
                }
            }

            if (mt) {
                mt->Leave();
            }
        }

        void RequestIDR() {
            if (!m_encoder) return;
            winrt::com_ptr<ICodecAPI> codecApi;
            if (SUCCEEDED(m_encoder->QueryInterface(IID_PPV_ARGS(codecApi.put())))) {
                VARIANT var = {};
                var.vt = VT_UI4;
                var.ulVal = 1;
                HRESULT hr = codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
                if (FAILED(hr)) {
                    std::cerr << "Failed to force keyframe: " << std::hex << hr << std::endl;
                }
            }
        }

    private:
        HMONITOR m_monitor;
        int m_bitrate;
        int m_fps;
        int m_gopSize;
        bool m_borderRequired;
        uint32_t m_width, m_height;
        uint32_t m_srcWidth, m_srcHeight;
        int64_t m_startTimeNs = 0;

        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_device{ nullptr };
        winrt::com_ptr<ID3D11Device> m_d3dDevice;
        winrt::com_ptr<ID3D11DeviceContext> m_d3dContext;
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };

        winrt::Windows::System::DispatcherQueueController m_controller{ nullptr };
        winrt::Windows::System::DispatcherQueue m_dispatcherQueue{ nullptr };

        winrt::com_ptr<IMFTransform> m_encoder;
        winrt::com_ptr<IMFTransform> m_converter;

        winrt::com_ptr<ID3D11VideoDevice> m_videoDevice;
        winrt::com_ptr<ID3D11VideoContext> m_videoContext;
        winrt::com_ptr<ID3D11VideoProcessor> m_videoProcessor;
        winrt::com_ptr<ID3D11VideoProcessorEnumerator> m_videoProcessorEnumerator;
        winrt::com_ptr<ID3D11Texture2D> m_nv12Texture;

        DataCallback m_callback;
        std::atomic<bool> m_closed;
        std::atomic<bool> m_running;
        GUID m_inputFormat;

        std::thread m_workerThread;
        std::mutex m_frameMutex;
        winrt::com_ptr<ID3D11Texture2D> m_cacheTexture;
        winrt::com_ptr<ID3D11Texture2D> m_lastWgcFrame;
    };

    std::vector<MonitorInfo> DesktopCapture::EnumerateMonitors() {
        std::vector<MonitorInfo> monitors;
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hmon, HDC, LPRECT, LPARAM lparam) {
            auto& monitors = *reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
            MONITORINFOEX mi = { sizeof(mi) };
            if (GetMonitorInfo(hmon, &mi)) {
                MonitorInfo info;
                info.handle = hmon;
                // mi.szDevice is TCHAR, which is CHAR in this context. 
                // We need to convert it to wstring.
                std::string name(mi.szDevice);
                info.name = std::wstring(name.begin(), name.end());
                info.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                monitors.push_back(info);
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&monitors));
        return monitors;
    }

    DesktopCapture::DesktopCapture() : m_impl(new Impl()) {}
    DesktopCapture::~DesktopCapture() { delete m_impl; }
    bool DesktopCapture::Initialize(HMONITOR monitor, int bitrate, int fps, int gopSize, int width, int height, bool borderRequired) { return m_impl->Initialize(monitor, bitrate, fps, gopSize, width, height, borderRequired); }
    void DesktopCapture::Start(DataCallback callback) { m_impl->Start(callback); }
    void DesktopCapture::Stop() { m_impl->Stop(); }
    void DesktopCapture::RequestIDR() { 
        m_impl->RequestIDR(); 
    }

}

extern "C" {
    using namespace CaptureLib;

    void* CreateDesktopCapture() {
        return new DesktopCapture();
    }

    void DestroyDesktopCapture(void* capture) {
        delete reinterpret_cast<DesktopCapture*>(capture);
    }

    bool InitializeCapture(void* capture, HMONITOR monitor, int bitrate, int fps, int gopSize, int width, int height, bool borderRequired) {
        return reinterpret_cast<DesktopCapture*>(capture)->Initialize(monitor, bitrate, fps, gopSize, width, height, borderRequired);
    }

    void StartCapture(void* capture, CaptureDataCallback callback) {
        reinterpret_cast<DesktopCapture*>(capture)->Start([callback](const uint8_t* data, size_t size, uint64_t timestamp) {
            if (callback) {
                callback(data, size, timestamp);
            }
        });
    }

    void StopCapture(void* capture) {
        reinterpret_cast<DesktopCapture*>(capture)->Stop();
    }

    void RequestIDR(void* capture) {
        reinterpret_cast<DesktopCapture*>(capture)->RequestIDR();
    }

    int GetMonitors(CMonitorInfo* monitors, int maxCount) {
        auto list = DesktopCapture::EnumerateMonitors();
        int count = (int)list.size();
        if (count > maxCount) count = maxCount;
        for (int i = 0; i < count; i++) {
            monitors[i].handle = list[i].handle;
            wcsncpy_s(monitors[i].name, list[i].name.c_str(), _TRUNCATE);
            monitors[i].isPrimary = list[i].isPrimary;
        }
        return count;
    }
}
