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
#include "google/cloud/odbc/bq_driver/internal/utils.h"
#include "google/cloud/odbc/internal/diagnostic_records.h"
#include "google/cloud/odbc/testing/bq_driver_utils/utils.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace google::cloud::odbc_bq_driver_internal {

using google::cloud::odbc_internal::SQLStates;
using google::cloud::odbc_internal::StatusRecord;
using google::cloud::odbc_testing_bq_driver_utils::CanaryBuffer;
using google::cloud::odbc_testing_bq_driver_utils::DecodeWire;
using google::cloud::odbc_testing_bq_driver_utils::ScopedWireEncoding;

TEST(StringValueToOutputBufferResponse,
     SuccessWhenDestBufferLenGreaterThanSrcLen) {
  std::string expected = "sample-test";
  SQLSMALLINT str_len;
  SQLSMALLINT buffer_len = 15;
  SQLCHAR dest[15];

  StatusRecord status_record = StringValueToOutputBufferResponse(
      expected.c_str(), dest, buffer_len, &str_len);

  ASSERT_TRUE(status_record.ok());
  std::string actual = reinterpret_cast<char*>(dest);
  EXPECT_EQ("sample-test", actual);
  EXPECT_EQ(11, str_len);
}

TEST(StringValueToOutputBufferResponse,
     SuccessWhenDestBufferLenGreaterThanSrcLenWithSqlIntegerOutputExplicit) {
  std::string expected = "sample-test";
  SQLINTEGER str_len;
  SQLINTEGER buffer_len = 15;
  SQLCHAR dest[15];

  StatusRecord status_record = StringValueToOutputBufferResponse<SQLINTEGER>(
      expected.c_str(), dest, buffer_len, &str_len);

  ASSERT_TRUE(status_record.ok());
  std::string actual = reinterpret_cast<char*>(dest);
  EXPECT_EQ("sample-test", actual);
  EXPECT_EQ(11, str_len);
}

TEST(StringValueToOutputBufferResponse,
     SuccessWhenDestBufferLenGreaterThanSrcLenWithSqlIntegerOutputImplicit) {
  std::string expected = "sample-test";
  SQLINTEGER str_len;
  SQLINTEGER buffer_len = 15;
  SQLCHAR dest[15];

  StatusRecord status_record = StringValueToOutputBufferResponse(
      expected.c_str(), dest, buffer_len, &str_len);

  ASSERT_TRUE(status_record.ok());
  std::string actual = reinterpret_cast<char*>(dest);
  EXPECT_EQ("sample-test", actual);
  EXPECT_EQ(11, str_len);
}

TEST(StringValueToOutputBufferResponse,
     SuccessWithInfoWhenDestBufferLenLessThanSrcLen) {
  std::string expected = "sample-test";
  SQLSMALLINT str_len;
  SQLSMALLINT buffer_len = 5;
  SQLCHAR dest[5];

  StatusRecord status_record = StringValueToOutputBufferResponse(
      expected.c_str(), dest, buffer_len, &str_len);

  ASSERT_FALSE(status_record.ok());
  EXPECT_EQ(SQLStates::k_01004(), status_record.sql_state);
  EXPECT_EQ("String data, right truncated", status_record.message);
  std::string actual = reinterpret_cast<char*>(dest);
  EXPECT_EQ("samp", actual);
  EXPECT_EQ(4, str_len);
}

TEST(StringValueToOutputBufferResponse,
     SuccessWithInfoWhenDestBufferLenEqualsSrcLen) {
  std::string expected = "sampl";
  SQLSMALLINT str_len;
  SQLSMALLINT buffer_len = 5;
  SQLCHAR dest[5];

  StatusRecord status_record = StringValueToOutputBufferResponse(
      expected.c_str(), dest, buffer_len, &str_len);

  ASSERT_FALSE(status_record.ok());
  EXPECT_EQ(SQLStates::k_01004(), status_record.sql_state);
  EXPECT_EQ("String data, right truncated", status_record.message);
  std::string actual = reinterpret_cast<char*>(dest);
  EXPECT_EQ("samp", actual);
  EXPECT_EQ(4, str_len);
}

