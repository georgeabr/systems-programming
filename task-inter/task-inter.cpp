// hotkey.c
// Minimal global hotkey interceptor for NT 3.51
// Build as Win32 GUI app in VC++ 4.x

#include <windows.h>

#define ID_HOTKEY 1

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_HOTKEY && wParam == ID_HOTKEY) {
        // Launch your program here
        WinExec("C:\\MYTOOLS\\MYAPP.EXE", SW_SHOWNORMAL);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int APIENTRY WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    WNDCLASS wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = "HotKeyWin";
    RegisterClass(&wc);

    HWND hWnd = CreateWindow(
        "HotKeyWin", "HotKeyWin",
        WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        HWND_DESKTOP, NULL, hInst, NULL
    );

    // Register Ctrl+Esc
    if (!RegisterHotKey(hWnd, ID_HOTKEY, MOD_CONTROL, VK_ESCAPE)) {
        MessageBox(NULL, "Failed to register hotkey", "Error", MB_OK);
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
