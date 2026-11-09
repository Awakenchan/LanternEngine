// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <span>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// bbport: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);
    std::vector<std::string> ListBlobs(BlobType type) const;

    /// bbport: the cache is a folder of files (not the archive): the recompile pass
    /// (cache_recompile.cpp) rewrites and removes records during preload, synchronously.
    [[nodiscard]] bool IsFolder() const;
    /// The cache folder (empty for the archive or a closed cache).
    [[nodiscard]] std::filesystem::path FolderPath() const {
        return IsFolder() ? cache_path : std::filesystem::path{};
    }
    bool SaveNow(BlobType type, const std::string& name, std::span<const u8> data);
    void Remove(BlobType type, const std::string& name);
    void ReadAhead();

private:
    bool TakeAhead(BlobType type, const std::string& name, std::vector<u8>& data);
    void Forget(BlobType type, const std::string& name);

    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::atomic<bool> opened{};
    std::mutex ahead_mutex;
    std::unordered_map<std::string, std::vector<u8>> ahead;
};

} // namespace Storage
