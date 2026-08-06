#include "QuickOpen.h"
#include "NotepadPlusMsgs.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <functional>
#include <array>
#include <iterator>
#include <system_error>
#include <shellapi.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <regex>
#include <sstream>
#include <thread>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
constexpr wchar_t WINDOW_CLASS[] = L"NPPWorkSpacePanel";
constexpr wchar_t SEARCH_POPUP_CLASS[] = L"NPPWorkSpaceSearchPopup";
constexpr wchar_t PANEL_NAME[] = L"NPPWorkSpace";
constexpr wchar_t MODULE_NAME[] = L"NPPWorkSpace.dll";
constexpr wchar_t REGISTRY_KEY[] = L"Software\\NPPWorkSpace";
constexpr wchar_t REGISTRY_WORKSPACE_VALUE[] = L"WorkspaceFile";

constexpr int ID_TITLE = 1000;
constexpr int ID_SEARCH = 1001;
constexpr int ID_TREE = 1002;
constexpr int ID_RESULTS = 1003;
constexpr int ID_ADD = 1004;
constexpr int ID_NEW = 1005;
constexpr int ID_SAVE = 1006;
constexpr int ID_OPEN = 1014;
constexpr int ID_REMOVE = 1007;
constexpr int ID_EXPAND_ALL = 1012;
constexpr int ID_COLLAPSE_ALL = 1013;
constexpr int ID_CREATE_CONTAINER = 1016;
constexpr int ID_OPEN_SELECTED = 1015;
constexpr int ID_STATUS = 1009;
constexpr int ID_SEARCH_GROUP = 1010;
constexpr int ID_SEARCH_SCOPE = 1017;
constexpr int ID_CONTENT_SEARCH = 1018;
constexpr int ID_CANCEL_SEARCH = 1019;
constexpr int ID_SEARCH_PROGRESS = 1020;
constexpr int ID_SEARCH_PROGRESS_TEXT = 1021;
constexpr int ID_RUN_SEARCH = 1022;
constexpr int ID_VIEW_FOLDERS = 1023;
constexpr int ID_VIEW_SEARCH = 1024;
constexpr int ID_CONTAINER_COLOR = 1025;
constexpr int ID_REMOTE_SSH = 1026;
constexpr int ID_REFRESH_WORKSPACE = 1027;
constexpr int ID_WORKSPACE_GROUP = 1011;
constexpr int ID_POPUP_SEARCH = 1200;
constexpr int ID_POPUP_RESULTS = 1201;
constexpr int ID_POPUP_CONTENT_SEARCH = 1202;
constexpr int ID_POPUP_RUN_SEARCH = 1203;
constexpr UINT_PTR WORKSPACE_SYNC_TIMER = 4101;
constexpr UINT WM_SEARCH_INDEX_READY = WM_APP + 0x5A1;
constexpr UINT WM_CONTENT_SEARCH_BATCH = WM_APP + 0x5A2;
constexpr UINT WM_CONTENT_SEARCH_PROGRESS = WM_APP + 0x5A3;
constexpr UINT WM_CONTENT_SEARCH_DONE = WM_APP + 0x5A4;
constexpr UINT WM_REMOTE_JOB_DONE = WM_APP + 0x5A5;
constexpr size_t MAX_CONTENT_FILE_SIZE = 16ull * 1024ull * 1024ull;
constexpr size_t CONTENT_BATCH_FILE_COUNT = 8;
constexpr size_t CONTENT_QUEUE_LIMIT = 512;
constexpr size_t MAX_SNIPPET_CHARS = 220;

constexpr UINT SCI_GOTOPOS = 2025;
constexpr UINT SCI_SETANCHOR = 2026;
constexpr UINT SCI_GETCODEPAGE = 2137;
constexpr UINT SCI_GETLINEENDPOSITION = 2136;
constexpr UINT SCI_POSITIONFROMLINE = 2167;
constexpr UINT SCI_SCROLLCARET = 2169;
constexpr UINT SCI_SETTARGETSTART = 2190;
constexpr UINT SCI_GETTARGETSTART = 2191;
constexpr UINT SCI_SETTARGETEND = 2192;
constexpr UINT SCI_GETTARGETEND = 2193;
constexpr UINT SCI_SEARCHINTARGET = 2197;
constexpr UINT SCI_SETSEARCHFLAGS = 2198;
constexpr UINT SCI_ENSUREVISIBLEENFORCEPOLICY = 2234;
constexpr UINT SCI_SETVISIBLEPOLICY = 2394;
constexpr WPARAM CARET_EVEN = 0x08;
constexpr WPARAM CARET_JUMPS = 0x10;

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

enum class TextEncoding
{
    Unknown,
    Utf8,
    Utf8Bom,
    Utf16LE,
    Utf16BE,
    ShiftJis,
    Big5,
    Ansi
};

struct SnippetInfo
{
    std::wstring text;
    size_t matchStart{};
};

void setButtonChecked(HWND button, bool checked)
{
    if (button) SendMessageW(button, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

bool isButtonChecked(HWND button)
{
    return button && SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

#ifdef _DEBUG
void debugLog(const std::wstring& message)
{
    OutputDebugStringW((L"[NPPWorkSpace] " + message + L"\n").c_str());
}
#define QO_DEBUG_LOG(expr) debugLog((expr))
#else
#define QO_DEBUG_LOG(expr) ((void)0)
#endif

const wchar_t* encodingName(TextEncoding encoding)
{
    switch (encoding)
    {
    case TextEncoding::Utf8: return L"UTF-8";
    case TextEncoding::Utf8Bom: return L"UTF-8 BOM";
    case TextEncoding::Utf16LE: return L"UTF-16 LE";
    case TextEncoding::Utf16BE: return L"UTF-16 BE";
    case TextEncoding::ShiftJis: return L"Shift-JIS";
    case TextEncoding::Big5: return L"Big5";
    case TextEncoding::Ansi: return L"ANSI";
    default: return L"Unknown";
    }
}

std::wstring lowerText(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool isSupportedTextExtension(const std::filesystem::path& path)
{
    std::wstring ext = lowerText(path.extension().wstring());
    static const std::array<const wchar_t*, 17> textExts = {
        L".ini", L".txt", L".json", L".xml", L".lua", L".cfg", L".conf",
        L".csv", L".log", L".hpp", L".h", L".cpp", L".c", L".cs", L".py",
        L".js", L".ts"
    };
    return std::find_if(textExts.begin(), textExts.end(),
        [&](const wchar_t* allowed) { return ext == allowed; }) != textExts.end();
}

bool isWorkspaceTemporaryFile(const std::filesystem::path& path)
{
    const std::wstring name = lowerText(path.filename().wstring());
    constexpr wchar_t suffix[] = L".nppworkspace.download";
    return name.size() >= std::size(suffix) - 1 &&
           name.compare(name.size() - (std::size(suffix) - 1),
                        std::size(suffix) - 1, suffix) == 0;
}


// std::filesystem::recursive_directory_iterator can stop the entire traversal
// after a transient error from the WSL UNC provider (\\wsl.localhost / \\wsl$)
// or from a network-backed workspace. Enumerate each directory independently
// through Win32 so one unreadable entry never hides all remaining files.
std::wstring extendedLengthPath(const std::filesystem::path& path)
{
    std::wstring value = path.lexically_normal().wstring();
    if (value.rfind(L"\\\\?\\", 0) == 0) return value;
    if (value.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + value.substr(2);
    if (path.is_absolute()) return L"\\\\?\\" + value;
    return value;
}

bool workspaceDirectoryExists(const std::filesystem::path& directory)
{
    if (directory.empty()) return false;
    DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        const std::wstring extended = extendedLengthPath(directory);
        attributes = GetFileAttributesW(extended.c_str());
    }
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::filesystem::path normalizeWorkspaceRootPath(const std::filesystem::path& root)
{
    if (root.empty()) return {};

    // Avoid weakly_canonical on UNC/WSL paths. Besides being expensive, some
    // redirectors report ERROR_NOT_SUPPORTED even though FindFirstFile works.
    const std::wstring raw = root.wstring();
    if (raw.rfind(L"\\\\", 0) == 0)
        return root.lexically_normal();

    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(root, ec);
    if (!ec) return normalized;

    ec.clear();
    normalized = std::filesystem::absolute(root, ec);
    if (ec) normalized = root;
    return normalized.lexically_normal();
}

std::filesystem::path workspaceRelativePath(const std::filesystem::path& file,
                                            const std::filesystem::path& root)
{
    const auto normalizedFile = file.lexically_normal();
    const auto normalizedRoot = root.lexically_normal();
    auto relative = normalizedFile.lexically_relative(normalizedRoot);
    if (!relative.empty() && relative.native() != L"." &&
        relative.native().rfind(L"..", 0) != 0)
        return relative;

    std::error_code ec;
    relative = std::filesystem::relative(normalizedFile, normalizedRoot, ec);
    if (!ec && !relative.empty()) return relative;
    return normalizedFile.filename();
}

bool shouldSkipSearchDirectory(const WIN32_FIND_DATAW& data)
{
    const std::wstring name = lowerText(data.cFileName);
    if (name == L"." || name == L"..") return true;
    if (name == L"$recycle.bin" || name == L"system volume information") return true;
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) && name != L".git") return true;

    // Directory junctions/symlinks can point back to an ancestor. Not following
    // them prevents loops while normal WSL folders remain fully searchable.
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    return false;
}

using WorkspaceFileVisitor = std::function<bool(const std::filesystem::path& root,
                                                const std::filesystem::path& file)>;
using WorkspaceCancelCheck = std::function<bool()>;

void enumerateWorkspaceFiles(const std::filesystem::path& requestedRoot,
                             size_t maxFiles,
                             const WorkspaceFileVisitor& visitor,
                             const WorkspaceCancelCheck& cancelled = {})
{
    const std::filesystem::path root = normalizeWorkspaceRootPath(requestedRoot);
    if (!workspaceDirectoryExists(root) || !visitor) return;

    std::vector<std::filesystem::path> pending;
    pending.push_back(root);
    size_t filesVisited = 0;

    while (!pending.empty() && filesVisited < maxFiles)
    {
        if (cancelled && cancelled()) return;

        std::filesystem::path directory = std::move(pending.back());
        pending.pop_back();

        std::filesystem::path wildcard = directory / L"*";
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileExW(wildcard.c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr,
                                       FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE)
        {
            const std::wstring extended = extendedLengthPath(wildcard);
            find = FindFirstFileExW(extended.c_str(), FindExInfoBasic, &data,
                                    FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        }
        if (find == INVALID_HANDLE_VALUE)
        {
            // Some redirectors (including older WSL providers) reject
            // FIND_FIRST_EX_LARGE_FETCH. Retry with the universally supported
            // flags before giving up on this directory only.
            find = FindFirstFileExW(wildcard.c_str(), FindExInfoBasic, &data,
                                    FindExSearchNameMatch, nullptr, 0);
            if (find == INVALID_HANDLE_VALUE)
            {
                const std::wstring extended = extendedLengthPath(wildcard);
                find = FindFirstFileExW(extended.c_str(), FindExInfoBasic, &data,
                                        FindExSearchNameMatch, nullptr, 0);
            }
        }
        if (find == INVALID_HANDLE_VALUE) continue;

        do
        {
            if (cancelled && cancelled())
            {
                FindClose(find);
                return;
            }

            const std::wstring name(data.cFileName);
            if (name == L"." || name == L"..") continue;
            const std::filesystem::path child = directory / name;

            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!shouldSkipSearchDirectory(data)) pending.push_back(child);
                continue;
            }
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) continue;

            ++filesVisited;
            if (!visitor(root, child) || filesVisited >= maxFiles)
            {
                FindClose(find);
                return;
            }
        }
        while (FindNextFileW(find, &data));

        FindClose(find);
    }
}

bool isAsciiString(const std::wstring& value)
{
    return std::all_of(value.begin(), value.end(), [](wchar_t ch) { return ch >= 0 && ch < 0x80; });
}

std::wstring normalizeScopePath(std::wstring value)
{
    value = lowerText(std::move(value));
    std::replace(value.begin(), value.end(), L'/', L'\\');

    // Windows exposes the same WSL share through both aliases. Treat them as
    // one scope so a workspace saved with \\wsl$ still matches paths returned
    // later as \\wsl.localhost (and vice versa).
    constexpr wchar_t wslLegacy[] = L"\\\\wsl$\\";
    constexpr wchar_t wslCurrent[] = L"\\\\wsl.localhost\\";
    if (value.rfind(wslLegacy, 0) == 0)
        value = std::wstring(wslCurrent) + value.substr(std::size(wslLegacy) - 1);

    while (!value.empty() && value.back() == L'\\')
        value.pop_back();
    return value;
}

bool isPathEnabledForScope(const std::filesystem::path& path,
                           const std::unordered_set<std::wstring>& includedPaths,
                           const std::unordered_set<std::wstring>& disabledPaths)
{
    std::wstring value = normalizeScopePath(path.wstring());
    if (!value.empty()) value.push_back(L'\\');

    auto isUnderAny = [&](const std::unordered_set<std::wstring>& roots)
    {
        for (const auto& root : roots)
        {
            std::wstring prefix = normalizeScopePath(root);
            if (!prefix.empty()) prefix.push_back(L'\\');
            if (!prefix.empty() && value.size() >= prefix.size() &&
                value.compare(0, prefix.size(), prefix) == 0)
                return true;
        }
        return false;
    };

    if (!includedPaths.empty() && !isUnderAny(includedPaths)) return false;
    if (!disabledPaths.empty() && isUnderAny(disabledPaths)) return false;
    return true;
}

bool isPathEnabledForScope(const std::filesystem::path& path, const std::unordered_set<std::wstring>& disabledPaths)
{
    static const std::unordered_set<std::wstring> emptyIncludedPaths;
    return isPathEnabledForScope(path, emptyIncludedPaths, disabledPaths);
}

COLORREF defaultContainerColor(size_t index)
{
    static constexpr COLORREF palette[] = {
        RGB(86, 156, 214),
        RGB(78, 201, 176),
        RGB(206, 145, 120),
        RGB(220, 220, 170),
        RGB(197, 134, 192),
        RGB(181, 206, 168),
        RGB(244, 166, 88),
        RGB(114, 159, 207)
    };
    return palette[index % std::size(palette)];
}

std::wstring colorToHex(COLORREF color)
{
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return buffer;
}

int hexValue(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
    if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
    return -1;
}

bool parseHexColor(const std::wstring& value, COLORREF& color)
{
    size_t offset = !value.empty() && value[0] == L'#' ? 1 : 0;
    if (value.size() - offset != 6) return false;

    int channels[3]{};
    for (size_t i = 0; i < 3; ++i)
    {
        const int hi = hexValue(value[offset + i * 2]);
        const int lo = hexValue(value[offset + i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        channels[i] = (hi << 4) | lo;
    }
    color = RGB(channels[0], channels[1], channels[2]);
    return true;
}

unsigned char foldAscii(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? static_cast<unsigned char>(ch + ('a' - 'A')) : ch;
}

bool containsAsciiBytesInsensitive(const std::vector<unsigned char>& data, const std::string& needle)
{
    if (needle.empty() || data.size() < needle.size()) return false;

    std::string foldedNeedle = needle;
    for (char& ch : foldedNeedle)
        ch = static_cast<char>(foldAscii(static_cast<unsigned char>(ch)));

    const size_t m = foldedNeedle.size();
    size_t skip[256];
    for (size_t i = 0; i < std::size(skip); ++i) skip[i] = m;
    for (size_t i = 0; i + 1 < m; ++i)
        skip[foldAscii(static_cast<unsigned char>(foldedNeedle[i]))] = m - 1 - i;

    for (size_t pos = 0; pos + m <= data.size(); )
    {
        size_t j = m;
        while (j > 0 &&
               foldAscii(data[pos + j - 1]) == static_cast<unsigned char>(foldedNeedle[j - 1]))
            --j;
        if (j == 0) return true;
        pos += skip[foldAscii(data[pos + m - 1])];
    }
    return false;
}

bool hasUtf16Pattern(const std::vector<unsigned char>& data)
{
    if (data.size() < 4) return false;
    if ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF))
        return true;

    size_t evenZeros = 0;
    size_t oddZeros = 0;
    const size_t probe = (std::min)(data.size(), size_t(4096));
    for (size_t i = 0; i < probe; ++i)
    {
        if (data[i] == 0)
        {
            if ((i & 1) == 0) ++evenZeros;
            else ++oddZeros;
        }
    }
    return oddZeros > probe / 8 || evenZeros > probe / 8;
}

bool looksBinaryWithoutUtf16(const std::vector<unsigned char>& data)
{
    if (data.empty()) return true;
    if (hasUtf16Pattern(data)) return false;

    size_t zeroCount = 0;
    size_t controlCount = 0;
    const size_t probe = (std::min)(data.size(), size_t(8192));
    for (size_t i = 0; i < probe; ++i)
    {
        const unsigned char ch = data[i];
        if (ch == 0) ++zeroCount;
        else if (ch < 0x20 && ch != '\r' && ch != '\n' && ch != '\t' && ch != '\f')
            ++controlCount;
    }
    return zeroCount > 0 || controlCount > probe / 8;
}

bool decodeWithCodePage(UINT codePage, DWORD flags, const unsigned char* data, size_t size, std::wstring& out)
{
    if (size == 0) { out.clear(); return true; }
    if (size > static_cast<size_t>((std::numeric_limits<int>::max)())) return false;
    const int inputSize = static_cast<int>(size);
    const auto* bytes = reinterpret_cast<LPCCH>(data);
    int needed = MultiByteToWideChar(codePage, flags, bytes, inputSize, nullptr, 0);
    if (needed <= 0 && flags != 0)
        needed = MultiByteToWideChar(codePage, 0, bytes, inputSize, nullptr, 0);
    if (needed <= 0) return false;

    out.assign(static_cast<size_t>(needed), L'\0');
    int written = MultiByteToWideChar(codePage, needed > 0 && flags != 0 ? flags : 0,
                                      bytes, inputSize, out.data(), needed);
    if (written <= 0 && flags != 0)
        written = MultiByteToWideChar(codePage, 0, bytes, inputSize, out.data(), needed);
    if (written <= 0) return false;
    out.resize(static_cast<size_t>(written));
    return true;
}

bool decodeUtf16(const std::vector<unsigned char>& data, bool bigEndian, size_t offset, std::wstring& out)
{
    if (data.size() <= offset) { out.clear(); return true; }
    const size_t units = (data.size() - offset) / 2;
    out.assign(units, L'\0');
    for (size_t i = 0; i < units; ++i)
    {
        const size_t pos = offset + i * 2;
        const uint16_t code = bigEndian
            ? static_cast<uint16_t>((data[pos] << 8) | data[pos + 1])
            : static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
        out[i] = static_cast<wchar_t>(code);
    }
    return true;
}

int scoreDecodedText(const std::wstring& value)
{
    if (value.empty()) return 0;
    int score = 0;
    const size_t probe = (std::min)(value.size(), size_t(8192));
    for (size_t i = 0; i < probe; ++i)
    {
        const wchar_t ch = value[i];
        if (ch == 0 || ch == 0xFFFD)
        {
            score -= 20;
        }
        else if (ch == L'\r' || ch == L'\n' || ch == L'\t')
        {
            score += 1;
        }
        else if (ch < 0x20)
        {
            score -= 8;
        }
        else if (ch >= 0x20 && ch <= 0x7E)
        {
            score += 3;
        }
        else if ((ch >= 0x00C0 && ch <= 0x024F) || (ch >= 0x1E00 && ch <= 0x1EFF))
        {
            score += 4;
        }
        else if ((ch >= 0x3040 && ch <= 0x30FF) || (ch >= 0x4E00 && ch <= 0x9FFF))
        {
            score += 4;
        }
        else
        {
            score += 1;
        }
    }
    return score;
}

void sanitizeDecodedText(std::wstring& value)
{
    for (wchar_t& ch : value)
    {
        if (ch == 0 || ch == 0xFFFD || (ch < 0x20 && ch != L'\r' && ch != L'\n' && ch != L'\t'))
            ch = L' ';
    }
}

bool decodeTextBytes(const std::vector<unsigned char>& data, std::wstring& text, TextEncoding& encoding)
{
    text.clear();
    encoding = TextEncoding::Unknown;
    if (data.empty() || looksBinaryWithoutUtf16(data)) return false;

    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
    {
        if (decodeWithCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, data.data() + 3, data.size() - 3, text))
        {
            encoding = TextEncoding::Utf8Bom;
            sanitizeDecodedText(text);
            return true;
        }
    }
    if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE)
    {
        decodeUtf16(data, false, 2, text);
        encoding = TextEncoding::Utf16LE;
        sanitizeDecodedText(text);
        return true;
    }
    if (data.size() >= 2 && data[0] == 0xFE && data[1] == 0xFF)
    {
        decodeUtf16(data, true, 2, text);
        encoding = TextEncoding::Utf16BE;
        sanitizeDecodedText(text);
        return true;
    }

    if (hasUtf16Pattern(data))
    {
        size_t evenZeros = 0;
        size_t oddZeros = 0;
        const size_t probe = (std::min)(data.size(), size_t(4096));
        for (size_t i = 0; i < probe; ++i)
        {
            if (data[i] == 0)
            {
                if ((i & 1) == 0) ++evenZeros;
                else ++oddZeros;
            }
        }
        const bool bigEndian = evenZeros > oddZeros;
        decodeUtf16(data, bigEndian, 0, text);
        encoding = bigEndian ? TextEncoding::Utf16BE : TextEncoding::Utf16LE;
        sanitizeDecodedText(text);
        return scoreDecodedText(text) > 0;
    }

    std::wstring utf8;
    if (decodeWithCodePage(CP_UTF8, MB_ERR_INVALID_CHARS, data.data(), data.size(), utf8) &&
        scoreDecodedText(utf8) > 0)
    {
        text = std::move(utf8);
        encoding = TextEncoding::Utf8;
        sanitizeDecodedText(text);
        return true;
    }

    struct Candidate
    {
        UINT codePage;
        TextEncoding encoding;
        DWORD flags;
    };
    static const Candidate candidates[] = {
        {CP_ACP, TextEncoding::Ansi, 0},
        {932, TextEncoding::ShiftJis, MB_ERR_INVALID_CHARS},
        {950, TextEncoding::Big5, MB_ERR_INVALID_CHARS}
    };

    int bestScore = (std::numeric_limits<int>::min)();
    std::wstring bestText;
    TextEncoding bestEncoding = TextEncoding::Unknown;
    for (const auto& candidate : candidates)
    {
        std::wstring decoded;
        if (!decodeWithCodePage(candidate.codePage, candidate.flags, data.data(), data.size(), decoded))
            continue;
        const int score = scoreDecodedText(decoded);
        if (score > bestScore)
        {
            bestScore = score;
            bestText = std::move(decoded);
            bestEncoding = candidate.encoding;
        }
    }

    if (bestEncoding == TextEncoding::Unknown || bestScore <= 0) return false;
    text = std::move(bestText);
    encoding = bestEncoding;
    sanitizeDecodedText(text);
    return true;
}

SnippetInfo makeSnippet(const std::wstring& line, size_t matchStart, size_t matchLength)
{
    const size_t half = MAX_SNIPPET_CHARS / 2;
    size_t start = 0;
    if (matchStart > half) start = matchStart - half;

    size_t end = (std::min)(line.size(), start + MAX_SNIPPET_CHARS);
    if (end < matchStart + matchLength)
    {
        end = (std::min)(line.size(), matchStart + matchLength + half);
        start = end > MAX_SNIPPET_CHARS ? end - MAX_SNIPPET_CHARS : 0;
    }

    std::wstring snippet = line.substr(start, end - start);
    for (wchar_t& ch : snippet)
    {
        if (ch == L'\r' || ch == L'\n') ch = L' ';
        else if (ch == L'\t') ch = L' ';
    }

    size_t adjusted = matchStart >= start ? matchStart - start : 0;
    if (start > 0)
    {
        snippet.insert(0, L"... ");
        adjusted += 4;
    }
    if (end < line.size())
        snippet += L" ...";

    return {std::move(snippet), adjusted};
}

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

std::wstring jsonEscape(const std::wstring& value)
{
    std::wstring out;
    out.reserve(value.size() + 16);
    for (wchar_t ch : value)
    {
        switch (ch)
        {
        case L'\\': out += L"\\\\"; break;
        case L'"': out += L"\\\""; break;
        case L'\r': out += L"\\r"; break;
        case L'\n': out += L"\\n"; break;
        case L'\t': out += L"\\t"; break;
        default: out += ch; break;
        }
    }
    return out;
}

std::wstring jsonUnescape(const std::wstring& value)
{
    std::wstring out;
    out.reserve(value.size());
    bool escaped = false;
    for (wchar_t ch : value)
    {
        if (!escaped)
        {
            if (ch == L'\\') escaped = true;
            else out += ch;
            continue;
        }
        switch (ch)
        {
        case L'\\': out += L'\\'; break;
        case L'"': out += L'"'; break;
        case L'r': out += L'\r'; break;
        case L'n': out += L'\n'; break;
        case L't': out += L'\t'; break;
        default: out += ch; break;
        }
        escaped = false;
    }
    if (escaped) out += L'\\';
    return out;
}

bool extractJsonString(const std::wstring& json, const std::wstring& key, std::wstring& value)
{
    const std::wstring needle = L"\"" + key + L"\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::wstring::npos) return false;
    size_t pos = json.find(L':', keyPos + needle.size());
    if (pos == std::wstring::npos) return false;
    ++pos;
    while (pos < json.size() && iswspace(json[pos])) ++pos;
    if (pos >= json.size() || json[pos] != L'"') return false;
    ++pos;
    std::wstring raw;
    bool escaped = false;
    while (pos < json.size())
    {
        const wchar_t ch = json[pos++];
        if (!escaped && ch == L'"')
        {
            value = jsonUnescape(raw);
            return true;
        }
        if (!escaped && ch == L'\\')
        {
            raw += ch;
            escaped = true;
        }
        else
        {
            raw += ch;
            escaped = false;
        }
    }
    return false;
}

bool extractJsonStringArray(const std::wstring& json, const std::wstring& key, std::vector<std::wstring>& values)
{
    const std::wstring needle = L"\"" + key + L"\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::wstring::npos) return false;
    const size_t open = json.find(L'[', keyPos + needle.size());
    if (open == std::wstring::npos) return false;
    const size_t close = json.find(L']', open + 1);
    if (close == std::wstring::npos) return false;

    size_t pos = open + 1;
    while (pos < close)
    {
        while (pos < close && (iswspace(json[pos]) || json[pos] == L',')) ++pos;
        if (pos >= close) break;
        if (json[pos] != L'"') return false;
        ++pos;
        std::wstring value;
        bool escaped = false;
        while (pos < close)
        {
            const wchar_t ch = json[pos++];
            if (!escaped && ch == L'"') break;
            if (!escaped && ch == L'\\') { escaped = true; value += ch; continue; }
            value += ch;
            escaped = false;
        }
        values.push_back(jsonUnescape(value));
    }
    return true;
}

