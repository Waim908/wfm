#include "main.h"

struct FileNode* treeFileNode = NULL;
struct FileNode* currPathFileNode = NULL;

static wchar_t desktopPath[MAX_PATH] = {0};
static wchar_t personalPath[MAX_PATH] = {0};
static wchar_t userProfilePath[MAX_PATH] = {0}; // 新增：用户目录路径
static wchar_t* userProfileNodeName = NULL;     // 用于指针识别

wchar_t* getDesktopPath() {
    return desktopPath;
}

static struct FileNode* allocFileNode(wchar_t* name, enum FileType type) {
    struct FileNode* node = malloc(sizeof(struct FileNode));
    node->name = name;
    node->type = type;
    node->parent = NULL;
    node->sibling = NULL;
    node->children = NULL;
    node->hasChildDirs = false;
    node->size = 0;
    memset(&node->modifiedTime, 0, sizeof(FILETIME));
    return node;
}

int getChildNodeCount(struct FileNode* parent) {
    int count = 0;
    struct FileNode* child = parent->children;
    while (child) {
        count++;
        child = child->sibling;
    }
    return count;
}

void freeChildNodes(struct FileNode* parent) {
    struct FileNode* child = parent->children;
    while (child) {
        freeChildNodes(child);
        struct FileNode* sibling = child->sibling;
        
        MEMFREE(child->name);
        MEMFREE(child);
        
        child = sibling;
    }
    parent->children = NULL;
}

void buildChildNodes(struct FileNode* parent, bool onlyDirs) {
    freeChildNodes(parent);
    
    struct FileNode* firstChild = NULL;
    struct FileNode* lastChild = NULL;  
    
    if (parent->type == TYPE_COMPUTER) {
        wchar_t drives[MAX_PATH] = {0};
        GetLogicalDriveStrings(MAX_PATH, drives);
        
        int i = 0;
        while (drives[i] != L'\0') {
            wchar_t* drive = &drives[i];
            i += wcslen(drive) + 1;

            wchar_t* name = malloc(3 * sizeof(wchar_t));
            name[0] = drive[0];
            name[1] = drive[1];
            name[2] = L'\0';
            
            struct FileNode* child = allocFileNode(name, TYPE_DRIVE);
            child->parent = parent;
            child->hasChildDirs = true;
            
            if (!firstChild) firstChild = child;
            if (lastChild) lastChild->sibling = child;
            lastChild = child;
        }
    }
    else {
        wchar_t path[MAX_PATH] = {0};
        getFileNodePath(parent, path);
        
        WIN32_FIND_DATA wfd = {0};
        wcscat_s(path, MAX_PATH, L"\\*");
        HANDLE handle = FindFirstFile(path, &wfd);
        if (handle == INVALID_HANDLE_VALUE) goto done;
        
        do {
            if (wfd.cFileName[0] == L'.' && (wfd.cFileName[1] == L'\0' || 
                (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))) continue;
            if (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;

            bool isDir = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (onlyDirs && !isDir) continue;
            if (!isDir && !(wfd.dwFileAttributes & FILE_ATTRIBUTE_ARCHIVE)) continue;
            
            enum FileType type = isDir ? TYPE_DIR : TYPE_FILE;
            
            wchar_t* name = wcsdup(wfd.cFileName);
            struct FileNode* child = allocFileNode(name, type);
            child->parent = parent;
            
            if (isDir) {
                child->hasChildDirs = true;
            }
            else {
                LARGE_INTEGER filesize;
                filesize.LowPart = wfd.nFileSizeLow;
                filesize.HighPart = wfd.nFileSizeHigh;
                child->size = filesize.QuadPart;
                memcpy(&child->modifiedTime, &wfd.ftLastWriteTime, sizeof(FILETIME));
            }
            
            if (!firstChild) firstChild = child;
            if (lastChild) lastChild->sibling = child;
            lastChild = child;              
        }
        while (FindNextFile(handle, &wfd));
        FindClose(handle);
    }
    
done:
    parent->children = firstChild;
}

void checkIfNodesHasChildDirs(struct FileNode* node, bool deep) {
    if (!node) return;
    wchar_t path[MAX_PATH] = {0};
    
    struct FileNode* parent = node;
    while (parent) {
        parent->hasChildDirs = false;
        if (parent->type == TYPE_COMPUTER) {
            parent->hasChildDirs = true;
        }
        else {
            getFileNodePath(parent, path);
            
            if (isPathExists(path)) {
                WIN32_FIND_DATA wfd = {0};
                wcscat_s(path, MAX_PATH, L"\\*");
                HANDLE handle = FindFirstFile(path, &wfd);

                if (handle != INVALID_HANDLE_VALUE) {
                    do {
                        if (wcscmp(wfd.cFileName, L".") == 0 || wcscmp(wfd.cFileName, L"..") == 0) continue;
                        if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                            parent->hasChildDirs = true;
                            break;
                        }               
                    }
                    while (FindNextFile(handle, &wfd));
                    FindClose(handle);
                }
            }
        }
        
        if (deep) checkIfNodesHasChildDirs(parent->children, deep);
        parent = parent->sibling;
    }
}

static void freeCurrPathFileNode() {
    struct FileNode* node = currPathFileNode;
    while (node) {
        struct FileNode* parent = node->parent;
        freeChildNodes(node);
        
        MEMFREE(node->name);
        MEMFREE(node);
        
        node = parent;
    }
    currPathFileNode = NULL;
}

