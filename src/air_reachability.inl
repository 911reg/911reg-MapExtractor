class AirReachability {
    struct Node { int x,y,layer,parent=-2;double low,high,entry=0;const char* seed=nullptr;uint8_t zone=0; };
    struct AirBlock { std::array<std::vector<int>,64> cells;uint64_t built=0; };
    struct Candidates { uint32_t key=0;bool valid=false;std::vector<uint32_t> triangles,cylinders; };
    struct Bounds { V low,high; };
    Geo& geo;
    struct QueryState {std::array<Candidates,64> candidates;Candidates oversized;std::vector<collision::BspInterval> bspScratch;};
    struct PreparedBlock {std::array<std::vector<int>,64> cells;std::vector<Node> nodes;uint64_t built=0;};
    QueryState queries;
    std::vector<Bounds> bounds;
    const std::vector<collision::BspInterval>& bsp(V from,V to,QueryState* state=nullptr) {auto& scratch=(state ? *state:queries).bspScratch;collision::bspRegions(geo.world,from,to,scratch);return scratch;}
    const Candidates& nearby(int bx0,int bx1,int by0,int by1,QueryState* state=nullptr) {
        auto& workspace=state ? *state:queries;
        const uint32_t key=(uint32_t(bx0)<<24)|(uint32_t(bx1)<<16)|(uint32_t(by0)<<8)|uint32_t(by1);
        auto& entry=workspace.candidates[(key*2654435761u)>>26];
        if(entry.valid && entry.key==key)return entry;
        Candidates result;result.key=key;result.valid=true;
        for(int bx=bx0;bx<=bx1;++bx)for(int by=by0;by<=by1;++by) {
            const size_t index=size_t(bx)*256+by;
            result.triangles.insert(result.triangles.end(),geo.index[index].begin(),geo.index[index].end());
            if(const auto found=geo.cylinderIndex.find(index);found!=geo.cylinderIndex.end())result.cylinders.insert(result.cylinders.end(),found->second.begin(),found->second.end());
        }
        for(auto* refs:{&result.triangles,&result.cylinders}){std::sort(refs->begin(),refs->end());refs->erase(std::unique(refs->begin(),refs->end()),refs->end());}
        if(result.triangles.capacity()+result.cylinders.capacity()>16384){workspace.oversized=std::move(result);return workspace.oversized;}
        entry=std::move(result);return entry;
    }
    std::array<std::unique_ptr<AirBlock>,65536> blocks;
    std::vector<Node> nodes;
    std::vector<int> pending;
    int minX=2048,minY=2048,maxX=-1,maxY=-1;
    size_t added=0,rejected=0,accepted=0,edges=0;

    V center(int x,int y,double z=0) const {return {geo.ox+x*16.+8,geo.oy+y*16.+8,z};}
    std::vector<air::Range> blocked(V from,V to,double low,double high,QueryState* state=nullptr) {
        std::vector<air::Range> result;
        const double x0=std::min(from.x,to.x)-air::radius,x1=std::max(from.x,to.x)+air::radius;
        const double y0=std::min(from.y,to.y)-air::radius,y1=std::max(from.y,to.y)+air::radius;
        if(geo.airBounded && (x0<geo.ox || y0<geo.oy || x1>geo.ox+32768. || y1>geo.oy+32768.))return {{low-1.,high+1.}};
        const int bx0=std::clamp(int(std::floor((x0-geo.ox)/128.)),0,255),bx1=std::clamp(int(std::floor((x1-geo.ox)/128.)),0,255);
        const int by0=std::clamp(int(std::floor((y0-geo.oy)/128.)),0,255),by1=std::clamp(int(std::floor((y1-geo.oy)/128.)),0,255);
        const auto& refs=nearby(bx0,bx1,by0,by1,state);
        for(const auto ref:refs.triangles) {
            const auto& t=geo.triangles[ref];const auto& box=bounds[ref];
            if(box.high.x<x0 || box.low.x>x1 || box.high.y<y0 || box.low.y>y1)continue;
            if(box.high.z+air::halfHeight+air::skin<low || box.low.z-air::halfHeight-air::skin>high)continue;
            if(const auto range=air::triangleRange(t,from,to))result.push_back({range->first-air::halfHeight-air::skin,range->second+air::halfHeight+air::skin});
        }
        const double dx=to.x-from.x,dy=to.y-from.y,length=dx*dx+dy*dy;
        for(const auto ref:refs.cylinders) {
            const auto& shape=geo.cylinders[ref];
            const double t=length>1e-12 ? std::clamp(((shape.location.x-from.x)*dx+(shape.location.y-from.y)*dy)/length,0.,1.) : 0.;
            if(std::hypot(from.x+t*dx-shape.location.x,from.y+t*dy-shape.location.y)<=air::radius+shape.radius+air::skin)result.push_back({shape.location.z-shape.halfHeight-air::halfHeight-air::skin,shape.location.z+shape.halfHeight+air::halfHeight+air::skin});
        }
        return result;
    }
    void fillCell(int x,int y,std::vector<int>& ids,std::vector<Node>& output,QueryState* state) {
        const auto p=center(x,y);
        const auto& floors=geo.cell(x,y);
        if(floors.empty() && geo.occupied.test((size_t(x/8)*256+y/8)*64+size_t(x%8)*8+y%8))return;
        double low=-16384.+air::halfHeight+air::skin,high=16384.-air::halfHeight-air::skin;
        if(!floors.empty())low=floors.front().floor+air::halfHeight+air::skin;
        const auto obstacles=blocked(p,p,low,high,state);
        auto append=[&](double bottom,double top,int layer){
            for(const auto range:air::freeRanges(bottom,top,obstacles)) {
                for(const auto region:bsp(center(x,y,range.first),center(x,y,range.second),state))if(region.outside) {
                    if(output.size()>=16000000)throw std::runtime_error("Air clearance graph exceeds node budget");
                    ids.push_back(int(output.size()));
                    output.push_back({x,y,layer,-2,range.first+(range.second-range.first)*region.low,range.first+(range.second-range.first)*region.high,0,nullptr,region.zone});
                }
            }
        };
        if(floors.empty())append(low,high,-1);
        else for(int i=0;i<int(floors.size());++i)if(floors[i].airAllowed)append(floors[i].floor+air::halfHeight+air::skin,std::min(high,floors[i].ceiling-air::halfHeight-air::skin),i);
    }
    const std::vector<int>& cell(int x,int y) {
        static const std::vector<int> empty;
        if(x<minX || y<minY || x>maxX || y>maxY)return empty;
        const size_t blockIndex=size_t(x/8)*256+y/8,cellIndex=size_t(x%8)*8+y%8;
        if(!blocks[blockIndex])blocks[blockIndex]=std::make_unique<AirBlock>();
        auto& block=*blocks[blockIndex];auto& ids=block.cells[cellIndex];const uint64_t mask=uint64_t(1)<<cellIndex;
        if(block.built&mask)return ids;
        block.built|=mask;
        fillCell(x,y,ids,nodes,nullptr);
        return ids;
    }
    void visit(int id,int parent,double z,const char* seed=nullptr) {
        auto& node=nodes[size_t(id)];if(node.parent!=-2)return;
        node.parent=parent;node.entry=z;node.seed=seed;pending.push_back(id);
        if(node.layer>=0) {
            auto& layer=(*geo.blocks[size_t(node.x/8)*256+node.y/8])[size_t(node.x%8)*8+node.y%8][node.layer];
            const double standing=air::standingCenter(layer.surface,layer.floor,retailHeightOffsets);
            if(!layer.reach && node.low<=standing && standing<=node.high){layer.reach=true;layer.air=true;++added;}
        }
    }
    void seed(V p,const char* kind) {
        if(!collision::bspEmpty(geo.world,p)){++rejected;return;}
        const int x=int(std::floor((p.x-geo.ox)/16.)),y=int(std::floor((p.y-geo.oy)/16.));
        for(const auto id:cell(x,y)) {
            const auto node=nodes[size_t(id)];
            if(p.z<node.low || p.z>node.high)continue;
            const auto& regions=bsp(p,center(x,y,p.z));
            if(std::any_of(regions.begin(),regions.end(),[&](const auto& r){return !r.outside || r.zone!=node.zone;}))continue;
            const auto ranges=blocked(p,center(x,y),p.z,p.z);
            if(std::any_of(ranges.begin(),ranges.end(),[&](const auto& range){return range.first<=p.z && range.second>=p.z;}))continue;
            visit(id,-1,p.z,kind);++accepted;return;
        }
        ++rejected;
    }
public:
    explicit AirReachability(Geo& value):geo(value) {
        bounds.reserve(geo.triangles.size());
        for(const auto& t:geo.triangles)bounds.push_back({{std::min({t.a.x,t.b.x,t.c.x}),std::min({t.a.y,t.b.y,t.c.y}),std::min({t.a.z,t.b.z,t.c.z})},{std::max({t.a.x,t.b.x,t.c.x}),std::max({t.a.y,t.b.y,t.c.y}),std::max({t.a.z,t.b.z,t.c.z})}});
        for(size_t b=0;b<geo.blocks.size();++b)if(geo.blocks[b])for(size_t c=0;c<64;++c)if(!(*geo.blocks[b])[c].empty()) {
            const int x=int(b/256)*8+int(c/8),y=int(b%256)*8+int(c%8);
            minX=std::min(minX,x);maxX=std::max(maxX,x);minY=std::min(minY,y);maxY=std::max(maxY,y);
        }
        minX=std::max(0,minX-2);minY=std::max(0,minY-2);maxX=std::min(2047,maxX+2);maxY=std::min(2047,maxY+2);
    }
    void run(unsigned workers=1) {
        auto measured=std::chrono::steady_clock::now();
        auto timing=[&](const char* stage){const auto now=std::chrono::steady_clock::now();std::cerr<<"STAGE air timing | "<<stage<<" | "<<std::chrono::duration_cast<std::chrono::milliseconds>(now-measured).count()<<" ms\n";measured=now;};
        const bool terrainZones=std::any_of(geo.world.terrainZones.begin(),geo.world.terrainZones.end(),[](bool value){return value;});
        for(const auto p:geo.zoneSeeds)seed(p,"ZoneInfo");
        for(const auto p:geo.seeds)seed(p,"PlayerStart");
        timing("actor seeds");
        for(int firstBlock=minX/8;firstBlock<=maxX/8;firstBlock+=8) {
            const int lastBlock=std::min(maxX/8,firstBlock+7),firstX=std::max(minX,firstBlock*8),lastX=std::min(maxX,lastBlock*8+7);
            std::vector<std::unique_ptr<PreparedBlock>> prepared(size_t(lastBlock-firstBlock+1)*256);
            std::atomic<size_t> stagedCount{0};
            if(workers>1)parallel(workers,[&](size_t index){
                if(stagedCount.load(std::memory_order_relaxed)>=262144)return;
                const size_t blockIndex=size_t(firstBlock)*256+index;
                const auto* existing=blocks[blockIndex].get();
                std::unique_ptr<PreparedBlock> block;
                thread_local QueryState workspace;
                for(int c=0;c<64;++c){
                    const int x=int(blockIndex/256)*8+c/8,y=int(blockIndex%256)*8+c%8;
                    if(x<minX || x>maxX || y<minY || y>maxY || (existing && (existing->built&(uint64_t(1)<<c))))continue;
                    const auto& floors=geo.cell(x,y);if(floors.empty() || floors.back().ceiling!=65535)continue;
                    if(!block)block=std::make_unique<PreparedBlock>();
                    fillCell(x,y,block->cells[c],block->nodes,&workspace);block->built|=uint64_t(1)<<c;
                }
                if(block){const size_t previous=stagedCount.fetch_add(block->nodes.size(),std::memory_order_relaxed);if(previous<262144 && block->nodes.size()<=262144-previous)prepared[index]=std::move(block);}
            },nullptr,prepared.size());
            for(int x=firstX;x<=lastX;++x)for(int y=minY;y<=maxY;++y) {
                const size_t blockIndex=size_t(x/8)*256+y/8,cellIndex=size_t(x%8)*8+y%8;
                auto& staged=prepared[blockIndex-size_t(firstBlock)*256];
                if(staged && (staged->built&(uint64_t(1)<<cellIndex))){
                    if(!blocks[blockIndex])blocks[blockIndex]=std::make_unique<AirBlock>();
                    auto& block=*blocks[blockIndex];auto& ids=block.cells[cellIndex];
                    for(const auto id:staged->cells[cellIndex]){if(nodes.size()>=16000000)throw std::runtime_error("Air clearance graph exceeds node budget");ids.push_back(int(nodes.size()));nodes.push_back(staged->nodes[size_t(id)]);}
                    block.built|=uint64_t(1)<<cellIndex;
                }

                const auto& floors=geo.cell(x,y);
                if(floors.empty() || floors.back().ceiling!=65535)continue;
                for(const auto id:cell(x,y)) {
                    const auto node=nodes[size_t(id)];
                    if(node.high<16384.-air::halfHeight-air::skin-0.001)continue;
                    if(terrainZones && (node.zone>=geo.world.terrainZones.size() || !geo.world.terrainZones[node.zone]))continue;
                    const double z=node.high-air::skin;
                    if(collision::bspEmpty(geo.world,center(x,y,z)))visit(id,-1,z,"sky");
                }
            }
        }
        timing("sky cells and seeds");
        constexpr int dx[]{1,-1,0,0},dy[]{0,0,1,-1};
        size_t processed=0;
        size_t next=0;
        while(next<pending.size()) {
            const int parent=pending[next++];const auto from=nodes[size_t(parent)];
            for(int d=0;d<4;++d) {
                const int nx=from.x+dx[d],ny=from.y+dy[d];
                for(const auto id:cell(nx,ny)) {
                    const auto to=nodes[size_t(id)];if(to.parent!=-2 || to.zone!=from.zone)continue;
                    const double low=std::max(from.low,to.low),high=std::min(from.high,to.high);
                    if(low>=high)continue;
                    ++edges;
                    const auto free=air::freeRanges(low,high,blocked(center(from.x,from.y),center(nx,ny),low,high));
                    bool moved=false;
                    for(const auto gap:free) {
                        for(const auto z:std::array<double,3>{(gap.first+gap.second)/2.,gap.first+(gap.second-gap.first)*0.001,gap.second-(gap.second-gap.first)*0.001}) {
                            const auto& regions=bsp(center(from.x,from.y,z),center(nx,ny,z));
                            if(std::all_of(regions.begin(),regions.end(),[&](const auto& region){return region.outside && region.zone==from.zone;})){visit(id,parent,z);moved=true;break;}
                        }
                        if(moved)break;
                    }
                }
            }
            if(++processed%262144==0)std::cerr<<"STAGE cylinder air | "<<processed<<" spans visited, "<<added<<" additional floors\n";
        }
        timing("graph traversal");
        std::cerr<<"STAGE cylinder air | radius="<<air::radius<<", half-height="<<air::halfHeight<<", "<<added<<" additional floors, "<<accepted<<" actor seeds accepted, "<<rejected<<" rejected, "<<edges<<" swept edges\n";
        for(const auto p:airTracePoints) {
            const int x=int(std::floor((p.x-geo.ox)/16.)),y=int(std::floor((p.y-geo.oy)/16.));int found=-1;
            for(const auto id:cell(x,y)){const auto& node=nodes[size_t(id)];if(node.low<=p.z && p.z<=node.high && node.parent!=-2){found=id;break;}}
            std::ostringstream trace;trace<<"AIRTRACE point="<<p.x<<','<<p.y<<','<<p.z<<" reachable="<<(found>=0)<<'\n';
            size_t steps=0;
            while(found>=0 && steps++<nodes.size()) {
                const auto& node=nodes[size_t(found)];const auto point=center(node.x,node.y,node.entry);
                if(steps<=32 || node.seed) {
                    trace<<"AIRTRACE step="<<steps<<" center="<<point.x<<','<<point.y<<','<<point.z<<" interval="<<node.low<<','<<node.high<<" zone="<<int(node.zone);
                    if(node.seed)trace<<" seed="<<node.seed;
                    trace<<'\n';
                }
                found=node.parent;
            }
            trace<<"AIRTRACE total_steps="<<steps<<'\n';std::cerr<<trace.str();
        }
    }
};
