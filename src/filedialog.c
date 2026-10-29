/* filedialog.c - see filedialog.h. */
#include "filedialog.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#else
#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
static void slashes(char *p) {
    for (; *p; p++) if (*p == '\\') *p = '/';
}
#else
/* Run a dialog program through the shell and read the path it prints.
 * It prints nothing and exits 1 when cancelled; the script below exits
 * 127 when there is no dialog program at all. */
static int run_dialog(const char *cmd, char *path, int len) {
    path[0] = 0;
    FILE *p = popen(cmd, "r");
    if (!p) return FILEDIALOG_NONE;
    if (!fgets(path, len, p)) path[0] = 0;
    char rest[256];
    while (fgets(rest, sizeof(rest), p)) {}     /* let it finish */
    int st = pclose(p);
    path[strcspn(path, "\r\n")] = 0;
    if (st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 127) return FILEDIALOG_NONE;
    return path[0] ? FILEDIALOG_PICKED : FILEDIALOG_CANCELLED;
}
#endif

int filedialog_open_picture(char *path, int len) {
#ifdef _WIN32
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Screenshots (*.png, *.jpg, *.jpeg, *.bmp)\0*.png;*.jpg;*.jpeg;*.bmp\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file) / sizeof(file[0]);
    ofn.lpstrTitle = L"Diagnose a screenshot of the broken machine";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return FILEDIALOG_CANCELLED;
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, path, len, NULL, NULL)) return FILEDIALOG_CANCELLED;
    slashes(path);
    return FILEDIALOG_PICKED;
#elif defined(__APPLE__)
    /* Cancel makes osascript fail (exit 1, nothing printed). No "tell
     * application": that would ask the user for automation rights. */
    return run_dialog("osascript"
                      " -e 'POSIX path of (choose file with prompt \"Diagnose a screenshot of the broken machine\""
                      " of type {\"public.png\", \"public.jpeg\", \"com.microsoft.bmp\"})' 2>/dev/null",
                      path, len);
#else
    /* zenity (GNOME and most others), else kdialog (KDE). */
    return run_dialog("if command -v zenity >/dev/null 2>&1; then"
                      " zenity --file-selection --title='Diagnose a screenshot of the broken machine'"
                      " --file-filter='Screenshots | *.png *.PNG *.jpg *.JPG *.jpeg *.JPEG *.bmp *.BMP'"
                      " --file-filter='All files | *' 2>/dev/null;"
                      " elif command -v kdialog >/dev/null 2>&1; then"
                      " kdialog --title 'Diagnose a screenshot of the broken machine'"
                      " --getopenfilename . 'image/png image/jpeg image/bmp' 2>/dev/null;"
                      " else exit 127; fi",
                      path, len);
#endif
}

int filedialog_state(bool save, char *path, int len) {
    int got;
    path[0] = 0;                /* a dialog that never ran writes nothing */
#ifdef _WIN32
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Save states (*.hwstate)\0*.hwstate\0All files\0*.*\0";
    ofn.lpstrDefExt = L"hwstate";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file) / sizeof(file[0]);
    ofn.lpstrTitle = save ? L"Save the board to a file" : L"Load a saved board";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return FILEDIALOG_CANCELLED;
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, path, len, NULL, NULL)) return FILEDIALOG_CANCELLED;
    slashes(path);
    got = FILEDIALOG_PICKED;
#elif defined(__APPLE__)
    got = run_dialog(save ? "osascript -e 'POSIX path of (choose file name with prompt \"Save the board to a file\""
                            " default name \"board.hwstate\")' 2>/dev/null"
                          : "osascript -e 'POSIX path of (choose file with prompt \"Load a saved board\")' 2>/dev/null",
                     path, len);
