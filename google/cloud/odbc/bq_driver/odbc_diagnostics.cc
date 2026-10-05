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
#include "google/cloud/odbc/bq_driver/internal/odbc_handle.h"
#include "google/cloud/odbc/bq_driver/internal/odbc_type_utils.h"
#include "google/cloud/odbc/bq_driver/internal/trace_utils.h"
#include "google/cloud/odbc/bq_driver/odbc_utils.h"
#include "google/cloud/odbc/internal/status_record_or.h"
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>

namespace google::cloud::odbc_bq_driver {

using google::cloud::odbc_bq_driver_internal::ConnectionHandle;
using google::cloud::odbc_bq_driver_internal::CopyUtf8ToWireBuffer;
using google::cloud::odbc_bq_driver_internal::DescriptorHandle;
using google::cloud::odbc_bq_driver_internal::Diagnostics;
using google::cloud::odbc_bq_driver_internal::EnvironmentHandle;
using google::cloud::odbc_bq_driver_internal::HandleType;
using google::cloud::odbc_bq_driver_internal::IntValueToOutputBufferResponse;
using google::cloud::odbc_bq_driver_internal::IsDiagIdentifierString;
using google::cloud::odbc_bq_driver_internal::SaturateLength;
using google::cloud::odbc_bq_driver_internal::StatementHandle;
using google::cloud::odbc_bq_driver_internal::StringValueToOutputBufferResponse;
using google::cloud::odbc_bq_driver_internal::WireUnitsForBytes;
using google::cloud::odbc_bq_driver_internal::WireWcharSize;
using google::cloud::odbc_internal::SQLStates;
using google::cloud::odbc_internal::StatusRecord;
using google::cloud::odbc_internal::StatusRecordOr;

static std::string const kPrefix = "[Google][ODBC BigQuery Driver] ";
static std::string const kIso9075 = "ISO 9075";
static std::string const kOdbc3 = "ODBC 3.0";
// SQLSTATE is always 5 characters plus a NUL terminator.
constexpr std::size_t kSqlStateUnits = 6;
// Size of the UTF-8 buffers the W functions hand to the internal functions.
// The SQLSMALLINT length arguments cannot describe anything longer, so this is
// independent of, and never smaller than, the caller's buffer.
constexpr SQLSMALLINT kUtf8DiagBufferLen =
    std::numeric_limits<SQLSMALLINT>::max();

static std::vector<std::string> const kOdbcSubclasses = {
    SQLStates::k_01S00(), SQLStates::k_01S01(), SQLStates::k_01S02(),
    SQLStates::k_01S06(), SQLStates::k_01S07(), SQLStates::k_07S01(),
    SQLStates::k_08S01(), SQLStates::k_21S01(), SQLStates::k_21S02(),
    SQLStates::k_25S01(), SQLStates::k_25S02(), SQLStates::k_25S03(),
    SQLStates::k_42S01(), SQLStates::k_42S01(), SQLStates::k_42S11(),
    SQLStates::k_42S12(), SQLStates::k_42S21(), SQLStates::k_42S22(),
    SQLStates::k_HY095(), SQLStates::k_HY097(), SQLStates::k_HY098(),
    SQLStates::k_HY099(), SQLStates::k_HY100(), SQLStates::k_HY101(),
    SQLStates::k_HY105(), SQLStates::k_HY107(), SQLStates::k_HY109(),
    SQLStates::k_HY110(), SQLStates::k_HY111(), SQLStates::k_HYT00(),
    SQLStates::k_HYT01(), SQLStates::k_IM001(), SQLStates::k_IM002(),
    SQLStates::k_IM003(), SQLStates::k_IM004(), SQLStates::k_IM005(),
    SQLStates::k_IM006(), SQLStates::k_IM007(), SQLStates::k_IM008(),
    SQLStates::k_IM010(), SQLStates::k_IM011(), SQLStates::k_IM012()};

StatusRecordOr<Diagnostics> GetDiagnostics(SQLSMALLINT handleType,
                                           SQLHANDLE handle) {
  if (handle == nullptr) {
    return StatusRecordOr<Diagnostics>(
        StatusRecord{SQLStates::k_HY000(), "Handle is null pointer"},
        SQL_INVALID_HANDLE);
  }
  switch (handleType) {
    case SQL_HANDLE_ENV: {
      auto* handle_ptr = reinterpret_cast<EnvironmentHandle*>(handle);
      if (handle_ptr->kType != HandleType::kEnvHandle) {
        return StatusRecordOr<Diagnostics>(
            StatusRecord{SQLStates::k_HY000(), "Invalid handle type"},
            SQL_INVALID_HANDLE);
      }
      return handle_ptr->GetDiagnostics();
    }
    case SQL_HANDLE_DBC: {
      auto* handle_ptr = reinterpret_cast<ConnectionHandle*>(handle);
      if (handle_ptr->kType != HandleType::kConnHandle) {
        return StatusRecordOr<Diagnostics>(
            StatusRecord{SQLStates::k_HY000(), "Invalid handle type"},
            SQL_INVALID_HANDLE);
      }
      return handle_ptr->GetDiagnostics();
    }
    case SQL_HANDLE_STMT: {
      auto* handle_ptr = reinterpret_cast<StatementHandle*>(handle);
      if (handle_ptr->kType != HandleType::kStmtHandle) {
        return StatusRecordOr<Diagnostics>(
            StatusRecord{SQLStates::k_HY000(), "Invalid handle type"},
            SQL_INVALID_HANDLE);
      }
      return handle_ptr->GetDiagnostics();
    }
    case SQL_HANDLE_DESC: {
      auto* handle_ptr = reinterpret_cast<DescriptorHandle*>(handle);
      if (handle_ptr->kType != HandleType::kDescHandle) {
        return StatusRecordOr<Diagnostics>(
            StatusRecord{SQLStates::k_HY000(), "Invalid handle type"},
            SQL_INVALID_HANDLE);
      }
      return handle_ptr->GetDiagnostics();
    }
    default:
      return StatusRecord{SQLStates::k_HY000(), "handleType is not recognized"};
  }
}

SQLRETURN SQLGetDiagFieldInternal(SQLSMALLINT handle_type, SQLHANDLE handle,
                                  SQLSMALLINT rec_number,
                                  SQLSMALLINT diag_identifier,
                                  SQLPOINTER diag_info,
                                  SQLSMALLINT diag_info_buffer_len,
                                  SQLSMALLINT* diag_info_string_len) {
  LOG(INFO) << "SQLGetDiagFieldInternal:: Start";
  StatusRecordOr<Diagnostics> diagnostic_status =
      GetDiagnostics(handle_type, handle);
  if (!diagnostic_status) {
    LOG(ERROR) << "SQLGetDiagField::GetDiagnostics:: "
               << diagnostic_status.GetStatusRecord().message;
    return diagnostic_status.GetCalculatedReturnCode();
  }
  Diagnostics diagnostics = *diagnostic_status;

  // Header Record diagnostics:
  auto header_record = diagnostics.GetHeaderRecord();
  switch (diag_identifier) {
    case SQL_DIAG_DYNAMIC_FUNCTION: {
      StatusRecord result = StringValueToOutputBufferResponse(
          header_record.function.c_str(), diag_info, diag_info_buffer_len,
          diag_info_string_len);
      return result.CalculateReturnCode();
    }
    case SQL_DIAG_DYNAMIC_FUNCTION_CODE: {
      return IntValueToOutputBufferResponse(header_record.function_code,
                                            diag_info, diag_info_string_len);
    }
    case SQL_DIAG_CURSOR_ROW_COUNT: {
      return IntValueToOutputBufferResponse(header_record.cursor_row_count,
                                            diag_info, diag_info_string_len);
    }
    case SQL_DIAG_ROW_COUNT:
      return IntValueToOutputBufferResponse(header_record.row_count, diag_info,
                                            diag_info_string_len);
    case SQL_DIAG_NUMBER:
      return IntValueToOutputBufferResponse<SQLINTEGER>(
          diagnostics.GetStatusRecords().size(), diag_info,
          diag_info_string_len);
  }

  // recNumber validation
  if (rec_number <= 0) {
    LOG(ERROR) << "SQLGetDiagField:: recNumber is less than 1";
    return SQL_ERROR;
  }
  if (static_cast<unsigned>(rec_number) >
      diagnostics.GetStatusRecords().size()) {
    LOG(WARNING)
        << "SQLGetDiagField:: There is no Status Record for such recNumber";
    return SQL_NO_DATA;
  }

  // Status Records diagnostics:
  auto status_record = diagnostics.GetStatusRecords()[rec_number - 1];
  switch (diag_identifier) {
    case SQL_DIAG_SQLSTATE: {
      StatusRecord result = StringValueToOutputBufferResponse(
          status_record.sql_state.c_str(), diag_info, diag_info_buffer_len,
          diag_info_string_len);
      return result.CalculateReturnCode();
    }
    case SQL_DIAG_MESSAGE_TEXT: {
      StatusRecord result = StringValueToOutputBufferResponse(
          (kPrefix + status_record.message).c_str(), diag_info,
          diag_info_buffer_len, diag_info_string_len);
      return result.CalculateReturnCode();
    }
    case SQL_DIAG_NATIVE:
      return IntValueToOutputBufferResponse(status_record.native_error_code,
                                            diag_info, diag_info_string_len);
    case SQL_DIAG_COLUMN_NUMBER:
      return IntValueToOutputBufferResponse(status_record.column_number,
                                            diag_info, diag_info_string_len);
    case SQL_DIAG_ROW_NUMBER:
      return IntValueToOutputBufferResponse(status_record.row_number, diag_info,
                                            diag_info_string_len);
    case SQL_DIAG_CONNECTION_NAME: {
      StatusRecord result = StringValueToOutputBufferResponse(
          status_record.connection_name.c_str(), diag_info,
          diag_info_buffer_len, diag_info_string_len);
      return result.CalculateReturnCode();
    }
    case SQL_DIAG_SERVER_NAME: {
      StatusRecord result = StringValueToOutputBufferResponse(
          status_record.server_name.c_str(), diag_info, diag_info_buffer_len,
          diag_info_string_len);
      return result.CalculateReturnCode();
    }
    case SQL_DIAG_CLASS_ORIGIN: {
      std::string class_origin =
          absl::StartsWith(status_record.sql_state, "IM") ? kOdbc3 : kIso9075;
      StatusRecord result = StringValueToOutputBufferResponse(
          class_origin.c_str(), diag_info, diag_info_buffer_len,
          diag_info_string_len);
      return result.CalculateReturnCode();
    }
    case SQL_DIAG_SUBCLASS_ORIGIN: {
      std::string subclass_origin =
          (std::find(kOdbcSubclasses.begin(), kOdbcSubclasses.end(),
                     status_record.sql_state) != kOdbcSubclasses.end())
              ? kOdbc3
              : kIso9075;
      StatusRecord result = StringValueToOutputBufferResponse(
          subclass_origin.c_str(), diag_info, diag_info_buffer_len,
          diag_info_string_len);
      return result.CalculateReturnCode();
    }
  }
  // diagIdentifier is invalid
  LOG(ERROR) << "SQLGetDiagField:: diagIdentifier is invalid";
  return SQL_ERROR;
}

SQLRETURN SQLGetDiagRecInternal(SQLSMALLINT handle_type, SQLHANDLE handle,
                                SQLSMALLINT rec_number, SQLCHAR* sql_state,
                                SQLINTEGER* native_error, SQLCHAR* message_text,
                                SQLSMALLINT message_text_buffer_len,
                                SQLSMALLINT* message_text_len) {
  LOG(INFO) << "SQLGetDiagRecInternal:: Start";
  StatusRecordOr<Diagnostics> diagnostic_status =
      GetDiagnostics(handle_type, handle);
  if (!diagnostic_status) {
    LOG(ERROR) << "SQLGetDiagRec::GetDiagnostics "
               << diagnostic_status.GetStatusRecord().message;
    return diagnostic_status.GetCalculatedReturnCode();
  }

  // recNumber validation
  if (rec_number <= 0) {
    LOG(ERROR) << "SQLGetDiagRec:: recNumber is less than 1";
    return SQL_ERROR;
  }
  if (static_cast<unsigned>(rec_number) >
      diagnostic_status->GetStatusRecords().size()) {
    LOG(WARNING)
        << "SQLGetDiagRec:: There is no Status Record for such recNumber";
    return SQL_NO_DATA;
  }

  auto status_record = diagnostic_status->GetStatusRecords()[rec_number - 1];

  // Writing down SqlState (always 5 characters + terminating NULL)
  StatusRecord sqlstate_result = StringValueToOutputBufferResponse<SQLINTEGER>(
      status_record.sql_state.c_str(), sql_state, 6, nullptr);
  if (!sqlstate_result.ok()) {
    return sqlstate_result.CalculateReturnCode();
  }
  // Writing down Message
  StatusRecord message_result = StringValueToOutputBufferResponse(
      (kPrefix + status_record.message).c_str(), message_text,
      message_text_buffer_len, message_text_len);
  if (!message_result.ok()) {
    return message_result.CalculateReturnCode();
  }
  // Writing down NativeErrorCode
  return IntValueToOutputBufferResponse<SQLINTEGER, SQLINTEGER>(
      status_record.native_error_code, native_error, nullptr);
}

SQLRETURN SQLGetDiagFieldWInternal(SQLSMALLINT handle_type, SQLHANDLE handle,
                                   SQLSMALLINT rec_number,
                                   SQLSMALLINT diag_identifier,
                                   SQLPOINTER diag_info,
                                   SQLSMALLINT diag_info_buffer_len,
                                   SQLSMALLINT* diag_info_string_len) {
  if (!IsDiagIdentifierString(diag_identifier)) {
    return SQLGetDiagFieldInternal(handle_type, handle, rec_number,
                                   diag_identifier, diag_info,
                                   diag_info_buffer_len, diag_info_string_len);
  }
  if (diag_info_buffer_len < 0) {
    LOG(ERROR) << "SQLGetDiagFieldW:: BufferLength is negative";
    return SQL_ERROR;
  }

  // Fetch the UTF-8 value into a local buffer, then copy it into the caller's
  // SQLWCHAR buffer of diag_info_buffer_len bytes.
  std::vector<SQLCHAR> utf8(kUtf8DiagBufferLen, 0);
  SQLSMALLINT utf8_len = 0;
  SQLRETURN rc =
      SQLGetDiagFieldInternal(handle_type, handle, rec_number, diag_identifier,
                              utf8.data(), kUtf8DiagBufferLen, &utf8_len);
  if (!SQL_SUCCEEDED(rc)) return rc;

  std::string_view value(
      reinterpret_cast<char const*>(utf8.data()),
      std::strlen(reinterpret_cast<char const*>(utf8.data())));
  auto copied = CopyUtf8ToWireBuffer(value, diag_info,
                                     WireUnitsForBytes(diag_info_buffer_len));
  if (!copied) {
    LOG(ERROR) << "SQLGetDiagFieldW:: " << copied.GetStatusRecord().message;
    return SQL_ERROR;
  }
  if (diag_info_string_len) {
    *diag_info_string_len =
        SaturateLength<SQLSMALLINT>(copied->total_units * WireWcharSize());
  }
  return copied->truncated ? SQL_SUCCESS_WITH_INFO : rc;
}

SQLRETURN SQLGetDiagRecWInternal(SQLSMALLINT handle_type, SQLHANDLE handle,
                                 SQLSMALLINT rec_number, SQLWCHAR* sql_state,
                                 SQLINTEGER* native_error,
                                 SQLWCHAR* message_text,
                                 SQLSMALLINT message_text_buffer_len,
                                 SQLSMALLINT* message_text_len) {
  if (message_text_buffer_len < 0) {
    LOG(ERROR) << "SQLGetDiagRecW:: BufferLength is negative";
    return SQL_ERROR;
  }

  // The internal function writes UTF-8 into local buffers that are sized
  // independently of the caller's SQLWCHAR buffers. Only the bounded copies
  // below touch caller memory.
  SQLCHAR utf8_sql_state[kSqlStateUnits] = {0};
  std::vector<SQLCHAR> utf8_message(kUtf8DiagBufferLen, 0);
  SQLSMALLINT utf8_message_len = 0;
  SQLRETURN rc = SQLGetDiagRecInternal(
      handle_type, handle, rec_number, utf8_sql_state, native_error,
      utf8_message.data(), kUtf8DiagBufferLen, &utf8_message_len);
  if (!SQL_SUCCEEDED(rc)) return rc;

  if (sql_state) {
    // Exactly 5 characters and a NUL: kSqlStateUnits wire code units.
    auto copied =
        CopyUtf8ToWireBuffer(reinterpret_cast<char const*>(utf8_sql_state),
                             sql_state, kSqlStateUnits);
    if (!copied) {
      LOG(ERROR) << "SQLGetDiagRecW:: " << copied.GetStatusRecord().message;
      return SQL_ERROR;
    }
  }

  std::string_view message(
      reinterpret_cast<char const*>(utf8_message.data()),
      std::strlen(reinterpret_cast<char const*>(utf8_message.data())));
  auto copied = CopyUtf8ToWireBuffer(
      message, message_text, static_cast<std::size_t>(message_text_buffer_len));
  if (!copied) {
    LOG(ERROR) << "SQLGetDiagRecW:: " << copied.GetStatusRecord().message;
    return SQL_ERROR;
  }
  // Per the ODBC spec this is the full length in characters, excluding the
  // NUL terminator, even when the message was truncated.
  if (message_text_len) {
    *message_text_len = SaturateLength<SQLSMALLINT>(copied->total_units);
  }
  return copied->truncated ? SQL_SUCCESS_WITH_INFO : rc;
}

}  // namespace google::cloud::odbc_bq_driver
