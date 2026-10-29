#ifndef INPUT_DIALOG_H
#define INPUT_DIALOG_H

// 打开输入框时预先选中哪一段。没有用 bool，是因为重命名需要「只选主文件名」这第三种
// 语义：扩展名不选中，输入新名字时它会自动保留（Windows 资源管理器就是这样），而
// 「全选」会把扩展名一起换掉。
enum InputSelectMode {
    INPUT_SELECT_NONE,      // 不选中，光标停在末尾（新建文件夹 / 新建文件）
    INPUT_SELECT_ALL,       // 整个名字全选
    INPUT_SELECT_BASENAME   // 只选到最后一个点之前，扩展名留着
};

wchar_t* InputDialog(wchar_t* title, wchar_t* label, wchar_t* defaultText, enum InputSelectMode mode);

#endif
