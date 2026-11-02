#pragma once
#include <filesystem>
namespace extractor {
int exportT3d(const std::filesystem::path& input,const std::filesystem::path& output,unsigned workers,bool classic=false);
int verifyT3d();
}
