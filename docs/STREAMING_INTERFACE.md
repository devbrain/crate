# Streaming Decompression and Archive Parsing

## Overview

This document defines crate's scope as a streaming-oriented library for decompressing
and parsing archive formats. The library supports non-seekable inputs (pipes, sockets),
chaining decompressors into archive parsers, and extracting files without full
materialization.

VFS functionality is explicitly out of scope - use ares VFS for filesystem abstractions,
with crate archives as backends.

## Goals

- Accept non-seekable inputs (socket/pipe) for decompression and archive parsing.
- Support nested pipelines (e.g., gzip → tar, xz → tar → per-entry decompression).
- Provide streaming extraction without materializing entire archives.
- Maintain existing self-contained archive formats (LHA, RAR, ARJ, etc.).
- Enable format detection for both compression and archive layers.

## Non-Goals

- Virtual filesystem abstraction (use ares VFS).
- Archive creation or modification.
- Async I/O or threading model.
- Full random access without caching.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                 Input Source                         │
│         (file, pipe, socket, memory)                 │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────┐
│              stream_reader                           │
│         (unified streaming interface)                │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────┐
│         Outer Decompressor (optional)                │
│         gzip, bzip2, xz, zstd, lz4                   │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────┐
│              Archive Parser                          │
│      tar, cpio (sequential)                          │
│      lha, rar, arj, etc. (self-contained)            │
└─────────────────────┬───────────────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────────────┐
│         Per-Entry Decompressor (if needed)           │
│         deflate, lzh, implode, etc.                  │
└─────────────────────────────────────────────────────┘
```

## Core Abstractions

### 1. Stream Reader

The fundamental input abstraction for all streaming operations.

```cpp
namespace crate {

class stream_reader {
public:
    virtual ~stream_reader() = default;

    // Read up to buffer.size() bytes. Returns bytes actually read.
    virtual result_t<size_t> read(mutable_byte_span buffer) = 0;

    // True when no more data available.
    [[nodiscard]] virtual bool eof() const = 0;

    // Total size if known (nullopt for pipes/sockets).
    [[nodiscard]] virtual std::optional<u64> size() const = 0;
};

} // namespace crate
```

Design notes:
- `size()` returns `std::optional`, not `result_t<std::optional>` - unknown size is not an error.
- `read()` returns 0 bytes only at EOF, not for temporary unavailability (this is synchronous).
- Implementations must be safe to call `read()` after `eof()` returns true (returns 0).

### 2. Stream Reader Implementations

```cpp
namespace crate {

// Wrap a file descriptor or handle
class file_stream_reader : public stream_reader {
public:
    static result_t<std::unique_ptr<file_stream_reader>> open(const std::filesystem::path& path);
    static std::unique_ptr<file_stream_reader> from_fd(int fd, bool take_ownership = false);
};

// Wrap existing memory
class memory_stream_reader : public stream_reader {
public:
    explicit memory_stream_reader(byte_span data);
    explicit memory_stream_reader(byte_vector data); // takes ownership
};

// Wrap existing byte_span-based archive for streaming interface
class span_stream_reader : public stream_reader {
public:
    explicit span_stream_reader(byte_span data);
};

} // namespace crate
```

### 3. Decompressor Streams

Wrap decompressors as stream readers for pipeline composition.

```cpp
namespace crate {

// Generic wrapper for any decompressor
class decompressor_stream : public stream_reader {
public:
    decompressor_stream(std::unique_ptr<stream_reader> source,
                        std::unique_ptr<decompressor> codec);

    result_t<size_t> read(mutable_byte_span buffer) override;
    [[nodiscard]] bool eof() const override;
    [[nodiscard]] std::optional<u64> size() const override; // usually nullopt
};

// Convenience factories
namespace stream {
    std::unique_ptr<stream_reader> gzip(std::unique_ptr<stream_reader> source);
    std::unique_ptr<stream_reader> bzip2(std::unique_ptr<stream_reader> source);
    std::unique_ptr<stream_reader> xz(std::unique_ptr<stream_reader> source);
    std::unique_ptr<stream_reader> zstd(std::unique_ptr<stream_reader> source);
    std::unique_ptr<stream_reader> lz4(std::unique_ptr<stream_reader> source);

