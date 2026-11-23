#include "class_database.h"
#include "map_selection.h"
#include <windows.h>
#include <bcrypt.h>
#include <functional>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
namespace extractor {
namespace {
std::string lower(std::string s) { for(auto& c:s)if(c>='A' && c<='Z')c+=32;return s; }
std::string leaf(const std::string& s) { const auto dot=s.rfind('.');return dot==std::string::npos ? s : s.substr(dot+1); }
const std::set<std::string> wanted={"bdeleteme","bstatic","bcollideactors","bblockactors","bblockplayers","bworldgeometry","bblocknonzeroextenttraces","bblockzeroextenttraces","busecylindercollision","bignoreencroachers","bclassblocker","blockedclasses","bwatervolume","bterrainzone","inverted","location","rotation","prepivot","drawscale","drawscale3d","collisionradius","collisionheight","priority","staticmesh","brush","terrainmap"};
std::string sha(const std::vector<uint8_t>& bytes) {
    BCRYPT_ALG_HANDLE handle=nullptr;unsigned char digest[32];
    if(BCryptOpenAlgorithmProvider(&handle,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("Cannot initialize class-cache SHA-256");
    const auto status=BCryptHash(handle,nullptr,0,const_cast<PUCHAR>(bytes.data()),ULONG(bytes.size()),digest,32);BCryptCloseAlgorithmProvider(handle,0);
    if(status<0)throw std::runtime_error("Cannot hash class-cache content");
    std::ostringstream out;for(auto b:digest)out<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(b);return out.str();
}
void word(std::vector<uint8_t>& out,uint32_t n) {for(int i=0;i<4;++i)out.push_back(uint8_t(n>>(8*i)));}
void string(std::vector<uint8_t>& out,const std::string& s) {word(out,uint32_t(s.size()));out.insert(out.end(),s.begin(),s.end());}
std::string string(Reader& r) {const auto n=r.word();if(n>1048576)throw std::runtime_error("Class cache string exceeds limit");const auto at=r.offset;r.skip(n);return {r.bytes.begin()+at,r.bytes.begin()+r.offset};}
size_t array(Reader& r) {const auto n=r.compact();if(n<0 || n>1000000)throw std::runtime_error("Class metadata array exceeds limit");return size_t(n);}
struct Definition {int32_t parent=0;std::vector<Property> defaults,zeros;};
class Script {
    const Package& p;Reader& r;int used=0,size;size_t budget;
    void spend() {if(!budget)throw std::runtime_error("Class bytecode parsing budget exhausted");--budget;}
    void bytes(unsigned n) {r.skip(n);used+=int(n);}
    void ref() {p.objectPath(r.compact());used+=4;}
    std::string name() {auto s=p.name(r.compact());used+=4;return s;}
    uint8_t expression(unsigned depth) {
        if(depth>256 || used>=size)throw std::runtime_error("Invalid class bytecode nesting or size");
        spend();const auto token=r.byte();++used;auto sub=[&]{return expression(depth+1);};
        if(token>=0x60) {if(token<0x70)bytes(1);while(sub()!=0x16){};}
        else switch(token) {
        case 0:case 1:case 2:case 0x20:case 0x29:ref();break;
        case 8:case 11:case 0x16:case 0x17:case 0x25:case 0x26:case 0x27:case 0x28:case 0x2a:case 0x2d:case 0x30:case 0x31:break;
        case 4:case 13:case 14:case 0x37:sub();break;
        case 15:case 16:case 20:case 0x1a:case 0x45:sub();sub();break;
        case 0x40:case 0x41:sub();sub();sub();break;
        case 0x11:sub();sub();sub();sub();break;
        case 6:bytes(2);break;
        case 7:case 9:case 0x18:bytes(2);sub();break;
        case 5:case 0x39:bytes(1);sub();break;
        case 10:{const auto end=r.word(2);used+=2;if(end!=65535)sub();break;}
        case 0x12:case 0x19:sub();bytes(3);sub();break;
        case 0x13:case 0x2e:case 0x36:ref();sub();break;
        case 0x32:case 0x33:ref();sub();sub();break;
        case 0x2f:sub();bytes(2);break;
        case 0x1b:case 0x1c:case 0x38:if(token==0x1c)ref();else name();while(sub()!=0x16){};break;
        case 0x1d:case 0x1e:bytes(4);break;
        case 0x22:case 0x23:bytes(12);break;
        case 0x24:case 0x2c:bytes(1);break;
        case 0x21:case 0x44:name();break;
        case 0x43:ref();name();break;
        case 0x1f:case 0x34:case 0x42:{if(token==0x42)bytes(12);const unsigned width=token==0x34 ? 2 : 1;uint32_t c;do{spend();c=r.word(width);used+=width;if(used>size)throw std::runtime_error("Class bytecode string exceeds logical size");}while(c);break;}
        case 12:for(;;){spend();const auto n=name();bytes(4);if(lower(n)=="none")break;}break;
        default:throw std::runtime_error("Unsupported class bytecode token "+std::to_string(token));
        }
        if(used>size)throw std::runtime_error("Class bytecode size mismatch");return token;
    }
public:
    Script(const Package& package,Reader& reader,int length,size_t limit=1000000):p(package),r(reader),size(length),budget(limit){}
    void skip() {if(size<0 || size>16000000)throw std::runtime_error("Invalid class script size");while(used<size)expression(0);}
};
Definition parse(const Package& p,size_t index) {
    const auto& e=p.exports.at(index);if(p.className(index)!="Core.Class" || (e.flags&0x02000000))throw std::runtime_error("Invalid bounded class export");
    Reader r{p.data,e.offset,e.offset+e.size};Definition d;d.parent=r.compact();p.objectPath(d.parent);p.objectPath(r.compact());
    if(d.parent!=e.super)throw std::runtime_error("Class superclass metadata disagrees with export table");
    p.objectPath(r.compact());p.objectPath(r.compact());p.name(r.compact());if(p.version>=120)p.objectPath(r.compact());r.skip(8);
    Script(p,r,int32_t(r.word())).skip();r.skip(22);r.word();r.skip(16);
    for(size_t n=array(r);n;--n){p.objectPath(r.compact());r.skip(8);}
    for(size_t n=array(r);n;--n)p.name(r.compact());
    p.objectPath(r.compact());p.name(r.compact());for(size_t n=array(r);n;--n)p.name(r.compact());
    for(auto& tag:p.tags(r))if(wanted.contains(lower(tag.name)))d.defaults.push_back(std::move(tag));
    if(r.offset!=r.end)throw std::runtime_error("Class defaults did not consume complete export");
    for(size_t i=0;i<p.exports.size();++i) {
        const auto& field=p.exports[i];if(field.outer!=int32_t(index+1) || !wanted.contains(lower(field.name)))continue;
        const auto type=leaf(p.className(i));if(!type.ends_with("Property"))continue;
        size_t at=0;p.properties(i,&at);Reader f{p.data,at,field.offset+field.size};f.compact();f.compact();const auto dimension=f.word(),flags=f.word();f.compact();if(flags&32)f.skip(2);
        if(dimension!=1)throw std::runtime_error("Unsupported fixed-array collision field: "+field.name);
        if(flags&0xc000)throw std::runtime_error("Config/localized collision defaults require runtime handling: "+field.name);
        Property z;z.name=field.name;
        if(type=="BoolProperty")z.kind=3;
        else if(type=="ByteProperty"){z.kind=1;z.value.resize(1);}
        else if(type=="IntProperty"){z.kind=2;z.value.resize(4);}
        else if(type=="FloatProperty"){z.kind=4;z.value.resize(4);}
        else if(type=="ObjectProperty"){z.kind=5;z.value.resize(1);}
        else if(type=="StructProperty") {
            z.structure=leaf(p.objectPath(f.compact()));if(z.structure!="Vector" && z.structure!="Rotator")throw std::runtime_error("Unsupported collision default struct: "+z.name);z.kind=10;z.value.resize(12);
        } else if(type=="ArrayProperty") {z.kind=9;z.value.resize(1);}
        else throw std::runtime_error("Unsupported collision default property: "+z.name+" ("+type+")");
        d.zeros.push_back(std::move(z));
    }
    return d;
}
void properties(std::vector<uint8_t>& out,const std::vector<Property>& tags) {
    word(out,uint32_t(tags.size()));for(const auto& p:tags){string(out,p.name);string(out,p.structure);word(out,p.kind);word(out,p.element);word(out,p.boolean);word(out,uint32_t(p.value.size()));out.insert(out.end(),p.value.begin(),p.value.end());}
}
std::vector<Property> properties(Reader& r) {
    std::vector<Property> tags;const auto count=r.word();if(count>100000)throw std::runtime_error("Invalid class cache count");
    for(unsigned i=0;i<count;++i){Property p;p.name=string(r);p.structure=string(r);const auto kind=r.word();if(kind>15 || !kind)throw std::runtime_error("Invalid cached property kind");p.kind=uint8_t(kind);p.element=r.word();const auto b=r.word();if(b>1)throw std::runtime_error("Invalid cached boolean");p.boolean=b!=0;const auto n=r.word();if(n>16777216)throw std::runtime_error("Invalid cached property size");const auto at=r.offset;r.skip(n);p.value.assign(r.bytes.begin()+at,r.bytes.begin()+r.offset);tags.push_back(std::move(p));}return tags;
}
}
bool CollisionActor::isA(const std::string& name) const {return ancestors.contains(lower(name));}
std::shared_ptr<Package> CollisionActor::context(const std::string& name) const {const auto found=contexts.find(lower(name));if(found==contexts.end())throw std::runtime_error("Unresolved collision property context: "+name);return found->second;}
struct ClientClasses::Impl {
    struct Ref {std::shared_ptr<Package> package;size_t index;};
    std::filesystem::path root,cache;
    std::map<std::string,std::filesystem::path> paths;
    std::map<std::string,std::shared_ptr<Package>> packages;
    std::map<const Package*,std::map<std::string,size_t>> classIndices;
    std::map<const Package*,std::string> hashes,modules;
    std::map<std::pair<const Package*,size_t>,Definition> definitions;
    std::map<const Package*,std::shared_ptr<Package>> classOwners;
    std::map<std::pair<const Package*,size_t>,CollisionActor> defaults;
    size_t hits=0,misses=0,cacheFailures=0;
    explicit Impl(const std::filesystem::path& map) {
        root=std::filesystem::absolute(map).lexically_normal();if(!std::filesystem::is_directory(root))root=root.parent_path();
        while(root!=root.parent_path() && !std::filesystem::is_directory(root/"System"))root=root.parent_path();
        if(!std::filesystem::is_directory(root/"System"))throw std::runtime_error("Class defaults require this client's System folder");
        for(const auto& dir:{"System/CustomFiles","System"})if(std::filesystem::is_directory(root/dir))for(const auto& e:std::filesystem::directory_iterator(root/dir))if(!e.is_symlink() && e.is_regular_file() && folded(e.path().extension().wstring())==L".u")paths.emplace(lower(output::utf8(e.path().stem())),e.path());
        wchar_t local[32768];const auto n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);if(n && n<32768)cache=std::filesystem::path(local)/"911reg"/"MapExtractor"/"ClassCache-v1";
    }
    std::shared_ptr<Package> load(const std::string& name) {
        const auto key=lower(name);if(const auto it=packages.find(key);it!=packages.end())return it->second;
        const auto file=paths.find(key);if(file==paths.end())throw std::runtime_error("Missing client class package: "+name+".u");
        auto p=std::make_shared<Package>(Package::open(file->second));modules[p.get()]=file->second.stem().string();hashes[p.get()]=sha(p->data);packages[key]=p;return p;
    }
    Ref resolve(const std::shared_ptr<Package>& context,int32_t reference) {
        if(reference>0) {if(size_t(reference)>context->exports.size())throw std::runtime_error("Invalid local class reference");return {context,size_t(reference-1)};}
        if(!reference)throw std::runtime_error("Null actor class reference");
        const auto path=context->objectPath(reference);const auto dot=path.find('.');if(dot==std::string::npos)throw std::runtime_error("Invalid imported class path");
        const auto package=load(path.substr(0,dot));const auto object=lower(path.substr(dot+1));
        auto& indices=classIndices[package.get()];
        if(indices.empty())for(size_t i=0;i<package->exports.size();++i)if(package->className(i)=="Core.Class")indices.emplace(lower(package->objectPath(int32_t(i+1))),i);
        if(const auto found=indices.find(object);found!=indices.end())return {package,found->second};
        throw std::runtime_error("Missing client class: "+path);
    }
    const Definition& definition(const Ref& ref) {
        const auto key=std::pair{ref.package.get(),ref.index};if(const auto it=definitions.find(key);it!=definitions.end())return it->second;
        if(!hashes.contains(ref.package.get()))hashes[ref.package.get()]=sha(ref.package->data);
        const auto identity=hashes[ref.package.get()]+"-"+std::to_string(ref.index);const auto file=cache/(identity+".bin");
        if(!cache.empty())try {
            std::ifstream in(file,std::ios::binary|std::ios::ate);if(in) {
                const auto length=in.tellg();if(length<4 || length>16777216)throw std::runtime_error("Invalid class cache length");std::vector<uint8_t> data(static_cast<size_t>(length));in.seekg(0);if(!in.read(reinterpret_cast<char*>(data.data()),length))throw std::runtime_error("Unreadable class cache");Reader r{data,0,data.size()};
                if(string(r)!="MapExtractorClassCache1" || string(r)!=identity)throw std::runtime_error("Stale class cache");const auto checksum=string(r);const auto at=r.offset;std::vector<uint8_t> body(data.begin()+at,data.end());if(sha(body)!=checksum)throw std::runtime_error("Corrupt class cache");
                Definition d;d.parent=int32_t(r.word());d.defaults=properties(r);d.zeros=properties(r);if(r.offset!=r.end || d.parent!=ref.package->exports.at(ref.index).super)throw std::runtime_error("Invalid class cache payload");++hits;classOwners[ref.package.get()]=ref.package;return definitions.emplace(key,std::move(d)).first->second;
            }
        }catch(const std::exception&){++cacheFailures;}
        classOwners[ref.package.get()]=ref.package;
        auto d=parse(*ref.package,ref.index);++misses;
        if(!cache.empty())try {
            std::vector<uint8_t> body;word(body,uint32_t(d.parent));properties(body,d.defaults);properties(body,d.zeros);std::vector<uint8_t> bytes;string(bytes,"MapExtractorClassCache1");string(bytes,identity);string(bytes,sha(body));bytes.insert(bytes.end(),body.begin(),body.end());
            std::filesystem::create_directories(cache);const auto temp=file.wstring()+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetCurrentThreadId())+L".partial";
            std::ofstream out(std::filesystem::path(temp),std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));out.close();if(!out || !MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){std::filesystem::remove(temp);throw std::runtime_error("Cannot persist class cache");}
        }catch(const std::exception&){++cacheFailures;}
        return definitions.emplace(key,std::move(d)).first->second;
    }
    CollisionActor actor(const std::shared_ptr<Package>& p,size_t index) {
        std::vector<Ref> chain;std::set<std::pair<const Package*,size_t>> seen;auto ref=resolve(p,p->exports.at(index).type);CollisionActor result;
        const auto classKey=std::pair{ref.package.get(),ref.index};
        if(const auto ready=defaults.find(classKey);ready!=defaults.end()) {
            result=ready->second;
            for(const auto& property:p->properties(index)) {
                const auto match=std::find_if(result.properties.begin(),result.properties.end(),[&](const auto& old){return lower(old.name)==lower(property.name) && old.element==property.element;});
                if(match==result.properties.end())result.properties.push_back(property);else *match=property;
                result.contexts[lower(property.name)]=p;
            }
            return result;
        }
        for(;;) {
            if(chain.size()>=128 || !seen.emplace(ref.package.get(),ref.index).second)throw std::runtime_error("Cyclic or excessive collision class hierarchy");chain.push_back(ref);
            auto module=modules.find(ref.package.get());const auto name=(module==modules.end() ? std::string{} : module->second+".")+ref.package->objectPath(int32_t(ref.index+1));result.ancestors.insert(lower(name));
            const auto parent=definition(ref).parent;if(!parent)break;ref=resolve(ref.package,parent);
        }
        std::map<std::pair<std::string,uint32_t>,Property> merged;
        auto apply=[&](const std::vector<Property>& ps,const std::shared_ptr<Package>& owner,bool zero) {
            std::set<std::pair<std::string,uint32_t>> once;
            for(const auto& property:ps){const auto key=std::pair{lower(property.name),property.element};if(!once.insert(key).second)throw std::runtime_error("Duplicate collision default: "+property.name);if(zero && merged.contains(key))continue;merged[key]=property;result.contexts[key.first]=owner;}
        };
        for(auto it=chain.rbegin();it!=chain.rend();++it){const auto& d=definition(*it);apply(d.zeros,it->package,true);apply(d.defaults,it->package,false);}
        for(const auto& [key,value]:merged)result.properties.push_back(value);defaults[classKey]=result;
        apply(p->properties(index),p,false);result.properties.clear();for(auto& [key,value]:merged)result.properties.push_back(std::move(value));return result;
    }
};
ClientClasses::ClientClasses(const std::filesystem::path& map):impl(std::make_unique<Impl>(map)){}
ClientClasses::~ClientClasses()=default;
CollisionActor ClientClasses::actor(const std::shared_ptr<Package>& p,size_t i){return impl->actor(p,i);}
std::string ClientClasses::summary() const {return "Client collision defaults: "+std::to_string(impl->definitions.size())+" classes, "+std::to_string(impl->hits)+" disk-cache hits, "+std::to_string(impl->misses)+" class reads, "+std::to_string(impl->cacheFailures)+" cache failures.";}
int verifyClassDatabase(const std::filesystem::path& root) {
    auto need=[](bool ok){if(!ok)throw std::runtime_error("Client class-default fixture failed");};
    if(std::filesystem::exists(root))throw std::runtime_error("Class-default fixture output already exists");
    std::filesystem::create_directories(root/"System");std::filesystem::create_directories(root/"Maps");
    auto engine=std::make_shared<Package>();engine->version=123;engine->licensee=37;
    engine->names={"None","bCollideActors","bBlockPlayers","StaticMesh"};
    engine->imports={{0,0,"Core"},{0,-1,"BoolProperty"}};
    auto metadata=[&](int parent,const std::vector<uint8_t>& tags,bool code=false) {
        const auto at=engine->data.size();auto& out=engine->data;
        out.push_back(uint8_t(parent));for(int j=0;j<5;++j)out.push_back(0);out.resize(out.size()+8,0);
        word(out,code ? 5 : 0);if(code){out.push_back(0x21);out.push_back(0);}
        out.resize(out.size()+22,0);word(out,0);out.resize(out.size()+16,0);for(int j=0;j<5;++j)out.push_back(0);
        out.insert(out.end(),tags.begin(),tags.end());return std::pair{at,out.size()-at};
    };
    const auto a=metadata(0,{0},true),b=metadata(1,{1,0x83,2,0x83,3,0x05,1,0}),c=metadata(2,{2,0x03,0});
    const auto fieldOffset=engine->data.size();engine->data.insert(engine->data.end(),{0,0,0});word(engine->data,1);word(engine->data,0);engine->data.push_back(0);
    engine->exports={{0,0,"Actor",0,a.first,a.second},{0,0,"Volume",0,b.first,b.second,1},{0,0,"CustomVolume",0,c.first,c.second,2},{-2,1,"bCollideActors",0,fieldOffset,engine->data.size()-fieldOffset}};
    auto map=std::make_shared<Package>();map->names={"None","bBlockPlayers"};map->imports={{0,0,"Engine"},{0,-1,"CustomVolume"}};
    map->data={0,1,0x83,0};map->exports={{-2,0,"DefaultActor",0,0,1},{-2,0,"OverrideActor",0,1,3}};
    auto create=[&] {
        auto db=std::make_unique<ClientClasses::Impl>(root/"Maps"/"fixture.unr");db->cache=root/"Cache";db->packages["engine"]=engine;db->modules[engine.get()]="Engine";db->hashes[engine.get()]=sha(engine->data);return db;
    };
    auto get=[](const CollisionActor& actor,const std::string& name) -> const Property& {for(const auto& p:actor.properties)if(lower(p.name)==lower(name))return p;throw std::runtime_error("Missing fixture property");};
    need(parse(*engine,0).zeros.size()==1 && !parse(*engine,0).zeros[0].boolean);
    auto db=create();const auto actor=db->actor(map,0);
    need(actor.isA("Engine.Volume") && actor.isA("engine.actor") && actor.isA("Engine.CustomVolume"));
    need(get(actor,"bCollideActors").boolean && !get(actor,"bBlockPlayers").boolean);
    need(actor.context("StaticMesh")==engine && actor.context("bBlockPlayers")==engine);
    const auto overridden=db->actor(map,1);need(get(overridden,"bBlockPlayers").boolean && overridden.context("bBlockPlayers")==map);
    auto cached=create();need(!get(cached->actor(map,0),"bBlockPlayers").boolean && cached->hits==3 && cached->misses==0);
    const auto cacheFile=db->cache/(sha(engine->data)+"-0.bin");{std::ofstream damaged(cacheFile,std::ios::binary|std::ios::trunc);damaged<<"broken";}
    auto repaired=create();need(get(repaired->actor(map,0),"bCollideActors").boolean && repaired->cacheFailures==1 && repaired->misses==1);
    engine->data[c.first+c.second-2]=0x83;auto changed=create();need(get(changed->actor(map,0),"bBlockPlayers").boolean && changed->hits==0 && changed->misses==3);
    engine->data[fieldOffset+8]=0x40;bool failed=false;try{parse(*engine,0);}catch(const std::exception&){failed=true;}need(failed);engine->data[fieldOffset+8]=0;
    engine->data[a.first+18]=0x46;failed=false;try{parse(*engine,0);}catch(const std::exception&){failed=true;}need(failed);engine->data[a.first+18]=0x21;
    std::vector<uint8_t> oversizedString{0x1f,65,65,65,0};Reader stringReader{oversizedString,0,oversizedString.size()};failed=false;
    try{Script(*engine,stringReader,5,2).skip();}catch(const std::exception&){failed=true;}need(failed);
    engine->data[a.first]=3;engine->exports[0].super=3;auto cyclic=create();failed=false;try{cyclic->actor(map,0);}catch(const std::exception&){failed=true;}need(failed);
    std::cout<<"Client class-default fixtures passed: bounded metadata, logical bytecode sizes, inheritance, zero defaults, actor overrides, source references, SHA-256 invalidation, disk cache, corrupt-cache recovery and cycles\n";return 0;
}

}
