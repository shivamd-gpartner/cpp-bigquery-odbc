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

#include "google/cloud/odbc/bq_driver/odbc_sql_results.h"
#include "google/cloud/odbc/bq_driver/internal/odbc_type_utils.h"
#include "google/cloud/odbc/bq_driver/odbc_commons.h"
#include "google/cloud/odbc/bq_driver/odbc_descriptor.h"
#include "google/cloud/odbc/bq_driver/odbc_diagnostics.h"
#include "google/cloud/odbc/bq_driver/odbc_statement.h"
#include "google/cloud/odbc/testing/bq_driver_utils/handles.h"
#include "google/cloud/odbc/testing/bq_driver_utils/utils.h"
#include "google/cloud/odbc/testing/utils/status_matchers.h"
#include <gtest/gtest.h>

namespace google::cloud::odbc_bq_driver {

using google::cloud::odbc_bq_driver_internal::BQDataType;
using google::cloud::odbc_bq_driver_internal::ColumnSchema;
using google::cloud::odbc_bq_driver_internal::ConnectionHandle;
using google::cloud::odbc_bq_driver_internal::DescriptorHandle;
using google::cloud::odbc_bq_driver_internal::DescriptorRecord;
using google::cloud::odbc_bq_driver_internal::DescriptorType;
using google::cloud::odbc_bq_driver_internal::DSValue;
using google::cloud::odbc_bq_driver_internal::EncodeWideToWire;
using google::cloud::odbc_bq_driver_internal::StatementHandle;
using google::cloud::odbc_bq_driver_internal::StmtStates;
using google::cloud::odbc_bq_driver_internal::Utf8ToUtf16;
using google::cloud::odbc_bq_driver_internal::WireEncoding;
using google::cloud::odbc_bq_driver_internal::WireWcharSize;
using google::cloud::odbc_internal::SQLStates;
using google::cloud::odbc_testing_bq_driver_utils::CanaryBuffer;
using google::cloud::odbc_testing_bq_driver_utils::CreateConnectionHandle;
using google::cloud::odbc_testing_bq_driver_utils::
    CreateDescRecordWithRandomValues;
using google::cloud::odbc_testing_bq_driver_utils::CreateStatementHandle;
using google::cloud::odbc_testing_bq_driver_utils::CreateStmtHandleWithState;
using google::cloud::odbc_testing_bq_driver_utils::ScopedWireEncoding;
using ::testing::HasSubstr;

inline SQLUSMALLINT GetDescCount(SQLPOINTER ard) {
  SQLUSMALLINT out_desc_count;
  SQLRETURN status = SQLGetDescFieldInternal(ard, 0, SQL_DESC_COUNT,
                                             &out_desc_count, 0, nullptr);
  EXPECT_EQ(SQL_SUCCESS, status);
  return out_desc_count;
}

TEST(SQLBindColInternal, Basic) {
  StatementHandle handle = CreateStatementHandle();
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status =
      SQLBindColInternal(&handle, 1, SQL_C_FLOAT, buf, 20, &target_str_len);
  ASSERT_EQ(SQL_SUCCESS, status);

  SQLPOINTER ard = nullptr;
  status =
      SQLGetStmtAttrInternal(&handle, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr);
  ASSERT_EQ(SQL_SUCCESS, status);

  SQLPOINTER out_buf;
  SQLINTEGER str_len = 0;
  status =
      SQLGetDescFieldInternal(ard, 1, SQL_DESC_DATA_PTR, &out_buf, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(buf, out_buf);

  SQLSMALLINT out_c_type;
  status =
      SQLGetDescFieldInternal(ard, 1, SQL_DESC_TYPE, &out_c_type, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(SQL_C_FLOAT, out_c_type);

  SQLSMALLINT out_concise_c_type;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_CONCISE_TYPE,
                                   &out_concise_c_type, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(SQL_C_FLOAT, out_concise_c_type);

  SQLLEN out_octet_length;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_OCTET_LENGTH,
                                   &out_octet_length, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(20, out_octet_length);

  SQLPOINTER out_desc_ind_ptr;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_INDICATOR_PTR,
                                   &out_desc_ind_ptr, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(&target_str_len, out_desc_ind_ptr);

  SQLPOINTER out_octet_length_ptr;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_OCTET_LENGTH_PTR,
                                   &out_octet_length_ptr, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(&target_str_len, out_octet_length_ptr);
}

TEST(SQLBindColInternal, TypeSqlCTypeDate) {
  StatementHandle handle = CreateStatementHandle();
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status =
      SQLBindColInternal(&handle, 1, SQL_C_TYPE_DATE, buf, 20, &target_str_len);
  ASSERT_EQ(SQL_SUCCESS, status);

  SQLPOINTER ard = nullptr;
  status =
      SQLGetStmtAttrInternal(&handle, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr);
  ASSERT_EQ(SQL_SUCCESS, status);

  SQLPOINTER out_buf;
  SQLINTEGER str_len = 0;
  status =
      SQLGetDescFieldInternal(ard, 1, SQL_DESC_DATA_PTR, &out_buf, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(buf, out_buf);

  SQLSMALLINT out_c_type;
  status =
      SQLGetDescFieldInternal(ard, 1, SQL_DESC_TYPE, &out_c_type, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(SQL_DATETIME, out_c_type);

  SQLSMALLINT out_concise_c_type;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_CONCISE_TYPE,
                                   &out_concise_c_type, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(SQL_C_TYPE_DATE, out_concise_c_type);

  SQLSMALLINT out_datetime_interval_code;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_DATETIME_INTERVAL_CODE,
                                   &out_datetime_interval_code, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(SQL_CODE_DATE, out_datetime_interval_code);

  SQLLEN out_octet_length;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_OCTET_LENGTH,
                                   &out_octet_length, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(20, out_octet_length);

  SQLPOINTER out_desc_ind_ptr;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_INDICATOR_PTR,
                                   &out_desc_ind_ptr, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(&target_str_len, out_desc_ind_ptr);

  SQLPOINTER out_octet_length_ptr;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_OCTET_LENGTH_PTR,
                                   &out_octet_length_ptr, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(&target_str_len, out_octet_length_ptr);
}

TEST(SQLBindColInternal, TypeSqlCIntervalMonth) {
  StatementHandle handle = CreateStatementHandle();
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status = SQLBindColInternal(&handle, 1, SQL_C_INTERVAL_MONTH, buf,
                                        20, &target_str_len);
  ASSERT_EQ(SQL_SUCCESS, status);

  SQLPOINTER ard = nullptr;
  status =
      SQLGetStmtAttrInternal(&handle, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr);
  ASSERT_EQ(SQL_SUCCESS, status);

  SQLPOINTER out_buf;
  SQLINTEGER str_len = 0;
  status =
      SQLGetDescFieldInternal(ard, 1, SQL_DESC_DATA_PTR, &out_buf, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(buf, out_buf);

  SQLSMALLINT out_c_type;
  status =
      SQLGetDescFieldInternal(ard, 1, SQL_DESC_TYPE, &out_c_type, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(SQL_INTERVAL, out_c_type);

  SQLSMALLINT out_concise_c_type;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_CONCISE_TYPE,
                                   &out_concise_c_type, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(SQL_C_INTERVAL_MONTH, out_concise_c_type);

  SQLSMALLINT out_datetime_interval_code;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_DATETIME_INTERVAL_CODE,
                                   &out_datetime_interval_code, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(SQL_CODE_MONTH, out_datetime_interval_code);

  SQLLEN out_octet_length;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_OCTET_LENGTH,
                                   &out_octet_length, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(20, out_octet_length);

  SQLPOINTER out_desc_ind_ptr;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_INDICATOR_PTR,
                                   &out_desc_ind_ptr, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(&target_str_len, out_desc_ind_ptr);

  SQLPOINTER out_octet_length_ptr;
  status = SQLGetDescFieldInternal(ard, 1, SQL_DESC_OCTET_LENGTH_PTR,
                                   &out_octet_length_ptr, 0, &str_len);
  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(&target_str_len, out_octet_length_ptr);
}

TEST(SQLBindColInternal, UnBindindingBasic) {
  StatementHandle handle = CreateStatementHandle();
  SQLCHAR buf[20];
  SQLLEN target_str_len;

  // Binding a column
  SQLRETURN status = SQLBindColInternal(&handle, 1, SQL_C_INTERVAL_MONTH, buf,
                                        20, &target_str_len);
  ASSERT_EQ(SQL_SUCCESS, status);

  SQLPOINTER ard = nullptr;
  status =
      SQLGetStmtAttrInternal(&handle, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr);
  ASSERT_EQ(SQL_SUCCESS, status);

  EXPECT_EQ(1, GetDescCount(ard));

  // Unbinding a column
  status = SQLBindColInternal(&handle, 1, SQL_C_INTERVAL_MONTH, nullptr, 20,
                              &target_str_len);
  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(0, GetDescCount(ard));
}

// This test binds multiple columns, unbinds the highest col number,
// and verifies if SQL_DESC_COUNT is correct
TEST(SQLBindColInternal, UnBindindingComplex) {
  StatementHandle handle = CreateStatementHandle();
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status;

  // Binding 2 columns
  status = SQLBindColInternal(&handle, 5, SQL_C_INTERVAL_MONTH, buf, 20,
                              &target_str_len);
  ASSERT_EQ(SQL_SUCCESS, status);
  status = SQLBindColInternal(&handle, 2, SQL_C_INTERVAL_MONTH, buf, 20,
                              &target_str_len);
  ASSERT_EQ(SQL_SUCCESS, status);

  SQLPOINTER ard = nullptr;
  status =
      SQLGetStmtAttrInternal(&handle, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr);
  ASSERT_EQ(SQL_SUCCESS, status);

  EXPECT_EQ(5, GetDescCount(ard));

  // Unbinding the column with highest index
  status = SQLBindColInternal(&handle, 5, SQL_C_INTERVAL_MONTH, nullptr, 20,
                              &target_str_len);
  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(2, GetDescCount(ard));
}

TEST(SQLBindColInternal, InvalidColNumber) {
  StatementHandle handle = CreateStatementHandle();
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status = SQLBindColInternal(&handle, 0, SQL_C_INTERVAL_MONTH,
                                        nullptr, 20, &target_str_len);
  ASSERT_EQ(SQL_ERROR, status);
}

TEST(SQLBindColInternal, InvalidBufLen) {
  StatementHandle handle = CreateStatementHandle();
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status = SQLBindColInternal(&handle, 0, SQL_C_INTERVAL_MONTH, buf,
                                        -1, &target_str_len);
  ASSERT_EQ(SQL_ERROR, status);
}

TEST(SQLBindColInternal, InvalidCType) {
  StatementHandle handle = CreateStatementHandle();
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status = SQLBindColInternal(&handle, 2, SQL_UNKNOWN_TYPE, buf, 20,
                                        &target_str_len);
  ASSERT_EQ(SQL_ERROR, status);

  SQLPOINTER ard = nullptr;
  status =
      SQLGetStmtAttrInternal(&handle, SQL_ATTR_APP_ROW_DESC, &ard, 0, nullptr);
  ASSERT_EQ(SQL_SUCCESS, status);

  // If SQLBindColInternal has failed, SQL_DESC_COUNT should remain unchanged
  EXPECT_EQ(0, GetDescCount(ard));
}

TEST(SQLNumResultColsInternal, InvalidHandle) {
  StatementHandle* stmt_handle = nullptr;
  SQLSMALLINT column_count;

  SQLRETURN ret = SQLNumResultColsInternal(stmt_handle, &column_count);

  EXPECT_EQ(ret, SQL_INVALID_HANDLE);
}

TEST(SQLNumResultColsInternal, NullColumnCountPtr) {
  StatementHandle handle = CreateStatementHandle();
  SQLRETURN ret = SQLNumResultColsInternal(&handle, nullptr);

  ASSERT_EQ(ret, SQL_ERROR);
  EXPECT_EQ(SQLStates::k_HY001(),
            handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLNumResultColsInternal, StateCheck) {
  StatementHandle handle = CreateStatementHandle();

  handle.SetStmtState(StmtStates::kStatementExecutedWithRs);

  SQLSMALLINT column_count;
  SQLRETURN ret = SQLNumResultColsInternal(&handle, &column_count);

  EXPECT_EQ(ret, SQL_SUCCESS);
  EXPECT_EQ(column_count, 0);
}
void AssertDescribeColumnResults(
    SQLRETURN status, DescriptorRecord const& record, SQLCHAR column_name[15],
    SQLSMALLINT column_name_Le, SQLSMALLINT data_type, SQLULEN column_size,
    SQLSMALLINT decimal_digits, SQLSMALLINT nullable) {
  ASSERT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(record.concise_type, data_type);
  switch (data_type) {
    case SQL_NUMERIC:
    case SQL_DECIMAL:
    case SQL_INTEGER:
    case SQL_SMALLINT:
    case SQL_TINYINT:
    case SQL_BIGINT:
      EXPECT_EQ(record.precision, column_size);
      break;
    default:
      EXPECT_EQ(record.length, column_size);
  }
  switch (data_type) {
    case SQL_TYPE_DATE:
    case SQL_TYPE_TIME:
    case SQL_TYPE_TIMESTAMP:
    case SQL_INTERVAL_SECOND:
    case SQL_INTERVAL_DAY_TO_SECOND:
    case SQL_INTERVAL_HOUR_TO_SECOND:
    case SQL_INTERVAL_MINUTE_TO_SECOND:
      EXPECT_EQ(record.precision, decimal_digits);
      break;
    case SQL_DECIMAL:
    case SQL_NUMERIC:
    case SQL_SMALLINT:
    case SQL_INTEGER:
    case SQL_BIGINT:
      EXPECT_EQ(record.scale, decimal_digits);
      break;
    default:
      EXPECT_EQ(0, decimal_digits);
  }

  EXPECT_EQ(record.nullable, nullable);
  EXPECT_EQ(record.name, std::string(reinterpret_cast<char*>(column_name)));
  EXPECT_EQ(record.name.size(), column_name_Le);
}

TEST(SQLDescribeColumn, FailInvalidhandle) {
  SQLSMALLINT data_type = 0;
  SQLULEN column_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = 0;
  SQLCHAR column_name[15];
  SQLSMALLINT column_name_le = 0;

  SQLRETURN status = SQLDescribeColInternal(
      nullptr, 1, column_name, 20, &column_name_le, &data_type, &column_size,
      &decimal_digits, &nullable);

  EXPECT_EQ(SQL_INVALID_HANDLE, status);
}

TEST(SQLDescribeColumn, FailColumnnumberiszero) {
  StatementHandle stmt_handle =
      CreateStmtHandleWithState(StmtStates::kStatementPrepared);
  SQLSMALLINT data_type = 0;
  SQLULEN column_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = 0;
  SQLCHAR column_name[15];
  SQLSMALLINT column_name_le = 0;

  SQLRETURN status = SQLDescribeColInternal(
      &stmt_handle, 0, column_name, 20, &column_name_le, &data_type,
      &column_size, &decimal_digits, &nullable);

  EXPECT_EQ(SQL_ERROR, status);
  EXPECT_EQ(SQLStates::k_07006(),
            stmt_handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLDescribeColumn, FailInvalidcolumnnumber) {
  StatementHandle stmt_handle =
      CreateStmtHandleWithState(StmtStates::kStatementPrepared);
  SQLSMALLINT data_type = 0;
  SQLULEN column_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = 0;
  SQLCHAR column_name[15];
  SQLSMALLINT column_name_le = 0;

  SQLRETURN status = SQLDescribeColInternal(
      &stmt_handle, 10, column_name, 20, &column_name_le, &data_type,
      &column_size, &decimal_digits, &nullable);

  EXPECT_EQ(SQL_ERROR, status);
  EXPECT_EQ(SQLStates::k_07009(),
            stmt_handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLDescribeColumn, FailStatementisnotprepared) {
  StatementHandle stmt_handle = CreateStatementHandle();
  DescriptorRecord record;
  DescriptorHandle& ird = stmt_handle.GetDescriptorHandle(DescriptorType::kIRD);
  SQLUSMALLINT column_number = 1;
  ird.BindNewDescriptorRecord(column_number, record);

  SQLSMALLINT data_type = 0;
  SQLULEN column_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = 0;
  SQLCHAR column_name[15];
  SQLSMALLINT column_name_le = 0;
  SQLRETURN status = SQLDescribeColInternal(
      &stmt_handle, column_number, column_name, 20, &column_name_le, &data_type,
      &column_size, &decimal_digits, &nullable);

  EXPECT_EQ(SQL_ERROR, status);
  EXPECT_EQ(SQLStates::k_HY010(),
            stmt_handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLDescribeColumn, DescribeSqlNumeric) {
  StatementHandle stmt_handle =
      CreateStmtHandleWithState(StmtStates::kStatementPrepared);
  DescriptorRecord record = CreateDescRecordWithRandomValues(SQL_NUMERIC);
  DescriptorHandle& ird = stmt_handle.GetDescriptorHandle(DescriptorType::kIRD);
  SQLUSMALLINT column_number = 1;
  ird.BindNewDescriptorRecord(column_number, record);

  SQLSMALLINT data_type = 0;
  SQLULEN column_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = 0;
  SQLCHAR column_name[15];
  SQLSMALLINT column_name_le = 0;
  SQLRETURN status = SQLDescribeColInternal(
      &stmt_handle, column_number, column_name, 20, &column_name_le, &data_type,
      &column_size, &decimal_digits, &nullable);

  AssertDescribeColumnResults(status, record, column_name, column_name_le,
                              data_type, column_size, decimal_digits, nullable);
}

TEST(SQLDescribeColumn, DescribeSqlChar) {
  StatementHandle stmt_handle =
      CreateStmtHandleWithState(StmtStates::kStatementPrepared);
  DescriptorRecord record = CreateDescRecordWithRandomValues(SQL_CHAR);
  DescriptorHandle& ird = stmt_handle.GetDescriptorHandle(DescriptorType::kIRD);
  SQLUSMALLINT column_number = 1;
  ird.BindNewDescriptorRecord(column_number, record);

  SQLSMALLINT data_type = 0;
  SQLULEN column_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = 0;
  SQLCHAR column_name[15];
  SQLSMALLINT column_name_le = 0;
  SQLRETURN status = SQLDescribeColInternal(
      &stmt_handle, column_number, column_name, 20, &column_name_le, &data_type,
      &column_size, &decimal_digits, &nullable);

  AssertDescribeColumnResults(status, record, column_name, column_name_le,
                              data_type, column_size, decimal_digits, nullable);
}

TEST(SQLDescribeColumn, DescribeSqlDate) {
  StatementHandle stmt_handle =
      CreateStmtHandleWithState(StmtStates::kStatementPrepared);
  DescriptorRecord record = CreateDescRecordWithRandomValues(SQL_TYPE_DATE);
  DescriptorHandle& ird = stmt_handle.GetDescriptorHandle(DescriptorType::kIRD);
  SQLUSMALLINT column_number = 1;
  ird.BindNewDescriptorRecord(column_number, record);

  SQLSMALLINT data_type = 0;
  SQLULEN column_size = 0;
  SQLSMALLINT decimal_digits = 0;
  SQLSMALLINT nullable = 0;
  SQLCHAR column_name[15];
  SQLSMALLINT column_name_le = 0;
  SQLRETURN status = SQLDescribeColInternal(
      &stmt_handle, column_number, column_name, 20, &column_name_le, &data_type,
      &column_size, &decimal_digits, &nullable);

  AssertDescribeColumnResults(status, record, column_name, column_name_le,
                              data_type, column_size, decimal_digits, nullable);
}

TEST(SQLColAttributeInternal, FailInvalidhandle) {
  ConnectionHandle conn_handle = CreateConnectionHandle(true);

  SQLRETURN status =
      SQLColAttributeInternal(&conn_handle, 0, 0, nullptr, 0, nullptr, nullptr);

  EXPECT_EQ(SQL_INVALID_HANDLE, status);
}

TEST(SQLColAttributeInternal, FailInvalidfieldidentifier) {
  StatementHandle stmt_handle = CreateStatementHandle();

  SQLRETURN status = SQLColAttributeInternal(&stmt_handle, 1, 111, nullptr, 0,
                                             nullptr, nullptr);

  EXPECT_EQ(SQL_ERROR, status);
  ASSERT_FALSE(stmt_handle.GetDiagnostics().GetStatusRecords().empty());
  EXPECT_EQ(SQLStates::k_HY091(),
            stmt_handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLCloseCursorInternal, FailInvalidhandle) {
  SQLRETURN status = SQLCloseCursorInternal(nullptr);

  EXPECT_EQ(SQL_INVALID_HANDLE, status);
}

TEST(SQLCloseCursorInternal, FailCursorisnotopen) {
  StatementHandle stmt_handle = CreateStatementHandle();

  SQLRETURN status = SQLCloseCursorInternal(&stmt_handle);

  EXPECT_EQ(SQL_ERROR, status);
  ASSERT_FALSE(stmt_handle.GetDiagnostics().GetStatusRecords().empty());
  EXPECT_EQ(SQLStates::k_24000(),
            stmt_handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLCloseCursorInternal, CloseCursorAftersqlexecute) {
  StatementHandle stmt_handle =
      CreateStmtHandleWithState(StmtStates::kStatementExecutedWithRs);
  stmt_handle.SetStatementPrepared();

  SQLRETURN status = SQLCloseCursorInternal(&stmt_handle);

  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(StmtStates::kStatementPrepared, stmt_handle.GetStmtState());
  EXPECT_FALSE(stmt_handle.IsCursorOpen());
}

TEST(SQLCloseCursorInternal, CloseCursorAftersqlexecdirect) {
  StatementHandle stmt_handle =
      CreateStmtHandleWithState(StmtStates::kStatementExecutedWithRs);

  SQLRETURN status = SQLCloseCursorInternal(&stmt_handle);

  EXPECT_EQ(SQL_SUCCESS, status);
  EXPECT_EQ(StmtStates::kStatementNotPrepared, stmt_handle.GetStmtState());
  EXPECT_FALSE(stmt_handle.IsCursorOpen());
}

TEST(SQLRowCountInternal, NullStatementHandle) {
  SQLLEN row_count = 0;
  SQLRETURN result = SQLRowCountInternal(nullptr, &row_count);

  EXPECT_EQ(result, SQL_INVALID_HANDLE);
}

TEST(SQLRowCountInternal, NullRowCountPointer) {
  StatementHandle handle = CreateStatementHandle();

  SQLRETURN result = SQLRowCountInternal(&handle, nullptr);
  ASSERT_EQ(result, SQL_ERROR);
  EXPECT_EQ(SQLStates::k_HY001(),
            handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLRowCountInternal, WrongState) {
  StatementHandle handle = CreateStatementHandle();
  handle.SetStmtState(StmtStates::kStatementNotPrepared);
  SQLLEN* row_count = nullptr;
  SQLRETURN ret = SQLRowCountInternal(&handle, row_count);

  ASSERT_EQ(ret, SQL_ERROR);
  EXPECT_EQ(SQLStates::k_HY001(),
            handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLGetData, InvalidColumnBookmark) {
  StatementHandle handle = CreateStatementHandle();
  handle.SetStmtState(StmtStates::kStatementPrepared);
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status =
      SQLGetDataInternal(&handle, 0, SQL_CHAR, buf, 1024, &target_str_len);
  ASSERT_EQ(SQL_ERROR, status);
  EXPECT_THAT(handle.GetDiagnostics().GetStatusRecords()[0].message,
              HasSubstr("Invalid descriptor index"));
  EXPECT_EQ(SQLStates::k_07009(),
            handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLGetData, FailNullTargetvalue) {
  StatementHandle handle = CreateStatementHandle();
  handle.SetStmtState(StmtStates::kStatementPrepared);
  SQLLEN target_str_len;
  SQLRETURN status =
      SQLGetDataInternal(&handle, 1, SQL_CHAR, nullptr, 1024, &target_str_len);
  ASSERT_EQ(SQL_ERROR, status);
  EXPECT_THAT(handle.GetDiagnostics().GetStatusRecords()[0].message,
              HasSubstr("Invalid use of null pointer"));
  EXPECT_EQ(SQLStates::k_HY009(),
            handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLGetData, InvalidBufferLength) {
  StatementHandle handle = CreateStatementHandle();
  handle.SetStmtState(StmtStates::kStatementPrepared);
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status =
      SQLGetDataInternal(&handle, 1, SQL_CHAR, buf, -20, &target_str_len);
  ASSERT_EQ(SQL_ERROR, status);
  EXPECT_THAT(handle.GetDiagnostics().GetStatusRecords()[0].message,
              HasSubstr("Invalid string or buffer length"));
  EXPECT_EQ(SQLStates::k_HY090(),
            handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLGetData, InvalidColumnNumber) {
  StatementHandle handle = CreateStatementHandle();
  handle.SetStmtState(StmtStates::kStatementPrepared);
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status =
      SQLGetDataInternal(&handle, 10, SQL_CHAR, buf, 1024, &target_str_len);
  ASSERT_EQ(SQL_ERROR, status);
  EXPECT_THAT(handle.GetDiagnostics().GetStatusRecords()[0].message,
              HasSubstr("Invalid Column In Result Set"));
  EXPECT_EQ(SQLStates::k_07009(),
            handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}

TEST(SQLGetData, InvalidTargetType) {
  StatementHandle handle = CreateStatementHandle();
  handle.SetStmtState(StmtStates::kStatementPrepared);
  SQLCHAR buf[20];
  SQLLEN target_str_len;
  SQLRETURN status =
      SQLGetDataInternal(&handle, 1, SQL_C_DATE, buf, 1024, &target_str_len);
  ASSERT_EQ(SQL_ERROR, status);
  EXPECT_THAT(handle.GetDiagnostics().GetStatusRecords()[0].message,
              HasSubstr("Program type out of range"));
  EXPECT_EQ(SQLStates::k_HY003(),
            handle.GetDiagnostics().GetStatusRecords()[0].sql_state);
}
namespace {

// A prepared statement positioned on a single row with one STRING column.
StatementHandle CreateStringResultHandle(std::string const& value) {
  StatementHandle handle = CreateStatementHandle();
  handle.SetStmtState(StmtStates::kStatementPrepared);
  auto& result_set = handle.GetResultSet();
  result_set.row_schema = {ColumnSchema{0, BQDataType::kString}};
  result_set.rows = {{DSValue(value.begin(), value.end())}};
  result_set.cursor = 0;
  return handle;
}

// Reads column 1 as SQL_C_WCHAR in chunks of `buffer_units` code units, each
// into a fresh guarded buffer, and returns the concatenated wire bytes.
std::string ReadWcharInChunks(StatementHandle& handle,
                              std::size_t buffer_units) {
  std::size_t const wire_sz = WireWcharSize();
  std::string collected;
  for (int call = 0; call < 100; ++call) {
    CanaryBuffer chunk(buffer_units * wire_sz);
    SQLLEN indicator = 0;
    SQLRETURN rc =
        SQLGetDataInternal(&handle, 1, SQL_C_WCHAR, chunk.data(),
                           static_cast<SQLLEN>(chunk.size()), &indicator);
    EXPECT_TRUE(chunk.CanariesIntact()) << "call " << call;
    EXPECT_TRUE(SQL_SUCCEEDED(rc)) << "call " << call;
    // Every chunk is NUL-terminated within the buffer.
    std::size_t units = 0;
    while (units < buffer_units && !chunk.IsNulAt(units, wire_sz)) ++units;
    EXPECT_LT(units, buffer_units) << "call " << call;
    collected.append(reinterpret_cast<char const*>(chunk.bytes()),
                     units * wire_sz);
    if (rc != SQL_SUCCESS_WITH_INFO) break;
  }
  return collected;
}

class SQLGetDataWcharTest : public ::testing::TestWithParam<WireEncoding> {
 protected:
  SQLGetDataWcharTest() : encoding_(GetParam()) {}

  static std::string Wire(std::string const& utf8) {
    auto wide = Utf8ToUtf16(utf8);
    return wide ? EncodeWideToWire(*wide) : std::string();
  }

  ScopedWireEncoding encoding_;
};

TEST_P(SQLGetDataWcharTest, FitsInOneCall) {
  std::string const value = "Hello";
  StatementHandle handle = CreateStringResultHandle(value);
  std::size_t const wire_sz = WireWcharSize();
  CanaryBuffer dest(16 * wire_sz);
  SQLLEN indicator = 0;

  EXPECT_EQ(SQL_SUCCESS,
            SQLGetDataInternal(&handle, 1, SQL_C_WCHAR, dest.data(),
                               static_cast<SQLLEN>(dest.size()), &indicator));
  EXPECT_TRUE(dest.CanariesIntact());
  EXPECT_EQ(indicator, value.size() * wire_sz);
  EXPECT_TRUE(dest.IsNulAt(value.size(), wire_sz));
}

TEST_P(SQLGetDataWcharTest, PartialDataStaysInBoundsAndReassembles) {
  std::string const value = "Hello, wide world! 0123456789";
  for (std::size_t units : {2, 3, 8, 29}) {
    SCOPED_TRACE("buffer_units=" + std::to_string(units));
    StatementHandle handle = CreateStringResultHandle(value);
    EXPECT_EQ(ReadWcharInChunks(handle, units), Wire(value));
  }
}

TEST_P(SQLGetDataWcharTest, PartialNonAsciiDataReassembles) {
  // 東京 😀 abc: multi-byte in UTF-8 and a surrogate pair in UTF-16.
  std::string const value = "\xE6\x9D\xB1\xE4\xBA\xAC \xF0\x9F\x98\x80 abc";
  for (std::size_t units : {2, 3, 4, 7}) {
    SCOPED_TRACE("buffer_units=" + std::to_string(units));
    StatementHandle handle = CreateStringResultHandle(value);
    EXPECT_EQ(ReadWcharInChunks(handle, units), Wire(value));
  }
}

TEST_P(SQLGetDataWcharTest, BufferSmallerThanOneCodeUnit) {
  std::size_t const wire_sz = WireWcharSize();
  if (wire_sz == 1) GTEST_SKIP() << "No buffer is smaller than one byte";
  StatementHandle handle = CreateStringResultHandle("Hello, world");
  CanaryBuffer dest(wire_sz - 1);
  SQLLEN indicator = 0;

  EXPECT_EQ(SQL_SUCCESS_WITH_INFO,
            SQLGetDataInternal(&handle, 1, SQL_C_WCHAR, dest.data(),
                               static_cast<SQLLEN>(dest.size()), &indicator));
  EXPECT_TRUE(dest.CanariesIntact());
}

INSTANTIATE_TEST_SUITE_P(WireEncodings, SQLGetDataWcharTest,
                         ::testing::Values(WireEncoding::kUtf8,
                                           WireEncoding::kUtf16Le,
                                           WireEncoding::kUtf32Le));

TEST(SQLGetDataWchar, MultiByteUtf8ThatFitsAsUtf16IsReturned) {
  // 8 UTF-8 bytes but only 4 UTF-16 code units: more bytes than the buffer
  // holds code units, yet the wire form and its NUL fit in one call.
  ScopedWireEncoding encoding(WireEncoding::kUtf16Le);
  std::string const value = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";  // éééé
  StatementHandle handle = CreateStringResultHandle(value);
  std::size_t const wire_sz = WireWcharSize();
  CanaryBuffer dest(5 * wire_sz);
  SQLLEN indicator = 0;

  EXPECT_EQ(SQL_SUCCESS,
            SQLGetDataInternal(&handle, 1, SQL_C_WCHAR, dest.data(),
                               static_cast<SQLLEN>(dest.size()), &indicator));
  EXPECT_TRUE(dest.CanariesIntact());
  auto wide = Utf8ToUtf16(value);
  ASSERT_TRUE(wide.Ok());
  std::string const expected = EncodeWideToWire(*wide);
  EXPECT_EQ(
      std::string(reinterpret_cast<char const*>(dest.bytes()), expected.size()),
      expected);
  EXPECT_TRUE(dest.IsNulAt(4, wire_sz));
  EXPECT_EQ(indicator, expected.size());
}

}  // namespace

}  // namespace google::cloud::odbc_bq_driver
