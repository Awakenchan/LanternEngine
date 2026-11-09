// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/native/tone_curve.h"

#include "video_core/cache_storage.h"
#include "video_core/cache_file.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <condition_variable>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace {

std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};
bool accepting_requests{};
constexpr u64 MaxRecordBytes = 64 * 1024 * 1024;
std::unordered_map<std::string, mz_uint> archive_index;

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    for (;;) {
        std::packaged_task<void()> request;
        {
            std::unique_lock lock{m_request};
            Common::CondvarWait(request_cv, lock, stoken, [&] { return !req_queue.empty(); });
            // Stop only after draining accepted writes. Shader/meta/key ordering is FIFO.
            if (req_queue.empty() && stoken.stop_requested()) {
                break;
            }
            if (req_queue.empty()) {
                continue;
            }
            request = std::move(req_queue.front());
            req_queue.pop();
        }
        request();
        try {
            request.get_future().get();
        } catch (const std::exception& error) {
            LOG_WARNING(Render, "Pipeline cache write failed: {}", error.what());
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();
    // bbport: shaders compiled with the tone curve extension (BB_HDR_SCENE_CURVE=1) are kept in a
    // cache of their own: turning it on or off never mixes modules with and without it.
    const std::string serial = std::string{game_info.GameSerial()} +
                               (Shader::Native::ToneCurveEnabled() ? "-scene-curve" : "");

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        ar_is_read_only = true;
        archive_index.clear();
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{serial}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_zero_struct(&zip_ar);
            if (!mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0)) {
                return;
            }
            ar_is_read_only = false;
        } else {
            for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip_ar); ++i) {
                std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> name{};
                mz_zip_reader_get_filename(&zip_ar, i, name.data(), name.size());
                // Old archives may contain multiple generations of the same name.
                archive_index.insert_or_assign(name.data(), i);
            }
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / serial;
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
    }

    {
        std::scoped_lock lock{m_request};
        accepting_requests = true;
    }
    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!opened.exchange(false)) {
        return;
    }

    {
        std::scoped_lock lock{m_request};
        accepting_requests = false;
    }
    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        if (ar_is_read_only) {
            mz_zip_reader_end(&zip_ar);
        } else {
            mz_zip_writer_finalize_archive(&zip_ar);
            mz_zip_writer_end(&zip_ar);
        }
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[type, path = std::move(path_),
                                                  v = std::move(v)]() mutable {
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                // bbport: written under a temporary name and renamed into place, so a crash or a
                // power loss mid-write leaves the old file or none, never a cut-short one that
                // the next start reads as a damaged entry (issue #28).
                using namespace Common::FS;
                const auto bytes = std::span{reinterpret_cast<const u8*>(v.data()),
                                             v.size() * sizeof(T)};
                if (!ReplaceCacheFile(path, bytes)) {
                    LOG_WARNING(Render, "Cannot replace pipeline cache record {}", path.string());
                }
            }
        }};
        std::scoped_lock lock{m_request};
        if (!accepting_requests) {
            return false;
        }
        req_queue.emplace(std::move(request));
    }

    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto entry = archive_index.find(path.string());
        if (entry == archive_index.end()) {
            return;
        }
        const auto index = entry->second;
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip_ar, index, &stat) ||
            stat.m_uncomp_size > MaxRecordBytes || stat.m_uncomp_size % sizeof(T)) {
            return;
        }
        v.resize(stat.m_uncomp_size / sizeof(T));
        if (!mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0)) {
            v.clear();
        }
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        if (!file.IsOpen() || file.GetSize() > MaxRecordBytes || file.GetSize() % sizeof(T)) {
            return;
        }
        v.resize(file.GetSize() / sizeof(T));
        if (file.Read(v) != v.size()) {
            v.clear();
        }
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    Forget(type, name);
    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    Forget(type, name);
    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::IsFolder() const {
    return opened && !EmulatorSettings.IsPipelineCacheArchived();
}

bool DataBase::SaveNow(BlobType type, const std::string& name, std::span<const u8> data) {
    if (!IsFolder()) {
        return false;
    }
    Forget(type, name);
    auto path = cache_path / name;
    path.replace_extension(GetBlobFileExtension(type));
    return ReplaceCacheFile(path, data);
}

void DataBase::Remove(BlobType type, const std::string& name) {
    if (!IsFolder()) {
        return;
    }
    Forget(type, name);
    auto path = cache_path / name;
    path.replace_extension(GetBlobFileExtension(type));
    std::error_code error;
    std::filesystem::remove(path, error);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }
    if (TakeAhead(type, name, data)) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }
    if (std::vector<u8> bytes; TakeAhead(type, name, bytes)) {
        if (bytes.size() % sizeof(u32) == 0) {
            data.resize(bytes.size() / sizeof(u32));
            std::memcpy(data.data(), bytes.data(), bytes.size());
        }
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    for (const auto& name : ListBlobs(type)) {
        std::vector<u8> data;
        Load(type, name, data);
        func(std::move(data));
    }
}

