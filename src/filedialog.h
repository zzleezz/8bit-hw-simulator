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
/* The picture on the clipboard (a screenshot copied, or a picture file
 * copied in the file manager): FILEDIALOG_PICKED and its path in PATH (a
 * copied image is first written to a file in the temp folder),
 * FILEDIALOG_CANCELLED if the clipboard holds no picture, FILEDIALOG_NONE
 * if this system has no way to read it (Linux without wl-paste or xclip). */
int filedialog_paste_picture(char *path, int len);
/* Ask for a save file (.hwstate): one to write (save, the extension
 * added if left out) or to read. Same results as above. */
int filedialog_state(bool save, char *path, int len);
/* Ask for a repair records file (repairs.h): one to export to (.hwrepairs
 * added if there is no extension) or to merge from. */
int filedialog_repairs(bool save, char *path, int len);
/* Open a file with the program the system has for it (a page in the
 * browser). False if it couldn't. */
bool filedialog_open_file(const char *path);
/* The folder of the program, '/' separated, no slash at the end. */
bool filedialog_exe_dir(char *dir, int len);

#endif
