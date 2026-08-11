#include "pch.h"
#include "TextMatchUtils.h"

#include <algorithm>
#include <cctype>

namespace WoLNamesBlackedOut::Core::TextMatch {

std::string ToUtf8(const wchar_t* text)
{
	if (!text) {
		return {};
	}

	int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 0) {
		return {};
	}

	std::string out(static_cast<size_t>(len) - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
	return out;
}

std::string Trim(const std::string& text)
{
	size_t begin = 0;
	while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
		++begin;
	}

	size_t end = text.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
		--end;
	}

	return text.substr(begin, end - begin);
}

std::string Sanitize(const std::string& text)
{
	std::string out;
	out.reserve(text.size());
	for (unsigned char c : text) {
		if (std::isalnum(c) || c >= 0x80 || c == '-' || c == '_' || c == '\'' || c == ' ') {
			out.push_back(static_cast<char>(c));
		}
	}
	return Trim(out);
}

std::vector<std::string> SplitCsv(const std::string& csv)
{
	std::vector<std::string> out;
	size_t i = 0;

	while (i < csv.size()) {
		size_t j = csv.find(',', i);
		if (j == std::string::npos) {
			j = csv.size();
		}

		std::string token = Trim(csv.substr(i, j - i));
		token = Sanitize(token);
		if (!token.empty()) {
			out.push_back(token);
		}

		i = j + 1;
	}

	return out;
}

int LevenshteinDistance(const std::string& a, const std::string& b)
{
	const size_t n = a.size();
	const size_t m = b.size();

	if (n == 0) return static_cast<int>(m);
	if (m == 0) return static_cast<int>(n);

	std::vector<int> prev(m + 1);
	std::vector<int> cur(m + 1);
	for (size_t j = 0; j <= m; ++j) {
		prev[j] = static_cast<int>(j);
	}

	for (size_t i = 1; i <= n; ++i) {
		cur[0] = static_cast<int>(i);
		for (size_t j = 1; j <= m; ++j) {
			int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
			cur[j] = std::min({ prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost });
		}
		prev.swap(cur);
	}

	return prev[m];
}

double LevenshteinSimilarity(const std::string& a, const std::string& b)
{
	if (a.empty() && b.empty()) {
		return 1.0;
	}

	std::string a2 = a;
	std::string b2 = b;
	std::transform(a2.begin(), a2.end(), a2.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	std::transform(b2.begin(), b2.end(), b2.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	int max_len = static_cast<int>(std::max(a2.size(), b2.size()));
	if (max_len <= 0) {
		return 1.0;
	}

	int distance = LevenshteinDistance(a2, b2);
	return 1.0 - (static_cast<double>(distance) / static_cast<double>(max_len));
}

bool IsExcludedBySimilarity(const std::string& source_text, const std::vector<std::string>& exclude_texts, float threshold01)
{
	if (source_text.empty() || exclude_texts.empty()) {
		return false;
	}

	const std::string normalized = Sanitize(source_text);
	if (normalized.empty()) {
		return false;
	}

	const double threshold = std::clamp(static_cast<double>(threshold01), 0.50, 1.00);

	for (const auto& candidateRaw : exclude_texts) {
		const std::string candidate = Sanitize(candidateRaw);
		if (candidate.empty()) {
			continue;
		}

		if (_stricmp(normalized.c_str(), candidate.c_str()) == 0) {
			return true;
		}

		double sim = LevenshteinSimilarity(normalized, candidate);
		if (sim >= threshold) {
			return true;
		}
	}

	return false;
}

float PercentToSimilarityThreshold(float percent_50_to_100)
{
	const float pct = std::clamp(percent_50_to_100, 50.0f, 100.0f);
	return pct / 100.0f;
}

} // namespace WoLNamesBlackedOut::Core::TextMatch
