OBJS_BASE=obj/main.o obj/content_view.o obj/toolbar.o obj/navbar.o obj/treeview.o obj/sizebar.o obj/statusbar.o obj/file_node.o obj/file_actions.o obj/input_dialog.o obj/bookmarks.o obj/resource.o
OBJS_LIBCDIO=obj/libcdio_loader.o
INCLUDE_DIR=-I./include
EXE_NAME=wfm.exe

CFLAGS=-O3 -s -std=c99 -DUNICODE -D_UNICODE -DCOBJMACROS -DWINVER=0x0603 -D_WIN32_WINNT=0x0603 -Wall
LDFLAGS=-s -lcomctl32 -lgdi32 -lole32 -luuid -lcomdlg32 -lgdiplus -lshlwapi -Wl,--subsystem,windows
ifeq ($(USE_LIBCDIO),1)
OBJS=$(OBJS_BASE) $(OBJS_LIBCDIO)
INCLUDE_DIR+=-I./include/libcdio
LDFLAGS+=./libcdio.dll
CFLAGS:=-DUSE_LIBCDIO $(CFLAGS)
endif
OBJS?=$(OBJS_BASE)

# Detect OS and set compiler/toolchain accordingly
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(UNAME_S),Linux)
    # Linux cross-compilation with mingw-w64
    CC = x86_64-w64-mingw32-gcc
    RC = x86_64-w64-mingw32-windres
    RM = rm -f
    MKDIR = mkdir -p
    RMDIR = rm -rf obj
else
    # Windows native compilation
    CC = gcc
    RC = windres
    RM = del
    MKDIR = mkdir
    RMDIR = del /q obj\*.o 2>nul || exit 0
endif

# Detect build flag changes and force recompilation when USE_LIBCDIO toggles
CURFLAGS := $(CFLAGS) $(INCLUDE_DIR)
SAVEDFLAGS := $(shell cat .cflags 2>/dev/null)

ifneq ($(CURFLAGS),$(SAVEDFLAGS))
  $(shell rm -rf obj)
  $(shell echo '$(CURFLAGS)' > .cflags)
  $(info Build flags changed, forcing clean rebuild...)
endif

all: ${EXE_NAME}

${EXE_NAME}: ${OBJS}
	${CC} -o ${EXE_NAME} ${OBJS} ${LDFLAGS}

clean:
	${RMDIR}
	${RM} ${EXE_NAME}
	${RM} .cflags

obj:
	${MKDIR} obj

obj/%.o: src/%.c obj
	${CC} ${CFLAGS} ${INCLUDE_DIR} -c $< -o $@

obj/resource.o: res/resource.rc res/Application.manifest res/main.ico res/go.ico res/refresh.ico res/search.ico res/nav_arrow.ico res/up.ico res/copy.ico res/cut.ico res/paste.ico res/delete.ico res/new_folder.ico res/new_file.ico include/resource.h
	${RC} ${INCLUDE_DIR} -I./res -i $< -o $@
