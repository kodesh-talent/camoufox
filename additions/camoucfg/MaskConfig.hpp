/*
Helper to extract values from the CAMOU_CONFIG environment variable(s).
Written by daijro.
*/

#pragma once
#include "json.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <optional>
#include <codecvt>
#include "mozilla/glue/Debug.h"
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <variant>
#include <cstddef>
#include <vector>
#include <algorithm>
#include <cmath>
#include <limits>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace MaskConfig {

inline constexpr size_t kMaxFingerprintFileSize = 4 * 1024 * 1024;

// Function to get the value of an environment variable as a UTF-8 string.
inline std::optional<std::string> get_env_utf8(const std::string& name) {
#ifdef _WIN32
  std::wstring wName(name.begin(), name.end());
  DWORD size = GetEnvironmentVariableW(wName.c_str(), nullptr, 0);
  if (size == 0) return std::nullopt;  // Environment variable not found

  std::vector<wchar_t> buffer(size);
  GetEnvironmentVariableW(wName.c_str(), buffer.data(), size);
  std::wstring wValue(buffer.data());

  // Convert UTF-16 to UTF-8
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  return converter.to_bytes(wValue);
#else
  const char* value = std::getenv(name.c_str());
  if (!value) return std::nullopt;
  return std::string(value);
#endif
}

inline bool set_env_utf8(const std::string& name,
                         const std::optional<std::string>& value) {
#ifdef _WIN32
  std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
  std::wstring wName = converter.from_bytes(name);
  if (!value) {
    if (SetEnvironmentVariableW(wName.c_str(), nullptr) != 0) return true;
    return GetLastError() == ERROR_ENVVAR_NOT_FOUND;
  }
  std::wstring wValue = converter.from_bytes(*value);
  return SetEnvironmentVariableW(wName.c_str(), wValue.c_str()) != 0;
#else
  if (!value) return unsetenv(name.c_str()) == 0;
  return setenv(name.c_str(), value->c_str(), 1) == 0;
#endif
}

inline bool ClearConfigEnvironment() {
  bool ok = set_env_utf8("CAMOU_CONFIG", std::nullopt);
  for (int index = 1; ; ++index) {
    std::string name = "CAMOU_CONFIG_" + std::to_string(index);
    if (!get_env_utf8(name)) break;
    ok = set_env_utf8(name, std::nullopt) && ok;
  }
  return ok;
}

inline bool SetConfigEnvironment(const std::string& jsonString) {
  if (!ClearConfigEnvironment()) return false;
  constexpr size_t kChunkSize =
#ifdef _WIN32
      2047;
#else
      32767;
#endif
  int index = 1;
  for (size_t start = 0; start < jsonString.size(); ++index) {
    size_t end = std::min(start + kChunkSize, jsonString.size());
    while (end < jsonString.size() && end > start &&
           (static_cast<unsigned char>(jsonString[end]) & 0xC0) == 0x80) {
      --end;
    }
    if (end == start) return false;
    std::string name = "CAMOU_CONFIG_" + std::to_string(index);
    if (!set_env_utf8(name, jsonString.substr(start, end - start))) {
      ClearConfigEnvironment();
      return false;
    }
    start = end;
  }
  return true;
}

inline const nlohmann::json& GetJson() {
  static std::once_flag initFlag;
  static nlohmann::json jsonConfig;

  std::call_once(initFlag, []() {
    std::string jsonString;
    int index = 1;

    while (true) {
      std::string envName = "CAMOU_CONFIG_" + std::to_string(index);
      auto partialConfig = get_env_utf8(envName);
      if (!partialConfig) break;

      jsonString += *partialConfig;
      index++;
    }

    if (jsonString.empty()) {
      // Check for the original CAMOU_CONFIG as fallback
      auto originalConfig = get_env_utf8("CAMOU_CONFIG");
      if (originalConfig) jsonString = *originalConfig;
    }

    if (jsonString.empty()) {
      jsonConfig = nlohmann::json{};
      return;
    }

    // Validate
    if (!nlohmann::json::accept(jsonString)) {
      printf_stderr("ERROR: Invalid JSON passed to CAMOU_CONFIG!\n");
      jsonConfig = nlohmann::json{};
      return;
    }

    jsonConfig = nlohmann::json::parse(jsonString);
  });

  return jsonConfig;
}

