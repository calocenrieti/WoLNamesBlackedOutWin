#pragma once

#include <string>
#include <vector>

namespace WoLNamesBlackedOut::Core::TextMatch {

std::string ToUtf8(const wchar_t* text);
std::string Trim(const std::string& text);
std::string Sanitize(const std::string& text);
std::vector<std::string> SplitCsv(const std::string& csv);

int LevenshteinDistance(const std::string& a, const std::string& b);
double LevenshteinSimilarity(const std::string& a, const std::string& b);
bool IsExcludedBySimilarity(const std::string& source_text, const std::vector<std::string>& exclude_texts, float threshold01);

float PercentToSimilarityThreshold(float percent_50_to_100);

} // namespace WoLNamesBlackedOut::Core::TextMatch
