# NPPWorkSpace

Native Notepad++ workspace panel with fast file navigation, persistent project containers, asynchronous content search, and SSH/SFTP folders backed by a local cache.

## English

### Overview

NPPWorkSpace is a native Win32/C++ Notepad++ plugin that provides a dedicated dockable workspace panel. It is designed for large projects and keeps the Notepad++ UI responsive while browsing folders, searching file names, searching inside text files, or working with folders hosted on an Ubuntu/Linux server.

The plugin uses native Notepad++ integration for docking, opening files, dark mode, shortcut mapping, save notifications, and Scintilla navigation.

### Features

- Dockable Notepad++ panel with a responsive layout and minimum usable size.
- Workspace tree with local folders, SSH folders, files, and project containers.
- SSH roots are shown by the selected folder name only, such as `Data`, without exposing the cache GUID or the full server path in the tree.
- Stable refresh that preserves expanded folders, selection, scroll position, active panel, and keyboard focus.
- The toolbar **Refresh** button, `F5`, or **Refresh/Sync** in the tree context menu updates local roots or synchronizes the selected SSH root.
- Container colors saved in the `.worknpp` file.
- `[+]` / `[-]` visual expansion state for folders and search result groups.
- Native file opening through Notepad++.
- `Ctrl+B` to show or hide the workspace panel.
- `Ctrl+P` to open the floating search dialog.
- Shortcut configuration through **Plugins > NPPWorkSpace > Shortcut Mapper**.
- Persistent workspace file path stored in the Windows registry.
- Workspace save/load support using the UTF-8 JSON `.worknpp` format.

### Ubuntu Server over SSH/SFTP

The **SSH** button in the workspace toolbar opens the remote connection manager. A connection can use:

- Host or IP address and a custom SSH port.
- Password authentication.
- Private-key authentication, with an optional key passphrase. Modern OpenSSH private keys are loaded directly; a sibling `.pub` file is not required.
- Interactive folder browsing starting at `/root`.

After **Connect** succeeds, the dialog immediately displays the directories inside `/root`. Expand a directory to load only that level, select the desired folder, choose an existing workspace container in **Add to**, and select **Add folder**. The SSH dialog never creates a container; it only connects and inserts the selected folder into the destination chosen by the user. **Workspace root (no container)** remains available when top-level placement is intentional. The same saved connection can add several remote folders to the workspace.

For every selected folder, the plugin:

1. Tests SSH authentication and SFTP access.
2. Shows the server SHA-256 host-key fingerprint on the first connection.
3. Stores the accepted fingerprint in the workspace.
4. Creates a separate local cache under `%LOCALAPPDATA%\NPPWorkSpace\Remote\<connection-id>\<folder-id>`.
5. Downloads the selected remote directory into that cache on a worker thread.
6. Adds the cache inside the selected existing container, or at workspace root when explicitly selected. The tree displays only the remote folder name.

One authenticated SSH/SFTP session is kept per connection and reused by every selected folder. A keep-alive is sent while Notepad++ is open. The session remains available for the whole Notepad++ process and is closed only when the connection is explicitly removed or Notepad++ closes.

Files are edited locally by Notepad++. Whenever Notepad++ confirms that a cached file was saved, the plugin queues an SFTP upload for that file. Uploads use a temporary remote file followed by a rename, reducing the chance of leaving a partially written destination file.

Opening the SSH interface for an existing connection, browsing to another directory, and choosing **Add folder** adds it through the same persistent login. Adding an already configured directory performs another remote-to-local update. Local files with a newer modification time are preserved so a manual update does not overwrite a recently saved local change waiting to be uploaded.

The recursive downloader first reads the complete contents of each remote directory and closes its SFTP directory handle before entering child folders. This prevents restrictive SFTP servers from ending the parent listing after the first subdirectory and ensures every sibling folder is synchronized.

When a synchronization finishes, the visible tree is refreshed automatically without requiring a scope change. The remote refresh is applied only after the transfer completes, avoiding a second intermediate redraw. Expanded nodes, the current selection, scroll position, active panel, and keyboard focus are restored, and the file-name index used by the panel search and `Ctrl+P` is rebuilt automatically. Changing a search scope immediately reapplies the active query. Double-clicking a cached file opens its normalized local cache path through `NPPM_DOOPEN`. Temporary partial-download files are hidden from the tree and both search modes.

The download is intentionally non-destructive: files removed from the server are not automatically deleted from the local cache. Symbolic links, Unix special files, and names that cannot be represented safely on Windows are skipped.

> The Ubuntu server must permit the selected account to log in through SSH and use SFTP. Access to `/root` normally requires a permitted root SSH login or another account with access to that directory.

