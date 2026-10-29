# wfm 给 AI 维护者的约定

这是一个 Win32 文件管理器，实际运行在 Wine（Winlator / Android 容器）上。下面只写「要遵守什么、
怎么判」，不记录进度与本轮改动。

## 构建与验证

- 构建由用户自己做。agent 不在宿主上跑 `make`、不编项目源码来「验证」。
- 自查只做语法层：`-fsyntax-only`，且**必须带 `-Werror`**。不带的话只有 warning 时退出码仍是 0，
  「零告警」就是空话。覆盖 x86_64 与 aarch64、带与不带 `USE_LIBCDIO`，四个组合都要过。
- `res/resource.rc` 可以单独验，不必整包构建：
  `x86_64-w64-mingw32-windres -I include -i res/resource.rc -o <仓库外的路径>.o`，退出码 0 即语法通过。
- **手写声明的系统函数（`.c` 里的 `WINGDIPAPI xxx(...)` 这类）必须回导入库核一遍符号名**，
  语法自查抓不到这类错误，只有链接时才报 `undefined symbol`。GDI+ 的扁平 API 名字不直觉，
  例如创建纯色刷子是 `GdipCreateSolidFill`（不是 `GdipCreateSolidBrush`）。核法：
  `nm -g --defined-only <toolchain>/x86_64-w64-mingw32/lib/libgdiplus.a | grep -w 'T <符号>'`；
  aarch64 的库宿主 `nm` 不认 ARM64 COFF，要用工具链自带的 `llvm-nm`。
- 改完源码必须明确告诉用户：改了哪些文件、**要不要重编**、设备上怎么确认生效。单文件语法通过
  不等于已验证。

## Wine 行为（判据都回过源码，别凭印象推翻）

- **MessageBox 的宽度不含标题**。`dlls/user32/msgbox.c` 里
  `twidth = max((bw + bspace) * buttons + bspace - tleft, rect.right)`，`wwidth` 由它推出，标题只在
  开头 `SetWindowText` 一次，不参与尺寸；而标题绘制带 `DT_END_ELLIPSIS`（`dlls/win32u/defwnd.c`）。
  所以「短正文 + 长标题」必然被截成省略号：中文下 `MB_YESNO` 的「是 / 否」比 `Yes / No` 窄，
  撑出来的框更窄，更容易踩到。需要可控宽度或本地化按钮文字时，用自绘对话框
  （`res/resource.rc` 里的 `IDD_*` + `DialogBoxParam`），不要用 MessageBox。
- **`wcscoll` 不是热点**。`dlls/msvcrt/wcs.c` 的 `_wcscoll_l` 在 `lc_handle[LC_COLLATE] == 0` 时
  直接 `wcscmp`，而 `msvcrt_init_locale()` 把初始 locale 设成 `"C"`。排序次序因此是二进制序
  （大写在小写之前），不要按 locale 排序去推断。
- 列表与图标绘制有一批 Wine 侧的硬约束（不要开 `LVS_EX_DOUBLEBUFFER`、虚拟列表不能物化、
  排序比较器不许碰图标与文件系统、绘制帧上不能同步枚举目录）。这些在 `src/content_view.c`
  对应小节的注释里写了成因，改动前先读那一节，不要只看结论。

## 性能

- **先测再改**。项目自带打点：设 `WFM_STARTUP_LOG=<路径>` 或 `WFM_STARTUP_TRACE=1` 启用。
  一次导航的分段是：
  `nav: entry`、`nav: enum`、`refresh: entry`、`refresh: fill items`、`refresh: list attach`、
  `refresh: sort`、`refresh: prewarm shared`、`refresh: fill first screen`、`refresh: done`。
- 读法：`nav: entry` 那一条吸收的是「上一次打点到这次导航开始」之间的空闲间隔，**不是开销**；
  真正的枚举开销在同一次导航的 `nav: enum`。
- 已有实测结论（用户实际目录 2 到 16 项）：枚举 0 到 4 ms、排序 0 ms、预热 2 到 7 ms，都不是瓶颈；
  每次导航的固定成本主要在 `refresh: entry`（改主窗口标题 + 重建地址栏）。
  **不要拿「几万项的大目录」当假设去做优化**，先确认用户手上真有这种目录再动手。
- 日志文件每次启动覆盖，同一时刻只允许一个实例写（第二个实例退到 stderr）。每段的 `t=` 是该进程
  自己从 0 起算的，不要把多段混在一起读。
- 打点本身是一次同步 `WriteFile`，且这笔开销计入**下一段**的增量。测性能时不要长期开着。

## 代码与提交

- 注释用中文写清「为什么这么做」，不写状态流水（谁在哪天改了什么）。
- 注释与文档里不用 emoji、警告标记、对错标记、装饰性箭头；要醒目就加粗。
- 提交信息用中文，标题 `type(scope): 描述`，正文先一段散文交代背景与原因，再用 `*` 列具体改动；
  按功能或文件拆分，保证每个提交单独都能编过。不主动 `commit` / `push`，等用户明说。
