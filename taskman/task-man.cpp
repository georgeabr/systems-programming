// feck task_manager_sample.cpp
// Revision 95 (NT 3.51 / VC++ 4.2):
// - FINAL, ABSOLUTE FIX for C2180/C2086 on line 1197 (WM_COMMAND block).
// - The initialized declaration 'HINSTANCE hInst = (HINSTANCE)GetWindowLongA(hwnd, GWL_HINSTANCE);'
//   was still causing the compiler to fail due to its poor handling of scope before a switch.
// - FIX: Split the declaration (HINSTANCE hInst;) and the initialization (hInst = ...) into two separate statements.

#include <windows.h>
#include <stdio.h>
#include <winnt.h>
#include <string.h>
#include <stdlib.h>
#include <commdlg.h>

// --- Conditional Type Definitions ---
#ifndef INT_PTR
typedef long INT_PTR;
#endif
#ifndef ULONG_PTR
typedef ULONG ULONG_PTR;
#endif

// --- Global Constants ---
#define IDC_PROCESS_LIST 101
#define ID_END_TASK      102
#define ID_NEW_TASK      104
#define ID_EXIT          105
#define IDC_STATUS_LINE1 106 // Used for Uptime and RAM (Total Commit Charge)
#define IDC_SORT_COMBO   107
#define IDC_STATUS_LINE2 108 // Used for Page File (PageF)
#define IDT_REFRESH      1

// Dialog box control IDs for New Task Dialog
#define IDC_CMD_TEXT     201
#define IDC_CMD_LABEL    202
#define ID_BROWSE        203

// --- DESCRIPTION LENGTH (40 chars) ---
#define MAX_DESC_CHARS 40

// Columns: Name(15) | Desc(40) | PID(6) | Thr(4) | Mem(7) | CPU(3) | Age(9 right-aligned) = 84 chars
#define LIST_CONTENT_WIDTH 84
#define RENDER_PAD_SPACES 8
#define HORIZ_EXTENT_PAD  24

// --- Sorting Columns ---
#define SORT_BY_PROCESS_NAME 0
#define SORT_BY_CPU          1
#define SORT_BY_MEM          2
#define SORT_BY_PID          3
#define SORT_BY_AGE          4

// --- ComboBox Constants ---
#define COMBO_EXPANDED_HEIGHT 200

// --- Alignment Helper Macros ---
#define CALC_ALIGN_DWORD(p) (((ULONG_PTR)(p) + 3) & ~3)
#define CALC_ALIGN_WORD(p)  (((ULONG_PTR)(p) + 1) & ~1)

// --- NT System Information Definitions (Required for NtQuerySystemInformation) ---

typedef enum _SYSTEM_INFORMATION_CLASS {
    // SystemBasicInformation is no longer needed/defined here, using GlobalMemoryStatus()
    SystemBasicInformation = 0,
    SystemPerformanceInformation = 2,
    SystemProcessInformation = 5
} SYSTEM_INFORMATION_CLASS;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *P_UNICODE_STRING;

typedef ULONG SIZE_T;
typedef LONG KPRIORITY;

typedef struct _SYSTEM_PROCESS_INFORMATION {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER Reserved1[3];
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;

    ULONG HandleCount;
    ULONG SessionId;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T WorkingSetSize;
    SIZE_T PeakWorkingSetSize;

} SYSTEM_PROCESS_INFORMATION, *PSYSTEM_PROCESS_INFORMATION;

typedef struct _SYSTEM_PERFORMANCE_INFORMATION {
    LARGE_INTEGER IdleTime;
    ULONG ReadOperationCount;
    ULONG WriteOperationCount;
    ULONG OtherOperationCount;
    ULONG AvailablePages;
    ULONG CommittedPages;
    ULONG CommitLimit;
} SYSTEM_PERFORMANCE_INFORMATION, *PSYSTEM_PERFORMANCE_INFORMATION;




// --- Custom Data Structures ---

typedef struct _FIND_WINDOW_PARAMS {
    DWORD dwPID;
    HWND hWnd;
} FIND_WINDOW_PARAMS, *PFIND_WINDOW_PARAMS;

typedef struct _PROCESS_ITEM_DATA {
    DWORD dwPID;
    char szProcessName[16];
    char szDescription[MAX_DESC_CHARS + 1];
    ULONGLONG LastKernelTime;
    ULONGLONG LastUserTime;
} PROCESS_ITEM_DATA, *PPROCESS_ITEM_DATA;

typedef struct _SORTABLE_ITEM {
    PPROCESS_ITEM_DATA pData;
    DWORD dwCPU;
    DWORD dwWorkingSetKB;
    DWORD dwThreads;
    DWORD dwAgeSeconds;
    char  szAgeHHMMSS[9];
} SORTABLE_ITEM, *PSORTABLE_ITEM;

// --- NT Function Pointer ---
typedef LONG (WINAPI *PFN_NTQUERYSYSTEMINFORMATION)(
    SYSTEM_INFORMATION_CLASS SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength
);

PFN_NTQUERYSYSTEMINFORMATION pfnNtQuerySystemInformation = NULL;

// --- Globals ---
static ULONGLONG g_ullLastQueryTime = 0;
static int g_iHorizontalExtent = 0;
static DWORD g_dwTotalPhysicalMB = 0; // Total physical RAM in MB

static DWORD s_dwSelectedPID = 0;
static int s_iCurrentTopIndex = 0;
static int s_iHorizontalOffset = 0;
static int s_iSortColumn = SORT_BY_PROCESS_NAME;

static HANDLE g_hSingleInstanceMutex = NULL;
const char CLASS_NAME[] = "TaskManagerSampleClass";
const char MUTEX_NAME[] = "NT_Task_Manager_Sample_Mutex_49";

WNDPROC g_pfnOriginalListBoxProc = NULL;

// Global handles for main controls
static HWND hList, hEndTaskBtn, hNewTaskBtn, hExitBtn, hSortCombo;

// Global handles for status controls
static HWND hStatusText1, hStatusText2;


// --- Prototypes ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
INT_PTR CALLBACK RunNewTaskDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK ListBoxSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void RefreshProcessList(HWND hList, HWND hwndParent);
BOOL LoadNtFunctions();
BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam);
void SwitchToProcessWindow(DWORD dwPID);
void CleanupListItemData(HWND hList);
BOOL EndProcessWithConfirmation(HWND hwnd, const PPROCESS_ITEM_DATA pData);
void GetProcessDescription(DWORD dwPID, char* pszDescription, int maxLen);
BOOL ExecuteNewTask(HWND hwnd, char* szCommand);
void UpdateStatusDisplay(HWND hStatusText1, HWND hStatusText2, HWND hwndParent);
int __cdecl CompareProcessData(const void* a, const void* b);
DWORD GetProcessAgeSeconds(LARGE_INTEGER createTime);
void FormatAgeHHMMSS(DWORD ageSeconds, char out[9]);
void RunNewTaskDialog(HWND hwndParent, HINSTANCE hInst);


