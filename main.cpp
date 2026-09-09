#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>
#include <string.h>

struct BindValue {
    BOOL isMouse;
    UINT code;
};

enum SpamBindSlot {
    SLOT_C1_CROUCH = 0,
    SLOT_C1_JUMP,
    SLOT_C1_TAPSTRAFE,
    SLOT_C2_INTERACT,
    SLOT_C2_JUMP,
    SLOT_C3_JUMP,
    SLOT_COUNT
};

enum BindTarget {
    BIND_TARGET_NONE = -1,
    BIND_TARGET_HOLD_TRIGGER = -2
};

#define SPAM_DELAY_MS 1
#define MOUSE_BIND_WHEEL_UP 0x1001u
#define MOUSE_BIND_WHEEL_DOWN 0x1002u

static HHOOK g_mouseHook = NULL;
static HHOOK g_keyHook   = NULL;

static volatile BOOL g_assistEnabled = FALSE;
static volatile BOOL g_holdActive    = FALSE;
static volatile BOOL g_spam          = FALSE;
static volatile BOOL g_cycle1HoldDown = FALSE;

static volatile LONG g_cycle = 1;

static volatile BOOL g_bindingMode = FALSE;
static volatile LONG g_bindingTarget = BIND_TARGET_NONE;
static volatile BOOL g_bindIsMouse = TRUE;
static volatile UINT g_bindCode    = VK_XBUTTON1;
static volatile UINT g_swallowMouseUp = 0;

static BindValue g_spamBinds[SLOT_COUNT] = {
    { FALSE, 'C' },               // Strafe Assist: Crouch
    { FALSE, VK_SPACE },          // Strafe Assist: Jump
    { TRUE, MOUSE_BIND_WHEEL_UP },// Strafe Assist: Tapstrafe
    { FALSE, 'E' },               // Zipline Jump: Interact
    { FALSE, VK_SPACE },          // Zipline Jump: Jump
    { FALSE, VK_SPACE }           // Bunnyhop: Jump
};

static volatile BOOL g_settingsOpen = FALSE;
static volatile LONG g_settingsSelection = 0;

static volatile BOOL g_navUpHeld    = FALSE;
static volatile BOOL g_navDownHeld  = FALSE;
static volatile BOOL g_navLeftHeld  = FALSE;
static volatile BOOL g_navRightHeld = FALSE;
static volatile BOOL g_bindHotkeyHeld = FALSE;
static volatile BOOL g_settingsHotkeyHeld = FALSE;
static volatile BOOL g_rebindHotkeyHeld = FALSE;
static volatile BOOL g_enterHeld = FALSE;
static volatile BOOL g_f1Held = FALSE;
static volatile BOOL g_escapeHeld = FALSE;

static HANDLE g_console = NULL;
static WORD   g_defaultAttr = 7;
static volatile BOOL g_uiDirty = TRUE;
static char g_configPath[MAX_PATH] = {0};

static BOOL is_extended_vk(UINT vk){
    switch(vk){
        case VK_RMENU: case VK_RCONTROL:
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN: case VK_NUMLOCK: case VK_CANCEL:
        case VK_SNAPSHOT: case VK_DIVIDE:
            return TRUE;
    }
    return FALSE;
}

static void keyboard_event_vk(UINT vk, BOOL down){
    INPUT ip = {0};
    ip.type = INPUT_KEYBOARD;

    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    if (scan != 0){
        ip.ki.wScan = (WORD)scan;
        ip.ki.dwFlags = KEYEVENTF_SCANCODE;
        if (is_extended_vk(vk)) ip.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        if (!down) ip.ki.dwFlags |= KEYEVENTF_KEYUP;
    }else{
        ip.ki.wVk = (WORD)vk;
        if (!down) ip.ki.dwFlags = KEYEVENTF_KEYUP;
    }

    SendInput(1, &ip, sizeof(INPUT));
}

