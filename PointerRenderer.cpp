#include "pch.h"
#include "PointerRenderer.h"
#include "PointerRenderer.g.cpp"

namespace winrt::PointerDemo::implementation
{
    using namespace Windows::Foundation;
    using namespace Windows::UI::Core;
    using namespace Windows::UI::Xaml;
    using namespace Windows::UI::Xaml::Controls;
    using namespace Windows::UI::Xaml::Interop;

    static std::once_flag s_dependencyPropInitFlag;

    static inline IAsyncAction SetSwapChainOnPanelAsync(com_ptr<IDXGISwapChain> swapChain, SwapChainPanel panel)
    {
        // Make sure we're on the XAML thread
        if (!panel.Dispatcher().HasThreadAccess())
        {
            co_await resume_foreground(panel.Dispatcher());
        }

        check_hresult(panel.as<ISwapChainPanelNative>()->SetSwapChain(swapChain.get()));
    }

    PointerRenderer::PointerRenderer()
        : m_width{ ActualWidth() }
        , m_height{ ActualHeight() }
        , m_readySignal{ ::CreateEventW(nullptr, true, false, nullptr) }
    {
        InitializeDependencyProperties();

        // Initialize render thread
        m_renderThread = std::thread(
            [this]() 
            { 
                init_apartment();

                CreateRenderingResources();
                RegisterForInputEvents();
                Run();

                uninit_apartment();
            });

        m_sizeChangedSubscription = SizeChanged(auto_revoke, [this](auto const&, auto const&) { OnSizeChanged(); });
    }

    PointerRenderer::~PointerRenderer()
    {
        if (m_renderThread.joinable())
        {
            m_running = false;
            m_renderThread.join();
        }
    }

    void PointerRenderer::InitializeDependencyProperties()
    {
        std::call_once(s_dependencyPropInitFlag, [&]()
            {
                s_captureInputOnPressProperty = DependencyProperty::Register(
                    L"CaptureInputOnPress",
                    xaml_typename<bool>(),
                    xaml_typename<PointerDemo::PointerRenderer>(),
                    PropertyMetadata(box_value(false), { &OnCaptureInputOnPressChanged }));
            });
    }

    void PointerRenderer::OnCaptureInputOnPressChanged(DependencyObject const& target, DependencyPropertyChangedEventArgs const& args)
    {
        target.as<implementation::PointerRenderer>()->SetCaptureInputOnPress(unbox_value<bool>(args.NewValue()));
    }

    void PointerRenderer::Run() noexcept
    {
        // Signal that the render thread has begun
        SetEvent(m_readySignal.get());

        // Run
        while (m_running)
        {
            // Pump input
            m_inputSource.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);

            // If we've got a frame ready signal, wait for it to be
            // set before beginning the render work
            if (m_frameReadySignal)
            {
                ::WaitForSingleObject(m_frameReadySignal.get(), INFINITE);
            }

            // Grab latest frame
            com_ptr<IDXGISurface> currentSurface;
            check_hresult(m_swapChain->GetBuffer(0, IID_PPV_ARGS(currentSurface.put())));

            // Render pointers
            Render(currentSurface.get());

            // Present
            check_hresult(m_swapChain->Present(0, 0));
        }
    }

    void PointerRenderer::CreateRenderingResources()
    {
        // Create D3D device
        UINT d3dFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        d3dFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        check_hresult(D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            d3dFlags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            m_d3dDevice.put(),
            nullptr,
            nullptr));

        // Create DXGI swap chain
        UINT dxgiFlags = 0;