enum class FingerprintPropertyType {
  String,
  UnsignedInteger,
  Integer,
  Number,
  Boolean,
  Array,
  Object,
};

struct FingerprintProperty {
  std::string_view name;
  FingerprintPropertyType type;
};

#define CAMOUFOX_FINGERPRINT_PROPERTY(name, type) \
  {name, FingerprintPropertyType::type},
#include "FingerprintProperties.inc"
inline constexpr FingerprintProperty kFingerprintProperties[] = {
    CAMOUFOX_FINGERPRINT_PROPERTIES(CAMOUFOX_FINGERPRINT_PROPERTY)};
#undef CAMOUFOX_FINGERPRINT_PROPERTIES
#undef CAMOUFOX_FINGERPRINT_PROPERTY

inline const FingerprintProperty* FindFingerprintProperty(
    const std::string& name) {
  for (const auto& property : kFingerprintProperties) {
    if (property.name == name) return &property;
  }
  return nullptr;
}

inline bool IsFingerprintValueValid(const nlohmann::json& value,
                                    FingerprintPropertyType type) {
  switch (type) {
    case FingerprintPropertyType::String:
      return value.is_string();
    case FingerprintPropertyType::UnsignedInteger:
      return value.is_number_integer() && value >= 0 &&
             value <= std::numeric_limits<uint32_t>::max();
    case FingerprintPropertyType::Integer:
      return value.is_number_integer() &&
             value >= std::numeric_limits<int32_t>::min() &&
             value <= std::numeric_limits<int32_t>::max();
    case FingerprintPropertyType::Number:
      return value.is_number() && std::isfinite(value.get<double>());
    case FingerprintPropertyType::Boolean:
      return value.is_boolean();
    case FingerprintPropertyType::Array:
      return value.is_array();
    case FingerprintPropertyType::Object:
      return value.is_object();
  }
  return false;
}

inline bool IsFingerprintStructureValid(const std::string& name,
                                        const nlohmann::json& value) {
  if (value.is_array()) {
    for (const auto& item : value) {
      if (name != "voices") {
        if (!item.is_string()) return false;
        continue;
      }
      if (!item.is_object()) return false;
      for (const auto* field : {"lang", "name", "voiceUri"}) {
        if (!item.contains(field) || !item[field].is_string()) return false;
      }
      for (const auto* field : {"isDefault", "isLocalService"}) {
        if (!item.contains(field) || !item[field].is_boolean()) return false;
      }
    }
  }
  if (!value.is_object()) return true;
  for (const auto& [key, item] : value.items()) {
    if (name.find(":contextAttributes") != std::string::npos) {
      if (key == "powerPreference") {
        if (item != "default" && item != "low-power" && item != "high-performance") return false;
      } else if (!item.is_boolean()) {
        return false;
      }
    } else if (name.find(":shaderPrecisionFormats") != std::string::npos) {
      if (!item.is_object()) return false;
      for (const auto* field : {"rangeMin", "rangeMax", "precision"}) {
        if (!item.contains(field) || !item[field].is_number_integer() ||
            item[field] < 0 || item[field] > 255) return false;
      }
    } else if (name.find(":parameters") != std::string::npos) {
      // These parameters are consumed as fixed-size native arrays.
      const bool floatPair = key == "33901" || key == "33902";
      const bool intPair = key == "3386";
      const bool boolQuad = key == "3107";
      if (floatPair || intPair || boolQuad) {
        if (!item.is_array() || item.size() != (boolQuad ? 4 : 2)) return false;
        for (const auto& component : item) {
          if (boolQuad ? !component.is_boolean()
                       : !IsFingerprintValueValid(
                             component, intPair ? FingerprintPropertyType::Integer
                                                : FingerprintPropertyType::Number)) {
            return false;
          }
        }
      } else if (item.is_array()) {
        for (const auto& component : item) {
          if (!component.is_number() && !component.is_boolean()) return false;
        }
      } else if (!item.is_primitive() ||
                 (item.is_number_unsigned() &&
                  item > std::numeric_limits<int64_t>::max())) {
        return false;
      }
    }
  }
  return true;
}

