#include "QuickOpen.h"
#include "NotepadPlusMsgs.h"

#include <algorithm>
#include <cwctype>
#include <codecvt>
#include <locale>
#include <fstream>
#include <functional>
#include <array>
#include <iterator>
#include <system_error>
#include <shellapi.h>
#include <shlobj.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
constexpr wchar_t WINDOW_CLASS[] = L"NPPWorkSpacePanel";
constexpr wchar_t SETTINGS_CLASS[] = L"NPPWorkSpaceSettings";
constexpr wchar_t SEARCH_POPUP_CLASS[] = L"NPPWorkSpaceSearchPopup";
constexpr wchar_t PANEL_NAME[] = L"NPPWorkSpace";
constexpr wchar_t MODULE_NAME[] = L"NPPWorkSpace.dll";

constexpr int ID_TITLE = 1000;
constexpr int ID_SEARCH = 1001;
constexpr int ID_TREE = 1002;
constexpr int ID_RESULTS = 1003;
constexpr int ID_ADD = 1004;
constexpr int ID_NEW = 1005;
constexpr int ID_SAVE = 1006;
constexpr int ID_REMOVE = 1007;
constexpr int ID_SETTINGS = 1008;
constexpr int ID_EXPAND_ALL = 1012;
constexpr int ID_COLLAPSE_ALL = 1013;
constexpr int ID_STATUS = 1009;
constexpr int ID_SEARCH_GROUP = 1010;
constexpr int ID_WORKSPACE_GROUP = 1011;
constexpr int ID_POPUP_SEARCH = 1200;
constexpr int ID_POPUP_RESULTS = 1201;
constexpr UINT_PTR WORKSPACE_SYNC_TIMER = 4101;

constexpr int ID_SETTINGS_MODE = 1100;
constexpr int ID_SETTINGS_CLOSE = 1101;
constexpr int ID_SETTINGS_SAVE = 1102;

constexpr int ID_NODE_DUMMY = 2000;
constexpr int MAX_SEARCH_RESULTS = 250;
constexpr int SEARCH_LIMIT_PER_ROOT = 20000;

QuickOpen* g_instance = nullptr;

struct SortingData4lParamMirror
{
    std::wstring _rootPath;
    std::wstring _label;
    bool _isFolder = false;
};

std::string utf8FromWide(const std::wstring& value)
{
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wideFromUtf8(const std::string& value)
{
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

bool chooseFolder(HWND owner, std::filesystem::path& result)
{
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Adicionar pasta ao NPPWorkSpace";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;

    wchar_t path[MAX_PATH * 4]{};
    const bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;

    result = std::filesystem::path(path);
    return true;
}

BOOL CALLBACK enumTree(HWND hwnd, LPARAM lp)
{
    auto* result = reinterpret_cast<HWND*>(lp);
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, 64);
    if (_wcsicmp(cls, L"SysTreeView32") != 0) return TRUE;

    const int id = GetDlgCtrlID(hwnd);
    if (id == 3531)
    {
        *result = hwnd;
        return FALSE;
    }

    HWND p = hwnd;
    for (int i = 0; i < 8 && p; ++i)
    {
        wchar_t text[256]{};
        GetWindowTextW(p, text, 256);
        if (_wcsicmp(text, L"Folder as Workspace") == 0)
        {
            *result = hwnd;
            return FALSE;
        }
        p = GetParent(p);
    }
    return TRUE;
}
}

QuickOpen& QuickOpen::instance()
{
    static QuickOpen instance;
    return instance;
}

void QuickOpen::initialize(HWND nppHandle)
{
    _npp = nppHandle;
    g_instance = this;

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    loadSettings();
    createWindow();
    if (_npp) {
        DragAcceptFiles(_npp, TRUE);
        _oldNppProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(_npp, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::nppProc)));
    }
    refreshWorkspace();
    disableNativeFolderWorkspace();
}

void QuickOpen::destroy()
{
    if (_syncTimer && _window)
    {
        KillTimer(_window, _syncTimer);
        _syncTimer = 0;
    }
    if (_tooltips)
    {
        DestroyWindow(_tooltips);
        _tooltips = nullptr;
    }
    if (_settingsWindow)
    {
        DestroyWindow(_settingsWindow);
        _settingsWindow = nullptr;
    }
    if (_searchPopup)
    {
        DestroyWindow(_searchPopup);
        _searchPopup = nullptr;
        _searchPopupEdit = nullptr;
        _searchPopupResults = nullptr;
    }

    if (_npp && _oldNppProc) {
        SetWindowLongPtrW(_npp, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(_oldNppProc));
        _oldNppProc = nullptr;
    }

    unregisterDock();

    if (_window)
    {
        DestroyWindow(_window);
        _window = nullptr;
    }

    if (_font) DeleteObject(_font);
    if (_titleFont) DeleteObject(_titleFont);
    if (_symbolFont) DeleteObject(_symbolFont);
    _font = nullptr;
    _titleFont = nullptr;
    _symbolFont = nullptr;
    g_instance = nullptr;
}

bool QuickOpen::isVisible() const
{
    return _window && IsWindowVisible(_window);
}

void QuickOpen::createWindow()
{
    if (_window) return;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &QuickOpen::windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = WINDOW_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    // Match the compact Win32 font metrics used by Notepad++ dialogs instead
    // of the oversized font that made the previous panel look cramped.
    _font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    _titleFont = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    _symbolFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");

    // This is a CHILD window intentionally. Notepad++'s docking manager takes
    // ownership of its placement and visibility after registration.
    _window = CreateWindowExW(
        WS_EX_CONTROLPARENT,
        WINDOW_CLASS,
        PANEL_NAME,
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 460, 700,
        _npp,
        nullptr,
        GetModuleHandleW(nullptr),
        this);

    if (!_window) return;

    createControls();
    applyTheme();
    _syncTimer = SetTimer(_window, WORKSPACE_SYNC_TIMER, 500, nullptr);
}

void QuickOpen::registerDockPanel()
{
    registerDock();
}

void QuickOpen::registerDock()
{
    if (!_window || _registeredDock || !_npp) return;

    DockedWidgetData data{};
    data.hClient = _window;
    data.pszName = PANEL_NAME;
    data.dlgID = _dockCommandId;
    data.uMask = DWS_ICONTAB | DWS_DF_CONT_LEFT;
    data.hIconTab = LoadIconW(nullptr, IDI_APPLICATION);
    data.pszModuleName = MODULE_NAME;

    if (SendMessageW(_npp, NPPM_DMMREGASDCKDLG, 0, reinterpret_cast<LPARAM>(&data)))
        _registeredDock = true;
}

