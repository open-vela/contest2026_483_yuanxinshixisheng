#include "voicelink/parsers.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace voicelink {
namespace {

void eraseAll(std::string& value, const std::string& needle) {
  for (std::size_t pos = value.find(needle); pos != std::string::npos;
       pos = value.find(needle)) {
    value.erase(pos, needle.size());
  }
}

const std::unordered_map<std::string, char> kSymbols{
    {"井号", '#'}, {"#", '#'},       {"星号", '*'}, {"*", '*'},
    {"下划线", '_'}, {"_", '_'},     {"横线", '-'}, {"减号", '-'},
    {"-", '-'},      {"感叹号", '!'}, {"!", '!'},   {"艾特", '@'},
    {"at", '@'},     {"@", '@'},      {"点号", '.'}, {"点", '.'},
    {".", '.'},      {"美元符号", '$'}, {"$", '$'}, {"百分号", '%'},
    {"%", '%'},      {"加号", '+'},  {"+", '+'},
    {"反斜杠", '\\'}, {"反斜线", '\\'}, {"\\", '\\'},
    {"正斜杠", '/'}, {"斜杠", '/'}, {"/", '/'}};

const std::unordered_map<std::string, PasswordControl> kControls{
    {"完成", PasswordControl::Finish},
    {"输入完成", PasswordControl::Finish},
    {"control_finish", PasswordControl::Finish},
    {"删除", PasswordControl::DeleteLast},
    {"删除一个", PasswordControl::DeleteLast},
    {"退格", PasswordControl::DeleteLast},
    {"清空", PasswordControl::Clear},
    {"重新输入", PasswordControl::Restart},
    {"重输", PasswordControl::Restart},
    {"取消", PasswordControl::Cancel},
    {"取消配网", PasswordControl::Cancel},
    {"当前长度", PasswordControl::ReportLength},
    {"重复当前长度", PasswordControl::ReportLength},
    {"切换大写", PasswordControl::SwitchUpper},
    {"切换小写", PasswordControl::SwitchLower},
    {"大写模式", PasswordControl::SwitchUpper},
    {"小写模式", PasswordControl::SwitchLower}};

char chineseDigit(const std::string& text) {
  static const std::unordered_map<std::string, char> digits{
      {"零", '0'}, {"一", '1'}, {"二", '2'}, {"三", '3'}, {"四", '4'},
      {"五", '5'}, {"六", '6'}, {"七", '7'}, {"八", '8'}, {"九", '9'}};
  const auto found = digits.find(text);
  return found == digits.end() ? '\0' : found->second;
}

}  // namespace

std::string normalize(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (unsigned char ch : text) {
    if (ch < 0x80) {
      if (!std::isspace(ch)) result.push_back(static_cast<char>(std::tolower(ch)));
    } else {
      result.push_back(static_cast<char>(ch));
    }
  }
  for (const auto* punctuation : {"，", "。", "、", "！", "？", "；", "：", "“", "”", "《", "》", "（", "）", "～"}) {
    eraseAll(result, punctuation);
  }
  return result;
}

namespace {

// 唤醒短语折叠：先 normalize()（ASCII 转小写、去空白、去常用中文标点），
// 再丢掉所有 ASCII 标点，最后补删 normalize() 未覆盖的全角符号。
// ASR 常输出半角逗号/句点（"Hello, openvela"），不折叠会漏唤醒。
// 注意：不改 normalize() 本身，密码解析依赖其中的 '.'、'#' 等符号语义。
std::string foldWakePhrase(const std::string& text) {
  const std::string normalized = normalize(text);
  std::string folded;
  folded.reserve(normalized.size());
  for (unsigned char ch : normalized) {
    if (ch < 0x80) {
      if (std::isalnum(ch) != 0) folded.push_back(static_cast<char>(ch));
    } else {
      folded.push_back(static_cast<char>(ch));
    }
  }
  for (const auto* punctuation :
       {"…", "·", "「", "」", "『", "』", "【", "】", "〈", "〉"}) {
    eraseAll(folded, punctuation);
  }
  return folded;
}

// 全等比较，或剥掉一层礼貌前缀后全等比较。大赛唤醒词是「礼貌前缀 + 品牌名」
// 的结构，前缀可选、品牌名必须完整，所以「hello」「你好」单独出现不会唤醒，
// 「小信」「openvela123」也不会唤醒。
bool matchesWakePhrase(const std::string& actual, const std::string& wanted) {
  if (actual.empty() || wanted.empty()) return false;
  // Explicit user-selected wake prefix. This parser is used only while
  // awaiting wake; never extend network/password/confirmation grammars.
  if (wanted == "你好") return actual.compare(0, wanted.size(), wanted) == 0;
  if (actual == wanted) return true;
  static const std::vector<std::string> kPrefixes{
      "你好", "您好", "嗨", "嘿", "哈喽", "哈啰", "hello", "hi", "hey"};
  for (const auto& prefix : kPrefixes) {
    if (actual.size() <= prefix.size()) continue;
    if (actual.compare(0, prefix.size(), prefix) != 0) continue;
    if (actual.compare(prefix.size(), std::string::npos, wanted) == 0) return true;
  }
  return false;
}

}  // namespace

