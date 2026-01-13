# Plan: Move Non-Public Headers to src/crate

## Overview

Move internal implementation headers from `include/crate/` to `src/crate/` to clearly separate the public API from internal implementation details. This improves API clarity for library users and reduces header pollution.

## Analysis

### Headers That STAY PUBLIC (in `include/crate/`)

These are part of the library's public API:

| Header | Reason |
|--------|--------|
| `crate.hh` | Main entry point, includes public API |
| `core/types.hh` | Basic types (`byte`, `u8`, `byte_span`, etc.) used everywhere |
| `core/error.hh` | Error types (`error`, `result_t`, etc.) returned to users |
| `core/system.hh` | Stream interfaces used by `archive.hh` (`file_output_stream`) |
| `formats/archive.hh` | Base `archive` class and `file_entry` - core public interface |
| `vfs/vfs.hh` | VFS layer - public interface for filesystem-like access |
| `compression/decompressor.hh` | Abstract decompressor interface (for custom decompressors) |

Individual format headers (for users who want specific formats):
| `formats/ace.hh` | `formats/arc.hh` | `formats/arj.hh` |
| `formats/cab.hh` | `formats/chm.hh` | `formats/ha.hh` |
| `formats/hyp.hh` | `formats/kwaj.hh` | `formats/lha.hh` |
| `formats/rar.hh` | `formats/szdd.hh` | `formats/zoo.hh` |

### Headers to MOVE (to `src/crate/`)

These are internal implementation details:

**Compression Internals** (6 files):
- `compression/arj_lz.hh` - ARJ-specific LZ decompression
- `compression/lzh.hh` - LZH decompression algorithm
- `compression/lzss.hh` - LZSS decompression
- `compression/lzx.hh` - LZX decompression (CAB)
- `compression/mszip.hh` - MSZIP decompression (CAB)
- `compression/quantum.hh` - Quantum decompression (CAB)
- `compression/rar_unpack.hh` - RAR decompression (~1600 lines)
- `compression/rar_filters.hh` - RAR filter implementation
- `compression/rar_ppm.hh` - RAR PPM model

**Crypto Internals** (3 files):
- `crypto/aes_decoder.hh` - AES implementation
- `crypto/rar_crypt.hh` - RAR encryption/decryption
- `crypto/sha.hh` - SHA hash implementation

**Core Internals** (5 files):
- `core/bitstream.hh` - Bit-level I/O utilities
- `core/crc.hh` - CRC calculation
- `core/huffman.hh` - Huffman coding
- `core/lru_cache.hh` - LRU cache helper
- `core/path.hh` - Internal path utilities

## Implementation Steps

### Step 1: Create directory structure in src/crate/
```
mkdir -p src/crate/include/crate/{compression,crypto,core}
```

### Step 2: Move internal headers (17 files)

Move from `include/crate/X.hh` to `src/crate/include/crate/X.hh`:

```bash
# Compression (9 files)
mv include/crate/compression/arj_lz.hh src/crate/include/crate/compression/
mv include/crate/compression/lzh.hh src/crate/include/crate/compression/
mv include/crate/compression/lzss.hh src/crate/include/crate/compression/
mv include/crate/compression/lzx.hh src/crate/include/crate/compression/
mv include/crate/compression/mszip.hh src/crate/include/crate/compression/
mv include/crate/compression/quantum.hh src/crate/include/crate/compression/
mv include/crate/compression/rar_unpack.hh src/crate/include/crate/compression/
mv include/crate/compression/rar_filters.hh src/crate/include/crate/compression/
mv include/crate/compression/rar_ppm.hh src/crate/include/crate/compression/

# Crypto (3 files)
mv include/crate/crypto/aes_decoder.hh src/crate/include/crate/crypto/
mv include/crate/crypto/rar_crypt.hh src/crate/include/crate/crypto/
mv include/crate/crypto/sha.hh src/crate/include/crate/crypto/

# Core (5 files)
mv include/crate/core/bitstream.hh src/crate/include/crate/core/
mv include/crate/core/crc.hh src/crate/include/crate/core/
mv include/crate/core/huffman.hh src/crate/include/crate/core/
mv include/crate/core/lru_cache.hh src/crate/include/crate/core/
mv include/crate/core/path.hh src/crate/include/crate/core/
```

### Step 3: Update CMakeLists.txt

Add private include directory for internal headers:

```cmake
target_include_directories(crate
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/crate/include>
)
```

Update header file paths in source list (change from `${PROJECT_SOURCE_DIR}/include/crate/...` to `include/crate/...` for moved files).

### Step 4: Update crate.hh

