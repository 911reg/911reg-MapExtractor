#pragma once
#include <string>
#include <vector>
namespace i18n {
struct Language { std::string code; std::wstring name; };
void load();
const std::vector<Language>& languages();
void select(size_t index);
size_t selected();
std::wstring tr(const std::string& key);
std::wstring format(const std::string& key, size_t first, size_t second);
}
