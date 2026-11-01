#include "events.h"
#include "package.h"
#include "collision.h"
#include "map_selection.h"
#include "export_readme.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace extractor {
namespace {
struct Event { int32_t id=0;unsigned command=0; };
using Catalog=std::map<int32_t,std::vector<std::string>>;
std::vector<Event> readEvents(const Package& package,const Property& property) {
    if(property.kind!=9)throw std::runtime_error("L2GameEvent is not an array");
    Reader reader{property.value};const auto count=reader.compact();
    if(count<0 || count>100000)throw std::runtime_error("Invalid L2GameEvent count");
    std::vector<Event> result;
    for(int32_t i=0;i<count;++i) {
        Event event;
        for(const auto& field:package.tags(reader)) {
            Reader value{field.value};
            if(field.name=="EventID") {
                if(field.kind!=2 || field.value.size()!=4)throw std::runtime_error("Invalid EventID");
                event.id=int32_t(value.word());
            } else if(field.name=="EventCmd") {
                if(field.kind!=1 || field.value.size()!=1)throw std::runtime_error("Invalid EventCmd");
                event.command=value.byte();
            }
        }
        result.push_back(event);
    }
    if(reader.offset!=property.value.size())throw std::runtime_error("Trailing L2GameEvent data");
    return result;
}
std::string quoted(const std::string& text) {
    std::string result="\"";
    for(const unsigned char c:text) {
        if(c=='"' || c=='\\'){result+='\\';result+=char(c);}
        else if(c=='\n')result+="\\n";
        else if(c=='\r')result+="\\r";
        else if(c=='\t')result+="\\t";
        else if(c>=32)result+=char(c);
    }
    return result+'"';
}
std::string nameValue(const Package& package,const Property& property) {
    if(property.kind!=6)throw std::runtime_error("Invalid actor name property");
    Reader reader{property.value};const auto result=package.name(reader.compact());
    if(reader.offset!=property.value.size())throw std::runtime_error("Trailing actor name data");
    return result;
}
std::string catalogText(const std::string& map,const Catalog& catalog) {
    if(catalog.empty())return {};
    size_t count=0;for(const auto& [id,entries]:catalog)count+=entries.size();
    std::string result="Map = "+quoted(map)+"\r\nEvent IDs = "+std::to_string(catalog.size())+"\r\nEntries = "+std::to_string(count)+"\r\n\r\n";
    for(const auto& [id,entries]:catalog) {
        result+="Event ID = "+std::to_string(id)+"\r\n";
        for(const auto& entry:entries)result+=entry+"\r\n";
    }
    return result;
}
std::string eventName(const std::string& actor,const std::string& tag,const std::string& group) {
    std::string result=tag.empty() || tag=="None" ? actor : tag;
    std::istringstream groups(group);std::string part;
    while(std::getline(groups,part,',')) {
        const auto first=part.find_first_not_of(" \t\r\n");if(first==std::string::npos)continue;
        part=part.substr(first,part.find_last_not_of(" \t\r\n")-first+1);
        if(part!="None")result+="_"+part;
    }
    return result;
}
}
int exportEvents(const std::filesystem::path& input,const std::filesystem::path& destination,unsigned workers,bool classic) {
    namespace fs=std::filesystem;
    if(fs::exists(destination))throw std::runtime_error("Event output exists; choose a new folder");
    auto root=fs::canonical(input);if(fs::is_regular_file(root))root=root.parent_path();
    const auto maps=selectMaps(input,classic,false);
    fs::create_directories(destination);
    std::atomic<size_t> next{0},total{0},rejected{0},failed{0};
    std::vector<std::string> notes(maps.size());std::mutex progress;size_t completed=0;
    workers=unsigned(std::min<size_t>(std::max(1u,workers),maps.size()));
    {
        std::vector<std::jthread> pool;
        for(unsigned worker=0;worker<workers;++worker)pool.emplace_back([&] {
            for(;;) {
                const auto index=next.fetch_add(1);if(index>=maps.size())break;
                std::ostringstream report;Catalog catalog;size_t count=0;
                {std::lock_guard lock(progress);std::cerr<<"MAP "<<output::utf8(maps[index].filename())<<" | map events\n";}
                try {
                    const auto package=Package::open(maps[index]);
                    for(const auto actor:collision::liveActors(package)) {
                        try {
                            const auto properties=package.properties(actor);
                            if(std::any_of(properties.begin(),properties.end(),[](const auto& p){return p.name=="bDeleteMe" && p.kind==3 && p.boolean;}))continue;
                            const auto found=std::find_if(properties.begin(),properties.end(),[](const auto& p){return p.name=="L2GameEvent";});
                            if(found==properties.end())continue;
                            const auto events=readEvents(package,*found);if(events.empty())continue;
                            std::string tag,group;double x=0,y=0,z=0;
                            for(const auto& p:properties) {
                                if(p.name=="Tag")tag=nameValue(package,p);
                                else if(p.name=="Group")group=nameValue(package,p);
                                else if(p.name=="Location") {
                                    if(p.structure!="Vector" || p.value.size()!=12)throw std::runtime_error("Invalid event actor Location");
                                    Reader value{p.value};x=std::bit_cast<float>(value.word());y=std::bit_cast<float>(value.word());z=std::bit_cast<float>(value.word());
                                    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))throw std::runtime_error("Non-finite event actor Location");
                                }
                            }
                            const auto& name=package.exports[actor].name;
                            const auto label=eventName(name,tag,group);
                            for(const auto& event:events) {
                                std::ostringstream entry;entry<<std::setprecision(9);
                                entry<<"  Name = "<<quoted(label)<<"\r\n  Actor = "<<quoted(name)<<"\r\n  Command = "<<event.command
                                    <<"\r\n  Location = (X="<<x<<", Y="<<y<<", Z="<<z<<")\r\n";
                                catalog[event.id].push_back(entry.str());
                            }
                            count+=events.size();
                        } catch(const std::exception& error) {
                            ++rejected;report<<output::utf8(maps[index].filename())<<" / "<<package.exports[actor].name<<": "<<error.what()<<'\n';
                        }
                    }
                    if(!catalog.empty()) {
                        auto relative=mapOutput(maps[index],root);relative.replace_filename(relative.stem().wstring()+L"_Events.txt");
                        output::save(destination/relative,catalogText(output::utf8(maps[index].filename()),catalog));
                    } else {std::lock_guard lock(progress);std::cerr<<"No events: "<<output::utf8(maps[index].filename())<<" (no file written)\n";}
                    total+=count;
                } catch(const std::exception& error) {++failed;report<<output::utf8(maps[index].filename())<<": "<<error.what()<<'\n';}
                notes[index]=report.str();std::lock_guard lock(progress);std::cerr<<"Checked "<<++completed<<" of "<<maps.size()<<" maps\n";
            }
        });
    }
    for(const auto& note:notes){std::istringstream lines(note);std::string line;while(std::getline(lines,line))if(!line.empty())std::cerr<<"SKIPPED: "<<line<<'\n';}
    writeExportReadme(destination,"EVENTS");
    if(debugFiles){std::string report;for(const auto& note:notes)report+=note;output::save(destination/"911reg-event-report.txt",report);}
    std::cerr<<"RESULT events "<<total<<' '<<rejected<<' '<<failed<<" 0 0\n";
    return rejected || failed ? 2 : 0;
}
int verifyEvents() {
    Package package;package.names={"None","EventID","EventCmd"};
    Property property;property.kind=9;property.value={2,1,0x22,0x78,0x56,0x34,0x12,2,1,7,0,1,0x22,0x78,0x56,0x34,0x12,0};
    const auto events=readEvents(package,property);
    if(events.size()!=2 || events[0].id!=0x12345678 || events[0].command!=7 || events[1].id!=events[0].id || events[1].command!=0)throw std::runtime_error("Event array check failed");
    property.value.pop_back();bool rejected=false;
    try {readEvents(package,property);}catch(const std::exception&){rejected=true;}
    if(!rejected)throw std::runtime_error("Truncated event accepted");
    property.value={0};if(!readEvents(package,property).empty())throw std::runtime_error("Empty event array failed");
    property.value={1,1,0x22,255,255,255,255,2,1,255,0};
    const auto unknown=readEvents(package,property);
    if(unknown[0].id!=-1 || unknown[0].command!=255)throw std::runtime_error("Signed ID or unknown command lost");
    property.value.push_back(0);rejected=false;
    try {readEvents(package,property);}catch(const std::exception&){rejected=true;}
    if(!rejected || quoted("a\"\\\n")!="\"a\\\"\\\\\\n\"")throw std::runtime_error("Event trailing data or text escaping failed");
    if(!catalogText("empty",{}).empty())throw std::runtime_error("Empty catalog was rendered");
    if(eventName("Actor","Effect","None, smoke, None")!="Effect_smoke" || eventName("Actor","None","")!="Actor")throw std::runtime_error("Event display name contains empty groups");
    const auto grouped=catalogText("map",{{20,{"  Actor = second\r\n"}},{10,{"  Actor = first\r\n","  Actor = duplicate\r\n"}}});
    if(grouped.find("Event IDs = 2\r\nEntries = 3")==std::string::npos || grouped.find("Event ID = 10")>grouped.find("Event ID = 20") || grouped.find("duplicate")==std::string::npos)throw std::runtime_error("Grouped catalog ordering or entries failed");
    std::cout<<"Event checks passed: arrays, repeated IDs, default command, empty arrays and truncated input\n";return 0;
}
}