void QuickOpen::unregisterDock()
{
    if (!_npp || !_window || !_registeredDock) return;
    // Notepad++ unregisters docking widgets as part of plugin shutdown. Hiding
    // first avoids leaving a stale tab during plugin unload.
    SendMessageW(_npp, NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(_window));
    _registeredDock = false;
}

void QuickOpen::createControls()
{
    // Two distinct native-looking group boxes keep the panel visually close to
    // Notepad++ dialogs: search on top, workspace actions underneath.
    _searchGroup = CreateWindowExW(0, L"BUTTON", L"Pesquisar",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        8, 4, 420, 58, _window, reinterpret_cast<HMENU>(ID_SEARCH_GROUP),
        GetModuleHandleW(nullptr), nullptr);

    _search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL,
        18, 24, 400, 26, _window, reinterpret_cast<HMENU>(ID_SEARCH),
        GetModuleHandleW(nullptr), nullptr);

    _workspaceGroup = CreateWindowExW(0, L"BUTTON", L"Workspace",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        8, 66, 420, 52, _window, reinterpret_cast<HMENU>(ID_WORKSPACE_GROUP),
        GetModuleHandleW(nullptr), nullptr);

    _addFolder = CreateWindowExW(0, L"BUTTON", L"\xE710",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        18, 86, 32, 28, _window, reinterpret_cast<HMENU>(ID_ADD),
        GetModuleHandleW(nullptr), nullptr);
    _newWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE8A7",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        54, 86, 32, 28, _window, reinterpret_cast<HMENU>(ID_NEW),
        GetModuleHandleW(nullptr), nullptr);
    _saveWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE74E",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        90, 86, 32, 28, _window, reinterpret_cast<HMENU>(ID_SAVE),
        GetModuleHandleW(nullptr), nullptr);
    _removeFolder = CreateWindowExW(0, L"BUTTON", L"\xE74D",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        126, 86, 32, 28, _window, reinterpret_cast<HMENU>(ID_REMOVE),
        GetModuleHandleW(nullptr), nullptr);
    _settings = CreateWindowExW(0, L"BUTTON", L"\xE713",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        162, 86, 32, 28, _window, reinterpret_cast<HMENU>(ID_SETTINGS),
        GetModuleHandleW(nullptr), nullptr);
    _expandAll = CreateWindowExW(0, L"BUTTON", L"\xE8A0",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        198, 86, 32, 28, _window, reinterpret_cast<HMENU>(ID_EXPAND_ALL),
        GetModuleHandleW(nullptr), nullptr);
    _collapseAll = CreateWindowExW(0, L"BUTTON", L"\xE8A1",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        234, 86, 32, 28, _window, reinterpret_cast<HMENU>(ID_COLLAPSE_ALL),
        GetModuleHandleW(nullptr), nullptr);

    _tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
        TVS_SHOWSELALWAYS | TVS_INFOTIP,
        10, 126, 410, 500, _window, reinterpret_cast<HMENU>(ID_TREE),
        GetModuleHandleW(nullptr), nullptr);

    _results = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        10, 126, 410, 500, _window, reinterpret_cast<HMENU>(ID_RESULTS),
        GetModuleHandleW(nullptr), nullptr);

    _status = CreateWindowExW(0, L"STATIC", L"Ctrl+B mostrar/ocultar  |  Ctrl+P pesquisar",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        10, 632, 410, 22, _window, reinterpret_cast<HMENU>(ID_STATUS),
        GetModuleHandleW(nullptr), nullptr);

    for (HWND h : {_searchGroup, _workspaceGroup, _search, _tree, _results,
                   _addFolder, _newWorkspace, _saveWorkspace, _removeFolder,
                   _settings, _expandAll, _collapseAll, _status})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);

    for (HWND h : {_addFolder, _newWorkspace, _saveWorkspace, _removeFolder,
                   _settings, _expandAll, _collapseAll})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(_symbolFont), TRUE);

    SendMessageW(_search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Pesquisar arquivos...  (use >texto para pesquisar dentro dos arquivos)"));

    createTooltips();

    ListView_SetExtendedListViewStyle(_results, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 230;
    col.pszText = const_cast<LPWSTR>(L"Arquivo");
    ListView_InsertColumn(_results, 0, &col);
    col.cx = 400;
    col.pszText = const_cast<LPWSTR>(L"Caminho");
    ListView_InsertColumn(_results, 1, &col);

    // Ask Notepad++ to apply its own native dark-mode treatment to the panel.
    SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                 static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(_window));

    _oldSearchProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        _search, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::editProc)));
}

void QuickOpen::createTooltips()
{
    if (_tooltips) return;
    _tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        _window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!_tooltips) return;
    SendMessageW(_tooltips, TTM_SETMAXTIPWIDTH, 0, 320);
    addButtonTooltip(_addFolder, L"Adicionar pasta ao NPPWorkSpace");
    addButtonTooltip(_newWorkspace, L"Criar um novo NPPWorkSpace");
    addButtonTooltip(_saveWorkspace, L"Salvar o NPPWorkSpace atual");
    addButtonTooltip(_removeFolder, L"Remover a pasta selecionada");
    addButtonTooltip(_settings, L"Configurar o NPPWorkSpace");
    addButtonTooltip(_expandAll, L"Expandir todas as pastas");
    addButtonTooltip(_collapseAll, L"Retrair todas as pastas");
}