static void mouse_button_event(UINT vk, BOOL down){
    INPUT ip = {0};
    ip.type = INPUT_MOUSE;

    if (vk == MOUSE_BIND_WHEEL_UP || vk == MOUSE_BIND_WHEEL_DOWN){
        if (!down) return;
        ip.mi.dwFlags = MOUSEEVENTF_WHEEL;
        ip.mi.mouseData = (vk == MOUSE_BIND_WHEEL_UP) ? +WHEEL_DELTA : -WHEEL_DELTA;
        SendInput(1, &ip, sizeof(INPUT));
        return;
    }

    switch(vk){
        case VK_LBUTTON:
            ip.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case VK_RBUTTON:
            ip.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case VK_MBUTTON:
            ip.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        case VK_XBUTTON1:
            ip.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            ip.mi.mouseData = XBUTTON1;
            break;
        case VK_XBUTTON2:
            ip.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
            ip.mi.mouseData = XBUTTON2;
            break;
        default:
            return;
    }

    SendInput(1, &ip, sizeof(INPUT));
}

static void binding_down(const BindValue* bind){
    if (!bind) return;
    if (bind->isMouse) mouse_button_event(bind->code, TRUE);
    else keyboard_event_vk(bind->code, TRUE);
}

static void binding_up(const BindValue* bind){
    if (!bind) return;
    if (bind->isMouse) mouse_button_event(bind->code, FALSE);
    else keyboard_event_vk(bind->code, FALSE);
}

static void send_binding_once(const BindValue* bind){
    binding_down(bind);
    binding_up(bind);
}

static void release_cycle1_hold_if_down(void){
    if (g_cycle1HoldDown){
        binding_up(&g_spamBinds[SLOT_C1_CROUCH]);
        g_cycle1HoldDown = FALSE;
    }
}

static void build_config_path(void){
    char exePath[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH){
        strncpy_s(g_configPath, sizeof(g_configPath), "MethodStrafe.ini", _TRUNCATE);
        return;
    }

    char* slash1 = strrchr(exePath, '\\');
    char* slash2 = strrchr(exePath, '/');
    char* slash = slash1;
    if (slash2 && (!slash || slash2 > slash)) slash = slash2;

    if (slash) *(slash + 1) = '\0';
    else exePath[0] = '\0';

    sprintf_s(g_configPath, sizeof(g_configPath), "%sMethodStrafe.ini", exePath);
}

static void write_int_setting(const char* key, int value){
    char text[32] = {0};
    sprintf_s(text, sizeof(text), "%d", value);
    WritePrivateProfileStringA("Settings", key, text, g_configPath);
}

static int read_int_setting(const char* key, int defaultValue){
    return (int)GetPrivateProfileIntA("Settings", key, defaultValue, g_configPath);
}

static void save_settings(void){
    write_int_setting("HoldTriggerIsMouse", g_bindIsMouse ? 1 : 0);
    write_int_setting("HoldTriggerCode", (int)g_bindCode);

    static const char* mouseKeys[SLOT_COUNT] = {
        "C1HoldIsMouse", "C1TapIsMouse", "C1TapstrafeIsMouse",
        "C2FirstIsMouse", "C2SecondIsMouse", "C3TapIsMouse"
    };
    static const char* codeKeys[SLOT_COUNT] = {
        "C1HoldCode", "C1TapCode", "C1TapstrafeCode",
        "C2FirstCode", "C2SecondCode", "C3TapCode"
    };

    for (int i = 0; i < SLOT_COUNT; ++i){
        write_int_setting(mouseKeys[i], g_spamBinds[i].isMouse ? 1 : 0);
        write_int_setting(codeKeys[i], (int)g_spamBinds[i].code);
    }
}

static BOOL valid_mouse_vk(UINT code){
    return code == VK_LBUTTON || code == VK_RBUTTON || code == VK_MBUTTON ||
           code == VK_XBUTTON1 || code == VK_XBUTTON2 ||
           code == MOUSE_BIND_WHEEL_UP || code == MOUSE_BIND_WHEEL_DOWN;
}

