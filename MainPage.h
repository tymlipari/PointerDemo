#pragma once

#include "MainPage.g.h"

#include "PointerRenderer.h"

namespace winrt::PointerDemo::implementation
{
    struct MainPage : MainPageT<MainPage>
    {
        MainPage();
    };
}

namespace winrt::PointerDemo::factory_implementation
{
    struct MainPage : MainPageT<MainPage, implementation::MainPage>
    {
    };
}