void QuickOpen::addButtonTooltip(HWND button, const wchar_t* text)
{
    if (!_tooltips || !button) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = _window;
    ti.uId = reinterpret_cast<UINT_PTR>(button);
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(_tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

void QuickOpen::disableNativeFolderWorkspace()
{
    if (!_npp) return;
    HWND tree = findFolderWorkspaceTree();
    if (tree)
    {
        HWND panel = GetParent(tree);
        for (int i = 0; i < 8 && panel; ++i)
        {
            wchar_t title[256]{};
            GetWindowTextW(panel, title, static_cast<int>(std::size(title)));
            if (_wcsicmp(title, L"Folder as Workspace") == 0)
            {
                SendMessageW(_npp, NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(panel));
                break;
            }
            panel = GetParent(panel);
        }
        // If the docking wrapper could not be found, at least hide the tree.
        ShowWindow(tree, SW_HIDE);
    }
}

void QuickOpen::syncNativeFolderWorkspace()
{
    if (!_npp) return;
    const auto roots = getNppWorkspaceRoots();
    bool changed = false;
    for (const auto& root : roots)
    {
        if (std::find(_savedRoots.begin(), _savedRoots.end(), root) == _savedRoots.end())
        {
            _savedRoots.push_back(root);
            changed = true;
        }
    }
    if (changed)
    {
        saveWorkspace();
        _nppRoots = roots;
        rebuildWorkspaceTree();
    }
    else if (_nppRoots != roots)
    {
        _nppRoots = roots;
        rebuildWorkspaceTree();
    }
    disableNativeFolderWorkspace();
}

void QuickOpen::layoutControls(int width, int height)
{
    if (!_window) return;
    constexpr int minWidth = 280;
    constexpr int maxWidth = 620;
    constexpr int minHeight = 260;
    constexpr int maxHeight = 1000;
    width = (std::max)(minWidth, (std::min)(maxWidth, width));
    height = (std::max)(minHeight, (std::min)(maxHeight, height));

    const int pad = 8;
    const int searchGroupH = 58;
    const int workspaceGroupY = 64;
    const int workspaceGroupH = 58;
    const int contentY = 128;
    const int statusH = 22;
    const int contentH = (std::max)(60, height - contentY - statusH - 6);

    MoveWindow(_searchGroup, pad, 4, width - 2 * pad, searchGroupH, TRUE);
    MoveWindow(_search, pad + 10, 24, width - 2 * pad - 20, 26, TRUE);
    MoveWindow(_workspaceGroup, pad, workspaceGroupY, width - 2 * pad, workspaceGroupH, TRUE);

    constexpr int buttonSize = 32;
    constexpr int gap = 5;
    const int buttonY = 82;
    int x = pad + 10;
    HWND buttons[] = {_addFolder, _newWorkspace, _saveWorkspace, _removeFolder, _settings, _expandAll, _collapseAll};
    for (HWND button : buttons)
    {
        MoveWindow(button, x, buttonY, buttonSize, 30, TRUE);
        x += buttonSize + gap;
    }

    MoveWindow(_tree, pad, contentY, width - 2 * pad, contentH, TRUE);
    MoveWindow(_results, pad, contentY, width - 2 * pad, contentH, TRUE);
    MoveWindow(_status, pad, height - statusH - 2, width - 2 * pad, statusH, TRUE);
}

void QuickOpen::toggleWorkspace()
{
    createWindow();
    if (!isVisible()) showWorkspace();
    else hideWorkspace();
}

void QuickOpen::showWorkspace()
{
    createWindow();
    if (!_window) return;
    SendMessageW(_npp, NPPM_DMMSHOW, 0, reinterpret_cast<LPARAM>(_window));
    SetFocus(_tree);
}

void QuickOpen::hideWorkspace()
{
    if (!_npp || !_window) return;
    SendMessageW(_npp, NPPM_DMMHIDE, 0, reinterpret_cast<LPARAM>(_window));
}

void QuickOpen::focusSearch()
{
    createSearchPopup();
    showSearchPopup();
    SetFocus(_searchPopupEdit);
    SendMessageW(_searchPopupEdit, EM_SETSEL, 0, -1);
}

void QuickOpen::createSearchPopup()
{
    if (_searchPopup) return;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &QuickOpen::searchPopupProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = SEARCH_POPUP_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    _searchPopup = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
        SEARCH_POPUP_CLASS,
        L"NPPWorkSpace - Pesquisar",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 760, 430,
        _npp, nullptr, GetModuleHandleW(nullptr), this);
    if (!_searchPopup) return;

    CreateWindowExW(0, L"STATIC", L"Pesquisar no NPPWorkSpace",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        12, 10, 300, 24, _searchPopup, nullptr, GetModuleHandleW(nullptr), nullptr);

    _searchPopupEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL,
        12, 38, 720, 30, _searchPopup, reinterpret_cast<HMENU>(ID_POPUP_SEARCH),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(_searchPopupEdit, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupEdit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Nome do arquivo/pasta ou >texto para pesquisar dentro dos arquivos..."));

    _searchPopupResults = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        12, 78, 720, 310, _searchPopup, reinterpret_cast<HMENU>(ID_POPUP_RESULTS),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(_searchPopupResults, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    ListView_SetExtendedListViewStyle(_searchPopupResults, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 240;
    col.pszText = const_cast<LPWSTR>(L"Arquivo");
    ListView_InsertColumn(_searchPopupResults, 0, &col);
    col.cx = 440;
    col.pszText = const_cast<LPWSTR>(L"Caminho");
    ListView_InsertColumn(_searchPopupResults, 1, &col);

    HWND hint = CreateWindowExW(0, L"STATIC", L"Enter abrir  |  Esc fechar",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        12, 394, 300, 20, _searchPopup, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);

    SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                 static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(_searchPopup));

    _oldPopupSearchProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        _searchPopupEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::editProc)));
}

void QuickOpen::layoutSearchPopup()
{
    if (!_searchPopup) return;
    RECT rc{};
    GetClientRect(_searchPopup, &rc);
    const int w = (std::max)(420, static_cast<int>(rc.right - rc.left));
    const int h = (std::max)(250, static_cast<int>(rc.bottom - rc.top));
    MoveWindow(_searchPopupEdit, 12, 38, w - 24, 30, TRUE);
    MoveWindow(_searchPopupResults, 12, 78, w - 24, h - 118, TRUE);
}