static void sanitize_bind(BindValue* bind, BOOL defaultMouse, UINT defaultCode){
    if (!bind) return;

    if (bind->isMouse){
        if (!valid_mouse_vk(bind->code)){
            bind->isMouse = defaultMouse;
            bind->code = defaultCode;
        }
    }else if (bind->code == 0 || bind->code > 0xFE){
        bind->isMouse = defaultMouse;
        bind->code = defaultCode;
    }
}

static void load_settings(void){
    g_bindIsMouse = read_int_setting("HoldTriggerIsMouse", 1) ? TRUE : FALSE;
    g_bindCode = (UINT)read_int_setting("HoldTriggerCode", VK_XBUTTON1);

    static const char* mouseKeys[SLOT_COUNT] = {
        "C1HoldIsMouse", "C1TapIsMouse", "C1TapstrafeIsMouse",
        "C2FirstIsMouse", "C2SecondIsMouse", "C3TapIsMouse"
    };
    static const char* codeKeys[SLOT_COUNT] = {
        "C1HoldCode", "C1TapCode", "C1TapstrafeCode",
        "C2FirstCode", "C2SecondCode", "C3TapCode"
    };
    static const BOOL defaultMouse[SLOT_COUNT] = { FALSE, FALSE, TRUE, FALSE, FALSE, FALSE };
    static const UINT defaultCode[SLOT_COUNT] = { 'C', VK_SPACE, MOUSE_BIND_WHEEL_UP, 'E', VK_SPACE, VK_SPACE };

    BindValue trigger = { g_bindIsMouse, g_bindCode };
    sanitize_bind(&trigger, TRUE, VK_XBUTTON1);
    if (trigger.isMouse && (trigger.code == MOUSE_BIND_WHEEL_UP || trigger.code == MOUSE_BIND_WHEEL_DOWN)){
        trigger.isMouse = TRUE;
        trigger.code = VK_XBUTTON1;
    }
    g_bindIsMouse = trigger.isMouse;
    g_bindCode = trigger.code;

    for (int i = 0; i < SLOT_COUNT; ++i){
        g_spamBinds[i].isMouse = read_int_setting(mouseKeys[i], defaultMouse[i] ? 1 : 0) ? TRUE : FALSE;
        g_spamBinds[i].code = (UINT)read_int_setting(codeKeys[i], (int)defaultCode[i]);
        sanitize_bind(&g_spamBinds[i], defaultMouse[i], defaultCode[i]);
        if (i == SLOT_C1_CROUCH && g_spamBinds[i].isMouse &&
            (g_spamBinds[i].code == MOUSE_BIND_WHEEL_UP || g_spamBinds[i].code == MOUSE_BIND_WHEEL_DOWN)){
            g_spamBinds[i].isMouse = FALSE;
            g_spamBinds[i].code = 'C';
        }
    }
}

static DWORD WINAPI action_thread(LPVOID){
    for(;;){
        if (!g_spam){
            release_cycle1_hold_if_down();
            Sleep(5);
            continue;
        }

        LONG cycle = g_cycle;

        if (cycle == 1){
            if (!g_cycle1HoldDown){
                binding_down(&g_spamBinds[SLOT_C1_CROUCH]);
                g_cycle1HoldDown = TRUE;
            }

            send_binding_once(&g_spamBinds[SLOT_C1_JUMP]);
            send_binding_once(&g_spamBinds[SLOT_C1_TAPSTRAFE]);
        }
        else if (cycle == 2){
            release_cycle1_hold_if_down();
            send_binding_once(&g_spamBinds[SLOT_C2_INTERACT]);
            send_binding_once(&g_spamBinds[SLOT_C2_JUMP]);
        }
        else{
            release_cycle1_hold_if_down();
            send_binding_once(&g_spamBinds[SLOT_C3_JUMP]);
        }

        if (SPAM_DELAY_MS > 0) Sleep(SPAM_DELAY_MS);
    }
}

static void start_action_if_needed(void){
    if (g_assistEnabled && g_holdActive)
        g_spam = TRUE;

    g_uiDirty = TRUE;
}

static void stop_action(void){
    g_spam = FALSE;
    release_cycle1_hold_if_down();
    g_uiDirty = TRUE;
}

