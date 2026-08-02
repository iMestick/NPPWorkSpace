# NPPWorkSpace

Plugin nativo para Notepad++ que fornece um workspace próprio, integrado ao sistema de docking do Notepad++.

## Recursos

- Painel dockável integrado ao Notepad++.
- Largura limitada entre **340 e 620 px** no modo acoplado e flutuante.
- Altura limitada entre **260 e 1000 px** tanto no modo acoplado quanto no modo flutuante.
- Árvore de pastas e subpastas.
- Expandir/retrair todas as pastas.
- Remover uma pasta raiz pelo botão ou menu de contexto.
- Arrastar pastas para o Notepad++ adiciona a pasta ao NPPWorkSpace e o Folder as Workspace nativo é ocultado.
- Pesquisa de arquivos e pesquisa de conteúdo usando `>texto`.
- `Ctrl+B` para mostrar/ocultar o workspace.
- `Ctrl+P` para abrir o pesquisador flutuante.
- Atalhos continuam disponíveis no **Plugins → NPPWorkSpace → Shortcut Mapper**.
- Salvar e abrir workspaces pelo formato **`.worknpp`**.
- O `.worknpp` é texto UTF-8 estruturado em JSON, contendo pastas e atalhos.
- Ao abrir um `.worknpp`, o workspace atual é substituído pela configuração carregada.

## Formato `.worknpp`

```json
{
  "format": "NPPWorkSpace",
  "version": 1,
  "workspace": {
    "folders": [
      "C:\\Projeto",
      "D:\\OutroProjeto"
    ],
    "shortcuts": {
      "toggleWorkspace": "Ctrl+B",
      "search": "Ctrl+P"
    }
  }
}
```

## Estrutura do projeto

```text
NPPWorkSpace/
├── CMakeLists.txt
├── README.md
├── build.bat
└── src/
    ├── dllmain.cpp
    ├── NotepadPlusMsgs.h
    ├── PluginDefinition.cpp
    ├── PluginInterface.h
    ├── QuickOpen.cpp
    └── QuickOpen.h
```

O projeto usa UTF-8 no código-fonte (`/utf-8`) e Win32 Unicode para manter compatibilidade com nomes acentuados e a interface do Notepad++.

## Compilação

Execute `build.bat` na raiz do projeto. Em caso de erro, a janela permanece aberta com `pause` para permitir a leitura do erro. Em caso de sucesso, o script termina automaticamente.