void QuickOpen::showSearchPopup()
{
    createSearchPopup();
    if (!_searchPopup) return;
    // Every Ctrl+P invocation starts a fresh search.
    if (_searchPopupEdit) SetWindowTextW(_searchPopupEdit, L"");
    if (_searchPopupResults) ListView_DeleteAllItems(_searchPopupResults);
    _searchResults.clear();
    layoutSearchPopup();
    RECT npp{};
    GetWindowRect(_npp, &npp);
    const int width = 760;
    const int height = 430;
    const int x = npp.left + ((npp.right - npp.left) - width) / 2;
    const int y = npp.top + ((npp.bottom - npp.top) - height) / 3;
    SetWindowPos(_searchPopup, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    SetForegroundWindow(_searchPopup);
}

void QuickOpen::hideSearchPopup()
{
    if (_searchPopup) ShowWindow(_searchPopup, SW_HIDE);
}

void QuickOpen::updatePopupSearch()
{
    if (!_searchPopupEdit) return;
    const std::wstring query = getWindowText(_searchPopupEdit);
    if (query.empty())
    {
        ListView_DeleteAllItems(_searchPopupResults);
        return;
    }
    showPopupSearchResults(query);
}

void QuickOpen::showPopupSearchResults(const std::wstring& query)
{
    std::vector<SearchResult> found;
    for (const auto& root : getWorkspaceRootsForPanel())
    {
        if (found.size() >= MAX_SEARCH_RESULTS) break;
        searchDirectory(root, query, found, MAX_SEARCH_RESULTS);
    }
    std::sort(found.begin(), found.end(), [&](const SearchResult& a, const SearchResult& b)
    {
        if (!query.empty() && query.front() == L'>') return lower(a.relative) < lower(b.relative);
        const int as = (std::max)(fuzzyScore(query, a.path.filename().wstring()), fuzzyScore(query, a.relative));
        const int bs = (std::max)(fuzzyScore(query, b.path.filename().wstring()), fuzzyScore(query, b.relative));
        if (as != bs) return as > bs;
        return lower(a.relative) < lower(b.relative);
    });
    _searchResults = found;
    ListView_DeleteAllItems(_searchPopupResults);
    for (size_t i = 0; i < _searchResults.size(); ++i)
    {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(_searchResults[i].path.filename().c_str());
        ListView_InsertItem(_searchPopupResults, &item);
        ListView_SetItemText(_searchPopupResults, static_cast<int>(i), 1,
                             const_cast<LPWSTR>(_searchResults[i].relative.c_str()));
    }
    if (!_searchResults.empty())
        ListView_SetItemState(_searchPopupResults, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void QuickOpen::openPopupSearchResult()
{
    const int row = ListView_GetNextItem(_searchPopupResults, -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(_searchResults.size())) return;
    const std::wstring path = _searchResults[static_cast<size_t>(row)].path.wstring();
    SendMessageW(_npp, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path.c_str()));
    hideSearchPopup();
}

void QuickOpen::refreshWorkspace()
{
    createWindow();
    _nppRoots = getNppWorkspaceRoots();
    loadWorkspace();
    for (const auto& root : _nppRoots)
        if (std::find(_savedRoots.begin(), _savedRoots.end(), root) == _savedRoots.end())
            _savedRoots.push_back(root);
    saveWorkspace();
    rebuildWorkspaceTree();
}

void QuickOpen::clearTreeData()
{
    _nodeData.clear();
}

void QuickOpen::rebuildWorkspaceTree(bool /*preserveExpansion*/)
{
    if (!_tree) return;
    clearTreeData();
    TreeView_DeleteAllItems(_tree);

    // NPPWorkSpace is the authoritative workspace. Roots discovered in
    // Notepad++'s legacy Folder as Workspace are imported into _savedRoots
    // and the native panel is kept hidden.
    for (const auto& root : _savedRoots)
        addRootToTree(root, false);

    _searchOnly = false;
    ShowWindow(_tree, SW_SHOW);
    ShowWindow(_results, SW_HIDE);

    std::wstring status = L"NPPWorkSpace  •  " + std::to_wstring(_savedRoots.size()) + L" pasta(s)  •  Ctrl+B mostrar/ocultar  •  Ctrl+P pesquisar";
    SetWindowTextW(_status, status.c_str());
}

void QuickOpen::addRootToTree(const std::filesystem::path& root, bool fromNppWorkspace)
{
    if (root.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    const std::wstring label = root.filename().wstring().empty() ? root.wstring() : root.filename().wstring();
    addNode(_tree, TVI_ROOT, label, root, NodeType::Root, fromNppWorkspace, true);
}

void QuickOpen::addNode(HWND tree, HTREEITEM parent, const std::wstring& label,
                        const std::filesystem::path& path, NodeType type,
                        bool fromNppWorkspace, bool hasChildren)
{
    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = const_cast<LPWSTR>(label.c_str());

    auto* data = new NodeData{type, path, fromNppWorkspace};
    ins.item.lParam = reinterpret_cast<LPARAM>(data);
    HTREEITEM item = TreeView_InsertItem(tree, &ins);
    _nodeData[item] = *data;

    if (hasChildren)
    {
        TVINSERTSTRUCTW dummy{};
        dummy.hParent = item;
        dummy.hInsertAfter = TVI_LAST;
        dummy.item.mask = TVIF_TEXT | TVIF_PARAM;
        dummy.item.pszText = const_cast<LPWSTR>(L"...");
        dummy.item.lParam = ID_NODE_DUMMY;
        TreeView_InsertItem(tree, &dummy);
    }
}

void QuickOpen::expandNode(HTREEITEM item)
{
    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;
    if (it->second.type == NodeType::File) return;

    HTREEITEM child = TreeView_GetChild(_tree, item);
    if (child)
    {
        TVITEMW tv{};
        tv.mask = TVIF_PARAM;
        tv.hItem = child;
        TreeView_GetItem(_tree, &tv);
        if (tv.lParam == ID_NODE_DUMMY)
        {
            TreeView_DeleteItem(_tree, child);
            addDirectoryChildren(_tree, item, it->second.path, it->second.fromNppWorkspace);
        }
    }
}

void QuickOpen::addDirectoryChildren(HWND tree, HTREEITEM parent,
                                     const std::filesystem::path& directory,
                                     bool fromNppWorkspace)
{
    std::vector<std::filesystem::directory_entry> dirs;
    std::vector<std::filesystem::directory_entry> files;
    std::error_code ec;

    std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; it != std::filesystem::directory_iterator{} && !ec; it.increment(ec))
    {
        const auto& entry = *it;
        const auto path = entry.path();
        std::error_code itemEc;
        if (entry.is_directory(itemEc))
        {
            if (!isHiddenSystemDirectory(path)) dirs.push_back(entry);
        }
        else if (entry.is_regular_file(itemEc))
        {
            files.push_back(entry);
        }
    }

    auto byName = [](const auto& a, const auto& b)
    {
        std::wstring an = lower(a.path().filename().wstring());
        std::wstring bn = lower(b.path().filename().wstring());
        return an < bn;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    for (const auto& d : dirs)
    {
        std::error_code childEc;
        bool hasChildren = std::filesystem::directory_iterator(d.path(),
            std::filesystem::directory_options::skip_permission_denied, childEc) != std::filesystem::directory_iterator{};
        addNode(tree, parent, d.path().filename().wstring(), d.path(), NodeType::Folder,
                fromNppWorkspace, hasChildren);
    }

    for (const auto& f : files)
        addNode(tree, parent, f.path().filename().wstring(), f.path(), NodeType::File,
                fromNppWorkspace, false);
}

void QuickOpen::populateDirectory(HWND tree, HTREEITEM parent,
                                  const std::filesystem::path& directory,
                                  bool rootNode, bool fromNppWorkspace,
                                  std::unordered_map<HTREEITEM, NodeData>& /*nodes*/)
{
    if (rootNode) addRootToTree(directory, fromNppWorkspace);
    else addDirectoryChildren(tree, parent, directory, fromNppWorkspace);
}

void QuickOpen::openTreeSelection()
{
    HTREEITEM item = TreeView_GetSelection(_tree);
    if (!item) return;

    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    if (it->second.type == NodeType::File)
    {
        const std::wstring path = it->second.path.wstring();
        SendMessageW(_npp, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path.c_str()));
    }
    else
    {
        expandNode(item);
        TVITEMW stateItem{};
        stateItem.mask = TVIF_STATE;
        stateItem.stateMask = TVIS_EXPANDED;
        stateItem.hItem = item;
        TreeView_GetItem(_tree, &stateItem);
        TreeView_Expand(_tree, item, (stateItem.state & TVIS_EXPANDED) ? TVE_COLLAPSE : TVE_EXPAND);
    }
}

void QuickOpen::showTreeContextMenu(HTREEITEM item, POINT screenPoint)
{
    auto it = _nodeData.find(item);
    if (it == _nodeData.end() || it->second.type != NodeType::Root || it->second.fromNppWorkspace)
        return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_REMOVE, L"Remover pasta do NPPWorkSpace");
    SetForegroundWindow(_window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, screenPoint.x, screenPoint.y, 0, _window, nullptr);
    DestroyMenu(menu);
}

void QuickOpen::expandAllFolders()
{
    if (!_tree) return;
    std::function<void(HTREEITEM)> expand = [&](HTREEITEM item)
    {
        for (HTREEITEM current = item; current; current = TreeView_GetNextSibling(_tree, current))
        {
            auto it = _nodeData.find(current);
            if (it != _nodeData.end() && it->second.type != NodeType::File)
            {
                expandNode(current);
                TreeView_Expand(_tree, current, TVE_EXPAND);
                expand(TreeView_GetChild(_tree, current));
            }
        }
    };
    expand(TreeView_GetRoot(_tree));
}

void QuickOpen::collapseAllFolders()
{
    if (!_tree) return;
    std::function<void(HTREEITEM)> collapse = [&](HTREEITEM item)
    {
        for (HTREEITEM current = item; current; current = TreeView_GetNextSibling(_tree, current))
        {
            HTREEITEM child = TreeView_GetChild(_tree, current);
            if (child)
            {
                collapse(child);
                TreeView_Expand(_tree, current, TVE_COLLAPSE);
            }
        }
    };
    collapse(TreeView_GetRoot(_tree));
}

void QuickOpen::handleTreeDoubleClick(LPNMTREEVIEWW tv)
{
    if (tv) openTreeSelection();
}

void QuickOpen::handleTreeItemExpanding(LPNMTREEVIEWW tv)
{
    if (tv && tv->action == TVE_EXPAND) expandNode(tv->itemNew.hItem);
}

HWND QuickOpen::findFolderWorkspaceTree() const
{
    HWND tree = nullptr;
    if (_npp) EnumChildWindows(_npp, enumTree, reinterpret_cast<LPARAM>(&tree));
    return tree;
}

bool QuickOpen::isReadableAddress(const void* p, size_t bytes)
{
    if (!p || !bytes) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t end = begin + mbi.RegionSize;
    const uintptr_t address = reinterpret_cast<uintptr_t>(p);
    return address >= begin && bytes <= end - address;
}

std::wstring QuickOpen::getTreeItemText(HWND tree, HTREEITEM item)
{
    wchar_t buffer[2048]{};
    TVITEMW tv{};
    tv.mask = TVIF_TEXT;
    tv.hItem = item;
    tv.pszText = buffer;
    tv.cchTextMax = static_cast<int>(std::size(buffer));
    return TreeView_GetItem(tree, &tv) ? std::wstring(buffer) : L"";
}

std::vector<std::filesystem::path> QuickOpen::getNppWorkspaceRoots() const
{
    std::vector<std::filesystem::path> roots;
    HWND tree = findFolderWorkspaceTree();
    if (!tree) return roots;

    // Notepad++'s Folder as Workspace stores _rootPath only on root nodes.
    // Reading only the top level avoids interpreting the private lParam of
    // normal folder/file nodes as SortingData4lParam.
    for (HTREEITEM item = TreeView_GetRoot(tree); item; item = TreeView_GetNextSibling(tree, item))
    {
        TVITEMW tv{};
        tv.mask = TVIF_PARAM;
        tv.hItem = item;
        if (!TreeView_GetItem(tree, &tv) || !tv.lParam) continue;
        if (!isReadableAddress(reinterpret_cast<void*>(tv.lParam), sizeof(SortingData4lParamMirror))) continue;

        const auto* data = reinterpret_cast<const SortingData4lParamMirror*>(tv.lParam);
        if (!data->_isFolder || data->_rootPath.empty()) continue;

        std::error_code ec;
        const std::filesystem::path path(data->_rootPath);
        if (std::filesystem::is_directory(path, ec)) roots.push_back(path);
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

bool QuickOpen::isHiddenSystemDirectory(const std::filesystem::path& p)
{
    const std::wstring n = lower(p.filename().wstring());
    if (n == L"$recycle.bin" || n == L"system volume information") return true;
    DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN) && n != L".git";
}

std::wstring QuickOpen::lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return std::towlower(c); });
    return value;
}

