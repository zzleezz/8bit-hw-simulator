/* sokol_impl.c - the single translation unit with the sokol and nuklear
 * implementations. Compiled as Objective-C on macOS (see Makefile). */
#define SOKOL_IMPL
#define SOKOL_NO_ENTRY
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "sokol/sokol_log.h"
#include "sokol/sokol_time.h"
#include "sokol/sokol_glue.h"
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "nuklear/nuklear.h"
#define SOKOL_NUKLEAR_IMPL
#include "sokol/sokol_nuklear.h"

/* Is the system set to light windows? (ui_system_light() in ui.h.) Here,
 * where Objective-C is compiled on macOS and windows.h is in. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#if defined(_SAPP_MACOS)
bool ui_system_light(void) {
    NSAppearanceName name = [[NSApp effectiveAppearance]
        bestMatchFromAppearancesWithNames:@[ NSAppearanceNameAqua, NSAppearanceNameDarkAqua ]];
    return ![name isEqualToString:NSAppearanceNameDarkAqua];
}
#elif defined(_SAPP_WIN32)
bool ui_system_light(void) {
    HKEY key;
    DWORD light = 0, size = sizeof(light), type = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;       /* no such setting (before Windows 10): dark, as always */
    LONG got = RegQueryValueExW(key, L"AppsUseLightTheme", NULL, &type, (BYTE *)&light, &size);
    RegCloseKey(key);
    return got == ERROR_SUCCESS && type == REG_DWORD && light != 0;
}
#else
/* GNOME's setting, asked once (it takes a process); dark without it. */
bool ui_system_light(void) {
    static int light = -1;
    if (light < 0) {
        char buf[64] = "";
        FILE *f = popen("gsettings get org.gnome.desktop.interface color-scheme 2>/dev/null", "r");
        if (f) {
            if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
            pclose(f);
        }
        light = buf[0] && !strstr(buf, "dark");
    }
    return light;
}
#endif