TEST(StringValueToOutputBufferResponse, SuccessWhenDestBufferLenIsZero) {
  std::string expected = "sample-test";
  SQLSMALLINT str_len;
  SQLSMALLINT buffer_len = 0;
  SQLCHAR dest[15];

  StatusRecord status_record = StringValueToOutputBufferResponse(
      expected.c_str(), dest, buffer_len, &str_len);

  ASSERT_TRUE(status_record.ok());
  std::string actual = reinterpret_cast<char*>(dest);
  EXPECT_EQ("", actual);
  EXPECT_EQ(0, str_len);
}

TEST(StringValueToOutputBufferResponse, SuccessWhenStcLenLenIsZero) {
  std::string expected;
  SQLSMALLINT str_len;
  SQLSMALLINT buffer_len = 15;
  SQLCHAR dest[15];

  StatusRecord status_record = StringValueToOutputBufferResponse(
      expected.c_str(), dest, buffer_len, &str_len);

  ASSERT_TRUE(status_record.ok());
  std::string actual = reinterpret_cast<char*>(dest);
  EXPECT_EQ("", actual);
  EXPECT_EQ(0, str_len);
}

TEST(StringValueToOutputBufferResponse, FailureWhenBufferLenIsNegative) {
  std::string expected = "sample-test";
  SQLSMALLINT str_len;
  SQLSMALLINT buffer_len = -15;
  SQLCHAR dest[15];

  StatusRecord status_record = StringValueToOutputBufferResponse(
      expected.c_str(), dest, buffer_len, &str_len);

  ASSERT_FALSE(status_record.ok());
  EXPECT_EQ(SQLStates::k_HY090(), status_record.sql_state);
  EXPECT_EQ("Buffer length is negative", status_record.message);
  EXPECT_EQ(11, str_len);
}

