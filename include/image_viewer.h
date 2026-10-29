#ifndef IMAGE_VIEWER_H
#define IMAGE_VIEWER_H

#include <windows.h>

// 按扩展名判断是不是图片。给 openFileNode 决定要不要交给内置查看器。
bool isImageFileName(const wchar_t* name);

// 用内置查看器打开一张图。只保留一个查看器窗口：已经开着就换图。
// 解码失败会弹一次提示并保持原窗口内容不变。
void openImageViewer(const wchar_t* path);

#endif
