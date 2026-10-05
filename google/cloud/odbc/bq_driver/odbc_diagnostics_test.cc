// Copyright 2023 Google LLC
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

#include "google/cloud/odbc/bq_driver/odbc_diagnostics.h"
#include "google/cloud/odbc/bq_driver/internal/odbc_conn_handle.h"
#include "google/cloud/odbc/bq_driver/internal/odbc_env_handle.h"
#include "google/cloud/odbc/bq_driver/internal/odbc_stmt_handle.h"
#include "google/cloud/odbc/bq_driver/internal/utils.h"
#include "google/cloud/odbc/testing/bq_driver_utils/utils.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <string>

namespace google::cloud::odbc_bq_driver {

using google::cloud::odbc_bq_driver_internal::ConnectionHandle;
using google::cloud::odbc_bq_driver_internal::EnvironmentHandle;
using google::cloud::odbc_bq_driver_internal::StatementHandle;
using google::cloud::odbc_bq_driver_internal::WireEncoding;
using google::cloud::odbc_bq_driver_internal::WireEncodingName;
using google::cloud::odbc_bq_driver_internal::WireWcharSize;
using google::cloud::odbc_internal::SQLStates;
using google::cloud::odbc_internal::StatusRecord;
using google::cloud::odbc_testing_bq_driver_utils::CanaryBuffer;
using google::cloud::odbc_testing_bq_driver_utils::DecodeWire;
using google::cloud::odbc_testing_bq_driver_utils::ScopedWireEncoding;

static StatusRecord const kRecord = {
    SQLStates::k_HY000(), "message", 11, 22, 33, "connection", "server"};

TEST(SQLGetDiagFieldInternal, InvalidHandleNull) {
  SQLSMALLINT diag_identifier = SQL_DIAG_DYNAMIC_FUNCTION;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_DBC, nullptr, 0, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_INVALID_HANDLE, status);
}

TEST(SQLGetDiagFieldInternal, InvalidHandleEnvironmenthandle) {
  EnvironmentHandle handle;
  SQLSMALLINT diag_identifier = SQL_DIAG_DYNAMIC_FUNCTION;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_DBC, &handle, 0, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_INVALID_HANDLE, status);
}

TEST(SQLGetDiagFieldInternal, InvalidHandleConnectionhandle) {
  ConnectionHandle handle;
  SQLSMALLINT diag_identifier = SQL_DIAG_DYNAMIC_FUNCTION;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_STMT, &handle, 0, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_INVALID_HANDLE, status);
}

TEST(SQLGetDiagFieldInternal, InvalidHandleStatementhandle) {
  StatementHandle handle;
  SQLSMALLINT diag_identifier = SQL_DIAG_DYNAMIC_FUNCTION;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 0, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_INVALID_HANDLE, status);
}

