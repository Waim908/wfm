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
    if (!node) return NULL;
    node->name = name;
    node->type = type;
    node->parent = NULL;
    node->sibling = NULL;
    node->children = NULL;
    node->hasChildDirs = false;
    node->isHidden = false;
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

// 释放「顶层节点」的结构体本身。
// 注意：不能释放 node->name —— 顶层节点的 name 可能指向 lc_str.* 这类字面量，
// 或指向全局的 userProfileNodeName（由 initFileNodes 单独持有）。
static void freeTopLevelNode(struct FileNode* node) {
    if (!node) return;
    freeChildNodes(node);
    MEMFREE(node);
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
            if (!name) break;   // 分配失败时停止枚举，避免解引用 NULL
            name[0] = drive[0];
            name[1] = drive[1];
            name[2] = L'\0';
            
            struct FileNode* child = allocFileNode(name, TYPE_DRIVE);
            if (!child) { free(name); continue; }
            child->parent = parent;
            // 延迟检查：不在启动时做磁盘IO，展开时再验证
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
        if (path[0] == L'\0') goto done;
        // 先按 MAX_PATH 截断判断再拼接，避免 wcscat_s 容量不足时触发约束违规
        if (wcslen(path) + 2 >= MAX_PATH) goto done;
        wcscat_s(path, MAX_PATH, path[wcslen(path) - 1] == L'\\' ? L"*" : L"\\*");
        HANDLE handle = FindFirstFile(path, &wfd);
        if (handle == INVALID_HANDLE_VALUE) goto done;
        
        do {
            if (wfd.cFileName[0] == L'.' && (wfd.cFileName[1] == L'\0' || 
                (wfd.cFileName[1] == L'.' && wfd.cFileName[2] == L'\0'))) continue;
            if (!g_showHiddenFiles && (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;

            bool isDir = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

            if (onlyDirs && !isDir) continue;
            // 不要用「必须带 ARCHIVE 位」做正向判据：真实 Windows 上清了归档位的
            // 普通文件会被这里凭空过滤掉（Wine 恰好恒设该位，掩盖了这个问题）
            
            enum FileType type = isDir ? TYPE_DIR : TYPE_FILE;
            
            wchar_t* name = wcsdup(wfd.cFileName);
            struct FileNode* child = allocFileNode(name, type);
            if (!child) { free(name); continue; }
            child->parent = parent;
            child->isHidden = (wfd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0;
            
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
        if (!newNode) { free(name); break; }
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
    if (!currNode) return;

    wchar_t* saveptr;
    wchar_t* token = wcstok(tmp, L"\\", &saveptr);
    int i = 0;
    while (token != NULL) {
        wchar_t* name = wcsdup(token);
        enum FileType type = i++ == 0 ? TYPE_DRIVE : TYPE_DIR;
        struct FileNode* newNode = allocFileNode(name, type);
        if (!newNode) { free(name); break; }
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
    if (!desktopNode || !documentsNode) {
        // 分配失败：释放已成功创建的节点，不留下无人持有的内存
        freeTopLevelNode(desktopNode);
        freeTopLevelNode(documentsNode);
        return;
    }
    documentsNode->hasChildDirs = true;

    // 创建用户目录节点（显示名建议使用本地化字符串，若无则用硬编码）
    userProfileNodeName = wcsdup(L"User");
    struct FileNode* userNode = allocFileNode(userProfileNodeName, TYPE_USERPROFILE);
    if (!userNode) {
        freeTopLevelNode(desktopNode);
        freeTopLevelNode(documentsNode);
        return;
    }
    userNode->hasChildDirs = true;

    struct FileNode* computerNode = allocFileNode(lc_str.computer, TYPE_COMPUTER);
    if (!computerNode) {
        freeTopLevelNode(desktopNode);
        freeTopLevelNode(documentsNode);
        freeTopLevelNode(userNode);
        return;
    }
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
    wchar_t* parts[32] = {0};   // 显式清零：拼接阶段按 numParts 反向读取，避免读到未初始化指针
    int numParts = 0;
    // 盘符片段需要一块在整个函数内都有效的存储（parts[] 会一直引用它到拼接阶段）。
    // 旧实现用 static，会让搜索线程与 UI 线程互相覆盖；改为函数局部。
    wchar_t drivePath[4] = {0};
    
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
                    // 对于驱动器，只返回盘号如 C:，路径拼接逻辑会添加分隔符
                    wchar_t* name = currNode->name;
                    if (name[0] != L'\0' && name[1] == L':') {
                        // 移除可能存在的尾部反斜杠，只保留 C: 格式
                        drivePath[0] = name[0];
                        drivePath[1] = L':';
                        drivePath[2] = L'\0';
                        filename = drivePath;
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
        
        bool needsSeparator = false;
        // 如果前面已有内容且不是以反斜杠结尾，需要分隔符
        if (pos > 0 && path[pos-1] != L'\\') {
            needsSeparator = true;
        }
        // 如果当前部分是驱动器号（如 C:），即使 pos>0 也不需要前导分隔符
        // 但驱动器后需要添加反斜杠
        if (len >= 2 && parts[i][1] == L':') {
            // 驱动器号后添加反斜杠
            if (pos + len + 2 >= MAX_PATH) break;
            memcpy(&path[pos], parts[i], len * sizeof(wchar_t));
            pos += len;
            path[pos++] = L'\\';
            continue;
        }
        
        if (needsSeparator) {
            path[pos++] = L'\\';
        }
        memcpy(&path[pos], parts[i], len * sizeof(wchar_t));
        pos += len;
    }
    path[pos] = L'\0';

    return count;
}