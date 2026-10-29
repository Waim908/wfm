EXE_NAME=wfm.exe

# 检测操作系统并选择工具链（必须放在上面的默认值判定之前）
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

# ------------------------------------------------------ 架构与工具链选择
# ARCH:      x86_64 / aarch64（Linux 下默认 clang + x86_64）
# TOOLCHAIN: clang（llvm-mingw，Linux 默认）/ gcc（mingw-w64，Windows 原生默认）
# 两者各自配套自己的 libcdio 静态库，不可混用：clang(llvm-mingw) 构建的库
# 依赖 UCRT 符号（__acrt_iob_func 等），gcc(msvcrt) 工具链里根本没有；
# gcc 构建的库则用根目录下的 libcdio.a / libiso9660.a（仅 x86_64）。
# 例子：
#   make ARCH=aarch64 USE_LIBCDIO=1        # clang 交叉编译 aarch64 版
#   make ARCH=x86_64 USE_LIBCDIO=0         # clang 编译 x86_64 版
#   make TOOLCHAIN=gcc                     # gcc 编译 x86_64 版（旧行为）
ifeq ($(UNAME_S),Linux)
    TOOLCHAIN ?= clang
    ARCH ?= x86_64
else
    TOOLCHAIN ?= gcc
    ARCH ?= x86_64
endif

ifeq ($(TOOLCHAIN)$(ARCH),gccaarch64)
    $(error gcc 工具链不支持 aarch64，请用 TOOLCHAIN=clang 或改用 llvm-mingw)
endif

# llvm-mingw 工具链位置，可用 make LLVM_MINGW=... 覆盖
LLVM_MINGW ?= /home/waim/mingw-toolchain/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64

ifeq ($(UNAME_S),Linux)
    # Linux 交叉编译到 Windows
    ifeq ($(TOOLCHAIN),clang)
        CC = $(LLVM_MINGW)/bin/$(ARCH)-w64-mingw32-clang
        RC = $(LLVM_MINGW)/bin/$(ARCH)-w64-mingw32-windres
    else
        CC = $(ARCH)-w64-mingw32-gcc
        RC = $(ARCH)-w64-mingw32-windres
    endif
    RM = rm -f
    MKDIR = mkdir -p
    RM_RF = rm -rf
else
    # Windows 原生编译
    CC = gcc
    RC = windres
    RM = del
    MKDIR = mkdir
    RM_RF = rmdir /s /q
endif

# ------------------------------------------------ 变体隔离的目标文件目录
# 每个构建变体各用一个目录。旧实现把两种变体的 .o 混放在 obj/ 下，靠
# 「CFLAGS 变了就 rm -rf obj」保证不混用 —— package.sh 的 all 分支会连续
# 构建 USE_LIBCDIO=0 与 =1，一旦那次删除没有真正生效（权限、被安全策略
# 拦截、或 Windows 下 rm 根本不存在），打出的第二个包就是上一个变体的
# 目标文件与当前变体混合链接的产物。分目录后，切换变体不需要删除任何东西。
# 目录名同时包含工具链与架构：不同工具链（gcc/clang）、不同架构
# （x86_64/aarch64）的 .o 互不兼容，绝不能混用。
ifeq ($(USE_LIBCDIO),1)
    CFG = libcdio
else
    CFG = base
endif
OBJDIR = obj/$(TOOLCHAIN)-$(ARCH)-$(CFG)

INCLUDE_DIR = -I./include

# -MMD -MP：为每个 .o 生成 .d 依赖文件（见文件末尾的 -include）。缺了它，
# 改头文件不会触发依赖它的 .c 重编，结构体布局不一致的目标文件会被链在
# 一起 —— 开 LTO 时报 -Wlto-type-mismatch，不开 LTO 时就是静默的内存错乱。
# -flto / -ffunction-sections / -fdata-sections + -Wl,--gc-sections：把未被
# 引用的函数与数据从最终 exe 中剔除（wfm.exe 368KB → 278KB）。
# -Wextra 用于及早暴露函数签名问题。gcc 与 clang 下这些开关行为一致。
# -s 只放在 LDFLAGS：clang 编译阶段会报「-s unused」警告，gcc 则静默忽略。
CFLAGS = -O3 -std=c99 -DUNICODE -D_UNICODE -DCOBJMACROS -DWINVER=0x0603 -D_WIN32_WINNT=0x0603 -Wall -Wextra -MMD -MP -flto -ffunction-sections -fdata-sections
LDFLAGS = -s -flto -Wl,--gc-sections -lcomctl32 -lgdi32 -lole32 -luuid -lcomdlg32 -lgdiplus -lshlwapi -Wl,--subsystem,windows

# libcdio 静态库按工具链/架构配套：
# clang 用 libcdio-static/<arch>/（llvm-mingw 构建，x86_64 与 aarch64 各一份，
# 由 agent_workspace/libcdio-static-build/build-libcdio-static.sh 维护）；
# gcc 用根目录的 libcdio.a / libiso9660.a（mingw-w64 gcc 构建，仅 x86_64）。
# 链接顺序：iso9660 依赖 cdio，被依赖者放后面。
ifeq ($(USE_LIBCDIO),1)
    INCLUDE_DIR += -I./include/libcdio
    ifeq ($(TOOLCHAIN),gcc)
        LDFLAGS += ./libiso9660.a ./libcdio.a -lwinmm
    else
        LDFLAGS += ./libcdio-static/$(ARCH)/libiso9660.a ./libcdio-static/$(ARCH)/libcdio.a -lwinmm
    endif
    CFLAGS := -DUSE_LIBCDIO -DUSE_LIBCDIO_STATIC $(CFLAGS)