bool readWorkspacePathFromRegistry(std::wstring& value)
{
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(key, REGISTRY_WORKSPACE_VALUE, nullptr, &type, nullptr, &bytes);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
    {
        RegCloseKey(key);
        return false;
    }

    std::wstring buffer(bytes / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(key, REGISTRY_WORKSPACE_VALUE, nullptr, &type,
                              reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return false;

    buffer.resize(wcsnlen_s(buffer.c_str(), buffer.size()));
    value = std::move(buffer);
    return !value.empty();
}

void writeWorkspacePathToRegistry(const std::wstring& value)
{
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REGISTRY_KEY, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;

    RegSetValueExW(key, REGISTRY_WORKSPACE_VALUE, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
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


struct TextPromptState
{
    std::wstring value;
    HWND npp{};
    HFONT font{};
    HFONT titleFont{};
    HBRUSH backgroundBrush{};
    COLORREF background{};
    COLORREF text{};
    COLORREF edge{};
    bool accepted{false};
    POINT screenPoint{-1, -1};
};

INT_PTR CALLBACK textPromptProc(HWND dlg, UINT msg, WPARAM w, LPARAM l)
{
    auto* state = reinterpret_cast<TextPromptState*>(GetWindowLongPtrW(dlg, GWLP_USERDATA));
    if (msg == WM_INITDIALOG)
    {
        state = reinterpret_cast<TextPromptState*>(l);
        SetWindowLongPtrW(dlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        NppDarkMode::Colors colors{};
        const bool dark = state->npp && SendMessageW(state->npp, NPPM_ISDARKMODEENABLED, 0, 0) != FALSE;
        bool themed = dark && state->npp &&
            SendMessageW(state->npp, NPPM_GETDARKMODECOLORS, sizeof(colors), reinterpret_cast<LPARAM>(&colors)) != FALSE;
        if (!themed)
        {
            colors.background = GetSysColor(COLOR_BTNFACE);
            colors.pureBackground = GetSysColor(COLOR_WINDOW);
            colors.text = GetSysColor(COLOR_WINDOWTEXT);
            colors.edge = GetSysColor(COLOR_BTNSHADOW);
        }
        state->background = colors.background;
        state->text = colors.text;
        state->edge = colors.edge;
        state->backgroundBrush = CreateSolidBrush(colors.background);

        // Let Notepad++ apply the exact same dark/light control theme used by
        // its own dialogs, while still keeping this dialog independent.
        if (state->npp)
        {
            for (HWND child : {dlg, GetDlgItem(dlg, 1000), GetDlgItem(dlg, 1001),
                               GetDlgItem(dlg, IDOK), GetDlgItem(dlg, IDCANCEL)})
            {
                if (child)
                    SendMessageW(state->npp, NPPM_DARKMODESUBCLASSANDTHEME,
                                 static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(child));
            }
        }

        if (state->font)
        {
            for (HWND child : {GetDlgItem(dlg, 1000), GetDlgItem(dlg, 1001),
                               GetDlgItem(dlg, IDOK), GetDlgItem(dlg, IDCANCEL)})
            {
                if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
            }
        }
        if (state->titleFont)
            SendMessageW(GetDlgItem(dlg, 1000), WM_SETFONT, reinterpret_cast<WPARAM>(state->titleFont), TRUE);

        SetDlgItemTextW(dlg, 1001, state->value.c_str());

        // Open the compact prompt close to the point that triggered it.
        if (state->screenPoint.x >= 0 && state->screenPoint.y >= 0)
        {
            RECT rc{};
            GetWindowRect(dlg, &rc);
            const int promptWidth = rc.right - rc.left;
            const int promptHeight = rc.bottom - rc.top;
            HMONITOR monitor = MonitorFromPoint(state->screenPoint, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(monitor, &mi);
            int x = state->screenPoint.x + 10;
            int y = state->screenPoint.y + 10;
            if (x + promptWidth > mi.rcWork.right) x = state->screenPoint.x - promptWidth - 10;
            if (y + promptHeight > mi.rcWork.bottom) y = state->screenPoint.y - promptHeight - 10;
            x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - promptWidth));
            y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - promptHeight));
            SetWindowPos(dlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        SetFocus(GetDlgItem(dlg, 1001));
        SendDlgItemMessageW(dlg, 1001, EM_SETSEL, 0, -1);
        return FALSE;
    }
    if (!state) return FALSE;

    if (msg == WM_CTLCOLORDLG || msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLOREDIT)
    {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, state->text);
        SetBkColor(dc, state->background);
        if (msg == WM_CTLCOLORSTATIC)
            SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<INT_PTR>(state->backgroundBrush);
    }

    if (msg == WM_COMMAND)
    {
        if (LOWORD(w) == IDOK)
        {
            wchar_t buffer[512]{};
            GetDlgItemTextW(dlg, 1001, buffer, static_cast<int>(std::size(buffer)));
            state->value = buffer;
            state->accepted = !state->value.empty();
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL)
        {
            state->accepted = false;
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
    }
    if (msg == WM_CLOSE)
    {
        state->accepted = false;
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    if (msg == WM_DESTROY)
    {
        if (state->backgroundBrush)
        {
            DeleteObject(state->backgroundBrush);
            state->backgroundBrush = nullptr;
        }
    }
    return FALSE;
}

bool promptForText(HWND owner, HWND npp, HFONT font, HFONT titleFont,
                   const wchar_t* title, const wchar_t* prompt, std::wstring& value,
                   POINT screenPoint)
{
    struct DialogBuffer
    {
        DLGTEMPLATE dlg{};
    };

    // Compact prompt sized like a small native Notepad++ utility dialog.
    std::vector<BYTE> data(2048, 0);
    auto* dlg = reinterpret_cast<DLGTEMPLATE*>(data.data());
    dlg->style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    dlg->dwExtendedStyle = 0;
    dlg->cdit = 4;
    dlg->x = 0; dlg->y = 0; dlg->cx = 250; dlg->cy = 88;

    WORD* p = reinterpret_cast<WORD*>(dlg + 1);
    *p++ = 0; *p++ = 0;
    const wchar_t* t = title;
    while ((*p++ = static_cast<WORD>(*t++)) != 0) {}

    // The actual font is applied to every control at WM_INITDIALOG. The
    // template still needs a valid face name for Win32 dialog creation.
    *p++ = 9;
    const wchar_t* fontName = L"Segoe UI";
    while ((*p++ = static_cast<WORD>(*fontName++)) != 0) {}

    auto alignDword = [&p]()
    {
        p = reinterpret_cast<WORD*>((reinterpret_cast<ULONG_PTR>(p) + 3) & ~static_cast<ULONG_PTR>(3));
    };
    auto addControl = [&](DWORD style, short x, short y, short cx, short cy, WORD id,
                          LPCWSTR cls, LPCWSTR text)
    {
        alignDword();
        auto* item = reinterpret_cast<DLGITEMTEMPLATE*>(p);
        item->style = style;
        item->dwExtendedStyle = 0;
        item->x = x; item->y = y; item->cx = cx; item->cy = cy; item->id = id;
        p = reinterpret_cast<WORD*>(item + 1);
        if (cls[0] == L'\0')
        {
            *p++ = 0xFFFF; *p++ = 0x0082;
        }
        else
        {
            while ((*p++ = static_cast<WORD>(*cls++)) != 0) {}
        }
        if (text)
        {
            while ((*p++ = static_cast<WORD>(*text++)) != 0) {}
        }
        else *p++ = 0;
        *p++ = 0;
    };

    addControl(WS_CHILD | WS_VISIBLE, 12, 10, 226, 14, 1000, L"STATIC", prompt);
    addControl(WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
               12, 28, 226, 18, 1001, L"EDIT", value.c_str());
    addControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
               132, 58, 50, 20, IDOK, L"BUTTON", L"OK");
    addControl(WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
               188, 58, 50, 20, IDCANCEL, L"BUTTON", L"Cancelar");

    TextPromptState state{value, npp, font, titleFont, nullptr, 0, 0, 0, false, screenPoint};
    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dlg, owner,
                                                   textPromptProc, reinterpret_cast<LPARAM>(&state));
    if (result == IDOK && state.accepted)
    {
        value = state.value;
        return true;
    }
    return false;
}


constexpr wchar_t REMOTE_DIALOG_CLASS[] = L"NPPWorkSpaceRemoteSshDialog";
constexpr int RID_CONNECTION = 3001;
constexpr int RID_NAME = 3002;
constexpr int RID_HOST = 3003;
constexpr int RID_PORT = 3004;
constexpr int RID_USER = 3005;
constexpr int RID_AUTH = 3006;
constexpr int RID_PASSWORD = 3007;
constexpr int RID_KEY = 3008;
constexpr int RID_KEY_BROWSE = 3009;
constexpr int RID_PASSPHRASE = 3010;
constexpr int RID_REMOTE_PATH = 3011;
constexpr int RID_TEST = 3012;
constexpr int RID_SAVE_SYNC = 3013;
constexpr int RID_REMOVE_REMOTE = 3014;
constexpr int RID_CANCEL_REMOTE = 3015;
constexpr int RID_SECURITY_NOTE = 3016;
constexpr int RID_REMOTE_TREE = 3017;
constexpr int RID_REMOTE_STATUS = 3018;
constexpr int RID_TARGET_CONTAINER = 3019;

bool sameRemoteConnectionSettings(const RemoteWorkspaceFolder& left,
                                  const RemoteWorkspaceFolder& right)
{
    return _wcsicmp(left.host.c_str(), right.host.c_str()) == 0 &&
           left.port == right.port &&
           _wcsicmp(left.username.c_str(), right.username.c_str()) == 0 &&
           left.authMode == right.authMode &&
           left.password == right.password &&
           left.privateKeyPath == right.privateKeyPath &&
           left.privateKeyPassphrase == right.privateKeyPassphrase &&
           left.hostKeySha256 == right.hostKeySha256;
}

struct RemoteDialogState
{
    enum class Action { Cancel, SaveAndSync, Remove };

    HWND owner{};
    HWND npp{};
    HFONT font{};
    // One representative per SSH connection. Several workspace roots may share
    // the same connectionId and therefore the same persistent worker session.
    std::vector<RemoteWorkspaceFolder> entries;
    std::vector<std::wstring> containerNames;
    int selectedIndex{-1};
    int targetContainerIndex{-1};
    RemoteWorkspaceFolder value;
    RemoteWorkspaceFolder connectedConfig;
    Action action{Action::Cancel};

    HWND connection{};
    HWND name{};
    HWND host{};
    HWND port{};
    HWND user{};
    HWND auth{};
    HWND password{};
    HWND key{};
    HWND passphrase{};
    HWND remotePath{};
    HWND targetContainer{};
    HWND browserTree{};
    HWND browserStatus{};
    HWND connectButton{};
    HWND removeButton{};

    std::unique_ptr<RemoteSsh::Client> browserClient;
    std::unordered_map<HTREEITEM, std::wstring> browserPaths;
    std::unordered_set<HTREEITEM> loadedBrowserNodes;
    bool connected{false};
};

std::wstring getControlText(HWND control)
{
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>((std::max)(0, length)) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>((std::max)(0, length)));
    return value;
}

void setRemoteDialogFont(RemoteDialogState* state, HWND control)
{
    if (state && state->font && control)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
}

HWND addRemoteControl(RemoteDialogState* state, HWND parent, DWORD exStyle,
                      const wchar_t* className, const wchar_t* text, DWORD style,
                      int x, int y, int width, int height, int id)
{
    HWND control = CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    setRemoteDialogFont(state, control);
    if (state && state->npp && control)
        SendMessageW(state->npp, NPPM_DARKMODESUBCLASSANDTHEME,
                     static_cast<WPARAM>(NppDarkMode::dmfHandleChange),
                     reinterpret_cast<LPARAM>(control));
    return control;
}

void updateRemoteAuthControls(RemoteDialogState* state)
{
    if (!state) return;
    const int auth = static_cast<int>(SendMessageW(state->auth, CB_GETCURSEL, 0, 0));
    const bool password = auth != 1;
    EnableWindow(state->password, password);
    EnableWindow(state->key, !password);
    EnableWindow(GetDlgItem(GetParent(state->key), RID_KEY_BROWSE), !password);
    EnableWindow(state->passphrase, !password);
}

void clearRemoteBrowser(RemoteDialogState* state)
{
    if (!state) return;
    if (state->browserTree) TreeView_DeleteAllItems(state->browserTree);
    state->browserPaths.clear();
    state->loadedBrowserNodes.clear();
    state->connected = false;
    state->browserClient.reset();
    if (state->browserStatus)
        SetWindowTextW(state->browserStatus, L"Desconectado. Clique em Conectar para listar /root.");
}

void fillRemoteDialog(RemoteDialogState* state, int selected)
{
    if (!state) return;
    clearRemoteBrowser(state);
    state->selectedIndex = selected;
    RemoteWorkspaceFolder item;
    if (selected >= 0 && selected < static_cast<int>(state->entries.size()))
        item = state->entries[static_cast<size_t>(selected)];
    else
    {
        item.name = L"Ubuntu Server";
        item.port = 22;
        item.username = L"root";
        item.remotePath = L"/root";
    }
    state->value = item;

    SetWindowTextW(state->name, item.name.c_str());
    SetWindowTextW(state->host, item.host.c_str());
    SetWindowTextW(state->port, std::to_wstring(item.port).c_str());
    SetWindowTextW(state->user, item.username.c_str());
    SendMessageW(state->auth, CB_SETCURSEL,
                 item.authMode == RemoteWorkspaceFolder::AuthMode::PrivateKey ? 1 : 0, 0);
    SetWindowTextW(state->password, item.password.c_str());
    SetWindowTextW(state->key, item.privateKeyPath.c_str());
    SetWindowTextW(state->passphrase, item.privateKeyPassphrase.c_str());
    SetWindowTextW(state->remotePath, L"/root");
    EnableWindow(state->removeButton, selected >= 0);
    updateRemoteAuthControls(state);
}

bool readRemoteDialog(RemoteDialogState* state, RemoteWorkspaceFolder& item, std::wstring& error)
{
    if (!state) return false;
    item = state->value;

    item.name = getControlText(state->name);
    item.host = getControlText(state->host);
    item.username = getControlText(state->user);
    item.password = getControlText(state->password);
    item.privateKeyPath = getControlText(state->key);
    item.privateKeyPassphrase = getControlText(state->passphrase);
    item.remotePath = getControlText(state->remotePath);
    item.authMode = SendMessageW(state->auth, CB_GETCURSEL, 0, 0) == 1
        ? RemoteWorkspaceFolder::AuthMode::PrivateKey
        : RemoteWorkspaceFolder::AuthMode::Password;
    if (item.authMode == RemoteWorkspaceFolder::AuthMode::PrivateKey)
        item.password.clear();
    else
        item.privateKeyPassphrase.clear();

    const std::wstring portText = getControlText(state->port);
    wchar_t* end = nullptr;
    const unsigned long port = wcstoul(portText.c_str(), &end, 10);
    if (portText.empty() || !end || *end != L'\0' || port == 0 || port > 65535)
    {
        error = L"Informe uma porta SSH válida entre 1 e 65535.";
        return false;
    }
    item.port = static_cast<unsigned short>(port);

    if (item.name.empty()) item.name = item.host;
    if (item.host.empty()) { error = L"Informe o endereço/IP do servidor."; return false; }
    if (item.username.empty()) { error = L"Informe o usuário SSH."; return false; }
    if (item.remotePath.empty() || item.remotePath.front() != L'/')
    {
        error = L"Selecione uma pasta absoluta do servidor, iniciando por /root.";
        return false;
    }
    if (item.authMode == RemoteWorkspaceFolder::AuthMode::PrivateKey && item.privateKeyPath.empty())
    {
        error = L"Selecione o arquivo da chave privada.";
        return false;
    }
    if (item.authMode == RemoteWorkspaceFolder::AuthMode::Password && item.password.empty())
    {
        error = L"Informe a senha SSH.";
        return false;
    }
    return true;
}

HTREEITEM insertRemoteBrowserNode(RemoteDialogState* state, HTREEITEM parent,
                                  const std::wstring& label, const std::wstring& path,
                                  bool expandable)
{
    if (!state || !state->browserTree) return nullptr;
    TVINSERTSTRUCTW insert{};
    insert.hParent = parent;
    insert.hInsertAfter = TVI_SORT;
    insert.item.mask = TVIF_TEXT | TVIF_CHILDREN;
    insert.item.pszText = const_cast<LPWSTR>(label.c_str());
    insert.item.cChildren = expandable ? 1 : 0;
    const HTREEITEM item = TreeView_InsertItem(state->browserTree, &insert);
    if (item)
    {
        state->browserPaths[item] = path;
        if (expandable)
        {
            TVINSERTSTRUCTW dummy{};
            dummy.hParent = item;
            dummy.hInsertAfter = TVI_LAST;
            dummy.item.mask = TVIF_TEXT;
            dummy.item.pszText = const_cast<LPWSTR>(L"Carregando...");
            TreeView_InsertItem(state->browserTree, &dummy);
        }
    }
    return item;
}

bool loadRemoteBrowserChildren(RemoteDialogState* state, HTREEITEM parent, bool showErrors)
{
    if (!state || !state->browserClient || !state->browserClient->isConnected()) return false;
    if (state->loadedBrowserNodes.find(parent) != state->loadedBrowserNodes.end()) return true;
    const auto pathIt = state->browserPaths.find(parent);
    if (pathIt == state->browserPaths.end()) return false;

    if (state->browserStatus)
        SetWindowTextW(state->browserStatus, (L"Listando " + pathIt->second + L"...").c_str());

    std::vector<RemoteDirectoryEntry> entries;
    const RemoteSshResult result = state->browserClient->listDirectory(pathIt->second, entries);
    if (!result.success)
    {
        if (state->browserStatus) SetWindowTextW(state->browserStatus, L"Falha ao listar a pasta remota.");
        if (showErrors)
            MessageBoxW(GetParent(state->browserTree), result.error.c_str(),
                        L"Navegação SSH", MB_OK | MB_ICONERROR);
        return false;
    }

    HTREEITEM child = TreeView_GetChild(state->browserTree, parent);
    while (child)
    {
        HTREEITEM next = TreeView_GetNextSibling(state->browserTree, child);
        state->browserPaths.erase(child);
        state->loadedBrowserNodes.erase(child);
        TreeView_DeleteItem(state->browserTree, child);
        child = next;
    }

    size_t folders = 0;
    for (const auto& entry : entries)
    {
        if (!entry.isDirectory) continue;
        insertRemoteBrowserNode(state, parent, entry.name, entry.fullPath, true);
        ++folders;
    }
    state->loadedBrowserNodes.insert(parent);
    TVITEMW item{};
    item.mask = TVIF_HANDLE | TVIF_CHILDREN;
    item.hItem = parent;
    item.cChildren = folders > 0 ? 1 : 0;
    TreeView_SetItem(state->browserTree, &item);

    if (state->browserStatus)
    {
        const std::wstring text = L"Conectado  •  " + std::to_wstring(folders) +
                                  L" pasta(s) em " + pathIt->second;
        SetWindowTextW(state->browserStatus, text.c_str());
    }
    return true;
}

bool connectRemoteBrowser(RemoteDialogState* state, HWND hwnd)
{
    RemoteWorkspaceFolder item;
    std::wstring error;
    SetWindowTextW(state->remotePath, L"/root");
    if (!readRemoteDialog(state, item, error))
    {
        MessageBoxW(hwnd, error.c_str(), L"Conexão SSH", MB_OK | MB_ICONWARNING);
        return false;
    }

    if (state->selectedIndex >= 0 && state->selectedIndex < static_cast<int>(state->entries.size()))
    {
        const auto& previous = state->entries[static_cast<size_t>(state->selectedIndex)];
        if (_wcsicmp(previous.host.c_str(), item.host.c_str()) != 0 || previous.port != item.port)
            item.hostKeySha256.clear();
    }

    if (state->browserStatus) SetWindowTextW(state->browserStatus, L"Conectando ao servidor...");
    auto client = std::make_unique<RemoteSsh::Client>();
    RemoteSshResult result = client->connect(item);
    if (!result.success)
    {
        if (state->browserStatus) SetWindowTextW(state->browserStatus, L"Falha na conexão SSH.");
        MessageBoxW(hwnd, result.error.c_str(), L"Falha na conexão SSH", MB_OK | MB_ICONERROR);
        return false;
    }

    if (item.hostKeySha256.empty())
    {
        const std::wstring question = L"A chave SHA-256 apresentada pelo servidor é:\n\n" +
                                      result.hostKeySha256 +
                                      L"\n\nConfia neste servidor e deseja fixar esta chave no workspace?";
        if (MessageBoxW(hwnd, question.c_str(), L"Confiar no servidor SSH",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        {
            client->disconnect();
            if (state->browserStatus) SetWindowTextW(state->browserStatus, L"Conexão não autorizada.");
            return false;
        }
        item.hostKeySha256 = result.hostKeySha256;
    }

    state->browserClient = std::move(client);
    state->connected = true;
    state->connectedConfig = item;
    state->value = item;
    TreeView_DeleteAllItems(state->browserTree);
    state->browserPaths.clear();
    state->loadedBrowserNodes.clear();

    const HTREEITEM root = insertRemoteBrowserNode(state, TVI_ROOT, L"/root", L"/root", true);
    if (!root || !loadRemoteBrowserChildren(state, root, true))
    {
        clearRemoteBrowser(state);
        return false;
    }
    TreeView_Expand(state->browserTree, root, TVE_EXPAND);
    TreeView_SelectItem(state->browserTree, root);
    SetWindowTextW(state->remotePath, L"/root");
    SetWindowTextW(state->connectButton, L"Reconectar");
    return true;
}

LRESULT CALLBACK remoteDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    auto* state = reinterpret_cast<RemoteDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE)
    {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<RemoteDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg)
    {
    case WM_CREATE:
    {
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Conexão", SS_LEFT | SS_CENTERIMAGE,
                         18, 16, 90, 22, 0);
        state->connection = addRemoteControl(state, hwnd, 0, L"COMBOBOX", L"",
            WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 116, 16, 402, 240, RID_CONNECTION);
        SendMessageW(state->connection, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Nova conexão SSH"));
        for (const auto& entry : state->entries)
        {
            const std::wstring label = entry.name + L"  —  " + entry.username + L"@" + entry.host +
                                       L":" + std::to_wstring(entry.port);
            SendMessageW(state->connection, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }

        addRemoteControl(state, hwnd, 0, L"BUTTON", L"Servidor SSH", BS_GROUPBOX,
                         10, 50, 516, 206, 0);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Nome", SS_LEFT | SS_CENTERIMAGE,
                         24, 72, 82, 22, 0);
        state->name = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_TABSTOP | ES_AUTOHSCROLL, 116, 72, 392, 24, RID_NAME);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Host/IP", SS_LEFT | SS_CENTERIMAGE,
                         24, 102, 82, 22, 0);
        state->host = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_TABSTOP | ES_AUTOHSCROLL, 116, 102, 266, 24, RID_HOST);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Porta", SS_LEFT | SS_CENTERIMAGE,
                         390, 102, 44, 22, 0);
        state->port = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"22",
                         WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 440, 102, 68, 24, RID_PORT);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Usuário", SS_LEFT | SS_CENTERIMAGE,
                         24, 132, 82, 22, 0);
        state->user = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"root",
                         WS_TABSTOP | ES_AUTOHSCROLL, 116, 132, 392, 24, RID_USER);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Autenticação", SS_LEFT | SS_CENTERIMAGE,
                         24, 162, 82, 22, 0);
        state->auth = addRemoteControl(state, hwnd, 0, L"COMBOBOX", L"",
                         WS_TABSTOP | CBS_DROPDOWNLIST, 116, 162, 160, 140, RID_AUTH);
        SendMessageW(state->auth, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Senha"));
        SendMessageW(state->auth, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Chave privada"));
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Senha", SS_LEFT | SS_CENTERIMAGE,
                         286, 162, 52, 22, 0);
        state->password = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 342, 162, 166, 24, RID_PASSWORD);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Chave", SS_LEFT | SS_CENTERIMAGE,
                         24, 192, 82, 22, 0);
        state->key = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_TABSTOP | ES_AUTOHSCROLL, 116, 192, 330, 24, RID_KEY);
        addRemoteControl(state, hwnd, 0, L"BUTTON", L"...", WS_TABSTOP | BS_PUSHBUTTON,
                         454, 192, 54, 24, RID_KEY_BROWSE);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Frase-chave", SS_LEFT | SS_CENTERIMAGE,
                         24, 222, 82, 22, 0);
        state->passphrase = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"",
                         WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 116, 222, 392, 24, RID_PASSPHRASE);

        addRemoteControl(state, hwnd, 0, L"BUTTON", L"Navegar no Ubuntu Server", BS_GROUPBOX,
                         10, 264, 516, 326, 0);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Selecionada", SS_LEFT | SS_CENTERIMAGE,
                         24, 286, 82, 22, 0);
        state->remotePath = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, L"EDIT", L"/root",
                         WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY, 116, 286, 392, 24, RID_REMOTE_PATH);
        addRemoteControl(state, hwnd, 0, L"STATIC", L"Adicionar em", SS_LEFT | SS_CENTERIMAGE,
                         24, 316, 82, 22, 0);
        state->targetContainer = addRemoteControl(state, hwnd, 0, L"COMBOBOX", L"",
                         WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                         116, 316, 392, 220, RID_TARGET_CONTAINER);
        SendMessageW(state->targetContainer, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Raiz do workspace (sem contêiner)"));
        for (const auto& containerName : state->containerNames)
            SendMessageW(state->targetContainer, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(containerName.c_str()));
        SendMessageW(state->targetContainer, CB_SETCURSEL,
                     state->targetContainerIndex >= 0 ? state->targetContainerIndex + 1 : 0, 0);

        state->browserTree = addRemoteControl(state, hwnd, WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                         WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                         TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP,
                         24, 348, 484, 202, RID_REMOTE_TREE);
        TreeView_SetExtendedStyle(state->browserTree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        state->browserStatus = addRemoteControl(state, hwnd, 0, L"STATIC",
                         L"Desconectado. Clique em Conectar para listar /root.",
                         SS_LEFT | SS_CENTERIMAGE, 24, 556, 484, 22, RID_REMOTE_STATUS);

        addRemoteControl(state, hwnd, 0, L"STATIC",
                         L"As credenciais são protegidas no .worknpp. Nenhum contêiner é criado: "
                         L"a pasta selecionada entra no destino escolhido acima.",
                         SS_LEFT, 18, 600, 500, 34, RID_SECURITY_NOTE);

        state->connectButton = addRemoteControl(state, hwnd, 0, L"BUTTON", L"Conectar",
                         WS_TABSTOP | BS_PUSHBUTTON, 18, 646, 112, 30, RID_TEST);
        state->removeButton = addRemoteControl(state, hwnd, 0, L"BUTTON", L"Remover conexão",
                         WS_TABSTOP | BS_PUSHBUTTON, 140, 646, 120, 30, RID_REMOVE_REMOTE);
        addRemoteControl(state, hwnd, 0, L"BUTTON", L"Adicionar pasta",
                         WS_TABSTOP | BS_DEFPUSHBUTTON, 274, 646, 144, 30, RID_SAVE_SYNC);
        addRemoteControl(state, hwnd, 0, L"BUTTON", L"Cancelar",
                         WS_TABSTOP | BS_PUSHBUTTON, 428, 646, 90, 30, RID_CANCEL_REMOTE);

        if (state->npp)
            SendMessageW(state->npp, NPPM_DARKMODESUBCLASSANDTHEME,
                         static_cast<WPARAM>(NppDarkMode::dmfHandleChange),
                         reinterpret_cast<LPARAM>(hwnd));

        const int comboSelection = state->selectedIndex >= 0 ? state->selectedIndex + 1 : 0;
        SendMessageW(state->connection, CB_SETCURSEL, comboSelection, 0);
        fillRemoteDialog(state, state->selectedIndex);
        return 0;
    }
    case WM_COMMAND:
    {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == RID_CONNECTION && code == CBN_SELCHANGE)
        {
            const int selected = static_cast<int>(SendMessageW(state->connection, CB_GETCURSEL, 0, 0));
            SetWindowTextW(state->connectButton, L"Conectar");
            fillRemoteDialog(state, selected <= 0 ? -1 : selected - 1);
            return 0;
        }
        if (id == RID_AUTH && code == CBN_SELCHANGE)
        {
            updateRemoteAuthControls(state);
            return 0;
        }
        if (id == RID_KEY_BROWSE && code == BN_CLICKED)
        {
            wchar_t fileName[MAX_PATH * 4]{};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"Chaves privadas (*.*)\0*.*\0\0";
            ofn.lpstrFile = fileName;
            ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
            ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            if (GetOpenFileNameW(&ofn)) SetWindowTextW(state->key, fileName);
            return 0;
        }
        if (id == RID_TEST && code == BN_CLICKED)
        {
            connectRemoteBrowser(state, hwnd);
            return 0;
        }
        if (id == RID_SAVE_SYNC && code == BN_CLICKED)
        {
            if (!state->connected || !state->browserClient || !state->browserClient->isConnected())
            {
                MessageBoxW(hwnd, L"Conecte ao servidor e escolha uma pasta em /root antes de adicionar.",
                            L"Navegação SSH", MB_OK | MB_ICONWARNING);
                return 0;
            }
            std::wstring error;
            if (!readRemoteDialog(state, state->value, error))
            {
                MessageBoxW(hwnd, error.c_str(), L"Conexão SSH", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!sameRemoteConnectionSettings(state->value, state->connectedConfig))
            {
                MessageBoxW(hwnd,
                            L"As credenciais foram alteradas depois da conexão. Clique em Reconectar antes de adicionar a pasta.",
                            L"Conexão SSH", MB_OK | MB_ICONWARNING);
                return 0;
            }
            const int targetSelection = static_cast<int>(
                SendMessageW(state->targetContainer, CB_GETCURSEL, 0, 0));
            state->targetContainerIndex = targetSelection <= 0 ? -1 : targetSelection - 1;
            state->action = RemoteDialogState::Action::SaveAndSync;
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == RID_REMOVE_REMOTE && code == BN_CLICKED && state->selectedIndex >= 0)
        {
            state->action = RemoteDialogState::Action::Remove;
            DestroyWindow(hwnd);
            return 0;
        }
        if ((id == RID_CANCEL_REMOTE && code == BN_CLICKED) || id == IDCANCEL)
        {
            state->action = RemoteDialogState::Action::Cancel;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_NOTIFY:
    {
        const auto* header = reinterpret_cast<const NMHDR*>(lParam);
        if (!header || header->idFrom != RID_REMOTE_TREE) break;
        if (header->code == TVN_ITEMEXPANDINGW)
        {
            const auto* expanding = reinterpret_cast<const NMTREEVIEWW*>(lParam);
            if (expanding->action == TVE_EXPAND)
                loadRemoteBrowserChildren(state, expanding->itemNew.hItem, true);
            return 0;
        }
        if (header->code == TVN_SELCHANGEDW)
        {
            const auto* changed = reinterpret_cast<const NMTREEVIEWW*>(lParam);
            const auto found = state->browserPaths.find(changed->itemNew.hItem);
            if (found != state->browserPaths.end())
                SetWindowTextW(state->remotePath, found->second.c_str());
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            state->action = RemoteDialogState::Action::Cancel;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        state->action = RemoteDialogState::Action::Cancel;
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool runRemoteDialog(RemoteDialogState& state)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = remoteDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = REMOTE_DIALOG_CLASS;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                  REMOTE_DIALOG_CLASS, L"Ubuntu Server — SSH/SFTP",
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 552, 724,
                                  state.owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dialog) return false;

    RECT ownerRect{};
    RECT dialogRect{};
    GetWindowRect(state.owner, &ownerRect);
    GetWindowRect(dialog, &dialogRect);
    const int width = dialogRect.right - dialogRect.left;
    const int height = dialogRect.bottom - dialogRect.top;
    int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
    HMONITOR monitor = MonitorFromWindow(state.owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(monitor, &mi);
    x = (std::max)(static_cast<int>(mi.rcWork.left), (std::min)(x, static_cast<int>(mi.rcWork.right) - width));
    y = (std::max)(static_cast<int>(mi.rcWork.top), (std::min)(y, static_cast<int>(mi.rcWork.bottom) - height));
    SetWindowPos(dialog, HWND_TOP, x, y, 0, 0, SWP_NOSIZE);

    EnableWindow(state.owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);

    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(dialog, &message))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(state.owner, TRUE);
    SetForegroundWindow(state.owner);
    return state.action != RemoteDialogState::Action::Cancel;
}

struct RemoteJobCompletion
{
    RemoteJobCompletion(RemoteWorkspaceFolder folderValue, int typeValue,
                        std::filesystem::path localValue, RemoteSshResult resultValue)
        : folder(std::move(folderValue)), type(typeValue), localFile(std::move(localValue)),
          result(std::move(resultValue)) {}

    RemoteWorkspaceFolder folder;
    int type{};
    std::filesystem::path localFile;
    RemoteSshResult result;
};

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
    if (_workspaceFile.empty()) _workspaceFile = getWorkspaceFilePath();
    createWindow();
    startRemoteWorker();
    if (_npp) {
        DragAcceptFiles(_npp, TRUE);
        _oldNppProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(_npp, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::nppProc)));
    }
    refreshWorkspace();
    disableNativeFolderWorkspace();
}