int QuickOpen::fuzzyScore(const std::wstring& query, const std::wstring& candidate)
{
    if (query.empty()) return 1;
    const auto q = lower(query);
    const auto c = lower(candidate);

    size_t pos = 0;
    int score = 0;
    int last = -1;
    for (wchar_t ch : q)
    {
        const size_t found = c.find(ch, pos);
        if (found == std::wstring::npos) return -1;
        score += 100;
        if (last >= 0)
            score += (static_cast<int>(found) == last + 1) ? 50 : -std::min(30, static_cast<int>(found) - last);
        if (found == 0 || c[found - 1] == L'\\' || c[found - 1] == L'/' ||
            c[found - 1] == L'_' || c[found - 1] == L'-')
            score += 40;
        last = static_cast<int>(found);
        pos = found + 1;
    }
    score -= static_cast<int>(c.size() - q.size());
    return std::max(0, std::min(1000000, score));
}

void QuickOpen::searchDirectory(const std::filesystem::path& root, const std::wstring& query,
                                std::vector<SearchResult>& results, size_t limit) const
{
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);

    size_t visited = 0;
    for (; it != std::filesystem::recursive_directory_iterator{} && !ec; it.increment(ec))
    {
        if (++visited > SEARCH_LIMIT_PER_ROOT || results.size() >= limit) break;
        const auto p = it->path();
        std::error_code itemEc;
        if (it->is_directory(itemEc))
        {
            if (isHiddenSystemDirectory(p)) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(itemEc)) continue;

        const std::wstring file = p.filename().wstring();
        std::error_code relEc;
        const std::wstring rel = std::filesystem::relative(p, root, relEc).wstring();
        bool match = false;
        if (!query.empty() && query.front() == L'>')
        {
            const std::wstring contentQuery = query.substr(1);
            match = !contentQuery.empty() && fileContainsText(p, contentQuery);
        }
        else
        {
            match = fuzzyScore(query, file) >= 0 || fuzzyScore(query, rel) >= 0;
        }
        if (match) results.push_back({p, relEc ? p.wstring() : rel});
    }
}