// --- ListBox Subclassing Implementation ---
LRESULT CALLBACK ListBoxSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    HWND hParent = GetParent(hwnd);

    if (uMsg == WM_KEYDOWN)
    {
        if (wParam == VK_ESCAPE)
        {
            PostQuitMessage(0);
            return 0;
        }

        if (wParam == VK_RETURN)
        {
            int sel = (int)SendMessage(hwnd, LB_GETCURSEL, 0, 0);
            if (sel >= 2 && sel != LB_ERR) {
                PPROCESS_ITEM_DATA pData = (PPROCESS_ITEM_DATA)SendMessage(hwnd, LB_GETITEMDATA, sel, 0);
                if (pData != (PPROCESS_ITEM_DATA)LB_ERR && pData != NULL && pData->dwPID > 0)
                {
                    SwitchToProcessWindow(pData->dwPID);
                }
            }
            return 0;
        }

        if (wParam == VK_DELETE)
        {
            int sel = (int)SendMessage(hwnd, LB_GETCURSEL, 0, 0);
            if (sel >= 2 && sel != LB_ERR) {
                PPROCESS_ITEM_DATA pData = (PPROCESS_ITEM_DATA)SendMessage(hwnd, LB_GETITEMDATA, sel, 0);
                if (pData != (PPROCESS_ITEM_DATA)LB_ERR && pData != NULL) {
                    PROCESS_ITEM_DATA temp = *pData;
                    if (EndProcessWithConfirmation(hParent, &temp)) {
                        RefreshProcessList(hwnd, hParent);
                    }
                }
            } else {
                MessageBox(hParent,
                           "Please select a process to end. (Note: The first two rows are headers.)",
                           "Selection Error",
                           MB_OK | MB_ICONEXCLAMATION);
            }
            return 0;
        }
    }

    return CallWindowProcA((FARPROC)g_pfnOriginalListBoxProc, hwnd, uMsg, wParam, lParam);
}


// --- Sorting implementation ---
int __cdecl CompareProcessData(const void* a, const void* b)
{
    PSORTABLE_ITEM pItemA = (PSORTABLE_ITEM)a;
    PSORTABLE_ITEM pItemB = (PSORTABLE_ITEM)b;

    BOOL bIsIdleA = (pItemA->pData->dwPID == 0);
    BOOL bIsIdleB = (pItemB->pData->dwPID == 0);

    if (bIsIdleA && !bIsIdleB) return -1;
    if (!bIsIdleA && bIsIdleB) return 1;
    if (bIsIdleA && bIsIdleB) return 0;

    switch (s_iSortColumn)
    {
        case SORT_BY_CPU:
            return (int)(pItemB->dwCPU - pItemA->dwCPU); // Descending
        case SORT_BY_MEM:
            return (int)(pItemB->dwWorkingSetKB - pItemA->dwWorkingSetKB); // Descending
        case SORT_BY_PID:
            return (int)(pItemA->pData->dwPID - pItemB->pData->dwPID);    // Ascending
        case SORT_BY_AGE:
            return (int)(pItemB->dwAgeSeconds - pItemA->dwAgeSeconds);    // Descending (older first)
        case SORT_BY_PROCESS_NAME:
        default:
            return _stricmp(pItemA->pData->szProcessName, pItemB->pData->szProcessName); // Ascending
    }
}

// --- Status Display (Uptime, RAM/Commit, PageF) ---
void UpdateStatusDisplay(HWND hStatusText1, HWND hStatusText2, HWND hwndParent)
{
    // A. Query Uptime
    DWORD dwTickCount = GetTickCount();

    ULONGLONG total_seconds = dwTickCount / 1000;
    ULONGLONG seconds = total_seconds % 60;
    ULONGLONG total_minutes = total_seconds / 60;
    ULONGLONG minutes = total_minutes % 60;
    ULONGLONG total_hours = total_minutes / 60;
    ULONGLONG hours = total_hours % 24;
    ULONGLONG days = total_hours / 24;

    // B. Query Memory Information (SystemPerformanceInformation)
    SYSTEM_PERFORMANCE_INFORMATION perfInfo = {0};
    ULONG retLen;
    LONG status = pfnNtQuerySystemInformation(
        SystemPerformanceInformation,
        &perfInfo,
        sizeof(perfInfo),
                                              &retLen
    );


    // C. Calculations using Commit Charge (NT Task Manager Style)
    // 4096 bytes per page. PAGES_PER_MB = 256.
    const ULONG PAGES_PER_MB = 1024 * 1024 / 4096;
    // Total Physical Pages relies on the reliably set g_dwTotalPhysicalMB
    ULONG totalPhysicalPages = g_dwTotalPhysicalMB * PAGES_PER_MB;

    DWORD dwCommitChargeMB = 0;   // Used RAM (Total Commit Charge)
    DWORD dwCommitLimitMB = 0;    // Total RAM (Total Commit Limit)
    DWORD dwUsedPageFileMB = 0;   // Page File Used (Commit Charge backed by Page File)
    DWORD dwTotalPageFileMB = 0;  // Page File Total (Page File Maximum)
    DWORD ramUsedMB = 0;
    DWORD ramTotalMB = 0;

    DWORD pageFileTotalMB = 0;
    DWORD pageFileUsedMB = 0;

    // Use GlobalMemoryStatus for RAM
    MEMORYSTATUS ms;
    ZeroMemory(&ms, sizeof(ms));
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);

    ramTotalMB = (DWORD)(ms.dwTotalPhys / (1024*1024));
    ramUsedMB  = (DWORD)((ms.dwTotalPhys - ms.dwAvailPhys) / (1024*1024));


    //to debug
    /*
    char buf[256];
    wsprintf(buf,
             "status=%ld retLen=%lu\nAvailPages=%lu\nCommittedPages=%lu\nCommitLimit=%lu",
             status, retLen,
             perfInfo.AvailablePages,
             perfInfo.CommittedPages,
             perfInfo.CommitLimit);
    MessageBox(hwndParent, buf, "Debug: NtQuerySystemInformation", MB_OK);
    */


    // D. Format and Display
    char szStatusLine1[200];
    char szStatusLine2[200];

    if (ms.dwTotalPageFile != 0 && ms.dwAvailPageFile != 0) {
        pageFileTotalMB = (DWORD)(ms.dwTotalPageFile / (1024*1024));
        pageFileUsedMB  = pageFileTotalMB - (DWORD)(ms.dwAvailPageFile / (1024*1024));
        sprintf(szStatusLine2, "PageF: %lu/%lu M", pageFileUsedMB, pageFileTotalMB);
    } else {
        sprintf(szStatusLine2, "PageF: n/a");
    }


    // Status Line 1 format: "Up: D:HH:MM:SS | RAM: used/totalM"
    sprintf(szStatusLine1,
            "Up: %lu:%02lu:%02lu:%02lu RAM: %4lu/%4lu M",
            (ULONG)days, (ULONG)hours, (ULONG)minutes, (ULONG)seconds,
            ramUsedMB, ramTotalMB);
