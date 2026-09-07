#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>

static HHOOK g_mouseHook = NULL;
static HHOOK g_keyHook   = NULL;

static volatile BOOL g_assistEnabled = FALSE;
static volatile BOOL g_backHeld      = FALSE;
static volatile BOOL g_spam          = FALSE;
static volatile BOOL g_cDown         = FALSE;

#define SPAM_DELAY_MS 1
#define SC_SPACE 0x39
#define SC_C     0x2E

static HANDLE g_console = NULL;
static WORD   g_defaultAttr = 7;
static volatile BOOL g_uiDirty = TRUE;

static inline DWORD now_ms(void){ return GetTickCount(); }

static void key_down_scan(WORD scan){
    INPUT ip = {0};
    ip.type = INPUT_KEYBOARD;
    ip.ki.dwFlags = KEYEVENTF_SCANCODE;
    ip.ki.wScan   = scan;
    SendInput(1, &ip, sizeof(INPUT));
}
static void key_up_scan(WORD scan){
    INPUT ip = {0};
    ip.type = INPUT_KEYBOARD;
    ip.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    ip.ki.wScan   = scan;
    SendInput(1, &ip, sizeof(INPUT));
}
static void send_space_once(void){
    key_down_scan(SC_SPACE);
    key_up_scan(SC_SPACE);
}
static void send_wheel_up_once(void){
    INPUT ip = {0};
    ip.type = INPUT_MOUSE;
    ip.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    ip.mi.mouseData = +WHEEL_DELTA;
    SendInput(1, &ip, sizeof(INPUT));
}

static DWORD WINAPI spam_thread(LPVOID){
    for(;;){
        if(g_spam){
            send_space_once();
            if (SPAM_DELAY_MS>0) Sleep(SPAM_DELAY_MS);
        }else{
            Sleep(5);
        }
    }
}
static DWORD WINAPI wheel_thread(LPVOID){
    for(;;){
        if(g_spam){
            send_wheel_up_once();
            if (SPAM_DELAY_MS>0) Sleep(SPAM_DELAY_MS);
        }else{
            Sleep(5);
        }
    }
}

static void hold_c_if_needed(void){
    if(g_spam && !g_cDown){ key_down_scan(SC_C); g_cDown = TRUE; }
}
static void release_c_if_down(void){
    if(g_cDown){ key_up_scan(SC_C); g_cDown = FALSE; }
}

static void start_action_if_needed(void){
    if(g_assistEnabled && g_backHeld){
        g_spam = TRUE;
        hold_c_if_needed();
    }
    g_uiDirty = TRUE;
}
static void stop_action(void){
    g_spam = FALSE;
    release_c_if_down();
    g_uiDirty = TRUE;
}

static void set_color(WORD a){ SetConsoleTextAttribute(g_console, a); }

static void clear_screen(void){
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if(!GetConsoleScreenBufferInfo(g_console,&csbi)) return;
    DWORD cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y, w=0;
    COORD home = {0,0};
    FillConsoleOutputCharacterA(g_console,' ',cells,home,&w);
    FillConsoleOutputAttribute(g_console,g_defaultAttr,cells,home,&w);
    SetConsoleCursorPosition(g_console, home);
}

static void draw_ui(){
    clear_screen();

    set_color(FOREGROUND_GREEN|FOREGROUND_INTENSITY);
    printf("MethodStrafe\n");
    set_color(g_defaultAttr);
    printf("------------------\n");

    printf("Strafe assist: ");
    if (g_assistEnabled){
        set_color(FOREGROUND_GREEN|FOREGROUND_INTENSITY); printf("ON\n");
    }else{
        set_color(FOREGROUND_RED|FOREGROUND_INTENSITY);   printf("OFF\n");
    }
    set_color(g_defaultAttr);

    printf("\nHotkeys: F1 = toggle strafe assist, XBUTTON1 (mouse back) = hold to enjoy\n");
}

static DWORD WINAPI ui_thread(LPVOID){
    for(;;){
        if (g_uiDirty){ draw_ui(); g_uiDirty = FALSE; }
        Sleep(100);
    }
}

static LRESULT CALLBACK mouse_proc(int nCode, WPARAM wParam, LPARAM lParam){
    if (nCode == HC_ACTION){
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lParam;

        if (wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP){
            WORD btn = HIWORD(ms->mouseData);
            if (btn == XBUTTON1){
                g_backHeld = (wParam == WM_XBUTTONDOWN);
                if (g_backHeld) start_action_if_needed();
                else            stop_action();
            }
        }
        (void)ms;
    }
    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static LRESULT CALLBACK key_proc(int nCode, WPARAM wParam, LPARAM lParam){
    if (nCode == HC_ACTION){
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
        BOOL down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

        if (down && k->vkCode == VK_F1){
            g_assistEnabled = !g_assistEnabled;
            if (!g_assistEnabled) stop_action(); else start_action_if_needed();
            g_uiDirty = TRUE;
            return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
        }
    }
    return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
}

int main(void){
    SetConsoleTitleA("MethodStrafe");
    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_console,&csbi)) g_defaultAttr = csbi.wAttributes;

    HANDLE thSpam  = CreateThread(NULL,0,spam_thread, NULL,0,NULL); if(!thSpam) return 1;
    HANDLE thWheel = CreateThread(NULL,0,wheel_thread,NULL,0,NULL); if(!thWheel) return 1;
    HANDLE thUi    = CreateThread(NULL,0,ui_thread,   NULL,0,NULL); if(!thUi) return 1;

    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouse_proc, GetModuleHandle(NULL), 0);
    if (!g_mouseHook) return 1;
    g_keyHook   = SetWindowsHookEx(WH_KEYBOARD_LL, key_proc, GetModuleHandle(NULL), 0);
    if (!g_keyHook)   return 1;

    g_uiDirty = TRUE;

    MSG msg;
    while (GetMessage(&msg,NULL,0,0) > 0){
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_spam && g_cDown) key_up_scan(SC_C);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_keyHook)   UnhookWindowsHookEx(g_keyHook);
    return 0;
}