TEST(IntValueToOutputBufferResponse, SuccessWithSqlInteger) {
  int expected = 42;
  SQLSMALLINT str_len;
  SQLINTEGER dest[15];

  SQLRETURN return_code =
      IntValueToOutputBufferResponse<SQLINTEGER>(expected, dest, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(42, *dest);
  EXPECT_EQ(sizeof(SQLINTEGER), str_len);
}

TEST(IntValueToOutputBufferResponse, SuccessWhenDestIsNull) {
  int expected = 42;
  SQLSMALLINT str_len;

  SQLRETURN return_code =
      IntValueToOutputBufferResponse<SQLINTEGER>(expected, nullptr, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(sizeof(SQLINTEGER), str_len);
}

TEST(IntValueToOutputBufferResponse, SuccessWithSqlLen) {
  int expected = 42;
  SQLSMALLINT str_len;
  SQLLEN dest[15];

  SQLRETURN return_code =
      IntValueToOutputBufferResponse<SQLLEN>(expected, dest, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(42, *dest);
  EXPECT_EQ(sizeof(SQLLEN), str_len);
}

TEST(IntValueToOutputBufferResponse, SuccessWithSqlLenOutputSqlInteger) {
  int expected = 42;
  SQLINTEGER str_len;
  SQLLEN dest[15];

  SQLRETURN return_code = IntValueToOutputBufferResponse<SQLLEN, SQLINTEGER>(
      expected, dest, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(42, *dest);
  EXPECT_EQ(sizeof(SQLLEN), str_len);
}

TEST(IntValueToOutputBufferResponse, SuccessWithImplicitSqlLen) {
  SQLLEN expected = 42;
  SQLSMALLINT str_len;
  SQLLEN dest[15];

  SQLRETURN return_code =
      IntValueToOutputBufferResponse(expected, dest, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(42, *dest);
  EXPECT_EQ(sizeof(SQLLEN), str_len);
}

TEST(IntValueToOutputBufferResponse,
     SuccessWithImplicitSqlLenOutputSqlInteger) {
  SQLLEN expected = 42;
  SQLINTEGER str_len;
  SQLLEN dest[15];

  SQLRETURN return_code =
      IntValueToOutputBufferResponse(expected, dest, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(42, *dest);
  EXPECT_EQ(sizeof(SQLLEN), str_len);
}

TEST(AddressToPointer, SetPointer) {
  SQLSMALLINT ptr[] = {1, 2, 3};
  SQLSMALLINT* out_buf = nullptr;
  SQLINTEGER str_len = 0;

  SQLRETURN return_code = AddressToPointer(ptr, &out_buf, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(ptr, out_buf);
  EXPECT_EQ(1, out_buf[0]);
  EXPECT_EQ(2, out_buf[1]);
  EXPECT_EQ(3, out_buf[2]);
  EXPECT_EQ(sizeof(SQLPOINTER), str_len);
}

TEST(AddressToPointer, SetPointerToNull) {
  SQLSMALLINT* out_buf = nullptr;
  SQLINTEGER str_len = 0;

  SQLRETURN return_code = AddressToPointer(nullptr, &out_buf, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(nullptr, out_buf);
  EXPECT_EQ(sizeof(SQLPOINTER), str_len);
}

TEST(AddressToPointer, SetPointerToNullWhenWasNotNull) {
  SQLSMALLINT value = 5;
  SQLSMALLINT* out_buf = &value;
  SQLINTEGER str_len = 0;

  SQLRETURN return_code = AddressToPointer(nullptr, &out_buf, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(nullptr, out_buf);
  EXPECT_EQ(sizeof(SQLPOINTER), str_len);
}

TEST(AddressToPointer, DoNotSetPointerToNull) {
  SQLSMALLINT ptr[] = {1, 2, 3};
  SQLINTEGER str_len = 0;

  SQLRETURN return_code = AddressToPointer(ptr, nullptr, &str_len);

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(sizeof(SQLPOINTER), str_len);
}

TEST(AddressToPointer, SetPointerNullStrLen) {
  SQLSMALLINT ptr[] = {1, 2, 3};
  SQLSMALLINT* out_buf = nullptr;

  SQLRETURN return_code =
      AddressToPointer(ptr, &out_buf, static_cast<SQLSMALLINT*>(nullptr));

  ASSERT_EQ(SQL_SUCCESS, return_code);
  EXPECT_EQ(ptr, out_buf);
  EXPECT_EQ(1, out_buf[0]);
  EXPECT_EQ(2, out_buf[1]);
  EXPECT_EQ(3, out_buf[2]);
}

TEST(WStrToOutputBufferResponse, SuccessWhenDestBufferLenGreaterThanSrcLen) {
  std::wstring expected = L"sample-test";
  SQLSMALLINT buffer_len = 15;
  SQLWCHAR dest[15];
  SQLLEN res_len = 0;

  StatusRecord status_record =
      WStrToOutputBufferResponse(expected, dest, buffer_len, 0, &res_len);

  ASSERT_TRUE(status_record.ok());
  std::wstring actual = SQLWcharToWstring(dest);
  EXPECT_EQ(L"sample-test", actual);
  EXPECT_EQ(res_len, expected.size() * sizeof(SQLWCHAR));
}

TEST(WStrToOutputBufferResponse,
     SuccessWithInfoWhenDestBufferLenLessThanSrcLen) {
  std::wstring expected = L"sample-test";
  SQLSMALLINT buffer_len = 5;
  SQLWCHAR dest[5];
  SQLLEN res_len = 0;

  StatusRecord status_record =
      WStrToOutputBufferResponse(expected, dest, buffer_len, 0, &res_len);

  ASSERT_FALSE(status_record.ok());
  EXPECT_EQ(SQLStates::k_01004(), status_record.sql_state);
  EXPECT_EQ("Data truncated", status_record.message);
  std::wstring actual = SQLWcharToWstring(dest);
  EXPECT_EQ(L"samp", actual);
  EXPECT_EQ(res_len, (buffer_len * sizeof(SQLWCHAR)));
}

TEST(WStrToOutputBufferResponse, SuccessWithInfoWhenDestBufferLenEqualsSrcLen) {
  std::wstring expected = L"sampl";
  SQLSMALLINT buffer_len = 5;
  SQLWCHAR dest[5];
  SQLLEN res_len = 0;

  StatusRecord status_record =
      WStrToOutputBufferResponse(expected, dest, buffer_len, 0, &res_len);

  ASSERT_FALSE(status_record.ok());
  EXPECT_EQ(SQLStates::k_01004(), status_record.sql_state);
  EXPECT_EQ("Data truncated", status_record.message);
  std::wstring actual = SQLWcharToWstring(dest);
  EXPECT_EQ(L"samp", actual);
  EXPECT_EQ(res_len, (buffer_len * sizeof(SQLWCHAR)));
}

TEST(WStrToOutputBufferResponse, SuccessWhenStcLenLenIsZero) {
  std::wstring expected;
  SQLSMALLINT buffer_len = 15;
  SQLWCHAR dest[15];
  SQLLEN res_len = 0;

  StatusRecord status_record =
      WStrToOutputBufferResponse(expected, dest, buffer_len, 0, &res_len);

  ASSERT_TRUE(status_record.ok());
  std::wstring actual = SQLWcharToWstring(dest);
  EXPECT_EQ(L"", actual);
  EXPECT_EQ(0, res_len);
}

TEST(WStrToOutputBufferResponse, FailureWhenBufferLenIsNegative) {
  std::wstring expected = L"sample-test";
  SQLSMALLINT buffer_len = -15;
  SQLWCHAR dest[15];
  SQLLEN res_len = 0;

  StatusRecord status_record =
      WStrToOutputBufferResponse(expected, dest, buffer_len, 0, &res_len);

  ASSERT_FALSE(status_record.ok());
  EXPECT_EQ(SQLStates::k_22003(), status_record.sql_state);
  EXPECT_EQ("Buffer length is insufficient", status_record.message);
}

namespace {

// L"A" U+00E9 U+6771 U+1F600: 1, 2, 3 and 4 bytes in UTF-8, and 1, 1, 1 and 2
// code units in UTF-16.
std::wstring MixedWidthText() {
#if defined(_WIN32)
  return std::wstring(L"Aé東") + L"\xD83D\xDE00";
#else
  return std::wstring(L"Aé東") + static_cast<wchar_t>(0x1F600);
#endif  // defined(_WIN32)
}

std::string EncodingParamName(
    ::testing::TestParamInfo<WireEncoding> const& info) {
  std::string name = WireEncodingName(info.param);
  name.erase(std::remove(name.begin(), name.end(), '-'), name.end());
  return name;
}

class WireCopyTest : public ::testing::TestWithParam<WireEncoding> {
 protected:
  WireCopyTest() : encoding_(GetParam()) {}
  ScopedWireEncoding encoding_;
};

TEST_P(WireCopyTest, EncodedSizeIsWholeCodeUnits) {
  std::string const encoded = EncodeWideToWire(L"hello");
  EXPECT_EQ(encoded.size(), 5 * WireWcharSize());
  EXPECT_EQ(EncodeWideToWire(L"").size(), 0);
}

TEST_P(WireCopyTest, FitsWithNul) {
  std::size_t const wire_sz = WireWcharSize();
  CanaryBuffer dest(6 * wire_sz);
  auto result = CopyWideToWireBuffer(L"hello", dest.data(), 6);
  EXPECT_TRUE(dest.CanariesIntact());
  EXPECT_FALSE(result.truncated);
  EXPECT_EQ(result.total_units, 5);
  EXPECT_EQ(result.copied_units, 5);
  EXPECT_TRUE(dest.IsNulAt(5, wire_sz));
  EXPECT_EQ(DecodeWire(dest.data(), 5), "hello");
}

TEST_P(WireCopyTest, TruncatesInsideBufferForEverySize) {
  std::size_t const wire_sz = WireWcharSize();
  for (std::size_t units = 0; units <= 6; ++units) {
    SCOPED_TRACE("dest_units=" + std::to_string(units));
    CanaryBuffer dest(units * wire_sz);
    auto result = CopyWideToWireBuffer(L"hello", dest.data(), units);
    EXPECT_TRUE(dest.CanariesIntact());
    EXPECT_EQ(result.total_units, 5);
    EXPECT_EQ(result.truncated, units <= 5);
    if (units > 0) {
      EXPECT_EQ(result.copied_units, std::min<std::size_t>(units - 1, 5));
      EXPECT_TRUE(dest.IsNulAt(result.copied_units, wire_sz));
    } else {
      EXPECT_EQ(result.copied_units, 0);
    }
  }
}

TEST_P(WireCopyTest, NullDestinationOnlyReportsLength) {
  auto result = CopyWideToWireBuffer(L"hello", nullptr, 100);
  EXPECT_EQ(result.total_units, 5);
  EXPECT_EQ(result.copied_units, 0);
  EXPECT_FALSE(result.truncated);
}

TEST_P(WireCopyTest, NeverSplitsAMultiUnitCharacter) {
  std::size_t const wire_sz = WireWcharSize();
  std::wstring const text = MixedWidthText();
  std::string const encoded = EncodeWideToWire(text);
  std::size_t const total = encoded.size() / wire_sz;
  for (std::size_t units = 1; units <= total + 1; ++units) {
    SCOPED_TRACE("dest_units=" + std::to_string(units));
    CanaryBuffer dest(units * wire_sz);
    auto result = CopyWireUnitsToBuffer(encoded, dest.data(), units);
    EXPECT_TRUE(dest.CanariesIntact());
    EXPECT_LE(result.copied_units, units - 1);
    EXPECT_TRUE(dest.IsNulAt(result.copied_units, wire_sz));
    // What was copied is a prefix of the text that decodes cleanly.
    if (result.copied_units > 0) {
      std::string const decoded = DecodeWire(dest.data(), result.copied_units);
      std::string const full = DecodeWire(encoded.data(), total);
      EXPECT_EQ(full.compare(0, decoded.size(), decoded), 0);
      // The prefix ends on a character boundary of the UTF-8 text.
      EXPECT_TRUE(decoded.size() == 1 || decoded.size() == 3 ||
                  decoded.size() == 6 || decoded.size() == 10)
          << "decoded size " << decoded.size();
    }
  }
}

TEST_P(WireCopyTest, MixedWidthLengthInCodeUnits) {
  std::size_t expected_units = 4;
  switch (GetParam()) {
    case WireEncoding::kUtf8:
      expected_units = 1 + 2 + 3 + 4;
      break;
    case WireEncoding::kUtf16Le:
      expected_units = 5;
      break;
    default:
      break;
  }
#if defined(_WIN32)
  expected_units = 5;
#endif  // defined(_WIN32)
  EXPECT_EQ(EncodeWideToWire(MixedWidthText()).size() / WireWcharSize(),
            expected_units);
}

TEST_P(WireCopyTest, Utf8RoundTrip) {
  std::string const utf8 = "SELECT N'\xE6\x9D\xB1\xE4\xBA\xAC'";
  std::size_t const wire_sz = WireWcharSize();
  CanaryBuffer dest(64 * wire_sz);
  auto result = CopyUtf8ToWireBuffer(utf8, dest.data(), 64);
  ASSERT_TRUE(result.Ok());
  EXPECT_TRUE(dest.CanariesIntact());
  EXPECT_FALSE(result->truncated);
  EXPECT_EQ(DecodeWire(dest.data(), result->total_units), utf8);
}

TEST_P(WireCopyTest, WStrToOutputBufferResponseStaysInBounds) {
  std::size_t const wire_sz = WireWcharSize();
  std::wstring const text = L"sample-test";
  for (SQLLEN units : {0, 1, 5, 11, 12}) {
    SCOPED_TRACE("buffer_length=" + std::to_string(units));
    CanaryBuffer dest(units * wire_sz);
    SQLLEN res_len = -1;
    WStrToOutputBufferResponse(text, dest.data(), units, 0, &res_len);
    EXPECT_TRUE(dest.CanariesIntact());
  }
}

TEST_P(WireCopyTest, WStrIntervalBufferResponseStaysInBounds) {
  std::size_t const wire_sz = WireWcharSize();
  std::wstring const text = L"12 10:20:30";
  for (SQLLEN units : {0, 1, 3, 11, 12}) {
    SCOPED_TRACE("buffer_length=" + std::to_string(units));
    CanaryBuffer dest(units * wire_sz);
    SQLLEN res_len = -1;
    WStrIntervalBufferResponse(text, dest.data(), units, 2, &res_len);
    EXPECT_TRUE(dest.CanariesIntact());
  }
}

INSTANTIATE_TEST_SUITE_P(WireEncodings, WireCopyTest,
                         ::testing::Values(WireEncoding::kUtf8,
                                           WireEncoding::kUtf16Le,
                                           WireEncoding::kUtf32Le),
                         EncodingParamName);

TEST(WireUnitsForBytes, WholeUnitsOnly) {
  EXPECT_EQ(WireUnitsForBytes(-4), 0);
  EXPECT_EQ(WireUnitsForBytes(0), 0);
  EXPECT_EQ(WireUnitsForBytes(static_cast<SQLLEN>(3 * WireWcharSize() + 1)), 3);
}

TEST(SaturateLength, ClampsToTypeMaximum) {
  EXPECT_EQ(SaturateLength<SQLSMALLINT>(12), 12);
  EXPECT_EQ(SaturateLength<SQLSMALLINT>(100000), 32767);
}

#if !defined(_WIN32)
TEST(BqConvertSQLWCHARToString, Utf16SurrogatePairs) {
  ScopedWireEncoding encoding(WireEncoding::kUtf16Le);
  std::uint16_t const input[] = {'a', 0xD83D, 0xDE00, 'b', 0};
  auto utf8 = BqConvertSQLWCHARToString(
      reinterpret_cast<SQLWCHAR const*>(input), SQL_NTS);
  ASSERT_TRUE(utf8.Ok());
  EXPECT_EQ(*utf8,
            "a\xF0\x9F\x98\x80"
            "b");
}

TEST(BqConvertSQLWCHARToString, Utf16LoneSurrogateIsReplaced) {
  ScopedWireEncoding encoding(WireEncoding::kUtf16Le);
  std::uint16_t const input[] = {'a', 0xDE00, 'b', 0};
  auto utf8 = BqConvertSQLWCHARToString(
      reinterpret_cast<SQLWCHAR const*>(input), SQL_NTS);
  ASSERT_TRUE(utf8.Ok());
  EXPECT_EQ(*utf8,
            "a\xEF\xBF\xBD"
            "b");
}

TEST(BqConvertSQLWCHARToString, NtsScanUsesConfiguredUnitSize) {
  // 2-byte units: the first 2-byte NUL ends the string, even though the
  // buffer has no 4-byte NUL before the guard values.
  ScopedWireEncoding encoding(WireEncoding::kUtf16Le);
  std::uint16_t const input[] = {'h', 'i', 0, 'X', 'X', 'X'};
  auto utf8 = BqConvertSQLWCHARToString(
      reinterpret_cast<SQLWCHAR const*>(input), SQL_NTS);
  ASSERT_TRUE(utf8.Ok());
  EXPECT_EQ(*utf8, "hi");

  ScopedWireEncoding utf8_encoding(WireEncoding::kUtf8);
  char const utf8_input[] = {'o', 'k', 0, 'X'};
  auto from_utf8 = BqConvertSQLWCHARToString(
      reinterpret_cast<SQLWCHAR const*>(utf8_input), SQL_NTS);
  ASSERT_TRUE(from_utf8.Ok());
  EXPECT_EQ(*from_utf8, "ok");
}
#endif  // !defined(_WIN32)

}  // namespace

}  // namespace google::cloud::odbc_bq_driver_internal
