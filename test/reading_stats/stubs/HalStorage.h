#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <string>
#include <utility>

namespace statsTest {
inline std::map<std::string, std::string> files;
inline std::map<std::string, size_t> readBytes;
inline std::string failOpen;
inline std::string failWrite;
inline std::string failClose;
inline std::string failRename;
inline std::string failRead;
inline std::string failRemove;
inline size_t writeBudget = SIZE_MAX;
inline bool consume(std::string& failure, const std::string& path) {
  if (failure != path) return false;
  failure.clear();
  return true;
}
inline void reset() {
  files.clear();
  readBytes.clear();
  failOpen.clear();
  failWrite.clear();
  failClose.clear();
  failRename.clear();
  failRead.clear();
  failRemove.clear();
  writeBudget = SIZE_MAX;
}
}  // namespace statsTest

class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::string path, size_t offset = 0) : path_(std::move(path)), offset_(offset), open_(true) {}
  HalFile(HalFile&&) = default;
  HalFile& operator=(HalFile&&) = default;
  HalFile(const HalFile&) = delete;
  explicit operator bool() const { return open_; }
  bool isOpen() const { return open_; }
  bool isDirectory() const { return false; }
  uint64_t fileSize64() const { return statsTest::files.at(path_).size(); }
  size_t position() const { return offset_; }
  bool seek64(uint64_t offset) {
    if (!open_ || offset > fileSize64()) return false;
    offset_ = offset;
    return true;
  }
  int read() {
    unsigned char byte;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  int read(void* buffer, size_t count) {
    if (!open_ || statsTest::consume(statsTest::failRead, path_)) return -1;
    const auto& data = statsTest::files.at(path_);
    count = std::min(count, data.size() - offset_);
    std::memcpy(buffer, data.data() + offset_, count);
    offset_ += count;
    statsTest::readBytes[path_] += count;
    return static_cast<int>(count);
  }
  size_t write(const uint8_t* buffer, size_t count) {
    if (!open_ || statsTest::consume(statsTest::failWrite, path_)) return 0;
    count = std::min(count, statsTest::writeBudget);
    if (statsTest::writeBudget != SIZE_MAX) statsTest::writeBudget -= count;
    auto& data = statsTest::files.at(path_);
    if (offset_ + count > data.size()) data.resize(offset_ + count);
    std::memcpy(data.data() + offset_, buffer, count);
    offset_ += count;
    return count;
  }
  bool truncate(uint64_t size) {
    if (!open_) return false;
    statsTest::files.at(path_).resize(size);
    return true;
  }
  void flush() {}
  bool close() {
    open_ = false;
    return !statsTest::consume(statsTest::failClose, path_);
  }

 private:
  std::string path_;
  size_t offset_ = 0;
  bool open_ = false;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool exists(const char* path) const { return statsTest::files.count(path) != 0; }
  bool mkdir(const char*, bool) { return true; }
  HalFile open(const char* path, int flags) {
    if (statsTest::consume(statsTest::failOpen, path)) return {};
    if (!exists(path) && !(flags & O_CREAT)) return {};
    auto& content = statsTest::files[path];
    if (flags & O_TRUNC) content.clear();
    return HalFile(path, flags & O_APPEND ? content.size() : 0);
  }
  bool remove(const char* path) {
    if (statsTest::consume(statsTest::failRemove, path)) return false;
    return statsTest::files.erase(path) != 0;
  }
  bool rename(const char* from, const char* to) {
    if (statsTest::consume(statsTest::failRename, to) || !exists(from) || exists(to)) return false;
    statsTest::files[to] = std::move(statsTest::files.at(from));
    statsTest::files.erase(from);
    return true;
  }
  bool replaceFile(const char* from, const char* to) {
    return (!exists(to) || remove(to)) && rename(from, to);
  }
};
#define Storage HalStorage::getInstance()