    // Auto-detect compression format from magic bytes
    result_t<std::unique_ptr<stream_reader>> decompress(std::unique_ptr<stream_reader> source);
}

} // namespace crate
```

### 4. Sequential Archive Interface

For streaming archives (tar, cpio), provide an iterator-based interface.

```cpp
namespace crate {

// Entry metadata for sequential archives
struct stream_entry {
    std::string path;
    u64 size;
    entry_type type;          // file, directory, symlink
    file_attributes attribs;
    dos_date_time mtime;
    std::string link_target;  // for symlinks
};

// Sequential archive parser
class stream_archive {
public:
    virtual ~stream_archive() = default;

    // Advance to next entry. Returns nullopt at end of archive.
    virtual result_t<std::optional<stream_entry>> next_entry() = 0;

    // Read current entry's data. Only valid after next_entry() returns a file.
    // Must fully consume entry data before calling next_entry() again.
    virtual result_t<size_t> read_entry(mutable_byte_span buffer) = 0;

    // Skip remaining data in current entry.
    virtual result_t<void> skip_entry() = 0;

    // Check if current entry data is fully consumed.
    [[nodiscard]] virtual bool entry_eof() const = 0;
};

// Concrete implementations
class tar_archive : public stream_archive {
public:
    static result_t<std::unique_ptr<tar_archive>> open(std::unique_ptr<stream_reader> source);
};

class cpio_archive : public stream_archive {
public:
    static result_t<std::unique_ptr<cpio_archive>> open(std::unique_ptr<stream_reader> source);
};

} // namespace crate
```

### 5. Format Detection

```cpp
namespace crate {

enum class compression_format {
    None,
    Gzip,
    Bzip2,
    Xz,
    Zstd,
    Lz4,
    Compress,  // .Z
    Unknown
};

enum class archive_format {
    Tar,
    Cpio,
    Lha,
    Rar,
    Arj,
    Zip,
    SevenZip,
    Ace,
    Arc,
    Cab,
    Chm,
    Ha,
    Hyp,
    Zoo,
    Unknown
};

// Detect compression from magic bytes (needs ~6 bytes)
compression_format detect_compression(byte_span header);

// Detect archive format from magic bytes (needs ~32 bytes)
archive_format detect_archive(byte_span header);

// Peek at stream without consuming (buffers internally)
class peekable_stream : public stream_reader {
public:
    explicit peekable_stream(std::unique_ptr<stream_reader> source);

    // Peek at upcoming bytes without consuming them
    result_t<byte_span> peek(size_t count);

    // stream_reader interface
    result_t<size_t> read(mutable_byte_span buffer) override;
    [[nodiscard]] bool eof() const override;
    [[nodiscard]] std::optional<u64> size() const override;
};

} // namespace crate
```

## Self-Contained Archives

Existing archive formats that handle their own compression remain unchanged.
They continue to use the `byte_span` interface for random access.

```cpp
// Existing interface preserved
class lha_archive : public archive {
public:
    static result_t<std::unique_ptr<lha_archive>> open(byte_span data);
};

// Same for: rar_archive, arj_archive, ace_archive, cab_archive, etc.
```

For these formats, the typical usage remains:

```cpp
auto data = read_file("archive.lha");
auto archive = lha_archive::open(data).value();
for (const auto& entry : archive->files()) {
    auto content = archive->extract(entry).value();
}
```

## Pipeline Examples

### Basic: tar.gz extraction

```cpp
using namespace crate;

auto file = file_stream_reader::open("data.tar.gz").value();
auto decompressed = stream::gzip(std::move(file));
auto tar = tar_archive::open(std::move(decompressed)).value();

while (auto entry = tar->next_entry().value()) {
    if (entry->type == entry_type::File) {
        std::vector<u8> content(entry->size);
        tar->read_entry(content).value();
        // process content
    } else {
        tar->skip_entry().value();
    }
}
```

### Auto-detect compression

```cpp
using namespace crate;

auto file = file_stream_reader::open("data.tar.???").value();
auto decompressed = stream::decompress(std::move(file)).value();
auto tar = tar_archive::open(std::move(decompressed)).value();
// ...
```

### Pipe input (non-seekable)

```cpp
using namespace crate;

// stdin as stream reader
auto input = file_stream_reader::from_fd(STDIN_FILENO);
auto decompressed = stream::gzip(std::move(input));
auto tar = tar_archive::open(std::move(decompressed)).value();
// ...
```

### Nested: tar.gz containing .lha files

```cpp
using namespace crate;

