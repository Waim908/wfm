#ifndef FILE_ACTIONS_H
#define FILE_ACTIONS_H

void deleteFiles(struct FileNode** nodes, int count);
void clearClipboard();
void copyFiles(struct FileNode** nodes, int count);
void cutFiles(struct FileNode** nodes, int count);

// 剪贴板当前状态（供粘贴菜单置灰、状态栏指示等 UI 使用）
bool clipboardHasItems();
int getClipboardCount();
bool isClipboardCut();
wchar_t* getClipboardFirstPath();

void pasteFiles(wchar_t* dstDir);
void pasteShortcuts(wchar_t* dstDir);
void createDesktopShortcuts(struct FileNode** nodes, int count);
void extractFilesFromISOImage(wchar_t* isoPath, wchar_t* dstPath);

#endif