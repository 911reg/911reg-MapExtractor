#pragma once
#include "package.h"
#include <memory>
#include <map>
#include <set>
namespace extractor {
struct CollisionActor {
    std::vector<Property> properties;
    std::set<std::string> ancestors;
    std::map<std::string,std::shared_ptr<Package>> contexts;
    bool isA(const std::string& name) const;
    std::shared_ptr<Package> context(const std::string& name) const;
};
class ClientClasses {
    friend int verifyClassDatabase(const std::filesystem::path& root);
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit ClientClasses(const std::filesystem::path& map);
    ~ClientClasses();
    CollisionActor actor(const std::shared_ptr<Package>& package,size_t index);
    std::string summary() const;
};
int verifyClassDatabase(const std::filesystem::path& root);
}
