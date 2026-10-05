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

//////////////////////////////////////////////////////////////////
// This file is the entry point for the implementation of the
// ODBC APIs defined in <sql.h>, <sqlext.h> and <sqlucode.h>
//////////////////////////////////////////////////////////////////

#include "google/cloud/odbc/bq_driver/internal/odbc_conn_attr.h"
#include "google/cloud/odbc/bq_driver/internal/odbc_type_utils.h"
#include "google/cloud/odbc/bq_driver/internal/trace_utils.h"
#include "google/cloud/odbc/bq_driver/internal/utils.h"
#include "google/cloud/odbc/bq_driver/odbc_commons.h"
#include "google/cloud/odbc/bq_driver/odbc_connection.h"
#include "google/cloud/odbc/bq_driver/odbc_descriptor.h"
#include "google/cloud/odbc/bq_driver/odbc_diagnostics.h"
#include "google/cloud/odbc/bq_driver/odbc_driver_metadata.h"
#include "google/cloud/odbc/bq_driver/odbc_environment.h"
#include "google/cloud/odbc/bq_driver/odbc_lock.h"
#include "google/cloud/odbc/bq_driver/odbc_sql_requests.h"
#include "google/cloud/odbc/bq_driver/odbc_sql_results.h"
#include "google/cloud/odbc/bq_driver/odbc_statement.h"
#include "google/cloud/odbc/bq_driver/odbc_utils.h"
#include "google/cloud/odbc/internal/odbc_includes.h"
#include "google/cloud/status_or.h"
#include <cstring>
#include <limits>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include "google/cloud/odbc/bq_driver/odbc_windows.h"
#endif  //_WIN32
////////////////////////////////////////////////////////////////////////////////////////
//
// ODBC APIs supported in initial driver release.
//
////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////
// Suppressing clang-tidy errors as we don't have function
// implementation right now. Remove the lint blocks once
// functions are implemented.
////////////////////////////////////////////////////////////////////////////////////////

// NOLINTBEGIN

using ::google::cloud::Status;
using ::google::cloud::StatusOr;
using google::cloud::odbc_bq_driver_internal::BqConvertSQLWCHARToString;
using google::cloud::odbc_bq_driver_internal::ConnectionAttr;
using google::cloud::odbc_bq_driver_internal::ConnectionHandle;
using google::cloud::odbc_bq_driver_internal::ConnectionValueType;
using google::cloud::odbc_bq_driver_internal::CopyUtf8ToWireBuffer;
using google::cloud::odbc_bq_driver_internal::DescriptorHandle;
using google::cloud::odbc_bq_driver_internal::IsFieldIdentifierString;
using google::cloud::odbc_bq_driver_internal::SaturateLength;
using google::cloud::odbc_bq_driver_internal::StatementHandle;
using ::google::cloud::odbc_bq_driver_internal::TraceOptions;
using google::cloud::odbc_bq_driver_internal::WireCopyResult;
using google::cloud::odbc_bq_driver_internal::WireUnitsForBytes;
using google::cloud::odbc_bq_driver_internal::WireWcharSize;
using ::google::cloud::odbc_internal::SQLStates;
using google::cloud::odbc_internal::StatusRecord;
using ::google::cloud::odbc_internal::StatusRecordOr;

using ::google::cloud::odbc_bq_driver::HandleLock;
using ::google::cloud::odbc_bq_driver::HandleLockError;

using google::cloud::odbc_bq_driver::ToCharStr;
using google::cloud::odbc_bq_driver::ToSqlChar;

constexpr int kBufferLength = 4096;
// Size of the local UTF-8 buffers for W APIs whose length arguments are
// SQLSMALLINT: large enough for any string those arguments can describe, and
// independent of the caller's buffer size.
constexpr SQLSMALLINT kSmallIntBufferLength =
    std::numeric_limits<SQLSMALLINT>::max();

// Internal Helper Functions
namespace {
void RecordTraceStatus(std::string const& name, StatusRecord const& s) {
  if (!s.ok()) {
    std::cout << "Tracing is misconfigured: " << s.message << std::endl;
    std::cout << "ODBC API: " << name << " will not be traced." << std::endl;
  }
}

void InitializeTracing(std::string const& name) {
  bool const kInitLogging = TraceOptions::InitializeLogging();
}

// Converts input pointer value which is SQLWCHAR* to SQLCHAR*
// and returns the converted value.
StatusRecordOr<std::string> ConvertSQLPointerToSQLChar(SQLPOINTER in_val,
                                                       SQLINTEGER in_val_len) {
  SQLWCHAR* in_wchar_val = reinterpret_cast<SQLWCHAR*>(in_val);
  StatusRecordOr<std::string> utf8_in_val =
      BqConvertSQLWCHARToString(in_wchar_val, in_val_len);
  if (!utf8_in_val) {
    return utf8_in_val.GetStatusRecord();
  }
  return utf8_in_val;
}

// Copies the NUL-terminated UTF-8 string in `utf8` (a local buffer of
// `utf8_capacity` bytes filled by an internal function) into the caller's
// SQLWCHAR buffer `dest`, which holds `dest_units` wire code units.
StatusRecordOr<WireCopyResult> CopyUtf8BufferToWire(SQLCHAR const* utf8,
                                                    std::size_t utf8_capacity,
                                                    void* dest,
                                                    std::size_t dest_units) {
  auto const* str = reinterpret_cast<char const*>(utf8);
  return CopyUtf8ToWireBuffer(
      std::string_view(str, strnlen(str, utf8_capacity)), dest, dest_units);
}

// Number of wire code units in a W API character-count buffer length.
std::size_t WireUnitsForChars(SQLLEN char_len) {
  return char_len > 0 ? static_cast<std::size_t>(char_len) : 0;
}

// Records 01004 on `handle` after a W API truncated an output string, and
// turns SQL_SUCCESS into SQL_SUCCESS_WITH_INFO.
template <typename HandleT>
SQLRETURN WithTruncationWarning(SQLHANDLE handle, SQLRETURN rc) {
  reinterpret_cast<HandleT*>(handle)->GetDiagnostics().AddStatusRecord(
      StatusRecord{SQLStates::k_01004(), "String data, right truncated"});
  return rc == SQL_SUCCESS ? SQL_SUCCESS_WITH_INFO : rc;
}
}  // namespace

////////////////////////////////////////////////////////////////////////////////////////
// SQLAllocHandle allocates an environment, connection, statement,
// or descriptor handle based on the handle type passed in.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlallochandle-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT handleType, SQLHANDLE inputHandle,
                                 SQLHANDLE* outputHandle) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLAllocHandle");

  switch (handleType) {
    case SQL_HANDLE_ENV: {
      // Call to Acquire mutex for environment handle in odbc_lock.h.
      // Environment handle has no parent, so inputHandle isn't required.
      // Passing nullptr lets HandleLock use the global driver mutex for
      // thread-safe allocation.
      HandleLock lock(nullptr, SQL_HANDLE_ENV, true);
      if (!lock.isLocked()) {
        return SQL_ERROR;
      }
      rc = google::cloud::odbc_bq_driver::SQLAllocEnvHandle(outputHandle);

      // Call to Release mutex for environment handle in odbc_lock.h.
      break;
    }
    case SQL_HANDLE_DBC: {
      // Call to Acquire mutex for connection handle in odbc_lock.h.
      HandleLock lock(inputHandle, SQL_HANDLE_ENV);
      if (!lock.isLocked()) {
        HandleLockError(SQL_HANDLE_DBC, inputHandle, "SQLAllocHandle");
        return SQL_ERROR;
      }
      rc = google::cloud::odbc_bq_driver::SQLAllocConnHandle(inputHandle,
                                                             outputHandle);
      // Call to Release mutex for connection handle in odbc_lock.h.
      break;
    }
    case SQL_HANDLE_STMT: {
      // Call to Acquire mutex for connection handle in odbc_lock.h.
      HandleLock lock(inputHandle, SQL_HANDLE_DBC);
      if (!lock.isLocked()) {
        HandleLockError(SQL_HANDLE_STMT, inputHandle, "SQLAllocHandle");
        return SQL_ERROR;
      }
      rc = google::cloud::odbc_bq_driver::SQLAllocStmtHandle(inputHandle,
                                                             outputHandle);

      // Call to Release mutex for connection handle in odbc_lock.h.
      break;
    }
    case SQL_HANDLE_DESC: {
      // Call to Acquire mutex for descriptor handle in odbc_lock.h.
      HandleLock lock(inputHandle, SQL_HANDLE_DBC);
      if (!lock.isLocked()) {
        HandleLockError(SQL_HANDLE_DESC, inputHandle, "SQLAllocHandle");
        return SQL_ERROR;
      }
      rc = google::cloud::odbc_bq_driver::SQLAllocDescHandle(inputHandle,
                                                             outputHandle);

      // Call to Trace function exit in odbc_trace.h if tracing is enabled.
      // Call to Release mutex for descriptor handle in odbc_lock.h.
      break;
    }
    default: {
      return SQL_INVALID_HANDLE;
    }
  }

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Establishes connection to a driver and data source.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldriverconnect-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLDriverConnectA(
    SQLHDBC connectionHandle, SQLHWND windowHandle, SQLCHAR* inConnectionString,
    SQLSMALLINT inConnectionStringLen, SQLCHAR* outConnectionString,
    SQLSMALLINT outConnectionStringBufferLen,
    SQLSMALLINT* outConnectionStringLen, SQLUSMALLINT driverCompletion) {
  return SQLDriverConnect(connectionHandle, windowHandle, inConnectionString,
                          inConnectionStringLen, outConnectionString,
                          outConnectionStringBufferLen, outConnectionStringLen,
                          driverCompletion);
}

SQLRETURN SQL_API SQLDriverConnect(
    SQLHDBC connectionHandle, SQLHWND windowHandle, SQLCHAR* inConnectionString,
    SQLSMALLINT inConnectionStringLen, SQLCHAR* outConnectionString,
    SQLSMALLINT outConnectionStringBufferLen,
    SQLSMALLINT* outConnectionStringLen, SQLUSMALLINT driverCompletion) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLDriverConnect");

  // Call to Acquire mutex for connection handle in odbc_lock.h.
  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLDriverConnect");
    return SQL_ERROR;
  }

  // Call to internal common function for SQLDriverConnect and SQLDriverConnectW
  // in odbc_connection.h.
  rc = google::cloud::odbc_bq_driver::SQLDriverConnectInternal(
      connectionHandle, windowHandle, inConnectionString, inConnectionStringLen,
      outConnectionString, outConnectionStringBufferLen, outConnectionStringLen,
      driverCompletion);

  return rc;
}

//////////////////////////////////////
// Unicode version of SQLDriverConnect.
//////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLDriverConnectW(
    SQLHDBC connectionHandle, SQLHWND windowHandle,
    SQLWCHAR* inConnectionString, SQLSMALLINT inConnectionStringLen,
    SQLWCHAR* outConnectionString, SQLSMALLINT outConnectionStringBufferLen,
    SQLSMALLINT* outConnectionStringLen, SQLUSMALLINT driverCompletion) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLDriverConnectW");
  // Call to Acquire mutex for connection handle in odbc_lock.h.
  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLDriverConnectW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_in_connection_str;
  SQLCHAR* sqlchar_in_connection_str = nullptr;
  if (inConnectionString) {
    utf8_in_connection_str =
        BqConvertSQLWCHARToString(inConnectionString, inConnectionStringLen);
    if (!utf8_in_connection_str) {
      return utf8_in_connection_str.GetCalculatedReturnCode();
    }
    sqlchar_in_connection_str = ToSqlChar(utf8_in_connection_str->data());
    if (inConnectionStringLen && inConnectionStringLen != SQL_NTS)
      inConnectionStringLen = utf8_in_connection_str->length();
  }
  // The internal function writes the UTF-8 output connection string into a
  // local buffer. It is then copied into outConnectionString, which holds
  // outConnectionStringBufferLen wire code units.
  std::vector<SQLCHAR> out_conn_str;
  if (outConnectionString) out_conn_str.assign(kSmallIntBufferLength, 0);
  SQLSMALLINT out_conn_str_len = 0;
  // Call to internal common function for SQLDriverConnect and
  // SQLDriverConnectW in odbc_connection.h.
  rc = google::cloud::odbc_bq_driver::SQLDriverConnectInternal(
      connectionHandle, windowHandle, sqlchar_in_connection_str,
      inConnectionStringLen,
      outConnectionString ? out_conn_str.data() : nullptr,
      outConnectionString ? kSmallIntBufferLength : 0, &out_conn_str_len,
      driverCompletion);

  // Handle Unicode conversion of output parameters.
  if (SQL_SUCCEEDED(rc) && outConnectionString) {
    auto copied = CopyUtf8BufferToWire(
        out_conn_str.data(), out_conn_str.size(), outConnectionString,
        WireUnitsForChars(outConnectionStringBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    out_conn_str_len = SaturateLength<SQLSMALLINT>(copied->total_units);
    if (copied->truncated) {
      rc = WithTruncationWarning<ConnectionHandle>(connectionHandle, rc);
    }
  }
  if (outConnectionStringLen) *outConnectionStringLen = out_conn_str_len;

  // Call to Release mutex for connection handle in odbc_lock.h.

  return rc;
}
////////////////////////////////////////////////////////////////////////////////////////
// Establishes connection to a driver and data source.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlbrowseconnect-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLBrowseConnectA(SQLHDBC connectionHandle,
                                    SQLCHAR* inConnectionString,
                                    SQLSMALLINT inConnectionStringLen,
                                    SQLCHAR* outConnectionString,
                                    SQLSMALLINT outConnectionStringBufferLen,
                                    SQLSMALLINT* outConnectionStringLen) {
  return SQLBrowseConnect(connectionHandle, inConnectionString,
                          inConnectionStringLen, outConnectionString,
                          outConnectionStringBufferLen, outConnectionStringLen);
}

