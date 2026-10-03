// Binary format reader: mmap + pointer-walking iteration.

#include "kairos/data_format.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kairos {
namespace data {
namespace {

// CRC32 (IEEE) without a table: bit-by-bit is fine for file validation
// (not the hot path). Reflected polynomial 0xEDB88320.
std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data,
                           std::size_t len) {
    crc ^= 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace

MappedFile::MappedFile(const char* path) {
    fd_ = ::open(path, O_RDONLY);
    if (fd_ < 0) {
        return;
    }
    struct stat st;
    if (::fstat(fd_, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(FileHeader))) {
        ::close(fd_);
        fd_ = -1;
        return;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (p == MAP_FAILED) {
        ::close(fd_);
        fd_ = -1;
        size_ = 0;
        return;
    }
    data_ = static_cast<const std::uint8_t*>(p);
    const FileHeader* h = header();
    if (h->magic != kMagic || h->version != kVersion ||
        h->header_size != sizeof(FileHeader)) {
        ::munmap(const_cast<std::uint8_t*>(data_), size_);
        ::close(fd_);
        data_ = nullptr;
        fd_ = -1;
        size_ = 0;
    }
}

MappedFile::~MappedFile() {
    if (data_) {
        ::munmap(const_cast<std::uint8_t*>(data_), size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        if (data_) {
            ::munmap(const_cast<std::uint8_t*>(data_), size_);
        }
        if (fd_ >= 0) {
            ::close(fd_);
        }
        data_ = other.data_;
        size_ = other.size_;
        fd_ = other.fd_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
    }
    return *this;
}

MappedFile::Iterator MappedFile::iterate() const {
    Iterator it;
    if (data_) {
        it.pos_ = data_ + sizeof(FileHeader);
        it.end_ = data_ + size_;
    }
    return it;
}

bool MappedFile::Iterator::next(EventView& out) {
    if (!pos_ || pos_ + sizeof(EventHeader) > end_) {
        return false;
    }
    const EventHeader* h =
        reinterpret_cast<const EventHeader*>(pos_);
    const std::uint8_t* levels_begin = pos_ + sizeof(EventHeader);
    const std::uint8_t* next_pos =
        levels_begin + static_cast<std::size_t>(h->n_levels) * sizeof(LevelEntry);
    if (next_pos > end_ || next_pos < pos_) {  // overflow / truncation guard
        return false;
    }
    out.header = h;
    out.levels = reinterpret_cast<const LevelEntry*>(levels_begin);
    pos_ = next_pos;
    return true;
}

std::uint32_t MappedFile::compute_crc32() const {
    if (!data_ || size_ <= sizeof(FileHeader)) {
        return 0;
    }
    // File validation is offline work; clarity over speed here.
    return crc32_update(0, data_ + sizeof(FileHeader),
                        size_ - sizeof(FileHeader));
}

CaptureReader::CaptureReader(const std::vector<std::string>& paths)
    : paths_(paths), current_("") {
}

const FileHeader* CaptureReader::header() const {
    return current_.valid() ? current_.header() : nullptr;
}

bool CaptureReader::next(EventView& out) {
    for (;;) {
        if (!started_ || !current_.valid()) {
            if (file_idx_ >= paths_.size()) {
                return false;
            }
            current_ = MappedFile(paths_[file_idx_].c_str());
            if (!current_.valid()) {
                error_ = "cannot open/mmap file: " + paths_[file_idx_];
                return false;
            }
            // Verify CRC if the file was finalized (crc != 0).
            const std::uint32_t stored = current_.header()->crc32;
            if (stored != 0 && current_.compute_crc32() != stored) {
                error_ = "crc mismatch in file: " + paths_[file_idx_];
                return false;
            }
            it_ = current_.iterate();
            started_ = true;
        }
        if (it_.next(out)) {
            return true;
        }
        // End of this file: advance.
        ++file_idx_;
        started_ = false;
    }
}

}  // namespace data
}  // namespace kairos
