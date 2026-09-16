#ifndef FILE_NODE_H
#define FILE_NODE_H

#include "file_utils.h"

// 文件夹条目数的「还没数过」哨兵。只有真的枚举过那个目录才可能有值（没有「只查数量」的
// API），所以显示路径见到它就留空、并由后台计数线程按需填充（见 content_view.c）。
#define CHILD_ITEM_COUNT_UNKNOWN (-1)

struct FileNode {
    wchar_t* name;
    enum FileType type;
    struct FileNode* parent;
    struct FileNode* sibling;
    struct FileNode* children;
    bool hasChildDirs;
    bool isHidden;      // 枚举时记录，供列表灰显隐藏文件（避免在重绘里反复查属性）
    uint64_t size;
    FILETIME modifiedTime;
    int childItemCount; // 仅目录有意义：该目录里的条目数，CHILD_ITEM_COUNT_UNKNOWN = 未知
};

void initFileNodes();
wchar_t* getDesktopPath();
void setCurrPathFileNode(struct FileNode* node);
void setCurrPathFromString(wchar_t* path);
int getChildNodeCount(struct FileNode* parent);
int getFileNodePath(struct FileNode* node, wchar_t* path);
void buildChildNodes(struct FileNode* parent, bool onlyDirs);
void freeChildNodes(struct FileNode* parent);

extern bool g_showHiddenFiles;

#endif