SQLRETURN SQL_API SQLBrowseConnect(SQLHDBC connectionHandle,
                                   SQLCHAR* inConnectionString,
                                   SQLSMALLINT inConnectionStringLen,
                                   SQLCHAR* outConnectionString,
                                   SQLSMALLINT outConnectionStringBufferLen,
                                   SQLSMALLINT* outConnectionStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLBrowseConnect");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLBrowseConnect");
    return SQL_ERROR;
  }

  // Call to internal common function for SQLBrowseConnect and SQLBrowseConnectW
  // in odbc_connection.h.
  rc = google::cloud::odbc_bq_driver::SQLBrowseConnectInternal(
      connectionHandle, inConnectionString, inConnectionStringLen,
      outConnectionString, outConnectionStringBufferLen,
      outConnectionStringLen);

  return rc;
}
//////////////////////////////////////
// Unicode version of SQLBrowseConnect.
//////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLBrowseConnectW(SQLHDBC connectionHandle,
                                    SQLWCHAR* inConnectionString,
                                    SQLSMALLINT inConnectionStringLen,
                                    SQLWCHAR* outConnectionString,
                                    SQLSMALLINT outConnectionStringBufferLen,
                                    SQLSMALLINT* outConnectionStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLBrowseConnectW");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLBrowseConnectW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  SQLCHAR* sqlchar_in_connection_str = nullptr;
  StatusRecordOr<std::string> utf8_in_connection_str;
  if (inConnectionString) {
    utf8_in_connection_str =
        BqConvertSQLWCHARToString(inConnectionString, inConnectionStringLen);
    if (!utf8_in_connection_str) {
      return utf8_in_connection_str.GetCalculatedReturnCode();
    }
    sqlchar_in_connection_str = ToSqlChar(utf8_in_connection_str->data());
    if (inConnectionStringLen && inConnectionStringLen != SQL_NTS)
      inConnectionStringLen = utf8_in_connection_str->length();
  }
  // Call to internal common function for SQLBrowseConnect and SQLBrowseConnectW
  // in odbc_connection.h. The UTF-8 output goes to a local buffer and is then
  // copied into outConnectionString, which holds outConnectionStringBufferLen
  // wire code units.
  std::vector<SQLCHAR> out_connection_string;
  if (outConnectionString) {
    out_connection_string.assign(kSmallIntBufferLength, 0);
  }
  SQLSMALLINT out_connection_string_len = 0;
  rc = google::cloud::odbc_bq_driver::SQLBrowseConnectInternal(
      connectionHandle, sqlchar_in_connection_str, inConnectionStringLen,
      outConnectionString ? out_connection_string.data() : nullptr,
      outConnectionString ? kSmallIntBufferLength : 0,
      &out_connection_string_len);

  // Handle Unicode conversion of output parameters.
  if ((SQL_SUCCEEDED(rc) || rc == SQL_NEED_DATA) && outConnectionString) {
    auto copied = CopyUtf8BufferToWire(
        out_connection_string.data(), out_connection_string.size(),
        outConnectionString, WireUnitsForChars(outConnectionStringBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    out_connection_string_len =
        SaturateLength<SQLSMALLINT>(copied->total_units);
    if (copied->truncated && SQL_SUCCEEDED(rc)) {
      rc = WithTruncationWarning<ConnectionHandle>(connectionHandle, rc);
    }
  }
  if (outConnectionStringLen) {
    *outConnectionStringLen = out_connection_string_len;
  }

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Establishes connection to a driver and data source.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlconnect-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLConnectA(SQLHDBC connectionHandle, SQLCHAR* serverName,
                              SQLSMALLINT serverNameLen, SQLCHAR* userName,
                              SQLSMALLINT userNameLen, SQLCHAR* authString,
                              SQLSMALLINT authStringLen) {
  return SQLConnect(connectionHandle, serverName, serverNameLen, userName,
                    userNameLen, authString, authStringLen);
}

SQLRETURN SQL_API SQLConnect(SQLHDBC connectionHandle, SQLCHAR* serverName,
                             SQLSMALLINT serverNameLen, SQLCHAR* userName,
                             SQLSMALLINT userNameLen, SQLCHAR* authString,
                             SQLSMALLINT authStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLConnect");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLConnect");
    return SQL_ERROR;
  }

  // Call to internal common function for SQLConnect and SQLConnectW
  // in odbc_connection.h.
  rc = google::cloud::odbc_bq_driver::SQLConnectInternal(
      connectionHandle, serverName, serverNameLen, userName, userNameLen,
      authString, authStringLen);

  return rc;
}
//////////////////////////////////////
// Unicode version of SQLConnect.
//////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLConnectW(SQLHDBC connectionHandle, SQLWCHAR* serverName,
                              SQLSMALLINT serverNameLen, SQLWCHAR* userName,
                              SQLSMALLINT userNameLen, SQLWCHAR* authString,
                              SQLSMALLINT authStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLConnectW");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLConnectW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters. All three strings are read
  // in the configured wire encoding, honouring SQL_NTS and explicit lengths.

  // For this API, DriverManager requires serverName or DSN string
  // to be non-empty hence we need to validate it before proceeding further.
  StatusRecordOr<std::string> utf8_server_name;
  if (serverName) {
    utf8_server_name = BqConvertSQLWCHARToString(serverName, serverNameLen);
    if (!utf8_server_name) {
      return utf8_server_name.GetCalculatedReturnCode();
    }
  }
  if (!serverName || utf8_server_name->empty()) {
    auto status =
        StatusRecord{SQLStates::k_HY000(),
                     "serverName or datasource name cannot be null/empty"};
    return status.CalculateReturnCode();
  }
  if (serverNameLen && serverNameLen != SQL_NTS)
    serverNameLen = utf8_server_name->length();

  // User name and Auth strings are optional.
  StatusRecordOr<std::string> utf8_user_name = std::string();
  if (userName) {
    utf8_user_name = BqConvertSQLWCHARToString(userName, userNameLen);
    if (!utf8_user_name) {
      return utf8_user_name.GetCalculatedReturnCode();
    }
    if (userNameLen && userNameLen != SQL_NTS)
      userNameLen = utf8_user_name->length();
  }
  bool const has_user_name = !utf8_user_name->empty();

  StatusRecordOr<std::string> utf8_auth_str = std::string();
  if (authString) {
    utf8_auth_str = BqConvertSQLWCHARToString(authString, authStringLen);
    if (!utf8_auth_str) {
      return utf8_auth_str.GetCalculatedReturnCode();
    }
    if (authStringLen && authStringLen != SQL_NTS)
      authStringLen = utf8_auth_str->length();
  }
  if (has_user_name && utf8_auth_str->empty()) {
    // It is an error to supply a username without a auth string.
    auto status = StatusRecord{
        SQLStates::k_HY000(),
        "authString cannot be empty or null of non-empty userName"};
    return status.CalculateReturnCode();
  }

  // Call to internal common function for SQLConnect and SQLConnectW
  // in odbc_connection.h. The input buffers are never written to.
  if (has_user_name) {
    rc = google::cloud::odbc_bq_driver::SQLConnectInternal(
        connectionHandle, ToSqlChar(utf8_server_name->data()), serverNameLen,
        ToSqlChar(utf8_user_name->data()), userNameLen,
        ToSqlChar(utf8_auth_str->data()), authStringLen);
  } else {
    rc = google::cloud::odbc_bq_driver::SQLConnectInternal(
        connectionHandle, ToSqlChar(utf8_server_name->data()), serverNameLen,
        ToSqlChar(""), 0, ToSqlChar(""), 0);
  }

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns general information about the driver and data source
// associated with a connection
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetinfo-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetInfoA(SQLHDBC connectionHandle, SQLUSMALLINT infoType,
                              SQLPOINTER infoValue,
                              SQLSMALLINT infoValueBufferLen,
                              SQLSMALLINT* infoValueStringLen) {
  return SQLGetInfo(connectionHandle, infoType, infoValue, infoValueBufferLen,
                    infoValueStringLen);
}

SQLRETURN SQL_API SQLGetInfo(SQLHDBC connectionHandle, SQLUSMALLINT infoType,
                             SQLPOINTER infoValue,
                             SQLSMALLINT infoValueBufferLen,
                             SQLSMALLINT* infoValueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetInfo");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLGetInfo");
    return SQL_ERROR;
  }

  // Call to internal common function for SQLGetInfo and SQLGetInfoW
  // in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetInfoInternal(
      connectionHandle, infoType, infoValue, infoValueBufferLen,
      infoValueStringLen);

  return rc;
}
//////////////////////////////////////
// Unicode version of SQLGetInfo.
//////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetInfoW(SQLHDBC connectionHandle, SQLUSMALLINT infoType,
                              SQLPOINTER infoValue,
                              SQLSMALLINT infoValueBufferLen,
                              SQLSMALLINT* infoValueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetInfoW");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLGetInfoW");
    return SQL_ERROR;
  }

  // Call to internal function for SQLGetInfoW in odbc_driver_metadata.h. It
  // converts string values to the wire encoding and bounds the copy by
  // infoValueBufferLen bytes.
  rc = ::google::cloud::odbc_bq_driver::SQLGetInfoWInternal(
      connectionHandle, infoType, infoValue, infoValueBufferLen,
      infoValueStringLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns information about whether a driver supports a specific ODBC function.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetfunctions-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetFunctions(SQLHDBC connectionHandle,
                                  SQLUSMALLINT functionId,
                                  SQLUSMALLINT* supportedFunction) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetFunctions");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLGetFunctions");
    return SQL_ERROR;
  }

  // Call to internal function for SQLGetFunctions in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetFunctionsInternal(
      connectionHandle, functionId, supportedFunction);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns information about data types supported by the data source.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgettypeinfo-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetTypeInfoA(SQLHSTMT statementHandle,
                                  SQLSMALLINT dataType) {
  return SQLGetTypeInfo(statementHandle, dataType);
}

SQLRETURN SQL_API SQLGetTypeInfo(SQLHSTMT statementHandle,
                                 SQLSMALLINT dataType) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetTypeInfo");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLGetTypeInfo");
    return SQL_ERROR;
  }

  // Call to internal common function for SQLGetTypeInfo and SQLGetTypeInfoW
  // in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetTypeInfoInternal(statementHandle,
                                                               dataType);

  return rc;
}

////////////////////////////////////////
// Unicode version of SQLGetTypeInfo.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetTypeInfoW(SQLHSTMT statementHandle,
                                  SQLSMALLINT dataType) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetTypeInfoW");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLGetTypeInfoW");
    return SQL_ERROR;
  }

  // Call to internal common function for SQLGetTypeInfo and SQLGetTypeInfoW
  // in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetTypeInfoInternal(statementHandle,
                                                               dataType);
  // Handle Unicode conversion of  output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Sets connection attributes.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetconnectattr-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLSetConnectAttrA(SQLHDBC connectionHandle,
                                     SQLINTEGER attribute, SQLPOINTER value,
                                     SQLINTEGER valueStringLen) {
  return SQLSetConnectAttr(connectionHandle, attribute, value, valueStringLen);
}

SQLRETURN SQL_API SQLSetConnectAttr(SQLHDBC connectionHandle,
                                    SQLINTEGER attribute, SQLPOINTER value,
                                    SQLINTEGER valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLSetConnectAttr");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLSetConnectAttr");
    return SQL_ERROR;
  }
  // Call to internal common function for SQLSetConnectAttr and
  // SQLSetConnectAttrW in odbc_connection.h.
  rc = ::google::cloud::odbc_bq_driver::SQLSetConnectAttrInternal(
      connectionHandle, attribute, value, valueStringLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLSetConnectAttr.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLSetConnectAttrW(SQLHDBC connectionHandle,
                                     SQLINTEGER attribute, SQLPOINTER value,
                                     SQLINTEGER valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLSetConnectAttrW");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLSetConnectAttrW");
    return SQL_ERROR;
  }
  // If the Attribute value is a character string then we need to do the unicode
  // conversion on the input parameters.
  SQLPOINTER updated_attrib_val;
  SQLINTEGER updated_value_string_len;
  StatusRecordOr<std::string> updated_attrib_status;
  ConnectionAttr conn_attr;
  if (conn_attr.GetAttributeValueType(attribute) ==
      ConnectionValueType::kSqlChr) {
    if (valueStringLen && valueStringLen > 0) {
      updated_attrib_status =
          ConvertSQLPointerToSQLChar(value, valueStringLen / WireWcharSize());
    } else {
      updated_attrib_status = ConvertSQLPointerToSQLChar(value, valueStringLen);
    }
    if (!updated_attrib_status) {
      return updated_attrib_status.GetCalculatedReturnCode();
    }
    updated_attrib_val = (SQLPOINTER)ToSqlChar(updated_attrib_status->data());
    updated_value_string_len = strlen(updated_attrib_status->c_str());
  } else {
    // If we are not dealing with strings no conversions needed.
    updated_attrib_val = value;
    updated_value_string_len = valueStringLen;
  }

  // Handle Unicode conversion of input parameters.

  // Call to internal common function for SQLSetConnectAttr and
  // SQLSetConnectAttrW in odbc_connection.h.
  rc = ::google::cloud::odbc_bq_driver::SQLSetConnectAttrInternal(
      connectionHandle, attribute, updated_attrib_val,
      updated_value_string_len);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the current setting of a connection attribute.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetconnectattr-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetConnectAttrA(SQLHDBC connectionHandle,
                                     SQLINTEGER attribute, SQLPOINTER value,
                                     SQLINTEGER valueBufferLen,
                                     SQLINTEGER* valueStringLen) {
  return SQLGetConnectAttr(connectionHandle, attribute, value, valueBufferLen,
                           valueStringLen);
}

SQLRETURN SQL_API SQLGetConnectAttr(SQLHDBC connectionHandle,
                                    SQLINTEGER attribute, SQLPOINTER value,
                                    SQLINTEGER valueBufferLen,
                                    SQLINTEGER* valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetConnectAttr");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLGetConnectAttr");
    return SQL_ERROR;
  }
  // Call to internal common function for SQLGetConnectAttr and
  // SQLGetConnectAttrW in odbc_connection.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetConnectAttrInternal(
      connectionHandle, attribute, value, valueBufferLen, valueStringLen);

  return rc;
}