std::vector<std::string> DataBase::ListBlobs(BlobType type) const {
    std::vector<std::string> names;
    const auto ext = "." + GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        for (const auto& [name, index] : archive_index) {
            if (std::filesystem::path(name).extension() == ext) {
                names.push_back(std::filesystem::path(name).stem().string());
            }
        }
    } else {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator{cache_path, error}) {
            if (entry.is_regular_file(error) && entry.path().extension() == ext) {
                names.push_back(entry.path().stem().string());
            }
        }
    }
    std::ranges::sort(names);
    return names;
}

void DataBase::Clear() {
    if (!opened) {
        return;
    }
    {
        std::scoped_lock lock{ahead_mutex};
        ahead.clear();
    }
    // Only called during preload, before any asynchronous writes have been submitted.
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        if (ar_is_read_only) {
            mz_zip_reader_end(&zip_ar);
        } else {
            mz_zip_writer_end(&zip_ar);
        }
        mz_zip_zero_struct(&zip_ar);
        archive_index.clear();
        ar_is_read_only = false;
        if (!mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0)) {
            LOG_WARNING(Render, "Cannot recreate pipeline cache archive");
            Close();
        }
        return;
    }
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

void DataBase::FinishPreload() {
    {
        std::scoped_lock lock{ahead_mutex};
        ahead.clear();
    }
    if (opened && EmulatorSettings.IsPipelineCacheArchived() && ar_is_read_only) {
        if (!mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str())) {
            LOG_WARNING(Render, "Cannot reopen pipeline cache archive for writing");
            Close();
            return;
        }
        ar_is_read_only = false;
    }
}

std::string AheadName(BlobType type, const std::string& name) {
    return name + "." + GetBlobFileExtension(type);
}

bool DataBase::TakeAhead(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        return false;
    }
    std::scoped_lock lock{ahead_mutex};
    if (ahead.empty()) {
        return false;
    }
    const auto found = ahead.find(AheadName(type, name));
    if (found == ahead.end()) {
        return false;
    }
    data = found->second;
    return true;
}

void DataBase::Forget(BlobType type, const std::string& name) {
    std::scoped_lock lock{ahead_mutex};
    if (!ahead.empty()) {
        ahead.erase(AheadName(type, name));
    }
}

void DataBase::ReadAhead() {
    if (!IsFolder()) {
        return;
    }
    const char* env = std::getenv("BB_CACHE_READ_AHEAD");
    if (env && env[0] == '0') {
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, error}) {
        const auto ext = entry.path().extension();
        if (ext == ".key" || ext == ".spv" || ext == ".meta") {
            files.push_back(entry.path());
        }
    }
    std::vector<std::vector<u8>> data(files.size());
    std::vector<u8> read(files.size());
    std::atomic<size_t> next{0};
    std::atomic<u64> bytes{0};
    const auto worker = [&] {
        for (size_t i; (i = next.fetch_add(1)) < files.size();) {
            const auto file = Common::FS::IOFile{files[i], Common::FS::FileAccessMode::Read};
            if (!file.IsOpen()) {
                continue;
            }
            const u64 size = file.GetSize();
            if (size > MaxRecordBytes) {
                continue;
            }
            data[i].resize(size);
            if (file.Read(data[i]) != size) {
                data[i] = {};
                continue;
            }
            read[i] = 1;
            bytes.fetch_add(size, std::memory_order_relaxed);
        }
    };
    const u32 threads = u32(std::clamp<size_t>(files.size() / 64, 1, 16));
    std::vector<std::thread> pool;
    for (u32 t = 1; t < threads; ++t) {
        pool.emplace_back(worker);
    }
    worker();
    for (auto& thread : pool) {
        thread.join();
    }
    size_t taken = 0;
    {
        std::scoped_lock lock{ahead_mutex};
        ahead.clear();
        ahead.reserve(files.size());
        for (size_t i = 0; i < files.size(); ++i) {
            if (read[i]) {
                ahead.emplace(files[i].filename().string(), std::move(data[i]));
                ++taken;
            }
        }
    }
    std::printf("GPU cache: %zu of %zu files read ahead in %.2f s on %u threads (%.1f MiB; "
                "BB_CACHE_READ_AHEAD=0: one at a time)\n",
                taken, files.size(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                threads, double(bytes.load()) / (1024.0 * 1024.0));
    std::fflush(stdout);
}

} // namespace Storage