static void set_cycle(LONG cycle){
    if (cycle < 1) cycle = 3;
    if (cycle > 3) cycle = 1;

    if (g_cycle != cycle)
        release_cycle1_hold_if_down();

    g_cycle = cycle;
    g_uiDirty = TRUE;
}

static void cycle_step(int direction){
    LONG next = g_cycle + direction;
    if (next < 1) next = 3;
    if (next > 3) next = 1;
    set_cycle(next);
}

static BOOL console_is_active(void){
    HWND fg = GetForegroundWindow();
    HWND cw = GetConsoleWindow();

    if (fg && cw && fg == cw)
        return TRUE;

    if (fg){
        char title[256] = {0};
        GetWindowTextA(fg, title, (int)sizeof(title));
        if (strstr(title, "MethodStrafe") != NULL)
            return TRUE;
    }

    return FALSE;
}

static void set_color(WORD a){
    SetConsoleTextAttribute(g_console, a);
}

static void clear_screen(void){
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if(!GetConsoleScreenBufferInfo(g_console, &csbi)) return;

    DWORD cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
    DWORD written = 0;
    COORD home = {0,0};

    FillConsoleOutputCharacterA(g_console, ' ', cells, home, &written);
    FillConsoleOutputAttribute(g_console, g_defaultAttr, cells, home, &written);
    SetConsoleCursorPosition(g_console, home);
}

static const char* cycle_name(LONG cycle){
    switch(cycle){
        case 1: return "Strafe Assist";
        case 2: return "Zipline Jump";
        case 3: return "Bunnyhop";
        default: return "Unknown";
    }
}

static void keyboard_key_name(UINT vk, char* out, size_t outSize){
    if (!out || outSize == 0) return;
    out[0] = '\0';

    switch(vk){
        case VK_SPACE:   strncpy_s(out, outSize, "Space", _TRUNCATE); return;
        case VK_RETURN:  strncpy_s(out, outSize, "Enter", _TRUNCATE); return;
        case VK_TAB:     strncpy_s(out, outSize, "Tab", _TRUNCATE); return;
        case VK_BACK:    strncpy_s(out, outSize, "Backspace", _TRUNCATE); return;
        case VK_ESCAPE:  strncpy_s(out, outSize, "Esc", _TRUNCATE); return;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
            strncpy_s(out, outSize, "Shift", _TRUNCATE); return;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
            strncpy_s(out, outSize, "Ctrl", _TRUNCATE); return;
        case VK_MENU: case VK_LMENU: case VK_RMENU:
            strncpy_s(out, outSize, "Alt", _TRUNCATE); return;
    }

    UINT scan = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    LONG lParam = (LONG)(scan << 16);
    if (is_extended_vk(vk)) lParam |= (1 << 24);

    if (GetKeyNameTextA(lParam, out, (int)outSize) <= 0)
        sprintf_s(out, outSize, "VK 0x%02X", vk);
}

static void binding_name(BOOL isMouse, UINT code, char* out, size_t outSize){
    if (!out || outSize == 0) return;

    if (isMouse){
        switch(code){
            case MOUSE_BIND_WHEEL_UP:   strncpy_s(out, outSize, "Mouse Wheel Up", _TRUNCATE); break;
            case MOUSE_BIND_WHEEL_DOWN: strncpy_s(out, outSize, "Mouse Wheel Down", _TRUNCATE); break;
            case VK_LBUTTON:  strncpy_s(out, outSize, "Mouse 1 (Left)", _TRUNCATE); break;
            case VK_RBUTTON:  strncpy_s(out, outSize, "Mouse 2 (Right)", _TRUNCATE); break;
            case VK_MBUTTON:  strncpy_s(out, outSize, "Mouse 3 (Middle)", _TRUNCATE); break;
            case VK_XBUTTON1: strncpy_s(out, outSize, "Mouse 4 (Back)", _TRUNCATE); break;
            case VK_XBUTTON2: strncpy_s(out, outSize, "Mouse 5 (Forward)", _TRUNCATE); break;
            default:          strncpy_s(out, outSize, "Mouse button", _TRUNCATE); break;
        }
    }else{
        keyboard_key_name(code, out, outSize);
    }
}