void QuickOpen::destroy()
{
    stopRemoteWorker();
    _searchIndexGeneration.fetch_add(1);
    _searchQueryGeneration.fetch_add(1);
    if (_searchIndexThread.joinable()) _searchIndexThread.join();
    if (_searchQueryThread.joinable()) _searchQueryThread.join();
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

    releaseDockHost();
    releaseDockSplitters();
    unregisterDock();

    if (_window)
    {
        DestroyWindow(_window);
        _window = nullptr;
    }

    if (_font) DeleteObject(_font);
    if (_titleFont) DeleteObject(_titleFont);
    if (_symbolFont) DeleteObject(_symbolFont);
    if (_backgroundBrush) DeleteObject(_backgroundBrush);
    _font = nullptr;
    _titleFont = nullptr;
    _symbolFont = nullptr;
    _backgroundBrush = nullptr;
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

    // Use the Windows UI font that Notepad++ itself follows instead of a
    // plugin-specific font. This keeps the panel consistent with the host.
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    const LOGFONTW& lf = ncm.lfMessageFont;
    _font = CreateFontIndirectW(&lf);
    LOGFONTW titleLf = lf;
    titleLf.lfWeight = FW_SEMIBOLD;
    _titleFont = CreateFontIndirectW(&titleLf);
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
    {
        _registeredDock = true;
        refreshDockHost();
        refreshDockSplitters();
    }
}

void QuickOpen::refreshDockHost()
{
    if (!_window) return;
    HWND host = GetParent(_window);
    if (!host || host == _dockHost) return;

    releaseDockHost();
    _dockHost = host;
    _oldDockHostProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        _dockHost, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::dockHostProc)));
}

void QuickOpen::releaseDockHost()
{
    if (_dockHost && _oldDockHostProc)
    {
        SetWindowLongPtrW(_dockHost, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(_oldDockHostProc));
    }
    _dockHost = nullptr;
    _oldDockHostProc = nullptr;
}

LRESULT CALLBACK QuickOpen::dockHostProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (!g_instance) return DefWindowProcW(h, msg, w, l);
    return g_instance->handleDockHostMessage(h, msg, w, l);
}

LRESULT CALLBACK QuickOpen::dockSplitterProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (!g_instance) return DefWindowProcW(h, msg, w, l);
    return g_instance->handleDockSplitterMessage(h, msg, w, l);
}

namespace
{
constexpr int WORKSPACE_MIN_WIDTH = 430;
constexpr int WORKSPACE_MIN_HEIGHT = 360;

enum class DockSide { None, Left, Right, Top, Bottom };

DockSide getDockSideForSplitter(HWND panel, HWND splitter)
{
    if (!panel || !splitter) return DockSide::None;
    HWND container = GetParent(panel);
    if (!container) return DockSide::None;

    RECT rcContainer{}, rcSplitter{};
    if (!GetWindowRect(container, &rcContainer) || !GetWindowRect(splitter, &rcSplitter))
        return DockSide::None;

    constexpr LONG tolerance = 8;
    if (rcSplitter.left >= rcContainer.right - tolerance && rcSplitter.left <= rcContainer.right + tolerance)
        return DockSide::Left;
    if (rcSplitter.right <= rcContainer.left + tolerance && rcSplitter.right >= rcContainer.left - tolerance)
        return DockSide::Right;
    if (rcSplitter.top >= rcContainer.bottom - tolerance && rcSplitter.top <= rcContainer.bottom + tolerance)
        return DockSide::Top;
    if (rcSplitter.bottom <= rcContainer.top + tolerance && rcSplitter.bottom >= rcContainer.top - tolerance)
        return DockSide::Bottom;
    return DockSide::None;
}

void clampWorkspaceMinMaxInfo(HWND, MINMAXINFO* info)
{
    if (!info) return;
    info->ptMinTrackSize.x = (std::max)(info->ptMinTrackSize.x, static_cast<LONG>(WORKSPACE_MIN_WIDTH));
    info->ptMinTrackSize.y = (std::max)(info->ptMinTrackSize.y, static_cast<LONG>(WORKSPACE_MIN_HEIGHT));
}

void clampWorkspaceWindowPos(HWND, WINDOWPOS* pos)
{
    if (!pos || (pos->flags & SWP_NOSIZE)) return;
    const int minWidth = static_cast<int>(WORKSPACE_MIN_WIDTH);
    const int minHeight = static_cast<int>(WORKSPACE_MIN_HEIGHT);
    pos->cx = (std::max)(minWidth, pos->cx);
    pos->cy = (std::max)(minHeight, pos->cy);
}
}

void QuickOpen::refreshDockSplitters()
{
    if (!_npp) return;

    auto hookClass = [this](const wchar_t* className)
    {
        HWND h = nullptr;
        while ((h = FindWindowExW(_npp, h, className, nullptr)) != nullptr)
        {
            bool already = false;
            for (const auto& hook : _dockSplitters)
            {
                if (hook.hwnd == h) { already = true; break; }
            }
            if (already) continue;

            WNDPROC oldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&QuickOpen::dockSplitterProc)));
            if (oldProc)
                _dockSplitters.push_back({h, oldProc});
        }
    };

    // Notepad++ creates these two native DockingSplitter classes. The splitter,
    // not the plugin child window, owns the mouse-driven resize operation.
    // This is important for docked panels: constraining only the plugin HWND
    // cannot stop DockingManager from moving the splitter past the panel.
    hookClass(L"wedockspliter");
    hookClass(L"nsdockspliter");
}

void QuickOpen::releaseDockSplitters()
{
    for (auto it = _dockSplitters.rbegin(); it != _dockSplitters.rend(); ++it)
    {
        if (it->hwnd && IsWindow(it->hwnd) && it->oldProc)
            SetWindowLongPtrW(it->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->oldProc));
    }
    _dockSplitters.clear();
    _activeSplitter = nullptr;
    _splitterTracking = false;
}

LRESULT QuickOpen::handleDockSplitterMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    WNDPROC oldProc = nullptr;
    for (const auto& hook : _dockSplitters)
    {
        if (hook.hwnd == h) { oldProc = hook.oldProc; break; }
    }
    if (!oldProc) return DefWindowProcW(h, msg, w, l);

    if (msg == WM_LBUTTONDOWN)
    {
        GetCursorPos(&_lastSplitterCursor);
        _activeSplitter = h;
        _splitterTracking = true;
    }
    else if (msg == WM_MOUSEMOVE && _splitterTracking && _activeSplitter == h && (w & MK_LBUTTON))
    {
        HWND container = _window ? GetParent(_window) : nullptr;
        DockSide side = getDockSideForSplitter(_window, h);
        if (container && side != DockSide::None)
        {
            RECT rc{};
            GetWindowRect(container, &rc);
            POINT cursor{};
            GetCursorPos(&cursor);

            const int dx = cursor.x - _lastSplitterCursor.x;
            const int dy = cursor.y - _lastSplitterCursor.y;
            int current = 0;
            int delta = 0;
            int minSize = 0;

            switch (side)
            {
            case DockSide::Left:
                current = rc.right - rc.left;
                delta = dx;
                minSize = WORKSPACE_MIN_WIDTH;
                break;
            case DockSide::Right:
                current = rc.right - rc.left;
                delta = -dx;
                minSize = WORKSPACE_MIN_WIDTH;
                break;
            case DockSide::Top:
                current = rc.bottom - rc.top;
                delta = dy;
                minSize = WORKSPACE_MIN_HEIGHT;
                break;
            case DockSide::Bottom:
                current = rc.bottom - rc.top;
                delta = -dy;
                minSize = WORKSPACE_MIN_HEIGHT;
                break;
            default:
                break;
            }

            const int requested = current + delta;
            const int clamped = (std::max)(minSize, requested);
            if (clamped != requested)
            {
                const int allowedDelta = clamped - current;
                POINT corrected = cursor;
                if (side == DockSide::Left) corrected.x = _lastSplitterCursor.x + allowedDelta;
                else if (side == DockSide::Right) corrected.x = _lastSplitterCursor.x - allowedDelta;
                else if (side == DockSide::Top) corrected.y = _lastSplitterCursor.y + allowedDelta;
                else if (side == DockSide::Bottom) corrected.y = _lastSplitterCursor.y - allowedDelta;

                SetCursorPos(corrected.x, corrected.y);
                GetCursorPos(&cursor);
            }

            _lastSplitterCursor = cursor;
        }
    }
    else if (msg == WM_LBUTTONUP || msg == WM_NCLBUTTONUP ||
             (msg == WM_CAPTURECHANGED && _splitterTracking))
    {
        _splitterTracking = false;
        _activeSplitter = nullptr;
    }

    return CallWindowProcW(oldProc, h, msg, w, l);
}

LRESULT QuickOpen::handleDockHostMessage(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    // Clamp BEFORE forwarding WM_WINDOWPOSCHANGING so the docking manager
    // receives the corrected size instead of immediately overwriting it.
    if (msg == WM_WINDOWPOSCHANGING)
        clampWorkspaceWindowPos(h, reinterpret_cast<WINDOWPOS*>(l));

    LRESULT result = _oldDockHostProc ? CallWindowProcW(_oldDockHostProc, h, msg, w, l)
                                      : DefWindowProcW(h, msg, w, l);

    if (msg == WM_GETMINMAXINFO)
        clampWorkspaceMinMaxInfo(h, reinterpret_cast<MINMAXINFO*>(l));

    return result;
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
        8, 4, 420, 102, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_GROUP)),
        GetModuleHandleW(nullptr), nullptr);

    _search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NOHIDESEL,
        18, 24, 250, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _runSearch = CreateWindowExW(0, L"BUTTON", L"Pesquisar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        274, 24, 74, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RUN_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _searchScopeButton = CreateWindowExW(0, L"BUTTON", L"Escopo",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        354, 24, 64, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_SCOPE)),
        GetModuleHandleW(nullptr), nullptr);
    _contentSearchCheck = CreateWindowExW(0, L"BUTTON", L"Pesquisar dentro dos arquivos",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        18, 54, 220, 22, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CONTENT_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _cancelSearch = CreateWindowExW(0, L"BUTTON", L"Cancelar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        342, 52, 76, 24, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CANCEL_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _searchProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        18, 80, 150, 14, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_PROGRESS)),
        GetModuleHandleW(nullptr), nullptr);
    _searchProgressText = CreateWindowExW(0, L"STATIC", L"Pronto",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        176, 74, 242, 24, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_PROGRESS_TEXT)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(_searchProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    EnableWindow(_cancelSearch, FALSE);
    setButtonChecked(_contentSearchCheck, _searchInsideFiles);

    _workspaceGroup = CreateWindowExW(0, L"BUTTON", L"Workspace",
        WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
        8, 110, 420, 52, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_WORKSPACE_GROUP)),
        GetModuleHandleW(nullptr), nullptr);

    _addFolder = CreateWindowExW(0, L"BUTTON", L"\xE710",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        18, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_ADD)),
        GetModuleHandleW(nullptr), nullptr);
    _newWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE8A7",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        54, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_NEW)),
        GetModuleHandleW(nullptr), nullptr);
    _saveWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE74E",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        90, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SAVE)),
        GetModuleHandleW(nullptr), nullptr);
    _openWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE8B7",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        126, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_OPEN)),
        GetModuleHandleW(nullptr), nullptr);
    _removeFolder = CreateWindowExW(0, L"BUTTON", L"\xE74D",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        162, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REMOVE)),
        GetModuleHandleW(nullptr), nullptr);
    _expandAll = CreateWindowExW(0, L"BUTTON", L"\xE8A0",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        198, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_EXPAND_ALL)),
        GetModuleHandleW(nullptr), nullptr);
    _collapseAll = CreateWindowExW(0, L"BUTTON", L"\xE8A1",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        234, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_COLLAPSE_ALL)),
        GetModuleHandleW(nullptr), nullptr);
    _createContainer = CreateWindowExW(0, L"BUTTON", L"\xE8B8",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        270, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_CREATE_CONTAINER)),
        GetModuleHandleW(nullptr), nullptr);
    _remoteSsh = CreateWindowExW(0, L"BUTTON", L"\xE968",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        306, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REMOTE_SSH)),
        GetModuleHandleW(nullptr), nullptr);
    _refreshWorkspace = CreateWindowExW(0, L"BUTTON", L"\xE72C",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_CENTER,
        342, 130, 32, 28, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_REFRESH_WORKSPACE)),
        GetModuleHandleW(nullptr), nullptr);

    _tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
        TVS_SHOWSELALWAYS | TVS_INFOTIP,
        10, 170, 410, 500, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_TREE)),
        GetModuleHandleW(nullptr), nullptr);
    TreeView_SetExtendedStyle(_tree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);

    _results = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | LVS_REPORT | LVS_SHOWSELALWAYS,
        10, 170, 410, 500, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_RESULTS)),
        GetModuleHandleW(nullptr), nullptr);

    _viewFolders = CreateWindowExW(0, L"BUTTON", L"Pastas",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
        10, 602, 200, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEW_FOLDERS)),
        GetModuleHandleW(nullptr), nullptr);
    _viewSearch = CreateWindowExW(0, L"BUTTON", L"Pesquisa",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE,
        220, 602, 200, 26, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_VIEW_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);

    _status = CreateWindowExW(0, L"STATIC", L"Ctrl+B mostrar/ocultar  |  Ctrl+P pesquisar",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        10, 632, 410, 22, _window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_STATUS)),
        GetModuleHandleW(nullptr), nullptr);

    for (HWND h : {_searchGroup, _workspaceGroup, _search, _runSearch, _contentSearchCheck, _searchProgressText,
                   _cancelSearch, _searchScopeButton, _tree, _results, _viewFolders, _viewSearch,
                   _addFolder, _newWorkspace, _saveWorkspace, _openWorkspace, _removeFolder,
                   _expandAll, _collapseAll, _createContainer, _remoteSsh, _refreshWorkspace, _status})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);

    for (HWND h : {_addFolder, _newWorkspace, _saveWorkspace, _openWorkspace, _removeFolder,
                   _expandAll, _collapseAll, _createContainer, _remoteSsh, _refreshWorkspace})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(_symbolFont), TRUE);

    SendMessageW(_search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Pesquisar arquivos..."));

    createTooltips();
    updatePanelSwitcher();

    ListView_SetExtendedListViewStyle(_results, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 220;
    col.pszText = const_cast<LPWSTR>(L"Arquivo");
    ListView_InsertColumn(_results, 0, &col);
    col.cx = 240;
    col.pszText = const_cast<LPWSTR>(L"Pasta");
    ListView_InsertColumn(_results, 1, &col);
    col.cx = 560;
    col.pszText = const_cast<LPWSTR>(L"Path completo");
    ListView_InsertColumn(_results, 2, &col);

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
    addButtonTooltip(_saveWorkspace, L"Salvar o NPPWorkSpace atual (.worknpp)");
    addButtonTooltip(_openWorkspace, L"Abrir um arquivo .worknpp");
    addButtonTooltip(_removeFolder, L"Remover a pasta selecionada");
    addButtonTooltip(_expandAll, L"Expandir pastas ou grupos de pesquisa");
    addButtonTooltip(_collapseAll, L"Retrair pastas ou grupos de pesquisa");
    addButtonTooltip(_createContainer, L"Criar contêiner de projeto");
    addButtonTooltip(_remoteSsh, L"Conectar ao Ubuntu Server, navegar em /root e adicionar pastas por SSH/SFTP");
    addButtonTooltip(_refreshWorkspace, L"Atualizar a árvore ou sincronizar a pasta SSH selecionada (F5)");
    addButtonTooltip(_runSearch, L"Executar pesquisa");
    addButtonTooltip(_searchScopeButton, L"Escolher contêineres e pastas incluídos na pesquisa");
    addButtonTooltip(_contentSearchCheck, L"Quando marcado, pesquisa o conteúdo dos arquivos de texto suportados");
    addButtonTooltip(_cancelSearch, L"Cancelar a pesquisa em conteúdo em andamento");
    addButtonTooltip(_viewFolders, L"Mostrar pastas da Workspace sem limpar a pesquisa");
    addButtonTooltip(_viewSearch, L"Voltar aos resultados preservados da pesquisa");
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
        bool alreadyInContainer = false;
        for (const auto& container : _containers)
        {
            if (std::find(container.folders.begin(), container.folders.end(), root) != container.folders.end())
            {
                alreadyInContainer = true;
                break;
            }
        }
        if (!alreadyInContainer && std::find(_savedRoots.begin(), _savedRoots.end(), root) == _savedRoots.end())
        {
            _savedRoots.push_back(root);
            changed = true;
        }
    }
    if (changed)
    {
        writeWorkspaceFile();
        _nppRoots = roots;
        rebuildWorkspaceTree(true);
    }
    else if (_nppRoots != roots)
    {
        _nppRoots = roots;
        rebuildWorkspaceTree(true);
    }
    disableNativeFolderWorkspace();
}

void QuickOpen::layoutControls(int width, int height)
{
    if (!_window) return;
    width = (std::max)(static_cast<int>(WORKSPACE_MIN_WIDTH), width);
    height = (std::max)(static_cast<int>(WORKSPACE_MIN_HEIGHT), height);

    const int pad = 8;
    const int searchGroupH = 102;
    const int workspaceGroupY = 108;
    const int workspaceGroupH = 58;
    const int contentY = 172;
    const int statusH = 22;
    const int switcherH = 28;
    const int switcherY = height - statusH - switcherH - 8;
    const int contentH = (std::max)(60, switcherY - contentY - 6);

    MoveWindow(_searchGroup, pad, 4, width - 2 * pad, searchGroupH, TRUE);
    const int scopeButtonX = width - pad - 74;
    const int runButtonX = scopeButtonX - 86;
    MoveWindow(_search, pad + 10, 24, (std::max)(90, runButtonX - (pad + 10) - 6), 26, TRUE);
    MoveWindow(_runSearch, runButtonX, 24, 80, 26, TRUE);
    MoveWindow(_searchScopeButton, width - pad - 74, 24, 64, 26, TRUE);
    MoveWindow(_contentSearchCheck, pad + 10, 54, (std::max)(160, width - 2 * pad - 112), 22, TRUE);
    MoveWindow(_cancelSearch, width - pad - 86, 52, 76, 24, TRUE);
    MoveWindow(_searchProgress, pad + 10, 82, (std::max)(100, width / 3), 14, TRUE);
    MoveWindow(_searchProgressText, pad + 20 + (std::max)(100, width / 3), 76,
               (std::max)(120, width - 2 * pad - 30 - (std::max)(100, width / 3)), 24, TRUE);
    MoveWindow(_workspaceGroup, pad, workspaceGroupY, width - 2 * pad, workspaceGroupH, TRUE);

    constexpr int maxButtonSize = 32;
    constexpr int minButtonSize = 26;
    constexpr int gap = 3;
    const int buttonY = 126;
    int x = pad + 10;
    HWND buttons[] = {_addFolder, _newWorkspace, _saveWorkspace, _openWorkspace, _removeFolder,
                      _expandAll, _collapseAll, _createContainer, _remoteSsh, _refreshWorkspace};
    const int available = (std::max)(0, width - 2 * (pad + 10) -
                                       gap * (static_cast<int>(std::size(buttons)) - 1));
    const int buttonSize = (std::max)(minButtonSize,
        (std::min)(maxButtonSize, available / static_cast<int>(std::size(buttons))));
    for (HWND button : buttons)
    {
        MoveWindow(button, x, buttonY, buttonSize, 30, TRUE);
        x += buttonSize + gap;
    }

    MoveWindow(_tree, pad, contentY, width - 2 * pad, contentH, TRUE);
    MoveWindow(_results, pad, contentY, width - 2 * pad, contentH, TRUE);
    const int switcherW = width - 2 * pad;
    const int half = (switcherW - 6) / 2;
    MoveWindow(_viewFolders, pad, switcherY, half, switcherH, TRUE);
    MoveWindow(_viewSearch, pad + half + 6, switcherY, switcherW - half - 6, switcherH, TRUE);
    MoveWindow(_status, pad, height - statusH - 2, width - 2 * pad, statusH, TRUE);
    configureResultsColumns(_results, _searchInsideFiles && _searchOnly);
}

void QuickOpen::updatePanelSwitcher()
{
    setButtonChecked(_viewFolders, !_resultsViewVisible);
    setButtonChecked(_viewSearch, _resultsViewVisible);
    const bool hasSearch = _searchOnly || !_searchResults.empty() || _searchQueryRunning;
    if (_viewSearch) EnableWindow(_viewSearch, hasSearch);
}

void QuickOpen::showFoldersPanel()
{
    _resultsViewVisible = false;
    if (_results) ShowWindow(_results, SW_HIDE);
    if (_tree) ShowWindow(_tree, SW_SHOW);
    updatePanelSwitcher();
    if (_tree) SetFocus(_tree);
    if (_searchOnly && _status)
        SetWindowTextW(_status, L"Pastas  •  resultados preservados em Pesquisa");
}

