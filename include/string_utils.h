#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <wchar.h>

static inline wchar_t* strReplace(wchar_t *str, const wchar_t *oldval, const wchar_t *newval, bool freestr) {
    if (!str || !oldval || !newval) return str;
    wchar_t* pwc;
    pwc = wcsstr(str, oldval);
    if (!pwc) return str;
    int offset = pwc - str;
    
    wchar_t* endstr = str + (offset + wcslen(oldval));
    int resLen = offset + wcslen(newval) + wcslen(endstr) + 1;
    wchar_t* res = malloc(resLen * sizeof(wchar_t));
    if (!res) return str;   // 分配失败时保持原串，避免调用方拿到 NULL 后解引用
    wcsncpy_s(res, resLen, str, offset);
    wcscat_s(res, resLen, newval);
    wcscat_s(res, resLen, endstr);
    res[resLen-1] = L'\0';
    
    if (freestr) free(str);
    return res;
}

// resultSize 是 result 的元素个数（含终止符）。旧实现不传容量、直接按输入长度写，
// 一旦输入长于目标缓冲就越界写栈（与 S1/S2 同类）。
static inline void strToLower(const wchar_t* str, wchar_t* result, int resultSize) {
    if (!result || resultSize <= 0) return;
    int count = str ? (int)wcslen(str) : 0;
    if (count > resultSize - 1) count = resultSize - 1;
    for (int i = 0; i < count; i++) result[i] = towlower(str[i]);
    result[count] = L'\0';
}

static inline void strToUpper(const wchar_t* str, wchar_t* result, int resultSize) {
    if (!result || resultSize <= 0) return;
    int count = str ? (int)wcslen(str) : 0;
    if (count > resultSize - 1) count = resultSize - 1;
    for (int i = 0; i < count; i++) result[i] = towupper(str[i]);
    result[count] = L'\0';
}

static inline wchar_t* wcsstrIgnoreCase(const wchar_t* str, const wchar_t* pattern) {
    if (!str || !pattern || !*pattern) return (wchar_t*)str;
    
    size_t patternLen = wcslen(pattern);
    for (const wchar_t* p = str; *p; p++) {
        if (towlower(*p) == towlower(*pattern)) {
            size_t i;
            for (i = 1; i < patternLen; i++) {
                if (!p[i] || towlower(p[i]) != towlower(pattern[i])) break;
            }
            if (i == patternLen) return (wchar_t*)p;
        }
    }
    return NULL;
}

#endif