//            1256, 1024);

  /*    sprintf(szStatusLine1,
            "Up: %lu:%02lu:%02lu:%02lu | RAM: %lu/%lu M",
            (ULONG)days, (ULONG)hours, (ULONG)minutes, (ULONG)seconds,
            dwCommitChargeMB, dwCommitLimitMB

    );
    */

    // Status Line 2 format: "PageF: used/total M"
    /*
    sprintf(szStatusLine2,
            "PageF: %lu/%lu M",
            dwUsedPageFileMB, dwTotalPageFileMB
    );
    */

    SetWindowTextA(hStatusText1, szStatusLine1);
    //SetWindowTextA(hStatusText2, szStatusLine2);
}

// --- Age calculation and formatting ---
DWORD GetProcessAgeSeconds(LARGE_INTEGER createTime)
{
    FILETIME ftNow;
    // FIX C2065: Corrected misspelling from GetSystemTimeAsFileFile to GetSystemTimeAsFileTime
    GetSystemTimeAsFileTime(&ftNow);
    ULONGLONG now = *(ULONGLONG*)&ftNow;
    ULONGLONG create = *(ULONGLONG*)&createTime;
    if (now <= create) return 0;
    return (DWORD)((now - create) / 10000000UL); // 10,000,000 ticks per second
}

void FormatAgeHHMMSS(DWORD ageSeconds, char out[9])
{
    DWORD hh = ageSeconds / 3600;
    DWORD mm = (ageSeconds % 3600) / 60;
    DWORD ss = ageSeconds % 60;
    if (hh > 99) hh = 99;
    sprintf(out, "%02lu:%02lu:%02lu", (ULONG)hh, (ULONG)mm, (ULONG)ss);
}

// --- New Task Dialog ---
BOOL ExecuteNewTask(HWND hwnd, char* szCommand)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessA(
        NULL,
        szCommand,
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        NULL,
        &si,
        &pi)
    ) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        return TRUE;
    }
    else
    {
        char msg[200];
        sprintf(msg, "Failed to start programme: %s\n\nError Code: %lu", szCommand, GetLastError());
        MessageBox(hwnd, msg, "Launch Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }
}

INT_PTR CALLBACK RunNewTaskDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static HWND hEdit;

    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            RECT rcDlg, rcParent;
            GetWindowRect(hDlg, &rcDlg);
            GetWindowRect(GetParent(hDlg), &rcParent);

            SetWindowPos(hDlg, NULL,
                         rcParent.left + (rcParent.right - rcParent.left - (rcDlg.right - rcDlg.left)) / 2,
                         rcParent.top + (rcParent.bottom - rcParent.top - (rcDlg.bottom - rcDlg.bottom)) / 2,
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
        }
        hEdit = GetDlgItem(hDlg, IDC_CMD_TEXT);
        SetFocus(hEdit);
        return FALSE;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
                case IDOK:
                {
                    char szCommand[MAX_PATH] = "";
                    GetWindowTextA(hEdit, szCommand, MAX_PATH);

                    if (strlen(szCommand) > 0)
                    {
                        if (ExecuteNewTask(GetParent(hDlg), szCommand))
                        {
                            PostMessage(GetParent(hDlg), WM_TIMER, IDT_REFRESH, 0);
                        }
                    }
                    else
                    {
                        MessageBox(hDlg, "Please enter a programme name.", "Input Required", MB_OK | MB_ICONEXCLAMATION);
                        return TRUE;
                    }

                    EndDialog(hDlg, IDOK);
                }
                return TRUE;

                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    return TRUE;

                case ID_BROWSE:
                {
                    OPENFILENAMEA ofn;
                    char szFile[MAX_PATH] = "";
                    char szFilter[] = "Programmes (*.exe;*.com;*.bat)\0*.exe;*.com;*.bat\0All Files (*.*)\0*.*\0";

                    ZeroMemory(&ofn, sizeof(ofn));
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hDlg;
                    ofn.lpstrFile = szFile;
                    ofn.nMaxFile = sizeof(szFile);
                    ofn.lpstrFilter = szFilter;
                    ofn.nFilterIndex = 1;
                    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

                    if (GetOpenFileNameA(&ofn) == TRUE)
                    {
                        SetWindowTextA(hEdit, szFile);
                    }
                }
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// --- New function to encapsulate the complex dialog template creation ---
void RunNewTaskDialog(HWND hwndParent, HINSTANCE hInst)
{
    BYTE buffer[512];
    LPWORD p = (LPWORD)buffer;

    // FIX for C2374: Declare pItem once at the beginning
    LPDLGITEMTEMPLATE pItem;

    p = (LPWORD)CALC_ALIGN_DWORD(p);
    LPDLGTEMPLATE pdt = (LPDLGTEMPLATE)p;
    p += sizeof(DLGTEMPLATE) / sizeof(WORD);

    pdt->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    pdt->dwExtendedStyle = 0;
    pdt->cdit = 5;
    pdt->x = 0; pdt->y = 0; pdt->cx = 250; pdt->cy = 80;

    *p++ = 0x0000;
    *p++ = 0x0000;

    WCHAR szTitle[] = L"Create New Task";
    lstrcpyW((LPWSTR)p, szTitle);
    p += wcslen(szTitle) + 1;

    *p++ = 8;
    WCHAR szFont[] = L"MS Sans Serif";
    lstrcpyW((LPWSTR)p, szFont);
    p += wcslen(szFont) + 1;

    // Label
    p = (LPWORD)CALC_ALIGN_DWORD(p);
    pItem = (LPDLGITEMTEMPLATE)p; // Assignment
    p += sizeof(DLGITEMTEMPLATE) / sizeof(WORD);

    pItem->style = WS_CHILD | WS_VISIBLE | SS_LEFT | WS_GROUP;
    pItem->dwExtendedStyle = 0;
    pItem->x = 7; pItem->y = 7; pItem->cx = 236; pItem->cy = 8;
    pItem->id = IDC_CMD_LABEL;

    *p++ = 0xFFFF; *p++ = 0x0082; // Static

    WCHAR szLabel[] = L"Type the name of programme, file, or document, and press ENTER.";
    lstrcpyW((LPWSTR)p, szLabel);
    p += wcslen(szLabel) + 1;
    *p++ = 0x0000;

    // Edit
    p = (LPWORD)CALC_ALIGN_DWORD(p);
    pItem = (LPDLGITEMTEMPLATE)p; // Assignment
    p += sizeof(DLGITEMTEMPLATE) / sizeof(WORD);

    pItem->style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    pItem->dwExtendedStyle = WS_EX_CLIENTEDGE;
    pItem->x = 7; pItem->y = 20; pItem->cx = 236; pItem->cy = 14;
    pItem->id = IDC_CMD_TEXT;

    *p++ = 0xFFFF; *p++ = 0x0081; // Edit

    WCHAR szEdit[] = L"";
    lstrcpyW((LPWSTR)p, szEdit);
    p += wcslen(szEdit) + 1;
    *p++ = 0x0000;

    // OK
    p = (LPWORD)CALC_ALIGN_DWORD(p);
    pItem = (LPDLGITEMTEMPLATE)p; // Assignment
    p += sizeof(DLGITEMTEMPLATE) / sizeof(WORD);

    pItem->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP;
    pItem->dwExtendedStyle = 0;
    pItem->x = 10; pItem->y = 59; pItem->cx = 70; pItem->cy = 14;
    pItem->id = IDOK;

    *p++ = 0xFFFF; *p++ = 0x0080; // Button

    WCHAR szOK[] = L"OK";
    lstrcpyW((LPWSTR)p, szOK);
    p += wcslen(szOK) + 1;
    *p++ = 0x0000;

    // Cancel
    p = (LPWORD)CALC_ALIGN_DWORD(p);
    pItem = (LPDLGITEMTEMPLATE)p; // Assignment
    p += sizeof(DLGITEMTEMPLATE) / sizeof(WORD);

    pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
    pItem->dwExtendedStyle = 0;
    pItem->x = 90; pItem->y = 59; pItem->cx = 70; pItem->cy = 14;
    pItem->id = IDCANCEL;

    *p++ = 0xFFFF; *p++ = 0x0080; // Button

    WCHAR szCancel[] = L"Cancel";
    lstrcpyW((LPWSTR)p, szCancel);
    p += wcslen(szCancel) + 1;
    *p++ = 0x0000;

    // Browse (optional)
    p = (LPWORD)CALC_ALIGN_DWORD(p);
    pItem = (LPDLGITEMTEMPLATE)p; // Assignment
    p += sizeof(DLGITEMTEMPLATE) / sizeof(WORD);

    pItem->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
    pItem->dwExtendedStyle = 0;
    pItem->x = 170; pItem->y = 59; pItem->cx = 73; pItem->cy = 14;
    pItem->id = ID_BROWSE;

    *p++ = 0xFFFF; *p++ = 0x0080; // Button

    WCHAR szBrowse[] = L"Browse...";
    lstrcpyW((LPWSTR)p, szBrowse);
    p += wcslen(szBrowse) + 1;
    *p++ = 0x0000;

    DialogBoxIndirectParam(hInst, (LPCDLGTEMPLATE)buffer, hwndParent, (DLGPROC)RunNewTaskDlgProc, 0);
    if (hList != NULL) SetFocus(hList);
}


// --- Window and Process Management Logic ---

BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
    PFIND_WINDOW_PARAMS pParams = (PFIND_WINDOW_PARAMS)lParam;
    DWORD dwProcessId = 0;

    GetWindowThreadProcessId(hWnd, &dwProcessId);

    if (dwProcessId == pParams->dwPID && IsWindowVisible(hWnd) && GetWindowTextLengthA(hWnd) > 0)
    {
        pParams->hWnd = hWnd;
        return FALSE;
    }
    return TRUE;
}

