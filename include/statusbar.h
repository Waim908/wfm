#ifndef STATUSBAR_H
#define STATUSBAR_H

void createStatusbar();
void setStatusbarText(wchar_t* text);

// 状态栏右端的视图切换按钮（大图标 / 小图标 / 列表 / 详细信息）。
// 它们是状态栏的兄弟子窗口，父窗口尺寸变化后要调 layout 重新摆位。
void layoutStatusbarViewButtons(void);
// 视图样式变了之后重画高亮，由 setViewStyle 调用。
void refreshStatusbarViewButtons(void);

#endif