inline std::optional<nlohmann::json> FingerprintOnly(
    const nlohmann::json& source, bool rejectUnknown, std::string& error) {
  if (!source.is_object()) {
    error = "fingerprint must be a JSON object";
    return std::nullopt;
  }

  nlohmann::json result = nlohmann::json::object();
  for (const auto& [name, value] : source.items()) {
    const auto* property = FindFingerprintProperty(name);
    if (!property) {
      if (rejectUnknown) {
        error = "property is not allowed in a fingerprint: " + name;
        return std::nullopt;
      }
      continue;
    }
    if (!IsFingerprintValueValid(value, property->type) ||
        !IsFingerprintStructureValid(name, value)) {
      error = "invalid value type for fingerprint property: " + name;
      return std::nullopt;
    }
    result[name] = value;
  }

  return result;
}

inline std::optional<std::string> CurrentFingerprintJson(std::string& error) {
  if (GetJson().is_null()) return "{}";
  auto fingerprint = FingerprintOnly(GetJson(), false, error);
  if (!fingerprint) return std::nullopt;
  return fingerprint->dump();
}

inline bool IsDocumentField(const std::string& name) {
  return name == "format" || name == "schemaVersion" ||
         name == "browserMajor" || name == "createdAt" || name == "fingerprint";
}

inline std::optional<std::string> CanonicalizeFingerprintDocument(
    const std::string& documentJson, uint32_t browserMajor,
    std::string& error) {
  if (documentJson.size() > kMaxFingerprintFileSize) {
    error = "fingerprint file exceeds 4 MiB";
    return std::nullopt;
  }
  auto document = nlohmann::json::parse(documentJson, nullptr, false);
  if (document.is_discarded()) {
    error = "fingerprint file is not valid JSON";
    return std::nullopt;
  }
  if (!document.is_object()) {
    error = "fingerprint file must contain a JSON object";
    return std::nullopt;
  }
  for (const auto& [name, unused] : document.items()) {
    if (!IsDocumentField(name)) {
      error = "unknown fingerprint document field: " + name;
      return std::nullopt;
    }
  }
  if (!document.contains("format") || !document["format"].is_string() ||
      document["format"] != "camoufox-fingerprint") {
    error = "unsupported fingerprint file format";
    return std::nullopt;
  }
  if (!document.contains("schemaVersion") ||
      !document["schemaVersion"].is_number_integer() ||
      document["schemaVersion"] != 1) {
    error = "unsupported fingerprint schema version";
    return std::nullopt;
  }
  if (!document.contains("browserMajor") ||
      !document["browserMajor"].is_number_integer() ||
      document["browserMajor"] != browserMajor) {
    error = "fingerprint browser major version does not match this build";
    return std::nullopt;
  }
  if (!document.contains("createdAt") || !document["createdAt"].is_string() ||
      document["createdAt"].get_ref<const std::string&>().empty()) {
    error = "fingerprint file has no creation timestamp";
    return std::nullopt;
  }
  if (!document.contains("fingerprint")) {
    error = "fingerprint file has no fingerprint object";
    return std::nullopt;
  }
  auto fingerprint = FingerprintOnly(document["fingerprint"], true, error);
  if (!fingerprint) return std::nullopt;
  return fingerprint->dump();
}

inline bool HasKey(const std::string& key, const nlohmann::json& data) {
  return data.contains(key);
}

inline std::optional<std::string> GetString(const std::string& key) {
  const auto& data = GetJson();
  if (!HasKey(key, data)) return std::nullopt;
  return data[key].get<std::string>();
}

inline std::vector<std::string> GetStringList(const std::string& key) {
  std::vector<std::string> result;
  const auto& data = GetJson();
  if (!HasKey(key, data)) return {};
  for (const auto& item : data[key]) {
    result.push_back(item.get<std::string>());
  }
  return result;
}

inline std::vector<std::string> GetStringListLower(const std::string& key) {
  std::vector<std::string> result = GetStringList(key);
  for (auto& str : result) {
    std::transform(str.begin(), str.end(), str.begin(),
                   [](unsigned char c) { return std::tolower(c); });
  }
  return result;
}