void GetProcessDescription(DWORD dwPID, char* pszDescription, int maxLen)
{
    FIND_WINDOW_PARAMS params = {0};
    params.dwPID = dwPID;

    strncpy(pszDescription, "", maxLen);
    pszDescription[maxLen - 1] = '\0';

    EnumWindows((WNDENUMPROC)EnumWindowsProc, (LPARAM)&params);

    if (params.hWnd != NULL)
    {
        char szFullTitle[512] = {0};
        GetWindowTextA(params.hWnd, szFullTitle, sizeof(szFullTitle) - 1);
        int actualTitleLen = strlen(szFullTitle);

        const int DISPLAY_LEN = maxLen - 1;
        const int ELLIPSIS_LEN = 2;
        const int SUFFIX_LEN = 14;

        if (actualTitleLen > DISPLAY_LEN && DISPLAY_LEN > ELLIPSIS_LEN + SUFFIX_LEN)
        {
            const int PREFIX_LEN = DISPLAY_LEN - ELLIPSIS_LEN - SUFFIX_LEN;

            strncpy(pszDescription, szFullTitle, PREFIX_LEN);
            pszDescription[PREFIX_LEN] = '\0';

            strcat(pszDescription, "..");
            strncat(pszDescription, szFullTitle + actualTitleLen - SUFFIX_LEN, SUFFIX_LEN);
        }
        else
        {
            strncpy(pszDescription, szFullTitle, DISPLAY_LEN);
            pszDescription[DISPLAY_LEN] = '\0';
        }
    }
    else
    {
        strncpy(pszDescription, "(No window/GUI)", maxLen - 1);
        pszDescription[maxLen - 1] = '\0';
    }
}

void SwitchToProcessWindow(DWORD dwPID)
{
    FIND_WINDOW_PARAMS params = {0};
    params.dwPID = dwPID;

    EnumWindows((WNDENUMPROC)EnumWindowsProc, (LPARAM)&params);

    if (params.hWnd != NULL)
    {
        HWND hWnd = params.hWnd;

        if (IsIconic(hWnd))
        {
            ShowWindow(hWnd, SW_RESTORE);
        }

        SetForegroundWindow(hWnd);
    }
    else
    {
        char msg[150];
        sprintf(msg, "Could not find a visible window for PID %lu.", dwPID);
        MessageBox(NULL, msg, "Switch Window Failed", MB_OK | MB_ICONEXCLAMATION);
    }
}

// --- End Task Functionality ---

