# NPPWorkSpace

Native Notepad++ workspace panel with fast file navigation, persistent project containers, and an asynchronous "Find in Files" style search experience.

## English

### Overview

NPPWorkSpace is a Win32/C++ Notepad++ plugin that provides a dedicated dockable workspace panel. It is designed for large project folders, keeping the Notepad++ UI responsive while browsing folders, searching file names, or searching inside text files.

The plugin uses native Notepad++ integration for docking, opening files, dark mode, shortcut mapping, and Scintilla navigation.

### Features

- Dockable Notepad++ panel with responsive layout and minimum usable size.
- Workspace tree with folders, files, and project containers.
- Container colors saved in the `.worknpp` file.
- `[+]` / `[-]` visual expansion state for both folders and search result groups.
- Native file opening through Notepad++.
- `Ctrl+B` to show or hide the workspace panel.
- `Ctrl+P` to open the floating search dialog.
- Shortcut configuration through **Plugins > NPPWorkSpace > Shortcut Mapper**.
- Persistent workspace file path stored in the Windows registry.
- Workspace save/load support using the `.worknpp` JSON format.

### Search

NPPWorkSpace supports two search modes:

- File name search: fast indexed search over files in the selected workspace scope.
- Content search: asynchronous search inside supported text files when **Search inside files** is enabled.

Search is started only by pressing the **Search** button or pressing **Enter**. Typing does not start disk scanning, which keeps the panel responsive.

Content search provides:

- Worker-thread execution.
- Incremental results while scanning is still running.
- Progress bar with processed file count, total file count, percentage, and current file.
- Immediate cancellation while preserving already found results.
- Result grouping by file.
- Expand/collapse support for each result group.
- Highlighting only the matched text in each result line.
- Native Scintilla navigation to the selected line and match.

Supported text extensions:

```text
*.ini, *.txt, *.json, *.xml, *.lua, *.cfg, *.conf, *.csv, *.log,
*.hpp, *.h, *.cpp, *.c, *.cs, *.py, *.js, *.ts
```

Supported encoding detection includes:

```text
UTF-8, UTF-8 BOM, UTF-16 LE, UTF-16 BE, ANSI, Big5, Shift-JIS
```

### Search Scope

The **Scope** menu controls which folders or containers are included in search. Content search respects the same scope as file name search.

Available scope options include:

- Search everything.
- Search a specific container.
- Enable or disable individual folders.

### Workspace Format

Workspaces are stored as UTF-8 JSON files with the `.worknpp` extension.

Example:

```json
{
  "format": "NPPWorkSpace",
  "version": 2,
  "workspace": {
    "folders": [
      "C:\\Project"
    ],
    "containers": [
      {
        "name": "Client Project",
        "color": "#569CD6",
        "folders": [
          "D:\\Work\\ClientProject"
        ]
      }
    ],
    "searchIncluded": [],
    "searchDisabled": [],
    "shortcuts": {
      "toggleWorkspace": "Ctrl+B",
      "search": "Ctrl+P"
    }
  }
}
```

### Build

Requirements:

- Windows
- CMake 3.20 or newer
- Visual Studio with the MSVC C++ toolchain
- Notepad++ for runtime testing

Build:

```bat
build.bat
```

The script configures CMake for x64 and builds the Release DLL:

```text
build\Release\NPPWorkSpace.dll
```

### Installation

Copy the built DLL into the Notepad++ plugin directory, typically:

```text
%ProgramFiles%\Notepad++\plugins\NPPWorkSpace\NPPWorkSpace.dll
```

Restart Notepad++ and enable the plugin from the **Plugins** menu.

### Project Structure

```text
NPPWorkSpace/
|-- CMakeLists.txt
|-- README.md
|-- build.bat
`-- src/
    |-- dllmain.cpp
    |-- NotepadPlusMsgs.h
    |-- PluginDefinition.cpp
    |-- PluginInterface.h
    |-- QuickOpen.cpp
    `-- QuickOpen.h
```

## Português

### Visão Geral

NPPWorkSpace é um plugin nativo Win32/C++ para Notepad++ que adiciona um painel dockável de workspace. Ele foi pensado para projetos grandes, mantendo a interface do Notepad++ responsiva ao navegar por pastas, pesquisar nomes de arquivos ou pesquisar dentro de arquivos de texto.