/**
 * The spoofed font family allowlist ("fonts"), lowercased and cached for the
 * lifetime of the process. CAMOU_CONFIG is read once at startup and never
 * changes, and the gfx font lookup paths consult this on every family
 * resolution, so re-parsing the JSON per call is not an option.
 * An empty list means no font spoofing is configured.
 */
inline const std::vector<std::string>& FontAllowlist() {
  static const std::vector<std::string> fonts = GetStringListLower("fonts");
  return fonts;
}

inline bool HasFontAllowlist() { return !FontAllowlist().empty(); }

/**
 * Whether a font family may be used. `family` must already be lowercased
 * (gfxPlatformFontList::GenerateFontListKey output is). Always true when no
 * allowlist is configured.
 */
inline bool IsFontAllowed(std::string_view family) {
  const auto& fonts = FontAllowlist();
  if (fonts.empty()) return true;
  return std::find(fonts.begin(), fonts.end(), family) != fonts.end();
}

template <typename T>
inline std::optional<T> GetUintImpl(const std::string& key) {
  const auto& data = GetJson();
  if (!HasKey(key, data)) return std::nullopt;
  if (data[key].is_number_unsigned()) return data[key].get<T>();
  printf_stderr("ERROR: Value for key '%s' is not an unsigned integer\n",
                key.c_str());
  return std::nullopt;
}

inline std::optional<uint64_t> GetUint64(const std::string& key) {
  return GetUintImpl<uint64_t>(key);
}

inline std::optional<uint32_t> GetUint32(const std::string& key) {
  return GetUintImpl<uint32_t>(key);
}

inline std::optional<int32_t> GetInt32(const std::string& key) {
  const auto& data = GetJson();
  if (!HasKey(key, data)) return std::nullopt;
  if (data[key].is_number_integer()) return data[key].get<int32_t>();
  printf_stderr("ERROR: Value for key '%s' is not an integer\n", key.c_str());
  return std::nullopt;
}

inline std::optional<double> GetDouble(const std::string& key) {
  const auto& data = GetJson();
  if (!HasKey(key, data)) return std::nullopt;
  if (data[key].is_number_float()) return data[key].get<double>();
  if (data[key].is_number_unsigned() || data[key].is_number_integer())
    return static_cast<double>(data[key].get<int64_t>());
  printf_stderr("ERROR: Value for key '%s' is not a double\n", key.c_str());
  return std::nullopt;
}

inline std::optional<bool> GetBool(const std::string& key) {
  const auto& data = GetJson();
  if (!HasKey(key, data)) return std::nullopt;
  if (data[key].is_boolean()) return data[key].get<bool>();
  printf_stderr("ERROR: Value for key '%s' is not a boolean\n", key.c_str());
  return std::nullopt;
}

inline bool CheckBool(const std::string& key) {
  return GetBool(key).value_or(false);
}

inline std::optional<std::array<uint32_t, 4>> GetRect(
    const std::string& left, const std::string& top, const std::string& width,
    const std::string& height) {
  std::array<std::optional<uint32_t>, 4> values = {
      GetUint32(left).value_or(0), GetUint32(top).value_or(0), GetUint32(width),
      GetUint32(height)};

  if (!values[2].has_value() || !values[3].has_value()) {
    if (values[2].has_value() ^ values[3].has_value())
      printf_stderr(
          "Both %s and %s must be provided. Using default behavior.\n",
          height.c_str(), width.c_str());
    return std::nullopt;
  }

  std::array<uint32_t, 4> result;
  std::transform(values.begin(), values.end(), result.begin(),
                 [](const auto& value) { return value.value(); });

  return result;
}

// Helpers for WebGL

inline std::optional<nlohmann::json> GetNested(const std::string& domain,
                                               std::string keyStr) {
  auto data = GetJson();
  if (!data.contains(domain)) return std::nullopt;

  if (!data[domain].contains(keyStr)) return std::nullopt;

  return data[domain][keyStr];
}

