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

#ifndef CPP_BIGQUERY_ODBC_GOOGLE_CLOUD_ODBC_BQ_DRIVER_INTERNAL_ODBC_TYPE_UTILS_H
#define CPP_BIGQUERY_ODBC_GOOGLE_CLOUD_ODBC_BQ_DRIVER_INTERNAL_ODBC_TYPE_UTILS_H

#include "google/cloud/odbc/bq_driver/internal/utils.h"
#include "google/cloud/odbc/internal/diagnostic_records.h"
#include "google/cloud/odbc/internal/sql_state_constants.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace google::cloud::odbc_bq_driver_internal {

struct DataBuffer {
  // C data type of the data the application expects
  SQLSMALLINT type;

  // Pointer to the buffer provided by the application
  SQLPOINTER buf;

  // Length of the buffer provided by the application
  SQLLEN buflen;

  // Length of the result populated by the driver
  SQLLEN* result_len;
};

struct Interval {
  SQLSMALLINT concise_sql_type;
  SQLSMALLINT concise_c_type;
  SQLSMALLINT datetime_interval_code;
};

static std::vector<Interval> const kDatetimeTypes = {
    {SQL_TYPE_DATE, SQL_C_TYPE_DATE, SQL_CODE_DATE},
    {SQL_TYPE_TIME, SQL_C_TYPE_TIME, SQL_CODE_TIME},
    {SQL_TYPE_TIMESTAMP, SQL_C_TYPE_TIMESTAMP, SQL_CODE_TIMESTAMP},
};

static std::vector<Interval> const kIntervalTypes = {
    {SQL_INTERVAL_MONTH, SQL_C_INTERVAL_MONTH, SQL_CODE_MONTH},
    {SQL_INTERVAL_YEAR, SQL_C_INTERVAL_YEAR, SQL_CODE_YEAR},
    {SQL_INTERVAL_YEAR_TO_MONTH, SQL_C_INTERVAL_YEAR_TO_MONTH,
     SQL_CODE_YEAR_TO_MONTH},
    {SQL_INTERVAL_DAY, SQL_C_INTERVAL_DAY, SQL_CODE_DAY},
    {SQL_INTERVAL_HOUR, SQL_C_INTERVAL_HOUR, SQL_CODE_HOUR},
    {SQL_INTERVAL_MINUTE, SQL_C_INTERVAL_MINUTE, SQL_CODE_MINUTE},
    {SQL_INTERVAL_SECOND, SQL_C_INTERVAL_SECOND, SQL_CODE_SECOND},
    {SQL_INTERVAL_DAY_TO_HOUR, SQL_C_INTERVAL_DAY_TO_HOUR,
     SQL_CODE_DAY_TO_HOUR},
    {SQL_INTERVAL_DAY_TO_MINUTE, SQL_C_INTERVAL_DAY_TO_MINUTE,
     SQL_CODE_DAY_TO_MINUTE},
    {SQL_INTERVAL_DAY_TO_SECOND, SQL_C_INTERVAL_DAY_TO_SECOND,
     SQL_CODE_DAY_TO_SECOND},
    {SQL_INTERVAL_HOUR_TO_MINUTE, SQL_C_INTERVAL_HOUR_TO_MINUTE,
     SQL_CODE_HOUR_TO_MINUTE},
    {SQL_INTERVAL_HOUR_TO_SECOND, SQL_C_INTERVAL_HOUR_TO_SECOND,
     SQL_CODE_HOUR_TO_SECOND},
    {SQL_INTERVAL_MINUTE_TO_SECOND, SQL_C_INTERVAL_MINUTE_TO_SECOND,
     SQL_CODE_MINUTE_TO_SECOND},
};

static std::vector<int> const kOtherSQLSupportedTypes = {
    SQL_CHAR,     SQL_VARCHAR,      SQL_LONGVARCHAR,   SQL_WCHAR,
    SQL_WVARCHAR, SQL_WLONGVARCHAR, SQL_DECIMAL,       SQL_NUMERIC,
    SQL_SMALLINT, SQL_INTEGER,      SQL_REAL,          SQL_FLOAT,
    SQL_DOUBLE,   SQL_BIT,          SQL_TINYINT,       SQL_BIGINT,
    SQL_BINARY,   SQL_VARBINARY,    SQL_LONGVARBINARY, SQL_GUID};

