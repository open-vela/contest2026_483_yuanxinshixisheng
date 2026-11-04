#pragma once

#include "voicelink/types.hpp"

#include <optional>
#include <string>
#include <vector>

namespace voicelink {

std::string normalize(const std::string& text);
// 唤醒判定：text 折叠（normalize + 去 ASCII 标点）后，与任一候选词全等，
// 或等于「礼貌前缀 + 候选词」（你好/您好/嗨/嘿/哈喽/hello/hi/hey）即命中。
// 用户配置「你好」时允许它作为前缀；「开始联网」仍按完整短语匹配。
// 锁定的板端模型会将前者稳定输出为精确短句「你好 open」，该完整短句也命中；
// 裸「open」、裸「你好」及带额外后缀的近似文本仍拒绝。
bool isWakeWord(const std::string& text, const std::vector<std::string>& configured);
// 单候选词便捷重载，等价于只配置一个词的 vector 版本。
bool isWakeWord(const std::string& text, const std::string& configured);
std::optional<int> parseNetworkIndex(const std::string& text);
bool isRescan(const std::string& text);
bool isCancel(const std::string& text);
bool isNetworkRestart(const std::string& text);
bool isSubmitConfirmation(const std::string& text);
bool isPasswordRetry(const std::string& text);
std::optional<PasswordToken> parsePasswordToken(const std::string& text);

}  // namespace voicelink
