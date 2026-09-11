#ifndef FILE_UTILS_H
#define FILE_UTILS_H

#include "main.h"
#include "string_utils.h"

#include <stdarg.h>

// 安全格式化：容量不足时截断，而不是让安全 CRT 触发约束违规（swprintf_s 溢出会
// 落入 invalid parameter handler）。拼接路径这类「长度不受控」的写入一律用它。
static inline void swprintfTrunc(wchar_t* buf, size_t bufSize, const wchar_t* fmt, ...) {
    if (!buf || bufSize == 0) return;
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, bufSize, _TRUNCATE, fmt, args);
    va_end(args);
}

enum FileType {
    TYPE_DIR,
    TYPE_FILE,
    TYPE_DRIVE,
    TYPE_DESKTOP,
    TYPE_PERSONAL,
    TYPE_USERPROFILE,
    TYPE_COMPUTER,
    TYPE_BOOKMARK_ROOT,
    TYPE_BOOKMARK_ITEM
};

struct FileInfo {
    int icon;
    wchar_t typeName[80];
};

static inline bool isPathExists(wchar_t* path) {
    // 存在性判据只看 INVALID_FILE_ATTRIBUTES。
    // 不能用 FILE_ATTRIBUTE_ARCHIVE 这类「正向属性」做判据：真实 Windows 上
    // 清除了归档位的文件（备份/同步工具常见）会被误判为不存在。
    if (!path || path[0] == L'\0') return false;
    return GetFileAttributes(path) != INVALID_FILE_ATTRIBUTES;
}

// 统一的容量换算：整数循环，不依赖 log10/pow。
// 整除时输出整数（"1 KB"），否则保留两位小数（"1.50 KB"），
// 保证文件大小列与磁盘剩余空间列的格式一致。
static inline void formatSizeU64(uint64_t size, wchar_t* buf, int bufSize) {
    static const wchar_t* units[6] = { L"bytes", L"KB", L"MB", L"GB", L"TB", L"PB" };
    if (!buf || bufSize <= 0) return;

    if (size == 0) {
        wcsncpy_s(buf, (size_t)bufSize, L"0 bytes", _TRUNCATE);
        return;
    }

    // 最多进位到 PB，避免 units[] 越界（旧实现 size >= 1024^5 时会读 units[5]）
    int group = 0;
    uint64_t scaled = size;
    while (group < 5 && scaled >= 1024) {
        scaled /= 1024;
        group++;
    }

    uint64_t divisor = 1;
    for (int i = 0; i < group; i++) divisor *= 1024;

    if (size % divisor == 0) {
        swprintfTrunc(buf, (size_t)bufSize, L"%llu %ls", (unsigned long long)scaled, units[group]);
    }
    else {
        swprintfTrunc(buf, (size_t)bufSize, L"%.2f %ls", (double)size / (double)divisor, units[group]);
    }
}

static inline void formatFileSize(uint64_t size, wchar_t* formattedSize) {
    formatSizeU64(size, formattedSize, 32);
}

static inline void formatOneSize(uint64_t val, wchar_t* buf, int bufSize) {
    formatSizeU64(val, buf, bufSize);
}

static inline void formatDriveSpace(uint64_t totalBytes, uint64_t freeBytes, wchar_t* result, int resultSize) {
    wchar_t freeStr[32] = {0};
    wchar_t totalStr[32] = {0};
    formatOneSize(freeBytes, freeStr, 32);
    formatOneSize(totalBytes, totalStr, 32);
    swprintf_s(result, resultSize, lc_str.fmt_drive_space, freeStr, totalStr);
}

static inline void getParentDirFromPath(wchar_t* path, wchar_t* result) {
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    int len = lastSlash ? lastSlash - path + 1 : 1;

    memcpy(result, path, (len - 1) * sizeof(wchar_t));
    result[len-1] = L'\0';
}

// 取路径的最后一段。必须传入 result 的真实容量，旧实现无边界检查（长文件名栈溢出）。
static inline void getBasenameFromPath(const wchar_t* path, wchar_t* result, int resultSize, bool removeExt) {
    if (!result || resultSize <= 0) return;
    result[0] = L'\0';
    if (!path) return;

    const wchar_t* lastSlash = wcsrchr(path, L'\\');
    const wchar_t* base = lastSlash ? lastSlash + 1 : path;
    wcsncpy_s(result, (size_t)resultSize, base, _TRUNCATE);

    if (removeExt) {
        wchar_t* lastDot = wcsrchr(result, L'.');
        // 前导点号表示隐藏文件名（.gitignore/.bashrc），不当扩展名处理
        if (lastDot && lastDot != result) *lastDot = L'\0';
    }
}

static inline void toUnixPath(wchar_t* dosPath, char* result) {
    if (!result) return;
    result[0] = '\0';
    if (!dosPath || wcslen(dosPath) < 2) return;   // 避免 dosPath + 2 越过字符串末尾

    wchar_t unixPath[MAX_PATH] = {0};
    wcsncpy_s(unixPath, MAX_PATH, dosPath + 2, _TRUNCATE);
    
    int count = 0;
    wchar_t* ptr = unixPath;
    while (*ptr && count++ < MAX_PATH) {
        if (*ptr == L'\\') *ptr = L'/';
        ptr++;
    }
    
    if (unixPath[0] != L'/') unixPath[0] = L'/';
    WideCharToMultiByte(CP_ACP, 0, unixPath, -1, result, MAX_PATH, NULL, NULL);       
}

