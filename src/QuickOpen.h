#pragma once
#include "NotepadPlusMsgs.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// Workspace shortcut accessors implemented by PluginDefinition.cpp.
std::wstring NPPWorkSpace_GetToggleShortcut();
std::wstring NPPWorkSpace_GetSearchShortcut();
void NPPWorkSpace_SetToggleShortcut(const std::wstring& value);
void NPPWorkSpace_SetSearchShortcut(const std::wstring& value);

// Current Notepad++ docking API structure. Kept here so the plugin does not
// need to link against Notepad++.exe or include Notepad++ private headers.
#ifndef DWS_ICONTAB
#define DWS_ICONTAB 0x00000001
#define DWS_ICONBAR 0x00000002
#define DWS_ADDINFO 0x00000004
#define DWS_USEOWNDARKMODE 0x00000008
#define DWS_PARAMSALL (DWS_ICONTAB|DWS_ICONBAR|DWS_ADDINFO)
#define DWS_DF_CONT_LEFT (0 << 28)
#define DWS_DF_CONT_RIGHT (1 << 28)
#define DWS_DF_CONT_TOP (2 << 28)
#define DWS_DF_CONT_BOTTOM (3 << 28)
#define DWS_DF_FLOATING 0x80000000
#endif

#ifndef NPPM_DMMREGASDCKDLG
#define NPPM_DMMSHOW          (NPPMSG + 30)
#define NPPM_DMMHIDE         (NPPMSG + 31)
#define NPPM_DMMUPDATEDISPINFO (NPPMSG + 32)
#define NPPM_DMMREGASDCKDLG  (NPPMSG + 33)
#define NPPM_DMMVIEWOTHERTAB (NPPMSG + 35)
#endif

struct DockedWidgetData
{
    HWND hClient{};
    const wchar_t* pszName{};
    int dlgID{};
    UINT uMask{};
    HICON hIconTab{};
    const wchar_t* pszAddInfo{};
    RECT rcFloat{};
    int iPrevCont{};
    const wchar_t* pszModuleName{};
};

class QuickOpen
{
public:
    static QuickOpen& instance();

    void initialize(HWND nppHandle);
    void destroy();

    void toggleWorkspace();
    void showWorkspace();
    void hideWorkspace();
    void focusSearch();
    bool isVisible() const;

    void refreshWorkspace();
    void onDarkModeChanged();

    // Called from the plugin menu command.
    void setDockCommandId(int id) { _dockCommandId = id; }
    void registerDockPanel();

private:
    QuickOpen() = default;
    ~QuickOpen() = default;
    QuickOpen(const QuickOpen&) = delete;
    QuickOpen& operator=(const QuickOpen&) = delete;

    enum class NodeType { Root, Folder, File };
    struct NodeData
    {
        NodeType type{NodeType::Folder};
        std::filesystem::path path;
        bool fromNppWorkspace{false};
    };

    struct SearchResult
    {
        std::filesystem::path path;
        std::wstring fileName;
        std::wstring folder;
        std::wstring relative;
    };

    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK searchPopupProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK nppProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK dockHostProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK dockSplitterProc(HWND, UINT, WPARAM, LPARAM);

