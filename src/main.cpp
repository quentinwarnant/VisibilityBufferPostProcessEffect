#include "renderer.h"

#include <windows.h>
#include <memory>
#include <stdexcept>

static std::unique_ptr<Renderer> g_renderer;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (g_renderer)
    {
        const LRESULT handled = g_renderer->HandleMessage(window, message, wParam, lParam);
        if (handled != 0)
            return handled;
    }

    switch (message)
    {
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_SIZE:
        if (g_renderer && wParam != SIZE_MINIMIZED)
            g_renderer->Resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProc(window, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    try
    {
        const wchar_t* className = L"VBufferAnimatedSailWindow";
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);

        RECT bounds{0, 0, 1440, 900};
        AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
        HWND window = CreateWindowExW(0, className, L"DX12 Animated Sail - Deferred Effects",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            nullptr, nullptr, instance, nullptr);
        if (!window)
            throw std::runtime_error("CreateWindowEx failed");

        g_renderer = std::make_unique<Renderer>(window, 1440, 900);
        ShowWindow(window, show == SW_HIDE ? SW_SHOWNORMAL : show);
        UpdateWindow(window);

        MSG message{};
        while (message.message != WM_QUIT)
        {
            if (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessage(&message);
            }
            else
            {
                g_renderer->Render();
            }
        }
        g_renderer.reset();
        UnregisterClassW(className, instance);
        return static_cast<int>(message.wParam);
    }
    catch (const std::exception& exception)
    {
        MessageBoxA(nullptr, exception.what(), "Fatal error", MB_OK | MB_ICONERROR);
        return 1;
    }
}