BOOL EndProcessWithConfirmation(HWND hwnd, const PPROCESS_ITEM_DATA pData)
{
    if (pData == NULL) return FALSE;

    if (pData->dwPID <= 4)
    {
        char msg[150];
        sprintf(msg, "Cannot end task for critical system process: %s (PID %lu).", pData->szProcessName, pData->dwPID);
        MessageBox(hwnd, msg, "End Task Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    char confirmMsg[300];
    sprintf(confirmMsg, "WARNING: Ending the following process may cause data loss or system instability.\n\nDescription: %s\nProcess Name: %s\nProcess ID: %lu\n\nDo you wish to continue?", pData->szDescription, pData->szProcessName, pData->dwPID);

    if (MessageBox(hwnd, confirmMsg, "Confirm End Task", MB_YESNO | MB_ICONWARNING) != IDYES)
    {
        return FALSE;
    }

    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pData->dwPID);

    if (hProcess == NULL)
    {
        char errorMsg[200];
        sprintf(errorMsg, "Failed to open process (PID %lu). Error code: %lu\n\nThis usually indicates insufficient user privileges.", pData->dwPID, GetLastError());
        MessageBox(hwnd, errorMsg, "Termination Failed", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    if (TerminateProcess(hProcess, 0))
    {
        CloseHandle(hProcess);
        return TRUE;
    }
    else
    {
        char errorMsg[200];
        sprintf(errorMsg, "Failed to terminate process (PID %lu). Error code: %lu", pData->dwPID, GetLastError());
        MessageBox(hwnd, errorMsg, "Termination Failed", MB_OK | MB_ICONERROR);
        CloseHandle(hProcess);
        return FALSE;
    }
}

// --- Process Listing Logic (NTDLL approach) ---

BOOL LoadNtFunctions()
{
    HMODULE hNtdll = LoadLibraryA("ntdll.dll");

    if (hNtdll != NULL) {
        pfnNtQuerySystemInformation = (PFN_NTQUERYSYSTEMINFORMATION)GetProcAddress(hNtdll, "NtQuerySystemInformation");
    }

    return (pfnNtQuerySystemInformation != NULL);
}

void CleanupListItemData(HWND hList)
{
    int count = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; i++)
    {
        PPROCESS_ITEM_DATA pData = (PPROCESS_ITEM_DATA)SendMessage(hList, LB_GETITEMDATA, i, 0);
        if (pData != (PPROCESS_ITEM_DATA)LB_ERR && pData != NULL)
        {
            HeapFree(GetProcessHeap(), 0, pData);
        }
        SendMessage(hList, LB_SETITEMDATA, i, (LPARAM)LB_ERR);
    }
}

void RefreshProcessList(HWND hList, HWND hwndParent)
{
    // 1. Save selection and scroll positions
    int iCurrentSelection = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);

    if (iCurrentSelection >= 2 && iCurrentSelection != LB_ERR) {
        PPROCESS_ITEM_DATA pSelectedData = (PPROCESS_ITEM_DATA)SendMessage(hList, LB_GETITEMDATA, iCurrentSelection, 0);
        if (pSelectedData != (PPROCESS_ITEM_DATA)LB_ERR && pSelectedData != NULL) {
            s_dwSelectedPID = pSelectedData->dwPID;
        } else {
            s_dwSelectedPID = 0;
        }
    } else {
        s_dwSelectedPID = 0;
    }

    s_iCurrentTopIndex = (int)SendMessage(hList, LB_GETTOPINDEX, 0, 0);
    s_iHorizontalOffset = GetScrollPos(hList, SB_HORZ);

    if (!LoadNtFunctions())
    {
        SendMessage(hList, LB_RESETCONTENT, 0, 0);
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)"Error: Low-level NT API function not found in NTDLL.DLL.");
        return;
    }

    // 2. Time for CPU calculation
    ULONGLONG ullCurrentTime = GetTickCount();
    ULONGLONG ullTimeDelta = (ullCurrentTime - g_ullLastQueryTime) * 10000;
    ULONGLONG ullTimeDeltaMs = ullTimeDelta / 10000;

    if (g_ullLastQueryTime == 0 || ullTimeDeltaMs == 0) {
        ullTimeDelta = 1;
        ullTimeDeltaMs = 1;
    }

    // 3. Query System Processes
    ULONG bufferSize = 0x10000;
    PVOID pBuffer = NULL;
    LONG status;

    do
    {
        if (pBuffer != NULL) {
            HeapFree(GetProcessHeap(), 0, pBuffer);
            bufferSize *= 2;
        }

        pBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bufferSize);
        if (pBuffer == NULL) {
            SendMessage(hList, LB_RESETCONTENT, 0, 0);
            SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)"Error: Failed to allocate memory.");
            return;
        }

        status = pfnNtQuerySystemInformation(SystemProcessInformation, pBuffer, bufferSize, &bufferSize);

    } while (status == 0xC0000004L); // STATUS_INFO_LENGTH_MISMATCH

    if (status != 0)
    {
        if (pBuffer != NULL) { HeapFree(GetProcessHeap(), 0, pBuffer); }
        SendMessage(hList, LB_RESETCONTENT, 0, 0);
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)"Error: NtQuerySystemInformation failed.");
        return;
    }

    // --- TEMPORARY DATA STRUCTURES FOR SORTING ---
    int iMaxProcesses = bufferSize / sizeof(SYSTEM_PROCESS_INFORMATION) + 10;
    PSORTABLE_ITEM pSortableArray = (PSORTABLE_ITEM)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, iMaxProcesses * sizeof(SORTABLE_ITEM));
    if (pSortableArray == NULL) {
        if (pBuffer != NULL) { HeapFree(GetProcessHeap(), 0, pBuffer); }
        SendMessage(hList, LB_RESETCONTENT, 0, 0);
        SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)"Error: Failed to allocate sorting memory.");
        return;
    }
    int iSortableCount = 0;

    // 4. Capture old item data and clear list
    int iOldCount = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    PPROCESS_ITEM_DATA* pOldDataArray = (PPROCESS_ITEM_DATA*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, iOldCount * sizeof(PPROCESS_ITEM_DATA));

    if (pOldDataArray != NULL)
    {
        for (int i = 0; i < iOldCount; i++) {
            pOldDataArray[i] = (PPROCESS_ITEM_DATA)SendMessage(hList, LB_GETITEMDATA, i, 0);
            SendMessage(hList, LB_SETITEMDATA, i, (LPARAM)LB_ERR);
        }
    }

    SendMessage(hList, LB_RESETCONTENT, 0, 0);

    // 5. Iterate processes, calculate metrics, and build the sortable array
    PSYSTEM_PROCESS_INFORMATION pCurrent = (PSYSTEM_PROCESS_INFORMATION)pBuffer;

    while (TRUE)
    {
        char szProcessNameA[MAX_PATH] = "";

        DWORD dwPID = (DWORD)pCurrent->UniqueProcessId;
        DWORD dwThreads = pCurrent->NumberOfThreads;
        DWORD dwWorkingSetKB = (DWORD)(pCurrent->WorkingSetSize / 1024);
        DWORD dwCPU = 0;

        // Determine name
        if (pCurrent->ImageName.Length > 0 && pCurrent->ImageName.Buffer != NULL)
        {
            WideCharToMultiByte(CP_ACP, 0, pCurrent->ImageName.Buffer, pCurrent->ImageName.Length / sizeof(WCHAR), szProcessNameA, sizeof(szProcessNameA) - 1, NULL, NULL);
            szProcessNameA[sizeof(szProcessNameA) - 1] = '\0';

            char* dot = strrchr(szProcessNameA, '.');
            if (dot != NULL) {
                if ((_stricmp(dot, ".exe") == 0) || (_stricmp(dot, ".com") == 0) || (_stricmp(dot, ".dll") == 0)) {
                    *dot = '\0';
                }
            }
        }
        else if (dwPID == 0)
        {
            strcpy(szProcessNameA, "System Idle");
            dwWorkingSetKB = 0;
        }
        else if (dwPID == 4)
        {
            strcpy(szProcessNameA, "System");
        }

        // CPU calculation
        ULONGLONG ullCurrentKernelTime = *(ULONGLONG*)&pCurrent->KernelTime;
        ULONGLONG ullCurrentUserTime   = *(ULONGLONG*)&pCurrent->UserTime;

        PPROCESS_ITEM_DATA pPrevData = NULL;
        for (int i = 0; i < iOldCount; i++)
        {
            if (pOldDataArray != NULL && pOldDataArray[i] != NULL && pOldDataArray[i]->dwPID == dwPID)
            {
                pPrevData = pOldDataArray[i];
                pOldDataArray[i] = NULL;
                break;
            }
        }

        if (pPrevData != NULL)
        {
            ULONGLONG ullKernelTimeDelta = ullCurrentKernelTime - pPrevData->LastKernelTime;
            ULONGLONG ullUserTimeDelta = ullCurrentUserTime - pPrevData->LastUserTime;
            ULONGLONG ullProcessTimeDelta = ullKernelTimeDelta + ullUserTimeDelta;

            if (ullProcessTimeDelta > 0)
            {
                dwCPU = (DWORD)((ullProcessTimeDelta * 100) / ullTimeDelta);
                if (dwCPU > 99) dwCPU = 99;
            }
        }

        // Build item
        PPROCESS_ITEM_DATA pNewData = (PPROCESS_ITEM_DATA)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(PROCESS_ITEM_DATA));
        if (pNewData != NULL)
        {
            pNewData->dwPID = dwPID;
            strncpy(pNewData->szProcessName, szProcessNameA, 15);
            pNewData->szProcessName[15] = '\0';

            GetProcessDescription(dwPID, pNewData->szDescription, sizeof(pNewData->szDescription));

            // Fallback to process name if no window
            if (_stricmp(pNewData->szDescription, "(No window/GUI)") == 0) {
                strncpy(pNewData->szDescription, pNewData->szProcessName, MAX_DESC_CHARS);
                pNewData->szDescription[MAX_DESC_CHARS] = '\0';
            }

            pNewData->LastKernelTime = ullCurrentKernelTime;
            pNewData->LastUserTime = ullCurrentUserTime;

            pSortableArray[iSortableCount].pData = pNewData;
            pSortableArray[iSortableCount].dwCPU = dwCPU;
            pSortableArray[iSortableCount].dwWorkingSetKB = dwWorkingSetKB;
            pSortableArray[iSortableCount].dwThreads = dwThreads;

            if (dwPID == 0) {
                pSortableArray[iSortableCount].dwAgeSeconds = 0;
                pSortableArray[iSortableCount].szAgeHHMMSS[0] = '\0'; // blank Age for Idle
            } else {
                DWORD ageSecs = GetProcessAgeSeconds(pCurrent->CreateTime);
                pSortableArray[iSortableCount].dwAgeSeconds = ageSecs;
                FormatAgeHHMMSS(ageSecs, pSortableArray[iSortableCount].szAgeHHMMSS);
            }

            iSortableCount++;
        }

        if (pPrevData != NULL) {
            HeapFree(GetProcessHeap(), 0, pPrevData);
        }

        if (pCurrent->NextEntryOffset == 0)
            break;

        pCurrent = (PSYSTEM_PROCESS_INFORMATION)((PBYTE)pCurrent + pCurrent->NextEntryOffset);
    }

    // 6. Sort
    qsort(pSortableArray, iSortableCount, sizeof(SORTABLE_ITEM), CompareProcessData);

    // 7. Header and separator
    char szHeader[LIST_CONTENT_WIDTH + 1] = "";
    // Age header right-aligned to 9 chars column
    sprintf(szHeader, "%-15s %-*s %6s %4s %7s%3s %9s",
            "Process Name",
            MAX_DESC_CHARS, "Programme Description",
            "PID",
            "Thr",
            "Mem(KB)",
            "CPU%",
            "Age");
    SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)szHeader);

    char szSeparator[LIST_CONTENT_WIDTH + 1] = "";
    strcpy(szSeparator, "--------------- ---------------------------------------- ------ ---- ------- ---- ---------");
    SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)szSeparator);

    // Padding string to push horizontal scroll farther, ensuring Age column fully visible
    char pad[RENDER_PAD_SPACES + 1];
    memset(pad, ' ', RENDER_PAD_SPACES);
    pad[RENDER_PAD_SPACES] = '\0';

    // 8. Rows
    for (int i = 0; i < iSortableCount; i++)
    {
        PPROCESS_ITEM_DATA pNewData = pSortableArray[i].pData;

        char szDisplayName[16];
        char szDisplayDesc[MAX_DESC_CHARS + 1];

        strncpy(szDisplayName, pNewData->szProcessName, 15);
        szDisplayName[15] = '\0';

        strncpy(szDisplayDesc, pNewData->szDescription, MAX_DESC_CHARS);
        szDisplayDesc[MAX_DESC_CHARS] = '\0';

        char szAgeField[10];
        szAgeField[0] = '\0';
        if (pSortableArray[i].szAgeHHMMSS[0] != '\0') {
            strcpy(szAgeField, pSortableArray[i].szAgeHHMMSS);
        }

        char szListItem[MAX_PATH + 200];
        // Age right-aligned in 9-char field to match header
        sprintf(szListItem,
                "%-15s %-*s %6lu %4lu %7lu%3lu%% %9s%s",
                szDisplayName,
                MAX_DESC_CHARS, szDisplayDesc,
                pNewData->dwPID,
                pSortableArray[i].dwThreads,
                pSortableArray[i].dwWorkingSetKB,
                pSortableArray[i].dwCPU,
                szAgeField,
                pad);

        int index = (int)SendMessage(hList, LB_ADDSTRING, 0, (LPARAM)szListItem);
        if (index != LB_ERR && pNewData != NULL)
        {
            SendMessage(hList, LB_SETITEMDATA, index, (LPARAM)pNewData);
        }
    }

    // 9. Cleanup
    if (pOldDataArray != NULL)
    {
        for (int i = 0; i < iOldCount; i++)
        {
            if (pOldDataArray[i] != NULL) {
                HeapFree(GetProcessHeap(), 0, pOldDataArray[i]);
            }
        }
        HeapFree(GetProcessHeap(), 0, pOldDataArray);
    }

    HeapFree(GetProcessHeap(), 0, pSortableArray);
    HeapFree(GetProcessHeap(), 0, pBuffer);
    g_ullLastQueryTime = ullCurrentTime;

    // 10. Restore selection/scroll
    int iCount = (int)SendMessage(hList, LB_GETCOUNT, 0, 0);
    int iNewSelectionIndex = LB_ERR;

    if (s_dwSelectedPID != 0) {
        for (int i = 2; i < iCount; i++) {
            PPROCESS_ITEM_DATA pData = (PPROCESS_ITEM_DATA)SendMessage(hList, LB_GETITEMDATA, i, 0);

            if (pData != (PPROCESS_ITEM_DATA)LB_ERR && pData != NULL && pData->dwPID == s_dwSelectedPID) {
                iNewSelectionIndex = i;
                break;
            }
        }
    }

    if (iNewSelectionIndex != LB_ERR) {
        SendMessage(hList, LB_SETCURSEL, iNewSelectionIndex, 0);

        if (s_iCurrentTopIndex != LB_ERR && s_iCurrentTopIndex < iCount) {
            SendMessage(hList, LB_SETTOPINDEX, s_iCurrentTopIndex, 0);
        } else {
            SendMessage(hList, LB_SETTOPINDEX, iNewSelectionIndex, 0);
        }
    } else if (s_iCurrentTopIndex != LB_ERR && s_iCurrentTopIndex < iCount) {
        SendMessage(hList, LB_SETTOPINDEX, s_iCurrentTopIndex, 0);
    }

    if (s_iHorizontalOffset != 0) {
        SetScrollPos(hList, SB_HORZ, s_iHorizontalOffset, FALSE);
        SendMessage(hList, WM_HSCROLL, MAKELONG(SB_THUMBPOSITION, s_iHorizontalOffset), 0);
    }
}