bool QuickOpen::fileContainsText(const std::filesystem::path& file, const std::wstring& query) const
{
    if (query.empty()) return false;
    const std::wstring ext = lower(file.extension().wstring());
    static const std::array<const wchar_t*, 24> textExts = {
        L".txt", L".ini", L".cfg", L".conf", L".log", L".xml", L".json", L".csv",
        L".cpp", L".h", L".hpp", L".c", L".cc", L".cxx", L".py", L".lua",
        L".js", L".ts", L".css", L".html", L".htm", L".md", L".yaml", L".yml"
    };
    bool allowed = false;
    for (const auto* e : textExts) if (ext == e) { allowed = true; break; }
    if (!allowed) return false;

    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > 8 * 1024 * 1024) return false;
    in.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(size), '\0');
    in.read(bytes.data(), size);
    if (!in) return false;

    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE)
    {
        const wchar_t* ptr = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
        const size_t chars = (bytes.size() - 2) / sizeof(wchar_t);
        text.assign(ptr, ptr + chars);
    }
    else if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF)
    {
        const size_t chars = (bytes.size() - 2) / 2;
        text.resize(chars);
        for (size_t i = 0; i < chars; ++i)
            text[i] = static_cast<wchar_t>((static_cast<unsigned char>(bytes[2 + i * 2]) << 8) | static_cast<unsigned char>(bytes[3 + i * 2]));
    }
    else if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        text = wideFromUtf8(bytes.substr(3));
    }
    else
    {
        text = wideFromUtf8(bytes);
        if (text.empty() && !bytes.empty())
        {
            const int n = MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
            if (n > 0)
            {
                text.resize(static_cast<size_t>(n));
                MultiByteToWideChar(CP_ACP, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
            }
        }
    }
    if (text.empty()) return false;
    return lower(text).find(lower(query)) != std::wstring::npos;
}

void QuickOpen::updateSearch()
{
    if (!_search || _suppressSearch) return;
    wchar_t buffer[2048]{};
    GetWindowTextW(_search, buffer, static_cast<int>(std::size(buffer)));
    const std::wstring query(buffer);

    if (query.empty())
    {
        _searchOnly = false;
        ShowWindow(_results, SW_HIDE);
        ShowWindow(_tree, SW_SHOW);
        return;
    }

    showSearchResults(query);
}

void QuickOpen::showSearchResults(const std::wstring& query)
{
    std::vector<SearchResult> found;
    const auto roots = getWorkspaceRootsForPanel();
    for (const auto& root : roots)
    {
        if (found.size() >= MAX_SEARCH_RESULTS) break;
        searchDirectory(root, query, found, MAX_SEARCH_RESULTS);
    }

    std::sort(found.begin(), found.end(), [&](const SearchResult& a, const SearchResult& b)
    {
        if (!query.empty() && query.front() == L'>') return lower(a.relative) < lower(b.relative);
        const int as = (std::max)(fuzzyScore(query, a.path.filename().wstring()), fuzzyScore(query, a.relative));
        const int bs = (std::max)(fuzzyScore(query, b.path.filename().wstring()), fuzzyScore(query, b.relative));
        if (as != bs) return as > bs;
        return lower(a.relative) < lower(b.relative);
    });

    _searchResults = std::move(found);
    ListView_DeleteAllItems(_results);
    for (size_t i = 0; i < _searchResults.size(); ++i)
    {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(_searchResults[i].path.filename().c_str());
        ListView_InsertItem(_results, &item);
        ListView_SetItemText(_results, static_cast<int>(i), 1,
                             const_cast<LPWSTR>(_searchResults[i].relative.c_str()));
    }

    ShowWindow(_tree, SW_HIDE);
    ShowWindow(_results, SW_SHOW);
    if (!_searchResults.empty())
        ListView_SetItemState(_results, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

    std::wstring status = std::to_wstring(_searchResults.size()) + L" resultado(s)  •  Enter abrir  •  Esc limpar pesquisa";
    SetWindowTextW(_status, status.c_str());
}

void QuickOpen::clearSearchResults()
{
    _suppressSearch = true;
    SetWindowTextW(_search, L"");
    _suppressSearch = false;
    _searchResults.clear();
    ListView_DeleteAllItems(_results);
    ShowWindow(_results, SW_HIDE);
    ShowWindow(_tree, SW_SHOW);
    SetFocus(_tree);
}

void QuickOpen::openSearchResult()
{
    const int row = ListView_GetNextItem(_results, -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(_searchResults.size())) return;
    const std::wstring path = _searchResults[static_cast<size_t>(row)].path.wstring();
    SendMessageW(_npp, NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(path.c_str()));
}

void QuickOpen::addFolder()
{
    std::filesystem::path folder;
    if (!chooseFolder(_window, folder)) return;

    if (std::find(_savedRoots.begin(), _savedRoots.end(), folder) == _savedRoots.end())
        _savedRoots.push_back(folder);

    saveWorkspace();
    rebuildWorkspaceTree();
}

void QuickOpen::removeSelectedRoot()
{
    HTREEITEM selected = TreeView_GetSelection(_tree);
    if (!selected) return;
    auto it = _nodeData.find(selected);
    if (it == _nodeData.end() || it->second.type != NodeType::Root) return;
    if (it->second.fromNppWorkspace) return;

    const auto path = it->second.path;
    _savedRoots.erase(std::remove(_savedRoots.begin(), _savedRoots.end(), path), _savedRoots.end());
    saveWorkspace();
    rebuildWorkspaceTree();
}

void QuickOpen::newWorkspace()
{
    _savedRoots.clear();
    saveWorkspace();
    rebuildWorkspaceTree();
}

void QuickOpen::saveWorkspace()
{
    const std::wstring file = getWorkspaceFilePath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(file).parent_path(), ec);

    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return;
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    out.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    for (const auto& root : _savedRoots)
    {
        const std::string line = utf8FromWide(root.wstring());
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.write("\r\n", 2);
    }
}

void QuickOpen::loadWorkspace()
{
    _savedRoots.clear();
    std::ifstream in(getWorkspaceFilePath(), std::ios::binary);
    if (!in) return;

    std::string line;
    bool first = true;
    while (std::getline(in, line))
    {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        first = false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const std::wstring wideLine = wideFromUtf8(line);
        if (wideLine.empty()) continue;
        std::error_code ec;
        std::filesystem::path path(wideLine);
        if (std::filesystem::is_directory(path, ec) &&
            std::find(_savedRoots.begin(), _savedRoots.end(), path) == _savedRoots.end())
            _savedRoots.push_back(path);
    }
}

std::vector<std::filesystem::path> QuickOpen::getWorkspaceRootsForPanel() const
{
    return _savedRoots;
}