static inline void formatModifiedDate(int month, int day, int year, int hour, int minute, wchar_t* result, int size) {
    swprintf_s(result, size, L"%02d/%02d/%04d %02d:%02d", month, day, year, hour, minute);
}

static inline wchar_t* getFileExtension(wchar_t* path) {
    wchar_t* ext = wcsrchr(path, L'.');
    return ext && *ext++ != L'\0' ? ext : NULL;
}

static inline bool isCDDrivePath(wchar_t* path) {
    return (path[0] == L'x' || path[0] == L'X') && path[1] == L':';
}

static inline bool hasFileExtension(wchar_t* path, wchar_t* targetExt) {
    wchar_t* ext = getFileExtension(path);
    return ext && wcsicmp(ext, targetExt) == 0;
}

static inline int getTreeIcon(wchar_t* path, enum FileType type) {
    SHFILEINFO sfi = {0};
    DWORD flags = SHGFI_SYSICONINDEX | SHGFI_SMALLICON;

    if (type == TYPE_DIR) {
        flags |= SHGFI_USEFILEATTRIBUTES;
        SHGetFileInfo(path, FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(SHFILEINFO), flags);
    }
    else if (type == TYPE_DRIVE) {
        wchar_t drivePath[4] = {0};
        swprintf_s(drivePath, 4, L"%lc:\\", path[0]);
        SHGetFileInfo(drivePath, 0, &sfi, sizeof(SHFILEINFO), flags);
    }
    else {
        flags |= SHGFI_USEFILEATTRIBUTES;
        SHGetFileInfo(path, FILE_ATTRIBUTE_ARCHIVE, &sfi, sizeof(SHFILEINFO), flags);
    }
    return sfi.iIcon;
}

static inline void getFileInfo(wchar_t* path, enum FileType type, bool largeIcon, struct FileInfo* result) {
    SHFILEINFO sfi = {0};
    result->icon = 0;
    
    DWORD flags = SHGFI_SYSICONINDEX | (largeIcon ? 0 : SHGFI_SMALLICON);
    
    if (type == TYPE_DIR) {
        // 目录使用 USEFILEATTRIBUTES 即可
        flags |= SHGFI_USEFILEATTRIBUTES;
        SHGetFileInfo(path, FILE_ATTRIBUTE_DIRECTORY, &sfi, sizeof(SHFILEINFO), flags);
    }
    else if (type == TYPE_DRIVE) {
        // 驱动器路径必须以反斜杠结尾（如 "C:\"），否则无法正确获取驱动器图标
        wchar_t drivePath[4] = {0};
        swprintf_s(drivePath, 4, L"%lc:\\", path[0]);
        SHGetFileInfo(drivePath, 0, &sfi, sizeof(SHFILEINFO), flags);
    }
    else {
        // 检查是否为 exe 或 lnk 文件，这些需要实际访问文件获取内嵌图标
        wchar_t* ext = wcsrchr(path, L'.');
        bool needRealAccess = ext && (wcsicmp(ext, L".exe") == 0 || wcsicmp(ext, L".lnk") == 0);
        
        if (needRealAccess) {
            // exe/lnk 需要访问文件获取真实图标
            SHGetFileInfo(path, 0, &sfi, sizeof(SHFILEINFO), flags);
        }
        else {
            // 其他文件类型使用 USEFILEATTRIBUTES 快速获取
            flags |= SHGFI_USEFILEATTRIBUTES;
            SHGetFileInfo(path, FILE_ATTRIBUTE_ARCHIVE, &sfi, sizeof(SHFILEINFO), flags);
        }
    }
    result->icon = sfi.iIcon;
    
    switch (type) {
        case TYPE_DIR:
            wcscpy_s(result->typeName, 80, lc_str.folder);
            break;
        case TYPE_DRIVE: {
            if (isCDDrivePath(path)) {
                wcscpy_s(result->typeName, 80, lc_str.cd_drive);           
            }
            else wcscpy_s(result->typeName, 80, lc_str.local_drive);
            break;
        }
        case TYPE_DESKTOP:
            wcscpy_s(result->typeName, 80, lc_str.desktop);
            break;
        case TYPE_PERSONAL:
            wcscpy_s(result->typeName, 80, lc_str.folder);
            break;
        case TYPE_USERPROFILE:
            wcscpy_s(result->typeName, 80, lc_str.folder);
            break;      
        case TYPE_COMPUTER:
            wcscpy_s(result->typeName, 80, lc_str.computer);
            break;
        default: {
            wcscpy_s(result->typeName, 80, lc_str.file);
            wchar_t* ext = getFileExtension(path);
            
            if (ext) {
                if (wcsicmp(ext, L"exe") == 0) {
                    wcscpy_s(result->typeName, 80, lc_str.application);
                }
                else if (wcsicmp(ext, L"lnk") == 0) {
                    wcscpy_s(result->typeName, 80, lc_str.shortcut);
                }
                else {
                    wchar_t value[30] = {0};
                    strToUpper(ext, value, 30);
                    swprintfTrunc(result->typeName, 80, lc_str.fmt_file, value);
                }
            }
            
            break;
        }
    }
}