////////////////////////////////////////
// Unicode version of SQLGetConnectAttr.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetConnectAttrW(SQLHDBC connectionHandle,
                                     SQLINTEGER attribute, SQLPOINTER value,
                                     SQLINTEGER valueBufferLen,
                                     SQLINTEGER* valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetConnectAttrW");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLGetConnectAttrW");
    return SQL_ERROR;
  }
  // For character strings SQLPOINTER may point to a WCHAR output value.
  // They need to be handled separately.
  ConnectionAttr conn_attr;
  SQLPOINTER updated_attrib_val;
  SQLCHAR attrib_val[kBufferLength] = "Not Set";
  if (conn_attr.GetAttributeValueType(attribute) ==
      ConnectionValueType::kSqlChr) {
    updated_attrib_val = (SQLPOINTER)attrib_val;
  } else {
    updated_attrib_val = value;
  }

  // Handle Unicode conversion of input parameters.
  // Call to internal common function for SQLGetConnectAttr and
  // SQLGetConnectAttrW in odbc_connection.h.
  SQLINTEGER internal_str_len = 0;
  rc = ::google::cloud::odbc_bq_driver::SQLGetConnectAttrInternal(
      connectionHandle, attribute, updated_attrib_val,
      static_cast<SQLINTEGER>(kBufferLength), &internal_str_len);
  // Handle unicode conversion for attribute string values for output
  // parameters.
  // valueBufferLen is in bytes for character attributes.
  if (SQL_SUCCEEDED(rc) && conn_attr.GetAttributeValueType(attribute) ==
                               ConnectionValueType::kSqlChr) {
    auto copied = CopyUtf8BufferToWire(attrib_val, sizeof(attrib_val), value,
                                       WireUnitsForBytes(valueBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    if (valueStringLen) {
      *valueStringLen =
          SaturateLength<SQLINTEGER>(copied->total_units * WireWcharSize());
    }
    if (copied->truncated) {
      rc = WithTruncationWarning<ConnectionHandle>(connectionHandle, rc);
    }
  }

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Sets attributes related to a statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetstmtattr-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLSetStmtAttrA(SQLHSTMT statementHandle,
                                  SQLINTEGER attribute, SQLPOINTER value,
                                  SQLINTEGER valueStringLen) {
  return SQLSetStmtAttr(statementHandle, attribute, value, valueStringLen);
}

SQLRETURN SQL_API SQLSetStmtAttr(SQLHSTMT statementHandle, SQLINTEGER attribute,
                                 SQLPOINTER value, SQLINTEGER valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLSetStmtAttr");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLSetStmtAttr");
    return SQL_ERROR;
  }
  // Call to internal common function for SQLSetStmtAttr and SQLSetStmtAttrW
  // in odbc_statement.h.
  rc = ::google::cloud::odbc_bq_driver::SQLSetStmtAttrInternal(
      statementHandle, attribute, value, valueStringLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLSetStmtAttr.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLSetStmtAttrW(SQLHSTMT statementHandle,
                                  SQLINTEGER attribute, SQLPOINTER value,
                                  SQLINTEGER valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLSetStmtAttrW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLSetStmtAttrW");
    return SQL_ERROR;
  }
  // Handle Unicode conversion of input parameters.

  // Call to internal common function for SQLSetStmtAttr and SQLSetStmtAttrW
  // in odbc_statement.h.
  rc = ::google::cloud::odbc_bq_driver::SQLSetStmtAttrInternal(
      statementHandle, attribute, value, valueStringLen);

  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the current setting of a statement attribute.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetstmtattr-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetStmtAttrA(SQLHSTMT statementHandle,
                                  SQLINTEGER attribute, SQLPOINTER value,
                                  SQLINTEGER valueBufferLen,
                                  SQLINTEGER* valueStringLen) {
  return SQLGetStmtAttr(statementHandle, attribute, value, valueBufferLen,
                        valueStringLen);
}

SQLRETURN SQL_API SQLGetStmtAttr(SQLHSTMT statementHandle, SQLINTEGER attribute,
                                 SQLPOINTER value, SQLINTEGER valueBufferLen,
                                 SQLINTEGER* valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLGetStmtAttr");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLGetStmtAttr");
    return SQL_ERROR;
  }
  // Call to internal common function for SQLGetStmtAttr and SQLGetStmtAttrW
  // in odbc_statement.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetStmtAttrInternal(
      statementHandle, attribute, value, valueBufferLen, valueStringLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLSetStmtAttr.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetStmtAttrW(SQLHSTMT statementHandle,
                                  SQLINTEGER attribute, SQLPOINTER value,
                                  SQLINTEGER valueBufferLen,
                                  SQLINTEGER* valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLGetStmtAttrW");
    return SQL_ERROR;
  }
  // Handle Unicode conversion of input parameters.
  // Call to internal common function for SQLGetStmtAttr and SQLGetStmtAttrW
  // in odbc_statement.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetStmtAttrInternal(
      statementHandle, attribute, value, valueBufferLen, valueStringLen);

  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Sets attributes that govern aspects of environments.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetenvattr-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLSetEnvAttr(SQLHENV environmentHandle, SQLINTEGER attribute,
                                SQLPOINTER value, SQLINTEGER valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLSetEnvAttr");

  HandleLock lock(environmentHandle, SQL_HANDLE_ENV);
  if (!lock.isLocked()) {
    return SQL_ERROR;
  }
  // Call to internal function for SQLSetEnvAttr in odbc_environment.h.
  rc = ::google::cloud::odbc_bq_driver::SQLSetEnvAttrInternal(
      environmentHandle, attribute, value, valueStringLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the current setting of an environment attribute.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetenvattr-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetEnvAttr(SQLHENV environmentHandle, SQLINTEGER attribute,
                                SQLPOINTER value, SQLINTEGER valueBufferLen,
                                SQLINTEGER* valueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetEnvAttr");
  HandleLock lock(environmentHandle, SQL_HANDLE_ENV);
  if (!lock.isLocked()) {
    return SQL_ERROR;
  }

  // Call to internal function for SQLGetEnvAttr in odbc_environment.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetEnvAttrInternal(
      environmentHandle, attribute, value, valueBufferLen, valueStringLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the current setting or value of a single field of a descriptor
// record.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdescfield-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetDescFieldA(SQLHDESC descriptorHandle,
                                   SQLSMALLINT recNumber, SQLSMALLINT fieldId,
                                   SQLPOINTER outDescValue,
                                   SQLINTEGER outDescValueBufferLen,
                                   SQLINTEGER* outDescValueStringLen) {
  return SQLGetDescField(descriptorHandle, recNumber, fieldId, outDescValue,
                         outDescValueBufferLen, outDescValueStringLen);
}

SQLRETURN SQL_API SQLGetDescField(SQLHDESC descriptorHandle,
                                  SQLSMALLINT recNumber, SQLSMALLINT fieldId,
                                  SQLPOINTER outDescValue,
                                  SQLINTEGER outDescValueBufferLen,
                                  SQLINTEGER* outDescValueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLGetDescField");
  HandleLock lock(descriptorHandle, SQL_HANDLE_DESC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DESC, descriptorHandle, "SQLGetDescField");
    return SQL_ERROR;
  }
  rc = google::cloud::odbc_bq_driver::SQLGetDescFieldInternal(
      descriptorHandle, recNumber, fieldId, outDescValue, outDescValueBufferLen,
      outDescValueStringLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLGetDescField.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetDescFieldW(SQLHDESC descriptorHandle,
                                   SQLSMALLINT recNumber, SQLSMALLINT fieldId,
                                   SQLPOINTER outDescValue,
                                   SQLINTEGER outDescValueBufferLen,
                                   SQLINTEGER* outDescValueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLGetDescFieldW");
  HandleLock lock(descriptorHandle, SQL_HANDLE_DESC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DESC, descriptorHandle, "SQLGetDescFieldW");
    return SQL_ERROR;
  }

  SQLCHAR out_desc_val[kBufferLength] = {0};
  SQLINTEGER out_desc_val_string_len = 0;
  bool const is_string = IsFieldIdentifierString(fieldId);

  // Handle Unicode conversion of input parameters.
  // Call to common internal function for SQLGetDescField and SQLGetDescFieldW
  // in odbc_descriptor.h. String values are fetched as UTF-8 into the local
  // buffer, whose size is independent of outDescValueBufferLen.
  rc = google::cloud::odbc_bq_driver::SQLGetDescFieldInternal(
      descriptorHandle, recNumber, fieldId, (SQLPOINTER)out_desc_val,
      is_string && outDescValueBufferLen >= 0 ? kBufferLength
                                              : outDescValueBufferLen,
      &out_desc_val_string_len);

  // Handle Unicode conversion of output parameters. outDescValueBufferLen is
  // in bytes for string fields.
  if (SQL_SUCCEEDED(rc) && is_string) {
    auto copied =
        CopyUtf8BufferToWire(out_desc_val, sizeof(out_desc_val), outDescValue,
                             WireUnitsForBytes(outDescValueBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    out_desc_val_string_len =
        SaturateLength<SQLINTEGER>(copied->total_units * WireWcharSize());
    if (copied->truncated) {
      rc = WithTruncationWarning<DescriptorHandle>(descriptorHandle, rc);
    }
  } else if (SQL_SUCCEEDED(rc) && out_desc_val_string_len > 0) {
    std::memcpy(outDescValue, (SQLPOINTER)out_desc_val,
                std::min<SQLINTEGER>(out_desc_val_string_len, kBufferLength));
  }
  if (outDescValueStringLen) *outDescValueStringLen = out_desc_val_string_len;

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the current settings or values of multiple fields of a descriptor
// record.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdescrec-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetDescRecA(
    SQLHDESC descriptorHandle, SQLSMALLINT recNumber, SQLCHAR* name,
    SQLSMALLINT nameBufferLen, SQLSMALLINT* nameStringLen,
    SQLSMALLINT* descType, SQLSMALLINT* descSubType, SQLLEN* descOctetLen,
    SQLSMALLINT* descPrecision, SQLSMALLINT* descScale, SQLSMALLINT* nullable) {
  return SQLGetDescRec(descriptorHandle, recNumber, name, nameBufferLen,
                       nameStringLen, descType, descSubType, descOctetLen,
                       descPrecision, descScale, nullable);
}

SQLRETURN SQL_API SQLGetDescRec(
    SQLHDESC descriptorHandle, SQLSMALLINT recNumber, SQLCHAR* name,
    SQLSMALLINT nameBufferLen, SQLSMALLINT* nameStringLen,
    SQLSMALLINT* descType, SQLSMALLINT* descSubType, SQLLEN* descOctetLen,
    SQLSMALLINT* descPrecision, SQLSMALLINT* descScale, SQLSMALLINT* nullable) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLGetDescRec");
  HandleLock lock(descriptorHandle, SQL_HANDLE_DESC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DESC, descriptorHandle, "SQLGetDescRec");
    return SQL_ERROR;
  }

  rc = google::cloud::odbc_bq_driver::SQLGetDescRecInternal(
      descriptorHandle, recNumber, name, nameBufferLen, nameStringLen, descType,
      descSubType, descOctetLen, descPrecision, descScale, nullable);

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLGetDescRec.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetDescRecW(
    SQLHDESC descriptorHandle, SQLSMALLINT recNumber, SQLWCHAR* name,
    SQLSMALLINT nameBufferLen, SQLSMALLINT* nameStringLen,
    SQLSMALLINT* descType, SQLSMALLINT* descSubType, SQLLEN* descOctetLen,
    SQLSMALLINT* descPrecision, SQLSMALLINT* descScale, SQLSMALLINT* nullable) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLGetDescRecW");
  HandleLock lock(descriptorHandle, SQL_HANDLE_DESC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DESC, descriptorHandle, "SQLGetDescRecW");
    return SQL_ERROR;
  }
  SQLCHAR name_buffer[kBufferLength] = {0};
  SQLSMALLINT name_string_len = 0;

  // Handle Unicode conversion of input parameters.
  // Call to common internal function for SQLGetDescRec and SQLGetDescRecW
  // in odbc_descriptor.h. The UTF-8 name goes to a local buffer whose size is
  // independent of nameBufferLen.
  rc = google::cloud::odbc_bq_driver::SQLGetDescRecInternal(
      descriptorHandle, recNumber, name_buffer,
      nameBufferLen < 0 ? nameBufferLen : kBufferLength, &name_string_len,
      descType, descSubType, descOctetLen, descPrecision, descScale, nullable);

  // Handle Unicode conversion of output parameters. nameBufferLen and
  // *nameStringLen are in characters.
  if (SQL_SUCCEEDED(rc)) {
    auto copied = CopyUtf8BufferToWire(name_buffer, sizeof(name_buffer), name,
                                       WireUnitsForChars(nameBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    name_string_len = SaturateLength<SQLSMALLINT>(copied->total_units);
    if (copied->truncated) {
      rc = WithTruncationWarning<DescriptorHandle>(descriptorHandle, rc);
    }
  }
  if (nameStringLen) *nameStringLen = name_string_len;

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Sets the value of a single field of a descriptor record.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetdescfield-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLSetDescFieldA(SQLHDESC descriptorHandle,
                                   SQLSMALLINT recNumber,
                                   SQLSMALLINT fieldIdentifier,
                                   SQLPOINTER descValue,
                                   SQLINTEGER descValueBufferLen) {
  return SQLSetDescField(descriptorHandle, recNumber, fieldIdentifier,
                         descValue, descValueBufferLen);
}

SQLRETURN SQL_API SQLSetDescField(SQLHDESC descriptorHandle,
                                  SQLSMALLINT recNumber,
                                  SQLSMALLINT fieldIdentifier,
                                  SQLPOINTER descValue,
                                  SQLINTEGER descValueBufferLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLSetDescField");
  HandleLock lock(descriptorHandle, SQL_HANDLE_DESC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DESC, descriptorHandle, "SQLSetDescField");
    return SQL_ERROR;
  }

  rc = google::cloud::odbc_bq_driver::SQLSetDescFieldInternal(
      descriptorHandle, recNumber, fieldIdentifier, descValue,
      descValueBufferLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLSetDescField.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLSetDescFieldW(SQLHDESC descriptorHandle,
                                   SQLSMALLINT recNumber,
                                   SQLSMALLINT fieldIdentifier,
                                   SQLPOINTER descValue,
                                   SQLINTEGER descValueBufferLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLSetDescFieldW");
  HandleLock lock(descriptorHandle, SQL_HANDLE_DESC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DESC, descriptorHandle, "SQLSetDescFieldW");
    return SQL_ERROR;
  }

  SQLPOINTER updated_desc_val = descValue;
  StatusRecordOr<std::string> updated_desc_status;
  if (IsFieldIdentifierString(fieldIdentifier)) {
    updated_desc_status = ConvertSQLPointerToSQLChar(descValue, SQL_NTS);
    if (!updated_desc_status) {
      return updated_desc_status.GetCalculatedReturnCode();
    }
    updated_desc_val = (SQLPOINTER)ToSqlChar(updated_desc_status->data());
  }

  // Handle Unicode conversion of input parameters.

  // Call to common internal function for SQLSetDescField and SQLSetDescFieldW
  // in odbc_descriptor.h.
  rc = google::cloud::odbc_bq_driver::SQLSetDescFieldInternal(
      descriptorHandle, recNumber, fieldIdentifier, updated_desc_val,
      descValueBufferLen);

  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Sets multiple descriptor fields that affect the data type and buffer bound
// to a column or parameter data.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetdescrec-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLSetDescRec(SQLHDESC descriptorHandle,
                                SQLSMALLINT recNumber, SQLSMALLINT descType,
                                SQLSMALLINT descSubType, SQLLEN descOctetLen,
                                SQLSMALLINT descPrecision,
                                SQLSMALLINT descScale, SQLPOINTER descData,
                                SQLLEN* descOctetLenPtr,
                                SQLLEN* descIndicator) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLSetDescRec");
  HandleLock lock(descriptorHandle, SQL_HANDLE_DESC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DESC, descriptorHandle, "SQLSetDescRec");
    return SQL_ERROR;
  }

  rc = google::cloud::odbc_bq_driver::SQLSetDescRecInternal(
      descriptorHandle, recNumber, descType, descSubType, descOctetLen,
      descPrecision, descScale, descData, descOctetLenPtr, descIndicator);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Copies descriptor information from one descriptor handle to another.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcopydesc-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLCopyDesc(SQLHDESC sourceDescHandle,
                              SQLHDESC targetDescHandle) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLCopyDesc");
  HandleLock lock(sourceDescHandle, SQL_HANDLE_DESC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DESC, sourceDescHandle, "SQLCopyDesc");
    return SQL_ERROR;
  }
  rc = google::cloud::odbc_bq_driver::SQLCopyDescInternal(sourceDescHandle,
                                                          targetDescHandle);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Prepares an SQL string for execution.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlprepare-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLPrepareA(SQLHSTMT statementHandle, SQLCHAR* statementText,
                              SQLINTEGER statementTextLen) {
  return SQLPrepare(statementHandle, statementText, statementTextLen);
}

SQLRETURN SQL_API SQLPrepare(SQLHSTMT statementHandle, SQLCHAR* statementText,
                             SQLINTEGER statementTextLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLPrepare");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLPrepare");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLPrepare and SQLPrepareW
  // in odbc_sql_requests.h.
  rc = google::cloud::odbc_bq_driver::SQLPrepareInternal(
      statementHandle, statementText, statementTextLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLPrepare.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLPrepareW(SQLHSTMT statementHandle, SQLWCHAR* statementText,
                              SQLINTEGER statementTextLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLPrepareW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLPrepareW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_stmt_txt;
  SQLCHAR* sqlchar_stmt_txt = nullptr;
  if (statementText) {
    utf8_stmt_txt = BqConvertSQLWCHARToString(statementText, statementTextLen);
    if (!utf8_stmt_txt) {
      return utf8_stmt_txt.GetCalculatedReturnCode();
    }
    sqlchar_stmt_txt = ToSqlChar(utf8_stmt_txt->data());
    if (statementTextLen && statementTextLen != SQL_NTS)
      statementTextLen = utf8_stmt_txt->length();
  }
  // Call to common internal function for SQLPrepare and SQLPrepareW
  // in odbc_sql_requests.h.
  rc = google::cloud::odbc_bq_driver::SQLPrepareInternal(
      statementHandle, sqlchar_stmt_txt, statementTextLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Binds a buffer to a parameter marker in an SQL statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlbindparameter-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API
SQLBindParameter(SQLHSTMT statementHandle, SQLUSMALLINT parameterNumber,
                 SQLSMALLINT inputOutputType, SQLSMALLINT valueType,
                 SQLSMALLINT parameterType, SQLULEN columnSize,
                 SQLSMALLINT decimalDigits, SQLPOINTER parameterValuePtr,
                 SQLLEN bufferLength, SQLLEN* strLen_or_IndPtr) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLBindParameter");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLBindParameter");
    return SQL_ERROR;
  }

  // Call to internal function for SQLBindParameter in odbc_sql_requests.h.
  rc = google::cloud::odbc_bq_driver::SQLBindParameterInternal(
      statementHandle, parameterNumber, inputOutputType, valueType,
      parameterType, columnSize, decimalDigits, parameterValuePtr, bufferLength,
      strLen_or_IndPtr);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the cursor name associated with a specified statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetcursorname-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetCursorNameA(SQLHSTMT statementHandle,
                                    SQLCHAR* cursorName,
                                    SQLSMALLINT cursorNameBufferLen,
                                    SQLSMALLINT* cursorNameStringLen) {
  return SQLGetCursorName(statementHandle, cursorName, cursorNameBufferLen,
                          cursorNameStringLen);
}

SQLRETURN SQL_API SQLGetCursorName(SQLHSTMT statementHandle,
                                   SQLCHAR* cursorName,
                                   SQLSMALLINT cursorNameBufferLen,
                                   SQLSMALLINT* cursorNameStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLGetCursorName");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLGetCursorName");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLGetCursorName and SQLGetCursorNameW
  // in odbc_sql_requests.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetCursorNameInternal(
      statementHandle, cursorName, cursorNameBufferLen, cursorNameStringLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLGetCursorName.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetCursorNameW(SQLHSTMT statementHandle,
                                    SQLWCHAR* cursorName,
                                    SQLSMALLINT cursorNameBufferLen,
                                    SQLSMALLINT* cursorNameStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLGetCursorNameW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  SQLCHAR cursor_name[kBufferLength] = {0};
  SQLSMALLINT cursor_name_len = 0;

  // Call to common internal function for SQLGetCursorName and SQLGetCursorNameW
  // in odbc_sql_requests.h.
  // The UTF-8 name goes to a local buffer whose size is independent of
  // cursorNameBufferLen.
  rc = ::google::cloud::odbc_bq_driver::SQLGetCursorNameInternal(
      statementHandle, cursor_name,
      cursorNameBufferLen < 0 ? cursorNameBufferLen : kBufferLength,
      &cursor_name_len);

  // Handle Unicode conversion of output parameters. cursorNameBufferLen and
  // *cursorNameStringLen are in characters.
  if (SQL_SUCCEEDED(rc)) {
    auto copied =
        CopyUtf8BufferToWire(cursor_name, sizeof(cursor_name), cursorName,
                             WireUnitsForChars(cursorNameBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    cursor_name_len = SaturateLength<SQLSMALLINT>(copied->total_units);
    if (copied->truncated) {
      rc = WithTruncationWarning<StatementHandle>(statementHandle, rc);
    }
  }
  if (cursorNameStringLen) *cursorNameStringLen = cursor_name_len;

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Associates a cursor name with an active statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetcursorname-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLSetCursorNameA(SQLHSTMT statementHandle,
                                    SQLCHAR* cursorName,
                                    SQLSMALLINT cursorNameLen) {
  return SQLSetCursorName(statementHandle, cursorName, cursorNameLen);
}

SQLRETURN SQL_API SQLSetCursorName(SQLHSTMT statementHandle,
                                   SQLCHAR* cursorName,
                                   SQLSMALLINT cursorNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLSetCursorName");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLSetCursorName");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLSetCursorName and SQLSetCursorNameW
  // in odbc_sql_requests.h.
  rc = ::google::cloud::odbc_bq_driver::SQLSetCursorNameInternal(
      statementHandle, cursorName, cursorNameLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLSetCursorName.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLSetCursorNameW(SQLHSTMT statementHandle,
                                    SQLWCHAR* cursorName,
                                    SQLSMALLINT cursorNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLSetCursorNameW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLSetCursorNameW");
    return SQL_ERROR;
  }
  // Handle Unicode conversion of input parameters.
  if (cursorNameLen <= 0 && cursorNameLen != SQL_NTS) {
    StatusRecord status_record = {SQLStates::k_HY090(),
                                  "Invalid string length"};
    return status_record.CalculateReturnCode();
  }
  SQLCHAR* sqlchar_cur_name = nullptr;
  StatusRecordOr<std::string> utf8_cur_name;
  if (cursorName) {
    utf8_cur_name = BqConvertSQLWCHARToString(cursorName, cursorNameLen);
    if (!utf8_cur_name) {
      return utf8_cur_name.GetCalculatedReturnCode();
    }
    sqlchar_cur_name = ToSqlChar(utf8_cur_name->data());
    if (cursorNameLen && cursorNameLen != SQL_NTS)
      cursorNameLen = utf8_cur_name->length();
  }

  // Call to common internal function for SQLSetCursorName and SQLSetCursorNameW
  // in odbc_sql_requests.h.
  rc = ::google::cloud::odbc_bq_driver::SQLSetCursorNameInternal(
      statementHandle, sqlchar_cur_name, cursorNameLen);

  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Executes a prepared statement, using the current values of the parameter
// marker variables if any parameter markers exist in the statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecute-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLExecute(SQLHSTMT statementHandle) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLExecute");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLExecute");
    return SQL_ERROR;
  }

  // Call to Acquire mutex for statement handle in odbc_lock.h.

  // Call to internal common function for SQLGetInfo and SQLGetInfoW
  // in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLExecuteInternal(statementHandle);

  // Call to Release mutex for statement handle in odbc_lock.h.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Executes a prepared statement, using the current values of the parameter
// marker variables if any parameter markers exist in the statement.
//
// Fastest way to submit a SQL statement for one-time execution
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecdirect-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLExecDirectA(SQLHSTMT statementHandle,
                                 SQLCHAR* statementText,
                                 SQLINTEGER statementTextLen) {
  return SQLExecDirect(statementHandle, statementText, statementTextLen);
}

SQLRETURN SQL_API SQLExecDirect(SQLHSTMT statementHandle,
                                SQLCHAR* statementText,
                                SQLINTEGER statementTextLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLExecDirect");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLExecDirect");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLExecDirect and SQLExecDirectW
  // in odbc_sql_requests.h.
  rc = google::cloud::odbc_bq_driver::SQLExecDirectInternal(
      statementHandle, statementText, statementTextLen);

  return rc;
}

////////////////////////////////////////
// Unicode version of SQLExecDirect.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLExecDirectW(SQLHSTMT statementHandle,
                                 SQLWCHAR* statementText,
                                 SQLINTEGER statementTextLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLExecDirectW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLExecDirectW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_stmt_txt;
  SQLCHAR* sqlchar_stmt_txt = nullptr;
  if (statementText) {
    utf8_stmt_txt = BqConvertSQLWCHARToString(statementText, statementTextLen);
    if (!utf8_stmt_txt) {
      return utf8_stmt_txt.GetCalculatedReturnCode();
    }
    sqlchar_stmt_txt = ToSqlChar(utf8_stmt_txt->data());
    if (statementTextLen && statementTextLen != SQL_NTS)
      statementTextLen = utf8_stmt_txt->length();
  }

  // Call to common internal function for SQLExecDirect and SQLExecDirectW
  // in odbc_sql_requests.h.
  // Handle Unicode conversion of output parameters.
  rc = google::cloud::odbc_bq_driver::SQLExecDirectInternal(
      statementHandle, sqlchar_stmt_txt, statementTextLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the SQL string as modified by the driver. Does not execute the SQL
// statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlnativesql-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLNativeSqlA(SQLHDBC connectionHandle,
                                SQLCHAR* inStatementText,
                                SQLINTEGER inStatementTextLen,
                                SQLCHAR* outStatementText,
                                SQLINTEGER outStatementTextBufferLen,
                                SQLINTEGER* outStatementTextLen) {
  return SQLNativeSql(connectionHandle, inStatementText, inStatementTextLen,
                      outStatementText, outStatementTextBufferLen,
                      outStatementTextLen);
}

SQLRETURN SQL_API SQLNativeSql(SQLHDBC connectionHandle,
                               SQLCHAR* inStatementText,
                               SQLINTEGER inStatementTextLen,
                               SQLCHAR* outStatementText,
                               SQLINTEGER outStatementTextBufferLen,
                               SQLINTEGER* outStatementTextLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLNativeSql");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLNativeSql");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLNativeSql and SQLNativeSqlW
  // in odbc_sql_results.h.
  rc = ::google::cloud::odbc_bq_driver::SQLNativeSqlInternal(
      connectionHandle, inStatementText, inStatementTextLen, outStatementText,
      outStatementTextBufferLen, outStatementTextLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLNativeSql.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLNativeSqlW(SQLHDBC connectionHandle,
                                SQLWCHAR* inStatementText,
                                SQLINTEGER inStatementTextLen,
                                SQLWCHAR* outStatementText,
                                SQLINTEGER outStatementTextBufferLen,
                                SQLINTEGER* outStatementTextLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLNativeSqlW");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLNativeSqlW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_in_stmt_txt;
  SQLCHAR* sqlchar_in_stmt_txt = nullptr;
  if (inStatementText) {
    utf8_in_stmt_txt =
        BqConvertSQLWCHARToString(inStatementText, inStatementTextLen);
    if (!utf8_in_stmt_txt) {
      return utf8_in_stmt_txt.GetCalculatedReturnCode();
    }
    sqlchar_in_stmt_txt = ToSqlChar(utf8_in_stmt_txt->data());
    if (inStatementTextLen && inStatementTextLen != SQL_NTS)
      inStatementTextLen = utf8_in_stmt_txt->length();
  }

  // Call to common internal function for SQLNativeSql and SQLNativeSqlW
  // in odbc_sql_requests.h. The output equals the input statement, so the local
  // UTF-8 buffer is sized to hold all of it regardless of the caller's buffer.
  std::vector<SQLCHAR> out_statement_text(
      (utf8_in_stmt_txt ? utf8_in_stmt_txt->size() : 0) + 1, 0);
  SQLINTEGER out_statement_text_len = 0;
  rc = ::google::cloud::odbc_bq_driver::SQLNativeSqlInternal(
      connectionHandle, sqlchar_in_stmt_txt, inStatementTextLen,
      out_statement_text.data(),
      outStatementTextBufferLen < 0
          ? outStatementTextBufferLen
          : SaturateLength<SQLINTEGER>(out_statement_text.size()),
      &out_statement_text_len);

  // Handle Unicode conversion of output parameters. outStatementTextBufferLen
  // and *outStatementTextLen are in characters.
  if (SQL_SUCCEEDED(rc)) {
    auto copied = CopyUtf8BufferToWire(
        out_statement_text.data(), out_statement_text.size(), outStatementText,
        WireUnitsForChars(outStatementTextBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    if (outStatementTextLen) {
      *outStatementTextLen = SaturateLength<SQLINTEGER>(copied->total_units);
    }
    if (copied->truncated) {
      rc = WithTruncationWarning<ConnectionHandle>(connectionHandle, rc);
    }
  }

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the number of parameters in an SQL statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlnumparams-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLNumParams(SQLHSTMT statementHandle,
                               SQLSMALLINT* paramCount) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLNumParams");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLNumParams");
    return SQL_ERROR;
  }

  // Call to internal function for SQLNumParams in odbc_sql_requests.h.
  rc = google::cloud::odbc_bq_driver::SQLNumParamsInternal(statementHandle,
                                                           paramCount);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Used together with SQLPutData to supply parameter data at statement execution
// time, and with SQLGetData to retrieve streamed output parameter data.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlparamdata-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLParamData(SQLHSTMT statementHandle,
                               SQLPOINTER* paramOrTargetValue) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLParamData");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLParamData");
    return SQL_ERROR;
  }

  // Call to internal function for SQLParamData in odbc_sql_requests.h.
  rc = google::cloud::odbc_bq_driver::SQLParamDataInternal(statementHandle,
                                                           paramOrTargetValue);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Allows an application to send data for a parameter or column to the driver at
// statement execution time.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlputdata-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLPutData(SQLHSTMT statementHandle, SQLPOINTER paramData,
                             SQLLEN paramDataLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLPutData");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLPutData");
    return SQL_ERROR;
  }

  // Call to internal function for SQLPutData in odbc_sql_requests.h.
  rc = ::google::cloud::odbc_bq_driver::SQLPutDataInternal(
      statementHandle, paramData, paramDataLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the description of a parameter marker associated with a
// prepared SQL statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldescribeparam-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLDescribeParam(SQLHSTMT statementHandle,
                                   SQLUSMALLINT paramNumber,
                                   SQLSMALLINT* paramSqlType,
                                   SQLULEN* paramSize, SQLSMALLINT* paramScale,
                                   SQLSMALLINT* paramNullable) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLDescribeParam");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLDescribeParam");
    return SQL_ERROR;
  }

  // Call to internal function for SQLDescribeParam in odbc_sql_requests.h.
  rc = ::google::cloud::odbc_bq_driver::SQLDescribeParamInternal(
      statementHandle, paramNumber, paramSqlType, paramSize, paramScale,
      paramNullable);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Retrieves data for a single column in the result set.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdata-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetData(SQLHSTMT statementHandle,
                             SQLUSMALLINT columnNumber, SQLSMALLINT targetCType,
                             SQLPOINTER targetValue,
                             SQLLEN targetValueBufferLen,
                             SQLLEN* targetValueStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLGetData");
    return SQL_ERROR;
  }

  // Call to internal function for SQLGetData in odbc_sql_results.h.
  rc = ::google::cloud::odbc_bq_driver::SQLGetDataInternal(
      statementHandle, columnNumber, targetCType, targetValue,
      targetValueBufferLen, targetValueStringLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the number of columns in a result set.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlnumresultcols-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLNumResultCols(SQLHSTMT statementHandle,
                                   SQLSMALLINT* columnCount) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLNumResultCols");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLNumResultCols");
    return SQL_ERROR;
  }
  // Call to internal function for SQLNumResultCols in odbc_sql_results.h.
  rc = google::cloud::odbc_bq_driver::SQLNumResultColsInternal(statementHandle,
                                                               columnCount);

  return rc;
}
////////////////////////////////////////////////////////////////////////////////////////
// Fetches the next rowset of data from the result set and returns data for
// all bound columns.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfetch-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLFetch(SQLHSTMT statementHandle) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLFetch");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLFetch");
    return SQL_ERROR;
  }
  // Call to internal common function for SQLGetInfo and SQLGetInfoW
  // in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLFetchInternal(statementHandle);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Fetches the specified rowset of data from the result set and returns data for