Remove includes for internal headers that users don't need:
- Remove: `core/bitstream.hh`, `core/huffman.hh`, `core/lru_cache.hh`
- Remove: All compression algorithm includes (keep only `decompressor.hh`)
- The format headers already internally include what they need

### Step 5: Verify includes in format headers

Each format header needs to include its dependencies. For example:
- `formats/rar.hh` includes `compression/rar_unpack.hh` and `crypto/rar_crypt.hh`
- `formats/cab.hh` includes `compression/lzx.hh`, `compression/mszip.hh`, etc.

Since these headers are in `src/crate/include/` (private), the includes will still work from the .cc files because they get the private include path.

### Step 6: Handle format headers that include internal headers

Problem: `formats/rar.hh` (public) includes `compression/rar_unpack.hh` (now private).

Solution options:
1. **Forward declare** internal types in public headers, move implementations to .cc files
2. **Keep some headers public** if needed by format headers
3. **Use PIMPL pattern** for formats that need internal types

For RAR specifically, move the implementation details out of the header:
- Keep `rar_archive` class declaration in public header
- Move decompressor member to PIMPL or use type erasure

## Files to Modify

1. `src/crate/CMakeLists.txt` - Add private include path, update file lists
2. `include/crate/crate.hh` - Remove internal includes
3. `include/crate/formats/rar.hh` - Remove direct include of internals, use forward declarations
4. `include/crate/formats/cab.hh` - Same treatment
5. `include/crate/formats/arj.hh` - Uses arj_lz.hh
6. `include/crate/formats/lha.hh` - Uses lzh.hh

## Verification

After changes:
1. Build should succeed
2. Public headers should not include private headers
3. `#include <crate/crate.hh>` should provide full public API
4. Internal headers should only be accessible to library implementation

## PIMPL Pattern for Format Classes

### Problem

Public format headers currently include internal headers and expose internal types as members:

**rar_archive** (lines 1700-1707):
```cpp
// These expose internal types in the public header:
mutable std::unique_ptr<rar_29_decompressor> solid_decomp_v4_;
mutable std::unique_ptr<rar5_decompressor> solid_decomp_v5_;
mutable std::unique_ptr<crypto::rar_decryptor> decryptor_;
```

**cab_archive** (line 384):
```cpp
mutable lru_cache<u32, byte_vector> folder_cache_;
```

### Solution: PIMPL Pattern

Replace internal type members with an opaque implementation pointer.

**rar_archive** refactored:
```cpp
// In include/crate/formats/rar.hh (public header)
class CRATE_EXPORT rar_archive : public archive {
public:
    // ... public interface unchanged ...
    ~rar_archive(); // Must be declared for PIMPL

private:
    struct impl;
    std::unique_ptr<impl> pimpl_;

    // Keep simple data members here:
    std::vector<byte_vector> volumes_;
    rar::version version_ = rar::V4;
    std::vector<rar::file_info> members_;
    std::vector<file_entry> files_;
    // ... etc
};

// In src/crate/formats/rar.cc (implementation)
#include <crate/compression/rar_unpack.hh>
#include <crate/crypto/rar_crypt.hh>

struct rar_archive::impl {
    std::mutex solid_mutex;
    std::unique_ptr<rar_29_decompressor> solid_decomp_v4;
    std::unique_ptr<rar5_decompressor> solid_decomp_v5;
    int solid_last_extracted = -1;
    std::unique_ptr<crypto::rar_decryptor> decryptor;
};

rar_archive::~rar_archive() = default;
```

**cab_archive** refactored:
```cpp
// In include/crate/formats/cab.hh (public header)
class CRATE_EXPORT cab_archive : public archive {
private:
    struct impl;
    std::unique_ptr<impl> pimpl_;
    // ... simple data members ...
};

// In src/crate/formats/cab.cc
#include <crate/core/lru_cache.hh>
#include <crate/compression/mszip.hh>
#include <crate/compression/lzx.hh>
#include <crate/compression/quantum.hh>

struct cab_archive::impl {
    lru_cache<u32, byte_vector> folder_cache{4};
};
```

### Classes Requiring PIMPL

| Class | Internal Dependencies | PIMPL Members |
|-------|----------------------|---------------|
| `rar_archive` | `rar_29_decompressor`, `rar5_decompressor`, `rar_decryptor` | Solid decompressor state, decryptor |
| `cab_archive` | `lru_cache`, `mszip/lzx/quantum_decompressor` | Folder cache |
| `arj_archive` | `arj_lz_decompressor` | Decompressor |
| `lha_archive` | `lzh_decompressor` | Decompressor |

