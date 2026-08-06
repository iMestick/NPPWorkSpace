#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct RemoteWorkspaceFolder
{
    enum class AuthMode
    {
        Password,
        PrivateKey
    };

    // id identifies this selected remote folder/cache. connectionId identifies
    // the SSH session and is shared when several folders use the same login.
    std::wstring id;
    std::wstring connectionId;
    std::wstring name;
    std::wstring host;
    unsigned short port{22};
    std::wstring username{L"root"};
    AuthMode authMode{AuthMode::Password};
    std::wstring password;
    std::wstring privateKeyPath;
    std::wstring privateKeyPassphrase;
    std::wstring remotePath{L"/root"};
    std::filesystem::path localCachePath;
    std::wstring hostKeySha256;
};

struct RemoteSshResult
{
    bool success{false};
    std::wstring error;
    std::wstring hostKeySha256;
    size_t filesProcessed{0};
    unsigned long long bytesTransferred{0};
};

struct RemoteDirectoryEntry
{
    std::wstring name;
    std::wstring fullPath;
    bool isDirectory{false};
    bool isRegularFile{false};
    unsigned long long size{0};
    unsigned long modifiedTime{0};
};

namespace RemoteSsh
{
    using ProgressCallback = std::function<void(const std::wstring&, size_t)>;

    // Protects secrets for the current Windows user. The resulting Base64 text
    // can be safely persisted in .worknpp, but can only be decrypted by the
    // same Windows user profile on the same machine.
    std::wstring protectSecret(const std::wstring& plainText);
    bool unprotectSecret(const std::wstring& protectedText, std::wstring& plainText);

    // A reusable authenticated SSH/SFTP session. QuickOpen keeps one Client per
    // connectionId on its remote worker thread, so saving several files does not
    // reconnect for every upload. The session closes when the connection is
    // removed or when Notepad++ unloads the plugin.
    class Client
    {
    public:
        Client();
        ~Client();
        Client(Client&&) noexcept;
        Client& operator=(Client&&) noexcept;
        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        RemoteSshResult connect(const RemoteWorkspaceFolder& config);
        void disconnect();
        bool isConnected() const;
        bool keepAlive();
        std::wstring hostKeySha256() const;

        RemoteSshResult listDirectory(const std::wstring& remotePath,
                                      std::vector<RemoteDirectoryEntry>& entries);
        RemoteSshResult downloadTree(const RemoteWorkspaceFolder& config,
                                     const ProgressCallback& progress = {});
        RemoteSshResult uploadFile(const RemoteWorkspaceFolder& config,
                                   const std::filesystem::path& localFile);

    private:
        struct Impl;
        std::unique_ptr<Impl> _impl;
    };

    // Convenience one-shot wrappers used by validation code.
    RemoteSshResult testConnection(const RemoteWorkspaceFolder& config);
    RemoteSshResult downloadTree(const RemoteWorkspaceFolder& config,
                                 const ProgressCallback& progress = {});
    RemoteSshResult uploadFile(const RemoteWorkspaceFolder& config,
                               const std::filesystem::path& localFile);
}