template <typename T>
inline std::optional<T> GetAttribute(const std::string attrib, bool isWebGL2) {
  auto value = MaskConfig::GetNested(
      isWebGL2 ? "webGl2:contextAttributes" : "webGl:contextAttributes",
      attrib);
  if (!value) return std::nullopt;
  return value.value().get<T>();
}

inline std::optional<
    std::variant<int64_t, bool, double, std::string, std::nullptr_t>>
GLParam(uint32_t pname, bool isWebGL2) {
  auto value =
      MaskConfig::GetNested(isWebGL2 ? "webGl2:parameters" : "webGl:parameters",
                            std::to_string(pname));
  if (!value) return std::nullopt;
  auto data = value.value();
  if (data.is_null()) return std::nullptr_t();
  if (data.is_number_integer()) return data.get<int64_t>();
  if (data.is_boolean()) return data.get<bool>();
  if (data.is_number_float()) return data.get<double>();
  if (data.is_string()) return data.get<std::string>();
  return std::nullopt;
}

template <typename T>
inline T MParamGL(uint32_t pname, T defaultValue, bool isWebGL2) {
  if (auto value = MaskConfig::GetNested(
          isWebGL2 ? "webGl2:parameters" : "webGl:parameters",
          std::to_string(pname));
      value.has_value()) {
    return value.value().get<T>();
  }
  return defaultValue;
}

template <typename T>
inline std::vector<T> MParamGLVector(uint32_t pname,
                                     std::vector<T> defaultValue,
                                     bool isWebGL2) {
  if (auto value = MaskConfig::GetNested(
          isWebGL2 ? "webGl2:parameters" : "webGl:parameters",
          std::to_string(pname));
      value.has_value()) {
    if (value.value().is_array()) {
      std::array<T, 4UL> result = value.value().get<std::array<T, 4UL>>();
      return std::vector<T>(result.begin(), result.end());
    }
  }
  return defaultValue;
}

inline std::optional<std::array<int32_t, 3UL>> MShaderData(
    uint32_t shaderType, uint32_t precisionType, bool isWebGL2) {
  std::string valueName =
      std::to_string(shaderType) + "," + std::to_string(precisionType);
  if (auto value =
          MaskConfig::GetNested(isWebGL2 ? "webGl2:shaderPrecisionFormats"
                                         : "webGl:shaderPrecisionFormats",
                                valueName)) {
    // Convert {rangeMin: int, rangeMax: int, precision: int} to array
    auto data = value.value();
    // Assert rangeMin, rangeMax, and precision are present
    if (!data.contains("rangeMin") || !data.contains("rangeMax") ||
        !data.contains("precision")) {
      return std::nullopt;
    }
    return std::array<int32_t, 3U>{data["rangeMin"].get<int32_t>(),
                                   data["rangeMax"].get<int32_t>(),
                                   data["precision"].get<int32_t>()};
  }
  return std::nullopt;
}

inline std::optional<
    std::vector<std::tuple<std::string, std::string, std::string, bool, bool>>>
MVoices() {
  auto data = GetJson();
  if (!data.contains("voices") || !data["voices"].is_array()) {
    return std::nullopt;
  }

  std::vector<std::tuple<std::string, std::string, std::string, bool, bool>>
      voices;
  for (const auto& voice : data["voices"]) {
    // Each voice must be a full object with all five fields. A bare string
    // (e.g. "Name:lang:type") or an object missing a field registers NOTHING
    // and would silently leave the host's native voices exposed, so warn
    // loudly instead of dropping it quietly.
    if (!voice.is_object() || !voice.contains("lang") ||
        !voice.contains("name") || !voice.contains("voiceUri") ||
        !voice.contains("isDefault") || !voice.contains("isLocalService")) {
      printf_stderr(
          "ERROR: 'voices' entry is not a complete object "
          "{lang,name,voiceUri,isDefault,isLocalService}; skipping: %s\n",
          voice.dump().c_str());
      continue;
    }

    voices.emplace_back(
        voice["lang"].get<std::string>(), voice["name"].get<std::string>(),
        voice["voiceUri"].get<std::string>(), voice["isDefault"].get<bool>(),
        voice["isLocalService"].get<bool>());
  }
  return voices;
}

}  // namespace MaskConfig