static void hold_key_name(char* out, size_t outSize){
    binding_name(g_bindIsMouse, g_bindCode, out, outSize);
}

static void spam_key_name(int slot, char* out, size_t outSize){
    if (slot < 0 || slot >= SLOT_COUNT){
        strncpy_s(out, outSize, "Unknown", _TRUNCATE);
        return;
    }
    binding_name(g_spamBinds[slot].isMouse, g_spamBinds[slot].code, out, outSize);
}

static void print_cycle_row(LONG row, const char* label){
    BOOL selected = (g_cycle == row);

    if (selected)
        set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    else
        set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    printf("   %s  %ld  %s\n", selected ? ">" : " ", row, label);
    set_color(g_defaultAttr);
}

static const char* setting_group(int slot){
    switch(slot){
        case SLOT_C1_CROUCH:
        case SLOT_C1_JUMP:
        case SLOT_C1_TAPSTRAFE:
            return "STRAFE ASSIST";
        case SLOT_C2_INTERACT:
        case SLOT_C2_JUMP:
            return "ZIPLINE JUMP";
        case SLOT_C3_JUMP:
            return "BUNNYHOP";
        default:
            return "";
    }
}

static const char* setting_label(int slot){
    switch(slot){
        case SLOT_C1_CROUCH:    return "Crouch";
        case SLOT_C1_JUMP:      return "Jump";
        case SLOT_C1_TAPSTRAFE: return "Tapstrafe";
        case SLOT_C2_INTERACT:  return "Interact";
        case SLOT_C2_JUMP:      return "Jump";
        case SLOT_C3_JUMP:      return "Jump";
        default:                return "Unknown";
    }
}

static void print_selected_cycle_binds(void){
    char a[96] = {0}, b[96] = {0}, c[96] = {0};

    printf("\n  SELECTED\n");
    printf("  --------------------------------------------------\n");

    if (g_cycle == 1){
        spam_key_name(SLOT_C1_CROUCH, a, sizeof(a));
        spam_key_name(SLOT_C1_JUMP, b, sizeof(b));
        spam_key_name(SLOT_C1_TAPSTRAFE, c, sizeof(c));
        printf("  Crouch     [ %s ]\n", a);
        printf("  Jump       [ %s ]\n", b);
        printf("  Tapstrafe  [ %s ]\n", c);
    }else if (g_cycle == 2){
        spam_key_name(SLOT_C2_INTERACT, a, sizeof(a));
        spam_key_name(SLOT_C2_JUMP, b, sizeof(b));
        printf("  Interact   [ %s ]\n", a);
        printf("  Jump       [ %s ]\n", b);
    }else{
        spam_key_name(SLOT_C3_JUMP, a, sizeof(a));
        printf("  Jump       [ %s ]\n", a);
    }
}

static void draw_main_ui(void){
    char holdName[96] = {0};
    hold_key_name(holdName, sizeof(holdName));

    clear_screen();

    set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    printf("  METHODSTRAFE\n");
    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    printf("  ==================================================\n\n");

    printf("  Status   ");
    if (g_assistEnabled){
        set_color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        printf("ON");
    }else{
        set_color(FOREGROUND_RED | FOREGROUND_INTENSITY);
        printf("OFF");
    }
    set_color(g_defaultAttr);

    printf("       Hold [ ");
    set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    printf("%s", holdName);
    set_color(g_defaultAttr);
    printf(" ]\n\n");

    printf("  SELECT CYCLE\n");
    printf("  --------------------------------------------------\n");
    print_cycle_row(1, "Strafe Assist");
    print_cycle_row(2, "Zipline Jump");
    print_cycle_row(3, "Bunnyhop");

    print_selected_cycle_binds();

    printf("\n  KEYS\n");
    printf("  --------------------------------------------------\n");
    printf("  Arrows   Select cycle\n");
    printf("  S        Settings\n");
    printf("  B        Change hold key\n");
    printf("  F1       Toggle ON / OFF\n");

    if (g_bindingMode && g_bindingTarget == BIND_TARGET_HOLD_TRIGGER){
        printf("\n");
        set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        printf("  Press a keyboard key or Mouse 1-5 for Hold Key\n");
        set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        printf("  ESC cancels\n");
        set_color(g_defaultAttr);
    }
}

