// Copyright 2020-2026 ONDEWO GmbH
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// The production KeycloakTransport of <ondewo/client/keycloak.h>, on libcurl.
//
// HAND-WRITTEN and header-only, in a header of its own so that only a consumer who includes it
// needs libcurl: link it with find_package(CURL REQUIRED) and target_link_libraries(... CURL::libcurl).
//
// One POST per call, application/x-www-form-urlencoded, bounded as a whole by the request's
// timeout (connect + read), no redirect followed, TLS verification per the request's verify_ssl
// for THAT request only (never a process-wide switch). A request that gets no HTTP answer throws
// KeycloakTransportError ("Timeout", "ConnectionError" or "TransportError"); any HTTP answer,
// including an error status, is returned for the provider to judge. Nothing is logged here, so no
// form field can reach a log line.

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <curl/curl.h>

#include <ondewo/client/keycloak.h>

namespace ondewo {
namespace client {

namespace internal {

inline std::size_t AppendCurlBody(char* data, std::size_t size, std::size_t count, void* body) {
  static_cast<std::string*>(body)->append(data, size * count);
  return size * count;
}

inline std::string CurlErrorKind(CURLcode code) {
  switch (code) {
    case CURLE_OPERATION_TIMEDOUT:
      return "Timeout";
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
      return "ConnectionError";
    default:
      return "TransportError";
  }
}

inline std::string UrlEncodedForm(CURL* curl, const KeycloakHttpRequest& request) {
  std::string form;
  for (const auto& [name, value] : request.form) {
    const std::unique_ptr<char, void (*)(void*)> encoded_name(
        curl_easy_escape(curl, name.data(), static_cast<int>(name.size())), curl_free);
    const std::unique_ptr<char, void (*)(void*)> encoded_value(
        curl_easy_escape(curl, value.data(), static_cast<int>(value.size())), curl_free);
    if (!form.empty()) form += '&';
    form += std::string(encoded_name.get()) + "=" + encoded_value.get();
  }
  return form;
}

}  // namespace internal

// Sends KeycloakHttpRequest with libcurl. Thread-safe: every call uses an easy handle of its own,
// and curl_global_init runs once.
inline KeycloakTransport CurlKeycloakTransport() {
  static std::once_flag initialised;
  std::call_once(initialised, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
  return [](const KeycloakHttpRequest& request) {
    const std::unique_ptr<CURL, void (*)(CURL*)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw KeycloakTransportError("TransportError", "curl_easy_init failed");  // GCOVR_EXCL_LINE
    const std::string form = internal::UrlEncodedForm(curl.get(), request);
    const std::unique_ptr<curl_slist, void (*)(curl_slist*)> headers(
        curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded"), curl_slist_free_all);
    KeycloakHttpResponse response;
    char error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, form.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(form.size()));
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout_s * 1000.0));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, request.verify_ssl ? 1L : 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, request.verify_ssl ? 2L : 0L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, internal::AppendCurlBody);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error);
    const CURLcode code = curl_easy_perform(curl.get());
    if (code != CURLE_OK) {
      throw KeycloakTransportError(internal::CurlErrorKind(code),
                                   std::string(curl_easy_strerror(code)) + (error[0] ? std::string(": ") + error : ""));
    }
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.status_code);
    return response;
  };
}

// KeycloakProviderOptions with the libcurl transport and every other member at its default.
inline KeycloakProviderOptions CurlKeycloakProviderOptions() {
  KeycloakProviderOptions options;
  options.transport = CurlKeycloakTransport();
  return options;
}

}  // namespace client
}  // namespace ondewo