void QuickOpen::showResultsPanel()
{
    if (!_searchOnly && _searchResults.empty() && !_searchQueryRunning)
    {
        updatePanelSwitcher();
        if (_status) SetWindowTextW(_status, L"Nenhuma pesquisa executada");
        return;
    }

    _resultsViewVisible = true;
    if (_tree) ShowWindow(_tree, SW_HIDE);
    if (_results) ShowWindow(_results, SW_SHOW);
    configureResultsColumns(_results, _searchInsideFiles && _searchOnly);
    updatePanelSwitcher();
    if (_results) SetFocus(_results);
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
        12, 38, 558, 30, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_POPUP_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _searchPopupRunSearch = CreateWindowExW(0, L"BUTTON", L"Pesquisar",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        576, 38, 86, 30, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_POPUP_RUN_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    _searchPopupScopeButton = CreateWindowExW(0, L"BUTTON", L"Escopo",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        670, 38, 78, 30, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_SEARCH_SCOPE)),
        GetModuleHandleW(nullptr), nullptr);
    _searchPopupContentCheck = CreateWindowExW(0, L"BUTTON", L"Pesquisar dentro dos arquivos",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        12, 72, 230, 22, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_POPUP_CONTENT_SEARCH)),
        GetModuleHandleW(nullptr), nullptr);
    setButtonChecked(_searchPopupContentCheck, _searchInsideFiles);
    SendMessageW(_searchPopupEdit, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupRunSearch, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupScopeButton, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupContentCheck, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    SendMessageW(_searchPopupEdit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Nome do arquivo/pasta..."));

    _searchPopupResults = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
        12, 100, 720, 288, _searchPopup, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_POPUP_RESULTS)),
        GetModuleHandleW(nullptr), nullptr);
    SendMessageW(_searchPopupResults, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);
    ListView_SetExtendedListViewStyle(_searchPopupResults, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = 220;
    col.pszText = const_cast<LPWSTR>(L"Arquivo");
    ListView_InsertColumn(_searchPopupResults, 0, &col);
    col.cx = 240;
    col.pszText = const_cast<LPWSTR>(L"Pasta");
    ListView_InsertColumn(_searchPopupResults, 1, &col);
    col.cx = 560;
    col.pszText = const_cast<LPWSTR>(L"Path completo");
    ListView_InsertColumn(_searchPopupResults, 2, &col);

    _searchPopupHint = CreateWindowExW(0, L"STATIC", L"Enter pesquisar  |  Na lista: Enter abrir  |  Esc fechar",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
        12, 394, 300, 20, _searchPopup, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(_searchPopupHint, WM_SETFONT, reinterpret_cast<WPARAM>(_font), TRUE);

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
    const int scopeButtonX = w - 90;
    const int runButtonX = scopeButtonX - 94;
    MoveWindow(_searchPopupEdit, 12, 38, (std::max)(140, runButtonX - 20), 30, TRUE);
    MoveWindow(_searchPopupRunSearch, runButtonX, 38, 86, 30, TRUE);
    MoveWindow(_searchPopupScopeButton, w - 90, 38, 78, 30, TRUE);
    MoveWindow(_searchPopupContentCheck, 12, 72, w - 24, 22, TRUE);
    MoveWindow(_searchPopupResults, 12, 100, w - 24, h - 140, TRUE);
    MoveWindow(_searchPopupHint, 12, h - 32, w - 24, 20, TRUE);
    configureResultsColumns(_searchPopupResults, _searchInsideFiles);
}

void QuickOpen::showSearchPopup()
{
    createSearchPopup();
    if (!_searchPopup) return;
    // Ctrl+P must reuse the already-built index. Rebuilding/invalidation on every
    // invocation made the first query race with the index worker and caused
    // subsequent queries to appear stale or empty. The index is invalidated only
    // when the workspace/scope actually changes.
    if (_searchPopupEdit) SetWindowTextW(_searchPopupEdit, L"");
    if (_searchPopupResults) ListView_DeleteAllItems(_searchPopupResults);
    _searchResults.clear();
    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();
    configureResultsColumns(_searchPopupResults, false);
    if (_searchPopupContentCheck)
        setButtonChecked(_searchPopupContentCheck, _searchInsideFiles);
    if (!_searchIndexValid && !_searchIndexBuilding)
        startSearchIndexBuild();
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
    if (_searchPopup)
    {
        if (_contentSearchPopup) cancelContentSearch();
        ShowWindow(_searchPopup, SW_HIDE);
    }
}

void QuickOpen::updatePopupSearch()
{
    if (!_searchPopupEdit) return;
    const std::wstring query = getWindowText(_searchPopupEdit);
    if (query.empty())
    {
        if (_contentSearchPopup) cancelContentSearch();
        _searchResults.clear();
        _contentSearchGroups.clear();
        _contentSearchGroupByPath.clear();
        ListView_DeleteAllItems(_searchPopupResults);
        configureResultsColumns(_searchPopupResults, false);
        return;
    }
    showPopupSearchResults(query);
}


void QuickOpen::startContentSearch(const std::wstring& query, bool popup)
{
    if (!isContentSearchEnabled(popup) || query.size() < 2 || !_window) return;

    if (_searchQueryRunning)
    {
        _pendingContentQuery = query;
        _pendingContentPopup = popup;
        _searchQueryGeneration.fetch_add(1);
        return;
    }
    if (_searchQueryThread.joinable())
        _searchQueryThread.join();

    _searchQueryRunning = true;
    const unsigned int generation = _searchQueryGeneration.fetch_add(1) + 1;
    _runningContentQuery = query;
    _runningContentPopup = popup;
    _contentSearchPopup = popup;
    _contentSearchProcessed = 0;
    _contentSearchTotal = 0;
    _contentSearchHits = 0;

    std::vector<std::filesystem::path> roots;
    if (_searchIncludedPaths.empty())
    {
        roots = getWorkspaceRootsForPanel();
    }
    else
    {
        roots.reserve(_searchIncludedPaths.size());
        for (const auto& included : _searchIncludedPaths)
            roots.emplace_back(included);
    }
    const auto included = _searchIncludedPaths;
    const auto disabled = _searchDisabledPaths;
    const HWND target = _window;
    const std::wstring queryLower = lowerText(query);
    const bool asciiQuery = isAsciiString(query);
    const std::string asciiNeedle = asciiQuery ? utf8FromWide(query) : std::string();

    EnableWindow(_cancelSearch, TRUE);
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Preparando pesquisa...");
    if (popup && _searchPopupHint) SetWindowTextW(_searchPopupHint, L"Pesquisando...");

    QO_DEBUG_LOG(L"Início da pesquisa em conteúdo");
    QO_DEBUG_LOG(L"Filtro: " + query);

    _searchQueryThread = std::thread([this, query, queryLower, asciiQuery, asciiNeedle,
                                      roots, included, disabled, popup, generation, target]()
    {
#ifdef _DEBUG
        const auto started = std::chrono::steady_clock::now();
#endif

        struct FileJob
        {
            std::filesystem::path path;
            std::wstring fileName;
            std::wstring folder;
            std::wstring relative;
        };

        std::deque<FileJob> jobs;
        std::mutex mutex;
        std::condition_variable cv;
        std::atomic<bool> enumerationDone{false};
        std::atomic<size_t> total{0};
        std::atomic<size_t> processed{0};
        std::atomic<size_t> hits{0};

        auto stillCurrent = [&]()
        {
            return _searchQueryGeneration.load() == generation;
        };

        auto enabled = [&](const std::filesystem::path& path)
        {
            return isPathEnabledForScope(path, included, disabled);
        };

        auto postProgress = [&](const std::wstring& currentFile)
        {
            if (!target) return;
            auto* progress = new ContentSearchProgress();
            progress->generation = generation;
            progress->popup = popup;
            progress->processed = processed.load();
            progress->total = total.load();
            progress->hits = hits.load();
            progress->currentFile = currentFile;
            if (!PostMessageW(target, WM_CONTENT_SEARCH_PROGRESS, 0, reinterpret_cast<LPARAM>(progress)))
                delete progress;
        };

        auto postBatch = [&](std::vector<ContentFileGroup>& batch)
        {
            if (batch.empty() || !target) return;
            auto* message = new ContentSearchBatch();
            message->generation = generation;
            message->popup = popup;
            message->files.swap(batch);
            if (!PostMessageW(target, WM_CONTENT_SEARCH_BATCH, 0, reinterpret_cast<LPARAM>(message)))
                delete message;
        };

        auto makeJob = [](const std::filesystem::path& root, const std::filesystem::path& path)
        {
            FileJob job;
            job.path = path;
            job.fileName = path.filename().wstring();
            if (job.fileName.empty()) job.fileName = path.wstring();

            std::filesystem::path relPath = workspaceRelativePath(path, root);
            if (relPath.empty()) relPath = path.filename();
            job.relative = relPath.wstring();
            std::filesystem::path folderPath = relPath.parent_path();
            job.folder = folderPath.empty() ? L"\\" : L"\\" + folderPath.wstring();
            std::replace(job.folder.begin(), job.folder.end(), L'/', L'\\');
            return job;
        };

        auto scanFile = [&](const FileJob& job)
        {
            ContentFileGroup group;
            group.path = job.path;
            group.fileName = job.fileName;
            group.folder = job.folder;
            group.relative = job.relative;

#ifdef _DEBUG
            const auto fileStart = std::chrono::steady_clock::now();
#endif
            std::error_code ec;
            const auto size = std::filesystem::file_size(job.path, ec);
            if (ec || size == 0 || size > MAX_CONTENT_FILE_SIZE)
                return group;

            std::ifstream in(job.path, std::ios::binary);
            if (!in) return group;

            std::vector<unsigned char> bytes(static_cast<size_t>(size));
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) return group;
            bytes.resize(static_cast<size_t>(got));

            if (asciiQuery && !hasUtf16Pattern(bytes) && !containsAsciiBytesInsensitive(bytes, asciiNeedle))
                return group;

            std::wstring text;
            TextEncoding encoding = TextEncoding::Unknown;
            if (!decodeTextBytes(bytes, text, encoding))
                return group;

            size_t lineNumber = 1;
            size_t lineStart = 0;
            while (lineStart <= text.size() && stillCurrent())
            {
                size_t lineEnd = text.find(L'\n', lineStart);
                if (lineEnd == std::wstring::npos) lineEnd = text.size();
                size_t logicalEnd = lineEnd;
                if (logicalEnd > lineStart && text[logicalEnd - 1] == L'\r')
                    --logicalEnd;

                std::wstring line = text.substr(lineStart, logicalEnd - lineStart);
                std::wstring lineLower = lowerText(line);

                size_t searchFrom = 0;
                size_t occurrenceOnLine = 0;
                while (stillCurrent())
                {
                    const size_t pos = lineLower.find(queryLower, searchFrom);
                    if (pos == std::wstring::npos) break;

                    const std::wstring matchText = line.substr(pos, (std::min)(query.size(), line.size() - pos));
                    SnippetInfo snippet = makeSnippet(line, pos, matchText.size());
                    group.matches.push_back({lineNumber, std::move(snippet.text), snippet.matchStart,
                                             matchText.size(), occurrenceOnLine, matchText});
                    searchFrom = pos + (std::max)(size_t(1), queryLower.size());
                    ++occurrenceOnLine;
                }

                if (lineEnd == text.size()) break;
                lineStart = lineEnd + 1;
                ++lineNumber;
            }

#ifdef _DEBUG
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - fileStart).count();
            QO_DEBUG_LOG(L"Arquivo: " + job.relative + L" | encoding=" + encodingName(encoding) +
                         L" | tempo=" + std::to_wstring(elapsed) + L"ms | ocorrências=" +
                         std::to_wstring(group.matches.size()));
#endif
            return group;
        };

        auto worker = [&]()
        {
            std::vector<ContentFileGroup> batch;
            batch.reserve(CONTENT_BATCH_FILE_COUNT);

            while (stillCurrent())
            {
                FileJob job;
                {
                    std::unique_lock<std::mutex> lock(mutex);
                    cv.wait(lock, [&]() { return !jobs.empty() || enumerationDone.load() || !stillCurrent(); });
                    if (!stillCurrent()) break;
                    if (jobs.empty())
                    {
                        if (enumerationDone.load()) break;
                        continue;
                    }
                    job = std::move(jobs.front());
                    jobs.pop_front();
                    cv.notify_one();
                }

                postProgress(job.relative);
                ContentFileGroup group = scanFile(job);
                const size_t fileHits = group.matches.size();
                if (fileHits > 0)
                {
                    hits.fetch_add(fileHits);
                    batch.push_back(std::move(group));
                    if (batch.size() >= CONTENT_BATCH_FILE_COUNT)
                        postBatch(batch);
                }
                processed.fetch_add(1);
                postProgress(job.relative);
            }

            if (stillCurrent())
                postBatch(batch);
        };

        const unsigned int hardware = std::thread::hardware_concurrency();
        const size_t requestedWorkers = static_cast<size_t>(hardware ? hardware : 2);
        const size_t workerCount = (std::max)(size_t(1), (std::min)(size_t(4), requestedWorkers));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i)
            workers.emplace_back(worker);

        std::unordered_set<std::wstring> seen;
        for (const auto& requestedRoot : roots)
        {
            if (!stillCurrent()) break;
            QO_DEBUG_LOG(L"Diretório: " + requestedRoot.wstring());
            enumerateWorkspaceFiles(
                requestedRoot,
                (std::numeric_limits<size_t>::max)(),
                [&](const std::filesystem::path& root, const std::filesystem::path& path)
                {
                    if (!stillCurrent()) return false;
                    if (isWorkspaceTemporaryFile(path) ||
                        !isSupportedTextExtension(path) || !enabled(path))
                        return true;

                    const std::wstring key = normalizeScopePath(path.lexically_normal().wstring());
                    if (!seen.insert(key).second) return true;

                    FileJob job = makeJob(root, path);
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        cv.wait(lock, [&]() { return jobs.size() < CONTENT_QUEUE_LIMIT || !stillCurrent(); });
                        if (!stillCurrent()) return false;
                        jobs.push_back(std::move(job));
                        total.fetch_add(1);
                    }
                    cv.notify_one();
                    return true;
                },
                [&]() { return !stillCurrent(); });
        }

        enumerationDone = true;
        cv.notify_all();
        for (auto& thread : workers)
            if (thread.joinable()) thread.join();

        _searchQueryRunning = false;

        auto* complete = new ContentSearchComplete();
        complete->generation = generation;
        complete->popup = popup;
        complete->cancelled = _searchQueryGeneration.load() != generation;
        complete->processed = processed.load();
        complete->total = total.load();
        complete->hits = hits.load();

#ifdef _DEBUG
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        QO_DEBUG_LOG((complete->cancelled ? L"Pesquisa cancelada" : L"Pesquisa concluída") +
                     (L" | arquivos=" + std::to_wstring(complete->processed) + L"/" +
                      std::to_wstring(complete->total) + L" | ocorrências=" +
                      std::to_wstring(complete->hits) + L" | tempo total=" +
                      std::to_wstring(elapsed) + L"ms"));
#endif

        if (!target || !PostMessageW(target, WM_CONTENT_SEARCH_DONE, 0, reinterpret_cast<LPARAM>(complete)))
            delete complete;
    });
}

void QuickOpen::applyContentSearchBatch(ContentSearchBatch* batch)
{
    std::unique_ptr<ContentSearchBatch> holder(batch);
    if (!holder || holder->generation != _searchQueryGeneration.load()) return;

    HWND list = holder->popup ? _searchPopupResults : _results;
    if (!list) return;

    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    for (auto& group : holder->files)
        appendContentSearchGroup(std::move(group), list);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, FALSE);

    if (!_searchResults.empty() && ListView_GetSelectedCount(list) == 0)
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void QuickOpen::updateContentSearchProgress(ContentSearchProgress* progress)
{
    std::unique_ptr<ContentSearchProgress> holder(progress);
    if (!holder || holder->generation != _searchQueryGeneration.load()) return;

    _contentSearchProcessed = holder->processed;
    _contentSearchTotal = holder->total;
    _contentSearchHits = holder->hits;

    const int percent = holder->total > 0
        ? static_cast<int>((holder->processed * 100) / holder->total)
        : 0;
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, percent, 0);

    std::wstring text = std::to_wstring(holder->processed) + L"/" +
        std::to_wstring(holder->total) + L" (" + std::to_wstring(percent) + L"%)";
    if (!holder->currentFile.empty())
        text += L"  Pesquisando: " + holder->currentFile;
    if (_searchProgressText) SetWindowTextW(_searchProgressText, text.c_str());

    if (holder->popup && _searchPopupHint)
    {
        std::wstring hint = std::to_wstring(holder->hits) + L" ocorrência(s)  |  " + text;
        SetWindowTextW(_searchPopupHint, hint.c_str());
    }
    if (!holder->popup)
    {
        std::wstring status = std::to_wstring(holder->hits) + L" ocorrência(s) em " +
            std::to_wstring(_contentSearchGroups.size()) + L" arquivo(s)";
        SetWindowTextW(_status, status.c_str());
    }
}

void QuickOpen::completeContentSearch(ContentSearchComplete* complete)
{
    std::unique_ptr<ContentSearchComplete> holder(complete);

    if (!_pendingContentQuery.empty() && !_searchQueryRunning)
    {
        const std::wstring query = _pendingContentQuery;
        const bool popup = _pendingContentPopup;
        _pendingContentQuery.clear();
        _pendingContentPopup = false;
        resetContentSearchResults(popup);
        startContentSearch(query, popup);
        return;
    }

    if (!holder || holder->generation != _searchQueryGeneration.load())
        return;

    _contentSearchProcessed = holder->processed;
    _contentSearchTotal = holder->total;
    _contentSearchHits = holder->hits;
    EnableWindow(_cancelSearch, FALSE);

    const int percent = holder->total > 0
        ? static_cast<int>((holder->processed * 100) / holder->total)
        : 100;
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, percent, 0);

    std::wstring text = (holder->cancelled ? L"Pesquisa cancelada: " : L"Pesquisa concluída: ");
    text += std::to_wstring(holder->processed) + L"/" + std::to_wstring(holder->total) +
        L" arquivo(s), " + std::to_wstring(holder->hits) + L" ocorrência(s)";
    if (_searchProgressText) SetWindowTextW(_searchProgressText, text.c_str());

    if (holder->popup && _searchPopupHint)
        SetWindowTextW(_searchPopupHint, text.c_str());
    if (!holder->popup)
        SetWindowTextW(_status, (text + L"  •  Enter abrir  •  Esc limpar pesquisa").c_str());
}

void QuickOpen::cancelContentSearch()
{
    _pendingContentQuery.clear();
    _pendingContentPopup = false;
    if (!_searchQueryRunning)
    {
        EnableWindow(_cancelSearch, FALSE);
        return;
    }

    _searchQueryGeneration.fetch_add(1);
    EnableWindow(_cancelSearch, FALSE);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pesquisa cancelada. Resultados preservados.");
    if (_searchPopupHint) SetWindowTextW(_searchPopupHint, L"Pesquisa cancelada. Resultados preservados.");
    SetWindowTextW(_status, L"Pesquisa cancelada. Resultados preservados.");
    QO_DEBUG_LOG(L"Cancelamento solicitado");
}

bool QuickOpen::isContentSearchEnabled(bool /*popup*/) const
{
    return _searchInsideFiles;
}

void QuickOpen::setContentSearchEnabled(bool enabled)
{
    if (_searchInsideFiles == enabled)
    {
        setButtonChecked(_contentSearchCheck, enabled);
        setButtonChecked(_searchPopupContentCheck, enabled);
        return;
    }

    _searchInsideFiles = enabled;
    setButtonChecked(_contentSearchCheck, enabled);
    setButtonChecked(_searchPopupContentCheck, enabled);
    _runSearchAfterIndexBuild = false;
    _runSearchAfterIndexPopup = false;

    cancelContentSearch();
    _searchResults.clear();
    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();

    if (_results)
    {
        configureResultsColumns(_results, enabled);
        ListView_DeleteAllItems(_results);
    }
    if (_searchPopupResults)
    {
        configureResultsColumns(_searchPopupResults, enabled);
        ListView_DeleteAllItems(_searchPopupResults);
    }

    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pressione Pesquisar ou Enter");
    if (_searchPopupHint) SetWindowTextW(_searchPopupHint, L"Enter pesquisar  |  Na lista: Enter abrir  |  Esc fechar");
    if (_searchOnly) SetWindowTextW(_status, L"Modo de pesquisa alterado. Pressione Pesquisar ou Enter.");
}

void QuickOpen::resetContentSearchResults(bool popup)
{
    HWND list = popup ? _searchPopupResults : _results;
    if (!list) return;

    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();
    _searchResults.clear();
    configureResultsColumns(list, true);
    ListView_DeleteAllItems(list);
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Preparando pesquisa...");
    if (popup && _searchPopupHint) SetWindowTextW(_searchPopupHint, L"Pesquisando...");
}

void QuickOpen::configureResultsColumns(HWND list, bool contentMode)
{
    if (!list) return;

    RECT rc{};
    GetClientRect(list, &rc);
    const int available = (std::max)(320, static_cast<int>(rc.right - rc.left) - GetSystemMetrics(SM_CXVSCROLL) - 8);
    const int firstWidth = contentMode ? (std::max)(170, available * 28 / 100)
                                       : (std::max)(180, available * 25 / 100);
    const int secondWidth = contentMode ? (std::max)(82, available * 14 / 100)
                                        : (std::max)(170, available * 28 / 100);
    const int thirdWidth = (std::max)(160, available - firstWidth - secondWidth);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = firstWidth;
    col.pszText = const_cast<LPWSTR>(contentMode ? L"Arquivo / linha" : L"Arquivo");
    ListView_SetColumn(list, 0, &col);
    col.cx = secondWidth;
    col.pszText = const_cast<LPWSTR>(contentMode ? L"Linha" : L"Pasta");
    ListView_SetColumn(list, 1, &col);
    col.cx = thirdWidth;
    col.pszText = const_cast<LPWSTR>(contentMode ? L"Trecho / path" : L"Path completo");
    ListView_SetColumn(list, 2, &col);
}

void QuickOpen::appendContentSearchGroup(ContentFileGroup&& group, HWND list)
{
    if (group.matches.empty()) return;

    std::wstring key = lowerText(group.path.lexically_normal().wstring());
    std::replace(key.begin(), key.end(), L'/', L'\\');
    if (auto it = _contentSearchGroupByPath.find(key); it != _contentSearchGroupByPath.end())
    {
        auto& existing = _contentSearchGroups[it->second];
        existing.matches.insert(existing.matches.end(),
                                std::make_move_iterator(group.matches.begin()),
                                std::make_move_iterator(group.matches.end()));
        rebuildContentResultsList(list);
        return;
    }

    const size_t groupIndex = _contentSearchGroups.size();
    _contentSearchGroupByPath[key] = groupIndex;
    _contentSearchGroups.push_back(std::move(group));

    auto appendRow = [&](const SearchResult& result)
    {
        const int row = static_cast<int>(_searchResults.size());
        _searchResults.push_back(result);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        std::wstring col0;
        std::wstring col1;
        std::wstring col2;
        const SearchResult& visible = _searchResults.back();
        if (visible.rowType == SearchResult::RowType::ContentFile)
        {
            col0 = (visible.contentFileIndex < _contentSearchGroups.size() &&
                    _contentSearchGroups[visible.contentFileIndex].collapsed ? L"[+] " : L"[-] ") +
                visible.fileName + L" (" + std::to_wstring(visible.lineNumber) + L")";
            col1 = visible.folder;
            col2 = visible.path.wstring();
        }
        else
        {
            col0 = L"    Linha " + std::to_wstring(visible.lineNumber);
            col1 = std::to_wstring(visible.lineNumber);
            col2 = visible.snippet;
        }
        item.pszText = const_cast<LPWSTR>(col0.c_str());
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, row, 1, const_cast<LPWSTR>(col1.c_str()));
        ListView_SetItemText(list, row, 2, const_cast<LPWSTR>(col2.c_str()));
    };

    const auto& stored = _contentSearchGroups.back();
    SearchResult header{};
    header.path = stored.path;
    header.fileName = stored.fileName;
    header.folder = stored.folder;
    header.relative = stored.relative;
    header.rowType = SearchResult::RowType::ContentFile;
    header.contentFileIndex = groupIndex;
    header.lineNumber = stored.matches.size();
    appendRow(header);

    if (!stored.collapsed)
    {
        for (size_t i = 0; i < stored.matches.size(); ++i)
        {
            const auto& match = stored.matches[i];
            SearchResult line{};
            line.path = stored.path;
            line.fileName = stored.fileName;
            line.folder = stored.folder;
            line.relative = stored.relative;
            line.rowType = SearchResult::RowType::ContentLine;
            line.contentFileIndex = groupIndex;
            line.contentMatchIndex = i;
            line.lineNumber = match.lineNumber;
            line.snippet = match.snippet;
            line.matchStart = match.matchStart;
            line.matchLength = match.matchLength;
            line.occurrenceOnLine = match.occurrenceOnLine;
            line.matchText = match.matchText;
            appendRow(line);
        }
    }
}

void QuickOpen::rebuildContentResultsList(HWND list)
{
    if (!list) return;

    std::vector<ContentFileGroup> groups = std::move(_contentSearchGroups);
    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();
    _searchResults.clear();
    ListView_DeleteAllItems(list);

    for (auto& group : groups)
        appendContentSearchGroup(std::move(group), list);
}

void QuickOpen::toggleContentGroupFromRow(HWND list, int row)
{
    if (row < 0 || row >= static_cast<int>(_searchResults.size())) return;
    const SearchResult& result = _searchResults[static_cast<size_t>(row)];
    if (result.rowType != SearchResult::RowType::ContentFile ||
        result.contentFileIndex >= _contentSearchGroups.size())
        return;

    _contentSearchGroups[result.contentFileIndex].collapsed =
        !_contentSearchGroups[result.contentFileIndex].collapsed;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    rebuildContentResultsList(list);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, FALSE);
}

void QuickOpen::showPopupSearchResults(const std::wstring& query)
{
    if (isContentSearchEnabled(true))
    {
        resetContentSearchResults(true);
        if (query.size() < 2)
        {
            cancelContentSearch();
            if (_searchPopupHint) SetWindowTextW(_searchPopupHint, L"Digite ao menos 2 caracteres");
            return;
        }
        startContentSearch(query, true);
        return;
    }

    cancelContentSearch();
    configureResultsColumns(_searchPopupResults, false);

    if (!_searchIndexValid)
    {
        if (!_searchIndexBuilding) startSearchIndexBuild();
        _runSearchAfterIndexBuild = true;
        _runSearchAfterIndexPopup = true;
        SetWindowTextW(_status, L"Indexando arquivos... aguarde");
        return;
    }

    const std::wstring q = lower(query);
    std::vector<std::pair<int, size_t>> ranked;
    ranked.reserve(MAX_SEARCH_RESULTS * 4);
    for (size_t i = 0; i < _searchIndex.size(); ++i)
    {
        const auto& item = _searchIndex[i];
        if (!isSearchPathEnabled(item.path)) continue;
        const int fileScore = fuzzyScoreLower(q, item.fileNameLower);
        const int pathScore = fuzzyScoreLower(q, item.relativeLower);
        const int score = (std::max)(fileScore, pathScore);
        if (score >= 0)
        {
            ranked.emplace_back(score, i);
            if (ranked.size() >= static_cast<size_t>(MAX_SEARCH_RESULTS * 4)) break;
        }
    }

    const size_t wanted = (std::min)(ranked.size(), static_cast<size_t>(MAX_SEARCH_RESULTS));
    std::partial_sort(ranked.begin(), ranked.begin() + wanted, ranked.end(),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });

    _searchResults.clear();
    _searchResults.reserve(wanted);
    for (size_t i = 0; i < wanted; ++i)
        _searchResults.push_back(_searchIndex[ranked[i].second]);

    ListView_DeleteAllItems(_searchPopupResults);
    for (size_t i = 0; i < _searchResults.size(); ++i)
    {
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(_searchResults[i].fileName.c_str());
        ListView_InsertItem(_searchPopupResults, &item);
        ListView_SetItemText(_searchPopupResults, static_cast<int>(i), 1,
                             const_cast<LPWSTR>(_searchResults[i].folder.c_str()));
        const std::wstring fullPath = _searchResults[i].path.wstring();
        ListView_SetItemText(_searchPopupResults, static_cast<int>(i), 2,
                             const_cast<LPWSTR>(fullPath.c_str()));
    }
    if (!_searchResults.empty())
        ListView_SetItemState(_searchPopupResults, 0, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
}

void QuickOpen::openPopupSearchResult()
{
    const int row = ListView_GetNextItem(_searchPopupResults, -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(_searchResults.size())) return;
    openSearchResultRow(_searchResults[static_cast<size_t>(row)]);
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
    writeWorkspaceFile();
    rebuildWorkspaceTree();

}

std::wstring QuickOpen::treeNodeKey(const NodeData& data) const
{
    if (data.type == NodeType::Container)
        return L"container:" + std::to_wstring(data.containerIndex);

    if (data.path.empty()) return {};
    std::error_code ec;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(data.path, ec);
    if (ec)
    {
        ec.clear();
        normalized = std::filesystem::absolute(data.path, ec);
        if (ec) normalized = data.path;
        normalized = normalized.lexically_normal();
    }
    return L"path:" + lower(normalized.wstring());
}

QuickOpen::TreeUiState QuickOpen::captureTreeUiState() const
{
    TreeUiState state;
    if (!_tree) return state;

    state.focusedWindow = GetFocus();
    const HTREEITEM selected = TreeView_GetSelection(_tree);
    const HTREEITEM firstVisible = TreeView_GetFirstVisible(_tree);

    std::function<void(HTREEITEM)> visit = [&](HTREEITEM item)
    {
        for (HTREEITEM current = item; current; current = TreeView_GetNextSibling(_tree, current))
        {
            const auto found = _nodeData.find(current);
            if (found == _nodeData.end()) continue;

            const std::wstring key = treeNodeKey(found->second);
            if (!key.empty())
            {
                TVITEMW tv{};
                tv.mask = TVIF_STATE;
                tv.stateMask = TVIS_EXPANDED;
                tv.hItem = current;
                if (TreeView_GetItem(_tree, &tv) && (tv.state & TVIS_EXPANDED))
                    state.expandedKeys.insert(key);
                if (current == selected) state.selectedKey = key;
                if (current == firstVisible) state.firstVisibleKey = key;
                if (_selectedTreeFiles.find(current) != _selectedTreeFiles.end())
                    state.selectedFileKeys.insert(key);
            }

            visit(TreeView_GetChild(_tree, current));
        }
    };
    visit(TreeView_GetRoot(_tree));
    return state;
}

void QuickOpen::restoreTreeUiState(const TreeUiState& state)
{
    if (!_tree) return;

    HTREEITEM selectedItem = nullptr;
    HTREEITEM firstVisibleItem = nullptr;
    _selectedTreeFiles.clear();

    std::function<void(HTREEITEM)> visit = [&](HTREEITEM item)
    {
        for (HTREEITEM current = item; current; current = TreeView_GetNextSibling(_tree, current))
        {
            const auto found = _nodeData.find(current);
            if (found == _nodeData.end()) continue;

            const std::wstring key = treeNodeKey(found->second);
            if (!key.empty())
            {
                if (key == state.selectedKey) selectedItem = current;
                if (key == state.firstVisibleKey) firstVisibleItem = current;
                if (state.selectedFileKeys.find(key) != state.selectedFileKeys.end() &&
                    found->second.type == NodeType::File)
                    _selectedTreeFiles.insert(current);
            }

            const bool shouldExpand = !key.empty() &&
                state.expandedKeys.find(key) != state.expandedKeys.end() &&
                found->second.type != NodeType::File;
            if (shouldExpand)
            {
                expandNode(current);
                TreeView_Expand(_tree, current, TVE_EXPAND);
                refreshTreeNodeLabel(current, true);
            }

            // Container roots are created eagerly even while their container
            // is collapsed. Visit any real children so selection and scroll
            // state can still be restored; lazy "..." nodes are not present
            // in _nodeData and are therefore ignored safely.
            HTREEITEM child = TreeView_GetChild(_tree, current);
            if (child && _nodeData.find(child) != _nodeData.end()) visit(child);
        }
    };
    visit(TreeView_GetRoot(_tree));

    if (selectedItem) TreeView_SelectItem(_tree, selectedItem);
    if (firstVisibleItem)
        SendMessageW(_tree, TVM_SELECTITEM, TVGN_FIRSTVISIBLE,
                     reinterpret_cast<LPARAM>(firstVisibleItem));
    if (!_selectedTreeFiles.empty()) InvalidateRect(_tree, nullptr, FALSE);

    if (state.focusedWindow && IsWindow(state.focusedWindow) &&
        (state.focusedWindow == _tree || state.focusedWindow == _results ||
         state.focusedWindow == _search || state.focusedWindow == _searchPopupEdit ||
         state.focusedWindow == _searchPopupResults))
        SetFocus(state.focusedWindow);
}