// all bound columns. Rowsets can be specified at an absolute or relative
// position or by bookmark.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlextendedfetch-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLExtendedFetch(SQLHSTMT statementHandletmt,
                                   SQLUSMALLINT fetchOrientation,
                                   SQLLEN fetchOffset, SQLULEN* rowCount,
                                   SQLUSMALLINT* rowStatusArray) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandletmt, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandletmt, "SQLExtendedFetch");
    return SQL_ERROR;
  }
  // Call to Trace function entry in odbc_trace.h if tracing is enabled.

  // Call to internal function for SQLExtendedFetch in odbc_sql_results.h.

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns descriptor information for a column in a result set. Descriptor
// information is returned as a character string, a descriptor-dependent value,
// or an integer value.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcolattribute-function
////////////////////////////////////////////////////////////////////////////////////////
// TODO(b/369324094): Unicode SQLColAttribute API not linking with ODBC library
// on Windows x86
#if !defined(_WIN32) || defined(_WIN64)
SQLRETURN SQL_API SQLColAttributeA(SQLHSTMT statementHandle,
                                   SQLUSMALLINT columnNumber,
                                   SQLUSMALLINT fieldIdentifier,
                                   SQLPOINTER characterAttribute,
                                   SQLSMALLINT characterAttributeBufferLen,
                                   SQLSMALLINT* characterAttributeStringLen,
                                   SQLLEN* numericAttribute) {
  return SQLColAttribute(statementHandle, columnNumber, fieldIdentifier,
                         characterAttribute, characterAttributeBufferLen,
                         characterAttributeStringLen, numericAttribute);
}
#endif /* WIN32 || WIN64 */