static std::vector<int> const kOtherCSupportedTypes = {
    SQL_C_CHAR,    SQL_C_WCHAR,    SQL_C_SSHORT,      SQL_C_USHORT,
    SQL_C_SLONG,   SQL_C_ULONG,    SQL_C_FLOAT,       SQL_C_DOUBLE,
    SQL_C_BIT,     SQL_C_STINYINT, SQL_C_UTINYINT,    SQL_C_SBIGINT,
    SQL_C_UBIGINT, SQL_C_BINARY,   SQL_C_VARBOOKMARK, SQL_C_NUMERIC,
    SQL_C_GUID};

// NOLINTBEGIN(performance-no-int-to-ptr)
template <typename T>
inline SQLPOINTER ToSqlPointer(T x) {
  return reinterpret_cast<SQLPOINTER>(x);
}
// NOLINTEND(performance-no-int-to-ptr)

// U usually can be SQLINTEGER, SQLSMALLINT or SQLLEN
template <typename U>
odbc_internal::StatusRecord StringValueToOutputBufferResponse(
    std::string_view src, SQLPOINTER buffer_ptr, U buffer_len, U* str_len_ptr) {
  auto src_len = src.length();
  if (str_len_ptr) {
    *str_len_ptr = static_cast<U>(src_len);
  }
  if (!buffer_ptr) {
    return odbc_internal::StatusRecord::Ok();
  }
  if (buffer_len < 0) {
    return odbc_internal::StatusRecord{odbc_internal::SQLStates::k_HY090(),
                                       "Buffer length is negative"};
  }

  char* dest = reinterpret_cast<char*>(buffer_ptr);

  if (src_len == 0 || buffer_len == 0) {
    *dest = '\0';
    if (str_len_ptr) {
      *str_len_ptr = 0;
    }
    return odbc_internal::StatusRecord::Ok();
  }
  if (src_len < buffer_len) {
    std::memcpy(dest, src.data(), src_len);
    dest[src_len] = '\0';
    if (str_len_ptr) {
      *str_len_ptr = static_cast<U>(src_len);
    }
    return odbc_internal::StatusRecord::Ok();
  }
  std::memcpy(dest, src.data(), (buffer_len - 1));
  dest[buffer_len - 1] = '\0';
  if (str_len_ptr) {
    *str_len_ptr = static_cast<U>(buffer_len - 1);
  }
  return odbc_internal::StatusRecord{odbc_internal::SQLStates::k_01004(),
                                     "String data, right truncated"};
}

inline odbc_internal::StatusRecord StringValueToOutputBufferResponse(
    std::string_view src, DataBuffer& dest_data) {
  return StringValueToOutputBufferResponse<SQLLEN>(
      src, dest_data.buf, dest_data.buflen, dest_data.result_len);
}

inline odbc_internal::StatusRecord TimestampToOutputBufferResponse(
    const SQL_TIMESTAMP_STRUCT& conn_timestamp, SQLPOINTER dest_buf,
    SQLLEN* result_len) {
  auto* dest_timestamp = reinterpret_cast<SQL_TIMESTAMP_STRUCT*>(dest_buf);

  if (result_len) {
    *result_len = sizeof(SQL_TIMESTAMP_STRUCT);
  }

  dest_timestamp->year = conn_timestamp.year;
  dest_timestamp->month = conn_timestamp.month;
  dest_timestamp->day = conn_timestamp.day;
  dest_timestamp->hour = conn_timestamp.hour;
  dest_timestamp->minute = conn_timestamp.minute;
  dest_timestamp->second = conn_timestamp.second;
  dest_timestamp->fraction = conn_timestamp.fraction;

  return odbc_internal::StatusRecord::Ok();
}

// T usually can be SQLINTEGER, SQLSMALLINT, SQLLEN, and it's unsigned values
// U usually can be SQLINTEGER and SQLSMALLINT
template <typename T, typename U>
SQLRETURN IntValueToOutputBufferResponse(T val, SQLPOINTER buffer_ptr,
                                         U* str_len_ptr) {
  if (str_len_ptr) {
    *str_len_ptr = static_cast<U>(sizeof(T));
  }
  if (buffer_ptr) {
    auto* val_ptr = reinterpret_cast<T*>(buffer_ptr);
    *val_ptr = val;
  }
  return SQL_SUCCESS;
}