TEST(SQLGetDiagFieldInternal, SQLDiagDynamicFunctionSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().GetHeaderRecord().function = "test-function";
  SQLSMALLINT diag_identifier = SQL_DIAG_DYNAMIC_FUNCTION;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 0, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ("test-function", actual);
  EXPECT_EQ(13, diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagDynamicFunctionCodeSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().GetHeaderRecord().function_code = 11;
  SQLSMALLINT diag_identifier = SQL_DIAG_DYNAMIC_FUNCTION_CODE;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 0, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(11, diag_info);
  EXPECT_EQ(sizeof(SQLINTEGER), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagCursorRowCountSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().GetHeaderRecord().cursor_row_count = 22;
  SQLSMALLINT diag_identifier = SQL_DIAG_CURSOR_ROW_COUNT;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 0, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(22, diag_info);
  EXPECT_EQ(sizeof(SQLLEN), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagRowCountSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().GetHeaderRecord().row_count = 33;
  SQLSMALLINT diag_identifier = SQL_DIAG_ROW_COUNT;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 0, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(33, diag_info);
  EXPECT_EQ(sizeof(SQLLEN), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagNumberSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord({});
  SQLSMALLINT diag_identifier = SQL_DIAG_NUMBER;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 0, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(1, diag_info);
  EXPECT_EQ(sizeof(SQLINTEGER), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, FailNegativerecnumber) {
  EnvironmentHandle handle;
  SQLSMALLINT diag_identifier = SQL_DIAG_SQLSTATE;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;
  SQLSMALLINT rec_number = -5;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, rec_number, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_ERROR, status);
}

TEST(SQLGetDiagFieldInternal, FailZerorecnumber) {
  EnvironmentHandle handle;
  SQLSMALLINT diag_identifier = SQL_DIAG_SQLSTATE;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;
  SQLSMALLINT rec_number = 0;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, rec_number, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_ERROR, status);
}

TEST(SQLGetDiagFieldInternal, FailRecnumberGtSize) {
  EnvironmentHandle handle;
  SQLSMALLINT diag_identifier = SQL_DIAG_SQLSTATE;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;
  SQLSMALLINT rec_number = 100;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, rec_number, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_NO_DATA, status);
}

TEST(SQLGetDiagFieldInternal, SQLDiagSqlstateSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLSMALLINT diag_identifier = SQL_DIAG_SQLSTATE;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ(kRecord.sql_state, actual);
  EXPECT_EQ(kRecord.sql_state.size(), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagMessageTextSuccess) {
  EnvironmentHandle handle;
  std::string expected = "[Google][ODBC BigQuery Driver] " + kRecord.message;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLSMALLINT diag_identifier = SQL_DIAG_MESSAGE_TEXT;
  SQLCHAR diag_info[40];
  SQLSMALLINT diag_info_buffer_len = 40;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ(expected, actual);
  EXPECT_EQ(expected.size(), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagNativeSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLSMALLINT diag_identifier = SQL_DIAG_NATIVE;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(kRecord.native_error_code, diag_info);
  EXPECT_EQ(sizeof(SQLINTEGER), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagColumnNumberSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLSMALLINT diag_identifier = SQL_DIAG_COLUMN_NUMBER;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(kRecord.column_number, diag_info);
  EXPECT_EQ(sizeof(SQLINTEGER), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagRowNumberSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLSMALLINT diag_identifier = SQL_DIAG_ROW_NUMBER;
  SQLULEN diag_info = 0;
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, &diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(kRecord.row_number, diag_info);
  EXPECT_EQ(sizeof(SQLLEN), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagConnectionNameSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLSMALLINT diag_identifier = SQL_DIAG_CONNECTION_NAME;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ(kRecord.connection_name, actual);
  EXPECT_EQ(kRecord.connection_name.size(), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagServerNameSuccess) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLSMALLINT diag_identifier = SQL_DIAG_SERVER_NAME;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ(kRecord.server_name, actual);
  EXPECT_EQ(kRecord.server_name.size(), diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagClassOriginSuccessOdbc3) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord({SQLStates::k_IM001(), "message"});
  SQLSMALLINT diag_identifier = SQL_DIAG_CLASS_ORIGIN;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ("ODBC 3.0", actual);
  EXPECT_EQ(8, diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagClassOriginSuccessIso) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord({SQLStates::k_HY000(), "message"});
  SQLSMALLINT diag_identifier = SQL_DIAG_CLASS_ORIGIN;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ("ISO 9075", actual);
  EXPECT_EQ(8, diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagSubclassOriginSuccessOdbc3) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord({SQLStates::k_IM001(), "message"});
  SQLSMALLINT diag_identifier = SQL_DIAG_SUBCLASS_ORIGIN;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ("ODBC 3.0", actual);
  EXPECT_EQ(8, diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, SQLDiagSubclassOriginSuccessIso) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord({SQLStates::k_HY000(), "message"});
  SQLSMALLINT diag_identifier = SQL_DIAG_SUBCLASS_ORIGIN;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  std::string actual = reinterpret_cast<char*>(diag_info);
  EXPECT_EQ("ISO 9075", actual);
  EXPECT_EQ(8, diag_info_string_len);
}

TEST(SQLGetDiagFieldInternal, FailDiagidentifierInvalid) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord({SQLStates::k_HY000(), "message"});
  SQLSMALLINT diag_identifier = 111;
  SQLCHAR diag_info[15];
  SQLSMALLINT diag_info_buffer_len = 15;
  SQLSMALLINT diag_info_string_len;

  SQLRETURN status = SQLGetDiagFieldInternal(
      SQL_HANDLE_ENV, &handle, 1, diag_identifier, diag_info,
      diag_info_buffer_len, &diag_info_string_len);

  ASSERT_EQ(SQL_ERROR, status);
}

TEST(SQLGetDiagRecInternal, Success) {
  EnvironmentHandle handle;
  std::string expected = "[Google][ODBC BigQuery Driver] " + kRecord.message;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLCHAR sql_state[6];
  SQLINTEGER native_error = 0;
  SQLCHAR message_text[45];
  SQLSMALLINT message_text_buffer_len = 45;
  SQLSMALLINT message_text_len;

  SQLRETURN status = SQLGetDiagRecInternal(
      SQL_HANDLE_ENV, &handle, 1, sql_state, &native_error, message_text,
      message_text_buffer_len, &message_text_len);

  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(kRecord.native_error_code, native_error);
  std::string actual_sqlstate = reinterpret_cast<char*>(sql_state);
  EXPECT_EQ(kRecord.sql_state, actual_sqlstate);
  std::string actual_message = reinterpret_cast<char*>(message_text);
  EXPECT_EQ(expected, actual_message);
  EXPECT_EQ(expected.size(), message_text_len);
}

TEST(SQLGetDiagRecInternal, FailNegativerecnumber) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLCHAR sql_state[5];
  SQLINTEGER native_error = 0;
  SQLCHAR message_text[15];
  SQLSMALLINT message_text_buffer_len = 15;
  SQLSMALLINT message_text_len;
  SQLSMALLINT rec_number = -5;

  SQLRETURN status = SQLGetDiagRecInternal(
      SQL_HANDLE_ENV, &handle, rec_number, sql_state, &native_error,
      message_text, message_text_buffer_len, &message_text_len);

  ASSERT_EQ(SQL_ERROR, status);
}

TEST(SQLGetDiagRecInternal, FailZerorecnumber) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLCHAR sql_state[5];
  SQLINTEGER native_error = 0;
  SQLCHAR message_text[15];
  SQLSMALLINT message_text_buffer_len = 15;
  SQLSMALLINT message_text_len;
  SQLSMALLINT rec_number = 0;

  SQLRETURN status = SQLGetDiagRecInternal(
      SQL_HANDLE_ENV, &handle, rec_number, sql_state, &native_error,
      message_text, message_text_buffer_len, &message_text_len);

  ASSERT_EQ(SQL_ERROR, status);
}

TEST(SQLGetDiagRecInternal, FailRecnumberGtSize) {
  EnvironmentHandle handle;
  handle.GetDiagnostics().AddStatusRecord(kRecord);
  SQLCHAR sql_state[5];
  SQLINTEGER native_error = 0;
  SQLCHAR message_text[15];
  SQLSMALLINT message_text_buffer_len = 15;
  SQLSMALLINT message_text_len;
  SQLSMALLINT rec_number = 100;

  SQLRETURN status = SQLGetDiagRecInternal(
      SQL_HANDLE_ENV, &handle, rec_number, sql_state, &native_error,
      message_text, message_text_buffer_len, &message_text_len);

  ASSERT_EQ(SQL_NO_DATA, status);
}

namespace {

std::string const kDiagPrefix = "[Google][ODBC BigQuery Driver] ";
StatusRecord const kTableNotFound = {
    "42S02", "Not found: Table project:dataset.missing", 7, 0, 0, "", ""};
std::string const kTableNotFoundText = kDiagPrefix + kTableNotFound.message;

std::string EncodingParamName(
    ::testing::TestParamInfo<WireEncoding> const& info) {
  std::string name = WireEncodingName(info.param);
  name.erase(std::remove(name.begin(), name.end(), '-'), name.end());
  return name;
}

auto const kAllWireEncodings = ::testing::Values(
    WireEncoding::kUtf8, WireEncoding::kUtf16Le, WireEncoding::kUtf32Le);

class SQLGetDiagRecWInternalTest
    : public ::testing::TestWithParam<WireEncoding> {
 protected:
  SQLGetDiagRecWInternalTest() : encoding_(GetParam()) {
    handle_.GetDiagnostics().AddStatusRecord(kTableNotFound);
  }

  ScopedWireEncoding encoding_;
  EnvironmentHandle handle_;
};

TEST_P(SQLGetDiagRecWInternalTest, FullBuffers) {
  std::size_t const wire_sz = WireWcharSize();
  CanaryBuffer sql_state(6 * wire_sz);
  CanaryBuffer message(256 * wire_sz);
  SQLINTEGER native_error = 0;
  SQLSMALLINT message_len = -1;

  SQLRETURN rc = SQLGetDiagRecWInternal(
      SQL_HANDLE_ENV, &handle_, 1, static_cast<SQLWCHAR*>(sql_state.data()),
      &native_error, static_cast<SQLWCHAR*>(message.data()), 256, &message_len);

  ASSERT_EQ(rc, SQL_SUCCESS);
  EXPECT_TRUE(sql_state.CanariesIntact());
  EXPECT_TRUE(message.CanariesIntact());
  EXPECT_EQ(DecodeWire(sql_state.data(), 5), "42S02");
  EXPECT_TRUE(sql_state.IsNulAt(5, wire_sz));
  EXPECT_EQ(DecodeWire(message.data(), kTableNotFoundText.size()),
            kTableNotFoundText);
  EXPECT_TRUE(message.IsNulAt(kTableNotFoundText.size(), wire_sz));
  EXPECT_EQ(message_len, kTableNotFoundText.size());
  EXPECT_EQ(native_error, 7);
}

TEST_P(SQLGetDiagRecWInternalTest, SmallMessageBuffersAreTruncatedInBounds) {
  std::size_t const wire_sz = WireWcharSize();
  for (SQLSMALLINT buffer_len : {1, 6, 10}) {
    SCOPED_TRACE("BufferLength=" + std::to_string(buffer_len));
    CanaryBuffer sql_state(6 * wire_sz);
    CanaryBuffer message(buffer_len * wire_sz);
    SQLSMALLINT message_len = -1;

    SQLRETURN rc = SQLGetDiagRecWInternal(
        SQL_HANDLE_ENV, &handle_, 1, static_cast<SQLWCHAR*>(sql_state.data()),
        nullptr, static_cast<SQLWCHAR*>(message.data()), buffer_len,
        &message_len);

    EXPECT_EQ(rc, SQL_SUCCESS_WITH_INFO);
    EXPECT_TRUE(sql_state.CanariesIntact());
    EXPECT_TRUE(message.CanariesIntact());
    EXPECT_EQ(DecodeWire(sql_state.data(), 5), "42S02");
    auto const kept = static_cast<std::size_t>(buffer_len - 1);
    EXPECT_EQ(DecodeWire(message.data(), kept),
              kTableNotFoundText.substr(0, kept));
    EXPECT_TRUE(message.IsNulAt(kept, wire_sz));
    // The full length in characters, not the truncated length.
    EXPECT_EQ(message_len, kTableNotFoundText.size());
  }
}

TEST_P(SQLGetDiagRecWInternalTest, ZeroLengthBufferWritesNothing) {
  CanaryBuffer message(0);
  SQLSMALLINT message_len = -1;

  SQLRETURN rc = SQLGetDiagRecWInternal(
      SQL_HANDLE_ENV, &handle_, 1, nullptr, nullptr,
      static_cast<SQLWCHAR*>(message.data()), 0, &message_len);

  EXPECT_EQ(rc, SQL_SUCCESS_WITH_INFO);
  EXPECT_TRUE(message.CanariesIntact());
  EXPECT_EQ(message_len, kTableNotFoundText.size());
}

TEST_P(SQLGetDiagRecWInternalTest, NullBuffersReportLength) {
  SQLSMALLINT message_len = -1;
  SQLRETURN rc = SQLGetDiagRecWInternal(SQL_HANDLE_ENV, &handle_, 1, nullptr,
                                        nullptr, nullptr, 0, &message_len);
  EXPECT_EQ(rc, SQL_SUCCESS);
  EXPECT_EQ(message_len, kTableNotFoundText.size());
}

TEST_P(SQLGetDiagRecWInternalTest, NegativeBufferLengthIsAnError) {
  CanaryBuffer message(16);
  SQLRETURN rc = SQLGetDiagRecWInternal(
      SQL_HANDLE_ENV, &handle_, 1, nullptr, nullptr,
      static_cast<SQLWCHAR*>(message.data()), -1, nullptr);
  EXPECT_EQ(rc, SQL_ERROR);
  EXPECT_TRUE(message.CanariesIntact());
}

TEST_P(SQLGetDiagRecWInternalTest, NoRecordReturnsNoDataAndWritesNothing) {
  std::size_t const wire_sz = WireWcharSize();
  CanaryBuffer sql_state(6 * wire_sz);
  CanaryBuffer message(64 * wire_sz);
  SQLRETURN rc = SQLGetDiagRecWInternal(
      SQL_HANDLE_ENV, &handle_, 2, static_cast<SQLWCHAR*>(sql_state.data()),
      nullptr, static_cast<SQLWCHAR*>(message.data()), 64, nullptr);
  EXPECT_EQ(rc, SQL_NO_DATA);
  EXPECT_TRUE(sql_state.CanariesIntact());
  EXPECT_TRUE(message.CanariesIntact());
  EXPECT_EQ(message.bytes()[0], CanaryBuffer::kFill);
}

// unixODBC (<= 2.3.14) calls SQLGetDiagRecW from extract_diag_error_w() with
// `SQLWCHAR sqlstate[6]` and a heap buffer of
// (SQL_MAX_MESSAGE_LENGTH + 1) * sizeof(SQLWCHAR) bytes with BufferLength 513,
// where its SQLWCHAR is 2 bytes. Reproduce exactly those buffers.
constexpr std::size_t kUnixOdbcWcharSize = 2;
constexpr SQLSMALLINT kUnixOdbcMessageBufferLen = SQL_MAX_MESSAGE_LENGTH + 1;
constexpr std::size_t kUnixOdbcMessageBytes =
    (SQL_MAX_MESSAGE_LENGTH + 1) * kUnixOdbcWcharSize;

TEST_P(SQLGetDiagRecWInternalTest, UnixOdbcBufferSizes) {
  std::size_t const wire_sz = WireWcharSize();
  // A 5-character SQLSTATE plus NUL is 6 configured code units. In 2-byte
  // units that is unixODBC's 12-byte sqlstate[6]; in 4-byte units it needs
  // 24 bytes, which is why WcharEncoding must match the Driver Manager.
  CanaryBuffer sql_state(
      std::max<std::size_t>(6 * kUnixOdbcWcharSize, 6 * wire_sz));
  CanaryBuffer message(kUnixOdbcMessageBytes);
  SQLINTEGER native_error = 0;
  SQLSMALLINT message_len = -1;

  SQLRETURN rc = SQLGetDiagRecWInternal(
      SQL_HANDLE_ENV, &handle_, 1, static_cast<SQLWCHAR*>(sql_state.data()),
      &native_error, static_cast<SQLWCHAR*>(message.data()),
      kUnixOdbcMessageBufferLen, &message_len);

  ASSERT_EQ(rc, SQL_SUCCESS);
  EXPECT_TRUE(sql_state.CanariesIntact());
  EXPECT_TRUE(message.CanariesIntact());
  EXPECT_EQ(DecodeWire(sql_state.data(), 5), "42S02");
  EXPECT_TRUE(sql_state.IsNulAt(5, wire_sz));
  EXPECT_EQ(DecodeWire(message.data(), kTableNotFoundText.size()),
            kTableNotFoundText);
  EXPECT_TRUE(message.IsNulAt(kTableNotFoundText.size(), wire_sz));
  EXPECT_EQ(message_len, kTableNotFoundText.size());
  // Nothing past the message's NUL is written: the old code cleared
  // BufferLength * WireWcharSize() bytes, past the end of this buffer.
  std::size_t const written = (kTableNotFoundText.size() + 1) * wire_sz;
  EXPECT_EQ(message.bytes()[written], CanaryBuffer::kFill);
}

TEST_P(SQLGetDiagRecWInternalTest, UnixOdbcBufferSizesLongMessage) {
  std::size_t const wire_sz = WireWcharSize();
  if (wire_sz > kUnixOdbcWcharSize) {
    GTEST_SKIP() << "A message of BufferLength wide code units does not fit "
                    "unixODBC's buffer when the configured code unit is "
                    "wider than unixODBC's SQLWCHAR";
  }
  EnvironmentHandle handle;
  StatusRecord long_record = kTableNotFound;
  long_record.message = std::string(700, 'x');
  handle.GetDiagnostics().AddStatusRecord(long_record);
  std::string const expected = kDiagPrefix + long_record.message;

  CanaryBuffer sql_state(6 * kUnixOdbcWcharSize);
  CanaryBuffer message(kUnixOdbcMessageBytes);
  SQLSMALLINT message_len = -1;

  SQLRETURN rc = SQLGetDiagRecWInternal(
      SQL_HANDLE_ENV, &handle, 1, static_cast<SQLWCHAR*>(sql_state.data()),
      nullptr, static_cast<SQLWCHAR*>(message.data()),
      kUnixOdbcMessageBufferLen, &message_len);

  EXPECT_EQ(rc, SQL_SUCCESS_WITH_INFO);
  EXPECT_TRUE(sql_state.CanariesIntact());
  EXPECT_TRUE(message.CanariesIntact());
  auto const kept = static_cast<std::size_t>(kUnixOdbcMessageBufferLen - 1);
  EXPECT_EQ(DecodeWire(message.data(), kept), expected.substr(0, kept));
  EXPECT_TRUE(message.IsNulAt(kept, wire_sz));
  EXPECT_EQ(message_len, expected.size());
}

TEST_P(SQLGetDiagRecWInternalTest, NonAsciiMessageLengthInCodeUnits) {
  EnvironmentHandle handle;
  StatusRecord record = kTableNotFound;
  record.message = "\xE6\x9D\xB1\xE4\xBA\xAC \xF0\x9F\x98\x80";  // 東京 😀
  handle.GetDiagnostics().AddStatusRecord(record);
  std::string const expected = kDiagPrefix + record.message;
  std::size_t expected_units = 0;
  switch (GetParam()) {
    case WireEncoding::kUtf8:
      expected_units = expected.size();
      break;
    case WireEncoding::kUtf16Le:
      expected_units = kDiagPrefix.size() + 5;  // 東 京 space + 2 surrogates
      break;
    default:
      expected_units = kDiagPrefix.size() + 4;
      break;
  }
#if defined(_WIN32)
  expected_units = kDiagPrefix.size() + 5;
#endif  // defined(_WIN32)

  std::size_t const wire_sz = WireWcharSize();
  CanaryBuffer message(64 * wire_sz);
  SQLSMALLINT message_len = -1;
  SQLRETURN rc = SQLGetDiagRecWInternal(
      SQL_HANDLE_ENV, &handle, 1, nullptr, nullptr,
      static_cast<SQLWCHAR*>(message.data()), 64, &message_len);

  ASSERT_EQ(rc, SQL_SUCCESS);
  EXPECT_TRUE(message.CanariesIntact());
  EXPECT_EQ(message_len, expected_units);
  EXPECT_EQ(DecodeWire(message.data(), expected_units), expected);
  EXPECT_TRUE(message.IsNulAt(expected_units, wire_sz));
}

INSTANTIATE_TEST_SUITE_P(WireEncodings, SQLGetDiagRecWInternalTest,
                         kAllWireEncodings, EncodingParamName);

class SQLGetDiagFieldWInternalTest
    : public ::testing::TestWithParam<WireEncoding> {
 protected:
  SQLGetDiagFieldWInternalTest() : encoding_(GetParam()) {
    handle_.GetDiagnostics().AddStatusRecord(kTableNotFound);
  }

  ScopedWireEncoding encoding_;
  EnvironmentHandle handle_;
};

TEST_P(SQLGetDiagFieldWInternalTest, MessageTextBufferLengthInBytes) {
  std::size_t const wire_sz = WireWcharSize();
  // An odd byte count: only whole code units may be used.
  auto const buffer_bytes = static_cast<SQLSMALLINT>(10 * wire_sz + 1);
  CanaryBuffer message(buffer_bytes);
  SQLSMALLINT string_len = -1;

  SQLRETURN rc = SQLGetDiagFieldWInternal(SQL_HANDLE_ENV, &handle_, 1,
                                          SQL_DIAG_MESSAGE_TEXT, message.data(),
                                          buffer_bytes, &string_len);

  // With 1-byte code units the extra byte is a whole code unit.
  std::size_t const kept = buffer_bytes / wire_sz - 1;
  EXPECT_EQ(rc, SQL_SUCCESS_WITH_INFO);
  EXPECT_TRUE(message.CanariesIntact());
  EXPECT_EQ(DecodeWire(message.data(), kept),
            kTableNotFoundText.substr(0, kept));
  EXPECT_TRUE(message.IsNulAt(kept, wire_sz));
  if (wire_sz > 1) {
    EXPECT_EQ(message.bytes()[buffer_bytes - 1], CanaryBuffer::kFill);
  }
  EXPECT_EQ(string_len, kTableNotFoundText.size() * wire_sz);
}

TEST_P(SQLGetDiagFieldWInternalTest, SqlState) {
  std::size_t const wire_sz = WireWcharSize();
  CanaryBuffer sql_state(6 * wire_sz);
  SQLSMALLINT string_len = -1;

  SQLRETURN rc = SQLGetDiagFieldWInternal(
      SQL_HANDLE_ENV, &handle_, 1, SQL_DIAG_SQLSTATE, sql_state.data(),
      static_cast<SQLSMALLINT>(6 * wire_sz), &string_len);

  EXPECT_EQ(rc, SQL_SUCCESS);
  EXPECT_TRUE(sql_state.CanariesIntact());
  EXPECT_EQ(DecodeWire(sql_state.data(), 5), "42S02");
  EXPECT_TRUE(sql_state.IsNulAt(5, wire_sz));
  EXPECT_EQ(string_len, 5 * wire_sz);
}

TEST_P(SQLGetDiagFieldWInternalTest, NumericFieldIsReturned) {
  SQLINTEGER native = 0;
  SQLRETURN rc =
      SQLGetDiagFieldWInternal(SQL_HANDLE_ENV, &handle_, 1, SQL_DIAG_NATIVE,
                               &native, sizeof(native), nullptr);
  EXPECT_EQ(rc, SQL_SUCCESS);
  EXPECT_EQ(native, 7);
}

INSTANTIATE_TEST_SUITE_P(WireEncodings, SQLGetDiagFieldWInternalTest,
                         kAllWireEncodings, EncodingParamName);

}  // namespace

}  // namespace google::cloud::odbc_bq_driver