O plugin usa integração nativa do Notepad++ para docking, abertura de arquivos, modo escuro, mapeamento de atalhos e navegação pelo Scintilla.

### Recursos

- Painel dockável no Notepad++ com layout responsivo e tamanho mínimo utilizável.
- Árvore de workspace com pastas, arquivos e contêineres de projeto.
- Cores de contêiner salvas no arquivo `.worknpp`.
- Estado visual `[+]` / `[-]` para pastas e grupos de resultado da pesquisa.
- Abertura de arquivos pelo fluxo nativo do Notepad++.
- `Ctrl+B` para mostrar ou ocultar o painel.
- `Ctrl+P` para abrir a janela flutuante de pesquisa.
- Configuração de atalhos em **Plugins > NPPWorkSpace > Shortcut Mapper**.
- Último arquivo de workspace salvo no Registro do Windows.
- Suporte para salvar e abrir workspaces no formato JSON `.worknpp`.

### Pesquisa

O NPPWorkSpace possui dois modos de pesquisa:

- Pesquisa por nome de arquivo: busca rápida indexada dentro do escopo selecionado.
- Pesquisa em conteúdo: busca assíncrona dentro de arquivos de texto suportados quando **Pesquisar dentro dos arquivos** está ativado.

A pesquisa só começa ao pressionar o botão **Pesquisar** ou a tecla **Enter**. Digitar no campo não inicia varredura em disco, mantendo o painel responsivo.

A pesquisa em conteúdo oferece:

- Execução em worker threads.
- Resultados incrementais enquanto a varredura ainda está em andamento.
- Barra de progresso com arquivos processados, total, percentual e arquivo atual.
- Cancelamento imediato preservando os resultados já encontrados.
- Resultados organizados por arquivo.
- Expansão/retração de cada grupo de resultado.
- Destaque apenas do texto encontrado em cada linha.
- Navegação nativa pelo Scintilla até a linha e a ocorrência selecionada.

Extensões de texto suportadas:

```text
*.ini, *.txt, *.json, *.xml, *.lua, *.cfg, *.conf, *.csv, *.log,
*.hpp, *.h, *.cpp, *.c, *.cs, *.py, *.js, *.ts
```

Detecção de encoding suportada:

```text
UTF-8, UTF-8 BOM, UTF-16 LE, UTF-16 BE, ANSI, Big5, Shift-JIS
```

### Escopo da Pesquisa

O menu **Escopo** controla quais pastas ou contêineres entram na pesquisa. A pesquisa em conteúdo respeita o mesmo escopo da pesquisa por nome.

Opções disponíveis:

- Pesquisar em tudo.
- Pesquisar em um contêiner específico.
- Ativar ou desativar pastas individuais.

### Formato do Workspace

Workspaces são salvos como arquivos JSON em UTF-8 com extensão `.worknpp`.

Exemplo:

```json
{
  "format": "NPPWorkSpace",
  "version": 2,
  "workspace": {
    "folders": [
      "C:\\Projeto"
    ],
    "containers": [
      {
        "name": "Projeto Cliente",
        "color": "#569CD6",
        "folders": [
          "D:\\Trabalho\\ProjetoCliente"
        ]
      }
    ],
    "searchIncluded": [],
    "searchDisabled": [],
    "shortcuts": {
      "toggleWorkspace": "Ctrl+B",
      "search": "Ctrl+P"
    }
  }
}
```

### Compilação

Requisitos:

- Windows
- CMake 3.20 ou superior
- Visual Studio com toolchain C++ MSVC
- Notepad++ para testes em runtime

Compilar:

```bat
build.bat
```

O script configura o CMake para x64 e compila a DLL em Release:

```text
build\Release\NPPWorkSpace.dll
```

### Instalação

Copie a DLL compilada para a pasta de plugins do Notepad++, normalmente:

```text
%ProgramFiles%\Notepad++\plugins\NPPWorkSpace\NPPWorkSpace.dll
```

Reinicie o Notepad++ e acesse o plugin pelo menu **Plugins**.

### Estrutura do Projeto

```text
NPPWorkSpace/
|-- CMakeLists.txt
|-- README.md
|-- build.bat
`-- src/
    |-- dllmain.cpp
    |-- NotepadPlusMsgs.h
    |-- PluginDefinition.cpp
    |-- PluginInterface.h
    |-- QuickOpen.cpp
    `-- QuickOpen.h
```
