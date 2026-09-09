#ifndef OPEN_WITH_DIALOG_H
#define OPEN_WITH_DIALOG_H

// 检查文件扩展名是否有自定义关联程序
// 如果有，将程序路径写入 program 缓冲区并返回 true
bool getFileAssociation(const wchar_t* ext, wchar_t* program, int programSize);

// 保存文件扩展名到程序的关联
void saveFileAssociation(const wchar_t* ext, const wchar_t* program);

// 删除文件扩展名的关联
void removeFileAssociation(const wchar_t* ext);

// 显示"打开方式"对话框
// filePath: 要打开的文件完整路径
// ext: 文件扩展名（如 ".txt"），用于关联管理
// outRemoveAssoc: 输出参数，如果为 true 表示需要删除关联
// 返回: 用户选择的程序路径（需要调用者 free），取消返回 NULL
wchar_t* showOpenWithDialog(const wchar_t* filePath, const wchar_t* ext, bool* outRemoveAssoc);

#endif
