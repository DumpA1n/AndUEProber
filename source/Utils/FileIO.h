#pragma once

#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <string>

class FileIO {
public:
    explicit FileIO(const std::filesystem::path& path) : path_(path) {}
    ~FileIO() {
        Close();
    }

    bool Exists() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::filesystem::exists(path_);
    }

    bool Open(bool create = true) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) return true;
        if (std::filesystem::exists(path_) == false && create) {
            std::filesystem::create_directories(path_.parent_path());
            std::ofstream tmp(path_, std::ios::binary);
        }
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
        return file_.is_open();
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.close();
        }
    }

    void SeekTo(std::streampos position) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.seekg(position);
            file_.seekp(position);
        }
    }

    void Write(const std::string& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.write(data.data(), data.size());
            file_.flush();
        }
    }

    void Append(const std::string& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.seekp(0, std::ios::end);
            file_.write(data.data(), data.size());
            file_.flush();
        }
    }

    template <typename... Args>
    void Append(std::format_string<Args...> fmt, Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.seekp(0, std::ios::end);
            auto data = std::format(fmt, std::forward<Args>(args)...);
            file_.write(data.data(), data.size());
            file_.flush();
        }
    }

    void Read(std::string& outData, std::streamsize size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            outData.resize(size);
            file_.read(outData.data(), size);
        }
    }

    void ReadAll(std::string& outData) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.seekg(0, std::ios::beg);
            outData.assign((std::istreambuf_iterator<char>(file_)),
                            std::istreambuf_iterator<char>());
        }
    }

    ssize_t Size() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            auto currentPos = file_.tellg();
            file_.seekg(0, std::ios::end);
            auto size = file_.tellg();
            file_.seekg(currentPos);
            return size;
        }
        return -1;
    }

    void Flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.flush();
        }
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.close();
        }
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    }

    void Delete() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.close();
        }
        if (std::filesystem::exists(path_)) {
            std::filesystem::remove(path_);
        }
    }

    void Rename(const std::filesystem::path& newPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.close();
        }
        std::filesystem::rename(path_, newPath);
        path_ = newPath;
        file_.open(path_, std::ios::binary | std::ios::in | std::ios::out);
    }

    void SetPath(const std::filesystem::path& newPath) {
        std::lock_guard<std::mutex> lock(mutex_);
        path_ = newPath;
    }

    struct BatchOps {
        std::fstream& file_;
        void Write(const std::string& data) {
            if (file_.is_open()) { file_.write(data.data(), data.size()); file_.flush(); }
        }
        void Append(const std::string& data) {
            if (file_.is_open()) { file_.seekp(0, std::ios::end); file_.write(data.data(), data.size()); file_.flush(); }
        }
        template <typename... Args>
        void Append(std::format_string<Args...> fmt, Args&&... args) {
            if (file_.is_open()) {
                file_.seekp(0, std::ios::end);
                auto data = std::format(fmt, std::forward<Args>(args)...);
                file_.write(data.data(), data.size());
                file_.flush();
            }
        }
        void SeekTo(std::streampos pos) {
            if (file_.is_open()) { file_.seekg(pos); file_.seekp(pos); }
        }
        void Flush() { if (file_.is_open()) file_.flush(); }
    };

    template<typename Fn>
    void batch(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        fn(BatchOps{file_});
    }

private:
    mutable std::mutex mutex_;
    std::fstream file_;
    std::filesystem::path path_;
};