void QuickOpen::clearTreeData()
{
    if (_tree)
    {
        for (const auto& entry : _nodeData)
        {
            TVITEMW tv{};
            tv.mask = TVIF_PARAM;
            tv.hItem = entry.first;
            if (TreeView_GetItem(_tree, &tv) && tv.lParam && tv.lParam != ID_NODE_DUMMY)
                delete reinterpret_cast<NodeData*>(tv.lParam);
        }
    }
    _selectedTreeFiles.clear();
    _nodeData.clear();
}

void QuickOpen::rebuildWorkspaceTree(bool preserveExpansion)
{
    invalidateSearchIndex();
    if (!_tree) return;

    const TreeUiState previousState = preserveExpansion ? captureTreeUiState() : TreeUiState{};
    const bool resultsWereVisible = _resultsViewVisible;
    SendMessageW(_tree, WM_SETREDRAW, FALSE, 0);
    clearTreeData();
    TreeView_DeleteAllItems(_tree);

    // NPPWorkSpace is the authoritative workspace. Roots discovered in
    // Notepad++'s legacy Folder as Workspace are imported into _savedRoots
    // and the native panel is kept hidden.
    for (size_t i = 0; i < _containers.size(); ++i)
        addContainerToTree(i);

    for (const auto& root : _savedRoots)
        addRootToTree(root, false);

    if (preserveExpansion) restoreTreeUiState(previousState);

    if (resultsWereVisible)
    {
        ShowWindow(_tree, SW_HIDE);
        ShowWindow(_results, SW_SHOW);
    }
    else
    {
        ShowWindow(_results, SW_HIDE);
        ShowWindow(_tree, SW_SHOW);
    }
    updatePanelSwitcher();

    SendMessageW(_tree, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(_tree, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);

    std::unordered_set<std::wstring> remoteConnections;
    for (const auto& remote : _remoteFolders)
        remoteConnections.insert(remote.connectionId.empty() ? remote.id : remote.connectionId);
    std::wstring status = L"NPPWorkSpace  •  " + std::to_wstring(_containers.size()) + L" projeto(s)  •  " +
                          std::to_wstring(_savedRoots.size()) + L" pasta(s)  •  " +
                          std::to_wstring(remoteConnections.size()) + L" conexão(ões) SSH  •  Ctrl+B mostrar/ocultar  •  Ctrl+P pesquisar";
    if (!resultsWereVisible && !_searchQueryRunning)
        SetWindowTextW(_status, status.c_str());
}

std::wstring QuickOpen::workspaceRootDisplayName(const std::filesystem::path& root) const
{
    const size_t remoteIndex = findRemoteFolderForPath(root);
    if (remoteIndex != static_cast<size_t>(-1))
    {
        auto normalizedKey = [](const std::filesystem::path& value)
        {
            std::error_code ec;
            std::filesystem::path normalized = std::filesystem::weakly_canonical(value, ec);
            if (ec)
            {
                ec.clear();
                normalized = std::filesystem::absolute(value, ec);
                if (ec) normalized = value;
                normalized = normalized.lexically_normal();
            }
            return QuickOpen::lower(normalized.wstring());
        };

        if (normalizedKey(root) == normalizedKey(_remoteFolders[remoteIndex].localCachePath))
        {
            std::wstring remotePath = _remoteFolders[remoteIndex].remotePath;
            std::replace(remotePath.begin(), remotePath.end(), L'\\', L'/');
            while (remotePath.size() > 1 && remotePath.back() == L'/') remotePath.pop_back();
            const size_t slash = remotePath.find_last_of(L'/');
            std::wstring name = slash == std::wstring::npos
                ? remotePath : remotePath.substr(slash + 1);
            if (name.empty()) name = remotePath == L"/" ? L"root" : _remoteFolders[remoteIndex].name;
            return name;
        }
    }

    const std::wstring fileName = root.filename().wstring();
    return fileName.empty() ? root.wstring() : fileName;
}

void QuickOpen::addContainerToTree(size_t containerIndex)
{
    if (containerIndex >= _containers.size()) return;
    const auto& container = _containers[containerIndex];
    addNode(_tree, TVI_ROOT, container.name, {}, NodeType::Container, false, !container.folders.empty(), containerIndex);
    HTREEITEM item = nullptr;
    // Locate the newly inserted container by walking the top-level nodes.
    // container by walking the top-level nodes instead.
    for (HTREEITEM current = TreeView_GetRoot(_tree); current; current = TreeView_GetNextSibling(_tree, current))
    {
        auto it = _nodeData.find(current);
        if (it != _nodeData.end() && it->second.type == NodeType::Container && it->second.containerIndex == containerIndex)
        {
            item = current;
            break;
        }
    }
    if (!item || container.folders.empty()) return;
    // Replace the lazy dummy with the actual folders so containers behave like
    // regular expandable project folders.
    HTREEITEM child = TreeView_GetChild(_tree, item);
    if (child) TreeView_DeleteItem(_tree, child);
    for (const auto& folder : container.folders)
        addRootToTree(folder, false, item, containerIndex);
}

void QuickOpen::addRootToTree(const std::filesystem::path& root, bool fromNppWorkspace, HTREEITEM parent, size_t containerIndex)
{
    if (root.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return;

    const std::wstring label = workspaceRootDisplayName(root);
    addNode(_tree, parent, label, root, NodeType::Root, fromNppWorkspace, true, containerIndex);
}

void QuickOpen::addNode(HWND tree, HTREEITEM parent, const std::wstring& label,
                        const std::filesystem::path& path, NodeType type,
                        bool fromNppWorkspace, bool hasChildren, size_t containerIndex)
{
    NodeData node{};
    node.type = type;
    node.path = path;
    node.fromNppWorkspace = fromNppWorkspace;
    node.containerIndex = containerIndex;
    node.label = label;
    node.hasChildren = hasChildren;

    const std::wstring displayLabel = formatTreeNodeLabel(node, false);

    TVINSERTSTRUCTW ins{};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = const_cast<LPWSTR>(displayLabel.c_str());

    auto* data = new NodeData(node);
    ins.item.lParam = reinterpret_cast<LPARAM>(data);
    HTREEITEM item = TreeView_InsertItem(tree, &ins);
    _nodeData[item] = node;

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

std::wstring QuickOpen::formatTreeNodeLabel(const NodeData& data, bool expanded) const
{
    if (data.type == NodeType::File || !data.hasChildren)
        return data.label;
    return std::wstring(expanded ? L"[-] " : L"[+] ") + data.label;
}

void QuickOpen::refreshTreeNodeLabel(HTREEITEM item, bool expanded)
{
    if (!_tree || !item) return;
    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    std::wstring label = formatTreeNodeLabel(it->second, expanded);
    TVITEMW tv{};
    tv.mask = TVIF_TEXT;
    tv.hItem = item;
    tv.pszText = label.data();
    TreeView_SetItem(_tree, &tv);
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
            if (!isWorkspaceTemporaryFile(path)) files.push_back(entry);
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

void QuickOpen::clearTreeFileSelection()
{
    _selectedTreeFiles.clear();
    if (_tree) InvalidateRect(_tree, nullptr, TRUE);
}

bool QuickOpen::isTreeFileSelected(HTREEITEM item) const
{
    return item && _selectedTreeFiles.find(item) != _selectedTreeFiles.end();
}

void QuickOpen::toggleTreeFileSelection(HTREEITEM item, bool ctrlDown)
{
    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    if (it->second.type != NodeType::File)
    {
        if (!ctrlDown)
        {
            _selectedTreeFiles.clear();
            InvalidateRect(_tree, nullptr, TRUE);
        }
        return;
    }

    if (!ctrlDown)
        _selectedTreeFiles.clear();

    if (ctrlDown && isTreeFileSelected(item))
        _selectedTreeFiles.erase(item);
    else
        _selectedTreeFiles.insert(item);

    InvalidateRect(_tree, nullptr, TRUE);
}

void QuickOpen::openSelectedTreeFiles()
{
    std::vector<std::filesystem::path> paths;
    for (HTREEITEM item : _selectedTreeFiles)
    {
        auto it = _nodeData.find(item);
        if (it != _nodeData.end() && it->second.type == NodeType::File)
            paths.push_back(it->second.path);
    }

    // If there is no multi-selection, keep the normal single-file behavior.
    if (paths.empty())
    {
        HTREEITEM item = TreeView_GetSelection(_tree);
        auto it = _nodeData.find(item);
        if (it != _nodeData.end() && it->second.type == NodeType::File)
            paths.push_back(it->second.path);
    }

    for (const auto& path : paths)
        openFileInNotepad(path);
}

bool QuickOpen::openFileInNotepad(const std::filesystem::path& file)
{
    if (!_npp || file.empty()) return false;

    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec))
    {
        SetWindowTextW(_status, (L"Arquivo indisponível no cache: " + file.filename().wstring()).c_str());
        return false;
    }

    ec.clear();
    std::filesystem::path fullPath = std::filesystem::weakly_canonical(file, ec);
    if (ec)
    {
        ec.clear();
        fullPath = std::filesystem::absolute(file, ec);
        if (ec) fullPath = file;
        fullPath = fullPath.lexically_normal();
    }

    const std::wstring path = fullPath.wstring();
    const bool opened = SendMessageW(_npp, NPPM_DOOPEN, 0,
                                     reinterpret_cast<LPARAM>(path.c_str())) != FALSE;
    if (!opened)
        SetWindowTextW(_status, (L"Não foi possível abrir: " + fullPath.filename().wstring()).c_str());
    return opened;
}

void QuickOpen::openTreeSelection()
{
    if (!_selectedTreeFiles.empty())
    {
        openSelectedTreeFiles();
        return;
    }

    HTREEITEM item = TreeView_GetSelection(_tree);
    if (!item) return;

    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    if (it->second.type == NodeType::File)
    {
        openFileInNotepad(it->second.path);
    }
    else
    {
        expandNode(item);
        TVITEMW stateItem{};
        stateItem.mask = TVIF_STATE;
        stateItem.stateMask = TVIS_EXPANDED;
        stateItem.hItem = item;
        TreeView_GetItem(_tree, &stateItem);
        const bool expanding = (stateItem.state & TVIS_EXPANDED) == 0;
        TreeView_Expand(_tree, item, expanding ? TVE_EXPAND : TVE_COLLAPSE);
        refreshTreeNodeLabel(item, expanding);
    }
}

void QuickOpen::showResultsContextMenu(HWND list, POINT screenPoint)
{
    if (!list) return;
    const int count = ListView_GetSelectedCount(list);
    if (count <= 0) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_OPEN_SELECTED, L"Abrir arquivos selecionados");
    SetForegroundWindow(list);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, screenPoint.x, screenPoint.y, 0,
                   (list == _searchPopupResults ? _searchPopup : _window), nullptr);
    DestroyMenu(menu);
}

void QuickOpen::openSelectedResults(HWND list, bool closePopup)
{
    if (!list) return;

    std::vector<size_t> indexes;
    for (int row = -1; (row = ListView_GetNextItem(list, row, LVNI_SELECTED)) >= 0; )
    {
        if (row >= 0 && row < static_cast<int>(_searchResults.size()))
            indexes.push_back(static_cast<size_t>(row));
    }

    for (size_t index : indexes)
    {
        openSearchResultRow(_searchResults[index]);
    }

    if (closePopup) hideSearchPopup();
}

void QuickOpen::openSearchResultRow(const SearchResult& result)
{
    if (result.rowType == SearchResult::RowType::ContentLine)
    {
        openFileAtOccurrence(result);
        return;
    }

    openFileInNotepad(result.path);
}