SQLRETURN SQL_API SQLColAttribute(SQLHSTMT statementHandle,
                                  SQLUSMALLINT columnNumber,
                                  SQLUSMALLINT fieldIdentifier,
                                  SQLPOINTER characterAttribute,
                                  SQLSMALLINT characterAttributeBufferLen,
                                  SQLSMALLINT* characterAttributeStringLen,
                                  SQLLEN* numericAttribute) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLColAttribute");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLColAttribute");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLColAttribute and SQLColAttributeW
  // in odbc_sql_results.h.
  rc = ::google::cloud::odbc_bq_driver::SQLColAttributeInternal(
      statementHandle, columnNumber, fieldIdentifier, characterAttribute,
      characterAttributeBufferLen, characterAttributeStringLen,
      numericAttribute);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLColAttribute.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLColAttributeW(SQLHSTMT statementHandle,
                                   SQLUSMALLINT columnNumber,
                                   SQLUSMALLINT fieldIdentifier,
                                   SQLPOINTER characterAttribute,
                                   SQLSMALLINT characterAttributeBufferLen,
                                   SQLSMALLINT* characterAttributeStringLen,
                                   SQLLEN* numericAttribute) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLSMALLINT character_attribute_string_len = 0;
  InitializeTracing("SQLColAttributeW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLColAttributeW");
    return SQL_ERROR;
  }

  // String attributes are fetched as UTF-8 into a local buffer whose size is
  // independent of characterAttributeBufferLen. Non-string attributes are
  // returned through numericAttribute and leave characterAttribute untouched.
  SQLCHAR character_attrib_val[kBufferLength] = {0};
  bool const is_string = IsFieldIdentifierString(fieldIdentifier);

  // Handle Unicode conversion of input parameters.
  // Call to common internal function for SQLColAttribute and SQLColAttributeW
  // in odbc_sql_results.h.
  rc = ::google::cloud::odbc_bq_driver::SQLColAttributeInternal(
      statementHandle, columnNumber, fieldIdentifier,
      (SQLPOINTER)character_attrib_val,
      is_string && characterAttributeBufferLen >= 0
          ? static_cast<SQLSMALLINT>(kBufferLength)
          : characterAttributeBufferLen,
      &character_attribute_string_len, numericAttribute);

  // Handle Unicode conversion of output parameters.
  // characterAttributeBufferLen is in bytes.
  if (SQL_SUCCEEDED(rc) && is_string) {
    auto copied = CopyUtf8BufferToWire(
        character_attrib_val, sizeof(character_attrib_val), characterAttribute,
        WireUnitsForBytes(characterAttributeBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    // In bytes, per the ODBC spec.
    character_attribute_string_len =
        SaturateLength<SQLSMALLINT>(copied->total_units * WireWcharSize());
    if (copied->truncated) {
      rc = WithTruncationWarning<StatementHandle>(statementHandle, rc);
    }
  }
  if (characterAttributeStringLen) {
    *characterAttributeStringLen = character_attribute_string_len;
  }

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Deprecated and Replaced by SQLColAttribute in ODBC 3.0.
// Please see the definition for that function.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcolattributes-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLColAttributesA(SQLHSTMT statementHandle,
                                    SQLUSMALLINT columnNumber,
                                    SQLUSMALLINT fieldIdentifier,
                                    SQLPOINTER characterAttribute,
                                    SQLSMALLINT characterAttributeBufferLen,
                                    SQLSMALLINT* characterAttributeStringLen,
                                    SQLLEN* numericAttribute) {
  return SQLColAttributes(statementHandle, columnNumber, fieldIdentifier,
                          characterAttribute, characterAttributeBufferLen,
                          characterAttributeStringLen, numericAttribute);
}

SQLRETURN SQL_API SQLColAttributes(SQLHSTMT statementHandle,
                                   SQLUSMALLINT columnNumber,
                                   SQLUSMALLINT fieldIdentifier,
                                   SQLPOINTER characterAttribute,
                                   SQLSMALLINT characterAttributeBufferLen,
                                   SQLSMALLINT* characterAttributeStringLen,
                                   SQLLEN* numericAttribute) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLColAttributes");
    return SQL_ERROR;
  }
  // Call to Trace function entry in odbc_trace.h if tracing is enabled.

  // Call to common internal function for SQLColAttribute and SQLColAttributeW
  // in odbc_sql_results.h.

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLColAttributes.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLColAttributesW(SQLHSTMT statementHandle,
                                    SQLUSMALLINT columnNumber,
                                    SQLUSMALLINT fieldIdentifier,
                                    SQLPOINTER characterAttribute,
                                    SQLSMALLINT characterAttributeBufferLen,
                                    SQLSMALLINT* characterAttributeStringLen,
                                    SQLLEN* numericAttribute) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLCHAR character_attribute_buffer[kBufferLength] = {0};
  SQLSMALLINT character_attribute_buffer_len = 0;
  InitializeTracing("SQLColAttributesW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLColAttributesW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  // Call to common internal function for SQLColAttribute and SQLColAttributeW
  // in odbc_sql_results.h.
  // Handle Unicode conversion of output parameters.
  // characterAttributeBufferLen is in bytes.
  if (SQL_SUCCEEDED(rc) && character_attribute_buffer_len > 0) {
    auto copied = CopyUtf8BufferToWire(
        character_attribute_buffer, sizeof(character_attribute_buffer),
        characterAttribute, WireUnitsForBytes(characterAttributeBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
  }
  if (characterAttributeStringLen)
    *characterAttributeStringLen = character_attribute_buffer_len;

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the result descriptor information for one column in the result set.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldescribecol-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLDescribeColA(
    SQLHSTMT statementHandle, SQLUSMALLINT columnNumber, SQLCHAR* columnName,
    SQLSMALLINT columnNameBufferLen, SQLSMALLINT* columnNameLe,
    SQLSMALLINT* columnSQLdataType, SQLULEN* columnSize,
    SQLSMALLINT* decimalDigits, SQLSMALLINT* columnNullable) {
  return SQLDescribeCol(statementHandle, columnNumber, columnName,
                        columnNameBufferLen, columnNameLe, columnSQLdataType,
                        columnSize, decimalDigits, columnNullable);
}

SQLRETURN SQL_API SQLDescribeCol(
    SQLHSTMT statementHandle, SQLUSMALLINT columnNumber, SQLCHAR* columnName,
    SQLSMALLINT columnNameBufferLen, SQLSMALLINT* columnNameLe,
    SQLSMALLINT* columnSQLdataType, SQLULEN* columnSize,
    SQLSMALLINT* decimalDigits, SQLSMALLINT* columnNullable) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLDescribeCol");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLDescribeCol");
    return SQL_ERROR;
  }
  // Call to common internal function for SQLDescribeCol and SQLDescribeColW
  // in odbc_sql_results.h.
  rc = ::google::cloud::odbc_bq_driver::SQLDescribeColInternal(
      statementHandle, columnNumber, columnName, columnNameBufferLen,
      columnNameLe, columnSQLdataType, columnSize, decimalDigits,
      columnNullable);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLDescribeCol.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLDescribeColW(
    SQLHSTMT statementHandle, SQLUSMALLINT columnNumber, SQLWCHAR* columnName,
    SQLSMALLINT columnNameBufferLen, SQLSMALLINT* columnNameLen,
    SQLSMALLINT* columnSQLdataType, SQLULEN* columnSize,
    SQLSMALLINT* decimalDigits, SQLSMALLINT* columnNullable) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLDescribeColW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLDescribeColW");
    return SQL_ERROR;
  }

  SQLCHAR column_name_buffer[kBufferLength] = {0};
  SQLSMALLINT column_name_string_len = 0;

  // Handle Unicode conversion of input parameters.

  // Call to common internal function for SQLDescribeCol and SQLDescribeColW
  // in odbc_sql_results.h.
  // The UTF-8 name goes to a local buffer whose size is independent of
  // columnNameBufferLen.
  rc = ::google::cloud::odbc_bq_driver::SQLDescribeColInternal(
      statementHandle, columnNumber, column_name_buffer,
      columnNameBufferLen < 0 ? columnNameBufferLen : kBufferLength,
      &column_name_string_len, columnSQLdataType, columnSize, decimalDigits,
      columnNullable);

  // Handle Unicode conversion of output parameters. columnNameBufferLen and
  // *columnNameLen are in characters.
  if (SQL_SUCCEEDED(rc)) {
    auto copied = CopyUtf8BufferToWire(column_name_buffer,
                                       sizeof(column_name_buffer), columnName,
                                       WireUnitsForChars(columnNameBufferLen));
    if (!copied) {
      return copied.GetCalculatedReturnCode();
    }
    column_name_string_len = SaturateLength<SQLSMALLINT>(copied->total_units);
    if (copied->truncated) {
      rc = WithTruncationWarning<StatementHandle>(statementHandle, rc);
    }
  }

  if (columnNameLen) {
    *columnNameLen = column_name_string_len;
  }

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Binds application data buffers to columns in the result set.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlbindcol-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLBindCol(SQLHSTMT statementHandle,
                             SQLUSMALLINT columnNumber, SQLSMALLINT targetCType,
                             SQLPOINTER targetValuePtr,
                             SQLLEN targetValueBufferLen,
                             SQLLEN* targetValueStrLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLBindCol");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLBindCol");
    return SQL_ERROR;
  }

  // Call to internal common function for SQLGetInfo and SQLGetInfoW
  // in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLBindColInternal(
      statementHandle, columnNumber, targetCType, targetValuePtr,
      targetValueBufferLen, targetValueStrLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the number of rows affected by an UPDATE, INSERT, or DELETE
// statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlrowcount-function.
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLRowCount(SQLHSTMT statementHandle, SQLLEN* rowCount) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLRowCount");

  // Call to Acquire mutex for statement handle in odbc_lock.h.
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLRowCount");
    return SQL_ERROR;
  }
  // Call to internal function for SQLRowCount in odbc_sql_results.h.
  rc = ::google::cloud::odbc_bq_driver::SQLRowCountInternal(statementHandle,
                                                            rowCount);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Fetches the specified rowset of data from the result set and returns