std::wstring QuickOpen::getSettingsPath() const
{
    wchar_t buffer[32768]{};
    if (_npp && SendMessageW(_npp, NPPM_GETNPPSETTINGSDIRPATH, 32768, reinterpret_cast<LPARAM>(buffer)))
        return std::wstring(buffer) + L"\\plugins\\config\\NPPWorkSpace.ini";
    return L"NPPWorkSpace.ini";
}

std::wstring QuickOpen::getWorkspaceFilePath() const
{
    wchar_t buffer[32768]{};
    if (_npp && SendMessageW(_npp, NPPM_GETNPPSETTINGSDIRPATH, 32768, reinterpret_cast<LPARAM>(buffer)))
        return std::wstring(buffer) + L"\\plugins\\config\\NPPWorkSpace.workspace";
    return L"NPPWorkSpace.workspace";
}

void QuickOpen::loadSettings()
{
    std::ifstream in(getSettingsPath(), std::ios::binary);
    std::string line;
    bool first = true;
    while (std::getline(in, line))
    {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        first = false;
        // Reserved for future NPPWorkSpace settings. Shortcut configuration
        // remains integrated with Notepad++ Shortcut Mapper.
    }
}

void QuickOpen::saveSettings() const
{
    const std::wstring path = getSettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out)
    {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        out.write(reinterpret_cast<const char*>(bom), sizeof(bom));
        out << "Version=1\r\n";
    }
}

void QuickOpen::openSettings()
{
    if (_settingsWindow)
    {
        ShowWindow(_settingsWindow, SW_SHOW);
        SetForegroundWindow(_settingsWindow);
        return;
    }

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = &QuickOpen::settingsProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = SETTINGS_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    _settingsWindow = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        SETTINGS_CLASS,
        L"NPPWorkSpace — Configurações",
        WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 300,
        _npp, nullptr, GetModuleHandleW(nullptr), this);

    if (_settingsWindow)
    {
        SendMessageW(_settingsWindow, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
        RECT r{};
        GetWindowRect(_npp, &r);
        SetWindowPos(_settingsWindow, nullptr, r.left + 80, r.top + 80, 620, 300, SWP_NOZORDER);
    }
}

void QuickOpen::applyTheme()
{
    if (!_npp || !_window) return;
    _darkMode = SendMessageW(_npp, NPPM_ISDARKMODEENABLED, 0, 0) != FALSE;

    NppDarkMode::Colors colors{};
    if (!SendMessageW(_npp, NPPM_GETDARKMODECOLORS, sizeof(colors), reinterpret_cast<LPARAM>(&colors)))
    {
        colors.background = _darkMode ? RGB(32, 32, 32) : RGB(245, 245, 245);
        colors.pureBackground = _darkMode ? RGB(38, 38, 38) : RGB(255, 255, 255);
        colors.text = _darkMode ? RGB(230, 230, 230) : RGB(30, 30, 30);
        colors.edge = _darkMode ? RGB(90, 90, 90) : RGB(180, 180, 180);
    }

    TreeView_SetBkColor(_tree, colors.pureBackground);
    TreeView_SetTextColor(_tree, colors.text);
    ListView_SetBkColor(_results, colors.pureBackground);
    ListView_SetTextBkColor(_results, colors.pureBackground);
    ListView_SetTextColor(_results, colors.text);

    for (HWND h : {_searchGroup, _workspaceGroup, _search, _tree, _results,
                   _addFolder, _newWorkspace, _saveWorkspace, _removeFolder,
                   _settings, _expandAll, _collapseAll, _status})
    {
        if (h) SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                            static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(h));
    }
    if (_searchPopup)
        SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                     static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(_searchPopup));

    InvalidateRect(_window, nullptr, TRUE);
    if (_searchPopup) InvalidateRect(_searchPopup, nullptr, TRUE);
}

void QuickOpen::onDarkModeChanged()
{
    applyTheme();
    if (_npp && _window) SendMessageW(_npp, NPPM_DMMUPDATEDISPINFO, 0, reinterpret_cast<LPARAM>(_window));
}