#ifdef _DEBUG
        dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif
        com_ptr<IDXGIFactory2> factory;
        check_hresult(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(factory.put())));

        DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.Width = std::max(1u, static_cast<UINT>(m_width));
        swapChainDesc.Height = std::max(1u, static_cast<UINT>(m_height));
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        check_hresult(factory->CreateSwapChainForComposition(m_d3dDevice.get(), &swapChainDesc, nullptr, m_swapChain.put()));
        m_frameReadySignal = handle{ m_swapChain.as<IDXGISwapChain2>()->GetFrameLatencyWaitableObject() };

        // Set the swap chain for the XAML panel
        //
        // Note, can't use the IAsyncAction.get() waiter because the render thread is also
        // ASTA, so synchronize manually by attaching to the Completed event.
        handle swapChainSyncEvent{ ::CreateEventW(nullptr, false, false, nullptr) };
        auto setSwapChainAction = SetSwapChainOnPanelAsync(m_swapChain, *this);
        setSwapChainAction.Completed([&swapChainSyncEvent](auto const&, auto const&)
            {
                SetEvent(swapChainSyncEvent.get());
            });
        WaitForSingleObject(swapChainSyncEvent.get(), INFINITE);

        // Create D2D context
        com_ptr<ID2D1Factory1> d2dFactory;
        check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.put()));
        check_hresult(d2dFactory->CreateDevice(m_d3dDevice.as<IDXGIDevice>().get(), m_d2dDevice.put()));
        check_hresult(m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dDeviceContext.put()));

        // Create D2D resources
        check_hresult(m_d2dDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Blue), m_hoverBrush.put()));
        check_hresult(m_d2dDeviceContext->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), m_pressedBrush.put()));        
    }

    void PointerRenderer::RegisterForInputEvents()
    {
        m_inputSource = CreateCoreIndependentInputSource(CoreInputDeviceTypes::Mouse | CoreInputDeviceTypes::Pen | CoreInputDeviceTypes::Touch);
        m_pointerEnteredSubscription = m_inputSource.PointerEntered(auto_revoke, [this](auto const&, auto const& args) { OnPointerEntered(args); });
        m_pointerExitedSubscription = m_inputSource.PointerExited(auto_revoke, [this](auto const&, auto const& args) { OnPointerExited(args); });
        m_pointerMovedSubscription = m_inputSource.PointerMoved(auto_revoke, [this](auto const&, auto const& args) { OnPointerMoved(args); });
        m_pointerPressedSubscription = m_inputSource.PointerPressed(auto_revoke, [this](auto const&, auto const& args) { OnPointerPressed(args); });
        m_pointerReleasedSubscription = m_inputSource.PointerReleased(auto_revoke, [this](auto const&, auto const& args) { OnPointerReleased(args); });
    }

    void PointerRenderer::Render(IDXGISurface* renderTarget)
    {
        // Setup the DXGI back buffer as a D2D1 bitmap render target
        auto& bitmap = m_swapChainSurfaceBitmaps[renderTarget];
        if (bitmap == nullptr)
        {
            D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            check_hresult(m_d2dDeviceContext->CreateBitmapFromDxgiSurface(renderTarget, bitmapProps, bitmap.put()));
        }
        m_d2dDeviceContext->SetTarget(bitmap.get());

        // Draw the scene
        {
            m_d2dDeviceContext->BeginDraw();

            // Clear the background
            m_d2dDeviceContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));

            // Draw an indicator for each position
            for (auto const& [id, pointerData] : m_currentPointers)
            {
                auto pointerRect = D2D1::RectF(
                    pointerData.m_currentPosition.X - 20,   // left
                    pointerData.m_currentPosition.Y - 20,   // top
                    pointerData.m_currentPosition.X + 20,   // right
                    pointerData.m_currentPosition.Y + 20);  // bottom

                m_d2dDeviceContext->FillRectangle(
                    pointerRect,
                    pointerData.m_pressed ? m_pressedBrush.get() : m_hoverBrush.get());
            }

            check_hresult(m_d2dDeviceContext->EndDraw());
        }
    }

    fire_and_forget PointerRenderer::OnSizeChanged()
    {
        // Grab the new width & height
        auto newWidth = ActualWidth();
        auto newHeight = ActualHeight();

        // Ensure the render thread has started up
        co_await resume_on_signal(m_readySignal.get());

        // Switch from the XAML thread to the render thread
        co_await resume_foreground(m_inputSource.Dispatcher());

        // Resize the swap chain (if needed)
        if (newWidth != m_width || newHeight != m_height)
        {
            // Release any lingering references to the swap chain buffers
            m_d2dDeviceContext->SetTarget(nullptr);
            m_swapChainSurfaceBitmaps.clear();

            check_hresult(m_swapChain->ResizeBuffers(
                2,
                std::max(1u, static_cast<UINT>(newWidth)),
                std::max(1u, static_cast<UINT>(newHeight)),
                DXGI_FORMAT_UNKNOWN,
                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));

            m_width = newWidth;
            m_height = newHeight;
        }
    }

    void PointerRenderer::OnPointerEntered(PointerEventArgs const& args)
    {
        auto currentPoint = args.CurrentPoint();

        PointerData newPointer{};
        newPointer.m_type = currentPoint.PointerDevice().PointerDeviceType();
        newPointer.m_pressed = currentPoint.IsInContact();
        newPointer.m_currentPosition = currentPoint.Position();

        // Insert the new pointer into our list
        auto const [_, inserted] = m_currentPointers.insert({ currentPoint.PointerId(), newPointer });
        if (!inserted)
        {
            throw hresult_illegal_state_change(L"A pointer with that ID already exists in the map");
        }

        args.Handled(true);
    }

    void PointerRenderer::OnPointerExited(PointerEventArgs const& args)
    {
        auto currentPoint = args.CurrentPoint();

        auto itr = m_currentPointers.find(currentPoint.PointerId());
        if (itr != m_currentPointers.end())
        {
            m_currentPointers.erase(currentPoint.PointerId());
        }
        else
        {
            OutputDebugStringW(L"Untracked pointer exit\n");
        }

        args.Handled(true);
    }

    void PointerRenderer::OnPointerMoved(PointerEventArgs const& args)
    {
        auto currentPoint = args.CurrentPoint();

        auto itr = m_currentPointers.find(currentPoint.PointerId());
        if (itr == m_currentPointers.end())
        {
            throw hresult_illegal_state_change(L"An untracked pointer was moved");
        }

        itr->second.m_currentPosition = currentPoint.Position();

        args.Handled(true);
    }

    void PointerRenderer::OnPointerPressed(PointerEventArgs const& args)
    {
        auto currentPoint = args.CurrentPoint();

        if (!currentPoint.IsInContact())
        {
            throw hresult_illegal_state_change(L"A pressed pointer is reporting as not pressed");
        }

        auto itr = m_currentPointers.find(currentPoint.PointerId());
        if (itr == m_currentPointers.end())
        {
            throw hresult_illegal_state_change(L"An untracked pointer was pressed");
        }

        itr->second.m_pressed = true;

        // Handle system capture if enabled
        if (m_captureOnPress && !m_inputSource.HasCapture())
        {
            m_inputSource.SetPointerCapture();
        }

        args.Handled(true);
    }

    void PointerRenderer::OnPointerReleased(PointerEventArgs const& args)
    {
        auto currentPoint = args.CurrentPoint();

        if (currentPoint.IsInContact())
        {
            throw hresult_illegal_state_change(L"A released pointer is reporting as pressed");
        }

        auto itr = m_currentPointers.find(currentPoint.PointerId());
        if (itr == m_currentPointers.end())
        {
            throw hresult_illegal_state_change(L"An untracked pointer was released");
        }

        itr->second.m_pressed = false;

        // If all pointers have been released, release the system capture
        bool allReleased = std::any_of(m_currentPointers.begin(), m_currentPointers.end(), [&](auto const& pointerData) { return !pointerData.second.m_pressed; });
        if (m_inputSource.HasCapture() && allReleased)
        {
            m_inputSource.ReleasePointerCapture();
        }

        args.Handled(true);
    }

    fire_and_forget PointerRenderer::SetCaptureInputOnPress(bool capture)
    {
        // Wait for the render thread to finish spinning up
        co_await resume_on_signal(m_readySignal.get());

        // Ensure we're running on the rendering thread
        if (!m_inputSource.Dispatcher().HasThreadAccess())
        {
            co_await resume_foreground(m_inputSource.Dispatcher());
        }

        m_captureOnPress = capture;

        // Cancel any existing capture
        if (!m_captureOnPress)
        {
            m_inputSource.ReleasePointerCapture();
        }
    }
}