// nt351_taskbar_final.c
// Build: Visual C++ 4.2, Win32 GUI (ANSI)
// NT 3.51 "taskbar-like" bar with stable ordering, owner-draw buttons (left-aligned, active pressed look),
// small icons, owner-draw Start button sized to its text, and a visible separator (no tooltip/comctl32).

#include <windows.h>
#include <stdio.h>

/* ---------------- IDs ---------------- */
#define IDT_REFRESH            1
#define ID_START_BTN           100
#define ID_SEP_STATIC          150
#define ID_FIRST_TASK_BTN      200
#define MAX_TASKS              64
#define ID_CLOCK_STATIC 300

/* -------------- Layout ---------------- */
#define BAR_HEIGHT             28
#define BTN_H                  24
#define BTN_MIN_W              100
#define BTN_SPACING            6
#define MARGIN                 6
#define SEP_W_DEFAULT          6
#define TEXT_PAD_L             6
#define TEXT_PAD_R             8
#define ICON_SIZE              16
#define ICON_PAD               4

/* -------------- Globals --------------- */
static const char g_szClass[] = "NT351TaskbarClass";

static HWND g_hwndBar = NULL;
static HWND g_hStart  = NULL;
static HWND g_hSep    = NULL;
static HWND g_taskBtns[MAX_TASKS];
static HWND g_taskWnds[MAX_TASKS];
static DWORD g_taskPIDs[MAX_TASKS];
static char  g_taskTitles[MAX_TASKS][256];
static HICON g_taskIcons[MAX_TASKS];
static int   g_taskCount = 0;
static HWND g_hClock = NULL;

/* Owner-draw/metrics state */
static int  g_activeIndex = -1;
static int  g_startW      = 0;
static int  g_sepW        = SEP_W_DEFAULT;

/* Prototypes */
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

/* ---------------- Utility ---------------- */

static void SafeZeroMemory(void* p, UINT cb)
{
    BYTE* b;
    UINT i;
    b = (BYTE*)p;
    for (i = 0; i < cb; i++) b[i] = 0;
}

static void DockBottomTopmost(HWND hwndBar)
{
    int cx, cy;
    cx = GetSystemMetrics(SM_CXSCREEN);
    cy = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwndBar, HWND_TOPMOST, 0, cy - BAR_HEIGHT, cx, BAR_HEIGHT, SWP_SHOWWINDOW);
}

/* Decide if a window should appear as a task */
static BOOL IsEligibleTask(HWND hWnd)
{
    LONG style;
    char title[256];

    if (!IsWindow(hWnd)) return FALSE;
    if (!IsWindowVisible(hWnd)) return FALSE;
    if (IsIconic(hWnd)) return FALSE;

    title[0] = '\0';
    if (GetWindowTextA(hWnd, title, sizeof(title)) == 0) return FALSE;
    if (GetWindow(hWnd, GW_OWNER) != NULL) return FALSE;
    if (hWnd == g_hwndBar) return FALSE;

    style = GetWindowLongA(hWnd, GWL_STYLE);
    if (style & WS_CHILD) return FALSE;

    return TRUE;
}

/* ---------------- Task enumeration and stable ordering ---------------- */

typedef struct TaskScan {
    HWND  wnd[MAX_TASKS];
    char  title[MAX_TASKS][256];
    DWORD pid[MAX_TASKS];
    int   count;
} TaskScan;

static BOOL CALLBACK EnumCollectorProc(HWND hWnd, LPARAM lParam)
{
    TaskScan* scan;
    DWORD pid;
    char t[256];

    scan = (TaskScan*)lParam;
    if (!scan) return FALSE;
    if (scan->count >= MAX_TASKS) return FALSE;

    if (IsEligibleTask(hWnd))
    {
        scan->wnd[scan->count] = hWnd;

        t[0] = '\0';
        GetWindowTextA(hWnd, t, sizeof(t));
        if (t[0] == '\0') lstrcpyA(t, "(Untitled)");
        lstrcpynA(scan->title[scan->count], t, sizeof(scan->title[scan->count]));

        pid = 0;
        GetWindowThreadProcessId(hWnd, &pid);
        scan->pid[scan->count] = pid;

        scan->count++;
    }
    return TRUE;
}