std::wstring QuickOpen::getWindowText(HWND h)
{
    if (!h) return {};
    const int length = GetWindowTextLengthW(h);
    std::wstring result(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(h, result.data(), length + 1);
    result.resize(wcslen(result.c_str()));
    return result;
}

LRESULT CALLBACK QuickOpen::windowProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<QuickOpen*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handleMessage(h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT QuickOpen::handleMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(l);
        if (info)
        {
            info->ptMinTrackSize.x = 280;
            info->ptMinTrackSize.y = 260;
            info->ptMaxTrackSize.x = 620;
            info->ptMaxTrackSize.y = 1000;
        }
        return 0;
    }
    case WM_SIZE:
    {
        constexpr int minWidth = 280;
        constexpr int maxWidth = 620;
        constexpr int minHeight = 260;
        constexpr int maxHeight = 1000;
        const int requestedWidth = static_cast<int>(LOWORD(l));
        const int requestedHeight = static_cast<int>(HIWORD(l));
        const int clampedWidth = (std::max)(minWidth, (std::min)(maxWidth, requestedWidth));
        const int clampedHeight = (std::max)(minHeight, (std::min)(maxHeight, requestedHeight));
        if (requestedWidth != clampedWidth || requestedHeight != clampedHeight)
        {
            SetWindowPos(h, nullptr, 0, 0, clampedWidth, clampedHeight,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        layoutControls(clampedWidth, clampedHeight);
        return 0;
    }

    case WM_TIMER:
        if (w == WORKSPACE_SYNC_TIMER)
        {
            syncNativeFolderWorkspace();
            return 0;
        }
        break;

    case WM_COMMAND:
    {
        const int id = LOWORD(w);
        const int code = HIWORD(w);
        if (id == ID_SEARCH && code == EN_CHANGE)
        {
            updateSearch();
            return 0;
        }
        if (id == ID_ADD) { addFolder(); return 0; }
        if (id == ID_NEW) { newWorkspace(); return 0; }
        if (id == ID_SAVE) { saveWorkspace(); return 0; }
        if (id == ID_REMOVE) { removeSelectedRoot(); return 0; }
        if (id == ID_SETTINGS) { openSettings(); return 0; }
        if (id == ID_EXPAND_ALL) { expandAllFolders(); return 0; }
        if (id == ID_COLLAPSE_ALL) { collapseAllFolders(); return 0; }
        break;
    }

    case WM_NOTIFY:
    {
        const auto* hdr = reinterpret_cast<const NMHDR*>(l);
        if (!hdr) break;

        if (hdr->idFrom == ID_TREE)
        {
            const auto* tv = reinterpret_cast<const NMTREEVIEWW*>(l);
            if (hdr->code == NM_DBLCLK) { handleTreeDoubleClick(const_cast<NMTREEVIEWW*>(tv)); return 0; }
            if (hdr->code == TVN_ITEMEXPANDINGW) { handleTreeItemExpanding(const_cast<NMTREEVIEWW*>(tv)); return 0; }
            if (hdr->code == NM_RCLICK)
            {
                POINT pt{};
                GetCursorPos(&pt);
                POINT client = pt;
                ScreenToClient(_tree, &client);
                TVHITTESTINFO hit{};
                hit.pt = client;
                HTREEITEM item = TreeView_HitTest(_tree, &hit);
                if (item && (hit.flags & TVHT_ONITEM))
                {
                    TreeView_SelectItem(_tree, item);
                    showTreeContextMenu(item, pt);
                }
                return 0;
            }
        }
        else if (hdr->idFrom == ID_RESULTS && hdr->code == NM_DBLCLK)
        {
            openSearchResult();
            return 0;
        }
        break;
    }

    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
        {
            if (_searchOnly || GetWindowTextLengthW(_search) > 0) clearSearchResults();
            return 0;
        }
        if (w == VK_RETURN)
        {
            if (_searchOnly) openSearchResult();
            else openTreeSelection();
            return 0;
        }
        break;

    case WM_SETFOCUS:
        if (_searchOnly) SetFocus(_search);
        break;
    }

    return DefWindowProcW(h, msg, w, l);
}

LRESULT CALLBACK QuickOpen::editProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    return g_instance ? g_instance->handleEditMessage(h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT QuickOpen::handleEditMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (h == _searchPopupEdit)
    {
        if (msg == WM_KEYDOWN)
        {
            if (w == VK_ESCAPE) { hideSearchPopup(); return 0; }
            if (w == VK_RETURN) { openPopupSearchResult(); return 0; }
            if (w == VK_DOWN) { SetFocus(_searchPopupResults); return 0; }
        }
        return _oldPopupSearchProc ? CallWindowProcW(_oldPopupSearchProc, h, msg, w, l)
                                    : DefWindowProcW(h, msg, w, l);
    }

    if (msg == WM_KEYDOWN)
    {
        if (w == VK_ESCAPE) { clearSearchResults(); return 0; }
        if (w == VK_RETURN && _searchOnly) { openSearchResult(); return 0; }
        if (w == VK_DOWN && _searchOnly) { SetFocus(_results); return 0; }
    }
    return _oldSearchProc ? CallWindowProcW(_oldSearchProc, h, msg, w, l)
                           : DefWindowProcW(h, msg, w, l);
}

void QuickOpen::addDroppedFolders(HDROP drop)
{
    if (!drop) return;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    bool changed = false;
    for (UINT i = 0; i < count; ++i)
    {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring value(len + 1, L'\0');
        DragQueryFileW(drop, i, value.data(), len + 1);
        value.resize(wcslen(value.c_str()));
        std::filesystem::path path(value);
        std::error_code ec;
        if (!std::filesystem::is_directory(path, ec)) continue;
        if (std::find(_savedRoots.begin(), _savedRoots.end(), path) == _savedRoots.end())
        {
            _savedRoots.push_back(path);
            changed = true;
        }
    }
    DragFinish(drop);
    if (changed)
    {
        saveWorkspace();
        rebuildWorkspaceTree();
        disableNativeFolderWorkspace();
    }
}

LRESULT CALLBACK QuickOpen::nppProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (!g_instance) return DefWindowProcW(h, msg, w, l);
    return g_instance->handleNppMessage(h, msg, w, l);
}

LRESULT QuickOpen::handleNppMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_DROPFILES)
    {
        addDroppedFolders(reinterpret_cast<HDROP>(w));
        return 0;
    }
    return _oldNppProc ? CallWindowProcW(_oldNppProc, h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT CALLBACK QuickOpen::searchPopupProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<QuickOpen*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handleSearchPopupMessage(h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT QuickOpen::handleSearchPopupMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_SIZE:
        layoutSearchPopup();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE)
        {
            hideSearchPopup();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(w) == ID_POPUP_SEARCH && HIWORD(w) == EN_CHANGE)
        {
            updatePopupSearch();
            return 0;
        }
        break;
    case WM_NOTIFY:
    {
        const auto* hdr = reinterpret_cast<const NMHDR*>(l);
        if (hdr && hdr->idFrom == ID_POPUP_RESULTS &&
            (hdr->code == NM_DBLCLK || hdr->code == NM_RETURN || hdr->code == LVN_ITEMACTIVATE))
        {
            openPopupSearchResult();
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
        {
            hideSearchPopup();
            return 0;
        }
        if (w == VK_RETURN)
        {
            openPopupSearchResult();
            return 0;
        }
        break;
    case WM_CLOSE:
        hideSearchPopup();
        return 0;
    case WM_DESTROY:
        _searchPopup = nullptr;
        _searchPopupEdit = nullptr;
        _searchPopupResults = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

LRESULT CALLBACK QuickOpen::settingsProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<QuickOpen*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handleSettingsMessage(h, msg, w, l) : DefWindowProcW(h, msg, w, l);
}

LRESULT QuickOpen::handleSettingsMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_CREATE:
        CreateWindowW(L"STATIC", L"Atalhos padrão do NPPWorkSpace:", WS_CHILD | WS_VISIBLE,
                      20, 20, 300, 24, h, nullptr, GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"STATIC", L"Ctrl+B  —  Mostrar/ocultar o painel NPPWorkSpace\nCtrl+P  —  Abrir a pesquisa do NPPWorkSpace\n\nOs atalhos podem ser alterados em Plugins → NPPWorkSpace → Shortcut Mapper.",
                      WS_CHILD | WS_VISIBLE, 20, 50, 550, 90, h, nullptr, GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"STATIC", L"As pastas adicionadas pelo NPPWorkSpace são salvas automaticamente no workspace do plugin.",
                      WS_CHILD | WS_VISIBLE, 20, 155, 550, 40, h, nullptr, GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Salvar configurações", WS_CHILD | WS_VISIBLE,
                      360, 215, 120, 28, h, reinterpret_cast<HMENU>(ID_SETTINGS_SAVE), GetModuleHandleW(nullptr), nullptr);
        CreateWindowW(L"BUTTON", L"Fechar", WS_CHILD | WS_VISIBLE,
                      490, 215, 80, 28, h, reinterpret_cast<HMENU>(ID_SETTINGS_CLOSE), GetModuleHandleW(nullptr), nullptr);
        return 0;

    case WM_COMMAND:
        if (LOWORD(w) == ID_SETTINGS_SAVE) { saveSettings(); return 0; }
        if (LOWORD(w) == ID_SETTINGS_CLOSE) { DestroyWindow(h); return 0; }
        break;

    case WM_CLOSE:
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        _settingsWindow = nullptr;
        return 0;
    }

    return DefWindowProcW(h, msg, w, l);
}
