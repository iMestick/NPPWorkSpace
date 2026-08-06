#pragma once
#include "NotepadPlusMsgs.h"
#include "RemoteSsh.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <atomic>
#include <memory>
#include <condition_variable>
#include <deque>
#include <mutex>

// Workspace shortcut accessors implemented by PluginDefinition.cpp.
std::wstring NPPWorkSpace_GetToggleShortcut();
std::wstring NPPWorkSpace_GetSearchShortcut();
void NPPWorkSpace_SetToggleShortcut(const std::wstring& value);
void NPPWorkSpace_SetSearchShortcut(const std::wstring& value);
HWND NPPWorkSpace_GetCurrentScintillaHandle();

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
    void onFileSaved(UINT_PTR bufferId);

    // Called from the plugin menu command.
    void setDockCommandId(int id) { _dockCommandId = id; }
    void registerDockPanel();

private:
    QuickOpen() = default;
    ~QuickOpen() = default;
    QuickOpen(const QuickOpen&) = delete;
    QuickOpen& operator=(const QuickOpen&) = delete;

    enum class NodeType { Container, Root, Folder, File };
    struct NodeData
    {
        NodeType type{NodeType::Folder};
        std::filesystem::path path;
        bool fromNppWorkspace{false};
        size_t containerIndex{static_cast<size_t>(-1)};
        std::wstring label;
        bool hasChildren{false};
    };

    struct WorkspaceContainer
    {
        std::wstring name;
        std::vector<std::filesystem::path> folders;
        COLORREF color{CLR_INVALID};
    };

    struct TreeUiState
    {
        std::unordered_set<std::wstring> expandedKeys;
        std::unordered_set<std::wstring> selectedFileKeys;
        std::wstring selectedKey;
        std::wstring firstVisibleKey;
        HWND focusedWindow{};
    };

    struct SearchResult
    {
        enum class RowType { FileName, ContentFile, ContentLine };

        std::filesystem::path path;
        std::wstring fileName;
        std::wstring folder;
        std::wstring relative;
        // Cached lowercase keys keep interactive filename search allocation-free.
        std::wstring fileNameLower;
        std::wstring relativeLower;
        RowType rowType{RowType::FileName};
        size_t contentFileIndex{static_cast<size_t>(-1)};
        size_t contentMatchIndex{static_cast<size_t>(-1)};
        size_t lineNumber{};
        std::wstring snippet;
        size_t matchStart{};
        size_t matchLength{};
        size_t occurrenceOnLine{};
        std::wstring matchText;
    };

    struct ContentMatch
    {
        size_t lineNumber{};
        std::wstring snippet;
        size_t matchStart{};
        size_t matchLength{};
        size_t occurrenceOnLine{};
        std::wstring matchText;
    };

    struct ContentFileGroup
    {
        std::filesystem::path path;
        std::wstring fileName;
        std::wstring folder;
        std::wstring relative;
        std::vector<ContentMatch> matches;
        bool collapsed{false};
    };

    struct ContentSearchBatch
    {
        unsigned int generation{};
        bool popup{};
        std::vector<ContentFileGroup> files;
    };

    struct ContentSearchProgress
    {
        unsigned int generation{};
        bool popup{};
        size_t processed{};
        size_t total{};
        size_t hits{};
        std::wstring currentFile;
    };

    struct ContentSearchComplete
    {
        unsigned int generation{};
        bool popup{};
        bool cancelled{};
        size_t processed{};
        size_t total{};
        size_t hits{};
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
    TreeUiState captureTreeUiState() const;
    void restoreTreeUiState(const TreeUiState& state);
    std::wstring treeNodeKey(const NodeData& data) const;
    std::wstring workspaceRootDisplayName(const std::filesystem::path& root) const;
    void populateDirectory(HWND tree, HTREEITEM parent, const std::filesystem::path& directory,
                           bool rootNode, bool fromNppWorkspace, std::unordered_map<HTREEITEM, NodeData>& nodes);
    void addRootToTree(const std::filesystem::path& root, bool fromNppWorkspace, HTREEITEM parent = TVI_ROOT, size_t containerIndex = static_cast<size_t>(-1));
    void addContainerToTree(size_t containerIndex);
    void addDirectoryChildren(HWND tree, HTREEITEM parent, const std::filesystem::path& directory,
                              bool fromNppWorkspace);
    void addNode(HWND tree, HTREEITEM parent, const std::wstring& label, const std::filesystem::path& path,
                 NodeType type, bool fromNppWorkspace, bool hasChildren, size_t containerIndex = static_cast<size_t>(-1));
    void expandNode(HTREEITEM item);
    std::wstring formatTreeNodeLabel(const NodeData& data, bool expanded) const;
    void refreshTreeNodeLabel(HTREEITEM item, bool expanded);
    void clearTreeData();

    std::vector<std::filesystem::path> getNppWorkspaceRoots() const;
    HWND findFolderWorkspaceTree() const;
    static std::wstring getTreeItemText(HWND tree, HTREEITEM item);
    static std::wstring getWindowText(HWND h);
    static bool isReadableAddress(const void* p, size_t bytes);
    static bool isHiddenSystemDirectory(const std::filesystem::path& p);

    void openTreeSelection();
    void openSelectedTreeFiles();
    bool openFileInNotepad(const std::filesystem::path& file);
    void toggleTreeFileSelection(HTREEITEM item, bool ctrlDown);
    bool isTreeFileSelected(HTREEITEM item) const;
    void clearTreeFileSelection();
    void showResultsContextMenu(HWND list, POINT screenPoint);
    void openSelectedResults(HWND list, bool closePopup = false);
    void handleTreeDoubleClick(LPNMTREEVIEWW tv);
    void handleTreeItemExpanding(LPNMTREEVIEWW tv);
    void showTreeContextMenu(HTREEITEM item, POINT screenPoint);
    void refreshSelectedWorkspaceNode();
    void createContainer(POINT screenPoint = POINT{-1, -1});
    void renameContainer(size_t index);
    void colorContainer(size_t index);
    void removeContainer(size_t index);
    void moveFolderToContainer(const std::filesystem::path& folder, size_t containerIndex);
    void expandAllFolders();
    void collapseAllFolders();
    void expandAllContentGroups();
    void collapseAllContentGroups();
    void showFoldersPanel();
    void showResultsPanel();
    void updatePanelSwitcher();

    void updateSearch();
    void markSearchPending(bool popup);
    void showSearchResults(const std::wstring& query);
    void clearSearchResults();
    void openSearchResult();
    void createSearchPopup();
    void showSearchPopup();
    void hideSearchPopup();
    void updatePopupSearch();
    void showPopupSearchResults(const std::wstring& query);
    void showSearchScopeMenu(HWND owner, POINT screenPoint);
    void applySearchScopeCommand(UINT id);
    bool isSearchPathEnabled(const std::filesystem::path& path) const;
    void invalidateSearchIndex();
    void startSearchIndexBuild();
    void finishSearchIndexBuild(std::vector<SearchResult>* built, unsigned int generation);
    void startContentSearch(const std::wstring& query, bool popup);
    void applyContentSearchBatch(ContentSearchBatch* batch);
    void updateContentSearchProgress(ContentSearchProgress* progress);
    void completeContentSearch(ContentSearchComplete* complete);
    void cancelContentSearch();
    bool isContentSearchEnabled(bool popup) const;
    void setContentSearchEnabled(bool enabled);
    void resetContentSearchResults(bool popup);
    void configureResultsColumns(HWND list, bool contentMode);
    void appendContentSearchGroup(ContentFileGroup&& group, HWND list);
    void rebuildContentResultsList(HWND list);
    void toggleContentGroupFromRow(HWND list, int row);
    void openSearchResultRow(const SearchResult& result);
    void openFileAtOccurrence(const SearchResult& result);
    LRESULT handleResultsCustomDraw(HWND list, NMLVCUSTOMDRAW* customDraw);
    void drawHighlightedSnippet(HWND list, NMLVCUSTOMDRAW* customDraw, const SearchResult& result);
    void openPopupSearchResult();
    void layoutSearchPopup();
    void searchDirectory(const std::filesystem::path& root, const std::wstring& query,
                         std::vector<SearchResult>& results, size_t limit) const;
    void addDroppedFolders(HDROP drop);
    static int fuzzyScore(const std::wstring& query, const std::wstring& candidate);
    static int fuzzyScoreLower(const std::wstring& queryLower, const std::wstring& candidateLower);
    static std::wstring lower(std::wstring value);

    void addFolder();
    void showRemoteSshDialog();
    void removeRemoteFolder(size_t index);
    void queueRemoteConnect(size_t index);
    void queueRemoteDisconnect(const RemoteWorkspaceFolder& config);
    void queueRemoteSync(size_t index);
    void queueRemoteUpload(size_t index, const std::filesystem::path& localFile);
    void startRemoteWorker();
    void stopRemoteWorker();
    void remoteWorkerLoop();
    size_t findRemoteFolderForPath(const std::filesystem::path& path) const;
    bool loadRemoteFoldersFromJson(const std::wstring& json, std::vector<RemoteWorkspaceFolder>& folders);
    std::filesystem::path getRemoteCacheBase() const;
    std::wstring createRemoteId() const;
    void removeSelectedRoot();
    void newWorkspace();
    bool saveWorkspace();
    bool saveWorkspaceAs();
    bool writeWorkspaceFile();
    bool loadContainersFromJson(const std::wstring& json, std::vector<WorkspaceContainer>& containers);
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
    HWND _createContainer{};
    HWND _remoteSsh{};
    HWND _refreshWorkspace{};
    HWND _status{};
    HWND _searchGroup{};
    HWND _contentSearchCheck{};
    HWND _searchProgress{};
    HWND _searchProgressText{};
    HWND _cancelSearch{};
    HWND _runSearch{};
    HWND _searchScopeButton{};
    HWND _workspaceGroup{};
    HWND _viewFolders{};
    HWND _viewSearch{};
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
    HWND _searchPopupScopeButton{};
    HWND _searchPopupRunSearch{};
    HWND _searchPopupContentCheck{};
    HWND _searchPopupHint{};

    HFONT _font{};
    HFONT _titleFont{};
    HFONT _symbolFont{};
    HBRUSH _backgroundBrush{};
    COLORREF _backgroundColor{};
    COLORREF _textColor{};
    COLORREF _edgeColor{};
    WNDPROC _oldSearchProc{};
    WNDPROC _oldPopupSearchProc{};

    std::vector<std::filesystem::path> _savedRoots;
    std::vector<WorkspaceContainer> _containers;
    std::vector<RemoteWorkspaceFolder> _remoteFolders;
    std::wstring _workspaceFile;
    std::vector<std::filesystem::path> _nppRoots;
    std::vector<SearchResult> _searchResults;
    std::vector<ContentFileGroup> _contentSearchGroups;
    std::unordered_map<std::wstring, size_t> _contentSearchGroupByPath;
    std::unordered_map<HTREEITEM, NodeData> _nodeData;
    std::unordered_set<HTREEITEM> _selectedTreeFiles;

    // Search scope: empty included/disabled sets mean all workspace roots are enabled.
    std::unordered_set<std::wstring> _searchIncludedPaths;
    std::unordered_set<std::wstring> _searchDisabledPaths;
    std::unordered_map<UINT, std::filesystem::path> _scopeMenuFolders;
    std::unordered_map<UINT, size_t> _scopeMenuContainers;
    UINT _nextScopeMenuId{50000};

    // File index is built off the UI thread so typing never walks the whole
    // workspace synchronously.
    std::thread _searchIndexThread;
    std::thread _searchQueryThread;
    std::atomic<bool> _searchIndexBuilding{false};
    std::atomic<unsigned int> _searchIndexGeneration{0};
    std::vector<SearchResult> _searchIndex;
    std::shared_ptr<const std::vector<SearchResult>> _searchIndexSnapshot;
    std::atomic<unsigned int> _searchQueryGeneration{0};
    std::atomic<bool> _searchQueryRunning{false};
    std::wstring _pendingContentQuery;
    bool _pendingContentPopup{false};
    std::wstring _runningContentQuery;
    bool _runningContentPopup{false};
    bool _runSearchAfterIndexBuild{false};
    bool _runSearchAfterIndexPopup{false};
    bool _searchIndexValid{false};


    struct RemoteJob
    {
        enum class Type { Connect, DownloadTree, UploadFile, Disconnect };
        Type type{Type::DownloadTree};
        RemoteWorkspaceFolder config;
        std::filesystem::path localFile;
    };

    std::thread _remoteWorker;
    mutable std::mutex _remoteMutex;
    std::condition_variable _remoteCv;
    std::deque<RemoteJob> _remoteJobs;
    bool _remoteStop{false};

    bool _darkMode{false};
    bool _searchOnly{false};
    bool _resultsViewVisible{false};
    bool _suppressSearch{false};
    bool _searchInsideFiles{false};
    bool _contentSearchPopup{false};
    size_t _contentSearchProcessed{};
    size_t _contentSearchTotal{};
    size_t _contentSearchHits{};
    unsigned int _searchGeneration{0};
    bool _registeredDock{false};
    int _dockCommandId{0};
    UINT_PTR _syncTimer{0};
};