/* Try to fetch a small icon for a window; fall back to class icon */
static HICON GetSmallIconForWindow(HWND hWnd)
{
    HICON hIcon;
    hIcon = (HICON)SendMessageA(hWnd, WM_GETICON, (WPARAM)ICON_SMALL, 0);
    if (!hIcon) hIcon = (HICON)GetClassLongA(hWnd, GCL_HICONSM);
    if (!hIcon) hIcon = (HICON)GetClassLongA(hWnd, GCL_HICON);
    return hIcon;
}

/* Create/update button for slot i */
static void EnsureButtonForSlot(HWND hwndBar, int i)
{
    if (g_taskBtns[i] == NULL)
    {
        g_taskBtns[i] = CreateWindowA(
            "BUTTON", "",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
            0, 0, BTN_MIN_W, BTN_H,
            hwndBar, (HMENU)(ID_FIRST_TASK_BTN + i),
                                      GetModuleHandle(NULL), NULL
        );
    }
    else
    {
        ShowWindow(g_taskBtns[i], SW_SHOW);
    }
}


/* Paint clock */
static void PaintClock(LPDRAWITEMSTRUCT dis)
{
    SYSTEMTIME st;
    char buf[16];
    GetLocalTime(&st);
    wsprintf(buf, "%02d:%02d", st.wHour, st.wMinute);

    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;

    HBRUSH hbr = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
    FillRect(hdc, &rc, hbr);
    DeleteObject(hbr);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(COLOR_BTNTEXT));

    DrawTextA(hdc, buf, -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}


/* Refresh tasks without reordering existing buttons */
static void RefreshTasks(HWND hwndBar)
{
    TaskScan scan;
    HWND  newWnds[MAX_TASKS];
    char  newTitles[MAX_TASKS][256];
    DWORD newPIDs[MAX_TASKS];
    HICON newIcons[MAX_TASKS];
    int   newCount;
    int   i, j;
    HWND  w;
    HWND  hActive;

    SafeZeroMemory(&scan, sizeof(scan));
    EnumWindows((WNDENUMPROC)EnumCollectorProc, (LPARAM)&scan);

    newCount = 0;

    /* Keep existing in-order if still present */
    for (i = 0; i < g_taskCount && newCount < MAX_TASKS; i++)
    {
        w = g_taskWnds[i];
        if (!w) continue;

        for (j = 0; j < scan.count; j++)
        {
            if (scan.wnd[j] == w)
            {
                newWnds[newCount] = w;
                lstrcpynA(newTitles[newCount], scan.title[j], sizeof(newTitles[newCount]));
                newPIDs[newCount] = scan.pid[j];
                newIcons[newCount] = GetSmallIconForWindow(w);
                newCount++;
                break;
            }
        }
    }

    /* Append new ones not already in newWnds */
    for (j = 0; j < scan.count && newCount < MAX_TASKS; j++)
    {
        int kk;
        BOOL already;
        w = scan.wnd[j];

        already = FALSE;
        for (kk = 0; kk < newCount; kk++)
        {
            if (newWnds[kk] == w) { already = TRUE; break; }
        }
        if (!already)
        {
            newWnds[newCount] = w;
            lstrcpynA(newTitles[newCount], scan.title[j], sizeof(newTitles[newCount]));
            newPIDs[newCount] = scan.pid[j];
            newIcons[newCount] = GetSmallIconForWindow(w);
            newCount++;
        }
    }

    /* Apply new list to globals and ensure buttons exist */
    for (i = 0; i < newCount; i++)
    {
        g_taskWnds[i] = newWnds[i];
        g_taskPIDs[i] = newPIDs[i];
        lstrcpynA(g_taskTitles[i], newTitles[i], sizeof(g_taskTitles[i]));
        g_taskIcons[i] = newIcons[i];
        EnsureButtonForSlot(hwndBar, i);
    }

    /* Hide trailing slots */
    for (i = newCount; i < MAX_TASKS; i++)
    {
        if (g_taskBtns[i] != NULL) ShowWindow(g_taskBtns[i], SW_HIDE);
        g_taskWnds[i] = NULL;
        g_taskPIDs[i] = 0;
        g_taskTitles[i][0] = '\0';
        g_taskIcons[i] = NULL;
    }

    g_taskCount = newCount;

    /* Track active window index without reordering */
    hActive = GetForegroundWindow();
    g_activeIndex = -1;
    for (i = 0; i < g_taskCount; i++)
    {
        if (g_taskWnds[i] == hActive) { g_activeIndex = i; break; }
    }

    InvalidateRect(hwndBar, NULL, TRUE);
}