// Encodes `src` into the configured SQLWCHAR wire encoding (see
// GetEffectiveWireEncoding()). `src` holds UTF-16 on Windows and UTF-32
// elsewhere, as produced by Utf8ToUtf16(). The result is a whole number of
// wire code units of WireWcharSize() bytes each, without a NUL terminator.
// Code points that cannot be represented are replaced with U+FFFD.
std::string EncodeWideToWire(std::wstring_view src);

// The outcome of copying a string into a caller-owned SQLWCHAR buffer. All
// counts are in wire code units and exclude the NUL terminator.
struct WireCopyResult {
  std::size_t total_units = 0;   // length of the whole string
  std::size_t copied_units = 0;  // units written to the buffer
  bool truncated = false;        // the buffer could not hold string + NUL
};

// Copies the wire-encoded string `encoded` (from EncodeWideToWire()) into
// `dest`, a caller-owned buffer that holds `dest_units` wire code units.
//
// Never writes more than `dest_units * WireWcharSize()` bytes. When
// `dest_units > 0` the output is always NUL-terminated inside the buffer, and
// truncation never splits a UTF-16 surrogate pair or a UTF-8 sequence. When
// `dest` is null or `dest_units` is 0 nothing is written; the result still
// reports the full length, and `truncated` is set only for a non-null `dest`.
WireCopyResult CopyWireUnitsToBuffer(std::string_view encoded, void* dest,
                                     std::size_t dest_units);

// Encodes `src` and copies it into `dest`; see CopyWireUnitsToBuffer().
WireCopyResult CopyWideToWireBuffer(std::wstring_view src, void* dest,
                                    std::size_t dest_units);

// Converts UTF-8 `src` to the wire encoding and copies it into `dest`; see
// CopyWireUnitsToBuffer(). Fails only if `src` is not valid UTF-8.
odbc_internal::StatusRecordOr<WireCopyResult> CopyUtf8ToWireBuffer(
    std::string_view src, void* dest, std::size_t dest_units);

// Number of whole wire code units that fit in `byte_len` bytes, for the W
// APIs whose buffer lengths are given in bytes. Negative lengths yield 0.
std::size_t WireUnitsForBytes(SQLLEN byte_len);

// Converts a length to the integer type of an ODBC length output argument,
// saturating at the type's maximum.
template <typename T>
T SaturateLength(std::size_t len) {
  return static_cast<T>(std::min<std::size_t>(
      len, static_cast<std::size_t>(std::numeric_limits<T>::max())));
}

// Copies `wstr` into the SQL_C_WCHAR buffer `dest_buf`, which holds
// `buffer_length` wire code units, and sets `*res_len` to a length in bytes:
//  - If the string and its NUL fit, it is copied and `*res_len` is its length.
//  - Else if `supp_max_len <= buffer_length`, the string is truncated to fit
//    (NUL-terminated), `*res_len` is `buffer_length` units, and 01004 is
//    returned.
//  - Else nothing is written and 22003 is returned.
odbc_internal::StatusRecord WStrToOutputBufferResponse(std::wstring const& wstr,
                                                       SQLPOINTER dest_buf,
                                                       SQLLEN buffer_length,
                                                       SQLINTEGER supp_max_len,
                                                       SQLLEN* res_len);

SQLRETURN AddressToPointer(SQLPOINTER ptr, SQLPOINTER out_buf,
                           SQLINTEGER* str_len_ptr);

SQLRETURN AddressToPointer(SQLPOINTER ptr, SQLPOINTER out_buf,
                           SQLSMALLINT* str_len_ptr);

// Like WStrToOutputBufferResponse(), for interval strings: truncation is
// allowed (01004) only while all `whole_digits_count` whole digits still fit,
// otherwise 22003 is returned and nothing is written.
odbc_internal::StatusRecord WStrIntervalBufferResponse(
    std::wstring const& wstr, SQLPOINTER dest_buf, SQLLEN buffer_length,
    SQLINTEGER whole_digits_count, SQLLEN* res_len);
}  // namespace google::cloud::odbc_bq_driver_internal

#endif  // CPP_BIGQUERY_ODBC_GOOGLE_CLOUD_ODBC_BQ_DRIVER_INTERNAL_ODBC_TYPE_UTILS_H