// data for all bound columns.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfetchscroll-function.
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLFetchScroll(SQLHSTMT statementHandle,
                                 SQLSMALLINT fetchOrientation,
                                 SQLLEN fetchOffset) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLFetchScroll");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLFetchScroll");
    return SQL_ERROR;
  }

  // Call to internal function for SQLFetchScroll in odbc_sql_results.h.
  rc = ::google::cloud::odbc_bq_driver::SQLFetchScrollInternal(
      statementHandle, fetchOrientation, fetchOffset);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Determines whether more results are available on a statement containing
// SELECT, UPDATE, INSERT, or DELETE statements and, if so, initializes
// processing for those results.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlmoreresults-function
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLMoreResults(SQLHSTMT statementHandle) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLMoreResults");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLMoreResults");
    return SQL_ERROR;
  }

  // Call to internal common function for SQLGetInfo and SQLGetInfoW
  // in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLMoreResultsInternal(statementHandle);

  // Call to Release mutex for statement handle in odbc_lock.h.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the current value of a field of a record of the diagnostic data
// structure (associated with a specified handle) that contains error, warning,
// and status information.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdiagfield-function.
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetDiagFieldA(SQLSMALLINT handleType, SQLHANDLE handle,
                                   SQLSMALLINT recNumber,
                                   SQLSMALLINT diagIdentifier,
                                   SQLPOINTER diagInfo,
                                   SQLSMALLINT diagInfoBufferLen,
                                   SQLSMALLINT* diagInfoStringLen) {
  return SQLGetDiagField(handleType, handle, recNumber, diagIdentifier,
                         diagInfo, diagInfoBufferLen, diagInfoStringLen);
}