void QuickOpen::openFileAtOccurrence(const SearchResult& result)
{
    if (!openFileInNotepad(result.path)) return;

    HWND scintilla = NPPWorkSpace_GetCurrentScintillaHandle();
    if (!scintilla || result.lineNumber == 0) return;

    const WPARAM line = static_cast<WPARAM>(result.lineNumber - 1);
    const LRESULT lineStart = SendMessageW(scintilla, SCI_POSITIONFROMLINE, line, 0);
    const LRESULT lineEnd = SendMessageW(scintilla, SCI_GETLINEENDPOSITION, line, 0);
    if (lineStart < 0 || lineEnd < lineStart) return;

    SendMessageW(scintilla, SCI_SETVISIBLEPOLICY, CARET_EVEN | CARET_JUMPS, 0);
    SendMessageW(scintilla, SCI_ENSUREVISIBLEENFORCEPOLICY, line, 0);
    SendMessageW(scintilla, SCI_SETSEARCHFLAGS, 0, 0);

    auto bytesFromWide = [](const std::wstring& value, UINT codePage)
    {
        std::string bytes;
        if (value.empty()) return bytes;
        int needed = WideCharToMultiByte(codePage, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return bytes;
        bytes.assign(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(codePage, 0, value.data(), static_cast<int>(value.size()),
                            bytes.data(), needed, nullptr, nullptr);
        return bytes;
    };

    auto findOccurrence = [&](const std::string& needle, LRESULT& foundStart, LRESULT& foundEnd)
    {
        if (needle.empty()) return false;
        LRESULT searchStart = lineStart;
        for (size_t occurrence = 0; occurrence <= result.occurrenceOnLine; ++occurrence)
        {
            SendMessageW(scintilla, SCI_SETTARGETSTART, static_cast<WPARAM>(searchStart), 0);
            SendMessageW(scintilla, SCI_SETTARGETEND, static_cast<WPARAM>(lineEnd), 0);
            const LRESULT pos = SendMessageA(scintilla, SCI_SEARCHINTARGET,
                                             static_cast<WPARAM>(needle.size()),
                                             reinterpret_cast<LPARAM>(needle.data()));
            if (pos < 0) return false;
            foundStart = pos;
            foundEnd = SendMessageW(scintilla, SCI_GETTARGETEND, 0, 0);
            searchStart = foundEnd > pos ? foundEnd : pos + 1;
        }
        return foundStart >= 0 && foundEnd >= foundStart;
    };

    LRESULT foundStart = -1;
    LRESULT foundEnd = -1;
    const std::wstring needle = result.matchText.empty() ? result.snippet : result.matchText;
    const UINT scintillaCodePage = static_cast<UINT>(SendMessageW(scintilla, SCI_GETCODEPAGE, 0, 0));

    std::vector<std::string> candidates;
    candidates.push_back(utf8FromWide(needle));
    if (scintillaCodePage != CP_UTF8)
    {
        std::string cpBytes = bytesFromWide(needle, scintillaCodePage ? scintillaCodePage : CP_ACP);
        if (!cpBytes.empty() && cpBytes != candidates.front())
            candidates.push_back(std::move(cpBytes));
    }
    std::string acpBytes = bytesFromWide(needle, CP_ACP);
    if (!acpBytes.empty() && std::find(candidates.begin(), candidates.end(), acpBytes) == candidates.end())
        candidates.push_back(std::move(acpBytes));

    for (const auto& candidate : candidates)
    {
        if (findOccurrence(candidate, foundStart, foundEnd))
            break;
    }

    if (foundStart >= 0 && foundEnd >= foundStart)
    {
        SendMessageW(scintilla, SCI_GOTOPOS, static_cast<WPARAM>(foundEnd), 0);
        SendMessageW(scintilla, SCI_SETANCHOR, static_cast<WPARAM>(foundStart), 0);
    }
    else
    {
        SendMessageW(scintilla, SCI_GOTOPOS, static_cast<WPARAM>(lineStart), 0);
        SendMessageW(scintilla, SCI_SETANCHOR, static_cast<WPARAM>(lineStart), 0);
    }
    SendMessageW(scintilla, SCI_SCROLLCARET, 0, 0);
}

LRESULT QuickOpen::handleResultsCustomDraw(HWND list, NMLVCUSTOMDRAW* customDraw)
{
    if (!customDraw) return CDRF_DODEFAULT;

    const DWORD stage = customDraw->nmcd.dwDrawStage;
    if (stage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (stage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
    if (stage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
    {
        const int row = static_cast<int>(customDraw->nmcd.dwItemSpec);
        if (row >= 0 && row < static_cast<int>(_searchResults.size()))
        {
            const auto& result = _searchResults[static_cast<size_t>(row)];
            if (result.rowType == SearchResult::RowType::ContentLine && customDraw->iSubItem == 2)
            {
                drawHighlightedSnippet(list, customDraw, result);
                return CDRF_SKIPDEFAULT;
            }
        }
    }
    return CDRF_DODEFAULT;
}

void QuickOpen::drawHighlightedSnippet(HWND list, NMLVCUSTOMDRAW* customDraw, const SearchResult& result)
{
    const int row = static_cast<int>(customDraw->nmcd.dwItemSpec);
    RECT rc{};
    if (!ListView_GetSubItemRect(list, row, 2, LVIR_BOUNDS, &rc)) return;

    HDC dc = customDraw->nmcd.hdc;
    const bool selected = (ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const COLORREF background = selected ? GetSysColor(COLOR_HIGHLIGHT) : ListView_GetBkColor(list);
    const COLORREF textColor = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : ListView_GetTextColor(list);
    const COLORREF matchBackground = _darkMode ? RGB(92, 76, 0) : RGB(255, 236, 120);
    const COLORREF matchText = _darkMode ? RGB(255, 255, 255) : RGB(0, 0, 0);

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(dc, &rc, backgroundBrush);
    DeleteObject(backgroundBrush);

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, _font);

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const int y = rc.top + ((rc.bottom - rc.top) - tm.tmHeight) / 2;
    int x = rc.left + 4;

    const size_t safeStart = (std::min)(result.matchStart, result.snippet.size());
    const size_t safeLength = (std::min)(result.matchLength, result.snippet.size() - safeStart);
    const std::wstring prefix = result.snippet.substr(0, safeStart);
    const std::wstring match = result.snippet.substr(safeStart, safeLength);
    const std::wstring suffix = result.snippet.substr(safeStart + safeLength);

    auto textWidth = [&](const std::wstring& text)
    {
        SIZE size{};
        if (!text.empty()) GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
        return size.cx;
    };

    auto drawText = [&](const std::wstring& text, COLORREF color)
    {
        if (text.empty()) return;
        SetTextColor(dc, color);
        ExtTextOutW(dc, x, y, ETO_CLIPPED, &rc, text.c_str(), static_cast<UINT>(text.size()), nullptr);
        x += textWidth(text);
    };

    drawText(prefix, textColor);
    if (!match.empty())
    {
        const int width = textWidth(match);
        RECT matchRect{x, rc.top + 2, (std::min)(x + width, static_cast<int>(rc.right)), rc.bottom - 2};
        HBRUSH matchBrush = CreateSolidBrush(matchBackground);
        FillRect(dc, &matchRect, matchBrush);
        DeleteObject(matchBrush);
        drawText(match, matchText);
    }
    drawText(suffix, textColor);

    RestoreDC(dc, saved);
}

void QuickOpen::showTreeContextMenu(HTREEITEM item, POINT screenPoint)
{
    auto it = _nodeData.find(item);
    const bool hasItem = it != _nodeData.end();
    const bool selectedFiles = !_selectedTreeFiles.empty();
    const bool isContainer = hasItem && it->second.type == NodeType::Container;
    const bool isRoot = hasItem && it->second.type == NodeType::Root && !it->second.fromNppWorkspace;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (selectedFiles)
    {
        AppendMenuW(menu, MF_STRING, ID_OPEN_SELECTED, L"Abrir arquivos selecionados");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    if (isContainer)
    {
        AppendMenuW(menu, MF_STRING, ID_ADD, L"Adicionar pasta ao projeto");
        AppendMenuW(menu, MF_STRING, ID_REMOVE, L"Excluir contêiner");
        AppendMenuW(menu, MF_STRING, ID_CREATE_CONTAINER + 1, L"Renomear contêiner");
        AppendMenuW(menu, MF_STRING, ID_CONTAINER_COLOR, L"Alterar cor do contêiner...");
    }
    else if (isRoot)
    {
        AppendMenuW(menu, MF_STRING, ID_CREATE_CONTAINER + 2, L"Mover pasta para contêiner");
        AppendMenuW(menu, MF_STRING, ID_REMOVE, L"Remover pasta do NPPWorkSpace");
    }

    AppendMenuW(menu, MF_STRING, ID_REFRESH_WORKSPACE,
                findRemoteFolderForPath(hasItem ? it->second.path : std::filesystem::path{}) != static_cast<size_t>(-1)
                    ? L"Sincronizar com o Ubuntu"
                    : L"Atualizar workspace");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_CREATE_CONTAINER, L"Criar contêiner");

    SetForegroundWindow(_window);
    const UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD,
                                        screenPoint.x, screenPoint.y, 0, _window, nullptr);
    DestroyMenu(menu);

    if (command == ID_CREATE_CONTAINER) { createContainer(screenPoint); return; }
    if (command == ID_REFRESH_WORKSPACE) { refreshSelectedWorkspaceNode(); return; }
    if (command == ID_ADD && isContainer) { addFolder(); return; }
    if (command == ID_REMOVE && (isContainer || isRoot)) { removeSelectedRoot(); return; }
    if (command == ID_CREATE_CONTAINER + 1 && isContainer) { renameContainer(it->second.containerIndex); return; }
    if (command == ID_CONTAINER_COLOR && isContainer) { colorContainer(it->second.containerIndex); return; }
    if (command == ID_CREATE_CONTAINER + 2 && isRoot)
    {
        HMENU sub = CreatePopupMenu();
        if (!sub) return;
        for (size_t i = 0; i < _containers.size(); ++i)
            AppendMenuW(sub, MF_STRING, 3000 + static_cast<UINT>(i), _containers[i].name.c_str());
        if (!_containers.empty())
        {
            const UINT selectedCommand = TrackPopupMenu(sub, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD,
                                                        screenPoint.x + 8, screenPoint.y + 8, 0, _window, nullptr);
            if (selectedCommand >= 3000 && selectedCommand < 3000 + _containers.size())
                moveFolderToContainer(it->second.path, selectedCommand - 3000);
        }
        else
        {
            MessageBoxW(_window, L"Crie um contêiner primeiro para organizar esta pasta.", L"NPPWorkSpace", MB_ICONINFORMATION);
        }
        DestroyMenu(sub);
        return;
    }
    if (command == ID_OPEN_SELECTED) openSelectedTreeFiles();
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
                refreshTreeNodeLabel(current, true);
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
                refreshTreeNodeLabel(current, false);
            }
        }
    };
    collapse(TreeView_GetRoot(_tree));
}

void QuickOpen::expandAllContentGroups()
{
    if (!_results || _contentSearchGroups.empty()) return;
    for (auto& group : _contentSearchGroups)
        group.collapsed = false;

    SendMessageW(_results, WM_SETREDRAW, FALSE, 0);
    rebuildContentResultsList(_results);
    SendMessageW(_results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(_results, nullptr, FALSE);
}

void QuickOpen::collapseAllContentGroups()
{
    if (!_results || _contentSearchGroups.empty()) return;
    for (auto& group : _contentSearchGroups)
        group.collapsed = true;

    SendMessageW(_results, WM_SETREDRAW, FALSE, 0);
    rebuildContentResultsList(_results);
    SendMessageW(_results, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(_results, nullptr, FALSE);
}

void QuickOpen::handleTreeDoubleClick(LPNMTREEVIEWW /*tv*/)
{
    if (!_tree) return;

    // NM_DBLCLK for a TreeView only guarantees an NMHDR. Treating lParam as an
    // NMTREEVIEW made itemNew contain garbage and prevented cached SSH files
    // from opening. Resolve the actual row from the cursor, then fall back to
    // the native selection for keyboard/accessibility generated activation.
    POINT point{};
    GetCursorPos(&point);
    ScreenToClient(_tree, &point);
    TVHITTESTINFO hit{};
    hit.pt = point;
    HTREEITEM item = TreeView_HitTest(_tree, &hit);
    if (!item || !(hit.flags & TVHT_ONITEM)) item = TreeView_GetSelection(_tree);
    if (!item) return;

    TreeView_SelectItem(_tree, item);
    auto it = _nodeData.find(item);
    if (it == _nodeData.end()) return;

    // A double-click on any selected file opens the whole current selection.
    // If the clicked file is not selected, make it the sole selection first.
    if (it->second.type == NodeType::File)
    {
        if (!isTreeFileSelected(item))
        {
            _selectedTreeFiles.clear();
            _selectedTreeFiles.insert(item);
            InvalidateRect(_tree, nullptr, TRUE);
        }
        openSelectedTreeFiles();
        return;
    }

    // The native TreeView performs the folder expand/collapse itself on a
    // double-click. Only make sure lazy children exist before that happens.
    expandNode(item);
}

void QuickOpen::refreshSelectedWorkspaceNode()
{
    if (!_tree) return;

    std::unordered_set<size_t> remoteIndexes;
    const HTREEITEM selected = TreeView_GetSelection(_tree);
    const auto selectedNode = _nodeData.find(selected);

    if (selectedNode != _nodeData.end())
    {
        const size_t directRemote = findRemoteFolderForPath(selectedNode->second.path);
        if (directRemote != static_cast<size_t>(-1))
        {
            remoteIndexes.insert(directRemote);
        }
        else if (selectedNode->second.type == NodeType::Container &&
                 selectedNode->second.containerIndex < _containers.size())
        {
            for (const auto& folder : _containers[selectedNode->second.containerIndex].folders)
            {
                const size_t index = findRemoteFolderForPath(folder);
                if (index != static_cast<size_t>(-1)) remoteIndexes.insert(index);
            }
        }
    }

    // F5 with no remote selection refreshes every remote root in the current
    // workspace. Local folders are refreshed immediately from disk.
    if (remoteIndexes.empty() && selectedNode == _nodeData.end())
        for (size_t i = 0; i < _remoteFolders.size(); ++i) remoteIndexes.insert(i);

    if (!remoteIndexes.empty())
    {
        // Keep the current tree stable while the remote transfer is running.
        // The DownloadTree completion performs one atomic state-preserving
        // rebuild after the cache is complete, avoiding two refreshes/flicker.
        for (const size_t index : remoteIndexes)
            queueRemoteSync(index);
        return;
    }

    rebuildWorkspaceTree(true);
    startSearchIndexBuild();
    SetWindowTextW(_status, L"Workspace atualizado  •  F5 atualizar novamente");
}

void QuickOpen::handleTreeItemExpanding(LPNMTREEVIEWW tv)
{
    if (!tv) return;
    if (tv->action == TVE_EXPAND)
    {
        expandNode(tv->itemNew.hItem);
        refreshTreeNodeLabel(tv->itemNew.hItem, true);
    }
    else if (tv->action == TVE_COLLAPSE)
    {
        refreshTreeNodeLabel(tv->itemNew.hItem, false);
    }
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


int QuickOpen::fuzzyScoreLower(const std::wstring& q, const std::wstring& c)
{
    if (q.empty()) return 1;
    size_t pos = 0;
    int score = 0;
    int last = -1;
    for (wchar_t ch : q)
    {
        const size_t found = c.find(ch, pos);
        if (found == std::wstring::npos) return -1;
        score += 100;
        if (last >= 0)
            score += (static_cast<int>(found) == last + 1) ? 50 : -(std::min)(30, static_cast<int>(found) - last);
        if (found == 0 || c[found - 1] == L'\\' || c[found - 1] == L'/' ||
            c[found - 1] == L'_' || c[found - 1] == L'-')
            score += 40;
        last = static_cast<int>(found);
        pos = found + 1;
    }
    score -= static_cast<int>(c.size() - q.size());
    return (std::max)(0, (std::min)(1000000, score));
}


bool QuickOpen::isSearchPathEnabled(const std::filesystem::path& path) const
{
    return isPathEnabledForScope(path, _searchIncludedPaths, _searchDisabledPaths);
}

void QuickOpen::invalidateSearchIndex()
{
    _searchIndexValid = false;
    _searchIndex.clear();
    _searchIndexSnapshot.reset();
    _searchIndexGeneration.fetch_add(1);
    _searchQueryGeneration.fetch_add(1);
}

void QuickOpen::startSearchIndexBuild()
{
    if (_searchIndexBuilding || !_window) return;
    if (_searchIndexThread.joinable()) _searchIndexThread.join();

    const unsigned int generation = _searchIndexGeneration.load();
    const auto roots = getWorkspaceRootsForPanel();
    _searchIndexBuilding = true;
    _searchIndexValid = false;

    _searchIndexThread = std::thread([this, roots, generation]()
    {
        auto* built = new std::vector<SearchResult>();
        constexpr size_t MAX_INDEX_FILES_PER_ROOT = 50000;
        std::unordered_set<std::wstring> seenFiles;
        for (const auto& requestedRoot : roots)
        {
            if (_searchIndexGeneration.load() != generation) break;
            enumerateWorkspaceFiles(
                requestedRoot,
                MAX_INDEX_FILES_PER_ROOT,
                [&](const std::filesystem::path& root, const std::filesystem::path& p)
                {
                    if (_searchIndexGeneration.load() != generation) return false;
                    if (isWorkspaceTemporaryFile(p)) return true;

                    const std::wstring key = normalizeScopePath(p.lexically_normal().wstring());
                    if (!seenFiles.insert(key).second) return true;

                    std::wstring file = p.filename().wstring();
                    if (file.empty()) return true;
                    const auto rel = workspaceRelativePath(p, root);
                    auto folderPath = rel.parent_path();
                    std::wstring folder = folderPath.empty() ? L"\\" : L"\\" + folderPath.wstring();
                    std::replace(folder.begin(), folder.end(), L'/', L'\\');
                    const std::wstring relative = rel.wstring();
                    built->push_back({p, file, folder, relative, lower(file), lower(relative)});
                    return true;
                },
                [&]() { return _searchIndexGeneration.load() != generation; });
        }

        if (_searchIndexGeneration.load() != generation || !_window)
        {
            delete built;
            if (_window)
            {
                if (!PostMessageW(_window, WM_SEARCH_INDEX_READY,
                                  static_cast<WPARAM>(generation), 0))
                    _searchIndexBuilding = false;
            }
            else
            {
                _searchIndexBuilding = false;
            }
            return;
        }
        if (!PostMessageW(_window, WM_SEARCH_INDEX_READY, static_cast<WPARAM>(generation),
                          reinterpret_cast<LPARAM>(built)))
        {
            delete built;
            _searchIndexBuilding = false;
        }
    });
}

void QuickOpen::finishSearchIndexBuild(std::vector<SearchResult>* built, unsigned int generation)
{
    std::unique_ptr<std::vector<SearchResult>> holder(built);
    _searchIndexBuilding = false;
    if (generation != _searchIndexGeneration.load())
    {
        // A remote synchronization or workspace refresh invalidated this build
        // while it was running. Start a fresh pass immediately instead of
        // waiting for the user to change scope or reopen Ctrl+P.
        if (_window) startSearchIndexBuild();
        return;
    }
    if (!holder)
    {
        if (_window && !_searchIndexValid) startSearchIndexBuild();
        return;
    }
    _searchIndex = std::move(*holder);
    _searchIndexSnapshot = std::make_shared<const std::vector<SearchResult>>(_searchIndex);
    _searchIndexValid = true;
    if (_runSearchAfterIndexBuild)
    {
        const bool popup = _runSearchAfterIndexPopup;
        _runSearchAfterIndexBuild = false;
        _runSearchAfterIndexPopup = false;
        if (popup) updatePopupSearch();
        else updateSearch();
    }
}

void QuickOpen::showSearchScopeMenu(HWND owner, POINT screenPoint)
{
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    _scopeMenuFolders.clear();
    _scopeMenuContainers.clear();
    _nextScopeMenuId = 50000;

    AppendMenuW(menu, MF_STRING | ((_searchIncludedPaths.empty() && _searchDisabledPaths.empty()) ? MF_CHECKED : 0),
                49999, L"Pesquisar em tudo");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    auto addFolderItem = [&](const std::filesystem::path& folder, const std::wstring& label)
    {
        if (_nextScopeMenuId >= 59900) return;
        const UINT id = _nextScopeMenuId++;
        _scopeMenuFolders[id] = folder;
        const bool enabled = isPathEnabledForScope(folder, _searchIncludedPaths, _searchDisabledPaths);
        AppendMenuW(menu, MF_STRING | (enabled ? MF_CHECKED : MF_UNCHECKED), id, label.c_str());
    };

    for (size_t i = 0; i < _containers.size(); ++i)
    {
        const auto& c = _containers[i];
        HMENU sub = CreatePopupMenu();
        if (!sub) continue;
        size_t enabledCount = 0;
        for (const auto& folder : c.folders)
        {
            if (isPathEnabledForScope(folder, _searchIncludedPaths, _searchDisabledPaths)) ++enabledCount;
            addFolderItem(folder, workspaceRootDisplayName(folder));
            // Move the last generated item from the main menu into this submenu.
            const UINT id = _nextScopeMenuId - 1;
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_ID | MIIM_STATE | MIIM_STRING;
            wchar_t text[1024]{};
            GetMenuStringW(menu, id, text, static_cast<int>(std::size(text)), MF_BYCOMMAND);
            mi.wID = id;
            mi.fState = isPathEnabledForScope(folder, _searchIncludedPaths, _searchDisabledPaths) ? MFS_CHECKED : MFS_UNCHECKED;
            mi.dwTypeData = text;
            DeleteMenu(menu, id, MF_BYCOMMAND);
            InsertMenuItemW(sub, 0xFFFFFFFF, TRUE, &mi);
        }
        const UINT cid = _nextScopeMenuId++;
        _scopeMenuContainers[cid] = i;
        const bool allEnabled = !c.folders.empty() && enabledCount == c.folders.size();
        InsertMenuW(sub, 0, MF_BYPOSITION | MF_STRING | (allEnabled ? MF_CHECKED : MF_UNCHECKED), cid, L"Pesquisar neste contêiner");
        InsertMenuW(sub, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), c.name.c_str());
    }

    for (const auto& root : _savedRoots)
        addFolderItem(root, workspaceRootDisplayName(root));

    // The menu above needs command handling by the owner window. Track the
    // menu while it is open and dispatch the selected id through WM_COMMAND.
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, owner, nullptr);
    if (cmd == 49999)
    {
        applySearchScopeCommand(49999);
    }
    else if (cmd >= 50000 && cmd < 60000)
    {
        applySearchScopeCommand(static_cast<UINT>(cmd));
    }
    DestroyMenu(menu);
    _scopeMenuFolders.clear();
    _scopeMenuContainers.clear();
}

void QuickOpen::applySearchScopeCommand(UINT id)
{
    if (id == 49999) { _searchIncludedPaths.clear(); _searchDisabledPaths.clear(); }
    else if (auto it = _scopeMenuFolders.find(id); it != _scopeMenuFolders.end())
    {
        const std::wstring key = it->second.wstring();
        if (!_searchIncludedPaths.empty())
        {
            if (!_searchIncludedPaths.insert(key).second) _searchIncludedPaths.erase(key);
        }
        else
        {
            if (!_searchDisabledPaths.insert(key).second) _searchDisabledPaths.erase(key);
        }
    }
    else if (auto containerIt = _scopeMenuContainers.find(id); containerIt != _scopeMenuContainers.end())
    {
        if (containerIt->second < _containers.size())
        {
            const auto& c = _containers[containerIt->second];
            _searchIncludedPaths.clear();
            _searchDisabledPaths.clear();
            for (const auto& folder : c.folders)
                _searchIncludedPaths.insert(folder.wstring());
        }
    }
    // Changing scope only changes filtering; the file index itself remains
    // valid. Rebuilding a large workspace index here made every checkbox
    // click stall the UI and caused unnecessary disk traversal.
    _searchQueryGeneration.fetch_add(1);
    writeWorkspaceFile();

    // Apply the new scope immediately to an existing query. Previously the
    // checkbox changed correctly but the old result list remained visible
    // until Enter/Search was pressed again, which made SSH scopes look stale.
    const bool popupVisible = _searchPopup && IsWindowVisible(_searchPopup);
    if (popupVisible && _searchPopupEdit && GetWindowTextLengthW(_searchPopupEdit) > 0)
        updatePopupSearch();
    else if (!popupVisible && _search && GetWindowTextLengthW(_search) > 0)
        updateSearch();
    else
        markSearchPending(popupVisible);
}

void QuickOpen::searchDirectory(const std::filesystem::path& requestedRoot,
                                const std::wstring& query,
                                std::vector<SearchResult>& results,
                                size_t limit) const
{
    enumerateWorkspaceFiles(
        requestedRoot,
        SEARCH_LIMIT_PER_ROOT,
        [&](const std::filesystem::path& root, const std::filesystem::path& p)
        {
            if (results.size() >= limit) return false;
            if (isWorkspaceTemporaryFile(p)) return true;

            std::wstring file = p.filename().wstring();
            if (file.empty()) file = p.stem().wstring();
            if (file.empty()) file = p.wstring();

            const std::filesystem::path relPath = workspaceRelativePath(p, root);
            const std::wstring rel = relPath.wstring();
            std::filesystem::path folderPath = relPath.parent_path();
            std::wstring folder = folderPath.empty() ? L"\\" : L"\\" + folderPath.wstring();
            std::replace(folder.begin(), folder.end(), L'/', L'\\');
            const bool match = fuzzyScore(query, file) >= 0 || fuzzyScore(query, rel) >= 0;
            if (match) results.push_back({p, file, folder, rel});
            return results.size() < limit;
        });
}

void QuickOpen::markSearchPending(bool popup)
{
    _pendingContentQuery.clear();
    _pendingContentPopup = false;
    _runSearchAfterIndexBuild = false;
    _runSearchAfterIndexPopup = false;
    if (_searchQueryRunning)
    {
        _searchQueryGeneration.fetch_add(1);
        EnableWindow(_cancelSearch, FALSE);
    }

    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pressione Pesquisar ou Enter");
    if (popup)
    {
        if (_searchPopupHint) SetWindowTextW(_searchPopupHint, L"Enter pesquisar  |  Na lista: Enter abrir  |  Esc fechar");
    }
    else if (_searchOnly)
    {
        SetWindowTextW(_status, L"Pressione Pesquisar ou Enter para buscar");
    }
}

void QuickOpen::updateSearch()
{
    if (!_search || _suppressSearch) return;
    wchar_t buffer[2048]{};
    GetWindowTextW(_search, buffer, static_cast<int>(std::size(buffer)));
    const std::wstring query(buffer);

    if (query.empty())
    {
        if (!_contentSearchPopup) cancelContentSearch();
        _searchOnly = false;
        _searchResults.clear();
        _contentSearchGroups.clear();
        _contentSearchGroupByPath.clear();
        ListView_DeleteAllItems(_results);
        configureResultsColumns(_results, false);
        showFoldersPanel();
        if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
        if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pronto");
        return;
    }

    showSearchResults(query);
}

void QuickOpen::showSearchResults(const std::wstring& query)
{
    if (isContentSearchEnabled(false))
    {
        _searchOnly = true;
        showResultsPanel();
        resetContentSearchResults(false);
        if (query.size() < 2)
        {
            cancelContentSearch();
            SetWindowTextW(_status, L"Digite ao menos 2 caracteres para pesquisar dentro dos arquivos");
            if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Aguardando termo de pesquisa");
            return;
        }
        startContentSearch(query, false);
        SetWindowTextW(_status, L"Pesquisando conteúdo...");
        return;
    }

    cancelContentSearch();
    configureResultsColumns(_results, false);

    if (!_searchIndexValid)
    {
        if (!_searchIndexBuilding) startSearchIndexBuild();
        _runSearchAfterIndexBuild = true;
        _runSearchAfterIndexPopup = false;
        _searchOnly = true;
        showResultsPanel();
        SetWindowTextW(_status, L"Indexando arquivos... aguarde");
        return;
    }

    const std::wstring q = lower(query);
    std::vector<SearchResult> found;
    found.reserve(MAX_SEARCH_RESULTS);
    std::vector<std::pair<int, size_t>> ranked;
    ranked.reserve(MAX_SEARCH_RESULTS * 2);

    for (size_t i = 0; i < _searchIndex.size(); ++i)
    {
        const auto& item = _searchIndex[i];
        if (!isSearchPathEnabled(item.path)) continue;

        const int fileScore = fuzzyScoreLower(q, item.fileNameLower);
        const int pathScore = fuzzyScoreLower(q, item.relativeLower);
        const int score = (std::max)(fileScore, pathScore);
        if (score >= 0)
        {
            ranked.emplace_back(score, i);
        }
    }

    std::partial_sort(ranked.begin(), ranked.end(), ranked.begin() +
                      (std::min)(ranked.size(), static_cast<size_t>(MAX_SEARCH_RESULTS)),
        [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return a.second < b.second;
        });

    const size_t count = (std::min)(ranked.size(), static_cast<size_t>(MAX_SEARCH_RESULTS));
    found.reserve(count);
    for (size_t i = 0; i < count; ++i)
        found.push_back(_searchIndex[ranked[i].second]);

    _searchResults = std::move(found);
    ListView_DeleteAllItems(_results);
    for (size_t i = 0; i < _searchResults.size(); ++i)
    {
        LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(_searchResults[i].fileName.c_str());
        ListView_InsertItem(_results, &item);
        ListView_SetItemText(_results, static_cast<int>(i), 1, const_cast<LPWSTR>(_searchResults[i].folder.c_str()));
        const std::wstring fullPath = _searchResults[i].path.wstring();
        ListView_SetItemText(_results, static_cast<int>(i), 2, const_cast<LPWSTR>(fullPath.c_str()));
    }
    _searchOnly = true;
    showResultsPanel();
    if (!_searchResults.empty())
        ListView_SetItemState(_results, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SetWindowTextW(_status, (std::to_wstring(_searchResults.size()) + L" resultado(s)  •  Enter abrir  •  Esc limpar pesquisa").c_str());
}

void QuickOpen::clearSearchResults()
{
    cancelContentSearch();
    _suppressSearch = true;
    SetWindowTextW(_search, L"");
    _suppressSearch = false;
    _searchResults.clear();
    _contentSearchGroups.clear();
    _contentSearchGroupByPath.clear();
    configureResultsColumns(_results, false);
    ListView_DeleteAllItems(_results);
    _searchOnly = false;
    showFoldersPanel();
    if (_searchProgress) SendMessageW(_searchProgress, PBM_SETPOS, 0, 0);
    if (_searchProgressText) SetWindowTextW(_searchProgressText, L"Pronto");
}

void QuickOpen::openSearchResult()
{
    const int row = ListView_GetNextItem(_results, -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(_searchResults.size())) return;
    openSearchResultRow(_searchResults[static_cast<size_t>(row)]);
}

void QuickOpen::createContainer(POINT screenPoint)
{
    if (screenPoint.x < 0 || screenPoint.y < 0) GetCursorPos(&screenPoint);
    std::wstring name = L"Novo Projeto";
    if (!promptForText(_window, _npp, _font, _titleFont, L"Criar contêiner", L"Nome do projeto:", name, screenPoint)) return;

    // Keep names unique and deterministic.
    const std::wstring base = name;
    int suffix = 2;
    while (std::any_of(_containers.begin(), _containers.end(), [&](const WorkspaceContainer& c)
    {
        return _wcsicmp(c.name.c_str(), name.c_str()) == 0;
    }))
    {
        name = base + L" " + std::to_wstring(suffix++);
    }

    _containers.push_back({name, {}, defaultContainerColor(_containers.size())});
    writeWorkspaceFile();
    rebuildWorkspaceTree(true);
}

void QuickOpen::renameContainer(size_t index)
{
    if (index >= _containers.size()) return;
    std::wstring name = _containers[index].name;
    POINT renamePoint{}; GetCursorPos(&renamePoint);
    if (!promptForText(_window, _npp, _font, _titleFont, L"Renomear contêiner", L"Nome do projeto:", name, renamePoint)) return;
    if (name.empty()) return;

    for (size_t i = 0; i < _containers.size(); ++i)
    {
        if (i != index && _wcsicmp(_containers[i].name.c_str(), name.c_str()) == 0)
        {
            MessageBoxW(_window, L"Já existe um projeto com esse nome.", L"NPPWorkSpace", MB_ICONWARNING);
            return;
        }
    }
    _containers[index].name = name;
    writeWorkspaceFile();
    rebuildWorkspaceTree(true);
}

void QuickOpen::colorContainer(size_t index)
{
    if (index >= _containers.size()) return;

    static COLORREF customColors[16] = {
        RGB(86, 156, 214), RGB(78, 201, 176), RGB(206, 145, 120), RGB(220, 220, 170),
        RGB(197, 134, 192), RGB(181, 206, 168), RGB(244, 166, 88), RGB(114, 159, 207),
        RGB(156, 220, 254), RGB(215, 186, 125), RGB(198, 120, 221), RGB(97, 175, 239),
        RGB(152, 195, 121), RGB(224, 108, 117), RGB(229, 192, 123), RGB(86, 182, 194)
    };

    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = _window;
    cc.lpCustColors = customColors;
    cc.rgbResult = _containers[index].color == CLR_INVALID
        ? defaultContainerColor(index)
        : _containers[index].color;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;

    _containers[index].color = cc.rgbResult;
    writeWorkspaceFile();
    InvalidateRect(_tree, nullptr, FALSE);
}

void QuickOpen::removeContainer(size_t index)
{
    if (index >= _containers.size()) return;
    const std::wstring question = L"Excluir o contêiner \"" + _containers[index].name +
                                  L"\"? As pastas serão mantidas na Workspace, fora do projeto.";
    if (MessageBoxW(_window, question.c_str(), L"Excluir contêiner", MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;

    for (const auto& folder : _containers[index].folders)
    {
        if (std::find(_savedRoots.begin(), _savedRoots.end(), folder) == _savedRoots.end())
            _savedRoots.push_back(folder);
    }
    _containers.erase(_containers.begin() + static_cast<std::ptrdiff_t>(index));
    writeWorkspaceFile();
    rebuildWorkspaceTree(true);
}

void QuickOpen::moveFolderToContainer(const std::filesystem::path& folder, size_t containerIndex)
{
    if (containerIndex >= _containers.size() || folder.empty()) return;

    for (auto& container : _containers)
        container.folders.erase(std::remove(container.folders.begin(), container.folders.end(), folder), container.folders.end());
    _savedRoots.erase(std::remove(_savedRoots.begin(), _savedRoots.end(), folder), _savedRoots.end());

    auto& target = _containers[containerIndex].folders;
    if (std::find(target.begin(), target.end(), folder) == target.end())
        target.push_back(folder);

    writeWorkspaceFile();
    rebuildWorkspaceTree(true);
}

void QuickOpen::addFolder()
{
    std::filesystem::path folder;
    if (!chooseFolder(_window, folder)) return;

    HTREEITEM selected = TreeView_GetSelection(_tree);
    auto node = _nodeData.find(selected);
    if (node != _nodeData.end() && node->second.type == NodeType::Container &&
        node->second.containerIndex < _containers.size())
    {
        moveFolderToContainer(folder, node->second.containerIndex);
        return;
    }

    if (std::find(_savedRoots.begin(), _savedRoots.end(), folder) == _savedRoots.end())
        _savedRoots.push_back(folder);

    writeWorkspaceFile();
    rebuildWorkspaceTree(true);
}


std::filesystem::path QuickOpen::getRemoteCacheBase() const
{
    wchar_t localAppData[MAX_PATH * 4]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr,
                                   SHGFP_TYPE_CURRENT, localAppData)))
        return std::filesystem::path(localAppData) / L"NPPWorkSpace" / L"Remote";
    return std::filesystem::temp_directory_path() / L"NPPWorkSpace" / L"Remote";
}

std::wstring QuickOpen::createRemoteId() const
{
    GUID guid{};
    if (SUCCEEDED(CoCreateGuid(&guid)))
    {
        wchar_t text[64]{};
        StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
        std::wstring id(text);
        id.erase(std::remove(id.begin(), id.end(), L'{'), id.end());
        id.erase(std::remove(id.begin(), id.end(), L'}'), id.end());
        return id;
    }
    return std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(GetCurrentProcessId());
}

size_t QuickOpen::findRemoteFolderForPath(const std::filesystem::path& path) const
{
    if (path.empty()) return static_cast<size_t>(-1);
    std::error_code ec;
    std::filesystem::path normalizedPath = std::filesystem::weakly_canonical(path, ec);
    if (ec) normalizedPath = std::filesystem::absolute(path, ec).lexically_normal();
    std::wstring file = lower(normalizedPath.wstring());

    for (size_t i = 0; i < _remoteFolders.size(); ++i)
    {
        ec.clear();
        std::filesystem::path normalizedRoot = std::filesystem::weakly_canonical(
            _remoteFolders[i].localCachePath, ec);
        if (ec) normalizedRoot = std::filesystem::absolute(_remoteFolders[i].localCachePath, ec).lexically_normal();
        std::wstring root = lower(normalizedRoot.wstring());
        while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (file.size() < root.size() || file.compare(0, root.size(), root) != 0) continue;
        if (file.size() == root.size() || file[root.size()] == L'\\' || file[root.size()] == L'/')
            return i;
    }
    return static_cast<size_t>(-1);
}

void QuickOpen::showRemoteSshDialog()
{
    RemoteDialogState state;
    state.owner = _window;
    state.npp = _npp;
    state.font = _font;
    state.containerNames.reserve(_containers.size());
    for (const auto& container : _containers) state.containerNames.push_back(container.name);
    if (!_containers.empty()) state.targetContainerIndex = 0;

    // The combo lists SSH connections, not every selected remote folder.
    std::unordered_set<std::wstring> seenConnections;
    for (const auto& folder : _remoteFolders)
    {
        const std::wstring key = folder.connectionId.empty() ? folder.id : folder.connectionId;
        if (seenConnections.insert(key).second)
        {
            RemoteWorkspaceFolder representative = folder;
            representative.connectionId = key;
            state.entries.push_back(std::move(representative));
        }
    }

    HTREEITEM selected = _tree ? TreeView_GetSelection(_tree) : nullptr;
    auto selectedNode = _nodeData.find(selected);
    if (selectedNode != _nodeData.end())
    {
        if (selectedNode->second.containerIndex < _containers.size())
            state.targetContainerIndex = static_cast<int>(selectedNode->second.containerIndex);

        const size_t remoteIndex = findRemoteFolderForPath(selectedNode->second.path);
        if (remoteIndex != static_cast<size_t>(-1))
        {
            const auto& selectedRemote = _remoteFolders[remoteIndex];
            const std::wstring connectionKey = selectedRemote.connectionId.empty()
                ? selectedRemote.id : selectedRemote.connectionId;
            for (size_t i = 0; i < state.entries.size(); ++i)
            {
                if (state.entries[i].connectionId == connectionKey)
                {
                    state.selectedIndex = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    if (!runRemoteDialog(state)) return;
    if (state.action == RemoteDialogState::Action::Remove)
    {
        if (state.selectedIndex >= 0 && state.selectedIndex < static_cast<int>(state.entries.size()))
        {
            const std::wstring connectionKey = state.entries[static_cast<size_t>(state.selectedIndex)].connectionId;
            for (size_t i = 0; i < _remoteFolders.size(); ++i)
            {
                const std::wstring currentKey = _remoteFolders[i].connectionId.empty()
                    ? _remoteFolders[i].id : _remoteFolders[i].connectionId;
                if (currentKey == connectionKey)
                {
                    removeRemoteFolder(i);
                    break;
                }
            }
        }
        return;
    }

    RemoteWorkspaceFolder config = state.value;
    const bool existingConnection = state.selectedIndex >= 0 &&
                                    state.selectedIndex < static_cast<int>(state.entries.size());
    if (existingConnection)
    {
        const auto& representative = state.entries[static_cast<size_t>(state.selectedIndex)];
        config.connectionId = representative.connectionId.empty()
            ? representative.id : representative.connectionId;
    }
    else
    {
        config.connectionId = createRemoteId();
    }

    const auto previousFolders = _remoteFolders;
    const auto previousRoots = _savedRoots;
    const auto previousContainers = _containers;

    // Updating a connection updates the login used by every remote folder that
    // shares it, while each folder keeps its own remote path and local cache.
    for (auto& folder : _remoteFolders)
    {
        const std::wstring key = folder.connectionId.empty() ? folder.id : folder.connectionId;
        if (key != config.connectionId) continue;
        folder.connectionId = config.connectionId;
        folder.name = config.name;
        folder.host = config.host;
        folder.port = config.port;
        folder.username = config.username;
        folder.authMode = config.authMode;
        folder.password = config.password;
        folder.privateKeyPath = config.privateKeyPath;
        folder.privateKeyPassphrase = config.privateKeyPassphrase;
        folder.hostKeySha256 = config.hostKeySha256;
    }

    auto normalizeLinuxPath = [](std::wstring path)
    {
        std::replace(path.begin(), path.end(), L'\\', L'/');
        while (path.size() > 1 && path.back() == L'/') path.pop_back();
        return path.empty() ? std::wstring(L"/") : path;
    };
    config.remotePath = normalizeLinuxPath(config.remotePath);

    size_t folderIndex = static_cast<size_t>(-1);
    for (size_t i = 0; i < _remoteFolders.size(); ++i)
    {
        const std::wstring key = _remoteFolders[i].connectionId.empty()
            ? _remoteFolders[i].id : _remoteFolders[i].connectionId;
        if (key == config.connectionId &&
            normalizeLinuxPath(_remoteFolders[i].remotePath) == config.remotePath)
        {
            folderIndex = i;
            break;
        }
    }

    if (folderIndex == static_cast<size_t>(-1))
    {
        config.id = createRemoteId();
        config.localCachePath = getRemoteCacheBase() / config.connectionId / config.id;
        _remoteFolders.push_back(config);
        folderIndex = _remoteFolders.size() - 1;
    }
    else
    {
        RemoteWorkspaceFolder& existing = _remoteFolders[folderIndex];
        config.id = existing.id;
        config.localCachePath = existing.localCachePath;
        existing = config;
    }

    std::error_code ec;
    std::filesystem::create_directories(config.localCachePath, ec);
    if (ec)
    {
        _remoteFolders = previousFolders;
        _savedRoots = previousRoots;
        _containers = previousContainers;
        MessageBoxW(_window, (L"Não foi possível criar o cache local da pasta SSH.\n\n" +
                              config.localCachePath.wstring()).c_str(),
                    L"NPPWorkSpace — SSH", MB_OK | MB_ICONERROR);
        return;
    }

    // Connecting only creates/reuses the SSH connection and its cache. It never
    // creates a workspace container. Place the selected remote folder in the
    // existing destination explicitly chosen by the user. Re-adding the same
    // remote folder can also move it between existing containers.
    const std::wstring cacheKey = lower(config.localCachePath.lexically_normal().wstring());
    const auto isCachePath = [&cacheKey](const std::filesystem::path& path)
    {
        return lower(path.lexically_normal().wstring()) == cacheKey;
    };
    _savedRoots.erase(std::remove_if(_savedRoots.begin(), _savedRoots.end(), isCachePath),
                      _savedRoots.end());
    for (auto& container : _containers)
    {
        container.folders.erase(
            std::remove_if(container.folders.begin(), container.folders.end(), isCachePath),
            container.folders.end());
    }

    if (state.targetContainerIndex >= 0 &&
        state.targetContainerIndex < static_cast<int>(_containers.size()))
    {
        _containers[static_cast<size_t>(state.targetContainerIndex)].folders.push_back(
            config.localCachePath);
    }
    else
    {
        _savedRoots.push_back(config.localCachePath);
    }

    if (!writeWorkspaceFile())
    {
        _remoteFolders = previousFolders;
        _savedRoots = previousRoots;
        _containers = previousContainers;
        MessageBoxW(_window,
                    L"A conexão foi validada, mas não foi possível salvar as credenciais protegidas no .worknpp.",
                    L"NPPWorkSpace — SSH", MB_OK | MB_ICONERROR);
        return;
    }

    rebuildWorkspaceTree(true);
    queueRemoteConnect(folderIndex);
    queueRemoteSync(folderIndex);
}

void QuickOpen::removeRemoteFolder(size_t index)
{
    if (index >= _remoteFolders.size()) return;
    const RemoteWorkspaceFolder selected = _remoteFolders[index];
    const std::wstring connectionKey = selected.connectionId.empty() ? selected.id : selected.connectionId;

    std::vector<RemoteWorkspaceFolder> removedFolders;
    for (const auto& folder : _remoteFolders)
    {
        const std::wstring key = folder.connectionId.empty() ? folder.id : folder.connectionId;
        if (key == connectionKey) removedFolders.push_back(folder);
    }

    const std::wstring question = L"Remover a conexão SSH \"" + selected.name +
                                  L"\" e todas as pastas dela do workspace?\n\n" +
                                  std::to_wstring(removedFolders.size()) +
                                  L" pasta(s) local(is) serão desvinculadas. Nada será apagado no Ubuntu Server.";
    if (MessageBoxW(_window, question.c_str(), L"Remover conexão SSH",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    queueRemoteDisconnect(selected);

    for (const auto& remote : removedFolders)
    {
        _savedRoots.erase(std::remove(_savedRoots.begin(), _savedRoots.end(), remote.localCachePath),
                          _savedRoots.end());
        for (auto& container : _containers)
            container.folders.erase(std::remove(container.folders.begin(), container.folders.end(), remote.localCachePath),
                                    container.folders.end());
    }
    _remoteFolders.erase(std::remove_if(_remoteFolders.begin(), _remoteFolders.end(),
        [&](const RemoteWorkspaceFolder& folder)
        {
            const std::wstring key = folder.connectionId.empty() ? folder.id : folder.connectionId;
            return key == connectionKey;
        }), _remoteFolders.end());

    writeWorkspaceFile();
    rebuildWorkspaceTree(true);

    std::wstring cacheList;
    for (const auto& remote : removedFolders)
        cacheList += L"\n" + remote.localCachePath.wstring();
    const std::wstring cacheQuestion = L"Deseja também apagar os caches locais desta conexão?" + cacheList;
    if (MessageBoxW(_window, cacheQuestion.c_str(), L"Cache SSH",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
    {
        bool failed = false;
        for (const auto& remote : removedFolders)
        {
            std::error_code removeError;
            std::filesystem::remove_all(remote.localCachePath, removeError);
            failed = failed || static_cast<bool>(removeError);
        }
        if (failed)
            MessageBoxW(_window, L"Não foi possível apagar todo o cache local da conexão.",
                        L"Cache SSH", MB_OK | MB_ICONWARNING);
    }
}

void QuickOpen::startRemoteWorker()
{
    std::lock_guard<std::mutex> lock(_remoteMutex);
    if (_remoteWorker.joinable()) return;
    _remoteStop = false;
    _remoteWorker = std::thread([this] { remoteWorkerLoop(); });
}

void QuickOpen::stopRemoteWorker()
{
    {
        std::lock_guard<std::mutex> lock(_remoteMutex);
        _remoteStop = true;
        _remoteJobs.clear();
    }
    _remoteCv.notify_all();
    if (_remoteWorker.joinable()) _remoteWorker.join();
}

void QuickOpen::queueRemoteConnect(size_t index)
{
    if (index >= _remoteFolders.size()) return;
    const std::wstring connectionKey = _remoteFolders[index].connectionId.empty()
        ? _remoteFolders[index].id : _remoteFolders[index].connectionId;
    {
        std::lock_guard<std::mutex> lock(_remoteMutex);
        _remoteJobs.erase(std::remove_if(_remoteJobs.begin(), _remoteJobs.end(),
            [&](const RemoteJob& job)
            {
                const std::wstring jobKey = job.config.connectionId.empty() ? job.config.id : job.config.connectionId;
                return job.type == RemoteJob::Type::Connect && jobKey == connectionKey;
            }), _remoteJobs.end());
        RemoteJob job;
        job.type = RemoteJob::Type::Connect;
        job.config = _remoteFolders[index];
        _remoteJobs.push_back(std::move(job));
    }
    SetWindowTextW(_status, (L"SSH  •  conectando " + _remoteFolders[index].name + L"...").c_str());
    _remoteCv.notify_one();
}

void QuickOpen::queueRemoteDisconnect(const RemoteWorkspaceFolder& config)
{
    const std::wstring connectionKey = config.connectionId.empty() ? config.id : config.connectionId;
    {
        std::lock_guard<std::mutex> lock(_remoteMutex);
        _remoteJobs.erase(std::remove_if(_remoteJobs.begin(), _remoteJobs.end(),
            [&](const RemoteJob& job)
            {
                const std::wstring jobKey = job.config.connectionId.empty() ? job.config.id : job.config.connectionId;
                return jobKey == connectionKey;
            }), _remoteJobs.end());
        RemoteJob job;
        job.type = RemoteJob::Type::Disconnect;
        job.config = config;
        _remoteJobs.push_front(std::move(job));
    }
    _remoteCv.notify_one();
}

void QuickOpen::queueRemoteSync(size_t index)
{
    if (index >= _remoteFolders.size()) return;
    {
        std::lock_guard<std::mutex> lock(_remoteMutex);
        _remoteJobs.erase(std::remove_if(_remoteJobs.begin(), _remoteJobs.end(),
            [&](const RemoteJob& job)
            {
                return job.type == RemoteJob::Type::DownloadTree && job.config.id == _remoteFolders[index].id;
            }), _remoteJobs.end());
        RemoteJob job;
        job.type = RemoteJob::Type::DownloadTree;
        job.config = _remoteFolders[index];
        _remoteJobs.push_back(std::move(job));
    }
    SetWindowTextW(_status, (L"SSH  •  sincronizando " + _remoteFolders[index].remotePath + L"...").c_str());
    _remoteCv.notify_one();
}

void QuickOpen::queueRemoteUpload(size_t index, const std::filesystem::path& localFile)
{
    if (index >= _remoteFolders.size() || localFile.empty()) return;
    {
        std::lock_guard<std::mutex> lock(_remoteMutex);
        _remoteJobs.erase(std::remove_if(_remoteJobs.begin(), _remoteJobs.end(),
            [&](const RemoteJob& job)
            {
                return job.type == RemoteJob::Type::UploadFile &&
                       job.config.id == _remoteFolders[index].id &&
                       lower(job.localFile.wstring()) == lower(localFile.wstring());
            }), _remoteJobs.end());
        RemoteJob job;
        job.type = RemoteJob::Type::UploadFile;
        job.config = _remoteFolders[index];
        job.localFile = localFile;
        _remoteJobs.push_back(std::move(job));
    }
    SetWindowTextW(_status, (L"SSH  •  aguardando envio de " + localFile.filename().wstring()).c_str());
    _remoteCv.notify_one();
}

void QuickOpen::remoteWorkerLoop()
{
    struct PersistentConnection
    {
        RemoteWorkspaceFolder config;
        std::unique_ptr<RemoteSsh::Client> client;
    };
    std::unordered_map<std::wstring, PersistentConnection> clients;

    auto ensureConnected = [&](const RemoteWorkspaceFolder& config) -> RemoteSshResult
    {
        const std::wstring key = config.connectionId.empty() ? config.id : config.connectionId;
        auto found = clients.find(key);
        if (found != clients.end() && found->second.client && found->second.client->isConnected() &&
            sameRemoteConnectionSettings(found->second.config, config))
        {
            RemoteSshResult ready;
            ready.success = true;
            ready.hostKeySha256 = found->second.client->hostKeySha256();
            return ready;
        }

        PersistentConnection connection;
        connection.config = config;
        connection.client = std::make_unique<RemoteSsh::Client>();
        RemoteSshResult result = connection.client->connect(config);
        if (result.success)
            clients[key] = std::move(connection);
        else
            clients.erase(key);
        return result;
    };

    while (true)
    {
        RemoteJob job;
        {
            std::unique_lock<std::mutex> lock(_remoteMutex);
            const bool awakened = _remoteCv.wait_for(lock, std::chrono::seconds(15),
                [this] { return _remoteStop || !_remoteJobs.empty(); });
            if (_remoteStop) return; // local map destruction disconnects every session.
            if (!awakened)
            {
                lock.unlock();
                for (auto it = clients.begin(); it != clients.end(); )
                {
                    if (it->second.client && it->second.client->keepAlive())
                    {
                        ++it;
                        continue;
                    }

                    // The user asked for a connection that stays alive for the
                    // whole Notepad++ session. If the server drops an idle
                    // transport, rebuild it immediately instead of waiting for
                    // the next file operation.
                    const RemoteWorkspaceFolder reconnectConfig = it->second.config;
                    auto replacement = std::make_unique<RemoteSsh::Client>();
                    const RemoteSshResult reconnect = replacement->connect(reconnectConfig);
                    if (reconnect.success)
                    {
                        it->second.client = std::move(replacement);
                        ++it;
                    }
                    else
                    {
                        // Keep the connection definition and retry again on the
                        // next keep-alive cycle. Only an explicit Disconnect job
                        // removes it permanently from the session manager.
                        it->second.client.reset();
                        ++it;
                    }
                }
                continue;
            }
            job = std::move(_remoteJobs.front());
            _remoteJobs.pop_front();
        }

        const std::wstring key = job.config.connectionId.empty() ? job.config.id : job.config.connectionId;
        RemoteSshResult result;
        if (job.type == RemoteJob::Type::Disconnect)
        {
            clients.erase(key);
            result.success = true;
        }
        else
        {
            result = ensureConnected(job.config);
            if (result.success && job.type != RemoteJob::Type::Connect)
            {
                auto found = clients.find(key);
                if (found == clients.end() || !found->second.client)
                {
                    result.success = false;
                    result.error = L"A sessão SSH persistente não está disponível.";
                }
                else if (job.type == RemoteJob::Type::UploadFile)
                    result = found->second.client->uploadFile(job.config, job.localFile);
                else
                    result = found->second.client->downloadTree(job.config);

                // Retry once through a fresh transport when the server closed
                // an idle socket between keep-alive intervals.
                if (!result.success)
                {
                    clients.erase(key);
                    RemoteSshResult reconnect = ensureConnected(job.config);
                    if (reconnect.success)
                    {
                        auto retryClient = clients.find(key);
                        if (retryClient != clients.end() && retryClient->second.client)
                        {
                            if (job.type == RemoteJob::Type::UploadFile)
                                result = retryClient->second.client->uploadFile(job.config, job.localFile);
                            else
                                result = retryClient->second.client->downloadTree(job.config);
                        }
                    }
                    if (!result.success)
                    {
                        PersistentConnection pending;
                        pending.config = job.config;
                        clients[key] = std::move(pending);
                    }
                }
            }
        }

        auto* completion = new RemoteJobCompletion(job.config,
            static_cast<int>(job.type), job.localFile, std::move(result));
        if (!_window || !PostMessageW(_window, WM_REMOTE_JOB_DONE, 0, reinterpret_cast<LPARAM>(completion)))
            delete completion;
    }
}

void QuickOpen::onFileSaved(UINT_PTR bufferId)
{
    if (!_npp || bufferId == 0 || _remoteFolders.empty()) return;
    const LRESULT length = SendMessageW(_npp, NPPM_GETFULLPATHFROMBUFFERID,
                                        static_cast<WPARAM>(bufferId), 0);
    if (length <= 0) return;
    std::wstring path(static_cast<size_t>(length) + 1, L'\0');
    const LRESULT copied = SendMessageW(_npp, NPPM_GETFULLPATHFROMBUFFERID,
                                        static_cast<WPARAM>(bufferId),
                                        reinterpret_cast<LPARAM>(path.data()));
    if (copied < 0) return;
    path.resize(static_cast<size_t>(copied));
    if (path.empty()) return;
    const size_t remoteIndex = findRemoteFolderForPath(std::filesystem::path(path));
    if (remoteIndex != static_cast<size_t>(-1))
        queueRemoteUpload(remoteIndex, std::filesystem::path(path));
}

void QuickOpen::removeSelectedRoot()
{
    HTREEITEM selected = TreeView_GetSelection(_tree);
    if (!selected) return;
    auto it = _nodeData.find(selected);
    if (it == _nodeData.end()) return;

    if (it->second.type == NodeType::Container)
    {
        removeContainer(it->second.containerIndex);
        return;
    }
    if (it->second.type != NodeType::Root || it->second.fromNppWorkspace) return;

    const auto path = it->second.path;
    const size_t remoteIndex = findRemoteFolderForPath(path);
    if (remoteIndex != static_cast<size_t>(-1))
    {
        std::error_code ec;
        const auto selectedPath = std::filesystem::weakly_canonical(path, ec);
        ec.clear();
        const auto cachePath = std::filesystem::weakly_canonical(_remoteFolders[remoteIndex].localCachePath, ec);
        if (!ec && lower(selectedPath.wstring()) == lower(cachePath.wstring()))
        {
            const RemoteWorkspaceFolder remote = _remoteFolders[remoteIndex];
            const std::wstring connectionKey = remote.connectionId.empty() ? remote.id : remote.connectionId;
            const size_t foldersInConnection = static_cast<size_t>(std::count_if(
                _remoteFolders.begin(), _remoteFolders.end(), [&](const RemoteWorkspaceFolder& candidate)
                {
                    const std::wstring key = candidate.connectionId.empty() ? candidate.id : candidate.connectionId;
                    return key == connectionKey;
                }));
            if (foldersInConnection <= 1)
            {
                removeRemoteFolder(remoteIndex);
                return;
            }

            const std::wstring question = L"Remover apenas a pasta remota \"" + remote.remotePath +
                                          L"\" do workspace?\n\nA conexão SSH continuará ativa para as outras pastas.";
            if (MessageBoxW(_window, question.c_str(), L"Remover pasta SSH",
                            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
                return;

            _savedRoots.erase(std::remove(_savedRoots.begin(), _savedRoots.end(), remote.localCachePath),
                              _savedRoots.end());
            for (auto& container : _containers)
                container.folders.erase(std::remove(container.folders.begin(), container.folders.end(), remote.localCachePath),
                                        container.folders.end());
            _remoteFolders.erase(_remoteFolders.begin() + static_cast<std::ptrdiff_t>(remoteIndex));
            writeWorkspaceFile();
            rebuildWorkspaceTree(true);

            if (MessageBoxW(_window, (L"Deseja apagar também o cache local?\n\n" +
                                      remote.localCachePath.wstring()).c_str(),
                            L"Cache SSH", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
            {
                std::error_code removeError;
                std::filesystem::remove_all(remote.localCachePath, removeError);
                if (removeError)
                    MessageBoxW(_window, L"Não foi possível apagar todo o cache local da pasta.",
                                L"Cache SSH", MB_OK | MB_ICONWARNING);
            }
            return;
        }
    }
    if (it->second.containerIndex < _containers.size())
    {
        auto& folders = _containers[it->second.containerIndex].folders;
        folders.erase(std::remove(folders.begin(), folders.end(), path), folders.end());
    }
    else
    {
        _savedRoots.erase(std::remove(_savedRoots.begin(), _savedRoots.end(), path), _savedRoots.end());
    }
    writeWorkspaceFile();
    rebuildWorkspaceTree(true);
}

void QuickOpen::newWorkspace()
{
    const int answer = MessageBoxW(
        _window,
        L"Deseja salvar a configuração atual da workspace antes de criar uma nova?",
        L"Nova Workspace",
        MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON1);

    if (answer == IDCANCEL) return;
    if (answer == IDYES && !saveWorkspace()) return;

    _savedRoots.clear();
    _containers.clear();
    _remoteFolders.clear();
    _searchIncludedPaths.clear();
    _searchDisabledPaths.clear();
    _workspaceFile = getWorkspaceFilePath();
    clearSearchResults();
    writeWorkspaceFile();
    rebuildWorkspaceTree(false);
    saveSettings();
}

bool QuickOpen::saveWorkspace()
{
    // Explicit Save always asks where the .worknpp should be written.
    return saveWorkspaceAs();
}

bool QuickOpen::loadContainersFromJson(const std::wstring& json, std::vector<WorkspaceContainer>& containers)
{
    const std::wstring needle = L"\"containers\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::wstring::npos) return false;
    size_t open = json.find(L'[', keyPos + needle.size());
    if (open == std::wstring::npos) return false;

    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t close = std::wstring::npos;
    for (size_t i = open; i < json.size(); ++i)
    {
        const wchar_t ch = json[i];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (ch == L'\\') escaped = true;
            else if (ch == L'"') inString = false;
            continue;
        }
        if (ch == L'"') { inString = true; continue; }
        if (ch == L'[') ++depth;
        else if (ch == L']' && --depth == 0) { close = i; break; }
    }
    if (close == std::wstring::npos) return false;

    size_t pos = open + 1;
    while (pos < close)
    {
        while (pos < close && (iswspace(json[pos]) || json[pos] == L',')) ++pos;
        if (pos >= close) break;
        if (json[pos] != L'{') return false;

        const size_t objectStart = pos;
        int objectDepth = 0;
        inString = false;
        escaped = false;
        size_t objectEnd = std::wstring::npos;
        for (; pos < close; ++pos)
        {
            const wchar_t ch = json[pos];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (ch == L'\\') escaped = true;
                else if (ch == L'"') inString = false;
                continue;
            }
            if (ch == L'"') { inString = true; continue; }
            if (ch == L'{') ++objectDepth;
            else if (ch == L'}' && --objectDepth == 0) { objectEnd = pos; ++pos; break; }
        }
        if (objectEnd == std::wstring::npos) return false;

        const std::wstring object = json.substr(objectStart, objectEnd - objectStart + 1);
        WorkspaceContainer container;
        if (!extractJsonString(object, L"name", container.name) || container.name.empty())
            continue;
        std::wstring colorText;
        if (!extractJsonString(object, L"color", colorText) || !parseHexColor(colorText, container.color))
            container.color = defaultContainerColor(containers.size());
        std::vector<std::wstring> folders;
        if (extractJsonStringArray(object, L"folders", folders))
        {
            for (const auto& folder : folders)
                if (!folder.empty()) container.folders.emplace_back(folder);
        }
        containers.push_back(std::move(container));
    }
    return true;
}


bool QuickOpen::loadRemoteFoldersFromJson(const std::wstring& json, std::vector<RemoteWorkspaceFolder>& folders)
{
    const std::wstring needle = L"\"remoteFolders\"";
    const size_t keyPos = json.find(needle);
    if (keyPos == std::wstring::npos) return true; // Workspaces v1/v2 did not have SSH entries.
    const size_t open = json.find(L'[', keyPos + needle.size());
    if (open == std::wstring::npos) return false;

    bool inString = false;
    bool escaped = false;
    int depth = 0;
    size_t close = std::wstring::npos;
    for (size_t i = open; i < json.size(); ++i)
    {
        const wchar_t ch = json[i];
        if (inString)
        {
            if (escaped) escaped = false;
            else if (ch == L'\\') escaped = true;
            else if (ch == L'\"') inString = false;
            continue;
        }
        if (ch == L'\"') { inString = true; continue; }
        if (ch == L'[') ++depth;
        else if (ch == L']' && --depth == 0) { close = i; break; }
    }
    if (close == std::wstring::npos) return false;

    size_t pos = open + 1;
    while (pos < close)
    {
        while (pos < close && (iswspace(json[pos]) || json[pos] == L',')) ++pos;
        if (pos >= close) break;
        if (json[pos] != L'{') return false;

        const size_t objectStart = pos;
        int objectDepth = 0;
        inString = false;
        escaped = false;
        size_t objectEnd = std::wstring::npos;
        for (; pos < close; ++pos)
        {
            const wchar_t ch = json[pos];
            if (inString)
            {
                if (escaped) escaped = false;
                else if (ch == L'\\') escaped = true;
                else if (ch == L'\"') inString = false;
                continue;
            }
            if (ch == L'\"') { inString = true; continue; }
            if (ch == L'{') ++objectDepth;
            else if (ch == L'}' && --objectDepth == 0) { objectEnd = pos; ++pos; break; }
        }
        if (objectEnd == std::wstring::npos) return false;

        const std::wstring object = json.substr(objectStart, objectEnd - objectStart + 1);
        RemoteWorkspaceFolder folder;
        std::wstring portText;
        std::wstring authText;
        std::wstring protectedPassword;
        std::wstring protectedPassphrase;
        std::wstring cacheText;

        extractJsonString(object, L"id", folder.id);
        extractJsonString(object, L"connectionId", folder.connectionId);
        extractJsonString(object, L"name", folder.name);
        extractJsonString(object, L"host", folder.host);
        extractJsonString(object, L"port", portText);
        extractJsonString(object, L"username", folder.username);
        extractJsonString(object, L"auth", authText);
        extractJsonString(object, L"passwordProtected", protectedPassword);
        extractJsonString(object, L"privateKeyPath", folder.privateKeyPath);
        extractJsonString(object, L"privateKeyPassphraseProtected", protectedPassphrase);
        extractJsonString(object, L"remotePath", folder.remotePath);
        extractJsonString(object, L"localCachePath", cacheText);
        extractJsonString(object, L"hostKeySha256", folder.hostKeySha256);

        if (!portText.empty())
        {
            try
            {
                const unsigned long parsed = std::stoul(portText);
                if (parsed >= 1 && parsed <= 65535) folder.port = static_cast<unsigned short>(parsed);
            }
            catch (...) { folder.port = 22; }
        }
        folder.authMode = _wcsicmp(authText.c_str(), L"privateKey") == 0
            ? RemoteWorkspaceFolder::AuthMode::PrivateKey
            : RemoteWorkspaceFolder::AuthMode::Password;

        // DPAPI data is bound to the Windows user account. If a workspace is
        // copied to another machine/account, keep the connection but require
        // the secret to be entered again instead of discarding the entry.
        RemoteSsh::unprotectSecret(protectedPassword, folder.password);
        RemoteSsh::unprotectSecret(protectedPassphrase, folder.privateKeyPassphrase);

        if (folder.id.empty()) folder.id = createRemoteId();
        if (folder.connectionId.empty()) folder.connectionId = folder.id; // migrate workspace v3
        if (folder.name.empty()) folder.name = folder.host;
        if (folder.remotePath.empty()) folder.remotePath = L"/root";
        folder.localCachePath = cacheText.empty()
            ? getRemoteCacheBase() / folder.connectionId / folder.id
            : std::filesystem::path(cacheText);

        if (folder.host.empty() || folder.username.empty()) continue;
        folders.push_back(std::move(folder));
    }
    return true;
}

bool QuickOpen::writeWorkspaceFile()
{
    if (_workspaceFile.empty()) return false;

    std::vector<std::wstring> protectedPasswords(_remoteFolders.size());
    std::vector<std::wstring> protectedPassphrases(_remoteFolders.size());
    for (size_t i = 0; i < _remoteFolders.size(); ++i)
    {
        if (!_remoteFolders[i].password.empty())
        {
            protectedPasswords[i] = RemoteSsh::protectSecret(_remoteFolders[i].password);
            if (protectedPasswords[i].empty()) return false;
        }
        if (!_remoteFolders[i].privateKeyPassphrase.empty())
        {
            protectedPassphrases[i] = RemoteSsh::protectSecret(_remoteFolders[i].privateKeyPassphrase);
            if (protectedPassphrases[i].empty()) return false;
        }
    }

    const std::filesystem::path target(_workspaceFile);
    std::error_code ec;
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), ec);

    std::wstringstream json;
    json << L"{\n";
    json << L"  \"format\": \"NPPWorkSpace\",\n";
    json << L"  \"version\": 4,\n";
    json << L"  \"workspace\": {\n";
    json << L"    \"folders\": [\n";
    for (size_t i = 0; i < _savedRoots.size(); ++i)
    {
        json << L"      \"" << jsonEscape(_savedRoots[i].wstring()) << L"\"";
        if (i + 1 < _savedRoots.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"containers\": [\n";
    for (size_t i = 0; i < _containers.size(); ++i)
    {
        const auto& container = _containers[i];
        const COLORREF color = container.color == CLR_INVALID ? defaultContainerColor(i) : container.color;
        json << L"      {\"name\": \"" << jsonEscape(container.name) << L"\", \"color\": \""
             << colorToHex(color) << L"\", \"folders\": [";
        for (size_t j = 0; j < container.folders.size(); ++j)
        {
            if (j) json << L", ";
            json << L"\"" << jsonEscape(container.folders[j].wstring()) << L"\"";
        }
        json << L"]}";
        if (i + 1 < _containers.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"remoteFolders\": [\n";
    for (size_t i = 0; i < _remoteFolders.size(); ++i)
    {
        const auto& remote = _remoteFolders[i];
        json << L"      {\n";
        json << L"        \"id\": \"" << jsonEscape(remote.id) << L"\",\n";
        json << L"        \"connectionId\": \"" << jsonEscape(remote.connectionId.empty() ? remote.id : remote.connectionId) << L"\",\n";
        json << L"        \"name\": \"" << jsonEscape(remote.name) << L"\",\n";
        json << L"        \"host\": \"" << jsonEscape(remote.host) << L"\",\n";
        json << L"        \"port\": \"" << remote.port << L"\",\n";
        json << L"        \"username\": \"" << jsonEscape(remote.username) << L"\",\n";
        json << L"        \"auth\": \""
             << (remote.authMode == RemoteWorkspaceFolder::AuthMode::PrivateKey ? L"privateKey" : L"password")
             << L"\",\n";
        json << L"        \"passwordProtected\": \"" << jsonEscape(protectedPasswords[i]) << L"\",\n";
        json << L"        \"privateKeyPath\": \"" << jsonEscape(remote.privateKeyPath) << L"\",\n";
        json << L"        \"privateKeyPassphraseProtected\": \"" << jsonEscape(protectedPassphrases[i]) << L"\",\n";
        json << L"        \"remotePath\": \"" << jsonEscape(remote.remotePath) << L"\",\n";
        json << L"        \"localCachePath\": \"" << jsonEscape(remote.localCachePath.wstring()) << L"\",\n";
        json << L"        \"hostKeySha256\": \"" << jsonEscape(remote.hostKeySha256) << L"\"\n";
        json << L"      }";
        if (i + 1 < _remoteFolders.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"searchIncluded\": [\n";
    size_t includedWritten = 0;
    for (const auto& included : _searchIncludedPaths)
    {
        json << L"      \"" << jsonEscape(included) << L"\"";
        if (++includedWritten < _searchIncludedPaths.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"searchDisabled\": [\n";
    size_t disabledWritten = 0;
    for (const auto& disabled : _searchDisabledPaths)
    {
        json << L"      \"" << jsonEscape(disabled) << L"\"";
        if (++disabledWritten < _searchDisabledPaths.size()) json << L',';
        json << L"\n";
    }
    json << L"    ],\n";
    json << L"    \"shortcuts\": {\n";
    json << L"      \"toggleWorkspace\": \"" << jsonEscape(NPPWorkSpace_GetToggleShortcut()) << L"\",\n";
    json << L"      \"search\": \"" << jsonEscape(NPPWorkSpace_GetSearchShortcut()) << L"\"\n";
    json << L"    }\n";
    json << L"  }\n";
    json << L"}\n";

    const std::string utf8 = utf8FromWide(json.str());
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    if (!out.good()) return false;

    saveSettings();
    return true;
}

bool QuickOpen::saveWorkspaceAs()
{
    wchar_t fileName[MAX_PATH * 4] = L"NPPWorkSpace.worknpp";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = _window;
    ofn.lpstrFilter = L"NPPWorkSpace (*.worknpp)\0*.worknpp\0Todos os arquivos (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
    ofn.lpstrDefExt = L"worknpp";
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&ofn)) return false;

    std::wstring path(fileName);
    if (path.size() < 8 || _wcsicmp(path.c_str() + path.size() - 8, L".worknpp") != 0)
        path += L".worknpp";
    _workspaceFile = path;
    return writeWorkspaceFile();
}

void QuickOpen::openWorkspaceFile()
{
    wchar_t fileName[MAX_PATH * 4]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = _window;
    ofn.lpstrFilter = L"NPPWorkSpace (*.worknpp)\0*.worknpp\0Todos os arquivos (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = static_cast<DWORD>(std::size(fileName));
    ofn.lpstrDefExt = L"worknpp";
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    loadWorkspaceFile(fileName);
}

bool QuickOpen::loadWorkspaceFile(const std::wstring& filePath)
{
    std::ifstream in(std::filesystem::path(filePath), std::ios::binary);
    if (!in) return false;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::wstring json = wideFromUtf8(bytes);
    if (json.empty()) return false;

    std::vector<std::wstring> folders;
    if (!extractJsonStringArray(json, L"folders", folders)) return false;

    std::vector<WorkspaceContainer> containers;
    loadContainersFromJson(json, containers);
    std::vector<RemoteWorkspaceFolder> remoteFolders;
    if (!loadRemoteFoldersFromJson(json, remoteFolders)) return false;
    std::vector<std::wstring> includedSearch;
    extractJsonStringArray(json, L"searchIncluded", includedSearch);
    std::vector<std::wstring> disabledSearch;
    extractJsonStringArray(json, L"searchDisabled", disabledSearch);

    std::wstring toggleShortcut;
    std::wstring searchShortcut;
    if (extractJsonString(json, L"toggleWorkspace", toggleShortcut))
        NPPWorkSpace_SetToggleShortcut(toggleShortcut);
    if (extractJsonString(json, L"search", searchShortcut))
        NPPWorkSpace_SetSearchShortcut(searchShortcut);

    std::vector<std::filesystem::path> loaded;
    for (const auto& folder : folders)
    {
        if (folder.empty()) continue;
        std::filesystem::path path(folder);
        if (std::find(loaded.begin(), loaded.end(), path) == loaded.end())
            loaded.push_back(path);
    }
    for (const auto& remote : remoteFolders)
    {
        std::error_code ec;
        std::filesystem::create_directories(remote.localCachePath, ec);
        const std::wstring remoteKey = lower(remote.localCachePath.lexically_normal().wstring());
        bool alreadyPlaced = false;
        for (const auto& root : loaded)
        {
            if (lower(root.lexically_normal().wstring()) == remoteKey) { alreadyPlaced = true; break; }
        }
        if (!alreadyPlaced)
        {
            for (const auto& container : containers)
            {
                for (const auto& root : container.folders)
                {
                    if (lower(root.lexically_normal().wstring()) == remoteKey)
                    {
                        alreadyPlaced = true;
                        break;
                    }
                }
                if (alreadyPlaced) break;
            }
        }
        if (!alreadyPlaced) loaded.push_back(remote.localCachePath);
    }

    // Only replace the current workspace after the file was parsed successfully.
    _savedRoots = std::move(loaded);
    _containers = std::move(containers);
    _remoteFolders = std::move(remoteFolders);
    _searchIncludedPaths.clear();
    for (const auto& path : includedSearch) if (!path.empty()) _searchIncludedPaths.insert(path);
    _searchDisabledPaths.clear();
    for (const auto& path : disabledSearch) if (!path.empty()) _searchDisabledPaths.insert(path);
    _workspaceFile = filePath;
    clearSearchResults();
    saveSettings();
    rebuildWorkspaceTree(false);
    disableNativeFolderWorkspace();
    std::unordered_set<std::wstring> queuedConnections;
    for (size_t i = 0; i < _remoteFolders.size(); ++i)
    {
        const std::wstring key = _remoteFolders[i].connectionId.empty()
            ? _remoteFolders[i].id : _remoteFolders[i].connectionId;
        if (!_remoteFolders[i].hostKeySha256.empty() && queuedConnections.insert(key).second)
            queueRemoteConnect(i);
    }
    return true;
}

void QuickOpen::loadWorkspace()
{
    _savedRoots.clear();
    _containers.clear();
    _remoteFolders.clear();
    _searchIncludedPaths.clear();
    _searchDisabledPaths.clear();
    if (_workspaceFile.empty()) _workspaceFile = getWorkspaceFilePath();
    if (std::filesystem::exists(_workspaceFile))
    {
        loadWorkspaceFile(_workspaceFile);
    }
    else
    {
        writeWorkspaceFile();
        saveSettings();
    }
}

std::vector<std::filesystem::path> QuickOpen::getWorkspaceRootsForPanel() const
{
    std::vector<std::filesystem::path> roots;
    std::unordered_set<std::wstring> seen;
    auto addUnique = [&](const std::filesystem::path& root)
    {
        if (root.empty() || !workspaceDirectoryExists(root)) return;
        const std::filesystem::path normalized = normalizeWorkspaceRootPath(root);
        const std::wstring key = normalizeScopePath(normalized.wstring());
        if (!key.empty() && seen.insert(key).second) roots.push_back(normalized);
    };

    for (const auto& root : _savedRoots) addUnique(root);
    for (const auto& container : _containers)
        for (const auto& root : container.folders) addUnique(root);

    // Remote SSH folders are real search roots even while their workspace tree
    // is being rebuilt or moved between containers. Explicitly adding each
    // cache closes the timing gap where Ctrl+P could build an index without the
    // freshly synchronized Ubuntu/WSL files.
    for (const auto& remote : _remoteFolders)
        addUnique(remote.localCachePath);

    return roots;
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
        return std::wstring(buffer) + L"\\plugins\\config\\NPPWorkSpace.worknpp";
    return L"NPPWorkSpace.worknpp";
}

void QuickOpen::loadSettings()
{
    std::wstring registryWorkspace;
    if (readWorkspacePathFromRegistry(registryWorkspace))
    {
        _workspaceFile = registryWorkspace;
        return;
    }

    std::ifstream in(getSettingsPath(), std::ios::binary);
    std::string line;
    bool first = true;
    while (std::getline(in, line))
    {
        if (first && line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF)
            line.erase(0, 3);
        first = false;
        if (line.rfind("WorkspaceFile=", 0) == 0)
        {
            const std::wstring value = wideFromUtf8(line.substr(14));
            if (!value.empty()) _workspaceFile = value;
        }
    }
}

void QuickOpen::saveSettings() const
{
    if (!_workspaceFile.empty())
        writeWorkspacePathToRegistry(_workspaceFile);

    const std::wstring path = getSettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out)
    {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        out.write(reinterpret_cast<const char*>(bom), sizeof(bom));
        out << "Version=2\r\n";
        out << "WorkspaceFile=" << utf8FromWide(_workspaceFile) << "\r\n";
    }
}

void QuickOpen::applyTheme()
{
    if (!_npp || !_window) return;

    _darkMode = SendMessageW(_npp, NPPM_ISDARKMODEENABLED, 0, 0) != FALSE;

    // NPPM_GETDARKMODECOLORS is the Notepad++ dark-mode palette API. It must
    // not be used as the light-mode palette: on light mode older N++ builds
    // can leave the returned structure with dark-theme values. In light mode
    // the native Windows colors are the UI palette used by Notepad++.
    NppDarkMode::Colors colors{};
    if (_darkMode &&
        SendMessageW(_npp, NPPM_GETDARKMODECOLORS, sizeof(colors), reinterpret_cast<LPARAM>(&colors)))
    {
        // Keep the active N++ dark/custom-tone palette exactly as supplied.
    }
    else
    {
        colors.background = GetSysColor(COLOR_BTNFACE);
        colors.softerBackground = GetSysColor(COLOR_BTNFACE);
        colors.hotBackground = GetSysColor(COLOR_3DLIGHT);
        colors.pureBackground = GetSysColor(COLOR_WINDOW);
        colors.errorBackground = GetSysColor(COLOR_WINDOW);
        colors.text = GetSysColor(COLOR_WINDOWTEXT);
        colors.darkerText = GetSysColor(COLOR_WINDOWTEXT);
        colors.disabledText = GetSysColor(COLOR_GRAYTEXT);
        colors.linkText = GetSysColor(COLOR_HOTLIGHT);
        colors.edge = GetSysColor(COLOR_BTNSHADOW);
        colors.hotEdge = GetSysColor(COLOR_HIGHLIGHT);
        colors.disabledEdge = GetSysColor(COLOR_3DLIGHT);
    }

    _backgroundColor = colors.background;
    _textColor = colors.text;
    _edgeColor = colors.edge;
    if (_backgroundBrush) DeleteObject(_backgroundBrush);
    _backgroundBrush = CreateSolidBrush(_backgroundColor);

    const COLORREF contentBackground = colors.pureBackground;
    if (_tree)
    {
        TreeView_SetBkColor(_tree, contentBackground);
        TreeView_SetTextColor(_tree, colors.text);
    }
    if (_results)
    {
        ListView_SetBkColor(_results, contentBackground);
        ListView_SetTextBkColor(_results, contentBackground);
        ListView_SetTextColor(_results, colors.text);
    }
    if (_searchPopupResults)
    {
        ListView_SetBkColor(_searchPopupResults, contentBackground);
        ListView_SetTextBkColor(_searchPopupResults, contentBackground);
        ListView_SetTextColor(_searchPopupResults, colors.text);
    }

    // Initialize the whole panel through Notepad++ once, then refresh its
    // children. The native API knows how to switch both directions (dark ->
    // light and light -> dark) and keeps buttons/scrollbars consistent.
    SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                 static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(_window));

    for (HWND h : {_searchGroup, _workspaceGroup, _search, _runSearch, _contentSearchCheck, _searchProgress,
                   _searchProgressText, _cancelSearch, _searchScopeButton, _tree, _results,
                   _addFolder, _newWorkspace, _saveWorkspace, _openWorkspace, _removeFolder,
                   _expandAll, _collapseAll, _createContainer, _remoteSsh, _refreshWorkspace,
                   _viewFolders, _viewSearch, _status})
    {
        if (!h) continue;
        if (_darkMode)
        {
            SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                         static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(h));
        }
        else
        {
            // Clear any stale dark-mode theme from controls when returning to
            // light mode. Reapply the standard Windows theme metrics.
            SetWindowTheme(h, L"Explorer", nullptr);
        }
    }

    if (_searchPopup)
    {
        SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                     static_cast<WPARAM>(NppDarkMode::dmfInit), reinterpret_cast<LPARAM>(_searchPopup));
        if (_searchPopupResults)
        {
            ListView_SetBkColor(_searchPopupResults, contentBackground);
            ListView_SetTextBkColor(_searchPopupResults, contentBackground);
            ListView_SetTextColor(_searchPopupResults, colors.text);
        }
        if (_darkMode)
        {
            for (HWND h : {_searchPopupEdit, _searchPopupRunSearch, _searchPopupContentCheck, _searchPopupScopeButton, _searchPopupResults, _searchPopupHint})
                if (h) SendMessageW(_npp, NPPM_DARKMODESUBCLASSANDTHEME,
                                    static_cast<WPARAM>(NppDarkMode::dmfHandleChange), reinterpret_cast<LPARAM>(h));
        }
        else
        {
            for (HWND h : {_searchPopupEdit, _searchPopupRunSearch, _searchPopupContentCheck, _searchPopupScopeButton, _searchPopupResults, _searchPopupHint})
                if (h) SetWindowTheme(h, L"Explorer", nullptr);
        }
    }

    InvalidateRect(_window, nullptr, TRUE);
    UpdateWindow(_window);
    if (_searchPopup) { InvalidateRect(_searchPopup, nullptr, TRUE); UpdateWindow(_searchPopup); }
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
    if (msg == WM_ERASEBKGND)
    {
        if (_backgroundBrush)
        {
            RECT rc{}; GetClientRect(h, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, _backgroundBrush);
            return 1;
        }
    }
    if (msg == WM_CTLCOLORSTATIC)
    {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, _textColor);
        SetBkColor(dc, _backgroundColor);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(_backgroundBrush);
    }
    switch (msg)
    {
    case WM_GETMINMAXINFO:
    {
        auto* info = reinterpret_cast<MINMAXINFO*>(l);
        // The panel itself is a child of Notepad++'s docking host. Applying
        // limits here keeps the layout usable in docked and floating modes.
        clampWorkspaceMinMaxInfo(h, info);
        return 0;
    }
    case WM_WINDOWPOSCHANGING:
    {
        clampWorkspaceWindowPos(h, reinterpret_cast<WINDOWPOS*>(l));
        break;
    }
    case WM_SIZE:
    {
        const int requestedWidth = static_cast<int>(LOWORD(l));
        const int requestedHeight = static_cast<int>(HIWORD(l));
        layoutControls(requestedWidth, requestedHeight);
        return 0;
    }

    case WM_SEARCH_INDEX_READY:
        finishSearchIndexBuild(reinterpret_cast<std::vector<SearchResult>*>(l),
                               static_cast<unsigned int>(w));
        return 0;
    case WM_CONTENT_SEARCH_BATCH:
        applyContentSearchBatch(reinterpret_cast<ContentSearchBatch*>(l));
        return 0;
    case WM_CONTENT_SEARCH_PROGRESS:
        updateContentSearchProgress(reinterpret_cast<ContentSearchProgress*>(l));
        return 0;
    case WM_CONTENT_SEARCH_DONE:
        completeContentSearch(reinterpret_cast<ContentSearchComplete*>(l));
        return 0;
    case WM_REMOTE_JOB_DONE:
    {
        std::unique_ptr<RemoteJobCompletion> completion(reinterpret_cast<RemoteJobCompletion*>(l));
        if (!completion) return 0;
        const auto type = static_cast<RemoteJob::Type>(completion->type);

        const std::wstring completionKey = completion->folder.connectionId.empty()
            ? completion->folder.id : completion->folder.connectionId;
        const bool folderStillExists = std::any_of(_remoteFolders.begin(), _remoteFolders.end(),
            [&](const RemoteWorkspaceFolder& remote) { return remote.id == completion->folder.id; });
        const bool connectionStillExists = std::any_of(_remoteFolders.begin(), _remoteFolders.end(),
            [&](const RemoteWorkspaceFolder& remote)
            {
                const std::wstring key = remote.connectionId.empty() ? remote.id : remote.connectionId;
                return key == completionKey;
            });

        // Ignore late worker notifications for roots/connections that the user
        // removed while an SFTP operation was still finishing. This prevents a
        // stale completion from rebuilding the tree or showing an irrelevant
        // error after the workspace has already changed.
        if ((type == RemoteJob::Type::DownloadTree || type == RemoteJob::Type::UploadFile) &&
            !folderStillExists)
            return 0;
        if (type == RemoteJob::Type::Connect && !connectionStillExists)
            return 0;

        if (completion->result.success)
        {
            for (auto& remote : _remoteFolders)
            {
                const std::wstring remoteKey = remote.connectionId.empty() ? remote.id : remote.connectionId;
                if (remoteKey == completionKey && remote.hostKeySha256.empty())
                    remote.hostKeySha256 = completion->result.hostKeySha256;
            }

            if (type == RemoteJob::Type::UploadFile)
            {
                const std::wstring text = L"SSH  •  enviado: " + completion->localFile.filename().wstring();
                SetWindowTextW(_status, text.c_str());
            }
            else if (type == RemoteJob::Type::DownloadTree)
            {
                writeWorkspaceFile();
                rebuildWorkspaceTree(true);
                startSearchIndexBuild();

                // Keep both search surfaces synchronized with the newly
                // downloaded cache. The active query is rerun automatically
                // when the fresh index finishes.
                if (_searchPopup && IsWindowVisible(_searchPopup) &&
                    _searchPopupEdit && GetWindowTextLengthW(_searchPopupEdit) > 0)
                {
                    _runSearchAfterIndexBuild = true;
                    _runSearchAfterIndexPopup = true;
                }
                else if (_resultsViewVisible && _search && GetWindowTextLengthW(_search) > 0)
                {
                    _runSearchAfterIndexBuild = true;
                    _runSearchAfterIndexPopup = false;
                }
                const std::wstring text = L"SSH  •  " + completion->folder.remotePath + L" sincronizada  •  " +
                                          std::to_wstring(completion->result.filesProcessed) + L" arquivo(s)";
                SetWindowTextW(_status, text.c_str());
            }
            else if (type == RemoteJob::Type::Connect)
            {
                const std::wstring text = L"SSH  •  conectado: " + completion->folder.name;
                SetWindowTextW(_status, text.c_str());
            }
            else if (type == RemoteJob::Type::Disconnect)
            {
                SetWindowTextW(_status, L"SSH  •  conexão removida");
            }
        }
        else
        {
            std::wstring action;
            if (type == RemoteJob::Type::UploadFile) action = L"enviar o arquivo";
            else if (type == RemoteJob::Type::DownloadTree) action = L"sincronizar a pasta";
            else action = L"manter a conexão SSH";
            const std::wstring message = L"Não foi possível " + action + L" por SSH/SFTP.\n\n" +
                                         completion->result.error;
            SetWindowTextW(_status, L"SSH  •  erro de conexão");
            MessageBoxW(_window, message.c_str(), L"NPPWorkSpace — SSH", MB_OK | MB_ICONERROR);
        }
        return 0;
    }

    case WM_TIMER:
        if (w == WORKSPACE_SYNC_TIMER)
        {
            refreshDockHost();
            refreshDockSplitters();
            syncNativeFolderWorkspace();
            return 0;
        }
        break;

    case WM_COMMAND:
    {
        const int id = LOWORD(w);
        const int code = HIWORD(w);
        if (id == ID_SEARCH_SCOPE && code == BN_CLICKED)
        { POINT pt{}; GetCursorPos(&pt); showSearchScopeMenu(h, pt); return 0; }
        if (id == ID_RUN_SEARCH && code == BN_CLICKED)
        {
            updateSearch();
            return 0;
        }
        if (id == ID_CONTENT_SEARCH && code == BN_CLICKED)
        {
            setContentSearchEnabled(isButtonChecked(_contentSearchCheck));
            return 0;
        }
        if (id == ID_CANCEL_SEARCH && code == BN_CLICKED)
        {
            cancelContentSearch();
            return 0;
        }
        if (id == ID_SEARCH && code == EN_CHANGE)
        {
            if (!_suppressSearch) markSearchPending(false);
            return 0;
        }
        if (id == ID_ADD) { addFolder(); return 0; }
        if (id == ID_NEW) { newWorkspace(); return 0; }
        if (id == ID_SAVE) { saveWorkspace(); return 0; }
        if (id == ID_OPEN) { openWorkspaceFile(); return 0; }
        if (id == ID_OPEN_SELECTED) {
            if (_resultsViewVisible) openSelectedResults(_results);
            else openSelectedTreeFiles();
            return 0;
        }
        if (id == ID_REMOVE) { removeSelectedRoot(); return 0; }
        if (id == ID_EXPAND_ALL) { if (_resultsViewVisible) expandAllContentGroups(); else expandAllFolders(); return 0; }
        if (id == ID_COLLAPSE_ALL) { if (_resultsViewVisible) collapseAllContentGroups(); else collapseAllFolders(); return 0; }
        if (id == ID_CREATE_CONTAINER) { POINT pt{}; GetCursorPos(&pt); createContainer(pt); return 0; }
        if (id == ID_REMOTE_SSH) { showRemoteSshDialog(); return 0; }
        if (id == ID_REFRESH_WORKSPACE) { refreshSelectedWorkspaceNode(); return 0; }
        if (id == ID_VIEW_FOLDERS && code == BN_CLICKED) { showFoldersPanel(); return 0; }
        if (id == ID_VIEW_SEARCH && code == BN_CLICKED) { showResultsPanel(); return 0; }
        if (id >= 50000 && id < 60000) { applySearchScopeCommand(static_cast<UINT>(id)); return 0; }
        break;
    }

    case WM_NOTIFY:
    {
        const auto* hdr = reinterpret_cast<const NMHDR*>(l);
        if (!hdr) break;

        if (hdr->idFrom == ID_TREE)
        {
            const auto* tv = reinterpret_cast<const NMTREEVIEWW*>(l);
            if (hdr->code == NM_DBLCLK) { handleTreeDoubleClick(nullptr); return 0; }
            if (hdr->code == NM_CLICK)
            {
                POINT pt{};
                GetCursorPos(&pt);
                POINT client = pt;
                ScreenToClient(_tree, &client);
                TVHITTESTINFO hit{};
                hit.pt = client;
                HTREEITEM clicked = TreeView_HitTest(_tree, &hit);
                if (clicked && (hit.flags & TVHT_ONITEM))
                {
                    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

                    // Keep the native TreeView focus/selection synchronized with
                    // our custom multi-selection. Ctrl+click preserves the other
                    // selected files; a normal click starts a new selection.
                    TreeView_SelectItem(_tree, clicked);
                    toggleTreeFileSelection(clicked, ctrlDown);
                }
                return 0;
            }
            if (hdr->code == NM_CUSTOMDRAW)
            {
                auto* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(const_cast<NMHDR*>(hdr));
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
                {
                    const HTREEITEM item = reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
                    if (isTreeFileSelected(item))
                    {
                        cd->clrTextBk = GetSysColor(COLOR_HIGHLIGHT);
                        cd->clrText = GetSysColor(COLOR_HIGHLIGHTTEXT);
                    }
                    else if (TreeView_GetSelection(_tree) != item)
                    {
                        auto it = _nodeData.find(item);
                        if (it != _nodeData.end() && it->second.type == NodeType::Container &&
                            it->second.containerIndex < _containers.size())
                        {
                            const COLORREF color = _containers[it->second.containerIndex].color;
                            if (color != CLR_INVALID)
                                cd->clrText = color;
                        }
                    }
                    return CDRF_DODEFAULT;
                }
            }
            if (hdr->code == TVN_ITEMEXPANDINGW) { handleTreeItemExpanding(const_cast<NMTREEVIEWW*>(tv)); return 0; }
            if (hdr->code == TVN_KEYDOWN)
            {
                const auto* kd = reinterpret_cast<const NMTVKEYDOWN*>(l);
                if (kd && kd->wVKey == VK_F5)
                {
                    refreshSelectedWorkspaceNode();
                    return 0;
                }
                if (kd && kd->wVKey == VK_RETURN)
                {
                    openTreeSelection();
                    return 0;
                }
            }
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
                    const bool alreadySelected = isTreeFileSelected(item);
                    if (GetKeyState(VK_CONTROL) & 0x8000)
                        toggleTreeFileSelection(item, true);
                    else if (!alreadySelected)
                    {
                        TreeView_SelectItem(_tree, item);
                        toggleTreeFileSelection(item, false);
                    }
                }
                showTreeContextMenu(item, pt);
                return 0;
            }
        }
        else if (hdr->idFrom == ID_RESULTS)
        {
            if (hdr->code == NM_CUSTOMDRAW)
                return handleResultsCustomDraw(_results, reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(hdr)));
            if (hdr->code == NM_CLICK)
            {
                const int row = ListView_GetNextItem(_results, -1, LVNI_SELECTED);
                if (row >= 0 && row < static_cast<int>(_searchResults.size()) &&
                    _searchResults[static_cast<size_t>(row)].rowType == SearchResult::RowType::ContentFile)
                {
                    toggleContentGroupFromRow(_results, row);
                    return 0;
                }
            }
            if (hdr->code == NM_DBLCLK || hdr->code == NM_RETURN || hdr->code == LVN_ITEMACTIVATE)
            {
                openSelectedResults(_results);
                return 0;
            }
            if (hdr->code == LVN_KEYDOWN)
            {
                const auto* kd = reinterpret_cast<const NMLVKEYDOWN*>(l);
                if (kd && kd->wVKey == VK_F5)
                {
                    refreshSelectedWorkspaceNode();
                    return 0;
                }
                if (kd && kd->wVKey == VK_RETURN)
                {
                    openSelectedResults(_results);
                    return 0;
                }
            }
            if (hdr->code == NM_RCLICK)
            {
                POINT pt{}; GetCursorPos(&pt);
                showResultsContextMenu(_results, pt);
                return 0;
            }
        }
        break;
    }

    case WM_KEYDOWN:
        if (w == VK_F5)
        {
            refreshSelectedWorkspaceNode();
            return 0;
        }
        if (w == VK_ESCAPE)
        {
            if (_searchOnly || GetWindowTextLengthW(_search) > 0) clearSearchResults();
            return 0;
        }
        if (w == VK_RETURN)
        {
            if (_resultsViewVisible) openSearchResult();
            else openTreeSelection();
            return 0;
        }
        break;

    case WM_SETFOCUS:
        if (_resultsViewVisible && _results) SetFocus(_results);
        else if (_tree) SetFocus(_tree);
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
            if (w == VK_F5) { refreshSelectedWorkspaceNode(); return 0; }
            if (w == VK_ESCAPE) { hideSearchPopup(); return 0; }
            if (w == VK_RETURN) { updatePopupSearch(); return 0; }
            if (w == VK_DOWN) { SetFocus(_searchPopupResults); return 0; }
        }
        return _oldPopupSearchProc ? CallWindowProcW(_oldPopupSearchProc, h, msg, w, l)
                                    : DefWindowProcW(h, msg, w, l);
    }

    if (msg == WM_KEYDOWN)
    {
        if (w == VK_F5) { refreshSelectedWorkspaceNode(); return 0; }
        if (w == VK_ESCAPE) { clearSearchResults(); return 0; }
        if (w == VK_RETURN) { updateSearch(); return 0; }
        if (w == VK_DOWN && _resultsViewVisible) { SetFocus(_results); return 0; }
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
        writeWorkspaceFile();
        rebuildWorkspaceTree(true);
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
        addDroppedFolders(reinterpret_cast<HDROP>(static_cast<UINT_PTR>(w)));
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
    case WM_SEARCH_INDEX_READY:
        finishSearchIndexBuild(reinterpret_cast<std::vector<SearchResult>*>(l),
                               static_cast<unsigned int>(w));
        return 0;
    case WM_CONTENT_SEARCH_BATCH:
        applyContentSearchBatch(reinterpret_cast<ContentSearchBatch*>(l));
        return 0;
    case WM_CONTENT_SEARCH_PROGRESS:
        updateContentSearchProgress(reinterpret_cast<ContentSearchProgress*>(l));
        return 0;
    case WM_CONTENT_SEARCH_DONE:
        completeContentSearch(reinterpret_cast<ContentSearchComplete*>(l));
        return 0;
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
        if (LOWORD(w) == ID_SEARCH_SCOPE && HIWORD(w) == BN_CLICKED)
        { POINT pt{}; GetCursorPos(&pt); showSearchScopeMenu(h, pt); return 0; }
        if (LOWORD(w) == ID_POPUP_RUN_SEARCH && HIWORD(w) == BN_CLICKED)
        {
            updatePopupSearch();
            return 0;
        }
        if (LOWORD(w) == ID_POPUP_CONTENT_SEARCH && HIWORD(w) == BN_CLICKED)
        {
            setContentSearchEnabled(isButtonChecked(_searchPopupContentCheck));
            return 0;
        }
        if (LOWORD(w) == ID_POPUP_SEARCH && HIWORD(w) == EN_CHANGE)
        {
            markSearchPending(true);
            return 0;
        }
        if (LOWORD(w) == ID_OPEN_SELECTED)
        {
            openSelectedResults(_searchPopupResults, true);
            return 0;
        }
        if (LOWORD(w) >= 50000 && LOWORD(w) < 60000) { applySearchScopeCommand(static_cast<UINT>(LOWORD(w))); return 0; }
        break;
    case WM_NOTIFY:
    {
        const auto* hdr = reinterpret_cast<const NMHDR*>(l);
        if (hdr && hdr->idFrom == ID_POPUP_RESULTS)
        {
            if (hdr->code == NM_CUSTOMDRAW)
                return handleResultsCustomDraw(_searchPopupResults, reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(hdr)));
            if (hdr->code == NM_CLICK)
            {
                const int row = ListView_GetNextItem(_searchPopupResults, -1, LVNI_SELECTED);
                if (row >= 0 && row < static_cast<int>(_searchResults.size()) &&
                    _searchResults[static_cast<size_t>(row)].rowType == SearchResult::RowType::ContentFile)
                {
                    toggleContentGroupFromRow(_searchPopupResults, row);
                    return 0;
                }
            }
            if (hdr->code == LVN_KEYDOWN)
            {
                const auto* kd = reinterpret_cast<const NMLVKEYDOWN*>(l);
                if (kd && kd->wVKey == VK_F5)
                {
                    refreshSelectedWorkspaceNode();
                    return 0;
                }
                if (kd && kd->wVKey == VK_RETURN)
                {
                    openSelectedResults(_searchPopupResults, true);
                    return 0;
                }
            }
            if (hdr->code == NM_DBLCLK || hdr->code == NM_RETURN || hdr->code == LVN_ITEMACTIVATE)
            {
                openSelectedResults(_searchPopupResults, true);
                return 0;
            }
            if (hdr->code == NM_RCLICK)
            {
                POINT pt{}; GetCursorPos(&pt);
                showResultsContextMenu(_searchPopupResults, pt);
                return 0;
            }
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
        _searchPopupScopeButton = nullptr;
        _searchPopupRunSearch = nullptr;
        _searchPopupContentCheck = nullptr;
        _searchPopupHint = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