### Credential Security

- Passwords and private-key passphrases are encrypted with Windows DPAPI before being written to `.worknpp`.
- Plain-text passwords and passphrases are never serialized to the workspace file.
- DPAPI protection is tied to the Windows user profile and machine. A copied workspace can keep the connection settings, but its protected secret may need to be entered again on another account or computer.
- The accepted SHA-256 host-key fingerprint is pinned. A later mismatch blocks the connection instead of silently trusting a different server.
- The private-key file path is saved, but the private-key file itself is not embedded in `.worknpp`.

### Search

NPPWorkSpace supports two search modes:

- File name search: fast indexed search over files in the selected workspace scope.
- Content search: asynchronous search inside supported text files when **Search inside files** is enabled.

Search is started only by pressing the **Search** button or pressing **Enter**. Typing does not start disk scanning.

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

The **Scope** menu controls which folders or containers are included in search. Local folders, SSH-cached folders, and folders opened directly through WSL UNC paths (`\\wsl.localhost\<distribution>\...` or `\\wsl$\<distribution>\...`) use the same search pipeline and respect the selected scope. The filename index used by both the panel and `Ctrl+P` enumerates WSL/network directories independently, so one inaccessible subfolder cannot abort the remaining scan. Both WSL aliases are normalized as the same scope.

Available scope options include:

- Search everything.
- Search a specific container.
- Enable or disable individual folders.

### Workspace Format

Workspaces are stored as UTF-8 JSON files with the `.worknpp` extension. Version 4 adds `connectionId`, allowing several remote folders to share one persistent SSH session.

Example with sensitive values shortened:

```json
{
  "format": "NPPWorkSpace",
  "version": 4,
  "workspace": {
    "folders": [
      "C:\\Project",
      "C:\\Users\\User\\AppData\\Local\\NPPWorkSpace\\Remote\\connection-7a7b...\\folder-a1..."
    ],
    "containers": [],
    "remoteFolders": [
      {
        "id": "folder-a1...",
        "connectionId": "connection-7a7b...",
        "name": "Ubuntu Server",
        "host": "192.168.1.20",
        "port": "22",
        "username": "root",
        "auth": "password",
        "passwordProtected": "<DPAPI Base64>",
        "privateKeyPath": "",
        "privateKeyPassphraseProtected": "",
        "remotePath": "/root",
        "localCachePath": "C:\\Users\\User\\AppData\\Local\\NPPWorkSpace\\Remote\\connection-7a7b...\\folder-a1...",
        "hostKeySha256": "SHA256:..."
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
- Git for Windows and internet access on the first build; `build.bat` bootstraps vcpkg, OpenSSL and libssh 0.12.2
- Notepad++ for runtime testing

Build:

```bat
build.bat
```

CMake downloads the verified libssh 0.12.2 security release and links it statically with OpenSSL supplied by vcpkg. This release replaces the previous libssh2 transport completely; no libssh2 object or library is used by the plugin. The plugin first negotiates with the standard modern algorithm set and only retries with a limited compatibility profile when the normal negotiation fails. The deployment remains a single runtime DLL, without separate SSH or OpenSSL DLL files.

The Release plugin is generated at:

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
|-- vcpkg.json
`-- src/
    |-- dllmain.cpp
    |-- NotepadPlusMsgs.h
    |-- PluginDefinition.cpp
    |-- PluginInterface.h
    |-- QuickOpen.cpp
    |-- QuickOpen.h
    |-- RemoteSsh.cpp
    `-- RemoteSsh.h
```

## Português

### Visão Geral

NPPWorkSpace é um plugin nativo Win32/C++ para Notepad++ que adiciona um painel dockável de workspace. Ele foi pensado para projetos grandes, mantendo a interface do Notepad++ responsiva ao navegar por pastas, pesquisar nomes de arquivos, pesquisar dentro de arquivos de texto ou trabalhar com pastas hospedadas em um Ubuntu/Linux Server.

O plugin usa integração nativa do Notepad++ para docking, abertura de arquivos, modo escuro, mapeamento de atalhos, notificação de salvamento e navegação pelo Scintilla.

### Recursos

