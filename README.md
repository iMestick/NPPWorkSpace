# NPPWorkSpace

Plugin para Notepad++ que fornece um workspace próprio com árvore de pastas, subpastas, arquivos e pesquisa rápida.

## Atalhos

- **Ctrl+B**: mostrar/ocultar o painel NPPWorkSpace.
- **Ctrl+P**: abrir o pesquisador flutuante.
- Os dois comandos aparecem no **Plugins > NPPWorkSpace** e podem ser alterados no **Shortcut Mapper** do próprio Notepad++.

## Interface

- O workspace é um painel dockável integrado ao Notepad++.
- A pesquisa permanente fica em uma caixa separada no topo.
- Os comandos de workspace ficam em uma caixa separada abaixo da pesquisa.
- O Ctrl+P abre uma janela de pesquisa pertencente ao Notepad++, sem comportamento de janela independente.
- A janela de pesquisa não fecha ao clicar fora; `Esc` ou o botão X fecha.
- A árvore suporta pastas e subpastas expansíveis.

## Encoding

O projeto é compilado explicitamente em UTF-8 no MSVC (`/utf-8`) e os arquivos de workspace/configuração são gravados em UTF-8 com BOM, preservando nomes de pastas e arquivos com acentos.


## NPPWorkSpace

- O NPPWorkSpace substitui visualmente o painel nativo **Folder as Workspace** do Notepad++.
- Pastas que chegam ao Folder as Workspace nativo (inclusive por arrastar e soltar do Explorer) são detectadas e importadas automaticamente para o NPPWorkSpace.
- O painel nativo é mantido oculto e o NPPWorkSpace passa a ser a fonte de verdade das pastas.
- Os botões do painel usam símbolos compactos e tooltips para manter a interface limpa e semelhante aos controles nativos do Notepad++.
- A busca Ctrl+P continua sendo um popup flutuante e fecha automaticamente quando perde a ativação (clique fora), além de Esc.
