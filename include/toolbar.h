#ifndef TOOLBAR_H
#define TOOLBAR_H

void createToolbar();
void createToolButtons(void);
// 首帧之后补 CMD / Explorer 两个按钮的真实图标（启动时故意留空，见 toolbar.c 里的说明）。
void appendShellExeToolIcons(void);
void toolbarCommand(int command);
void setPasteButtonEnabled(bool enabled);
void onUpButtonClick();
void onCopyButtonClick();
void onCutButtonClick();
void onPasteButtonClick();
void onDeleteButtonClick();
void onNewFolderButtonClick();
void onNewFileButtonClick();
void onBookmarkButtonClick();

#endif