// The application entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // SINGLE-INSTANCE CHECK
    g_hSingleInstanceMutex = CreateMutexA(NULL, TRUE, MUTEX_NAME);
    DWORD dwLastError = GetLastError();

    if (g_hSingleInstanceMutex != NULL && dwLastError == ERROR_ALREADY_EXISTS)
    {
        HWND hWndExisting = FindWindowA(CLASS_NAME, NULL);
        if (hWndExisting != NULL)
        {
            if (IsIconic(hWndExisting)) {
                ShowWindow(hWndExisting, SW_RESTORE);
            }
            SetForegroundWindow(hWndExisting);
        }
        CloseHandle(g_hSingleInstanceMutex);
        return 0;
    }

    // Register Window Class
    WNDCLASS wc;
    ZeroMemory(&wc, sizeof(wc));

    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);

    if (!RegisterClass(&wc))
    {
        if (g_hSingleInstanceMutex != NULL) {
            CloseHandle(g_hSingleInstanceMutex);
        }
        return 0;
    }

    // Create Main Window (Height adjusted to 420px to fit two status lines)
    const int windowWidth = 630;
    const int windowHeight = 380;

    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    int x = (screenWidth - windowWidth) / 2;
    int y = (screenHeight - windowHeight) / 2;

    HWND hwnd = CreateWindowEx(
        0,
        CLASS_NAME,
        "NT Task Manager Real Sample (NTDLL)",
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               x, y,
                               windowWidth, windowHeight,
                               NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL)
    {
        if (g_hSingleInstanceMutex != NULL) {
            CloseHandle(g_hSingleInstanceMutex);
        }
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Message Loop
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    KillTimer(hwnd, IDT_REFRESH);

    return (int)msg.wParam;
}

