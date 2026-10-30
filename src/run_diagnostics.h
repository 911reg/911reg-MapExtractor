#pragma once

struct RunDiagnostic {
    std::string key,subject,reason;
    size_t occurrences=1;
    std::set<std::string> reasons;
};
struct RunCounters {
    size_t errors=0,warnings=0,skipped=0,duplicateWarnings=0,revision=0;
    std::set<std::string> warningMessages,skippedMaps;
    std::array<std::vector<RunDiagnostic>,3> details;
    std::array<std::map<std::string,size_t>,3> indices;
    bool duplicateSection=false;
    std::string duplicateExplanation;

    static std::pair<std::string,std::string> split(const std::string& value) {
        const auto at=value.find(": ");
        if(at!=std::string::npos && at<180)return {value.substr(0,at),value.substr(at+2)};
        return {"",value};
    }
    void record(size_t category,std::string subject,std::string reason,std::string key={}) {
        if(key.empty())key=subject+'\n'+reason;
        auto& lookup=indices[category];
        auto& rows=details[category];
        if(const auto found=lookup.find(key);found!=lookup.end()) {
            auto& row=rows[found->second];++row.occurrences;
            if(!reason.empty() && row.reasons.insert(reason).second)row.reason+="\n\n"+reason;
        } else {
            lookup.emplace(key,rows.size());
            RunDiagnostic row{std::move(key),std::move(subject),std::move(reason)};
            row.reasons.insert(row.reason);rows.push_back(std::move(row));
        }
        ++revision;
    }
    bool consume(const std::string& line,unsigned mode) {
        const auto before=std::tuple{errors,warnings,skipped};
        if(line.starts_with("ERROR:")) {
            ++errors;auto [subject,reason]=split(line.substr(line.starts_with("ERROR: ")?7:6));
            record(0,std::move(subject),std::move(reason));
        } else if(line.starts_with("WARNING: ")) {
            warningMessages.insert(line);warnings=warningMessages.size()+duplicateWarnings;
            auto [subject,reason]=split(line.substr(9));record(1,std::move(subject),std::move(reason));
        } else if(line.starts_with("SKIPPED: ")) {
            const auto item=line.substr(9);
            if(mode>=2) {
                auto map=item.substr(0,item.find_first_of("/:"));
                while(!map.empty() && map.back()==' ')map.pop_back();
                skippedMaps.insert(map);skipped=skippedMaps.size();
                auto reason=item.substr(map.size());
                if(reason.starts_with(" / "))reason.erase(0,3);
                else if(reason.starts_with(": "))reason.erase(0,2);
                record(2,map,std::move(reason),map);
            } else {
                ++skipped;auto [subject,reason]=split(item);record(2,std::move(subject),std::move(reason));
            }
        } else if(line.starts_with("Duplicate door IDs mean"))duplicateExplanation=line;
        else if(line.starts_with("For stacked duplicates"))duplicateExplanation+='\n'+line;
        else if(line=="Duplicate door IDs:")duplicateSection=true;
        else if(line.starts_with("RESULT ")) {
            duplicateSection=false;
            std::string kind;
            size_t exported=0,rejected=0,maps=0,extra=0,duplicates=0;
            std::istringstream fields(line.substr(7));
            if(fields>>kind>>exported>>rejected>>maps>>extra>>duplicates) {
                skipped=rejected+maps+extra;duplicateWarnings=kind=="doors"?duplicates:0;
                warnings=warningMessages.size()+duplicateWarnings;
                if(skipped && details[2].empty())record(2,"","");
                if(warnings && details[1].empty())record(1,"","");
                ++revision;
            }
        } else if(duplicateSection && line.find(": ")!=std::string::npos) {
            auto [subject,reason]=split(line);
            if(!duplicateExplanation.empty())reason=duplicateExplanation+"\n\n"+reason;
            record(1,std::move(subject),std::move(reason));
        }
        return before!=std::tuple{errors,warnings,skipped};
    }
    void finish(DWORD code) {
        if(code && code!=2 && !errors) {errors=1;record(0,"Exit code "+std::to_string(code),"");}
    }
};
