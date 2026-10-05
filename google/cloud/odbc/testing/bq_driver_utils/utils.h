// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef CPP_BIGQUERY_ODBC_GOOGLE_CLOUD_ODBC_TESTING_BQ_DRIVER_UTILS_UTILS_H
#define CPP_BIGQUERY_ODBC_GOOGLE_CLOUD_ODBC_TESTING_BQ_DRIVER_UTILS_UTILS_H

#include "google/cloud/odbc/bq_driver/internal/utils.h"
#include "google/cloud/odbc/internal/odbc_includes.h"
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace google::cloud::odbc_testing_bq_driver_utils {

inline SQLCHAR* CastToSQLCHAR(char const* str) {
  return reinterpret_cast<SQLCHAR*>(const_cast<char*>(str));
}

// Sets the process-wide SQLWCHAR wire encoding for the lifetime of the object
// and restores the build default afterwards, so that tests do not leak the
// encoding into each other.
class ScopedWireEncoding {
 public:
  explicit ScopedWireEncoding(odbc_bq_driver_internal::WireEncoding encoding) {
    odbc_bq_driver_internal::SetWireEncoding(encoding);
  }
  ~ScopedWireEncoding() {
    odbc_bq_driver_internal::SetWireEncoding(
        odbc_bq_driver_internal::WireEncoding::kDefault);
  }
  ScopedWireEncoding(ScopedWireEncoding const&) = delete;
  ScopedWireEncoding& operator=(ScopedWireEncoding const&) = delete;
};

// Decodes `units` wire code units (in the current wire encoding) from a
// SQLWCHAR buffer into UTF-8. Returns "<invalid>" if decoding fails.
inline std::string DecodeWire(void const* buffer, std::size_t units) {
  if (units == 0) return {};
  auto utf8 = odbc_bq_driver_internal::BqConvertSQLWCHARToString(
      static_cast<SQLWCHAR const*>(buffer), static_cast<SQLINTEGER>(units));
  if (!utf8) return "<invalid>";
  std::string result = *utf8;
  // The Windows conversion leaves trailing NULs in the returned string.
  result.erase(result.find_last_not_of('\0') + 1);
  return result;
}

// A caller-owned output buffer of `size` bytes surrounded by guard bytes.
// Writing outside [data(), data() + size()) changes a guard byte, which
// CanariesIntact() reports. The usable bytes start out as kFill so tests can
// also tell which bytes were written.
class CanaryBuffer {
 public:
  static constexpr unsigned char kGuardByte = 0xAB;
  static constexpr unsigned char kFill = 0xCD;
  static constexpr std::size_t kGuardSize = 64;

  explicit CanaryBuffer(std::size_t size)
      : bytes_(size + 2 * kGuardSize, kGuardByte), size_(size) {
    std::fill_n(bytes_.begin() + kGuardSize, size_, kFill);
  }

  void* data() { return bytes_.data() + kGuardSize; }
  unsigned char const* bytes() const { return bytes_.data() + kGuardSize; }
  std::size_t size() const { return size_; }

  bool CanariesIntact() const {
    auto is_guard = [](unsigned char b) { return b == kGuardByte; };
    return std::all_of(bytes_.begin(), bytes_.begin() + kGuardSize, is_guard) &&
           std::all_of(bytes_.end() - kGuardSize, bytes_.end(), is_guard);
  }

  // True if the `unit_size` bytes at code unit index `index` are all zero.
  bool IsNulAt(std::size_t index, std::size_t unit_size) const {
    if ((index + 1) * unit_size > size_) return false;
    auto const* p = bytes() + index * unit_size;
    return std::all_of(p, p + unit_size,
                       [](unsigned char b) { return b == 0; });
  }

 private:
  std::vector<unsigned char> bytes_;
  std::size_t size_;
};

}  // namespace google::cloud::odbc_testing_bq_driver_utils

#endif  // CPP_BIGQUERY_ODBC_GOOGLE_CLOUD_ODBC_TESTING_BQ_DRIVER_UTILS_UTILS_H