bool isWakeWord(const std::string& text, const std::vector<std::string>& configured) {
  const std::string actual = foldWakePhrase(text);
  if (actual.empty()) return false;
  for (const auto& candidate : configured) {
    if (matchesWakePhrase(actual, foldWakePhrase(candidate))) return true;
  }
  return false;
}

bool isWakeWord(const std::string& text, const std::string& configured) {
  const std::string wanted = foldWakePhrase(configured);
  const std::string actual = foldWakePhrase(text);
  return matchesWakePhrase(actual, wanted);
}

std::optional<int> parseNetworkIndex(const std::string& text) {
  auto value = normalize(text);
  if (value.compare(0, std::string("选择").size(), "选择") == 0)
    value.erase(0, std::string("选择").size());
  else if (value.compare(0, std::string("选").size(), "选") == 0)
    value.erase(0, std::string("选").size());
  static const std::vector<std::vector<std::string>> aliases{
      {"1", "一", "第一", "第一个", "网络一", "网络1"},
      {"2", "二", "两", "第二", "第二个", "网络二", "网络两", "网络2"},
      {"3", "三", "第三", "第三个", "网络三", "网络3"},
      {"4", "四", "第四", "第四个", "网络四", "网络4"},
      {"5", "五", "第五", "第五个", "网络五", "网络5"}};
  for (std::size_t i = 0; i < aliases.size(); ++i) {
    if (std::find(aliases[i].begin(), aliases[i].end(), value) != aliases[i].end())
      return static_cast<int>(i + 1);
  }
  return std::nullopt;
}

bool isRescan(const std::string& text) {
  static const std::unordered_set<std::string> values{"重新扫描", "再扫描", "扫描网络"};
  return values.count(normalize(text)) != 0;
}

bool isCancel(const std::string& text) {
  const auto value = normalize(text);
  return value == "取消" || value == "取消配网";
}

bool isNetworkRestart(const std::string& text) {
  const auto value = normalize(text);
  return isRescan(value) || value == "重新选择网络" || value == "重新配网" || value == "更换网络";
}

bool isSubmitConfirmation(const std::string& text) {
  const auto value = normalize(text);
  return value == "确认" || value == "确认提交";
}

bool isPasswordRetry(const std::string& text) {
  const auto value = normalize(text);
  return value == "否" || value == "不确认" || value == "重新输入" || value == "重输";
}

std::optional<PasswordToken> parsePasswordToken(const std::string& text) {
  auto value = normalize(text);
  if (value.empty()) return std::nullopt;
  if (const auto found = kControls.find(value); found != kControls.end())
    return PasswordToken{std::nullopt, found->second};
  if (const auto found = kSymbols.find(value); found != kSymbols.end())
    return PasswordToken{found->second, std::nullopt};

  if (value.rfind("upper_", 0) == 0 && value.size() == 7 && std::isalpha(static_cast<unsigned char>(value[6])))
    return PasswordToken{static_cast<char>(std::toupper(static_cast<unsigned char>(value[6]))), std::nullopt, true};
  if (value.rfind("lower_", 0) == 0 && value.size() == 7 && std::isalpha(static_cast<unsigned char>(value[6])))
    return PasswordToken{static_cast<char>(std::tolower(static_cast<unsigned char>(value[6]))), std::nullopt, true};
  if (value.rfind("digit_", 0) == 0 && value.size() == 7 && std::isdigit(static_cast<unsigned char>(value[6])))
    return PasswordToken{value[6], std::nullopt};

  for (const std::string prefix : {"大写字母", "大写", "小写字母", "小写"}) {
    if (value.rfind(prefix, 0) == 0 && value.size() == prefix.size() + 1) {
      const unsigned char raw = static_cast<unsigned char>(value.back());
      if (!std::isalpha(raw)) return std::nullopt;
      const bool upper = prefix.rfind("大写", 0) == 0;
      return PasswordToken{static_cast<char>(upper ? std::toupper(raw) : std::tolower(raw)), std::nullopt, true};
    }
  }
  if (value.rfind("数字", 0) == 0) value.erase(0, std::string("数字").size());
  if (value.size() == 1 && std::isdigit(static_cast<unsigned char>(value[0])))
    return PasswordToken{value[0], std::nullopt};
  if (const char digit = chineseDigit(value); digit != '\0')
    return PasswordToken{digit, std::nullopt};
  if (value.size() == 1 && std::isalpha(static_cast<unsigned char>(value[0])))
    return PasswordToken{static_cast<char>(std::tolower(static_cast<unsigned char>(value[0]))), std::nullopt};
  return std::nullopt;
}

}  // namespace voicelink
