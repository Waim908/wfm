# ==============================
# Cross-compile for Windows (MinGW-w64)
# ==============================

# 可执行文件名
TARGET = wfm.exe

# 目标对象文件
OBJS = obj/main.o obj/content_view.o obj/toolbar.o obj/navbar.o obj/treeview.o \
       obj/sizebar.o obj/statusbar.o obj/file_node.o obj/file_actions.o \
       obj/input_dialog.o obj/resource.o

# MinGW 工具链前缀（根据你的系统可能需要调整）
# 常见值: x86_64-w64-mingw32- （64位） 或 i686-w64-mingw32- （32位）
PREFIX = x86_64-w64-mingw32-

CC = $(PREFIX)gcc
RC = $(PREFIX)windres

# 头文件搜索路径（使用正斜杠 /）
INCLUDE_DIR = -I./include -I./include/libcdio

# 链接选项：
# - 不要直接链接 .dll！应链接导入库（如 -lcdio）
# - 确保 libcdio 已为 MinGW 交叉编译，并提供 libcdio.a 或 libcdio.dll.a
LDFLAGS = -s -lcomctl32 -lgdi32 -lole32 -luuid -lcdio -Wl,--subsystem,windows

# 编译选项
CFLAGS = -O2 -std=c99 -DUNICODE -D_UNICODE -DCOBJMACROS -DWINVER=0x0600 -Wall

# ==============================
# 构建规则
# ==============================

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(OBJS) $(TARGET)
	-rmdir obj

# 自动创建 obj 目录
obj:
	mkdir -p obj

# 编译 C 源文件
obj/%.o: src/%.c | obj
	$(CC) $(CFLAGS) $(INCLUDE_DIR) -c $< -o $@

# 编译 Windows 资源文件 (.rc)
obj/resource.o: res/resource.rc \
                res/Application.manifest \
                res/main.ico res/go.ico res/refresh.ico res/search.ico \
                res/nav_arrow.ico res/up.ico res/copy.ico res/cut.ico \
                res/paste.ico res/delete.ico res/new_folder.ico res/new_file.ico \
                include/resource.h | obj
	$(RC) $(INCLUDE_DIR) -I./res -i $< -o $@