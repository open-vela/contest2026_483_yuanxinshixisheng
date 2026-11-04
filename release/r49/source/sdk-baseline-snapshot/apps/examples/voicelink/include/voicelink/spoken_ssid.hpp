#pragma once
#include <string>
namespace voicelink {
// This Chinese voice has no English lexicon. Spell ASCII SSIDs explicitly;
// this presentation text is never used as the actual Wi-Fi identifier.
inline std::string spokenSsid(const std::string& ssid) {
  static const char* letters[]={"艾","比","西","迪","伊","艾夫","吉","艾尺","爱","杰","凯","艾勒","艾姆","恩","欧","皮","丘","阿尔","艾丝","踢","优","维","达不溜","艾克斯","歪","贼德"};
  static const char* digits[]={"零","一","二","三","四","五","六","七","八","九"};
  std::string out;
  for (unsigned char c : ssid) {
    if (c>='A' && c<='Z') { out+="大写";out+=letters[c-'A']; }
    else if(c>='a' && c<='z') out+=letters[c-'a'];
    else if(c>='0' && c<='9') out+=digits[c-'0'];
    else if(c=='_') out+="下划线";
    else if(c=='-') out+="横杠";
    else if(c=='.') out+="点";
    else if(c==' ') out+="空格";
    else if(c<128) out+="符号";
    else out+=char(c);
  }
  return out;
}
}