    LRESULT handleMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleEditMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleSearchPopupMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleNppMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleDockHostMessage(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleDockSplitterMessage(HWND, UINT, WPARAM, LPARAM);

    void createWindow();
    void createControls();
    void registerDock();
    void unregisterDock();
    void refreshDockHost();
    void releaseDockHost();
    void refreshDockSplitters();
    void releaseDockSplitters();
    void layoutControls(int width, int height);

    void rebuildWorkspaceTree(bool preserveExpansion = true);
    void populateDirectory(HWND tree, HTREEITEM parent, const std::filesystem::path& directory,
                           bool rootNode, bool fromNppWorkspace, std::unordered_map<HTREEITEM, NodeData>& nodes);
    void addRootToTree(const std::filesystem::path& root, bool fromNppWorkspace);
    void addDirectoryChildren(HWND tree, HTREEITEM parent, const std::filesystem::path& directory,
                              bool fromNppWorkspace);
    void addNode(HWND tree, HTREEITEM parent, const std::wstring& label, const std::filesystem::path& path,
                 NodeType type, bool fromNppWorkspace, bool hasChildren);
    void expandNode(HTREEITEM item);
    void clearTreeData();

    std::vector<std::filesystem::path> getNppWorkspaceRoots() const;
    HWND findFolderWorkspaceTree() const;
    static std::wstring getTreeItemText(HWND tree, HTREEITEM item);
    static std::wstring getWindowText(HWND h);
    static bool isReadableAddress(const void* p, size_t bytes);
    static bool isHiddenSystemDirectory(const std::filesystem::path& p);

    void openTreeSelection();
    void openSelectedTreeFiles();
    void toggleTreeFileSelection(HTREEITEM item, bool ctrlDown);
    bool isTreeFileSelected(HTREEITEM item) const;
    void clearTreeFileSelection();
    void showResultsContextMenu(HWND list, POINT screenPoint);
    void openSelectedResults(HWND list, bool closePopup = false);
    void handleTreeDoubleClick(LPNMTREEVIEWW tv);
    void handleTreeItemExpanding(LPNMTREEVIEWW tv);
    void showTreeContextMenu(HTREEITEM item, POINT screenPoint);
    void expandAllFolders();
    void collapseAllFolders();

    void updateSearch();
    void scheduleSearch(bool popup);
    void showSearchResults(const std::wstring& query);
    void clearSearchResults();
    void openSearchResult();
    void createSearchPopup();
    void showSearchPopup();
    void hideSearchPopup();
    void updatePopupSearch();
    void showPopupSearchResults(const std::wstring& query);
    void openPopupSearchResult();
    void layoutSearchPopup();
    void searchDirectory(const std::filesystem::path& root, const std::wstring& query,
                         std::vector<SearchResult>& results, size_t limit) const;
    bool fileContainsText(const std::filesystem::path& file, const std::wstring& query) const;
    void addDroppedFolders(HDROP drop);
    static int fuzzyScore(const std::wstring& query, const std::wstring& candidate);
    static std::wstring lower(std::wstring value);

    void addFolder();
    void removeSelectedRoot();
    void newWorkspace();
    bool saveWorkspace();
    bool saveWorkspaceAs();
    bool writeWorkspaceFile();
    void openWorkspaceFile();
    bool loadWorkspaceFile(const std::wstring& filePath);
    void loadWorkspace();
    std::wstring getWorkspaceFilePath() const;

    std::vector<std::filesystem::path> getWorkspaceRootsForPanel() const;
    std::wstring getSettingsPath() const;
    void loadSettings();
    void saveSettings() const;

    void applyTheme();
    void createTooltips();
    void addButtonTooltip(HWND button, const wchar_t* text);
    void disableNativeFolderWorkspace();
    void syncNativeFolderWorkspace();

    HWND _npp{};
    WNDPROC _oldNppProc{};
    HWND _window{};
    HWND _title{};
    HWND _search{};
    HWND _tree{};
    HWND _results{};
    HWND _addFolder{};
    HWND _newWorkspace{};
    HWND _saveWorkspace{};
    HWND _openWorkspace{};
    HWND _removeFolder{};
    HWND _expandAll{};
    HWND _collapseAll{};
    HWND _status{};
    HWND _searchGroup{};
    HWND _workspaceGroup{};
    HWND _tooltips{};
    HWND _dockHost{};
    WNDPROC _oldDockHostProc{};

    struct DockSplitterHook { HWND hwnd{}; WNDPROC oldProc{}; };
    std::vector<DockSplitterHook> _dockSplitters;
    HWND _activeSplitter{};
    POINT _lastSplitterCursor{};
    bool _splitterTracking{false};
    HWND _searchPopup{};
    HWND _searchPopupEdit{};
    HWND _searchPopupResults{};

    HFONT _font{};
    HFONT _titleFont{};
    HFONT _symbolFont{};
    WNDPROC _oldSearchProc{};
    WNDPROC _oldPopupSearchProc{};

    std::vector<std::filesystem::path> _savedRoots;
    std::wstring _workspaceFile;
    std::vector<std::filesystem::path> _nppRoots;
    std::vector<SearchResult> _searchResults;
    std::unordered_map<HTREEITEM, NodeData> _nodeData;
    std::unordered_set<HTREEITEM> _selectedTreeFiles;

    bool _darkMode{false};
    bool _searchOnly{false};
    bool _suppressSearch{false};
    unsigned int _searchGeneration{0};
    bool _registeredDock{false};
    int _dockCommandId{0};
    UINT_PTR _syncTimer{0};
};