// --- Window Procedure ---
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    static HFONT hFont;

    switch (uMsg)
    {
        case WM_CREATE:
        {
            // hInst is retrieved locally here for control creation
            HINSTANCE hInst = (HINSTANCE)GetWindowLongA(hwnd, GWL_HINSTANCE);

            // Load NT functions pointer
            if (!LoadNtFunctions()) {
                MessageBox(hwnd, "Error: Could not load NTDLL functions. Cannot run.", "Fatal Error", MB_OK | MB_ICONERROR);
                PostQuitMessage(1);
                return -1;
            }

            // Get total physical RAM
            MEMORYSTATUS ms;
            ZeroMemory(&ms, sizeof(ms));
            ms.dwLength = sizeof(ms);

            GlobalMemoryStatus(&ms);

            // to debug
            /*
			char buf[128];
            wsprintf(buf, "TotalPhys=%lu MB (dwTotalPhys=%lu bytes)",
                     g_dwTotalPhysicalMB,
                     (DWORD)(ms.dwTotalPhys));
            MessageBox(hwnd, buf, "Debug: GlobalMemoryStatus", MB_OK);
			*/

            g_dwTotalPhysicalMB = (ms.dwTotalPhys / (1024 * 1024));
            if (g_dwTotalPhysicalMB == 0 && ms.dwTotalPhys > 0)
                g_dwTotalPhysicalMB = 1;


            // ListBox
            hList = CreateWindowEx(
                WS_EX_CLIENTEDGE,
                "LISTBOX",
                "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                10, 10, 610, 300,
                hwnd, (HMENU)IDC_PROCESS_LIST, hInst, NULL
            );

            g_pfnOriginalListBoxProc = (WNDPROC)SetWindowLongA(hList, GWL_WNDPROC, (LONG)ListBoxSubclassProc);


            // Buttons (WIDTH ADJUSTED to 80)
            hEndTaskBtn = CreateWindow("BUTTON", "&End Task", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 80, 25, hwnd, (HMENU)ID_END_TASK, hInst, NULL);
            hNewTaskBtn = CreateWindow("BUTTON", "&New Task", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 80, 25, hwnd, (HMENU)ID_NEW_TASK, hInst, NULL);
            hExitBtn    = CreateWindow("BUTTON", "E&xit", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 80, 25, hwnd, (HMENU)ID_EXIT, hInst, NULL);

            // Status Text Line 1 (Up/RAM)
            hStatusText1 = CreateWindow("STATIC", "Status: Calculating...", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | WS_GROUP,
                                        0, 0, 190, 25,
                                        hwnd, (HMENU)IDC_STATUS_LINE1, hInst, NULL
            );

            // Status Text Line 2 (PageF)
            /*
			hStatusText2 = CreateWindow("STATIC", "PageF: Calculating...", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | WS_GROUP,
                                        0, 0, 190, 25,
                                        hwnd, (HMENU)IDC_STATUS_LINE2, hInst, NULL
            );*/

            // Sort Combo (WIDTH ADJUSTED)
            hSortCombo = CreateWindow("COMBOBOX", NULL,
                                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                      10, 10, 200, 200,   // <-- height = edit + dropdown
                                      hwnd, (HMENU)IDC_SORT_COMBO, hInst, NULL);

            SendMessage(hSortCombo, CB_ADDSTRING, 0, (LPARAM)"Process Name");
            SendMessage(hSortCombo, CB_ADDSTRING, 0, (LPARAM)"CPU");
            SendMessage(hSortCombo, CB_ADDSTRING, 0, (LPARAM)"Memory (KB)");
            SendMessage(hSortCombo, CB_ADDSTRING, 0, (LPARAM)"PID");
            SendMessage(hSortCombo, CB_ADDSTRING, 0, (LPARAM)"Age (h:m:s)");
            SendMessage(hSortCombo, CB_SETCURSEL, SORT_BY_PROCESS_NAME, 0);

            // Monospace font
            hFont = CreateFontA(
                -12, 0, 0, 0, FW_NORMAL,
                FALSE, FALSE, FALSE,
                ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                FIXED_PITCH | FF_DONTCARE, "Courier New"
            );

            if (hFont != NULL) {
                SendMessage(hList, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
                SendMessage(hEndTaskBtn, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
                SendMessage(hNewTaskBtn, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
                SendMessage(hExitBtn, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
                SendMessage(hStatusText1, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
                SendMessage(hStatusText2, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
                SendMessage(hSortCombo, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));

                // Horizontal extent: ensure Age column fully visible
                HDC hdc = GetDC(hList);
                HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

                SIZE size;
                int widthChars = LIST_CONTENT_WIDTH + RENDER_PAD_SPACES;
                char testString[LIST_CONTENT_WIDTH + RENDER_PAD_SPACES + 1];
                memset(testString, 'W', widthChars);
                testString[widthChars] = '\0';

                if (GetTextExtentPoint32A(hdc, testString, widthChars, &size)) {
                    g_iHorizontalExtent = size.cx + HORIZ_EXTENT_PAD;
                    SendMessage(hList, LB_SETHORIZONTALEXTENT, g_iHorizontalExtent, 0);
                }

                SelectObject(hdc, hOldFont);
                ReleaseDC(hList, hdc);
            }

            SetFocus(hList);

            // Update status text immediately on launch
            UpdateStatusDisplay(hStatusText1, hStatusText2, hwnd);

            // Initial refresh for process list and set timer
            RefreshProcessList(hList, hwnd);
            SetTimer(hwnd, IDT_REFRESH, 2000, NULL); // Timer set to 2000ms (2 seconds)
        }
        break;

        case WM_ACTIVATE:
            // Aggressive Focus Fix 1
            if (wParam != WA_INACTIVE && hList != NULL) {
                SetFocus(hList);
            }
            break;

        case WM_SETFOCUS:
            // Aggressive Focus Fix 2
            if (hList != NULL) {
                SetFocus(hList);
            }
            break;


        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                PostQuitMessage(0);
                return 0;
            }
            break;

        case WM_TIMER:
            if (wParam == IDT_REFRESH)
            {
                // This is correctly fired every 2000ms
                UpdateStatusDisplay(hStatusText1, hStatusText2, hwnd);
                RefreshProcessList(hList, hwnd);
            }
            break;

        case WM_COMMAND:
        {
            // Split declaration and assignment to avoid VC++ 4.x C2180/C2086
            HINSTANCE hInst;
            hInst = (HINSTANCE)GetWindowLongA(hwnd, GWL_HINSTANCE);
            int wmId = LOWORD(wParam);

            switch (wmId)
            {
                case ID_EXIT:
                    PostQuitMessage(0);
                    break;

                case ID_NEW_TASK:
                    RunNewTaskDialog(hwnd, hInst);
                    break;

                case ID_END_TASK:
                {
                    int selection = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
                    PPROCESS_ITEM_DATA pData = NULL;

                    if (selection >= 2 && selection != LB_ERR)
                    {
                        pData = (PPROCESS_ITEM_DATA)SendMessage(hList, LB_GETITEMDATA, selection, 0);
                    }

                    if (pData != (PPROCESS_ITEM_DATA)LB_ERR && pData != NULL)
                    {
                        PROCESS_ITEM_DATA temp_data = *pData;

                        if (EndProcessWithConfirmation(hwnd, &temp_data))
                        {
                            RefreshProcessList(hList, hwnd);
                        }
                    } else {
                        MessageBox(hwnd, "Please select a process to end. (Note: The first two rows are headers.)", "Selection Error", MB_OK | MB_ICONEXCLAMATION);
                    }
                    if (hList != NULL) SetFocus(hList);
                }
                break;

                case IDC_PROCESS_LIST:
                    if (HIWORD(wParam) == LBN_DBLCLK)
                    {
                        int selection = (int)SendMessage(hList, LB_GETCURSEL, 0, 0);
                        if (selection >= 2 && selection != LB_ERR)
                        {
                            PPROCESS_ITEM_DATA pData = (PPROCESS_ITEM_DATA)SendMessage(hList, LB_GETITEMDATA, selection, 0);

                            if (pData != (PPROCESS_ITEM_DATA)LB_ERR && pData != NULL && pData->dwPID > 0)
                            {
                                SwitchToProcessWindow(pData->dwPID);
                            }
                        }
                    }
                    break;

                case IDC_SORT_COMBO:
                    if (HIWORD(wParam) == CBN_SELCHANGE)
                    {
                        s_iSortColumn = (int)SendMessage(hSortCombo, CB_GETCURSEL, 0, 0);
                        RefreshProcessList(hList, hwnd);
                        SetFocus(hList);
                    }
                    break;
            } // end switch(wmId)
        } // end WM_COMMAND case block
        break;

                case WM_DESTROY:
                    if (hList != NULL && g_pfnOriginalListBoxProc != NULL) {
                        SetWindowLongA(hList, GWL_WNDPROC, (LONG)g_pfnOriginalListBoxProc);
                    }

                    CleanupListItemData(hList);
                    if (g_hSingleInstanceMutex != NULL) {
                        CloseHandle(g_hSingleInstanceMutex);
                        g_hSingleInstanceMutex = NULL;
                    }
                    PostQuitMessage(0);
                    return 0;

                case WM_SIZE:
                {
                    int clientWidth  = LOWORD(lParam);
                    int clientHeight = HIWORD(lParam);

                    const int MARGIN     = 10;
                    const int BUTTON_H   = 25;
                    const int STATUS_H   = 25;
                    const int SPACING    = 5;
                    const int BUTTON_W   = 80;
                    const int BUTTON_SP  = 10;

                    int rowY = clientHeight - MARGIN - BUTTON_H;

                    // --- Right group: buttons ---
                    int currentX = clientWidth - MARGIN;
                    if (hExitBtn) {
                        currentX -= BUTTON_W;
                        MoveWindow(hExitBtn, currentX, rowY, BUTTON_W, BUTTON_H, TRUE);
                        currentX -= BUTTON_SP;
                    }
                    if (hNewTaskBtn) {
                        currentX -= BUTTON_W;
                        MoveWindow(hNewTaskBtn, currentX, rowY, BUTTON_W, BUTTON_H, TRUE);
                        currentX -= BUTTON_SP;
                    }
                    if (hEndTaskBtn) {
                        currentX -= BUTTON_W;
                        MoveWindow(hEndTaskBtn, currentX, rowY, BUTTON_W, BUTTON_H, TRUE);
                    }
                    int rightGroupLeft = currentX;

                    // --- Left group: status + combo ---
                    int statusWidth = 225;
                    int comboWidth  = 106;   // wide enough for text

                    int leftX = MARGIN;
                    if (hStatusText1) {
                        MoveWindow(hStatusText1, leftX, rowY, statusWidth, STATUS_H, TRUE);
                    }
                    if (hSortCombo) {
                        int comboX = leftX + statusWidth + SPACING;
                        if (comboX + comboWidth + SPACING > rightGroupLeft) {
                            comboWidth = 120;
							//max(0, rightGroupLeft - comboX - SPACING);
                        }
                        MoveWindow(hSortCombo, comboX, rowY, comboWidth, 100, TRUE);
                    }

                    // --- ListBox fills the rest above ---
                    if (hList) {
                        int listHeight = rowY - MARGIN - SPACING;
                        MoveWindow(hList, MARGIN, MARGIN,
                                   clientWidth - 2*MARGIN,
                                   listHeight,
                                   TRUE);
                    }
                }
                break;


    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