endif

SRC_NAMES = main content_view toolbar navbar treeview sizebar statusbar \
            file_node file_actions input_dialog bookmarks open_with_dialog
OBJS = $(SRC_NAMES:%=$(OBJDIR)/%.o) $(OBJDIR)/resource.o
ifeq ($(USE_LIBCDIO),1)
    OBJS += $(OBJDIR)/libcdio_loader.o
endif

# ------------------------------ 同一变体内 CFLAGS 变化时才清该变体的目录
CURFLAGS := $(CFLAGS) $(INCLUDE_DIR)
STAMP = .cflags.$(TOOLCHAIN)-$(ARCH)-$(CFG)
SAVEDFLAGS := $(shell cat $(STAMP) 2>/dev/null)

ifneq ($(CURFLAGS),$(SAVEDFLAGS))
    $(shell ${RM_RF} $(OBJDIR))
    $(shell echo '$(CURFLAGS)' > $(STAMP))
    $(info CFLAGS changed -> wiping $(OBJDIR) for a clean rebuild...)
endif

# --------------------------- 变体切换时强制重新链接
# wfm.exe 是多种变体共用的输出名，它的时间戳无法表达「我是哪个变体」：
# 先建 libcdio 再建默认时，wfm.exe 比 obj/base/*.o 都新，make 会认为无需
# 重链，于是留下一个 libcdio 版的 wfm.exe 而 obj/ 里却是默认版目标文件。
# 用一个记录「上次产出 wfm.exe 的变体」的小文件做前置条件来消除这种歧义：
# 它只在变体真正变化时被重写，因此同变体连续构建不会触发多余重链。
# 工具链与架构切换同样会触发重链。
VARIANT_STAMP = .variant
SAVED_VARIANT := $(shell cat $(VARIANT_STAMP) 2>/dev/null)
CUR_VARIANT = $(TOOLCHAIN)-$(ARCH)-$(CFG)

ifneq ($(CUR_VARIANT),$(SAVED_VARIANT))
    $(shell echo '$(CUR_VARIANT)' > $(VARIANT_STAMP))
endif

all: ${EXE_NAME}

${EXE_NAME}: ${OBJS} ${VARIANT_STAMP}
	${CC} -o ${EXE_NAME} ${OBJS} ${LDFLAGS}

# 兜底规则：万一 ${VARIANT_STAMP} 不存在（首次构建且上面的写入失败），
# make 也能按需生成它，而不是直接报「无规则可生成目标」。
${VARIANT_STAMP}:
	@echo '$(CUR_VARIANT)' > $@

clean:
	${RM_RF} obj
	${RM} ${EXE_NAME}
	${RM} .cflags
	${RM} .cflags.*
	${RM} ${VARIANT_STAMP}

$(OBJDIR):
	${MKDIR} $@

# 末尾的 "| $(OBJDIR)" 是 order-only 前置条件：目录只表示「必须存在」，
# 不参与时间戳比较。写成普通前置条件（`$(OBJDIR)/%.o: src/%.c $(OBJDIR)`）
# 会让每个 .o 都依赖目录的 mtime —— 每写一个 .o 就刷新一次目录 mtime，
# 于是每轮 make 都误判一大批目标过期而重编；而最后写入的那个 .o 因与
# 目录 mtime 同秒反而被判为最新并永久跳过（改了头文件也不重编），
# 正是隐蔽的陈旧对象来源。
$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	${CC} ${CFLAGS} ${INCLUDE_DIR} -c $< -o $@

# 前置条件必须列全 res/ 下被 resource.rc 引用的每个素材：windres 不是 C
# 编译器，-MMD 管不到它，漏掉任何一个都会让「只换素材」的改动不触发重编，
# 编出来的 exe 里还是旧图标（且体积一样、看不出异常）。
$(OBJDIR)/resource.o: res/resource.rc res/Application.manifest res/main.ico res/go.ico res/refresh.ico res/search.ico res/nav_arrow.ico res/up.ico res/copy.ico res/cut.ico res/paste.ico res/delete.ico res/new_folder.ico res/new_file.ico res/shortcut_overlay.ico res/bookmark.ico res/preloader_1.ico res/preloader_2.ico res/preloader_3.ico res/preloader_4.ico res/preloader_5.ico res/preloader_6.ico res/preloader_7.ico res/preloader_8.ico res/computer.ico res/terminal.ico include/resource.h | $(OBJDIR)
	${RC} ${INCLUDE_DIR} -I./res -i $< -o $@

# 头文件依赖由 -MMD 生成，必须放在 OBJS / USE_LIBCDIO 判定之后。
# 用 -include（而非 include）容忍首次构建时 .d 尚不存在；
# -MP 为每个头文件补一条空规则，删掉头文件后不会因「找不到目标」而报错。
-include ${OBJS:.o=.d}
