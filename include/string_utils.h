#ifndef STRING_UTILS_H
#define STRING_UTILS_H

#include <wchar.h>

static inline wchar_t* strReplace(wchar_t *str, const wchar_t *oldval, const wchar_t *newval, bool freestr) {
    wchar_t* pwc;
    pwc = wcsstr(str, oldval);
    if (!pwc) return str;
    int offset = pwc - str;
    
    wchar_t* endstr = str + (offset + wcslen(oldval));
    int resLen = offset + wcslen(newval) + wcslen(endstr) + 1;
    wchar_t* res = malloc(resLen * sizeof(wchar_t));
    wcsncpy_s(res, resLen, str, offset);
    wcscat_s(res, resLen, newval);
    wcscat_s(res, resLen, endstr);
    res[resLen-1] = L'\0';
    
    if (freestr) free(str);
    return res;
}

static inline void strToLower(wchar_t* str, wchar_t* result) {
    int count = wcslen(str);
    for (int i = 0; i < count; i++) result[i] = towlower(str[i]);
    result[count] = L'\0';
}

static inline void strToUpper(wchar_t* str, wchar_t* result) {
    int count = wcslen(str);
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