---

## CRATE_EXPORT Macro Cleanup

### Problem

Many internal classes have `CRATE_EXPORT` but shouldn't be exported since they're not part of the public API. This:
- Increases DLL symbol table size
- Exposes implementation details
- Creates unnecessary ABI commitments

### Classes to REMOVE CRATE_EXPORT from

**compression/** (all internal - 15+ classes):
```cpp
// These should NOT have CRATE_EXPORT after move:
class rar_bit_input { ... };              // was CRATE_EXPORT
struct rar_decode_table { ... };          // was CRATE_EXPORT
struct rar_block_tables_30 { ... };       // was CRATE_EXPORT
struct rar_block_tables { ... };          // was CRATE_EXPORT
class rar_15_decompressor { ... };        // was CRATE_EXPORT
class rar_29_decompressor { ... };        // was CRATE_EXPORT
class rar5_decompressor { ... };          // was CRATE_EXPORT
class mszip_decompressor { ... };         // was CRATE_EXPORT
class lzx_decompressor { ... };           // was CRATE_EXPORT
class quantum_decompressor { ... };       // was CRATE_EXPORT
class lzh_decompressor { ... };           // was CRATE_EXPORT
class lzss_decompressor { ... };          // was CRATE_EXPORT
class arj_lz_decompressor { ... };        // was CRATE_EXPORT
// etc.
```

**crypto/** (all internal - 4 classes):
```cpp
class rar4_key_derivation { ... };        // was CRATE_EXPORT
class rar5_key_derivation { ... };        // was CRATE_EXPORT
class rar_decryptor { ... };              // was CRATE_EXPORT
class aes_decoder { ... };                // was CRATE_EXPORT
class sha1_hasher { ... };                // was CRATE_EXPORT
```

**core/** (internal utilities - 3+ classes):
```cpp
class bitstream_reader { ... };           // was CRATE_EXPORT
template<> class lru_cache { ... };       // was CRATE_EXPORT (if class)
// CRC functions are inline, no export needed
```

### Classes to KEEP CRATE_EXPORT

These are part of the public API or exposed through public accessors:

```cpp
// Public interface classes:
class CRATE_EXPORT archive { ... };
class CRATE_EXPORT file_entry { ... };
class CRATE_EXPORT decompressor { ... };
class CRATE_EXPORT archive_vfs { ... };
class CRATE_EXPORT file_reader { ... };
// etc.

// Format-specific public classes:
class CRATE_EXPORT rar_archive { ... };
class CRATE_EXPORT cab_archive { ... };
// etc.

// Exposed through public accessors (rar::file_info via members()):
struct CRATE_EXPORT rar::file_info { ... };
struct CRATE_EXPORT rar::file_part { ... };
struct CRATE_EXPORT cab::header { ... };
struct CRATE_EXPORT cab::folder { ... };
```

### Implementation

After moving headers to `src/crate/include/`, simply remove `CRATE_EXPORT` from all internal classes:

```bash
# In moved headers, replace:
class CRATE_EXPORT internal_class { ... };
# With:
class internal_class { ... };
```

---

## Implementation Order

The changes should be applied in this order to maintain a buildable state:

### Phase 1: PIMPL Refactoring (before moving headers)

1. Add `struct impl;` forward declaration and `std::unique_ptr<impl> pimpl_` to each format class
2. Move internal-type members into the impl struct in .cc files
3. Update all methods that use moved members to go through `pimpl_->`
4. Verify build still works

### Phase 2: Move Internal Headers

1. Create `src/crate/include/crate/{compression,crypto,core}/` directories
2. Move 17 internal headers
3. Update CMakeLists.txt with private include path
4. Update header paths in CMakeLists.txt source list
5. Remove internal includes from public format headers (they now only need forward decls)
6. Verify build

### Phase 3: Remove CRATE_EXPORT from Internal Classes

1. Remove `CRATE_EXPORT` from all classes in moved headers
2. Verify build (especially shared library builds)

### Phase 4: Clean Up crate.hh

1. Remove includes for internal headers from `crate.hh`
2. Keep only public API includes
3. Verify build

---

## Summary

| Category | Count | Action |
|----------|-------|--------|
| Public headers | 19 | Keep in `include/crate/` |
| Internal headers | 17 | Move to `src/crate/include/crate/` |
| Format headers needing PIMPL | 4 | `rar_archive`, `cab_archive`, `arj_archive`, `lha_archive` |
| Classes losing CRATE_EXPORT | ~20 | All internal classes after move |
| Classes keeping CRATE_EXPORT | ~25 | Public API classes and exposed structs |
