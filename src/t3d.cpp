#include "t3d.h"
#include "package.h"
#include "map_selection.h"
#include "export_readme.h"
#include <atomic>
#include <bit>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <windows.h>

namespace extractor {
namespace {
std::string lower(std::string value){for(auto& c:value)if(c>='A' && c<='Z')c+=32;return value;}
std::string leaf(const std::string& value){return value.substr(value.find_last_of('.')+1);}
std::string quote(const std::string& value) {
    std::string result="\"";
    for(const char c:value){if(c=='"' || c=='\\')result+='\\';if(c=='\n')result+="\\n";else if(c=='\r')result+="\\r";else if(c=='\t')result+="\\t";else result+=c;}
    return result+'"';
}
std::string number(float value) {
    if(!std::isfinite(value))throw std::runtime_error("Non-finite T3D float");
    std::ostringstream text;text.imbue(std::locale::classic());text<<std::setprecision(std::numeric_limits<float>::max_digits10)<<value;
    auto result=text.str();if(result.find_first_of(".eE")==std::string::npos)result+=".0";return result;
}
size_t count(Reader& reader,size_t limit=1000000){const auto n=reader.compact();if(n<0 || size_t(n)>limit)throw std::runtime_error("T3D array exceeds limit");return size_t(n);}
std::string stringValue(Reader& reader) {
    const auto length=reader.compact();if(length==0)return {};
    if(length==INT32_MIN || std::abs(length)>1048576)throw std::runtime_error("Invalid T3D string length");
    std::wstring wide;std::string narrow;
    for(int i=0;i<std::abs(length);++i){const auto c=reader.word(length<0 ? 2 : 1);if(i==std::abs(length)-1){if(c)throw std::runtime_error("Unterminated T3D string");}else if(length<0)wide+=wchar_t(c);else narrow+=char(c);}
    if(length>0){const auto n=MultiByteToWideChar(1252,0,narrow.data(),int(narrow.size()),nullptr,0);wide.resize(n);MultiByteToWideChar(1252,0,narrow.data(),int(narrow.size()),wide.data(),n);}
    const auto n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),int(wide.size()),nullptr,0,nullptr,nullptr);if(!n && !wide.empty())throw std::runtime_error("Invalid T3D Unicode string");
    std::string result(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),int(wide.size()),result.data(),n,nullptr,nullptr);return result;
}
struct Ref {std::shared_ptr<const Package> package;std::string module;size_t index=0;};
class Packages {
    std::filesystem::path system;
    std::map<std::string,std::filesystem::path> files;
    std::map<std::string,std::shared_ptr<const Package>> loaded;
    std::mutex mutex;
public:
    Packages()=default;
    explicit Packages(const std::filesystem::path& map) {
        auto folder=map.parent_path();
        if(lower(output::utf8(folder.filename()))=="maps")folder=folder.parent_path();
        system=folder/"System";
        if(!std::filesystem::is_directory(system))throw std::runtime_error("T3D requires the selected client's System folder for property types");
        for(const auto& file:std::filesystem::directory_iterator(system))if(file.is_regular_file() && lower(output::utf8(file.path().extension()))==".u")files.emplace(lower(output::utf8(file.path().stem())),file.path());
    }
    std::shared_ptr<const Package> get(const std::string& module) {
        std::lock_guard lock(mutex);const auto key=lower(module);
        if(const auto found=loaded.find(key);found!=loaded.end())return found->second;
        const auto found=files.find(key);if(found==files.end())throw std::runtime_error("Missing T3D class package: "+module+".u");
        auto package=std::make_shared<const Package>(Package::open(found->second));loaded.emplace(key,package);return package;
    }
    Ref resolve(const Ref& context,int32_t reference) {
        if(reference>0){if(size_t(reference)>context.package->exports.size())throw std::runtime_error("Invalid T3D local reference");return {context.package,context.module,size_t(reference-1)};}
        if(!reference)throw std::runtime_error("Missing T3D type reference");
        const auto path=context.package->objectPath(reference);const auto dot=path.find('.');if(dot==std::string::npos)throw std::runtime_error("Invalid T3D imported type: "+path);
        const auto module=path.substr(0,dot),object=lower(path.substr(dot+1));const auto package=get(module);
        for(size_t i=0;i<package->exports.size();++i)if(lower(package->objectPath(int32_t(i+1)))==object)return {package,module,i};
        throw std::runtime_error("Missing T3D type: "+path);
    }
};
struct Type {std::string kind;uint32_t flags=0,dimension=1;Ref definition;int32_t target=0;};
class Writer {
    friend int ::extractor::verifyT3d();
    Packages& packages;Ref map;
    std::map<std::pair<const Package*,size_t>,Type> types;
    std::set<size_t> active,emitted;
    std::map<std::tuple<const Package*,size_t,std::string>,Type> fields;
    Type type(const Ref& ref) {
        const auto key=std::pair{ref.package.get(),ref.index};if(const auto found=types.find(key);found!=types.end())return found->second;
        Type result;result.kind=leaf(ref.package->className(ref.index));result.definition=ref;
        size_t offset=0;ref.package->properties(ref.index,&offset);const auto& object=ref.package->exports[ref.index];Reader r{ref.package->data,offset,object.offset+object.size};
        r.compact();r.compact();result.dimension=r.word();result.flags=r.word();r.compact();if(result.flags&32)r.skip(2);
        if(!result.dimension || result.dimension>1000000)throw std::runtime_error("Invalid T3D property dimension");
        if(result.kind=="ArrayProperty" || result.kind=="StructProperty" || result.kind=="ByteProperty" || result.kind=="ObjectProperty" || result.kind=="ClassProperty")result.target=r.compact();
        types.emplace(key,result);return result;
    }
    Type field(Ref owner,const Property& property) {
        const auto& name=property.name;const auto key=std::tuple{owner.package.get(),owner.index,lower(name)};
        if(const auto found=fields.find(key);found!=fields.end())return found->second;
        const auto originalOwner=owner.package->exports[owner.index].name;
        std::set<std::pair<const Package*,size_t>> seen;
        for(unsigned depth=0;depth<64;++depth) {
            if(!seen.emplace(owner.package.get(),owner.index).second)throw std::runtime_error("Cyclic T3D class hierarchy");
            for(size_t i=0;i<owner.package->exports.size();++i){const auto& object=owner.package->exports[i];if(object.outer==int32_t(owner.index+1) && lower(object.name)==lower(name) && owner.package->className(i).ends_with("Property"))return fields.emplace(key,type({owner.package,owner.module,i})).first->second;}
            const auto parent=owner.package->exports[owner.index].super;if(!parent)break;owner=packages.resolve(owner,parent);
        }
        if(name=="ZoneRenderState" && (originalOwner=="TerrainLayer" || originalOwner=="DecorationLayer"))return fields.emplace(key,Type{"NativeIntArray"}).first->second;
        const std::map<unsigned,std::string> scalars{{1,"ByteProperty"},{2,"IntProperty"},{3,"BoolProperty"},{4,"FloatProperty"},{6,"NameProperty"},{13,"StrProperty"}};
        if(const auto found=scalars.find(property.kind);found!=scalars.end())return fields.emplace(key,Type{found->second}).first->second;
        throw std::runtime_error("Missing T3D property type: "+owner.module+"."+name);
    }
    std::string objectText(int32_t ref) {
        if(!ref)return "None";
        const auto& p=*map.package;
        if(ref>0)return leaf(p.className(size_t(ref-1)))+"'"+p.objectPath(ref)+"'";
        const auto index=size_t(-int64_t(ref)-1);if(index>=p.imports.size())throw std::runtime_error("Invalid T3D import");
        return leaf(p.imports[index].importClass)+"'"+p.objectPath(ref)+"'";
    }
    std::string value(Reader& r,const Type& t,unsigned depth,std::vector<size_t>& children) {
        if(depth>32)throw std::runtime_error("T3D property nesting exceeds limit");
        if(t.kind=="IntProperty")return std::to_string(int32_t(r.word()));
        if(t.kind=="Int64Property"){const uint64_t low=r.word(),high=r.word();return std::to_string(std::bit_cast<int64_t>(low|(high<<32)));}
        if(t.kind=="FloatProperty")return number(std::bit_cast<float>(r.word()));
        if(t.kind=="BoolProperty")return r.byte() ? "True" : "False";
        if(t.kind=="NameProperty")return quote(map.package->name(r.compact()));
        if(t.kind=="StrProperty" || t.kind=="StringProperty")return quote(stringValue(r));
        if(t.kind=="ByteProperty") {
            const auto n=r.byte();if(!t.target)return std::to_string(n);
            const auto enumeration=packages.resolve(t.definition,t.target);size_t offset=0;enumeration.package->properties(enumeration.index,&offset);const auto& object=enumeration.package->exports[enumeration.index];Reader names{enumeration.package->data,offset,object.offset+object.size};names.compact();names.compact();const auto size=count(names,256);
            if(n>=size)throw std::runtime_error("Unknown T3D enum value");for(unsigned i=0;i<n;++i)names.compact();return enumeration.package->name(names.compact());
        }
        if(t.kind=="ObjectProperty" || t.kind=="ClassProperty") {
            const auto ref=r.compact();if(ref>0 && (t.flags&8))children.push_back(size_t(ref-1));return objectText(ref);
        }
        if(t.kind=="StructProperty") {
            const auto structure=packages.resolve(t.definition,t.target);const auto name=structure.package->exports[structure.index].name;
            if(name=="Vector" || name=="Rotator" || name=="Color") {
                std::string result="(";const auto labels=name=="Rotator" ? std::vector<std::string>{"Pitch","Yaw","Roll"} : name=="Color" ? std::vector<std::string>{"B","G","R","A"} : std::vector<std::string>{"X","Y","Z"};
                for(const auto& label:labels){if(result.size()>1)result+=',';result+=label+'='+(name=="Color" ? std::to_string(r.byte()) : name=="Rotator" ? std::to_string(int32_t(r.word())) : number(std::bit_cast<float>(r.word())));}return result+')';
            }
            if(map.package->version<118)throw std::runtime_error("Legacy binary T3D struct is unsupported: "+name);
            return "("+properties(map.package->tags(r),structure,"",depth+1,children,true)+")";
        }
        if(t.kind=="ArrayProperty") {
            const auto size=count(r);auto inner=type(packages.resolve(t.definition,t.target));inner.flags|=t.flags&8;std::string result="(";
            for(size_t i=0;i<size;++i){if(i)result+=',';result+=value(r,inner,depth+1,children);}return result+')';
        }
        if(t.kind=="DelegateProperty"){const auto ref=r.compact();const auto name=map.package->name(r.compact());return ref ? map.package->objectPath(ref)+'.'+name : "None";}
        throw std::runtime_error("Unsupported T3D property type: "+t.kind);
    }
    std::string properties(const std::vector<Property>& properties,const Ref& owner,const std::string& indent,unsigned depth,std::vector<size_t>& children,bool inlineStruct=false) {
        std::string result;
        for(const auto& p:properties) {
            const auto t=field(owner,p);Reader r{p.value};
            auto append=[&](const std::string& key,const std::string& text){if(inlineStruct){if(!result.empty())result+=',';result+=key+'='+text;}else result+=indent+key+'='+text+"\r\n";};
            auto key=p.name;if(t.dimension>1 || p.element)key+='('+std::to_string(p.element)+')';
            if(p.kind==3){if(t.kind!="BoolProperty")throw std::runtime_error("T3D bool type mismatch: "+p.name);append(key,p.boolean ? "True" : "False");continue;}
            if(t.kind=="ArrayProperty" || t.kind=="NativeIntArray") {
                const auto size=count(r);auto inner=t.kind=="NativeIntArray" ? Type{"IntProperty"} : type(packages.resolve(t.definition,t.target));inner.flags|=t.flags&8;
                if(inlineStruct) {std::string values="(";for(size_t i=0;i<size;++i){if(i)values+=',';values+=value(r,inner,depth+1,children);}append(key,values+')');}
                else for(size_t i=0;i<size;++i)append(p.name+'('+std::to_string(i)+')',value(r,inner,depth+1,children));
            }else append(key,value(r,t,depth+1,children));
            if(r.offset!=p.value.size())throw std::runtime_error("T3D property boundary mismatch: "+p.name+" ("+std::to_string(r.offset)+"/"+std::to_string(p.value.size())+")");
        }
        return result;
    }
    std::string point(Reader& r){const auto x=number(std::bit_cast<float>(r.word())),y=number(std::bit_cast<float>(r.word())),z=number(std::bit_cast<float>(r.word()));return x+','+y+','+z;}
    int32_t polys(size_t model) {
        size_t offset=0;map.package->properties(model,&offset);const auto& object=map.package->exports[model];Reader r{map.package->data,offset,object.offset+object.size};r.skip(41);
        for(int i=0;i<2;++i)r.skip(count(r)*12);
        const auto nodes=count(r);for(size_t i=0;i<nodes;++i){r.skip(25);for(int j=0;j<7;++j)r.compact();r.skip(55);}
        const auto surfaces=count(r);for(size_t i=0;i<surfaces;++i){r.compact();r.word();for(int j=0;j<6;++j)r.compact();r.skip(20+(map.package->version>=123 && map.package->licensee>=23 ? 4:0));}
        const auto vertices=count(r,4000000);for(size_t i=0;i<vertices;++i){r.compact();r.compact();}r.word();const auto zones=r.word();if(zones>64)throw std::runtime_error("Invalid T3D BSP zones");for(unsigned i=0;i<zones;++i){r.compact();r.skip(20);}return r.compact();
    }
    std::string brush(size_t model,const std::string& indent) {
        const auto reference=polys(model);if(reference<=0 || size_t(reference)>map.package->exports.size() || map.package->className(size_t(reference-1))!="Engine.Polys")throw std::runtime_error("Brush has no original polygon data");
        size_t offset=0;map.package->properties(size_t(reference-1),&offset);const auto& object=map.package->exports[size_t(reference-1)];Reader r{map.package->data,offset,object.offset+object.size};const auto size=r.word(),capacity=r.word();if(size>1000000 || capacity<size)throw std::runtime_error("Invalid T3D polygon database");
        std::string result=indent+"Begin Brush Name="+map.package->exports[model].name+"\r\n"+indent+"   Begin PolyList\r\n";
        for(unsigned i=0;i<size;++i) {
            const auto vertices=count(r,64);const auto origin=point(r),normal=point(r),u=point(r),v=point(r);std::vector<std::string> points;for(size_t j=0;j<vertices;++j)points.push_back(point(r));
            const auto flags=r.word();r.compact();const auto material=r.compact();const auto item=map.package->name(r.compact());const auto link=r.compact();r.compact();if(map.package->version<78)throw std::runtime_error("Unsupported old brush polygon");if(map.package->version>=106)r.skip(4);if(map.package->version>=123 && map.package->version<=133 && map.package->licensee>=23 && map.package->licensee<=40)r.skip(4);
            result+=indent+"      Begin Polygon";if(item!="None")result+=" Item="+item;if(material)result+=" Texture="+map.package->objectPath(material);if(flags)result+=" Flags="+std::to_string(flags);if(link!=-1)result+=" Link="+std::to_string(link);result+="\r\n";
            result+=indent+"         Origin   "+origin+"\r\n"+indent+"         Normal   "+normal+"\r\n"+indent+"         TextureU "+u+"\r\n"+indent+"         TextureV "+v+"\r\n";for(const auto& p:points)result+=indent+"         Vertex   "+p+"\r\n";result+=indent+"      End Polygon\r\n";
        }
        if(r.offset!=r.end)throw std::runtime_error("T3D polygon boundary mismatch");return result+indent+"   End PolyList\r\n"+indent+"End Brush\r\n";
    }
    std::string object(size_t index,const std::string& indent,const std::string& kind,unsigned depth) {
        if(depth>32 || !active.insert(index).second)throw std::runtime_error("Cyclic T3D inline object");
        const auto& p=*map.package;const auto& item=p.exports.at(index);const auto classRef=packages.resolve(map,item.type);
        std::vector<size_t> children;const auto body=properties(p.properties(index),classRef,indent+"   ",0,children);
        std::string nested;for(const auto child:children)if(!emitted.contains(child)){emitted.insert(child);nested+=p.className(child)=="Engine.Model" ? brush(child,indent+"   ") : object(child,indent+"   ","Object",depth+1);}
        active.erase(index);return indent+"Begin "+kind+" Class="+(kind=="LevelObject" ? p.className(index) : leaf(p.className(index)))+" Name="+item.name+"\r\n"+nested+body+indent+"End "+kind+"\r\n";
    }
public:
    Writer(Packages& packages,const std::shared_ptr<const Package>& package,const std::string& module):packages(packages),map{package,module,0}{}
    std::string actor(size_t index,bool levelObject){active.clear();emitted.clear();return object(index,"",levelObject ? "LevelObject" : "Actor",0);}
};
std::vector<std::pair<size_t,bool>> actors(const Package& p) {
    std::vector<std::pair<size_t,bool>> result;unsigned levels=0;
    for(size_t i=0;i<p.exports.size();++i)if(p.className(i)=="Engine.Level") {
        ++levels;size_t offset=0;p.properties(i,&offset);const auto& object=p.exports[i];Reader r{p.data,offset,object.offset+object.size};const int databases=p.version>=123 && p.licensee>=23 ? 2:1;
        for(int database=0;database<databases;++database){const auto size=r.word(),capacity=r.word();if(size>1000000 || capacity<size)throw std::runtime_error("Invalid T3D actor database");for(unsigned j=0;j<size;++j){const auto ref=r.compact();if(!ref)continue;if(ref<0 || size_t(ref)>p.exports.size())throw std::runtime_error("Invalid T3D actor reference");result.emplace_back(size_t(ref-1),databases==2 && database==0);}}
    }
    if(levels!=1)throw std::runtime_error("Missing or ambiguous T3D level");return result;
}
}
int exportT3d(const std::filesystem::path& input,const std::filesystem::path& destination,unsigned workers,bool classic) {
    namespace fs=std::filesystem;if(fs::exists(destination))throw std::runtime_error("T3D output exists; choose a new folder");
    auto root=fs::canonical(input);if(fs::is_regular_file(root))root=root.parent_path();const auto maps=selectMaps(input,classic,false);Packages packages(maps.front());fs::create_directories(destination);
    std::atomic<size_t> next{0},accepted{0},rejected{0},failed{0};std::mutex progress;size_t completed=0;std::vector<std::string> notes(maps.size());
    {
        std::vector<std::jthread> pool;for(unsigned worker=0;worker<std::min<size_t>(workers,maps.size());++worker)pool.emplace_back([&]{
            for(;;){const auto index=next.fetch_add(1);if(index>=maps.size())break;std::ostringstream report;
                {std::lock_guard lock(progress);std::cerr<<"MAP "<<output::utf8(maps[index].filename())<<" | T3D actors and brushes\n";}
                try {
                    const auto p=std::make_shared<const Package>(Package::open(maps[index]));Writer writer(packages,p,output::utf8(maps[index].stem()));std::map<std::string,std::string> groups;size_t count=0;
                    for(const auto [actor,levelObject]:actors(*p))try {
                        const auto properties=p->properties(actor);if(std::any_of(properties.begin(),properties.end(),[](const auto& v){return (v.name=="bDeleteMe" || v.name=="bPendingDelete") && v.kind==3 && v.boolean;}))continue;
                        const auto text=writer.actor(actor,levelObject);const auto type=p->className(actor);if(type.find_first_of("/\\:\"<>|?*\r\n")!=std::string::npos)throw std::runtime_error("Invalid T3D class filename");groups[type]+=text;++count;
                    }catch(const std::exception& e){++rejected;report<<output::utf8(maps[index].filename())<<" / "<<p->exports[actor].name<<": "<<e.what()<<'\n';}
                    auto folder=mapOutput(maps[index],root);folder.replace_extension();for(const auto& [type,text]:groups)output::save(destination/folder/(type+".t3d"),"Begin Map\r\n"+text+"Begin Surface\r\nEnd Surface\r\nEnd Map\r\n");accepted+=count;
                }catch(const std::exception& e){++failed;report<<output::utf8(maps[index].filename())<<": "<<e.what()<<'\n';}
                notes[index]=report.str();std::lock_guard lock(progress);std::cerr<<"Checked "<<++completed<<" of "<<maps.size()<<" maps\n";
            }
        });
    }
    for(const auto& note:notes){std::istringstream lines(note);std::string line;while(std::getline(lines,line))if(!line.empty())std::cerr<<"SKIPPED: "<<line<<'\n';}
    writeExportReadme(destination,"T3D");std::cerr<<"RESULT t3d "<<accepted<<' '<<rejected<<' '<<failed<<" 0 0\n";return rejected || failed ? 2:0;
}
int verifyT3d() {
    auto need=[](bool condition){if(!condition)throw std::runtime_error("T3D serialization check failed");};
    need(quote("a\"\\\n\r\t")=="\"a\\\"\\\\\\n\\r\\t\"");
    need(number(1.5f)=="1.5" && number(-1532.f)=="-1532.0");
    for(const auto value:{1.e-30f,std::numeric_limits<float>::max(),std::numeric_limits<float>::min(),183888.f,0.00000012345678f})need(std::stof(number(value))==value);
    bool rejected=false;try{number(std::numeric_limits<float>::infinity());}catch(const std::exception&){rejected=true;}need(rejected);
    auto package=std::make_shared<Package>();package->version=123;package->names={"None","Count","ModeA","ModeB"};
    package->imports={{0,0,"Core"},{0,-1,"IntProperty"}};
    package->data={0,0,0,2,2,3,0,0,0,1,0,0,0,0,0,0,0,0};
    package->exports={{0,0,"Vector"},{0,0,"Record"},{0,0,"Mode",0,0,6},{-2,0,"Inner",0,6,12}};
    Packages packages;Writer writer(packages,package,"Fixture");std::vector<size_t> children;
    auto decode=[&](const Type& type,const std::vector<uint8_t>& bytes){Reader r{bytes};const auto result=writer.value(r,type,0,children);need(r.offset==bytes.size());return result;};
    need(decode(Type{"IntProperty"},{255,255,255,255})=="-1");
    need(decode(Type{"Int64Property"},{0,0,0,0,0,0,0,128})=="-9223372036854775808");
    need(decode(Type{"FloatProperty"},{0,0,192,63})=="1.5");
    need(decode(Type{"BoolProperty"},{1})=="True");
    need(decode(Type{"NameProperty"},{1})=="\"Count\"");
    need(decode(Type{"StrProperty"},{0})=="\"\"");
    need(decode(Type{"StrProperty"},{3,'h','i',0})=="\"hi\"");
    need(decode(Type{"StrProperty"},{130,0xA9,3,0,0})=="\"Ω\"");
    const Ref ref{package,"Fixture",0};
    need(decode(Type{"ByteProperty",0,1,ref,3},{1})=="ModeB");
    need(decode(Type{"StructProperty",0,1,ref,1},{0,0,128,63,0,0,0,64,0,0,64,64})=="(X=1.0,Y=2.0,Z=3.0)");
    need(decode(Type{"StructProperty",0,1,ref,2},{1,0x22,42,0,0,0,0})=="(Count=42)");
    need(decode(Type{"ArrayProperty",0,1,ref,4},{2,1,0,0,0,255,255,255,255})=="(1,-1)");
    need(decode(Type{"ObjectProperty",8},{1})=="Class'Vector'" && children==std::vector<size_t>{0});
    for(const auto& bytes:std::vector<std::vector<uint8_t>>{{3,'h','i'},{130,0xA9,3,1,0},{129,1}}) {
        rejected=false;try{decode(Type{"StrProperty"},bytes);}catch(const std::exception&){rejected=true;}need(rejected);
    }
    rejected=false;try{decode(Type{"IntProperty"},{1,2,3});}catch(const std::exception&){rejected=true;}need(rejected);
    rejected=false;try{decode(Type{"ArrayProperty",0,1,ref,4},{129});}catch(const std::exception&){rejected=true;}need(rejected);
    std::cout<<"T3D checks passed: precision, escaping, Unicode, scalars, structs, enums, arrays, inline references and invalid boundaries\n";return 0;
}
}