/* Lay out Start + separator + task buttons inside the bar */
static void ArrangeButtons(HWND hwndBar)
{
    RECT rc;
    int w, h;
    int x, y;
    int available, per;
    int i;

    GetClientRect(hwndBar, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;

    /* Start button sized to measured text width */
    if (g_hStart != NULL)
        MoveWindow(g_hStart, MARGIN, (h - BTN_H) / 2, g_startW, BTN_H, TRUE);

    /* Separator immediately after Start */
    x = MARGIN + g_startW;
    y = (h - BTN_H) / 2;
    if (g_hSep != NULL)
    {
        MoveWindow(g_hSep, x + (BTN_SPACING / 2), y, g_sepW, BTN_H, TRUE);
    }

    /* Task buttons begin after separator + spacing */
    x = MARGIN + g_startW + BTN_SPACING + g_sepW + BTN_SPACING;

    available = w - x - MARGIN - 60; // leave space for clock
    per = BTN_MIN_W;

    if (g_taskCount > 0)
    {
        int needed;
        needed = g_taskCount * (BTN_MIN_W + BTN_SPACING) - BTN_SPACING;
        if (needed > available)
        {
            per = (available + BTN_SPACING) / g_taskCount - BTN_SPACING;
            if (per < 60) per = 60;
        }
    }

    for (i = 0; i < g_taskCount; i++)
    {
        if (g_taskBtns[i]) MoveWindow(g_taskBtns[i], x, y, per, BTN_H, TRUE);
        x += per + BTN_SPACING;
    }

    /* clock */
    if (g_hClock != NULL)
    {
        MoveWindow(g_hClock, w - MARGIN - 60, (h - BTN_H) / 2 - 2, 60, BTN_H + 4, TRUE);
    
    }


}

/* Activate/restore a window */
static void ActivateTaskWindow(HWND hWnd)
{
    if (!IsWindow(hWnd)) return;
    if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
}

/* Minimize a window */
static void MinimizeTaskWindow(HWND hWnd)
{
    if (!IsWindow(hWnd)) return;
    ShowWindow(hWnd, SW_MINIMIZE);
}

/* Bring Program Manager to front or launch it */
static BOOL LaunchStartTarget(HWND hwndBar)
{
    HWND hProgman;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    hProgman = FindWindowA("Progman", NULL);
    if (hProgman) { SetForegroundWindow(hProgman); return TRUE; }

    SafeZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    SafeZeroMemory(&pi, sizeof(pi));

    if (CreateProcessA(NULL, "progman.exe", NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return TRUE;
    }
    return FALSE;
}

/* ---------------- Owner-draw helpers ---------------- */

static void Paint3DBorder(HDC hdc, const RECT* rc, BOOL pressed)
{
    COLORREF clrHili, clrDk;
    HPEN penTL, penBR, old;

    clrHili = GetSysColor(COLOR_3DHILIGHT);
    clrDk   = GetSysColor(COLOR_3DDKSHADOW);

    penTL = CreatePen(PS_SOLID, 1, pressed ? clrDk : clrHili);
    penBR = CreatePen(PS_SOLID, 1, pressed ? clrHili : clrDk);
    old   = (HPEN)SelectObject(hdc, penTL);

    MoveToEx(hdc, rc->left, rc->bottom-1, NULL); LineTo(hdc, rc->left, rc->top);
    LineTo(hdc, rc->right-1, rc->top);

    SelectObject(hdc, penBR);
    MoveToEx(hdc, rc->right-1, rc->top, NULL); LineTo(hdc, rc->right-1, rc->bottom-1);
    LineTo(hdc, rc->left, rc->bottom-1);

    SelectObject(hdc, old);
    DeleteObject(penTL); DeleteObject(penBR);
}

static void PaintTaskButton(LPDRAWITEMSTRUCT dis)
{
    UINT id;
    int idx;
    RECT rc, rt, rf;
    HDC hdc;
    BOOL isActive, isPressed, isFocus, isDisabled;
    COLORREF clrFace, clrShadow, clrText;
    HBRUSH hbrBg;

    id  = dis->CtlID;
    idx = (int)(id - ID_FIRST_TASK_BTN);
    rc  = dis->rcItem;
    hdc = dis->hDC;

    if (idx < 0 || idx >= g_taskCount) return;

    isActive  = (idx == g_activeIndex);
    isPressed = ((dis->itemState & ODS_SELECTED) != 0) || isActive;
    isFocus   = ((dis->itemState & ODS_FOCUS) != 0);
    isDisabled= ((dis->itemState & ODS_DISABLED) != 0);

    clrFace   = GetSysColor(COLOR_BTNFACE);
    clrShadow = GetSysColor(COLOR_3DSHADOW);
    clrText   = GetSysColor(isDisabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT);

    /* Background */
    hbrBg = CreateSolidBrush(isPressed ? clrShadow : clrFace);
    FillRect(hdc, &rc, hbrBg);
    DeleteObject(hbrBg);

    /* 3D border */
    Paint3DBorder(hdc, &rc, isPressed);

    /* Text/icon rect; shift if pressed */
    rt = rc;
    rt.left  += TEXT_PAD_L + (isPressed ? 1 : 0);
    rt.right -= TEXT_PAD_R;
    rt.top   += (isPressed ? 1 : 0);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, clrText);

    /* Icon if present */
    if (g_taskIcons[idx])
    {
        int iconX, iconY;
        iconX = rt.left;
        iconY = rt.top + ((rt.bottom - rt.top - ICON_SIZE) / 2);
        if (iconY < rt.top) iconY = rt.top;

        DrawIconEx(hdc, iconX, iconY, g_taskIcons[idx],
                   ICON_SIZE, ICON_SIZE, 0, NULL, DI_NORMAL);

        rt.left += ICON_SIZE + ICON_PAD;
    }

    DrawTextA(hdc,
              g_taskTitles[idx],
              -1,
              &rt,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    if (isFocus)
    {
        rf = rc;
        rf.left  += 3; rf.right -= 3; rf.top += 3; rf.bottom -= 3;
        DrawFocusRect(hdc, &rf);
    }
}

static void PaintStartButton(LPDRAWITEMSTRUCT dis)
{
    RECT rc, rt, rf;
    HDC hdc;
    BOOL isPressed, isFocus, isDisabled;
    COLORREF clrFace, clrShadow, clrText;
    HBRUSH hbrBg;

    rc  = dis->rcItem;
    hdc = dis->hDC;

    isPressed = ((dis->itemState & ODS_SELECTED) != 0);
    isFocus   = ((dis->itemState & ODS_FOCUS) != 0);
    isDisabled= ((dis->itemState & ODS_DISABLED) != 0);

    clrFace   = GetSysColor(COLOR_BTNFACE);
    clrShadow = GetSysColor(COLOR_3DSHADOW);
    clrText   = GetSysColor(isDisabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT);

    /* Background */
    hbrBg = CreateSolidBrush(isPressed ? clrShadow : clrFace);
    FillRect(hdc, &rc, hbrBg);
    DeleteObject(hbrBg);

    /* 3D border */
    Paint3DBorder(hdc, &rc, isPressed);

    /* Text rect; small left pad, pressed offset */
    rt = rc;
    rt.left  += TEXT_PAD_L + (isPressed ? 1 : 0);
    rt.right -= TEXT_PAD_R;
    rt.top   += (isPressed ? 1 : 0);

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, clrText);

    DrawTextA(hdc, "Start", -1, &rt,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    if (isFocus)
    {
        rf = rc;
        rf.left  += 3; rf.right -= 3; rf.top += 3; rf.bottom -= 3;
        DrawFocusRect(hdc, &rf);
    }
}

static void PaintSeparator(LPDRAWITEMSTRUCT dis)
{
    RECT rc;
    HDC hdc;
    COLORREF clrHili, clrDk;
    HPEN penLight, penDark, old;
    int mid;

    rc  = dis->rcItem;
    hdc = dis->hDC;

    /* Draw a visible 3D vertical divider */
    clrHili = GetSysColor(COLOR_3DHILIGHT);
    clrDk   = GetSysColor(COLOR_3DDKSHADOW);

    penDark  = CreatePen(PS_SOLID, 1, clrDk);
    penLight = CreatePen(PS_SOLID, 1, clrHili);

    mid = (rc.left + rc.right) / 2;

    old = (HPEN)SelectObject(hdc, penDark);
    MoveToEx(hdc, mid, rc.top + 2, NULL);
    LineTo(hdc, mid, rc.bottom - 2);

    SelectObject(hdc, penLight);
    MoveToEx(hdc, mid + 1, rc.top + 2, NULL);
    LineTo(hdc, mid + 1, rc.bottom - 2);

    SelectObject(hdc, old);
    DeleteObject(penLight);
    DeleteObject(penDark);
}

/* ---------------- Window proc ---------------- */

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    int id, idx, i;
    POINT pt;
    RECT r;
    LPDRAWITEMSTRUCT dis;

    switch (uMsg)
    {
        case WM_CREATE:
        {
            HDC hdc;
            SIZE sz;

            DockBottomTopmost(hwnd);

            /* Start button: owner-draw */
            g_hStart = CreateWindowA(
                "BUTTON", "",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                0, 0, 10, BTN_H,
                hwnd, (HMENU)ID_START_BTN, GetModuleHandle(NULL), NULL
            );

            /* Measure "Start" and compute width with padding */
            hdc = GetDC(hwnd);
            sz.cx = 0; sz.cy = 0;
            GetTextExtentPoint32A(hdc, "Start", 5, &sz);
            ReleaseDC(hwnd, hdc);
            g_startW = sz.cx + TEXT_PAD_L + TEXT_PAD_R;
            if (g_startW < 40) g_startW = 40; /* minimal width safeguard */

                /* Separator: owner-draw STATIC (widened) */
                g_hSep = CreateWindowA(
                    "STATIC", "",
                    WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                    0, 0, g_sepW, BTN_H,
                    hwnd, (HMENU)ID_SEP_STATIC, GetModuleHandle(NULL), NULL
                );

            /* Create the clock button */
            g_hClock = CreateWindowA(
                "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
                0, 0, 60, BTN_H,
                hwnd, (HMENU)ID_CLOCK_STATIC, GetModuleHandle(NULL), NULL
            );



            /* Poll tasks frequently to reflect active changes */
            SetTimer(hwnd, IDT_REFRESH, 500, NULL);

            RefreshTasks(hwnd);
            ArrangeButtons(hwnd);
        }
        return 0;

        case WM_SIZE:
            ArrangeButtons(hwnd);
            return 0;

        case WM_TIMER:
            if (wParam == IDT_REFRESH)
            {
                RefreshTasks(hwnd);
                ArrangeButtons(hwnd);

                if (g_hClock)
                {
                    SYSTEMTIME st;
                    char buf[16];
                    GetLocalTime(&st);
                    wsprintf(buf, "%02d:%02d", st.wHour, st.wMinute);
                    InvalidateRect(g_hClock, NULL, TRUE);
                }
                return 0;
            }


        case WM_LBUTTONDBLCLK:
        {
            POINT pt;
            pt.x = LOWORD(lParam);
            pt.y = HIWORD(lParam);

            HWND hChild = ChildWindowFromPoint(hwnd, pt);
            if (hChild == g_hClock)
            {
                PostQuitMessage(0);
                return 0;
            }
        }
        break;




        case WM_COMMAND:
            id = LOWORD(wParam);

            if (id == ID_START_BTN)
            {
                if (HIWORD(wParam) == BN_CLICKED)
                {
                    LaunchStartTarget(hwnd);
                }
                return 0;
            }
            else if (id >= ID_FIRST_TASK_BTN && id < ID_FIRST_TASK_BTN + MAX_TASKS)
            {
                idx = id - ID_FIRST_TASK_BTN;
                if (idx >= 0 && idx < g_taskCount)
                {
                    HWND hWndTask = g_taskWnds[idx];
                    if (HIWORD(wParam) == BN_CLICKED)
                    {
                        ActivateTaskWindow(hWndTask);
                        RefreshTasks(hwnd);
                        ArrangeButtons(hwnd);
                    }
                }
                return 0;
            }
            else if (id == ID_CLOCK_STATIC)
            {
                if (HIWORD(wParam) == STN_DBLCLK)
                {
                    PostQuitMessage(0);
                    return 0;
                }
            }
            break;

        case WM_DRAWITEM:
            dis = (LPDRAWITEMSTRUCT)lParam;
            id  = dis->CtlID;

            if (id == ID_CLOCK_STATIC)
            {
                PaintClock(dis);
                return TRUE;
            }

            if (id == ID_START_BTN)
            {
                PaintStartButton(dis);
                return TRUE;
            }
            else if (id == ID_SEP_STATIC)
            {
                PaintSeparator(dis);
                return TRUE;
            }
            else if (id >= ID_FIRST_TASK_BTN && id < ID_FIRST_TASK_BTN + MAX_TASKS)
            {
                PaintTaskButton(dis);
                return TRUE;
            }
            break;

        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
        {
            GetCursorPos(&pt);
            for (i = 0; i < g_taskCount; i++)
            {
                if (!g_taskBtns[i]) continue;
                GetWindowRect(g_taskBtns[i], &r);
                if (PtInRect(&r, pt))
                {
                    MinimizeTaskWindow(g_taskWnds[i]);
                    RefreshTasks(hwnd);
                    ArrangeButtons(hwnd);
                    break;
                }
            }
        }
        return 0;

        case WM_DESTROY:
        {
            KillTimer(hwnd, IDT_REFRESH);
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

/* ---------------- Entry point ---------------- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSA wc;
    MSG msg;

    SafeZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = g_szClass;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;

    if (!RegisterClassA(&wc)) return 0;

    g_hwndBar = CreateWindowExA(
        WS_EX_TOOLWINDOW,
        g_szClass,
        "Taskbar",
        WS_POPUP | WS_VISIBLE,
        0, 0, GetSystemMetrics(SM_CXSCREEN), BAR_HEIGHT,
                                NULL, NULL, hInst, NULL
    );

    if (!g_hwndBar) return 0;

    ShowWindow(g_hwndBar, nCmdShow);
    UpdateWindow(g_hwndBar);
    DockBottomTopmost(g_hwndBar);

    while (GetMessageA(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return (int)msg.wParam;
}
