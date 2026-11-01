#pragma once
#include <filesystem>
namespace extractor {
int exportEvents(const std::filesystem::path& input,const std::filesystem::path& output,unsigned workers,bool classic=false);
int verifyEvents();
}
