#ifndef STRINGS_RU_H
#define STRINGS_RU_H

#include "resource.h"

static inline void loadStrings_ru() {
    lc_str.app_name = APP_NAME;
    lc_str.app_version = L"Версия " APP_VERSION;
    lc_str.app_dev_name = L"от " APP_DEV_NAME;
    lc_str.app_mod_name = L"Модификация от Waim908";
    lc_str.app_url = L"https://github.com/Waim908/wfm";
    lc_str.application = L"Приложение";
    lc_str.shortcut = L"Ярлык";
    lc_str.file = L"Файл";
    lc_str.folder = L"Папка";
    lc_str.local_drive = L"Локальный диск";
    lc_str.cd_drive = L"CD привод";
    lc_str.computer = L"Компьютер";
    lc_str.desktop = L"Рабочий стол";
    lc_str.documents = L"Документы";
    lc_str.exit = L"Выход";
    lc_str.edit = L"Редактировать";
    lc_str.cut = L"Вырезать";
    lc_str.copy = L"Копировать";
    lc_str.paste = L"Вставить";
    lc_str.paste_shortcut = L"Вставить Ярлык";
    lc_str.select_all = L"Выбрать все";
    lc_str.view = L"Вид";
    lc_str.large_icons = L"Большие Иконки";
    lc_str.small_icons = L"Маленькие Иконки";
    lc_str.list = L"Список";
    lc_str.details = L"Подробности";
    lc_str.help = L"Помощь";
    lc_str.about = L"О программе";
    lc_str.ok = L"OK";
    lc_str.cancel = L"Отмена";
    lc_str.loading = L"Загрузка...";
    lc_str.open = L"Открыть";
    lc_str.create_shortcut = L"Создать Ярлык";
    lc_str.delete = L"Удалить";
    lc_str.rename = L"Переименовать";
    lc_str.new_folder = L"Новая папка";
    lc_str.new_file = L"Новый файл";
    lc_str.items = L"Предметы";
    lc_str.load_iso_image = L"Загрузить ISO-образ";
    lc_str.unload_iso_image = L"Выгрузить ISO-образ";
    lc_str.no_media = L"Нет СМИ";
    lc_str.alert = L"Тревога";
    lc_str.enter_folder_name = L"Введите имя папки:";
    lc_str.enter_file_name = L"Введите имя файла:";
    lc_str.enter_new_name = L"Введите новое имя:";
    lc_str.name = L"Имя";
    lc_str.type = L"Тип";
    lc_str.size = L"Размер";
    lc_str.date = L"Дата";
    lc_str.path = L"Путь";
    lc_str.deleting_files = L"Удаление файлов";
    lc_str.copying_files = L"Копирование файлов";
    lc_str.moving_files = L"Перемещение файлов";
    lc_str.extracting_files = L"Извлечение файлов";
    lc_str.confirm_delete = L"Подтвердить удаление";
    lc_str.confirm_exit = L"Подтвердить выход";
    lc_str.search = L"Поиск";
    lc_str.up = L"Вверх";
    lc_str.show_icon = L"Показать значок";
    lc_str.bookmarks = L"Закладки";
    lc_str.bookmark = L"Закладка";
    lc_str.add_bookmark = L"Добавить в закладки";
    lc_str.remove_bookmark = L"Удалить закладку";
    lc_str.bookmark_exists = L"Этот путь уже в закладках";
    lc_str.bookmark_path_not_found = L"Путь закладки не существует: %ls";
    lc_str.auto_open_on_start = L"Открыть при запуске";
    lc_str.cancel_auto_open = L"Отменить запуск";
    lc_str.auto_open_path_not_found = L"Не удалось открыть путь при запуске (не найден): %ls";

    lc_str.clear_icon_cache = L"Очистить Кэш Иконок";
    lc_str.mount = L"Монтировать";
    lc_str.locate_iso = L"Найти файл образа";
    lc_str.open_file_location = L"Открыть расположение файла";
    lc_str.unmount_iso = L"Размонтировать";
    lc_str.msg_no_mounted_image = L"Нет смонтированного образа";
    lc_str.msg_x_drive_not_found = L"\u0414\u0438\u0441\u043a X: \u043d\u0435 \u043d\u0430\u0439\u0434\u0435\u043d. \u0414\u043e\u0431\u0430\u0432\u044c\u0442\u0435 \u0434\u0438\u0441\u043a X: \u0432 winecfg \u0438 \u0443\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u0435 \u0442\u0438\u043f CD-ROM";
    lc_str.msg_image_dir_not_found = L"Папка файла образа не найдена";
    lc_str.msg_no_libcdio = L"Эта версия не поддерживает libcdio";
    lc_str.msg_confirm_unmount_iso = L"Отмонтирование удалит ВСЁ содержимое диска X:.\n\nОбычно X: указывает на реальную папку, поэтому отменить это нельзя. Продолжить?";
    lc_str.save_icon = L"Сохранить значок";
    lc_str.fmt_drive_space = L"%ls / %ls";

    lc_str.fmt_file = L"%ls Файл";

    lc_str.msg_invalid_iso_image_file = L"Неверный файл образа ISO!";
    
    lc_str.msg_deleting_files = L"Удаление файлов, пожалуйста, подождите...";
    lc_str.msg_copying_files = L"Копирование файлов, пожалуйста, подождите...";
    lc_str.msg_moving_files = L"Перемещение файлов, пожалуйста, подождите...";
    lc_str.msg_extracting_files = L"Извлечение файлов, пожалуйста, подождите...";
    lc_str.msg_cancel_file_operation = L"Вы хотите отменить операцию?";
    lc_str.msg_confirm_delete_item = L"Вы уверены, что хотите удалить \"%ls\"?";
    lc_str.msg_confirm_delete_multiple_items = L"Вы уверены, что хотите удалить %d элементов?";
    lc_str.msg_confirm_exit_app = L"Вы уверены, что хотите выйти?";

    // Toolbar short strings (abbreviated for narrow buttons)
    lc_str.tb_up = L"Вверх";
    lc_str.tb_copy = L"Копир.";
    lc_str.tb_cut = L"Вырез.";
    lc_str.tb_paste = L"Вставить";
    lc_str.tb_delete = L"Удалить";
    lc_str.tb_new_folder = L"Папка";
    lc_str.tb_new_file = L"Файл";
    lc_str.tb_bookmark = L"Закладка";
    lc_str.import_reg = L"Импорт в реестр";
    lc_str.show_hidden_files = L"Показать скрытые файлы";
    lc_str.open_with = L"Открыть с помощью";
    lc_str.open_with_label = L"Выберите программу для открытия этого файла:";
    lc_str.browse = L"Обзор...";
    lc_str.always_use = L"Всегда использовать эту программу для этого типа файлов";
    lc_str.open_with_menu = L"Открыть с помощью...";

    // Положение папок (политика сортировки)
    lc_str.folder_position = L"Положение папок";
    lc_str.folder_pos_top = L"Сверху (как в Проводнике)";
    lc_str.folder_pos_bottom = L"Снизу (классический WFM)";
    lc_str.folder_pos_plain = L"Не группировать (по столбцу)";
}

#endif