SQLRETURN SQL_API SQLGetDiagField(SQLSMALLINT handleType, SQLHANDLE handle,
                                  SQLSMALLINT recNumber,
                                  SQLSMALLINT diagIdentifier,
                                  SQLPOINTER diagInfo,
                                  SQLSMALLINT diagInfoBufferLen,
                                  SQLSMALLINT* diagInfoStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetDiagField");

  HandleLock lock(handle, handleType);
  if (!lock.isLocked()) {
    HandleLockError(handleType, handle, "SQLGetDiagField");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLGetDiagField and SQLGetDiagFieldW
  // in odbc_diagnostics.h.
  rc = google::cloud::odbc_bq_driver::SQLGetDiagFieldInternal(
      handleType, handle, recNumber, diagIdentifier, diagInfo,
      diagInfoBufferLen, diagInfoStringLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLGetDiagField.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetDiagFieldW(SQLSMALLINT handleType, SQLHANDLE handle,
                                   SQLSMALLINT recNumber,
                                   SQLSMALLINT diagIdentifier,
                                   SQLPOINTER diagInfo,
                                   SQLSMALLINT diagInfoBufferLen,
                                   SQLSMALLINT* diagInfoStringLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetDiagFieldW");

  HandleLock lock(handle, handleType);
  if (!lock.isLocked()) {
    HandleLockError(handleType, handle, "SQLGetDiagFieldW");
    return SQL_ERROR;
  }
  // Call to the internal Unicode variant in odbc_diagnostics.h. It converts
  // string fields to the wire encoding and bounds the copy by
  // diagInfoBufferLen bytes.
  rc = google::cloud::odbc_bq_driver::SQLGetDiagFieldWInternal(
      handleType, handle, recNumber, diagIdentifier, diagInfo,
      diagInfoBufferLen, diagInfoStringLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the current values of multiple fields of a diagnostic record that
// contains error, warning, and status information.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdiagrec-function.
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLGetDiagRecA(SQLSMALLINT handleType, SQLHANDLE handle,
                                 SQLSMALLINT recNumber, SQLCHAR* sqlState,
                                 SQLINTEGER* nativeError, SQLCHAR* messageText,
                                 SQLSMALLINT messageTextBufferLen,
                                 SQLSMALLINT* messageTextLen) {
  return SQLGetDiagRec(handleType, handle, recNumber, sqlState, nativeError,
                       messageText, messageTextBufferLen, messageTextLen);
}

SQLRETURN SQL_API SQLGetDiagRec(SQLSMALLINT handleType, SQLHANDLE handle,
                                SQLSMALLINT recNumber, SQLCHAR* sqlState,
                                SQLINTEGER* nativeError, SQLCHAR* messageText,
                                SQLSMALLINT messageTextBufferLen,
                                SQLSMALLINT* messageTextLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLGetDiagRec");

  HandleLock lock(handle, handleType);
  if (!lock.isLocked()) {
    HandleLockError(handleType, handle, "SQLGetDiagRec");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLGetDiagRec and SQLGetDiagRecW
  // in odbc_diagnostics.h.
  rc = google::cloud::odbc_bq_driver::SQLGetDiagRecInternal(
      handleType, handle, recNumber, sqlState, nativeError, messageText,
      messageTextBufferLen, messageTextLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLGetDiagRec.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLGetDiagRecW(SQLSMALLINT handleType, SQLHANDLE handle,
                                 SQLSMALLINT recNumber, SQLWCHAR* sqlState,
                                 SQLINTEGER* nativeError, SQLWCHAR* messageText,
                                 SQLSMALLINT messageTextBufferLen,
                                 SQLSMALLINT* messageTextLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLGetDiagRecW");

  HandleLock lock(handle, handleType);
  if (!lock.isLocked()) {
    HandleLockError(handleType, handle, "SQLGetDiagRecW");
    return SQL_ERROR;
  }
  // Call to the internal Unicode variant in odbc_diagnostics.h. It writes
  // SQLSTATE as exactly 6 wire code units and bounds the message by
  // messageTextBufferLen wire code units.
  rc = google::cloud::odbc_bq_driver::SQLGetDiagRecWInternal(
      handleType, handle, recNumber, sqlState, nativeError, messageText,
      messageTextBufferLen, messageTextLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the list of column names in specified tables.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcolumns-function.
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLColumnsA(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                              SQLSMALLINT catalogNameLen, SQLCHAR* schemaName,
                              SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
                              SQLSMALLINT tableNameLen, SQLCHAR* columnName,
                              SQLSMALLINT columnNameLen) {
  return SQLColumns(statementHandle, catalogName, catalogNameLen, schemaName,
                    schemaNameLen, tableName, tableNameLen, columnName,
                    columnNameLen);
}

SQLRETURN SQL_API SQLColumns(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                             SQLSMALLINT catalogNameLen, SQLCHAR* schemaName,
                             SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
                             SQLSMALLINT tableNameLen, SQLCHAR* columnName,
                             SQLSMALLINT columnNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLColumns");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLColumns");
    return SQL_ERROR;
  }
  // Call to common internal function for SQLColumns and SQLColumnsW
  // in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLColumnsInternal(
      statementHandle, catalogName, catalogNameLen, schemaName, schemaNameLen,
      tableName, tableNameLen, columnName, columnNameLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLColumns.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLColumnsW(SQLHSTMT statementHandle, SQLWCHAR* catalogName,
                              SQLSMALLINT catalogNameLen, SQLWCHAR* schemaName,
                              SQLSMALLINT schemaNameLen, SQLWCHAR* tableName,
                              SQLSMALLINT tableNameLen, SQLWCHAR* columnName,
                              SQLSMALLINT columnNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLColumnsW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLColumnsW");
    return SQL_ERROR;
  }
  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_catalog_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_catalog_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_table_name;
  SQLCHAR* sqlchar_table_name = nullptr;
  if (tableName) {
    utf8_table_name = BqConvertSQLWCHARToString(tableName, tableNameLen);
    if (!utf8_table_name) {
      return utf8_table_name.GetCalculatedReturnCode();
    }
    sqlchar_table_name = ToSqlChar(utf8_table_name->data());
    if (tableNameLen && tableNameLen != SQL_NTS)
      tableNameLen = utf8_table_name->length();
  }

  StatusRecordOr<std::string> utf8_col_name;
  SQLCHAR* sqlchar_column_name = nullptr;
  if (columnName) {
    utf8_col_name = BqConvertSQLWCHARToString(columnName, columnNameLen);
    if (!utf8_col_name) {
      return utf8_col_name.GetCalculatedReturnCode();
    }
    sqlchar_column_name = ToSqlChar(utf8_col_name->data());
    if (columnNameLen && columnNameLen != SQL_NTS)
      columnNameLen = utf8_col_name->length();
  }

  // Call to common internal function for SQLColumns and SQLColumnsW
  // in odbc_driver_metadata.h.
  // Handle Unicode conversion of output parameters.
  rc = google::cloud::odbc_bq_driver::SQLColumnsInternal(
      statementHandle, sqlchar_catalog_name, catalogNameLen,
      sqlchar_schema_name, schemaNameLen, sqlchar_table_name, tableNameLen,
      sqlchar_column_name, columnNameLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the list of table, catalog, or schema names, and table types,
// stored in a specific data source.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqltables-function.
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLTablesA(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                             SQLSMALLINT catalogNameLen, SQLCHAR* schemaName,
                             SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
                             SQLSMALLINT tableNameLen, SQLCHAR* tableType,
                             SQLSMALLINT tableTypeLen) {
  return SQLTables(statementHandle, catalogName, catalogNameLen, schemaName,
                   schemaNameLen, tableName, tableNameLen, tableType,
                   tableTypeLen);
}

SQLRETURN SQL_API SQLTables(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                            SQLSMALLINT catalogNameLen, SQLCHAR* schemaName,
                            SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
                            SQLSMALLINT tableNameLen, SQLCHAR* tableType,
                            SQLSMALLINT tableTypeLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLTables");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLTables");
    return SQL_ERROR;
  }
  // Call to common internal function for SQLTables and SQLTablesW
  // in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLTablesInternal(
      statementHandle, catalogName, catalogNameLen, schemaName, schemaNameLen,
      tableName, tableNameLen, tableType, tableTypeLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLTables.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLTablesW(SQLHSTMT statementHandle, SQLWCHAR* catalogName,
                             SQLSMALLINT catalogNameLen, SQLWCHAR* schemaName,
                             SQLSMALLINT schemaNameLen, SQLWCHAR* tableName,
                             SQLSMALLINT tableNameLen, SQLWCHAR* tableType,
                             SQLSMALLINT tableTypeLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLTablesW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLTablesW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_category_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_category_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (schemaNameLen && schemaNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_table_name;
  SQLCHAR* sqlchar_table_name = nullptr;
  if (tableName) {
    utf8_table_name = BqConvertSQLWCHARToString(tableName, tableNameLen);
    if (!utf8_table_name) {
      return utf8_table_name.GetCalculatedReturnCode();
    }
    sqlchar_table_name = ToSqlChar(utf8_table_name->data());
    if (tableNameLen && tableNameLen != SQL_NTS)
      tableNameLen = utf8_table_name->length();
  }

  StatusRecordOr<std::string> utf8_table_type;
  SQLCHAR* sqlchar_table_type = nullptr;
  if (tableType) {
    utf8_table_type = BqConvertSQLWCHARToString(tableType, tableTypeLen);
    if (!utf8_table_type) {
      return utf8_table_type.GetCalculatedReturnCode();
    }
    sqlchar_table_type = ToSqlChar(utf8_table_type->data());

    if (tableTypeLen && tableTypeLen != SQL_NTS)
      tableTypeLen = utf8_table_type->length();
  }

  // Call to common internal function for SQLTables and SQLTablesW
  // in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLTablesInternal(
      statementHandle, sqlchar_category_name, catalogNameLen,
      sqlchar_schema_name, schemaNameLen, sqlchar_table_name, tableNameLen,
      sqlchar_table_type, tableTypeLen);
  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////
// Returns the column names that make up the primary key for a table.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlprimarykeys-function.
////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLPrimaryKeysA(SQLHSTMT statementHandle,
                                  SQLCHAR* catalogName,
                                  SQLSMALLINT catalogNameLen,
                                  SQLCHAR* schemaName,
                                  SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
                                  SQLSMALLINT tableNameLen) {
  return SQLPrimaryKeys(statementHandle, catalogName, catalogNameLen,
                        schemaName, schemaNameLen, tableName, tableNameLen);
}

SQLRETURN SQL_API SQLPrimaryKeys(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                                 SQLSMALLINT catalogNameLen,
                                 SQLCHAR* schemaName, SQLSMALLINT schemaNameLen,
                                 SQLCHAR* tableName, SQLSMALLINT tableNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLPrimaryKeys");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLPrimaryKeys");
    return SQL_ERROR;
  }
  // Call to common internal function for SQLPrimaryKeys and SQLPrimaryKeysW
  // in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLPrimaryKeysInternal(
      statementHandle, catalogName, catalogNameLen, schemaName, schemaNameLen,
      tableName, tableNameLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLPrimaryKeys.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLPrimaryKeysW(
    SQLHSTMT statementHandle, SQLWCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLWCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLWCHAR* tableName,
    SQLSMALLINT tableNameLen) {
  SQLRETURN rc = SQL_SUCCESS;

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLPrimaryKeysW");
    return SQL_ERROR;
  }
  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_category_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_category_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (schemaNameLen && schemaNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_table_name;
  SQLCHAR* sqlchar_table_name = nullptr;
  if (tableName) {
    utf8_table_name = BqConvertSQLWCHARToString(tableName, tableNameLen);
    if (!utf8_table_name) {
      return utf8_table_name.GetCalculatedReturnCode();
    }
    sqlchar_table_name = ToSqlChar(utf8_table_name->data());
    if (tableNameLen && tableNameLen != SQL_NTS)
      tableNameLen = utf8_table_name->length();
  }

  // Call to common internal function for SQLPrimaryKeys and SQLPrimaryKeysW
  // in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLPrimaryKeysInternal(
      statementHandle, sqlchar_category_name, catalogNameLen,
      sqlchar_schema_name, schemaNameLen, sqlchar_table_name, tableNameLen);

  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Returns the list of input and output parameters, as well as the columns that
// make up the result set for the specified procedures.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlprocedurecolumns-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLProcedureColumnsA(
    SQLHSTMT statementHandle, SQLCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLCHAR* procName,
    SQLSMALLINT procNameLen, SQLCHAR* columnName, SQLSMALLINT columnNameLen) {
  return SQLProcedureColumns(statementHandle, catalogName, catalogNameLen,
                             schemaName, schemaNameLen, procName, procNameLen,
                             columnName, columnNameLen);
}

SQLRETURN SQL_API SQLProcedureColumns(
    SQLHSTMT statementHandle, SQLCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLCHAR* procName,
    SQLSMALLINT procNameLen, SQLCHAR* columnName, SQLSMALLINT columnNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLProcedureColumns");

  // Call to Acquire mutex for statement handle in odbc_lock.h.
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLProcedureColumns");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLProcedureColumns and
  // SQLProcedureColumnsW in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLProcedureColumnsInternal(
      statementHandle, catalogName, catalogNameLen, schemaName, schemaNameLen,
      procName, procNameLen, columnName, columnNameLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLProcedureColumns.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLProcedureColumnsW(
    SQLHSTMT statementHandle, SQLWCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLWCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLWCHAR* procName,
    SQLSMALLINT procNameLen, SQLWCHAR* columnName, SQLSMALLINT columnNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLProcedureColumnsW");

  // Call to Acquire mutex for statement handle in odbc_lock.h.
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLProcedureColumnsW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_catalog_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_catalog_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (schemaNameLen && schemaNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_proc_name;
  SQLCHAR* sqlchar_proc_name = nullptr;
  if (procName) {
    utf8_proc_name = BqConvertSQLWCHARToString(procName, procNameLen);
    if (!utf8_proc_name) {
      return utf8_proc_name.GetCalculatedReturnCode();
    }
    sqlchar_proc_name = ToSqlChar(utf8_proc_name->data());
    if (procNameLen && procNameLen != SQL_NTS)
      procNameLen = utf8_proc_name->length();
  }

  StatusRecordOr<std::string> utf8_col_name;
  SQLCHAR* sqlchar_column_name = nullptr;
  if (columnName) {
    utf8_col_name = BqConvertSQLWCHARToString(columnName, columnNameLen);
    if (!utf8_col_name) {
      return utf8_col_name.GetCalculatedReturnCode();
    }
    sqlchar_column_name = ToSqlChar(utf8_col_name->data());
    if (columnNameLen && columnNameLen != SQL_NTS)
      columnNameLen = utf8_col_name->length();
  }

  // Call to common internal function for SQLProcedureColumns and
  // SQLProcedureColumnsW in odbc_driver_metadata.h.
  // Handle Unicode conversion of output parameters.
  rc = google::cloud::odbc_bq_driver::SQLProcedureColumnsInternal(
      statementHandle, sqlchar_catalog_name, catalogNameLen,
      sqlchar_schema_name, schemaNameLen, sqlchar_proc_name, procNameLen,
      sqlchar_column_name, columnNameLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Returns the list of procedure names stored in a specific data source.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlprocedures-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLProceduresA(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                                 SQLSMALLINT catalogNameLen,
                                 SQLCHAR* schemaName, SQLSMALLINT schemaNameLen,
                                 SQLCHAR* procName, SQLSMALLINT procNameLen) {
  return SQLProcedures(statementHandle, catalogName, catalogNameLen, schemaName,
                       schemaNameLen, procName, procNameLen);
}

SQLRETURN SQL_API SQLProcedures(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                                SQLSMALLINT catalogNameLen, SQLCHAR* schemaName,
                                SQLSMALLINT schemaNameLen, SQLCHAR* procName,
                                SQLSMALLINT procNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLProcedures");

  // Acquire mutex lock
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLProcedures");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLProcedures and SQLProceduresW
  // in odbc_driver_metadata.h.
  rc = ::google::cloud::odbc_bq_driver::SQLProcedureInternal(
      statementHandle, catalogName, catalogNameLen, schemaName, schemaNameLen,
      procName, procNameLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLProcedures.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLProceduresW(SQLHSTMT statementHandle,
                                 SQLWCHAR* catalogName,
                                 SQLSMALLINT catalogNameLen,
                                 SQLWCHAR* schemaName,
                                 SQLSMALLINT schemaNameLen, SQLWCHAR* procName,
                                 SQLSMALLINT procNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLProceduresW");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLProceduresW");
    return SQL_ERROR;
  }
  // Handle Unicode conversion of input parameters.

  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_catalog_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_catalog_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (schemaNameLen && schemaNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_proc_name;
  SQLCHAR* sqlchar_proc_name = nullptr;
  if (procName) {
    utf8_proc_name = BqConvertSQLWCHARToString(procName, procNameLen);
    if (!utf8_proc_name) {
      return utf8_proc_name.GetCalculatedReturnCode();
    }
    sqlchar_proc_name = ToSqlChar(utf8_proc_name->data());
    if (procNameLen && procNameLen != SQL_NTS)
      procNameLen = utf8_proc_name->length();
  }

  // Call to common internal function for SQLProcedures and SQLProceduresW
  // in odbc_driver_metadata.h.
  // Handle Unicode conversion of output parameters.
  rc = google::cloud::odbc_bq_driver::SQLProcedureInternal(
      statementHandle, sqlchar_catalog_name, catalogNameLen,
      sqlchar_schema_name, schemaNameLen, sqlchar_proc_name, procNameLen);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Retrieves the following information about columns within a specified table:
//   - The optimal set of columns that uniquely identifies a row in the table.
//   - Columns that are automatically updated when any value in the row is
//     updated by a transaction.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlspecialcolumns-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLSpecialColumnsA(
    SQLHSTMT statementHandle, SQLUSMALLINT identifierType, SQLCHAR* catalogName,
    SQLSMALLINT catalogNameLen, SQLCHAR* schemaName, SQLSMALLINT schemaNameLen,
    SQLCHAR* tableName, SQLSMALLINT tableNameLen, SQLUSMALLINT minRowIdScope,
    SQLUSMALLINT colNullable) {
  return SQLSpecialColumns(statementHandle, identifierType, catalogName,
                           catalogNameLen, schemaName, schemaNameLen, tableName,
                           tableNameLen, minRowIdScope, colNullable);
}

SQLRETURN SQL_API SQLSpecialColumns(
    SQLHSTMT statementHandle, SQLUSMALLINT identifierType, SQLCHAR* catalogName,
    SQLSMALLINT catalogNameLen, SQLCHAR* schemaName, SQLSMALLINT schemaNameLen,
    SQLCHAR* tableName, SQLSMALLINT tableNameLen, SQLUSMALLINT minRowIdScope,
    SQLUSMALLINT colNullable) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLSpecialColumns");
    return SQL_ERROR;
  }
  // Call to Trace function entry in odbc_trace.h if tracing is enabled.

  // Call to common internal function for SQLSpecialColumns and
  // SQLSpecialColumnsW in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLSpecialColumnsInternal(
      statementHandle, identifierType, catalogName, catalogNameLen, schemaName,
      schemaNameLen, tableName, tableNameLen, minRowIdScope, colNullable);

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLSpecialColumns.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLSpecialColumnsW(
    SQLHSTMT statementHandle, SQLUSMALLINT identifierType,
    SQLWCHAR* catalogName, SQLSMALLINT catalogNameLen, SQLWCHAR* schemaName,
    SQLSMALLINT schemaNameLen, SQLWCHAR* tableName, SQLSMALLINT tableNameLen,
    SQLUSMALLINT minRowIdScope, SQLUSMALLINT colNullable) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLSpecialColumnsW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLSpecialColumnsW");
    return SQL_ERROR;
  }
  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_category_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_category_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (schemaNameLen && schemaNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_table_name;
  SQLCHAR* sqlchar_table_name = nullptr;
  if (tableName) {
    utf8_table_name = BqConvertSQLWCHARToString(tableName, tableNameLen);
    if (!utf8_table_name) {
      return utf8_table_name.GetCalculatedReturnCode();
    }
    sqlchar_table_name = ToSqlChar(utf8_table_name->data());
    if (tableNameLen && tableNameLen != SQL_NTS)
      tableNameLen = utf8_table_name->length();
  }
  // Call to common internal function for SQLSpecialColumns and
  // SQLSpecialColumnsW in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLSpecialColumnsInternal(
      statementHandle, identifierType, sqlchar_category_name, catalogNameLen,
      sqlchar_schema_name, schemaNameLen, sqlchar_table_name, tableNameLen,
      minRowIdScope, colNullable);
  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Retrieves a list of statistics about a single table and the indexes
// associated with the table. The driver returns the information as a result
// set.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlstatistics-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLStatisticsA(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                                 SQLSMALLINT catalogNameLen,
                                 SQLCHAR* schemaName, SQLSMALLINT schemaNameLen,
                                 SQLCHAR* tableName, SQLSMALLINT tableNameLen,
                                 SQLUSMALLINT indexType,
                                 SQLUSMALLINT reserved) {
  return SQLStatistics(statementHandle, catalogName, catalogNameLen, schemaName,
                       schemaNameLen, tableName, tableNameLen, indexType,
                       reserved);
}

SQLRETURN SQL_API SQLStatistics(SQLHSTMT statementHandle, SQLCHAR* catalogName,
                                SQLSMALLINT catalogNameLen, SQLCHAR* schemaName,
                                SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
                                SQLSMALLINT tableNameLen,
                                SQLUSMALLINT indexType, SQLUSMALLINT reserved) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLStatistics");
    return SQL_ERROR;
  }

  // Call to Trace function entry in odbc_trace.h if tracing is enabled.

  // Call to common internal function for SQLStatistics and SQLStatisticsW
  // in odbc_driver_metadata.h.

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLStatistics.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLStatisticsW(
    SQLHSTMT statementHandle, SQLWCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLWCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLWCHAR* tableName,
    SQLSMALLINT tableNameLen, SQLUSMALLINT indexType, SQLUSMALLINT reserved) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLStatisticsW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLStatisticsW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_category_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_category_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (schemaNameLen && schemaNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_table_name;
  SQLCHAR* sqlchar_table_name = nullptr;
  if (tableName) {
    utf8_table_name = BqConvertSQLWCHARToString(tableName, tableNameLen);
    if (!utf8_table_name) {
      return utf8_table_name.GetCalculatedReturnCode();
    }
    sqlchar_table_name = ToSqlChar(utf8_table_name->data());
    if (tableNameLen && tableNameLen != SQL_NTS)
      tableNameLen = utf8_table_name->length();
  }

  // Call to common internal function for SQLStatistics and SQLStatisticsW
  // in odbc_driver_metadata.h.
  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Returns a list of tables and the privileges associated with each table.
// The driver returns the information as a result set on the specified
// statement..
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqltableprivileges-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLTablePrivilegesA(
    SQLHSTMT statementHandle, SQLCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
    SQLSMALLINT tableNameLen) {
  return SQLTablePrivileges(statementHandle, catalogName, catalogNameLen,
                            schemaName, schemaNameLen, tableName, tableNameLen);
}

SQLRETURN SQL_API SQLTablePrivileges(
    SQLHSTMT statementHandle, SQLCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
    SQLSMALLINT tableNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLTablePrivileges");
    return SQL_ERROR;
  }
  // Call to Trace function entry in odbc_trace.h if tracing is enabled.

  // Call to common internal function for SQLTablePrivileges and
  // SQLTablePrivilegesW in odbc_driver_metadata.h.

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLTablePrivileges.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLTablePrivilegesW(
    SQLHSTMT statementHandle, SQLWCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLWCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLWCHAR* tableName,
    SQLSMALLINT tableNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLTablePrivilegesW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLTablePrivilegesW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_category_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_category_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (schemaNameLen && schemaNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_table_name;
  SQLCHAR* sqlchar_table_name = nullptr;
  if (tableName) {
    utf8_table_name = BqConvertSQLWCHARToString(tableName, tableNameLen);
    if (!utf8_table_name) {
      return utf8_table_name.GetCalculatedReturnCode();
    }
    sqlchar_table_name = ToSqlChar(utf8_table_name->data());
    if (tableNameLen && tableNameLen != SQL_NTS)
      tableNameLen = utf8_table_name->length();
  }

  // Call to common internal function for SQLTablePrivileges and
  // SQLTablePrivilegesW in odbc_driver_metadata.h.
  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Returns
//   -- A list of foreign keys in the specified table (columns in the specified
//   table that
//      refer to primary keys in other tables).
//   -- A list of foreign keys in other tables that refer to the primary key in
//   the
//      specified table.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlforeignkeys-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API
SQLForeignKeysA(SQLHSTMT statementHandle, SQLCHAR* pkCatalogName,
                SQLSMALLINT pkCatalogNameLen, SQLCHAR* pkSchemaName,
                SQLSMALLINT pkSchemaNameLen, SQLCHAR* pkTableName,
                SQLSMALLINT pkTableNameLen, SQLCHAR* fkCatalogName,
                SQLSMALLINT fkCatalogNameLen, SQLCHAR* fkSchemaName,
                SQLSMALLINT fkSchemaNameLen, SQLCHAR* fkTableName,
                SQLSMALLINT fkTableNameLen) {
  return SQLForeignKeys(statementHandle, pkCatalogName, pkCatalogNameLen,
                        pkSchemaName, pkSchemaNameLen, pkTableName,
                        pkTableNameLen, fkCatalogName, fkCatalogNameLen,
                        fkSchemaName, fkSchemaNameLen, fkTableName,
                        fkTableNameLen);
}

SQLRETURN SQL_API
SQLForeignKeys(SQLHSTMT statementHandle, SQLCHAR* pkCatalogName,
               SQLSMALLINT pkCatalogNameLen, SQLCHAR* pkSchemaName,
               SQLSMALLINT pkSchemaNameLen, SQLCHAR* pkTableName,
               SQLSMALLINT pkTableNameLen, SQLCHAR* fkCatalogName,
               SQLSMALLINT fkCatalogNameLen, SQLCHAR* fkSchemaName,
               SQLSMALLINT fkSchemaNameLen, SQLCHAR* fkTableName,
               SQLSMALLINT fkTableNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLForeignKeys");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLForeignKeys");
    return SQL_ERROR;
  }

  // Call to common internal function for SQLForeignKeys and SQLForeignKeysW
  // in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLForeignKeysInternal(
      statementHandle, pkCatalogName, pkCatalogNameLen, pkSchemaName,
      pkSchemaNameLen, pkTableName, pkTableNameLen, fkCatalogName,
      fkCatalogNameLen, fkSchemaName, fkSchemaNameLen, fkTableName,
      fkTableNameLen);

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLForeignKeys.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API
SQLForeignKeysW(SQLHSTMT statementHandle, SQLWCHAR* pkCatalogName,
                SQLSMALLINT pkCatalogNameLen, SQLWCHAR* pkSchemaName,
                SQLSMALLINT pkSchemaNameLen, SQLWCHAR* pkTableName,
                SQLSMALLINT pkTableNameLen, SQLWCHAR* fkCatalogName,
                SQLSMALLINT fkCatalogNameLen, SQLWCHAR* fkSchemaName,
                SQLSMALLINT fkSchemaNameLen, SQLWCHAR* fkTableName,
                SQLSMALLINT fkTableNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLForeignKeysW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLForeignKeysW");
    return SQL_ERROR;
  }

  // Handle Unicode conversion of input parameters.
  StatusRecordOr<std::string> utf8_pk_catalog_name;
  SQLCHAR* sqlchar_pk_category_name = nullptr;
  if (pkCatalogName) {
    utf8_pk_catalog_name =
        BqConvertSQLWCHARToString(pkCatalogName, pkCatalogNameLen);
    if (!utf8_pk_catalog_name) {
      return utf8_pk_catalog_name.GetCalculatedReturnCode();
    }

    sqlchar_pk_category_name = ToSqlChar(utf8_pk_catalog_name->data());
    if (pkCatalogNameLen && pkCatalogNameLen != SQL_NTS)
      pkCatalogNameLen = utf8_pk_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_pk_schema_name;
  SQLCHAR* sqlchar_pk_schema_name = nullptr;
  if (pkSchemaName) {
    utf8_pk_schema_name =
        BqConvertSQLWCHARToString(pkSchemaName, pkSchemaNameLen);
    if (!utf8_pk_schema_name) {
      return utf8_pk_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_pk_schema_name = ToSqlChar(utf8_pk_schema_name->data());
    if (pkSchemaNameLen && pkSchemaNameLen != SQL_NTS)
      pkSchemaNameLen = utf8_pk_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_pk_table_name;
  SQLCHAR* sqlchar_pk_table_name = nullptr;
  if (pkTableName) {
    utf8_pk_table_name = BqConvertSQLWCHARToString(pkTableName, pkTableNameLen);
    if (!utf8_pk_table_name) {
      return utf8_pk_table_name.GetCalculatedReturnCode();
    }
    sqlchar_pk_table_name = ToSqlChar(utf8_pk_table_name->data());
    if (pkTableNameLen && pkTableNameLen != SQL_NTS)
      pkTableNameLen = utf8_pk_table_name->length();
  }

  StatusRecordOr<std::string> utf8_fk_catalog_name;
  SQLCHAR* sqlchar_fk_category_name = nullptr;
  if (fkCatalogName) {
    utf8_fk_catalog_name =
        BqConvertSQLWCHARToString(fkCatalogName, fkCatalogNameLen);
    if (!utf8_fk_catalog_name) {
      return utf8_fk_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_fk_category_name = ToSqlChar(utf8_fk_catalog_name->data());
    if (fkCatalogNameLen && fkCatalogNameLen != SQL_NTS)
      fkCatalogNameLen = utf8_fk_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_fk_schema_name;
  SQLCHAR* sqlchar_fk_schema_name = nullptr;
  if (fkSchemaName) {
    utf8_fk_schema_name =
        BqConvertSQLWCHARToString(fkSchemaName, fkSchemaNameLen);
    if (!utf8_fk_schema_name) {
      return utf8_fk_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_fk_schema_name = ToSqlChar(utf8_fk_schema_name->data());
    if (fkSchemaNameLen && fkSchemaNameLen != SQL_NTS)
      fkSchemaNameLen = utf8_fk_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_fk_table_name;
  SQLCHAR* sqlchar_fk_table_name = nullptr;
  if (fkTableName) {
    utf8_fk_table_name = BqConvertSQLWCHARToString(fkTableName, fkTableNameLen);
    if (!utf8_fk_table_name) {
      return utf8_fk_table_name.GetCalculatedReturnCode();
    }
    sqlchar_fk_table_name = ToSqlChar(utf8_fk_table_name->data());
    if (fkTableNameLen && fkTableNameLen != SQL_NTS)
      fkTableNameLen = utf8_fk_table_name->length();
  }

  // Call to common internal function for SQLForeignKeys and SQLForeignKeysW
  // in odbc_driver_metadata.h.
  rc = google::cloud::odbc_bq_driver::SQLForeignKeysInternal(
      statementHandle, sqlchar_pk_category_name, pkCatalogNameLen,
      sqlchar_pk_schema_name, pkSchemaNameLen, sqlchar_pk_table_name,
      pkTableNameLen, sqlchar_fk_category_name, fkCatalogNameLen,
      sqlchar_fk_schema_name, fkSchemaNameLen, sqlchar_fk_table_name,
      fkTableNameLen);

  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Returns a list of columns and associated privileges for the specified table.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcolumnprivileges-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLColumnPrivilegesA(
    SQLHSTMT statementHandle, SQLCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
    SQLSMALLINT tableNameLen, SQLCHAR* columnName, SQLSMALLINT columnNameLen) {
  return SQLColumnPrivileges(statementHandle, catalogName, catalogNameLen,
                             schemaName, schemaNameLen, tableName, tableNameLen,
                             columnName, columnNameLen);
}

SQLRETURN SQL_API SQLColumnPrivileges(
    SQLHSTMT statementHandle, SQLCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLCHAR* tableName,
    SQLSMALLINT tableNameLen, SQLCHAR* columnName, SQLSMALLINT columnNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLColumnPrivileges");
    return SQL_ERROR;
  }

  // Call to Trace function entry in odbc_trace.h if tracing is enabled.

  // Call to common internal function for SQLColumnPrivileges and
  // SQLColumnPrivilegesW in odbc_driver_metadata.h.

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}
////////////////////////////////////////
// Unicode version of SQLColumnPrivileges.
////////////////////////////////////////
// TODO(b/361047481): Add Integration Testcase for Unicode Support.
SQLRETURN SQL_API SQLColumnPrivilegesW(
    SQLHSTMT statementHandle, SQLWCHAR* catalogName, SQLSMALLINT catalogNameLen,
    SQLWCHAR* schemaName, SQLSMALLINT schemaNameLen, SQLWCHAR* tableName,
    SQLSMALLINT tableNameLen, SQLWCHAR* columnName, SQLSMALLINT columnNameLen) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLColumnPrivilegesW");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLColumnPrivilegesW");
    return SQL_ERROR;
  }
  // Handle Unicode conversion of input parameters.

  StatusRecordOr<std::string> utf8_catalog_name;
  SQLCHAR* sqlchar_category_name = nullptr;
  if (catalogName) {
    utf8_catalog_name = BqConvertSQLWCHARToString(catalogName, catalogNameLen);
    if (!utf8_catalog_name) {
      return utf8_catalog_name.GetCalculatedReturnCode();
    }
    sqlchar_category_name = ToSqlChar(utf8_catalog_name->data());
    if (catalogNameLen && catalogNameLen != SQL_NTS)
      catalogNameLen = utf8_catalog_name->length();
  }

  StatusRecordOr<std::string> utf8_schema_name;
  SQLCHAR* sqlchar_schema_name = nullptr;
  if (schemaName) {
    utf8_schema_name = BqConvertSQLWCHARToString(schemaName, schemaNameLen);
    if (!utf8_schema_name) {
      return utf8_schema_name.GetCalculatedReturnCode();
    }
    sqlchar_schema_name = ToSqlChar(utf8_schema_name->data());
    if (schemaNameLen && schemaNameLen != SQL_NTS)
      schemaNameLen = utf8_schema_name->length();
  }

  StatusRecordOr<std::string> utf8_table_name;
  SQLCHAR* sqlchar_table_name = nullptr;
  if (tableName) {
    utf8_table_name = BqConvertSQLWCHARToString(tableName, tableNameLen);
    if (!utf8_table_name) {
      return utf8_table_name.GetCalculatedReturnCode();
    }
    sqlchar_table_name = ToSqlChar(utf8_table_name->data());
    if (tableNameLen && tableNameLen != SQL_NTS)
      tableNameLen = utf8_table_name->length();
  }

  StatusRecordOr<std::string> utf8_col_name;
  SQLCHAR* sqlchar_column_name = nullptr;
  if (columnName) {
    utf8_col_name = BqConvertSQLWCHARToString(columnName, columnNameLen);
    if (!utf8_col_name) {
      return utf8_col_name.GetCalculatedReturnCode();
    }
    sqlchar_column_name = ToSqlChar(utf8_col_name->data());
    if (columnNameLen && columnNameLen != SQL_NTS)
      columnNameLen = utf8_col_name->length();
  }

  // Call to common internal function for SQLColumnPrivileges and
  // SQLColumnPrivilegesW in odbc_driver_metadata.h.
  // Handle Unicode conversion of output parameters.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Stops processing associated with a specific statement, closes any open
// cursors associated with the statement, discards pending results, or,
// optionally, frees all resources associated with the statement handle.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfreestmt-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLFreeStmt(SQLHSTMT statementHandle, SQLUSMALLINT option) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLFreeStmt");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLFreeStmt");
    return SQL_ERROR;
  }

  // Call to internal function for SQLFreeStmt in odbc_statement.h.
  rc = google::cloud::odbc_bq_driver::SQLFreeStmtInternal(statementHandle,
                                                          option);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Requests a commit or rollback operation for all active operations on all
// statements associated with a connection.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlendtran-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLEndTran(SQLSMALLINT handleType, SQLHANDLE handle,
                             SQLSMALLINT completionType) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLEndTran");

  HandleLock lock(handle, handleType);
  if (!lock.isLocked()) {
    HandleLockError(handleType, handle, "SQLEndTran");
    return SQL_ERROR;
  }

  // Call to internal function for SQLEndTran in odbc_statement.h.
  rc = google::cloud::odbc_bq_driver::SQLEndTranInternal(handleType, handle,
                                                         completionType);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Cancels the processing on a statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcancel-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLCancel(SQLHSTMT statementHandle) {
  SQLRETURN status = SQL_SUCCESS;
  InitializeTracing("SQLCancel");

  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLCancel");
    return SQL_ERROR;
  }

  // Call to internal function for SQLCancel in odbc_sql_results.h.
  status = google::cloud::odbc_bq_driver::SQLCancelInternal(statementHandle);

  return status;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Closes a cursor that has been opened on a statement and discards pending
// results.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlclosecursor-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLCloseCursor(SQLHSTMT statementHandle) {
  SQLRETURN rc = SQL_SUCCESS;
  InitializeTracing("SQLCloseCursor");
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLCloseCursor");
    return SQL_ERROR;
  }

  // Call to internal function for SQLCloseCursor in odbc_sql_results.h.
  rc = google::cloud::odbc_bq_driver::SQLCloseCursorInternal(statementHandle);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Closes the connection associated with a specific connection handle.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldisconnect-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLDisconnect(SQLHDBC connectionHandle) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLDisconnect");

  HandleLock lock(connectionHandle, SQL_HANDLE_DBC);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_DBC, connectionHandle, "SQLDisconnect");
    return SQL_ERROR;
  }

  // Call to internal function for SQLCancel in odbc_connection.h.
  rc = google::cloud::odbc_bq_driver::SQLDisconnectInternal(connectionHandle);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Frees resources associated with a specific environment, connection,
// statement, or descriptor handle.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfreehandle-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLFreeHandle(SQLSMALLINT handleType, SQLHANDLE handle) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;
  InitializeTracing("SQLFreeHandle");

  // Send lock request on the parent as the handle will be deleted
  HandleLock lock(handle, handleType, true);
  if (!lock.isLocked()) {
    HandleLockError(handleType, handle, "SQLFreeHandle");
    return SQL_ERROR;
  }
  // Call to internal function for SQLFreeHandle in odbc_commons.h
  rc = google::cloud::odbc_bq_driver::SQLFreeHandleInternal(handleType, handle);

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
//
// ODBC APIs supported in future driver releases.
//
////////////////////////////////////////////////////////////////////////////////////////////

#if !defined(_WIN32) || defined(_WIN64)

////////////////////////////////////////////////////////////////////////////////////////////
// Cancels the processing on a connection or statement.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcancelhandle-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQLCancelHandle(SQLSMALLINT handleType, SQLHANDLE handle) {
  SQLRETURN rc = SQL_SUCCESS;
  SQLRETURN status;

  HandleLock lock(handle, handleType);
  if (!lock.isLocked()) {
    HandleLockError(handleType, handle, "SQLCancelHandle");
    return SQL_ERROR;
  }
  // passed in. Call to Trace function entry in odbc_trace.h if tracing is
  // enabled.

  // Call to internal function for SQLCancelHandle in odbc_environment.h

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  // passed in.

  return rc;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Sets the cursor position in a rowset and allows an application to refresh
// data in the rowset or to update or delete data in the result set.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetpos-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQLSetPos(SQLHSTMT statementHandle, SQLSETPOSIROW rowNumber,
                    SQLUSMALLINT operation, SQLUSMALLINT lockType) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLSetPos");
    return SQL_ERROR;
  }
  // Call to Trace function entry in odbc_trace.h if tracing is enabled.

  // Call to internal function for SQLSetPos in odbc_sql_results.h.

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}
#endif  //_WIN32

////////////////////////////////////////////////////////////////////////////////////////////
// Performs bulk insertions and bulk bookmark operations, including update,
// delete, and fetch by bookmark.
//
// For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlbulkoperations-function.
////////////////////////////////////////////////////////////////////////////////////////////
SQLRETURN SQL_API SQLBulkOperations(SQLHSTMT statementHandle,
                                    SQLSMALLINT operation) {
  SQLRETURN rc = SQL_SUCCESS;
  HandleLock lock(statementHandle, SQL_HANDLE_STMT);
  if (!lock.isLocked()) {
    HandleLockError(SQL_HANDLE_STMT, statementHandle, "SQLBulkOperations");
    return SQL_ERROR;
  }

  // Call to Trace function entry in odbc_trace.h if tracing is enabled.

  // Call to internal function for SQLBulkOperations in odbc_sql_requests.h.

  // Call to Trace function exit in odbc_trace.h if tracing is enabled.

  return rc;
}

#ifdef _WIN32
////////////////////////////////////////////////////////////////////////////////////////////
//  adds, modifies, or deletes data sources from the system information.
// It may prompt the user for connection information. It can be in the driver
// DLL or a separate setup DLL. For more details see:
// https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/configdsn-function?view=sql-server-ver16
////////////////////////////////////////////////////////////////////////////////////////////
BOOL SQL_API ConfigDSN(HWND hwndParent, WORD fRequest, LPCSTR lpszDriver,
                       LPCSTR lpszAttributes) {
  bool rc = TRUE;
  InitializeTracing("ConfigDSN");

  // Call to common internal function for ConfigDSN
  // in odbc_windows.h.
  rc = google::cloud::odbc_bq_driver::ConfigDSNInternal(
      hwndParent, fRequest, lpszDriver, lpszAttributes);

  return rc;
}
#endif  // _WIN32
// NOLINTEND
