#pragma once

// C++ reader for the Kairos market data binary format v1.
// See docs/design/data-format.md for the spec.
//
// The file is mmap'd; events are walked with pure pointer arithmetic.
// No allocation, no exceptions.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kairos {
namespace data {

inline constexpr std::uint32_t kMagic = 0x5249414B;  // "KAIR"
inline constexpr std::uint16_t kVersion = 1;

#pragma pack(push, 1)
struct FileHeader {
    std::uint32_t magic;         // 0
    std::uint16_t version;       // 4
    std::uint16_t header_size;   // 6
    char symbol[16];             // 8
    char venue[16];              // 24
    std::int64_t price_scale;    // 40
    std::uint32_t qty_scale;     // 48
    std::uint32_t crc32;         // 52 (of all bytes after the header; 0 if not finalized)
    std::uint64_t created_wall_ns;  // 56
};
static_assert(sizeof(FileHeader) == 64);

struct EventHeader {
    std::uint8_t type;       // 0=snapshot, 1=diff, 2=trade
    std::uint8_t reserved0;  // 1
    std::uint16_t reserved1;  // 2
    std::uint32_t n_levels;  // 4
    std::uint64_t seq;       // 8
    std::uint64_t exchange_ts_ns;  // 16
    std::uint64_t local_mono_ns;   // 24
    std::uint64_t local_wall_ns;   // 32
    std::uint64_t reserved2;       // 40
};
static_assert(sizeof(EventHeader) == 48);

struct LevelEntry {
    std::int64_t price_ticks;  // 0
    std::int64_t qty_lots;     // 8
    std::uint8_t side;         // 16: 0=bid, 1=ask
    std::uint8_t pad[7];       // 17
};
static_assert(sizeof(LevelEntry) == 24);
#pragma pack(pop)

enum class EventType : std::uint8_t { Snapshot = 0, Diff = 1, Trade = 2 };

// Non-owning view of one event.
struct EventView {
    const EventHeader* header;
    const LevelEntry* levels;  // header->n_levels entries
};

// RAII mmap of a capture file.
class MappedFile {
public:
    explicit MappedFile(const char* path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept
        : data_(other.data_), size_(other.size_), fd_(other.fd_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.fd_ = -1;
    }
    MappedFile& operator=(MappedFile&& other) noexcept;

    bool valid() const { return data_ != nullptr; }
    const FileHeader* header() const {
        return reinterpret_cast<const FileHeader*>(data_);
    }
    std::size_t size() const { return size_; }

    // Forward iteration over events. Returns false at end.
    class Iterator {
    public:
        bool next(EventView& out);
    private:
        friend class MappedFile;
        const std::uint8_t* pos_ = nullptr;
        const std::uint8_t* end_ = nullptr;
    };
    Iterator iterate() const;

    // CRC32 of all bytes after the file header.
    std::uint32_t compute_crc32() const;

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
};

// Reads a list of capture files in order, yielding events.
class CaptureReader {
public:
    explicit CaptureReader(const std::vector<std::string>& paths);

    // Returns false when all files are exhausted or on error.
    // On error, error() describes it.
    bool next(EventView& out);
    const char* error() const { return error_.c_str(); }
    const FileHeader* header() const;

private:
    std::vector<std::string> paths_;
    std::size_t file_idx_ = 0;
    MappedFile current_;
    MappedFile::Iterator it_;
    std::string error_;
    bool started_ = false;
};

}  // namespace data
}  // namespace kairos
