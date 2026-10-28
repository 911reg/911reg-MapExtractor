#include "cpu_workers.h"
#include "package.h"
#include "doors.h"
#include "localization.h"
#include "history.h"
#include "water.h"
#include "geodata.h"
#include "collision.h"
#include "map_selection.h"
#include "output_tasks.h"
#include "export_readme.h"
#include "ui_preferences.h"
#include "settings_file.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <mutex>
#include <cwchar>
#include <windows.h>

namespace fs = std::filesystem;
int showUi();
int verifyCompletion();
int verifyUi(const wchar_t* source, const wchar_t* destination, bool doors = false, bool water = false, bool geodata = false, bool simple = false, bool pathnodes = false);
int previewUi(const wchar_t* destination);
int verifyUiStudio(const wchar_t* directory);

static std::string json(const std::string& value) {
    std::string result = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4]; result += hex[c & 15];
        } else result += char(c);
    }
    return result + '"';
}

static std::string pathText(const fs::path& path) {
    const auto value = path.generic_u8string();
    return std::string(value.begin(), value.end());
}

static std::string jsonPath(const fs::path& path) {
    std::string result = "\"";
    for (char c : pathText(path)) {
        if (c == '"' || c == '\\') result += '\\';
        if (static_cast<unsigned char>(c) < 32) {
            result += json(std::string(1,c)).substr(1,6);
        } else result += c;
    }
    return result + '"';
}

static std::string inspect(const fs::path& path, bool& failed, std::string& reason) {
    std::ostringstream out;
    out << "{\"path\":" << jsonPath(path);
    try {
        const auto package = extractor::Package::open(path);
        std::map<std::string, size_t> classes;
        for (size_t i = 0; i < package.exports.size(); ++i) ++classes[package.className(i)];
        out << ",\"status\":\"ok\",\"version\":" << package.version
            << ",\"licensee\":" << package.licensee << ",\"envelope\":" << json(package.envelope)
            << ",\"imports\":" << package.imports.size() << ",\"exports\":" << package.exports.size()
            << ",\"classes\":{";
        bool first = true;
        for (const auto& [name, count] : classes) {
            if (!first) out << ',';
            first = false;
            out << json(name) << ':' << count;
        }
        out << '}';
    } catch (const std::exception& error) {
        failed = true;
        reason = error.what();
        out << ",\"status\":\"error\",\"error\":" << json(error.what());
    }
    return out.str() + '}';
}

static unsigned threads(const std::string& value) {
    unsigned result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < 1 || result > 64)
        throw std::runtime_error("--threads must be between 1 and 64");
    return result;
}