- Painel dockável no Notepad++ com layout responsivo e tamanho mínimo utilizável.
- Árvore de workspace com pastas locais, pastas SSH, arquivos e contêineres de projeto.
- Raízes SSH exibidas somente pelo nome da pasta selecionada, como `Data`, sem mostrar GUID do cache ou o caminho completo do servidor na árvore.
- Atualização estável preservando pastas expandidas, seleção, rolagem, painel ativo e foco do teclado.
- O botão **Atualizar** da barra, `F5` ou **Atualizar/Sincronizar** no menu de contexto atualiza pastas locais ou sincroniza a raiz SSH selecionada.
- Cores de contêiner salvas no arquivo `.worknpp`.
- Estado visual `[+]` / `[-]` para pastas e grupos de resultado da pesquisa.
- Abertura de arquivos pelo fluxo nativo do Notepad++.
- `Ctrl+B` para mostrar ou ocultar o painel.
- `Ctrl+P` para abrir a janela flutuante de pesquisa.
- Configuração de atalhos em **Plugins > NPPWorkSpace > Shortcut Mapper**.
- Último arquivo de workspace salvo no Registro do Windows.
- Suporte para salvar e abrir workspaces no formato JSON UTF-8 `.worknpp`.

### Ubuntu Server por SSH/SFTP

O novo botão **SSH** na barra do workspace abre o gerenciador de conexões remotas. A conexão aceita:

- Host ou endereço IP e porta SSH personalizada.
- Autenticação por senha.
- Autenticação por chave privada, com frase-chave opcional. Chaves privadas modernas do OpenSSH são carregadas diretamente; o arquivo `.pub` ao lado não é obrigatório.
- Navegação interativa de pastas iniciando em `/root`.

Depois que **Conectar** for concluído, a interface mostra imediatamente as pastas existentes em `/root`. Ao expandir uma pasta, apenas aquele nível é consultado. Selecione a pasta desejada, escolha um contêiner já existente em **Adicionar em** e clique em **Adicionar pasta**. A interface SSH nunca cria um contêiner: ela somente conecta e insere a pasta no destino escolhido. A opção **Raiz do workspace (sem contêiner)** continua disponível quando essa posição for intencional. A mesma conexão salva pode adicionar várias pastas remotas ao workspace.

Para cada pasta selecionada, o plugin:

1. Testa a autenticação SSH e o acesso por SFTP.
2. Exibe a impressão SHA-256 da chave do servidor na primeira conexão.
3. Salva a chave aceita no workspace.
4. Cria um cache separado em `%LOCALAPPDATA%\NPPWorkSpace\Remote\<id-da-conexão>\<id-da-pasta>`.
5. Baixa a pasta remota para o cache usando uma worker thread.
6. Adiciona o cache dentro do contêiner existente escolhido, ou na raiz quando isso for selecionado explicitamente. A árvore mostra somente o nome da pasta remota.

Uma única sessão SSH/SFTP autenticada é mantida por conexão e reutilizada por todas as pastas selecionadas. O plugin envia keep-alive enquanto o Notepad++ está aberto. A sessão permanece disponível durante todo o processo do Notepad++ e só é encerrada ao remover explicitamente a conexão ou fechar o Notepad++.

Os arquivos são editados localmente pelo Notepad++. Sempre que o Notepad++ confirma o salvamento de um arquivo pertencente ao cache, o plugin coloca esse arquivo na fila de envio por SFTP. O upload grava primeiro em um arquivo remoto temporário e depois o renomeia, reduzindo o risco de deixar o arquivo de destino parcialmente escrito.

Ao abrir novamente uma conexão existente, navegar até outra pasta e clicar em **Adicionar pasta**, ela é adicionada usando o mesmo login persistente. Selecionar uma pasta já configurada atualiza o cache a partir do servidor. Arquivos locais com data de modificação mais recente são preservados para que essa atualização manual não sobrescreva uma alteração recém-salva que ainda será enviada.

O download recursivo primeiro lê todo o conteúdo de cada diretório remoto e fecha o handle SFTP dessa pasta antes de entrar nas subpastas. Isso impede que servidores SFTP mais restritivos encerrem a listagem da pasta pai após a primeira subpasta e garante a sincronização de todas as pastas irmãs.

Quando a sincronização termina, a árvore visível é atualizada automaticamente, sem precisar trocar o escopo. A atualização remota só é aplicada depois que a transferência termina, evitando um segundo redesenho intermediário. Pastas expandidas, seleção, rolagem, painel ativo e foco do teclado são restaurados, e o índice de nomes usado pela pesquisa do painel e pelo `Ctrl+P` é reconstruído automaticamente. Ao mudar o escopo, a pesquisa ativa é reaplicada imediatamente. O duplo clique abre o caminho normalizado do arquivo no cache por `NPPM_DOOPEN`. Arquivos temporários de download parcial ficam ocultos da árvore e dos dois modos de pesquisa.

O download é propositalmente não destrutivo: arquivos removidos no servidor não são apagados automaticamente do cache local. Links simbólicos, arquivos especiais do Unix e nomes que não podem ser representados com segurança no Windows são ignorados.

