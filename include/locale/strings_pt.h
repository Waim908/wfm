#ifndef STRINGS_PT_H
#define STRINGS_PT_H

#include "resource.h"

static inline void loadStrings_pt() {
    lc_str.app_name = APP_NAME;
    lc_str.app_version = L"Versão " APP_VERSION;
    lc_str.app_dev_name = L"por " APP_DEV_NAME;
    lc_str.app_mod_name = L"Modificado por Waim908";
    lc_str.app_url = L"https://github.com/Waim908/wfm";
    lc_str.application = L"Aplicação";
    lc_str.shortcut = L"Atalho";
    lc_str.file = L"Arquivo";
    lc_str.folder = L"Pasta";
    lc_str.local_drive = L"Unidade Local";
    lc_str.computer = L"Computador";
    lc_str.desktop = L"Desktop";
    lc_str.documents = L"Documentos";
    lc_str.exit = L"Sair";
    lc_str.edit = L"Editar";
    lc_str.cut = L"Recortar";
    lc_str.copy = L"Copiar";
    lc_str.paste = L"Colar";
    lc_str.paste_shortcut = L"Colar Atalho";
    lc_str.select_all = L"Selecionar Tudo";
    lc_str.view = L"Visualizar";
    lc_str.large_icons = L"Ícones Grandes";
    lc_str.small_icons = L"Ícones Pequenos";
    lc_str.list = L"Lista";
    lc_str.details = L"Detalhes";
    lc_str.help = L"Ajuda";
    lc_str.about = L"Sobre";
    lc_str.ok = L"OK";
    lc_str.cancel = L"Cancelar";
    lc_str.loading = L"Carregando...";
    lc_str.open = L"Abrir";
    lc_str.create_shortcut = L"Criar Atalho";
    lc_str.delete = L"Excluir";
    lc_str.rename = L"Renomear";
    lc_str.new_folder = L"Nova Pasta";
    lc_str.new_file = L"Novo Arquivo";
    lc_str.items = L"Itens";
    lc_str.load_iso_image = L"Carregar Imagem ISO";
    lc_str.unload_iso_image = L"Descarregar Imagem ISO";
    lc_str.no_media = L"Sem mídia";
    lc_str.alert = L"Alerta";
    lc_str.enter_folder_name = L"Digite o nome da pasta:";
    lc_str.enter_file_name = L"Digite o nome do arquivo:";
    lc_str.enter_new_name = L"Digite o novo nome:";
    lc_str.name = L"Nome";
    lc_str.type = L"Tipo";
    lc_str.size = L"Tamanho";
    lc_str.date = L"Data";
    lc_str.path = L"Caminho";
    lc_str.deleting_files = L"Excluindo arquivos";
    lc_str.copying_files = L"Copiando arquivos";
    lc_str.moving_files = L"Movendo arquivos";
    lc_str.extracting_files = L"Extraindo arquivos";
    lc_str.confirm_delete = L"Confirmar Excluir";
    lc_str.confirm_exit = L"Confirmar Saída";
    lc_str.search = L"Pesquisar";
    lc_str.up = L"Acima";
    lc_str.show_icon = L"Mostrar Ícone";
    lc_str.bookmarks = L"Favoritos";
    lc_str.bookmark = L"Favorito";
    lc_str.add_bookmark = L"Adicionar aos Favoritos";
    lc_str.remove_bookmark = L"Remover Favorito";
    lc_str.bookmark_exists = L"Este caminho já está nos favoritos";
    lc_str.bookmark_path_not_found = L"Caminho favorito não existe: %ls";
    lc_str.auto_open_on_start = L"Abrir na Inicialização";
    lc_str.cancel_auto_open = L"Cancelar Inicialização";
    lc_str.auto_open_path_not_found = L"Não foi possível abrir o caminho na inicialização (não encontrado): %ls";

    lc_str.clear_icon_cache = L"Limpar Cache de Ícones";
    lc_str.mount = L"Montar";
    lc_str.locate_iso = L"Localizar Arquivo de Imagem";
    lc_str.open_file_location = L"Abrir Local do Arquivo";
    lc_str.unmount_iso = L"Desmontar";
    lc_str.msg_no_mounted_image = L"Nenhum arquivo de imagem montado";
    lc_str.msg_x_drive_not_found = L"Drive X: n\u00e3o encontrado. Adicione a unidade X: no winecfg e defina como CD-ROM";
    lc_str.msg_image_dir_not_found = L"Diretório do arquivo de imagem não encontrado";
    lc_str.msg_no_libcdio = L"Esta versão não suporta libcdio";
    lc_str.save_icon = L"Salvar Ícone";
    lc_str.fmt_drive_space = L"%ls / %ls";

    lc_str.fmt_file = L"Arquivo %ls";

    lc_str.msg_invalid_iso_image_file = L"Arquivo de Imagem ISO inválido!";
    
    lc_str.msg_deleting_files = L"Excluindo arquivos, aguarde...";
    lc_str.msg_copying_files = L"Copiando arquivos, aguarde...";
    lc_str.msg_moving_files = L"Movendo arquivos, aguarde...";
    lc_str.msg_extracting_files = L"Extraindo arquivos, aguarde...";
    lc_str.msg_cancel_file_operation = L"Você quer cancelar a operação?";
    lc_str.msg_confirm_delete_item = L"Tem certeza de que deseja excluir \"%ls\"?";
    lc_str.msg_confirm_delete_multiple_items = L"Tem certeza de que deseja excluir %d itens?";
    lc_str.msg_confirm_exit_app = L"Tem certeza de que deseja sair?";

    // Toolbar short strings (abbreviated for narrow buttons)
    lc_str.tb_up = L"Acima";
    lc_str.tb_copy = L"Copiar";
    lc_str.tb_cut = L"Cortar";
    lc_str.tb_paste = L"Colar";
    lc_str.tb_delete = L"Excluir";
    lc_str.tb_new_folder = L"Pasta";
    lc_str.tb_new_file = L"Arquivo";
    lc_str.tb_bookmark = L"Favorito";
    lc_str.import_reg = L"Importar para o Registro";
    lc_str.show_hidden_files = L"Mostrar Arquivos Ocultos";
}

#endif