#pragma once
#include <filesystem>
#include <string>
namespace extractor {
// Writes L2J/*.l2j and L2OFF/*_conv.dat. classic prefers XX_YY_Classic.unr. fixture reads an OBJ, not a client map.
int generateGeodata(const std::filesystem::path& source,const std::filesystem::path& output,unsigned workers,bool classic=false,bool fixture=false);
// Fixed triangle scene used by VERIFY-GEODATA. It does not read a map.
int verifyGeodata();
int generatePathnodes(const std::filesystem::path& input,const std::filesystem::path& output);
}
