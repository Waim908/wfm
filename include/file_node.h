#ifndef FILE_NODE_H
#define FILE_NODE_H

#include "file_utils.h"

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