static void draw_settings_ui(void){
    clear_screen();

    set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
    printf("  METHODSTRAFE // SETTINGS\n");
    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    printf("  ==================================================\n\n");

    const char* lastGroup = "";
    for (int i = 0; i < SLOT_COUNT; ++i){
        const char* group = setting_group(i);
        if (strcmp(group, lastGroup) != 0){
            if (i != 0) printf("\n");
            set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
            printf("  %s\n", group);
            set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
            printf("  --------------------------------------------------\n");
            lastGroup = group;
        }

        char name[96] = {0};
        spam_key_name(i, name, sizeof(name));
        BOOL selected = (g_settingsSelection == i);

        if (selected)
            set_color(FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        else
            set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

        printf("   %s %-11s [ %s ]\n", selected ? ">" : " ", setting_label(i), name);
    }

    set_color(g_defaultAttr);
    printf("\n  KEYS\n");
    printf("  --------------------------------------------------\n");
    printf("  Up/Down    Select\n");
    printf("  Enter / R  Change bind\n");
    printf("  S / ESC    Back\n");

    if (g_bindingMode && g_bindingTarget >= 0 && g_bindingTarget < SLOT_COUNT){
        printf("\n");
        set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        printf("  Rebinding %s - press a keyboard key, Mouse 1-5 or wheel\n", setting_label((int)g_bindingTarget));
        set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
        printf("  ESC cancels\n");
        set_color(g_defaultAttr);
    }
}

static void draw_ui(){
    if (g_settingsOpen) draw_settings_ui();
    else draw_main_ui();
}

static DWORD WINAPI ui_thread(LPVOID){
    for(;;){
        if (g_uiDirty){
            draw_ui();
            g_uiDirty = FALSE;
        }
        Sleep(50);
    }
}

static void start_binding_target(LONG target){
    stop_action();
    g_holdActive = FALSE;
    g_bindingTarget = target;
    g_bindingMode = TRUE;
    g_uiDirty = TRUE;
}

static void cancel_binding(void){
    g_bindingMode = FALSE;
    g_bindingTarget = BIND_TARGET_NONE;
    g_uiDirty = TRUE;
}

static void set_binding_value(LONG target, BOOL isMouse, UINT code){
    if (isMouse && (code == MOUSE_BIND_WHEEL_UP || code == MOUSE_BIND_WHEEL_DOWN) &&
        (target == BIND_TARGET_HOLD_TRIGGER || target == SLOT_C1_CROUCH))
        return;

    stop_action();
    g_holdActive = FALSE;

    if (target == BIND_TARGET_HOLD_TRIGGER){
        g_bindIsMouse = isMouse;
        g_bindCode = code;
    }else if (target >= 0 && target < SLOT_COUNT){
        if (target == SLOT_C1_CROUCH)
            release_cycle1_hold_if_down();

        g_spamBinds[target].isMouse = isMouse;
        g_spamBinds[target].code = code;
    }

    g_bindingMode = FALSE;
    g_bindingTarget = BIND_TARGET_NONE;
    save_settings();
    g_uiDirty = TRUE;
}

static UINT mouse_message_to_vk(WPARAM wParam, const MSLLHOOKSTRUCT* ms, BOOL* isDown, BOOL* isUp){
    *isDown = FALSE;
    *isUp = FALSE;

    switch(wParam){
        case WM_LBUTTONDOWN: *isDown = TRUE; return VK_LBUTTON;
        case WM_LBUTTONUP:   *isUp   = TRUE; return VK_LBUTTON;
        case WM_RBUTTONDOWN: *isDown = TRUE; return VK_RBUTTON;
        case WM_RBUTTONUP:   *isUp   = TRUE; return VK_RBUTTON;
        case WM_MBUTTONDOWN: *isDown = TRUE; return VK_MBUTTON;
        case WM_MBUTTONUP:   *isUp   = TRUE; return VK_MBUTTON;
        case WM_XBUTTONDOWN:
            *isDown = TRUE;
            return HIWORD(ms->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        case WM_XBUTTONUP:
            *isUp = TRUE;
            return HIWORD(ms->mouseData) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
    }

    return 0;
}

static LRESULT CALLBACK mouse_proc(int nCode, WPARAM wParam, LPARAM lParam){
    if (nCode == HC_ACTION){
        const MSLLHOOKSTRUCT* ms = (const MSLLHOOKSTRUCT*)lParam;

        if (ms->flags & LLMHF_INJECTED)
            return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);

        if (g_bindingMode && wParam == WM_MOUSEWHEEL){
            short delta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
            UINT wheelCode = delta >= 0 ? MOUSE_BIND_WHEEL_UP : MOUSE_BIND_WHEEL_DOWN;
            LONG target = g_bindingTarget;
            if (target != BIND_TARGET_HOLD_TRIGGER && target != SLOT_C1_CROUCH){
                set_binding_value(target, TRUE, wheelCode);
                return 1;
            }
        }

        BOOL down = FALSE, up = FALSE;
        UINT vk = mouse_message_to_vk(wParam, ms, &down, &up);

        if (vk != 0){
            if (g_bindingMode && down){
                LONG target = g_bindingTarget;
                set_binding_value(target, TRUE, vk);
                g_swallowMouseUp = vk;
                return 1;
            }

            if (g_swallowMouseUp != 0 && up && vk == g_swallowMouseUp){
                g_swallowMouseUp = 0;
                return 1;
            }

            if (g_bindIsMouse && vk == g_bindCode){
                if (down && !g_holdActive){
                    g_holdActive = TRUE;
                    start_action_if_needed();
                }
                else if (up && g_holdActive){
                    g_holdActive = FALSE;
                    stop_action();
                }
            }
        }
    }

    return CallNextHookEx(g_mouseHook, nCode, wParam, lParam);
}

static void settings_step(int direction){
    LONG next = g_settingsSelection + direction;
    if (next < 0) next = SLOT_COUNT - 1;
    if (next >= SLOT_COUNT) next = 0;
    g_settingsSelection = next;
    g_uiDirty = TRUE;
}

static LRESULT CALLBACK key_proc(int nCode, WPARAM wParam, LPARAM lParam){
    if (nCode == HC_ACTION){
        const KBDLLHOOKSTRUCT* k = (const KBDLLHOOKSTRUCT*)lParam;
        BOOL down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        BOOL up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);

        if (k->flags & LLKHF_INJECTED)
            return CallNextHookEx(g_keyHook, nCode, wParam, lParam);

        if (g_bindingMode){
            if (up){
                if (k->vkCode == 'B') g_bindHotkeyHeld = FALSE;
                if (k->vkCode == 'S') g_settingsHotkeyHeld = FALSE;
                if (k->vkCode == 'R') g_rebindHotkeyHeld = FALSE;
                if (k->vkCode == VK_RETURN) g_enterHeld = FALSE;
                if (k->vkCode == VK_ESCAPE) g_escapeHeld = FALSE;
            }

            if (down){
                if (k->vkCode == VK_ESCAPE){
                    cancel_binding();
                    g_escapeHeld = TRUE;
                    return 1;
                }

                if (k->vkCode == VK_F1){
                    g_uiDirty = TRUE;
                    return 1;
                }

                LONG target = g_bindingTarget;
                set_binding_value(target, FALSE, k->vkCode);
                return 1;
            }

            return 1;
        }

        BOOL activeConsole = console_is_active();

        if (activeConsole){
            if (g_settingsOpen){
                if (k->vkCode == VK_UP){
                    if (down && !g_navUpHeld){
                        g_navUpHeld = TRUE;
                        settings_step(-1);
                    }else if (up){
                        g_navUpHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == VK_DOWN){
                    if (down && !g_navDownHeld){
                        g_navDownHeld = TRUE;
                        settings_step(+1);
                    }else if (up){
                        g_navDownHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == VK_RETURN){
                    if (down && !g_enterHeld){
                        g_enterHeld = TRUE;
                        start_binding_target(g_settingsSelection);
                    }else if (up){
                        g_enterHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == 'R'){
                    if (down && !g_rebindHotkeyHeld){
                        g_rebindHotkeyHeld = TRUE;
                        start_binding_target(g_settingsSelection);
                    }else if (up){
                        g_rebindHotkeyHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == 'S'){
                    if (down && !g_settingsHotkeyHeld){
                        g_settingsHotkeyHeld = TRUE;
                        g_settingsOpen = FALSE;
                        g_uiDirty = TRUE;
                    }else if (up){
                        g_settingsHotkeyHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == VK_ESCAPE){
                    if (down && !g_escapeHeld){
                        g_escapeHeld = TRUE;
                        g_settingsOpen = FALSE;
                        g_uiDirty = TRUE;
                    }else if (up){
                        g_escapeHeld = FALSE;
                    }
                    return 1;
                }
            }else{
                if (k->vkCode == VK_UP){
                    if (down && !g_navUpHeld){
                        g_navUpHeld = TRUE;
                        cycle_step(-1);
                    }else if (up){
                        g_navUpHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == VK_DOWN){
                    if (down && !g_navDownHeld){
                        g_navDownHeld = TRUE;
                        cycle_step(+1);
                    }else if (up){
                        g_navDownHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == VK_LEFT){
                    if (down && !g_navLeftHeld){
                        g_navLeftHeld = TRUE;
                        cycle_step(-1);
                    }else if (up){
                        g_navLeftHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == VK_RIGHT){
                    if (down && !g_navRightHeld){
                        g_navRightHeld = TRUE;
                        cycle_step(+1);
                    }else if (up){
                        g_navRightHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == 'S'){
                    if (down && !g_settingsHotkeyHeld){
                        g_settingsHotkeyHeld = TRUE;
                        g_settingsOpen = TRUE;
                        g_uiDirty = TRUE;
                    }else if (up){
                        g_settingsHotkeyHeld = FALSE;
                    }
                    return 1;
                }

                if (k->vkCode == 'B'){
                    if (down && !g_bindHotkeyHeld){
                        g_bindHotkeyHeld = TRUE;
                        start_binding_target(BIND_TARGET_HOLD_TRIGGER);
                    }else if (up){
                        g_bindHotkeyHeld = FALSE;
                    }
                    return 1;
                }
            }
        }

        if (k->vkCode == VK_F1){
            if (down && !g_f1Held){
                g_f1Held = TRUE;
                g_assistEnabled = !g_assistEnabled;

                if (!g_assistEnabled)
                    stop_action();
                else
                    start_action_if_needed();

                g_uiDirty = TRUE;
            }else if (up){
                g_f1Held = FALSE;
            }

            return 1;
        }

        if (!g_bindIsMouse && k->vkCode == g_bindCode){
            if (down && !g_holdActive){
                g_holdActive = TRUE;
                start_action_if_needed();
            }
            else if (up && g_holdActive){
                g_holdActive = FALSE;
                stop_action();
            }
        }
    }

    return CallNextHookEx(g_keyHook, nCode, wParam, lParam);
}

int main(void){
    SetConsoleTitleA("MethodStrafe");

    g_console = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_console, &csbi))
        g_defaultAttr = csbi.wAttributes;

    build_config_path();
    load_settings();

    HANDLE thAction = CreateThread(NULL, 0, action_thread, NULL, 0, NULL);
    if (!thAction) return 1;

    HANDLE thUi = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    if (!thUi) return 1;

    g_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouse_proc, GetModuleHandle(NULL), 0);
    if (!g_mouseHook) return 1;

    g_keyHook = SetWindowsHookEx(WH_KEYBOARD_LL, key_proc, GetModuleHandle(NULL), 0);
    if (!g_keyHook) return 1;

    g_uiDirty = TRUE;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0){
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    g_spam = FALSE;
    release_cycle1_hold_if_down();

    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_keyHook)   UnhookWindowsHookEx(g_keyHook);

    return 0;
}
