// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/odbc/bq_driver/internal/odbc_type_utils.h"
#include "google/cloud/odbc/bq_driver/internal/trace_utils.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace google::cloud::odbc_bq_driver_internal {

// During program execution out_buf is never NULL
SQLRETURN AddressToPointer(SQLPOINTER ptr, SQLPOINTER out_buf,
                           SQLINTEGER* str_len_ptr) {
  if (out_buf) {
    *(static_cast<SQLPOINTER*>(out_buf)) = ptr;
  }
  if (str_len_ptr) {
    *str_len_ptr = static_cast<SQLINTEGER>(sizeof(SQLPOINTER));
  }
  return SQL_SUCCESS;
}

// During program execution out_buf is never NULL
SQLRETURN AddressToPointer(SQLPOINTER ptr, SQLPOINTER out_buf,
                           SQLSMALLINT* str_len_ptr) {
  if (out_buf) {
    *(static_cast<SQLPOINTER*>(out_buf)) = ptr;
  }
  if (str_len_ptr) {
    *str_len_ptr = static_cast<SQLSMALLINT>(sizeof(SQLPOINTER));
  }
  return SQL_SUCCESS;
}

namespace {

#if !defined(_WIN32)
constexpr char32_t kReplacementCharacter = 0xFFFD;

char32_t ToCodePoint(wchar_t c) {
  auto const cp =
      static_cast<char32_t>(static_cast<std::make_unsigned_t<wchar_t>>(c));
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return kReplacementCharacter;
  }
  return cp;
}

template <typename Unit>
void AppendUnit(std::string& out, Unit unit) {
  char bytes[sizeof(Unit)];
  std::memcpy(bytes, &unit, sizeof(Unit));
  out.append(bytes, sizeof(Unit));
}

void AppendUtf8(std::string& out, char32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}
#endif  // !defined(_WIN32)

// True if the code unit at `index` continues a character that starts at an
// earlier unit, i.e. cutting the string at `index` would split a character.
bool IsContinuationUnit(std::string_view encoded, std::size_t index,
                        std::size_t wire_sz) {
  if (wire_sz == 1) {
    return (static_cast<unsigned char>(encoded[index]) & 0xC0) == 0x80;
  }
  if (wire_sz == 2) {
    std::uint16_t unit;
    std::memcpy(&unit, encoded.data() + index * wire_sz, sizeof(unit));
    return unit >= 0xDC00 && unit <= 0xDFFF;
  }
  return false;
}

}  // namespace

std::string EncodeWideToWire(std::wstring_view src) {
  std::string out;
#if defined(_WIN32)
  out.assign(reinterpret_cast<char const*>(src.data()),
             src.size() * sizeof(wchar_t));
#else
  switch (GetEffectiveWireEncoding()) {
    case WireEncoding::kUtf8:
      out.reserve(src.size());
      for (wchar_t c : src) AppendUtf8(out, ToCodePoint(c));
      break;
    case WireEncoding::kUtf16Le:
      out.reserve(src.size() * sizeof(std::uint16_t));
      for (wchar_t c : src) {
        char32_t cp = ToCodePoint(c);
        if (cp < 0x10000) {
          AppendUnit(out, static_cast<std::uint16_t>(cp));
        } else {
          cp -= 0x10000;
          AppendUnit(out, static_cast<std::uint16_t>(0xD800 + (cp >> 10)));
          AppendUnit(out, static_cast<std::uint16_t>(0xDC00 + (cp & 0x3FF)));
        }
      }
      break;
    case WireEncoding::kUtf32Le:
    case WireEncoding::kDefault:
      out.reserve(src.size() * sizeof(std::uint32_t));
      for (wchar_t c : src) {
        AppendUnit(out, static_cast<std::uint32_t>(ToCodePoint(c)));
      }
      break;
  }
#endif  // defined(_WIN32)
  return out;
}