void setCurrPathFileNode(struct FileNode* node) {
    struct FileNode* currNode = node;
    int count = 0;
    while (currNode) {
        count++;
        currNode = currNode->parent;
    }
    
    struct FileNode* nodes[count];
    int i = 0;
    currNode = node;
    while (currNode) {
        nodes[i++] = currNode;
        currNode = currNode->parent;
    }   
    
    for (int i = count-1; i >= 0; i--) {
        wchar_t* name = wcsdup(nodes[i]->name);
        struct FileNode* newNode = allocFileNode(name, nodes[i]->type);
        newNode->parent = currNode;
        currNode = newNode;
    }
    
    freeCurrPathFileNode();
    currPathFileNode = currNode;
}

void setCurrPathFromString(wchar_t* path) {
    if (!isPathExists(path)) return;
    wchar_t tmp[MAX_PATH] = {0};
    wcscpy_s(tmp, MAX_PATH, path);

    struct FileNode* currNode = allocFileNode(lc_str.computer, TYPE_COMPUTER);
    
    wchar_t* saveptr;
    wchar_t* token = wcstok(tmp, L"\\", &saveptr);
    int i = 0;
    while (token != NULL) {
        wchar_t* name = wcsdup(token);
        enum FileType type = i++ == 0 ? TYPE_DRIVE : TYPE_DIR;
        struct FileNode* newNode = allocFileNode(name, type);
        newNode->parent = currNode;
        currNode = newNode;
        token = wcstok(NULL, L"\\",&saveptr);
    }
    
    freeCurrPathFileNode();
    currPathFileNode = currNode;
}

void initFileNodes() {
    // 获取标准路径
    SHGetFolderPath(NULL, CSIDL_DESKTOP, NULL, SHGFP_TYPE_CURRENT, desktopPath);
    SHGetFolderPath(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, personalPath);
    GetEnvironmentVariableW(L"USERPROFILE", userProfilePath, MAX_PATH);

    // 创建顶级节点
    struct FileNode* desktopNode = allocFileNode(lc_str.desktop, TYPE_DESKTOP);
    struct FileNode* documentsNode = allocFileNode(lc_str.documents, TYPE_PERSONAL);
    documentsNode->hasChildDirs = true;
    
    // 创建用户目录节点（显示名建议使用本地化字符串，若无则用硬编码）
    userProfileNodeName = wcsdup(L"User");
    struct FileNode* userNode = allocFileNode(userProfileNodeName, TYPE_USERPROFILE);
    userNode->hasChildDirs = true;

    struct FileNode* computerNode = allocFileNode(lc_str.computer, TYPE_COMPUTER);
    computerNode->hasChildDirs = true;
    
    // 链接顺序：桌面 -> 文档 -> 用户 -> 此电脑
    desktopNode->sibling = documentsNode;
    documentsNode->sibling = userNode;
    userNode->sibling = computerNode;
    
    // 构建“此电脑”的子项（驱动器）
    buildChildNodes(computerNode, true);
    
    treeFileNode = desktopNode;
    currPathFileNode = NULL;
    

    
    // 默认选中“此电脑”
    setCurrPathFileNode(computerNode);
}

int getFileNodePath(struct FileNode* node, wchar_t* path) {
    struct FileNode* currNode = node;
    wmemset(path, L'\0', MAX_PATH);
    int count = 0;
    
    // 收集路径片段（从叶到根）
    wchar_t* parts[32];
    int numParts = 0;
    
    while (currNode && numParts < 32) {
        wchar_t* filename = NULL;
        
        if (currNode->name == userProfileNodeName) {
            filename = userProfilePath;
        }
        else {
            switch (currNode->type) {
                case TYPE_DESKTOP:
                    filename = desktopPath;
                    break;
                case TYPE_PERSONAL:
                    filename = personalPath;
                    break;
                case TYPE_USERPROFILE:
                    filename = userProfilePath;
                    break;
                case TYPE_FILE:
                case TYPE_DIR:
                    filename = currNode->name;
                    break;
                case TYPE_DRIVE: {
                    // 对于驱动器，需要添加反斜杠形成 C:\ 格式
                    static wchar_t drivePath[MAX_PATH];
                    wchar_t* name = currNode->name;
                    if (name[0] != L'\0' && name[1] == L':') {
                        // 检查 name 是否已经包含反斜杠
                        if (name[2] == L'\\' || name[2] == L'/') {
                            filename = name;
                        } else {
                            // 构造 C:\ 格式
                            drivePath[0] = name[0];
                            drivePath[1] = L':';
                            drivePath[2] = L'\\';
                            drivePath[3] = L'\0';
                            filename = drivePath;
                        }
                    } else {
                        filename = name;
                    }
                    break;
                }
                default:
                    break;
            }
        }
        
        if (filename && filename[0] != L'\0') {
            parts[numParts++] = filename;
            count++;
        }
        
        currNode = currNode->parent;
    }
    
    // 反向拼接（从根到叶）
    int pos = 0;
    for (int i = numParts - 1; i >= 0; i--) {
        int len = wcslen(parts[i]);
        if (pos + len + 1 >= MAX_PATH) break;
        if (pos > 0) {
            path[pos++] = L'\\';
        }
        memcpy(&path[pos], parts[i], len * sizeof(wchar_t));
        pos += len;
    }
    path[pos] = L'\0';

    return count;
}