#else
    got = run_dialog(save ? "if command -v zenity >/dev/null 2>&1; then"
                            " zenity --file-selection --save --confirm-overwrite --title='Save the board to a file'"
                            " --file-filter='Save states | *.hwstate' 2>/dev/null;"
                            " elif command -v kdialog >/dev/null 2>&1; then"
                            " kdialog --title 'Save the board to a file' --getsavefilename . '*.hwstate' 2>/dev/null;"
                            " else exit 127; fi"
                          : "if command -v zenity >/dev/null 2>&1; then"
                            " zenity --file-selection --title='Load a saved board'"
                            " --file-filter='Save states | *.hwstate' --file-filter='All files | *' 2>/dev/null;"
                            " elif command -v kdialog >/dev/null 2>&1; then"
                            " kdialog --title 'Load a saved board' --getopenfilename . '*.hwstate' 2>/dev/null;"
                            " else exit 127; fi",
                     path, len);
#endif
    size_t n = strlen(path);
    if (got == FILEDIALOG_PICKED && save && (n < 8 || strcmp(path + n - 8, ".hwstate")) && (int)n + 9 <= len)
        strcat(path, ".hwstate");
    return got;
}

int filedialog_repairs(bool save, char *path, int len) {
    int got;
    path[0] = 0;
#ifdef _WIN32
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = L"Repair records (*.hwrepairs, repairs.bin)\0*.hwrepairs;*.bin\0All files\0*.*\0";
    ofn.lpstrDefExt = L"hwrepairs";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof(file) / sizeof(file[0]);
    ofn.lpstrTitle = save ? L"Export repair records to a file" : L"Merge repair records from a file";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return FILEDIALOG_CANCELLED;
    if (!WideCharToMultiByte(CP_UTF8, 0, file, -1, path, len, NULL, NULL)) return FILEDIALOG_CANCELLED;
    slashes(path);
    got = FILEDIALOG_PICKED;
#elif defined(__APPLE__)
    got = run_dialog(save ? "osascript -e 'POSIX path of (choose file name with prompt \"Export repair records\""
                            " default name \"repairs.hwrepairs\")' 2>/dev/null"
                          : "osascript -e 'POSIX path of (choose file with prompt \"Merge repair records\")' 2>/dev/null",
                     path, len);
#else
    got = run_dialog(save ? "if command -v zenity >/dev/null 2>&1; then"
                            " zenity --file-selection --save --confirm-overwrite --title='Export repair records'"
                            " 2>/dev/null;"
                            " elif command -v kdialog >/dev/null 2>&1; then"
                            " kdialog --title 'Export repair records' --getsavefilename . '*.hwrepairs' 2>/dev/null;"
                            " else exit 127; fi"
                          : "if command -v zenity >/dev/null 2>&1; then"
                            " zenity --file-selection --title='Merge repair records' 2>/dev/null;"
                            " elif command -v kdialog >/dev/null 2>&1; then"
                            " kdialog --title 'Merge repair records' --getopenfilename . 2>/dev/null;"
                            " else exit 127; fi",
                     path, len);
#endif
    size_t n = strlen(path);
    if (got == FILEDIALOG_PICKED && save && !strchr(strrchr(path, '/') ? strrchr(path, '/') : path, '.') &&
        (int)n + 11 <= len)
        strcat(path, ".hwrepairs");
    return got;
}

bool filedialog_exe_dir(char *dir, int len) {
#ifdef _WIN32
    wchar_t w[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(NULL, w, sizeof(w) / sizeof(w[0]));
    if (!n || n >= sizeof(w) / sizeof(w[0])) return false;
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, dir, len, NULL, NULL)) return false;
    slashes(dir);
#elif defined(__APPLE__)
    char raw[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) return false;
    if (!realpath(raw, real)) return false;       /* resolve symlinks and "./" */
    if ((int)strlen(real) >= len) return false;
    snprintf(dir, (size_t)len, "%s", real);
#elif defined(__linux__)
    char real[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", real, sizeof(real) - 1);
    if (n <= 0 || n >= len) return false;
    real[n] = 0;
    snprintf(dir, (size_t)len, "%s", real);
#else
    (void)dir;
    (void)len;
    return false;
#endif
    char *s = strrchr(dir, '/');
    if (s) *s = 0;
    return true;
}