WireCopyResult CopyWireUnitsToBuffer(std::string_view encoded, void* dest,
                                     std::size_t dest_units) {
  std::size_t const wire_sz = WireWcharSize();
  WireCopyResult result;
  result.total_units = encoded.size() / wire_sz;
  if (dest == nullptr) return result;
  result.truncated = result.total_units >= dest_units;
  if (dest_units == 0) return result;

  std::size_t n = std::min(result.total_units, dest_units - 1);
  if (result.truncated) {
    while (n > 0 && IsContinuationUnit(encoded, n, wire_sz)) --n;
  }
  auto* out = static_cast<unsigned char*>(dest);
  std::memcpy(out, encoded.data(), n * wire_sz);
  std::memset(out + n * wire_sz, 0, wire_sz);
  result.copied_units = n;
  return result;
}

WireCopyResult CopyWideToWireBuffer(std::wstring_view src, void* dest,
                                    std::size_t dest_units) {
  return CopyWireUnitsToBuffer(EncodeWideToWire(src), dest, dest_units);
}

odbc_internal::StatusRecordOr<WireCopyResult> CopyUtf8ToWireBuffer(
    std::string_view src, void* dest, std::size_t dest_units) {
  auto wide = Utf8ToUtf16(src);
  if (!wide) return wide.GetStatusRecord();
  return CopyWideToWireBuffer(*wide, dest, dest_units);
}

std::size_t WireUnitsForBytes(SQLLEN byte_len) {
  if (byte_len <= 0) return 0;
  return static_cast<std::size_t>(byte_len) / WireWcharSize();
}

odbc_internal::StatusRecord WStrToOutputBufferResponse(std::wstring const& wstr,
                                                       SQLPOINTER dest_buf,
                                                       SQLLEN buffer_length,
                                                       SQLINTEGER supp_max_len,
                                                       SQLLEN* res_len) {
  auto const wire_sz = static_cast<SQLLEN>(WireWcharSize());
  std::string const encoded = EncodeWideToWire(wstr);
  auto const total = static_cast<SQLLEN>(encoded.size()) / wire_sz;
  std::size_t const capacity =
      buffer_length > 0 ? static_cast<std::size_t>(buffer_length) : 0;

  if (total == 0) {
    CopyWireUnitsToBuffer(encoded, dest_buf, capacity);
    if (res_len) *res_len = 0;
    return odbc_internal::StatusRecord::Ok();
  }
  if (buffer_length > total) {
    CopyWireUnitsToBuffer(encoded, dest_buf, capacity);
    if (res_len) *res_len = total * wire_sz;
    return odbc_internal::StatusRecord::Ok();
  }
  if (supp_max_len <= buffer_length) {
    CopyWireUnitsToBuffer(encoded, dest_buf, capacity);
    if (res_len) *res_len = buffer_length * wire_sz;
    return odbc_internal::StatusRecord{
        google::cloud::odbc_internal::SQLStates::k_01004(), "Data truncated"};
  }
  return odbc_internal::StatusRecord{
      google::cloud::odbc_internal::SQLStates::k_22003(),
      "Buffer length is insufficient"};
}

odbc_internal::StatusRecord WStrIntervalBufferResponse(
    std::wstring const& wstr, SQLPOINTER dest_buf, SQLLEN buffer_length,
    SQLINTEGER whole_digits_count, SQLLEN* res_len) {
  auto const wire_sz = static_cast<SQLLEN>(WireWcharSize());
  std::string const encoded = EncodeWideToWire(wstr);
  auto const total = static_cast<SQLLEN>(encoded.size()) / wire_sz;
  std::size_t const capacity =
      buffer_length > 0 ? static_cast<std::size_t>(buffer_length) : 0;

  if (buffer_length > total) {
    CopyWireUnitsToBuffer(encoded, dest_buf, capacity);
    if (res_len) *res_len = total * wire_sz;
    return odbc_internal::StatusRecord::Ok();
  }
  if (buffer_length > whole_digits_count) {
    CopyWireUnitsToBuffer(encoded, dest_buf, capacity);
    if (res_len) *res_len = buffer_length * wire_sz;
    return odbc_internal::StatusRecord{
        google::cloud::odbc_internal::SQLStates::k_01004(), "Data truncated"};
  }
  LOG(ERROR) << "WStrIntervalBufferResponse:: Buffer length is insufficient.";
  return odbc_internal::StatusRecord{
      google::cloud::odbc_internal::SQLStates::k_22003(),
      "Buffer length is insufficient"};
}
}  // namespace google::cloud::odbc_bq_driver_internal
