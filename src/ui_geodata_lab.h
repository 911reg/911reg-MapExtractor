HANDLE labJob=nullptr;
std::vector<extractor::viewer::MapChoice> labMissing;
std::vector<extractor::GeodataRegion> labRegions;
size_t labNext=0;
std::filesystem::path labScratch;

void finishLabGeneration(HWND window) {
    KillTimer(window,3);
    if(labProcess){CloseHandle(labProcess);labProcess=nullptr;}
    if(labJob){CloseHandle(labJob);labJob=nullptr;}
    EnableWindow(geoViewerButton,TRUE);EnableWindow(clearOutputButton,!process);EnableWindow(output,!process);EnableWindow(outputButton,!process);EnableWindow(startButton,!process && sourceReady() && !text(output).empty());
    SetWindowTextW(status,L"Geodata ready. Opening selected maps.");
    if(!labRegions.empty())extractor::openGeodataRegions(window,labRegions);
}
void nextLabGeneration(HWND window) {
    while(labNext<labMissing.size()){
        auto& item=labMissing[labNext];const auto current=extractor::viewer::geodataChoices({item.map},std::filesystem::absolute(text(output)),extractor::classicMap(item.map));
        if(current.empty() || !std::filesystem::is_regular_file(current.front().geodata))break;
        item.geodata=current.front().geodata;labRegions.push_back({item.geodata,item.name,item.map});consoleLine(L"GEO",theme::green,L"Loading existing geodata: "+item.name,theme::body);++labNext;
    }
    if(labNext>=labMissing.size()){finishLabGeneration(window);return;}
    const auto& item=labMissing[labNext];
    std::vector<wchar_t> executable(32768);GetModuleFileNameW(nullptr,executable.data(),DWORD(executable.size()));
    labScratch=std::filesystem::absolute(text(output))/L"Geodata"/L".lab-jobs"/(std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(labNext));
    if(!std::filesystem::create_directories(labScratch))throw std::runtime_error("Cannot create isolated geodata generation folder");
    auto command=quote(executable.data())+L" geodata "+quote(item.map.wstring())+L" --output "+quote((labScratch/L"Generated").wstring())+L" --all-variants --geo l2j --no-pathnodes --simple-output --threads "+std::to_wstring(workerCount);
    if(disregardAdjacentEnabled)command+=L" --disregard-adjacent-maps";
    SECURITY_ATTRIBUTES security{sizeof(security),nullptr,TRUE};
    const auto log=CreateFileW((labScratch/L"generation.log").c_str(),GENERIC_WRITE,FILE_SHARE_READ,&security,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(log==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create geodata generation log");
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=startup.hStdError=log;startup.hStdInput=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&security,OPEN_EXISTING,0,nullptr);
    const bool launched=CreateProcessW(executable.data(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,nullptr,nullptr,&startup,&child)!=FALSE;
    CloseHandle(log);if(startup.hStdInput!=INVALID_HANDLE_VALUE)CloseHandle(startup.hStdInput);
    if(!launched)throw std::runtime_error("Cannot launch geodata generation");
    if(!AssignProcessToJobObject(labJob,child.hProcess)){TerminateProcess(child.hProcess,1);CloseHandle(child.hThread);CloseHandle(child.hProcess);throw std::runtime_error("Cannot protect geodata generation process");}
    labProcess=child.hProcess;const auto resumed=ResumeThread(child.hThread);CloseHandle(child.hThread);if(resumed==DWORD(-1))throw std::runtime_error("Cannot start geodata generation process");
    const auto message=L"Generating geodata for "+item.name+L" ("+std::to_wstring(labNext+1)+L"/"+std::to_wstring(labMissing.size())+L")";
    SetWindowTextW(status,message.c_str());consoleLine(L"GEO",theme::accent,message,theme::body);SetTimer(window,3,250,nullptr);
}
void pollLabGeneration(HWND window) {
    if(!labProcess || WaitForSingleObject(labProcess,0)!=WAIT_OBJECT_0)return;
    DWORD code=1;GetExitCodeProcess(labProcess,&code);CloseHandle(labProcess);labProcess=nullptr;
    const auto& item=labMissing.at(labNext);const bool classic=extractor::classicMap(item.map);auto base=item.name;if(classic)base.resize(base.size()-8);
    const auto generated=(classic ? labScratch/L"Generated"/L"Classic":labScratch/L"Generated")/L"L2J"/(base+L".l2j");
    if(code==0 && std::filesystem::is_regular_file(generated)){
        std::filesystem::create_directories(item.geodata.parent_path());
        if(!std::filesystem::exists(item.geodata) && !MoveFileExW(generated.c_str(),item.geodata.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Cannot publish generated geodata");
        labRegions.push_back({item.geodata,item.name,item.map});consoleLine(L"GEO",theme::green,L"Geodata ready: "+item.name,theme::body);
    }else{const auto error=L"Geodata generation failed for "+item.name+L".\n\nLog: "+(labScratch/L"generation.log").wstring();MessageBoxW(window,error.c_str(),L"View Geodata",MB_OK|MB_ICONERROR);consoleLine(L"FAIL",theme::red,error,theme::body);}
    ++labNext;nextLabGeneration(window);
}
void openSelectedGeodata(HWND window) {
    if(process || labProcess){MessageBoxW(window,L"Wait for the current extraction to finish.",L"View Geodata",MB_OK|MB_ICONINFORMATION);return;}
    if(selectedMaps.empty()){MessageBoxW(window,L"Select maps in Choose maps first.",L"View Geodata",MB_OK|MB_ICONINFORMATION);return;}
    if(text(output).empty())throw std::runtime_error("Choose the results folder containing your geodata");
    bool classic=false;
    if(extractor::viewer::hasVariantPairs(selectedMaps))classic=MessageBoxW(window,L"Load geodata from Classic maps?",L"View Geodata",MB_YESNO|MB_ICONQUESTION)==IDYES;
    const auto choices=extractor::viewer::geodataChoices(selectedMaps,std::filesystem::absolute(text(output)),classic);
    if(choices.empty())throw std::runtime_error("Select numeric region maps to view geodata");
    labRegions.clear();labMissing.clear();labNext=0;
    for(const auto& item:choices)if(std::filesystem::is_regular_file(item.geodata))labRegions.push_back({item.geodata,item.name,item.map});else labMissing.push_back(item);
    if(!labMissing.empty()){
        const auto question=L"Generate geodata for "+std::to_wstring(labMissing.size())+(labMissing.size()==1 ? L" map?":L" maps?");
        const auto answer=MessageBoxW(window,question.c_str(),L"View Geodata",MB_YESNOCANCEL|MB_ICONQUESTION|MB_DEFBUTTON2);if(answer==IDCANCEL)return;if(answer!=IDYES)labMissing.clear();
    }
    if(labMissing.empty()){if(!labRegions.empty())extractor::openGeodataRegions(window,labRegions);return;}
    labJob=CreateJobObjectW(nullptr,nullptr);if(!labJob)throw std::runtime_error("Cannot create geodata generation job");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!SetInformationJobObject(labJob,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){CloseHandle(labJob);labJob=nullptr;throw std::runtime_error("Cannot configure geodata generation job");}
    EnableWindow(geoViewerButton,FALSE);EnableWindow(startButton,FALSE);EnableWindow(clearOutputButton,FALSE);EnableWindow(output,FALSE);EnableWindow(outputButton,FALSE);nextLabGeneration(window);
}
