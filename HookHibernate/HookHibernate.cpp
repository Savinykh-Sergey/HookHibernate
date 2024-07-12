#include <windows.h>
#include <powrprof.h>
#include <stdio.h>

#pragma comment(lib, "PowrProf.lib")

SERVICE_STATUS g_ServiceStatus = { 0 };
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;

#define SERVICE_NAME L"PreventHibernateService"
#define TIMER_ID 1

void RebootSystem() {
    HANDLE hToken;
    TOKEN_PRIVILEGES tkp;

    // Получение привилегии для перезагрузки системы
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        LookupPrivilegeValue(NULL, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid);
        tkp.PrivilegeCount = 1;
        tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, (PTOKEN_PRIVILEGES)NULL, 0);

        if (GetLastError() != ERROR_SUCCESS) {
            wprintf(L"Failed to adjust token privileges. Error: %ld\n", GetLastError());
        }
    }
    else {
        wprintf(L"Failed to open process token. Error: %ld\n", GetLastError());
    }

    // Перезагрузка системы
    if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_OTHER)) {
        wprintf(L"Failed to reboot the system. Error: %ld\n", GetLastError());
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_POWERBROADCAST) {
        if (wParam == PBT_APMSUSPEND) {
            // Перехват гибернации и запуск таймера для перезагрузки системы
            wprintf(L"System is attempting to hibernate. Rebooting instead.\n");

            // Запуск таймера на 5 секунд, чтобы предотвратить немедленное вхождение в гибернацию
            SetTimer(hwnd, TIMER_ID, 5000, NULL);

            return TRUE; // Сообщаем системе, что обработали событие
        }
    }
    else if (uMsg == WM_TIMER) {
        if (wParam == TIMER_ID) {
            // Остановить таймер
            KillTimer(hwnd, TIMER_ID);

            // Перезагрузка системы
            RebootSystem();
        }
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

DWORD WINAPI ServiceWorkerThread(LPVOID lpParam) {
    // Регистрация класса окна
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"MyWindowClass";

    if (!RegisterClassW(&wc)) {
        wprintf(L"Failed to register window class.\n");
        return 1;
    }

    // Создание окна
    HWND hwnd = CreateWindowExW(
        0,                      // Расширенный стиль окна
        L"MyWindowClass",       // Имя класса
        L"My Window",           // Имя окна
        0,                      // Стиль окна
        0, 0, 0, 0,             // Положение и размер окна
        NULL,                   // Дескриптор родительского окна
        NULL,                   // Дескриптор меню
        GetModuleHandle(NULL),  // Дескриптор модуля приложения
        NULL                    // Дополнительные параметры
    );

    if (!hwnd) {
        wprintf(L"Failed to create window. Error: %ld\n", GetLastError());
        return 1;
    }

    // Регистрация уведомлений о состоянии питания
    HPOWERNOTIFY hPowerNotify = RegisterPowerSettingNotification(hwnd, &GUID_POWERSCHEME_PERSONALITY, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (!hPowerNotify) {
        wprintf(L"Failed to register for power setting notifications.\n");
        return 1;
    }

    // Основной цикл обработки сообщений
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Освобождение ресурсов
    UnregisterPowerSettingNotification(hPowerNotify);
    DestroyWindow(hwnd);

    return ERROR_SUCCESS;
}

VOID WINAPI ServiceCtrlHandler(DWORD CtrlCode) {
    switch (CtrlCode) {
    case SERVICE_CONTROL_STOP:
        if (g_ServiceStatus.dwCurrentState != SERVICE_RUNNING)
            break;

        // Сообщаем системе, что мы останавливаемся
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

        // Устанавливаем событие остановки службы
        SetEvent(g_ServiceStopEvent);

        break;

    default:
        break;
    }
}

VOID WINAPI ServiceMain(DWORD argc, LPTSTR* argv) {
    g_StatusHandle = RegisterServiceCtrlHandler(SERVICE_NAME, ServiceCtrlHandler);

    if (!g_StatusHandle)
        return;

    // Сообщаем системе о состоянии службы
    ZeroMemory(&g_ServiceStatus, sizeof(g_ServiceStatus));
    g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwServiceSpecificExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // Инициализация службы
    g_ServiceStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (g_ServiceStopEvent == NULL) {
        g_ServiceStatus.dwControlsAccepted = 0;
        g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        g_ServiceStatus.dwWin32ExitCode = GetLastError();
        g_ServiceStatus.dwCheckPoint = 1;

        SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
        return;
    }

    // Сообщаем системе, что служба работает
    g_ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP;
    g_ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 0;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);

    // Запуск рабочего потока службы
    HANDLE hThread = CreateThread(NULL, 0, ServiceWorkerThread, NULL, 0, NULL);

    // Ожидание события остановки службы
    WaitForSingleObject(g_ServiceStopEvent, INFINITE);

    // Сообщаем системе, что служба остановлена
    CloseHandle(g_ServiceStopEvent);

    g_ServiceStatus.dwControlsAccepted = 0;
    g_ServiceStatus.dwCurrentState = SERVICE_STOPPED;
    g_ServiceStatus.dwWin32ExitCode = 0;
    g_ServiceStatus.dwCheckPoint = 3;

    SetServiceStatus(g_StatusHandle, &g_ServiceStatus);
}

int wmain(int argc, wchar_t* argv[]) {
    SERVICE_TABLE_ENTRY ServiceTable[] = {
        {const_cast<LPWSTR>(SERVICE_NAME), reinterpret_cast<LPSERVICE_MAIN_FUNCTION>(ServiceMain)},
        {nullptr, nullptr}
    };

    if (StartServiceCtrlDispatcher(ServiceTable) == FALSE) {
        return GetLastError();
    }

    return 0;
}




