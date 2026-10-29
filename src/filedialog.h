/* filedialog.h - the system's "open a file" dialog, for the screenshot to
 * diagnose, and where the program is (to find fault_atlas.bin and the
 * cartridges next to it). Windows: its own dialog. macOS: AppleScript's
 * (osascript). Linux: zenity or kdialog, if one is installed.
 * Kept apart from the interface files: windows.h and nuklear don't mix. */
#ifndef FILEDIALOG_H
#define FILEDIALOG_H
#include <stdbool.h>

enum { FILEDIALOG_PICKED, FILEDIALOG_CANCELLED, FILEDIALOG_NONE };

/* Ask for a picture: FILEDIALOG_PICKED and its path (UTF-8, '/' separated)
 * in PATH, FILEDIALOG_CANCELLED, or FILEDIALOG_NONE if this system has no
 * dialog to show. Blocks until the dialog is closed. */
int filedialog_open_picture(char *path, int len);
/* Ask for a save file (.hwstate): one to write (save, the extension
 * added if left out) or to read. Same results as above. */
int filedialog_state(bool save, char *path, int len);
/* Ask for a repair records file (repairs.h): one to export to (.hwrepairs
 * added if there is no extension) or to merge from. */
int filedialog_repairs(bool save, char *path, int len);
/* The folder of the program, '/' separated, no slash at the end. */
bool filedialog_exe_dir(char *dir, int len);

#endif
