#pragma once

#include "PointerRenderer.g.h"

namespace winrt::PointerDemo::implementation
{
    struct PointerRenderer : PointerRendererT<PointerRenderer>
    {
        PointerRenderer();
        ~PointerRenderer();

        static inline Windows::UI::Xaml::DependencyProperty CaptureInputOnPressProperty() { return s_captureInputOnPressProperty; }
        inline bool CaptureInputOnPress() const { return unbox_value<bool>(GetValue(CaptureInputOnPressProperty())); }
        inline void CaptureInputOnPress(bool newValue) { SetValue(CaptureInputOnPressProperty(), box_value(newValue)); }

    private:
        // DependencyProperty handling
        static void InitializeDependencyProperties();
        static void OnCaptureInputOnPressChanged(Windows::UI::Xaml::DependencyObject const& target, Windows::UI::Xaml::DependencyPropertyChangedEventArgs const& args);

        // Rendering
        void CreateRenderingResources();
        void RegisterForInputEvents();
        void Run() noexcept;
        void Render(IDXGISurface* renderTarget);

        // Event handlers
        fire_and_forget OnSizeChanged();
        void OnPointerEntered(Windows::UI::Core::PointerEventArgs const& args);
        void OnPointerExited(Windows::UI::Core::PointerEventArgs const& args);
        void OnPointerMoved(Windows::UI::Core::PointerEventArgs const& args);
        void OnPointerPressed(Windows::UI::Core::PointerEventArgs const& args);
        void OnPointerReleased(Windows::UI::Core::PointerEventArgs const& args);

        // Internal helpers
        fire_and_forget SetCaptureInputOnPress(bool capture);

    private:
        // XAML
        inline static Windows::UI::Xaml::DependencyProperty s_captureInputOnPressProperty{ nullptr };

        // Render thread
        std::thread m_renderThread;
        std::atomic_bool m_running{ true };
        handle m_readySignal;

        // Rendering
        handle m_frameReadySignal;
        com_ptr<IDXGISwapChain1> m_swapChain;
        com_ptr<IDXGIOutput> m_outputDevice;
        com_ptr<ID3D11Device> m_d3dDevice;
        com_ptr<ID2D1Device> m_d2dDevice;
        com_ptr<ID2D1DeviceContext> m_d2dDeviceContext;
        std::map<IDXGISurface*, com_ptr<ID2D1Bitmap1>> m_swapChainSurfaceBitmaps;

        com_ptr<ID2D1SolidColorBrush> m_hoverBrush;
        com_ptr<ID2D1SolidColorBrush> m_pressedBrush;

        double m_width;
        double m_height;
        Windows::UI::Xaml::Controls::SwapChainPanel::SizeChanged_revoker m_sizeChangedSubscription;

        // Input
        bool m_captureOnPress{ false };
        Windows::UI::Core::CoreIndependentInputSource m_inputSource{ nullptr };
        Windows::UI::Core::CoreIndependentInputSource::PointerEntered_revoker m_pointerEnteredSubscription;
        Windows::UI::Core::CoreIndependentInputSource::PointerExited_revoker m_pointerExitedSubscription;
        Windows::UI::Core::CoreIndependentInputSource::PointerMoved_revoker m_pointerMovedSubscription;
        Windows::UI::Core::CoreIndependentInputSource::PointerPressed_revoker m_pointerPressedSubscription;
        Windows::UI::Core::CoreIndependentInputSource::PointerReleased_revoker m_pointerReleasedSubscription;

        struct PointerData
        {
            Windows::Devices::Input::PointerDeviceType m_type;
            bool m_pressed{ false };
            Windows::Foundation::Point m_currentPosition;
        };

        std::unordered_map<uint32_t, PointerData> m_currentPointers{};
    };
}

namespace winrt::PointerDemo::factory_implementation
{
    struct PointerRenderer : PointerRendererT<PointerRenderer, implementation::PointerRenderer>
    {
    };
}