// No arguments opens the window. scan writes a JSON inventory and is not a window task.
// doors, water and geodata refuse an output path that already exists.
static int run(int argc, wchar_t** argv) {
    if (argc == 1) return showUi();
    if (argc==3 && std::wstring(argv[1])==L"--verify-map-selection") {
        for (const auto& name:{L"22_22.unr",L"22_22_Classic.unr",L"01_09_CLASSIC.unr"}) if (!extractor::regionMap(name)) throw std::runtime_error("Region map rejected");
        for (const auto& name:{L"sky.unr",L"lobby.unr",L"22_22_sky.unr",L"2_22.unr",L"122_22.unr"}) if (extractor::regionMap(name)) throw std::runtime_error("Non-region map accepted");
        const std::vector<fs::path> both{L"maps/22_22.unr",L"maps/22_22_Classic.unr",L"maps/23_22.unr",L"maps/24_22_Classic.unr"};
        for (bool classic:{false,true}) {
            auto selected=both; extractor::preferMapVariant(selected,classic);
            if (selected.size()!=3 || std::find(selected.begin(),selected.end(),both[classic ? 0 : 1])!=selected.end()) throw std::runtime_error("Map preference removed wrong maps");
            extractor::preferMapVariant(selected,classic); if (selected.size()!=3) throw std::runtime_error("Map preference is not stable");
        }
        const auto fixture=fs::absolute(argv[2]);
        if (fs::exists(fixture)) throw std::runtime_error("Map selection verification needs a new folder");
        fs::create_directories(fixture);
        for (const auto& name:{L"22_22.unr",L"22_22_Classic.unr",L"23_22.unr",L"sky.unr",L"lobby.unr"}) { std::ofstream file(fixture/name); file<<"selection fixture"; }
        extractor::allVariants=true;
        if (extractor::selectMaps(fixture,false).size()!=3) throw std::runtime_error("All-variant selection included non-region maps");
        extractor::allVariants=false;
        if (extractor::selectMaps(fixture,false).size()!=2 || extractor::selectMaps(fixture,true).size()!=2) throw std::runtime_error("Preferred selection included non-region maps");
        bool rejected=false;
        try { extractor::selectMaps(fixture/L"sky.unr",false); } catch (const std::exception&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Explicit non-region map was accepted");
        extractor::requestedMaps={fixture/L"sky.unr"}; rejected=false;
        try { extractor::selectMaps(fixture,false); } catch (const std::exception&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Map list bypassed region filter");
        std::cout<<"Region names, directory/single/list filtering and paired map preferences passed\n"; return 0;
    }
    if (argc==2 && std::wstring(argv[1])==L"--verify-completion") return verifyCompletion();
    if (argc==3 && std::wstring(argv[1])==L"--verify-preferences") {
        const auto path=fs::absolute(argv[2]); if (fs::exists(path)) throw std::runtime_error("Preference verification needs a new file");
        fs::create_directories(path.parent_path());
        WritePrivateProfileStringW(L"Recent",L"Count",L"9",path.c_str());
        for (const auto& profile:extractor::serverProfiles) {
            const extractor::UiPreferences wanted{profile,false,true,"l2off",false};
            if (!wanted.save(path)) throw std::runtime_error("Preferences could not be saved");
            const auto loaded=extractor::UiPreferences::load(path);
            if (loaded.server!=profile || loaded.combine || !loaded.debug || loaded.geodata!="l2off" || loaded.pathnodes || GetPrivateProfileIntW(L"Recent",L"Count",0,path.c_str())!=9) throw std::runtime_error("Preference round trip failed");
        }
        WritePrivateProfileStringW(L"UI",L"ServerType",L"invalid",path.c_str());
        if (extractor::UiPreferences::load(path).server!="l2jserver") throw std::runtime_error("Invalid server preference was not rejected");
        const auto migration=path.parent_path()/L"migration";
        fs::create_directory(migration);
        const auto legacy=migration/L"911reg.ini";
        const auto current=migration/L"settings.ini";
        const std::string original="[UI]\nLanguage=es\nServerType=acis\n[Recent]\nCount=9\n";
        { std::ofstream file(legacy,std::ios::binary); file<<original; }
        if (extractor::settingsFile(migration)!=current || fs::exists(legacy)) throw std::runtime_error("Settings migration failed");
        std::ifstream migrated(current,std::ios::binary);
        const std::string actual((std::istreambuf_iterator<char>(migrated)),{});
        migrated.close();
        if (actual!=original) throw std::runtime_error("Migration changed saved settings");
        { std::ofstream file(legacy); file<<"older settings"; }
        if (extractor::settingsFile(migration)!=current || !fs::exists(legacy) || fs::file_size(current)!=original.size()) throw std::runtime_error("Existing settings were overwritten");
        std::cout<<"Server preferences and settings migration passed\n"; return 0;
    }
    if (argc==3 && std::wstring(argv[1])==L"--verify-output") {
        const auto root=fs::absolute(argv[2]);
        if (fs::exists(root)) throw std::runtime_error("Output verification requires a new fixture directory");
        fs::create_directories(root);
        auto need=[](bool condition) { if (!condition) throw std::runtime_error("Task output verification failed"); };
        for (unsigned mode=0;mode<4;++mode) {
            const auto target=extractor::taskOutput(root,mode);
            need(target.parent_path()==fs::canonical(root));
            if (mode==2) { std::ofstream file(target); file<<"inventory"; }
            else { fs::create_directories(target/"nested"); std::ofstream file(target/"nested"/"keep.txt"); file<<"result"; }
        }
        std::ofstream(root/"unrelated.txt")<<"keep";
        extractor::clearTaskOutput(root,3);
        need(!fs::exists(root/"Geodata") && fs::exists(root/"Doors"/"nested"/"keep.txt") && fs::exists(root/"Water"/"nested"/"keep.txt") && fs::exists(root/"unrelated.txt"));
        extractor::clearTaskOutput(root,3);
        extractor::clearTaskOutput(root,2);
        need(!fs::exists(root/"map-details.json") && fs::exists(root/"Doors"));
        bool rejected=false; try { extractor::clearTaskOutput(root,5); } catch (const std::exception&) { rejected=true; } need(rejected);
        rejected=false; try { extractor::clearTaskOutput({},0); } catch (const std::exception&) { rejected=true; } need(rejected);
        fs::create_directories(root/"External"); std::ofstream(root/"External"/"keep.txt")<<"external";
        if (CreateSymbolicLinkW((root/"Geodata").c_str(),(root/"External").c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
            rejected=false; try { extractor::clearTaskOutput(root,3); } catch (const std::exception&) { rejected=true; }
            need(rejected && fs::exists(root/"External"/"keep.txt"));
            std::cout<<"Linked output rejection passed\n";
        }
        std::cout<<"Direct output paths and scoped clearing passed\n"; return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--verify-collision") return extractor::collision::verify(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--inspect-collision") return extractor::collision::inspect(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--check-collision") {
        const auto scene=extractor::collision::read(argv[2]);
        std::cout << "terrain=" << scene.terrain << " bsp=" << scene.bsp << " mesh=" << scene.meshes << " volumes=" << scene.volumes << " errors=" << scene.errors.size() << '\n';
        for (const auto& error:scene.errors) std::cout << error << '\n'; return scene.errors.empty() ? 0 : 2;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--verify-geodata") return extractor::verifyGeodata();
    if (argc == 3 && std::wstring(argv[1]) == L"--dump-water") return extractor::dumpWater(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--verify-history") {
        const auto result = extractor::verifyHistory(argv[2]);
        std::cout << "Client history checks passed\n"; return result;
    }
    if (argc==3 && std::wstring(argv[1])==L"--verify-ui-studio") return verifyUiStudio(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--preview-ui") return previewUi(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--dump-doors") return extractor::dumpDoors(argv[2]);
    if (argc == 2 && std::wstring(argv[1]) == L"--verify-core") return extractor::verifyDoorCore();
    if (argc == 4 && std::wstring(argv[1]) == L"--verify-ui") return verifyUi(argv[2],argv[3]);
    if (argc == 4 && std::wstring(argv[1]) == L"--verify-ui-doors") return verifyUi(argv[2],argv[3],true);
    if (argc == 4 && std::wstring(argv[1]) == L"--verify-ui-water") return verifyUi(argv[2],argv[3],false,true);
    if (argc==4 && std::wstring(argv[1])==L"--verify-ui-pathnodes") return verifyUi(argv[2],argv[3],false,false,false,true,true);
    if (argc==4 && std::wstring(argv[1])==L"--verify-ui-simple-geodata") return verifyUi(argv[2],argv[3],false,false,true,true);
    if (argc==4 && std::wstring(argv[1])==L"--verify-ui-simple-doors") return verifyUi(argv[2],argv[3],true,false,false,true);
    if (argc == 4 && std::wstring(argv[1]) == L"--verify-ui-geodata") return verifyUi(argv[2],argv[3],false,false,true);
    if (argc == 2 && std::wstring(argv[1]) == L"--verify-water-core") return extractor::verifyWaterCore();
    if (argc == 2 && std::wstring(argv[1]) == L"--verify-languages") {
        i18n::load(); const auto original = i18n::selected();
        for (size_t i = 0; i < i18n::languages().size(); ++i) {
            i18n::select(i);
            for (const auto& key : {"subtitle","client_label","output_label","task_doors","task_water","water_finished","task_scan","done","partial","failed","server_type","settings_error","open_output_prompt","result_doors","result_water","result_geodata","result_scan","skipped_reasons","result_ready","maps_prefer_normal","maps_prefer_classic","tip_prefer_normal","tip_prefer_classic","tile_pathnodes","include_pathnodes","result_pathnodes","tip_pathnodes","node_format","pn_hint"})
                if (i18n::tr(key).empty()) throw std::runtime_error("Empty translation");
            if (i18n::format("progress",1,2).find(L"{") != std::wstring::npos) throw std::runtime_error("Translation placeholder failure");
        }
        i18n::select(original);
        std::cout << "Language checks passed: " << i18n::languages().size() << '\n'; return 0;
    }
    if (std::wstring(argv[1]) == L"--help") {
        std::cout << "911reg MapExtractor 0.8.0 | CPU only\n"
            << "MapExtractor scan <client-folder-or-map.unr> --output <manifest.json> [--threads 1..64]\n"
            << "MapExtractor doors <client-folder-or-map.unr> --output <new-folder> [--threads 1..64]\n"
            << "MapExtractor water <client-folder-or-map.unr> --output <new-folder> [--threads 1..64]\n"
            << "MapExtractor pathnodes <client-folder-or-map.unr> --output <new-folder> [--threads 1..64]\n"
            << "Geodata includes Pathnodes/*.pn. --no-pathnodes disables them. --world-heights disables client surface offsets for diagnostics.\n"
            << "MapExtractor geodata <client-folder-or-map.unr> --output <new-folder> [--threads 1..64] [--classic]\n"
            << "Door and water: four XML profiles plus L2OFF script fragments. --classic prefers Classic maps with normal output names.\n"
            << "All native tasks accept --map-list <UTF-8-file> with one absolute UNR path per line to process only those maps.\n"
            << "--all-variants exports normal and Classic maps separately. Doors/water accept --server <all|l2jserver|acis|l2scripts|mobius|l2off> and --combine-xml.\n"
            << "The window label L2jMabios is the mobius profile. --simple-output omits debug reports and flattens single-server door/water output.\n"
            << "Geodata reads native UNR terrain, BSP, static-mesh and blocking-volume collision. --geo <l2j|both|l2off> chooses L2J *.l2j (default), both, or L2OFF *_conv.dat. Same cells, four directions.\n";
        return 0;
    }
    const bool doors = std::wstring(argv[1]) == L"doors";
    const bool water = std::wstring(argv[1]) == L"water";
    const bool fixture = std::wstring(argv[1]) == L"geodata-fixture";
    extractor::pathnodesOnly=std::wstring(argv[1])==L"pathnodes";
    const bool geodata = std::wstring(argv[1]) == L"geodata" || fixture || extractor::pathnodesOnly;
    if ((!doors && !water && !geodata && std::wstring(argv[1]) != L"scan") || argc < 5)
        throw std::runtime_error("Expected scan <directory> --output <manifest.json> [--threads N]");
    fs::path root = fs::canonical(argv[2]);
    fs::path singleMap;
    if (fs::is_regular_file(root) && (geodata || _wcsicmp(root.extension().c_str(),L".unr") == 0)) {
        singleMap = root; root = root.parent_path();
    }
    if (!fs::is_directory(root)) throw std::runtime_error("Scan input must be a directory");
    if (fs::is_directory(root / "Maps")) root /= "Maps";
    else if (root.filename() == L"System" && fs::is_directory(root.parent_path() / "Maps"))
        root = root.parent_path() / "Maps";
    fs::path output;
    bool classic = false;
    unsigned workers = extractor::defaultWorkers();
    for (int i = 3; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--exact-heights") { extractor::compactGeodata=false; continue; }
        if (option == L"--no-pathnodes") { extractor::emitPathnodes=false; continue; }
        if (option == L"--world-heights") { extractor::retailHeightOffsets=false; continue; }
        if (option == L"--retail-heights") { extractor::retailHeightOffsets=true; continue; }
        if (option == L"--classic") { classic = true; continue; }
        if (option == L"--all-variants") { extractor::allVariants=true; continue; }
        if (option == L"--simple-output") { extractor::debugFiles=false; continue; }
        if (option == L"--combine-xml") { extractor::combineXml=true; continue; }
        if (++i >= argc) throw std::runtime_error("Missing option value");
        if (option == L"--output" && output.empty()) output = fs::absolute(argv[i]);
        else if (option == L"--threads") workers = threads(pathText(fs::path(argv[i])));
        else if (option == L"--geo") {
            extractor::geodataFormat=pathText(fs::path(argv[i]));
            if (extractor::geodataFormat!="both" && extractor::geodataFormat!="l2j" && extractor::geodataFormat!="l2off") throw std::runtime_error("--geo must be both, l2j, or l2off");
        }
        else if (option == L"--server") {
            extractor::targetProfile=pathText(fs::path(argv[i]));
            if (extractor::targetProfile!="all" && extractor::targetProfile!="l2jserver" && extractor::targetProfile!="acis" && extractor::targetProfile!="l2scripts" && extractor::targetProfile!="mobius" && extractor::targetProfile!="l2off") throw std::runtime_error("Unknown server profile");
        }
        else if (option == L"--map-list" && extractor::requestedMaps.empty()) {
            std::ifstream list(fs::path(argv[i]),std::ios::binary);
            if (!list) throw std::runtime_error("Cannot read selected map list");
            std::string line;
            while (std::getline(list,line)) {
                if (!line.empty() && line.back()=='\r') line.pop_back();
                if (!line.empty()) extractor::requestedMaps.push_back(fs::canonical(fs::path(std::u8string(line.begin(),line.end()))));
            }
            if (extractor::requestedMaps.empty()) throw std::runtime_error("Selected map list is empty");
        }
        else throw std::runtime_error("Unknown or duplicate option");
    }
    if (output.empty()) throw std::runtime_error("--output is required");
    if (extractor::pathnodesOnly) {
        const bool geoFile=!singleMap.empty() && extractor::folded(singleMap.extension().wstring())==L".l2j";
        const bool geoFolder=singleMap.empty() && std::none_of(std::filesystem::directory_iterator(root),std::filesystem::directory_iterator(),[](const auto& entry) { return extractor::folded(entry.path().extension().wstring())==L".unr"; });
        if (geoFile || geoFolder) return extractor::generatePathnodes(singleMap.empty() ? root : singleMap,output);
    }
    if (geodata) {
        if (!singleMap.empty() && extractor::folded(singleMap.extension().wstring()) != (fixture ? L".obj" : L".unr")) throw std::runtime_error("Geodata reads UNR maps directly; select a client folder or map.unr");
        return extractor::generateGeodata(singleMap.empty() ? root : singleMap,output,workers,classic,fixture);
    }
    if (doors) return extractor::exportDoors(singleMap.empty() ? root : singleMap,output,workers,classic);
    if (water) return extractor::exportWater(singleMap.empty() ? root : singleMap,output,workers,classic);
    if (fs::exists(output)) throw std::runtime_error("Output exists; choose a new manifest path");
    auto paths = extractor::selectMaps(singleMap.empty() ? root : singleMap,classic,false);
    if (paths.empty()) throw std::runtime_error("No .unr maps found");
    workers = static_cast<unsigned>(std::min<size_t>(workers, paths.size()));
    std::vector<std::string> records(paths.size());
    std::atomic<size_t> next{0}, errors{0};
    std::mutex progress;
    size_t completed = 0;
    std::cerr << "Found " << paths.size() << " maps. Checking files...\n";
    {
        std::vector<std::jthread> pool;
        for (unsigned i = 0; i < workers; ++i) pool.emplace_back([&] {
            for (;;) {
                const size_t index = next.fetch_add(1);
                if (index >= paths.size()) break;
                { std::lock_guard lock(progress); std::cerr << "MAP " << pathText(paths[index].filename()) << " | package inventory\n"; }
                bool failed = false;
                std::string reason;
                records[index] = inspect(paths[index], failed, reason);
                if (failed) ++errors;
                std::lock_guard lock(progress);
                if (failed) std::cerr << "SKIPPED: " << pathText(paths[index].filename()) << ": " << reason << '\n';
                std::cerr << "Checked " << ++completed << " of " << paths.size() << " maps\n";
            }
        });
    }
    fs::create_directories(output.parent_path());
    std::ofstream out(output, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create manifest");
    out << "{\"tool\":\"911reg MapExtractor\",\"schema\":1,\"stage\":\"inventory\",\"root\":"
        << jsonPath(root) << ",\"map_count\":" << paths.size() << ",\"error_count\":" << errors.load()
        << ",\"maps\":[\n";
    for (size_t i = 0; i < records.size(); ++i) out << (i ? ",\n" : "") << records[i];
    out << "\n]}\n";
    out.close();
    if (!out) throw std::runtime_error("Manifest write failed");
    extractor::writeExportReadme(output.parent_path(),"MAPS");
    std::cerr<<"RESULT scan "<<paths.size()-errors.load()<<' '<<errors.load()<<" 0 0 0\n";
    std::cerr << "Finished: " << paths.size() << " maps checked. " << errors.load() << " need attention.\n";
    return errors.load() ? 2 : 0;
}

// A double-clicked window has no console. A command typed in an existing one reuses that console.
// Redirected pipes and files are left alone so scripts can still capture the output.
static void useParentConsole() {
    const auto existing = GetStdHandle(STD_OUTPUT_HANDLE);
    const auto kind = existing == nullptr || existing == INVALID_HANDLE_VALUE ? FILE_TYPE_UNKNOWN : GetFileType(existing);
    if (kind == FILE_TYPE_PIPE || kind == FILE_TYPE_DISK) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    std::cout.clear();
    std::cerr.clear();
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) useParentConsole();
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "ERROR: " << error.what() << '\n'; return 1; }
}