static inline void makeDirs(wchar_t* path) {
    if (isPathExists(path)) return;
    wchar_t parentDir[MAX_PATH] = {0};
    getParentDirFromPath(path, parentDir);
    if (!isPathExists(parentDir)) makeDirs(parentDir);
    CreateDirectory(path, NULL);
}

// 拼接路径。旧实现把 MAX_PATH 当作「剩余容量」传给 CRT（result + pathLen 之后
// 实际只剩 MAX_PATH - pathLen），且 pathA 为空串时会读 result[-1]。
static inline void joinPaths(wchar_t* pathA, wchar_t* pathB, wchar_t* result, int resultSize) {
    if (!result || resultSize <= 0) return;
    result[0] = L'\0';
    if (!pathA) pathA = L"";
    if (!pathB) pathB = L"";

    wcsncpy_s(result, (size_t)resultSize, pathA, _TRUNCATE);
    int pathLen = (int)wcslen(result);

    if (pathLen > 0 && result[pathLen - 1] != L'\\' && pathLen + 1 < resultSize) {
        result[pathLen++] = L'\\';
        result[pathLen] = L'\0';
    }

    wcsncpy_s(result + pathLen, (size_t)(resultSize - pathLen), pathB, _TRUNCATE);
}

static inline void joinUnixPaths(char* pathA, char* pathB, char* result, int resultSize) {
    if (!result || resultSize <= 0) return;
    result[0] = '\0';
    if (!pathA) pathA = "";
    if (!pathB) pathB = "";

    strncpy_s(result, (size_t)resultSize, pathA, _TRUNCATE);
    int pathLen = (int)strlen(result);

    if (pathLen > 0 && result[pathLen - 1] != '/' && pathLen + 1 < resultSize) {
        result[pathLen++] = '/';
        result[pathLen] = '\0';
    }

    strncpy_s(result + pathLen, (size_t)(resultSize - pathLen), pathB, _TRUNCATE);
}

static inline void clearDirectory(wchar_t* targetPath) {
    if (!targetPath || targetPath[0] == L'\0') return;

    wchar_t path[MAX_PATH] = {0};
    if (wcsstr(targetPath, L"\\*")) {
        wcsncpy_s(path, MAX_PATH, targetPath, _TRUNCATE);
    }
    else {
        // 父目录路径 + 分隔符 + 通配符 + 终止符必须都放得下。
        // 放不下就整体放弃：截断后再交给 FindFirstFile 会匹配到错误路径，
        // 而沿用 wcscat_s 会因容量不足落进安全 CRT 的无效参数处理器。
        if (wcslen(targetPath) + 2 >= MAX_PATH) return;
        swprintfTrunc(path, MAX_PATH, L"%ls%ls*", targetPath,
                      targetPath[wcslen(targetPath) - 1] == L'\\' ? L"" : L"\\");
    }

    WIN32_FIND_DATA wfd = {0};
    HANDLE handle = FindFirstFile(path, &wfd);

    if (handle != INVALID_HANDLE_VALUE) {
        do {
            // 只跳过 "." 和 ".."。旧写法 cFileName[0] == '.' 会把 .config/.git/.wine
            // 这些合法的隐藏目录一并跳过，导致卸载后删除不干净。
            if (wcscmp(wfd.cFileName, L".") == 0 || wcscmp(wfd.cFileName, L"..") == 0) continue;
            
            wchar_t fullPath[MAX_PATH] = {0};
            joinPaths(targetPath, wfd.cFileName, fullPath, MAX_PATH);
            if (fullPath[0] == L'\0') continue;
            bool isDir = (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            
            if (isDir) {
                clearDirectory(fullPath);
                RemoveDirectory(fullPath);
            }
            else DeleteFile(fullPath);
        }
        while (FindNextFile(handle, &wfd));
        FindClose(handle);
    }
}

static inline bool getCurrentISOPath(wchar_t* result) {
    if (!result) return false;
    wmemset(result, L'\0', MAX_PATH);
    // RegQueryValue 的 lpcbData 单位是「字节」；传入的元素个数要乘 sizeof(wchar_t)
    LONG size = MAX_PATH * sizeof(wchar_t);
    LONG ret = ERROR_FILE_NOT_FOUND;
    HKEY hkey;
    if (RegOpenKey(HKEY_CURRENT_USER, L"SOFTWARE\\Winlator\\WFM\\CurrentISOPath", &hkey) == ERROR_SUCCESS) {
        ret = RegQueryValue(hkey, NULL, result, &size);
        RegCloseKey(hkey);
    }
    // Wine 在值不存在时返回 ERROR_SUCCESS 且数据为空串，因此必须同时判断内容
    return ret == ERROR_SUCCESS && result[0] != L'\0';
}

#endif