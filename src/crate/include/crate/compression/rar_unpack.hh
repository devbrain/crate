#pragma once

// This header provides backward compatibility by including all RAR decompressor components.
// For new code, prefer including specific headers directly:
//   - rar_common.hh  - Common utilities (bit input, decode tables, constants)
//   - rar_15.hh      - RAR 1.5 decompressor
//   - rar_29.hh      - RAR 2.9/3.x decompressor
//   - rar5.hh        - RAR 5.x decompressor

#include <crate/compression/rar_common.hh>
#include <crate/compression/rar_15.hh>
#include <crate/compression/rar_29.hh>
#include <crate/compression/rar5.hh>