> O Ubuntu Server precisa permitir que a conta escolhida entre por SSH e utilize SFTP. O acesso a `/root` normalmente exige login SSH de root permitido ou outra conta com acesso a essa pasta.

### Segurança das Credenciais

- Senhas e frases-chave de chaves privadas são criptografadas com o DPAPI do Windows antes de serem gravadas no `.worknpp`.
- A senha e a frase-chave nunca são serializadas em texto puro.
- A proteção DPAPI fica vinculada ao usuário e ao computador Windows. Ao copiar o workspace para outro usuário ou PC, as configurações permanecem, mas o segredo protegido poderá precisar ser digitado novamente.
- A impressão SHA-256 aceita da chave do servidor fica fixada. Se a chave mudar posteriormente, a conexão é bloqueada em vez de confiar silenciosamente em outro servidor.
- O caminho da chave privada é salvo, mas o arquivo da chave privada não é incorporado ao `.worknpp`.

### Pesquisa

O NPPWorkSpace possui dois modos de pesquisa:

- Pesquisa por nome de arquivo: busca rápida indexada dentro do escopo selecionado.
- Pesquisa em conteúdo: busca assíncrona dentro de arquivos de texto suportados quando **Pesquisar dentro dos arquivos** está ativado.

A pesquisa só começa ao pressionar o botão **Pesquisar** ou a tecla **Enter**. Digitar no campo não inicia varredura em disco.

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

O menu **Escopo** controla quais pastas ou contêineres entram na pesquisa. Pastas locais, caches SSH e pastas abertas diretamente pelo caminho UNC do WSL (`\\wsl.localhost\<distribuição>\...` ou `\\wsl$\<distribuição>\...`) usam o mesmo sistema e respeitam o escopo escolhido. O índice de nomes utilizado pelo painel e pelo `Ctrl+P` enumera cada pasta do WSL/rede de forma independente; uma subpasta inacessível não interrompe mais toda a pesquisa. Os dois aliases do WSL são tratados como o mesmo escopo.

Opções disponíveis:

- Pesquisar em tudo.
- Pesquisar em um contêiner específico.
- Ativar ou desativar pastas individuais.

### Formato do Workspace

Workspaces são salvos como arquivos JSON em UTF-8 com extensão `.worknpp`. A versão 4 adiciona `connectionId`, permitindo que várias pastas remotas compartilhem uma única sessão SSH persistente.

Exemplo com os valores sensíveis abreviados:

```json
{
  "format": "NPPWorkSpace",
  "version": 4,
  "workspace": {
    "folders": [
      "C:\\Projeto",
      "C:\\Users\\Usuario\\AppData\\Local\\NPPWorkSpace\\Remote\\connection-7a7b...\\folder-a1..."
    ],
    "containers": [],
    "remoteFolders": [
      {
        "id": "folder-a1...",
        "connectionId": "connection-7a7b...",
        "name": "Ubuntu Server",
        "host": "192.168.1.20",
        "port": "22",
        "username": "root",
        "auth": "password",
        "passwordProtected": "<DPAPI em Base64>",
        "privateKeyPath": "",
        "privateKeyPassphraseProtected": "",
        "remotePath": "/root",
        "localCachePath": "C:\\Users\\Usuario\\AppData\\Local\\NPPWorkSpace\\Remote\\connection-7a7b...\\folder-a1...",
        "hostKeySha256": "SHA256:..."
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
- Git for Windows e acesso à internet na primeira compilação; o script baixa o vcpkg, o OpenSSL e a versão verificada do libssh 0.12.2
- Notepad++ para testes em runtime

Compilar:

```bat
build.bat
```

O CMake baixa a versão de segurança verificada do libssh 0.12.2 e a vincula estaticamente ao OpenSSL fornecido pelo vcpkg. Esta versão substitui completamente o transporte anterior baseado em libssh2; nenhum objeto ou biblioteca libssh2 é usado pelo plugin. A conexão tenta primeiro os algoritmos modernos padrão e só usa um perfil limitado de compatibilidade quando a negociação normal falha. O plugin continua sendo uma única DLL em runtime, sem DLLs separadas do SSH ou do OpenSSL.

A DLL Release será criada em:

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
|-- vcpkg.json
`-- src/
    |-- dllmain.cpp
    |-- NotepadPlusMsgs.h
    |-- PluginDefinition.cpp
    |-- PluginInterface.h
    |-- QuickOpen.cpp
    |-- QuickOpen.h
    |-- RemoteSsh.cpp
    `-- RemoteSsh.h
```