auto file = file_stream_reader::open("bundle.tar.gz").value();
auto tar = tar_archive::open(stream::gzip(std::move(file))).value();

while (auto entry = tar->next_entry().value()) {
    if (entry->path.ends_with(".lha")) {
        // Must materialize for random-access archive
        std::vector<u8> lha_data(entry->size);
        tar->read_entry(lha_data).value();

        auto lha = lha_archive::open(lha_data).value();
        for (const auto& inner : lha->files()) {
            auto content = lha->extract(inner).value();
            // process
        }
    }
}
```

## Error Handling

Extend existing error codes:

```cpp
enum class error_code {
    // Existing codes...

    // Streaming-specific
    UnexpectedEof,          // Stream ended mid-record
    CompressionError,       // Decompressor failure
    UnsupportedCompression, // Unknown compression format
    EntryNotConsumed,       // Called next_entry() without consuming previous
    InvalidTarHeader,
    InvalidCpioHeader,
};
```

## Buffering Strategy

Internal buffer sizes for streaming operations:

| Component | Buffer Size | Notes |
|-----------|-------------|-------|
| Decompressor input | 16 KB | Read-ahead for compression codecs |
| Decompressor output | 64 KB | Decompressed data staging |
| Archive parser | 512 bytes | Header parsing (tar: 512, cpio: varies) |
| Peek buffer | 64 bytes | Format detection |

Buffer sizes should be configurable for embedded environments.

## Implementation Phases

### Phase 1: Core streaming infrastructure (in progress)
- `output_stream` base class (done)
- `decompressor::supports_streaming` and `decompress_stream` helper (done)
- `archive::extract_to` streaming surface (done)
- `stream_reader` base class and implementations (pending)
- `peekable_stream` for format detection (pending)
- `decompressor_stream` wrapper (pending)

### Phase 2: Compression formats (in progress)
- mark existing codecs as streaming-capable in the decompressor API (done)
- gzip stream (wrap existing inflate) (pending)
- bzip2 stream (new or wrap libbz2) (pending)
- xz stream (wrap lzma) (pending)
- Auto-detection (pending)

### Phase 3: Sequential archives (pending)
- tar parser (ustar, pax, gnu extensions) (pending)
- cpio parser (bin, odc, newc formats) (pending)

### Phase 4: Integration (in progress)
- random-access archive extraction via `extract_to` (ARJ, ZOO initial coverage) (done)
- Format detection pipeline (pending)
- Convenience functions for common patterns (pending)
- Documentation and examples (in progress)

## Streaming Codec Status (current)

True streaming (supports partial input/output):
- deflate/zlib/gzip
- bzip2
- xz/lzma
- zstd
- brotli
- diet
- pkware explode
- mszip
- szdd lzss
- szdd container
- kwaj container

Bounded streaming (requires expected output size):
- lzx
- quantum

Buffered fallback (requires full input today):
- lzh
- arj method 4
- rar unpackers

## Testing Plan

1. **Stream reader tests** (pending)
   - File-based reader with known content
   - Memory reader round-trip
   - Pipe simulation (non-seekable)

2. **Decompressor stream tests** (partial)
   - Each format: compress → stream decompress → verify
   - Chunked reads (1 byte, 13 bytes, 4KB, etc.)
   - Truncated input handling
   - PKWARE explode streaming coverage (small/medium/large/implicit end) (done)
   - SZDD streaming coverage (done)
   - KWAJ streaming coverage (done)

3. **Sequential archive tests** (pending)
   - tar: ustar, pax, gnu, sparse files
   - cpio: binary, odc, newc
   - Various file types (regular, directory, symlink)
   - Empty archives, single file, many files

4. **Pipeline integration tests** (pending)
   - tar.gz end-to-end
   - tar.xz end-to-end
   - Auto-detection accuracy
   - Nested archive extraction

5. **Random-access archive extract_to tests** (partial)
   - ARJ LZH and ZOO stored streaming extraction (done)

## Relationship with Ares VFS

Crate provides archive parsing; ares provides filesystem abstraction.

Integration pattern for ares VFS modules:

```cpp
// In ares VFS module
class crate_tar_module : public archived_fs<tar_entry> {
    std::unique_ptr<crate::tar_archive> archive_;

    // Implement ares VFS interface using crate::tar_archive
};
```

This keeps crate focused on parsing/extraction while ares handles mounting,
path resolution, and filesystem semantics.
