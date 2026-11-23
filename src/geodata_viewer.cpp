#include "geodata_viewer.h"
#include "geodata_document.h"
#include "collision.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <cfloat>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <map>
#include <stdexcept>
#include <iostream>
#include <regex>
#include <set>
#include <uxtheme.h>
#include <objidl.h>
namespace Gdiplus { using std::min;using std::max; }
#include <gdiplus.h>
namespace extractor {
namespace {
using Microsoft::WRL::ComPtr;
using namespace DirectX;
using viewer::Document;
#include "ui_theme.h"
constexpr UINT loadedMessage=WM_APP+510;
HWND labWindow=nullptr;
constexpr COLORREF back=theme::base,panel=theme::surface,ink=theme::text,muted=theme::muted,accent=theme::accent;
enum Control {Open=2101,Home,Focus,Mode,Grid,Arrows,OneWay,MinZ,MaxZ,Apply,All,Isolate,Range,Info,Minimize,Maximize,Close,ShowCells,ShowTerrain,ShowMeshes,ShowVolumes,ShowBsp,Antialiasing};
void checked(HRESULT hr,const char* message) {if(FAILED(hr)){std::ostringstream text;text<<message<<" (0x"<<std::hex<<unsigned(hr)<<')';throw std::runtime_error(text.str());}}
std::wstring wide(const std::string& s){if(s.empty())return {};const int n=MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),nullptr,0);std::wstring result(n,0);MultiByteToWideChar(CP_UTF8,0,s.data(),int(s.size()),result.data(),n);return result;}
struct Instance {float x,y,z;uint32_t flags,oneWay,id;};
struct Chunk {uint32_t first=0,count=0;float low=0,high=0;};
struct Constants {XMFLOAT4X4 matrix;XMFLOAT4 camera,limits;XMUINT4 options;};
struct MeshVertex {XMFLOAT3 position,normal;uint32_t kind;};
struct MeshBatch {uint32_t first=0,count=0;XMFLOAT3 low{FLT_MAX,FLT_MAX,FLT_MAX},high{-FLT_MAX,-FLT_MAX,-FLT_MAX};};
struct MapGeometry {std::vector<MeshVertex> vertices;std::array<MeshBatch,4096> batches;ComPtr<ID3D11Buffer> buffer;uint32_t kindMask=0;std::string notice;};
struct Scene {std::shared_ptr<Document> document;ComPtr<ID3D11Buffer> buffer,lodBuffer;std::vector<Instance> instances;std::vector<MeshVertex> lod;std::array<Chunk,1024> chunks;std::array<MeshBatch,1024> lodBatches;std::shared_ptr<MapGeometry> geometry;XMFLOAT3 anchor{};};
void extend(MeshBatch& batch,const XMFLOAT3& p){batch.low={std::min(batch.low.x,p.x),std::min(batch.low.y,p.y),std::min(batch.low.z,p.z)};batch.high={std::max(batch.high.x,p.x),std::max(batch.high.y,p.y),std::max(batch.high.z,p.z)};}
struct Frustum {
    std::array<XMFLOAT4,6> planes{};
    void update(const Constants& cb){
        const auto& m=cb.matrix;
        planes={XMFLOAT4{m._41+m._11,m._42+m._12,m._43+m._13,m._44+m._14},XMFLOAT4{m._41-m._11,m._42-m._12,m._43-m._13,m._44-m._14},XMFLOAT4{m._41+m._21,m._42+m._22,m._43+m._23,m._44+m._24},XMFLOAT4{m._41-m._21,m._42-m._22,m._43-m._23,m._44-m._24},XMFLOAT4{m._31,m._32,m._33,m._34},XMFLOAT4{m._41-m._31,m._42-m._32,m._43-m._33,m._44-m._34}};
    }
    bool visible(const MeshBatch& batch,const Constants& cb) const {
        if(!batch.count || batch.high.z<cb.limits.x || batch.low.z>cb.limits.y)return false;
        const float dx=std::max({batch.low.x-cb.camera.x,0.f,cb.camera.x-batch.high.x}),dy=std::max({batch.low.y-cb.camera.y,0.f,cb.camera.y-batch.high.y});if(dx*dx+dy*dy>cb.limits.z*cb.limits.z)return false;
        for(const auto& p:planes){const float x=p.x>=0 ? batch.high.x:batch.low.x,y=p.y>=0 ? batch.high.y:batch.low.y,z=p.z>=0 ? batch.high.z+64:batch.low.z;const float margin=std::abs(p.x)+std::abs(p.y)+std::abs(p.z);if(p.x*x+p.y*y+p.z*z+p.w< -margin)return false;}
        return true;
    }
};
std::shared_ptr<MapGeometry> prepareGeometry(const std::filesystem::path& path,const Document& doc,std::stop_token stop){
    auto result=std::make_shared<MapGeometry>();const auto map=collision::readViewport(path,stop);if(stop.stop_requested())throw std::runtime_error("Geometry loading cancelled");
    auto group=[&](const collision::Triangle& t){const int x=std::clamp(int(std::floor(((t.a.x+t.b.x+t.c.x)/3-doc.originX)/1024)),0,31),y=std::clamp(int(std::floor(((t.a.y+t.b.y+t.c.y)/3-doc.originY)/1024)),0,31);return unsigned(t.kind)*1024+unsigned(x*32+y);};
    for(const auto& t:map.triangles){result->batches[group(t)].count+=3;result->kindMask|=1u<<t.kind;}uint32_t count=0;for(auto& batch:result->batches){batch.first=count;count+=batch.count;}result->vertices.resize(count);std::array<uint32_t,4096> cursor{};
    std::map<std::array<double,3>,collision::Point> terrainNormals;
    for(const auto& t:map.triangles)if(t.kind==1)for(const auto& point:{t.a,t.b,t.c}){auto& normal=terrainNormals[{point.x,point.y,point.z}];normal.x+=t.normal.x;normal.y+=t.normal.y;normal.z+=t.normal.z;}
    size_t done=0;for(const auto& t:map.triangles){if((done++&16383)==0 && stop.stop_requested())throw std::runtime_error("Geometry preparation cancelled");const auto index=group(t);auto& batch=result->batches[index];const auto length=std::sqrt(t.normal.x*t.normal.x+t.normal.y*t.normal.y+t.normal.z*t.normal.z);const XMFLOAT3 n=length>1e-12 ? XMFLOAT3{float(t.normal.x/length),float(t.normal.y/length),float(t.normal.z/length)}:XMFLOAT3{0,0,1};for(const auto& p:{t.a,t.b,t.c}){const XMFLOAT3 position{float(p.x),float(p.y),float(p.z)};XMFLOAT3 smooth=n;if(t.kind==1){const auto& sum=terrainNormals.at({p.x,p.y,p.z});const auto size=std::sqrt(sum.x*sum.x+sum.y*sum.y+sum.z*sum.z);if(size>1e-12)smooth={float(sum.x/size),float(sum.y/size),float(sum.z/size)};}result->vertices[batch.first+cursor[index]++]={position,smooth,t.kind};extend(batch,position);}}
    if(!map.errors.empty())result->notice="Geometry incomplete: "+std::to_string(map.errors.size())+" errors; "+map.errors.front();else if(!map.warnings.empty())result->notice="Geometry: "+std::to_string(map.warnings.size())+" warnings; "+map.warnings.front();return result;
}
const char* shader=R"(
cbuffer View : register(b0) {float4x4 transform;float4 camera;float4 limits;uint4 options;}
struct Input {float3 p:POSITION;uint flags:FLAGS;uint diagnostic:DIAGNOSTIC;uint id:CELL;};
struct Pixel {float4 p:SV_POSITION;float2 uv:TEXCOORD;nointerpolation uint flags:FLAGS;nointerpolation uint diagnostic:DIAGNOSTIC;nointerpolation uint id:CELL;};
Pixel vertex(Input input,uint index:SV_VertexID) {
    float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    Pixel o;o.uv=corners[index];o.flags=input.flags;o.diagnostic=input.diagnostic;o.id=input.id;
    float3 position=input.p+float3(o.uv*16,64*(1-limits.w)+((options.y&2)!=0 ? 8:0));o.p=mul(float4(position,1),transform);
    if(input.p.z<limits.x || input.p.z>limits.y || distance(input.p.xy+8,camera.xy)>limits.z)o.p=float4(2,2,2,1);
    return o;
}
float arrow(float2 uv,float2 direction) {
    float2 offset=uv-.5;float2 q=float2(dot(offset,direction),dot(offset,float2(-direction.y,direction.x)));
    return ((q.x>.02 && q.x<.23 && abs(q.y)<.022) || (q.x>.16 && q.x<.36 && abs(q.y)<(.36-q.x)*.65)) ? 1:0;
}
float4 pixel(Pixel input):SV_TARGET {
    bool overlay=(options.y&2)!=0;float alpha=overlay ? .10:1;
    float2 uv=input.uv;float3 color=input.flags==15 ? float3(.10,.23,.26):float3(.17,.20,.26);
    if(input.flags==0)color=float3(.25,.16,.20);
    if(options.x==input.id){color=float3(1,.57,.18);if(overlay)alpha=.65;}
    float footprint=max(fwidth(uv.x),fwidth(uv.y));
    float detail=smoothstep(4,16,1/max(footprint,.0001));
    float border=min(.07,footprint*1.1);
    float edgeDistance=min(min(uv.x,1-uv.x),min(uv.y,1-uv.y));
    if((options.y&1)!=0){float gridLine=(1-smoothstep(border,border+footprint,edgeDistance))*detail;color=lerp(color,float3(.34,.64,.65),gridLine);if(overlay)alpha=max(alpha,gridLine*.5);}
    float2 dirs[4]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1)};
    uint bits[4]={1,2,4,8};
    for(uint d=0;d<4;d++) {
        bool blocked=(input.flags&bits[d])==0;bool asymmetric=options.w!=0 && (input.diagnostic&bits[d])!=0;
        float3 mark=asymmetric ? float3(1,.67,.18):blocked ? float3(1,.29,.25):float3(.31,1,.76);
        bool edge=(d==0 && uv.x>.945 && uv.y>.22 && uv.y<.78) || (d==1 && uv.x<.055 && uv.y>.22 && uv.y<.78) || (d==2 && uv.y>.945 && uv.x>.22 && uv.x<.78) || (d==3 && uv.y<.055 && uv.x>.22 && uv.x<.78);
        if(edge && (blocked || asymmetric)){color=lerp(color,mark,detail);if(overlay)alpha=max(alpha,detail*.9);}
        if(options.z!=0 && !blocked && arrow(uv,dirs[d])>0){color=lerp(color,mark,detail);if(overlay)alpha=max(alpha,detail*.85);}
    }
    color=lerp(float3(.035,.047,.065),color,smoothstep(0,1,limits.w));
    return float4(color,alpha*smoothstep(0,1,limits.w));
}
)";
const char* geometryShader=R"(
cbuffer View : register(b0) {float4x4 transform;float4 camera;float4 limits;uint4 options;}
struct Input {float3 p:POSITION;float3 n:NORMAL;uint kind:KIND;};
struct Pixel {float4 p:SV_POSITION;float3 world:POSITION;float3 n:NORMAL;nointerpolation uint kind:KIND;};
Pixel vertex(Input input){Pixel o;o.p=mul(float4(input.p,1),transform);o.world=input.p;o.n=input.n;o.kind=input.kind;return o;}
float4 pixel(Pixel input,bool front:SV_IsFrontFace):SV_TARGET {
    float distanceToEye=distance(input.world,camera.xyz);
    if(input.world.z<limits.x || input.world.z>limits.y || distance(input.world.xy,camera.xy)>limits.z)discard;
    float3 normal=normalize(input.n);if(!front)normal=-normal;
    float3 colors[5]={float3(.18,.43,.78),float3(.22,.46,.23),float3(.68,.27,.88),float3(.80,.42,.14),float3(.12,.38,.37)};
    float3 base=colors[min(input.kind,4u)];
    if(input.kind==1){float rock=1-smoothstep(.40,.85,normal.z);base=lerp(base,float3(.34,.43,.29),rock*.65);base*=lerp(.94,1.08,saturate((input.world.z+4096)/8192));}
    float3 key=normalize(float3(-.4,-.35,.85));float diffuse=max(0,dot(normal,key));float fill=max(0,dot(normal,normalize(float3(.7,.2,.4))));
    float3 sky=lerp(float3(.14,.12,.10),float3(.27,.32,.40),normal.z*.5+.5);
    float3 color=base*(sky+diffuse*.85+fill*.13);
    float rim=pow(1-saturate(dot(normal,normalize(camera.xyz-input.world))),3);color+=float3(.045,.055,.07)*rim;
    color=pow(saturate(color/(1+color)*1.35),1/2.2);
    float fog=1-exp(-pow(distanceToEye/max(limits.z*.8,8192),1.7)*.65);
    color=lerp(color,float3(.045,.062,.085),min(fog,.82));
    return float4(color,input.kind==4 && (options.y&2)!=0 ? .12:1);
}
)";
class Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11DepthStencilView> depth;
    ComPtr<ID3D11Texture2D> backbuffer,colorSurface;
    ComPtr<ID3D11DepthStencilState> solidDepth,overlayDepth;
    ComPtr<ID3D11BlendState> overlayBlend;
    UINT samples=1;
    Frustum frustum;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11RasterizerState> raster,wire,overlayRaster;
    ComPtr<ID3D11VertexShader> meshVs;ComPtr<ID3D11PixelShader> meshPs;ComPtr<ID3D11InputLayout> meshLayout;
public:
    int width=1,height=1;
    void initialize(HWND window) {
        DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=2;desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.OutputWindow=window;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL level{};const D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        HRESULT result=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,1,D3D11_SDK_VERSION,&desc,&swap,&device,&level,&context);
        if(FAILED(result))result=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels,1,D3D11_SDK_VERSION,&desc,&swap,&device,&level,&context);
        checked(result,"Cannot initialize Direct3D 11 geodata viewport");
        for(const UINT requested:{2u}){UINT color=0,z=0;if(SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,requested,&color)) && SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT,requested,&z)) && color && z){samples=requested;break;}}ComPtr<IDXGIDevice1> latency;if(SUCCEEDED(device.As(&latency)))checked(latency->SetMaximumFrameLatency(1),"Cannot set viewport frame latency");
        ComPtr<IDXGIFactory> factory;checked(swap->GetParent(IID_PPV_ARGS(&factory)),"Cannot access viewport factory");checked(factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER|DXGI_MWA_NO_WINDOW_CHANGES),"Cannot configure viewport window");
        ComPtr<ID3DBlob> vertexCode,pixelCode,errors;
        auto compile=[&](const char* entry,const char* profile,ComPtr<ID3DBlob>& code){const auto hr=D3DCompile(shader,std::strlen(shader),"MapExtractor geodata",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);if(FAILED(hr))throw std::runtime_error(errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Cannot compile viewer shader");};
        compile("vertex","vs_5_0",vertexCode);compile("pixel","ps_5_0",pixelCode);
        checked(device->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&vs),"Cannot create viewport vertex shader");checked(device->CreatePixelShader(pixelCode->GetBufferPointer(),pixelCode->GetBufferSize(),nullptr,&ps),"Cannot create viewport pixel shader");
        const D3D11_INPUT_ELEMENT_DESC fields[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_INSTANCE_DATA,1},{"FLAGS",0,DXGI_FORMAT_R32_UINT,0,12,D3D11_INPUT_PER_INSTANCE_DATA,1},{"DIAGNOSTIC",0,DXGI_FORMAT_R32_UINT,0,16,D3D11_INPUT_PER_INSTANCE_DATA,1},{"CELL",0,DXGI_FORMAT_R32_UINT,0,20,D3D11_INPUT_PER_INSTANCE_DATA,1}};
        checked(device->CreateInputLayout(fields,4,vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),&layout),"Cannot create viewport input layout");
        const auto compileMesh=[&](const char* entry,const char* profile,ComPtr<ID3DBlob>& code){const auto hr=D3DCompile(geometryShader,std::strlen(geometryShader),"MapExtractor geometry",nullptr,nullptr,entry,profile,D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);if(FAILED(hr))throw std::runtime_error(errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Cannot compile geometry shader");};
        compileMesh("vertex","vs_5_0",vertexCode);compileMesh("pixel","ps_5_0",pixelCode);checked(device->CreateVertexShader(vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),nullptr,&meshVs),"Cannot create geometry shader");checked(device->CreatePixelShader(pixelCode->GetBufferPointer(),pixelCode->GetBufferSize(),nullptr,&meshPs),"Cannot create geometry pixel shader");
        const D3D11_INPUT_ELEMENT_DESC meshFields[]={{"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},{"NORMAL",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},{"KIND",0,DXGI_FORMAT_R32_UINT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0}};checked(device->CreateInputLayout(meshFields,3,vertexCode->GetBufferPointer(),vertexCode->GetBufferSize(),&meshLayout),"Cannot create geometry layout");
        D3D11_BUFFER_DESC cb{};cb.ByteWidth=sizeof(Constants);cb.Usage=D3D11_USAGE_DEFAULT;cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;checked(device->CreateBuffer(&cb,nullptr,&constants),"Cannot create viewport constants");
        D3D11_RASTERIZER_DESC state{};state.FillMode=D3D11_FILL_SOLID;state.CullMode=D3D11_CULL_NONE;state.DepthClipEnable=TRUE;state.MultisampleEnable=TRUE;checked(device->CreateRasterizerState(&state,&raster),"Cannot create viewport rasterizer");state.FillMode=D3D11_FILL_WIREFRAME;checked(device->CreateRasterizerState(&state,&wire),"Cannot create volume wireframe");state.FillMode=D3D11_FILL_SOLID;state.DepthBias=1;checked(device->CreateRasterizerState(&state,&overlayRaster),"Cannot create geodata overlay rasterizer");
        D3D11_DEPTH_STENCIL_DESC depthState{};depthState.DepthEnable=TRUE;depthState.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;depthState.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;checked(device->CreateDepthStencilState(&depthState,&solidDepth),"Cannot create reversed depth state");depthState.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;checked(device->CreateDepthStencilState(&depthState,&overlayDepth),"Cannot create overlay depth state");
        D3D11_BLEND_DESC blend{};auto& rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;rt.SrcBlend=D3D11_BLEND_SRC_ALPHA;rt.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOp=D3D11_BLEND_OP_ADD;rt.SrcBlendAlpha=D3D11_BLEND_ONE;rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;checked(device->CreateBlendState(&blend,&overlayBlend),"Cannot create overlay blending");resize(1,1);
    }
    UINT sampleCount() const {return samples;}
    bool supportsSamples(UINT requested) const {UINT color=0,z=0;return requested==1 || (SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,requested,&color)) && SUCCEEDED(device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT,requested,&z)) && color && z);}
    void cycleSamples(){const UINT previous=samples;UINT next=samples==1 ? 2:samples==2 ? 4:1;while(next!=samples && !supportsSamples(next))next=next==2 ? 4:1;if(next==samples)return;samples=next;try{resize(width,height);}catch(...){samples=previous;resize(width,height);throw;}}
    void resolve(){if(samples>1 && colorSurface && backbuffer)context->ResolveSubresource(backbuffer.Get(),0,colorSurface.Get(),0,DXGI_FORMAT_R8G8B8A8_UNORM);}
    void style(bool overlay,bool wireframe=false){context->OMSetDepthStencilState(overlay ? overlayDepth.Get():solidDepth.Get(),0);context->OMSetBlendState(overlay ? overlayBlend.Get():nullptr,nullptr,0xffffffff);context->RSSetState(wireframe ? wire.Get():overlay ? overlayRaster.Get():raster.Get());}
    void resize(int w,int h) {
        if(!swap || w<=0 || h<=0)return;width=w;height=h;context->OMSetRenderTargets(0,nullptr,nullptr);target.Reset();depth.Reset();colorSurface.Reset();backbuffer.Reset();checked(swap->ResizeBuffers(0,UINT(w),UINT(h),DXGI_FORMAT_UNKNOWN,0),"Cannot resize viewport");
        checked(swap->GetBuffer(0,IID_PPV_ARGS(&backbuffer)),"Cannot access viewport backbuffer");
        D3D11_TEXTURE2D_DESC td{};td.Width=UINT(w);td.Height=UINT(h);td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=samples;td.BindFlags=D3D11_BIND_RENDER_TARGET;
        if(samples>1)checked(device->CreateTexture2D(&td,nullptr,&colorSurface),"Cannot create antialiased viewport");else colorSurface=backbuffer;
        checked(device->CreateRenderTargetView(colorSurface.Get(),nullptr,&target),"Cannot create viewport target");td.Format=DXGI_FORMAT_D32_FLOAT;td.BindFlags=D3D11_BIND_DEPTH_STENCIL;ComPtr<ID3D11Texture2D> buffer;checked(device->CreateTexture2D(&td,nullptr,&buffer),"Cannot create viewport depth buffer");checked(device->CreateDepthStencilView(buffer.Get(),nullptr,&depth),"Cannot create viewport depth view");
    }
    static std::unique_ptr<Scene> prepare(std::shared_ptr<Document> document,std::stop_token stop) {
        auto scene=std::make_unique<Scene>();scene->document=std::move(document);const auto& doc=*scene->document;
        for(const auto& cell:doc.cells)if(!Document::isVoid(cell))++scene->chunks[(cell.key.x/64)*32+cell.key.y/64].count;
        uint32_t count=0;for(auto& chunk:scene->chunks){chunk.first=count;count+=chunk.count;chunk.low=32768;chunk.high=-32768;}
        scene->instances.resize(count);std::array<uint32_t,1024> cursor{};float best=std::numeric_limits<float>::max();
        for(uint32_t i=0;i<doc.cells.size();++i) {
            if((i&16383)==0 && stop.stop_requested())throw std::runtime_error("Geodata preparation cancelled");
            const auto& c=doc.cells[i];if(Document::isVoid(c))continue;const auto group=(c.key.x/64)*32+c.key.y/64;auto& chunk=scene->chunks[group];
            unsigned diagnostic=doc.oneWay(i);if(c.key.x==2047)diagnostic&=~1u;if(c.key.x==0)diagnostic&=~2u;if(c.key.y==2047)diagnostic&=~4u;if(c.key.y==0)diagnostic&=~8u;
            scene->instances[chunk.first+cursor[group]++]={doc.originX+c.key.x*16.f,doc.originY+c.key.y*16.f,float(c.height),c.nswe,diagnostic,i};chunk.low=std::min(chunk.low,float(c.height));chunk.high=std::max(chunk.high,float(c.height));
            const float dx=c.key.x*16.f+8-16384,dy=c.key.y*16.f+8-16384,distance=dx*dx+dy*dy;
            if(distance<best || (distance==best && c.height>scene->anchor.z)){best=distance;scene->anchor={doc.originX+c.key.x*16.f+8,doc.originY+c.key.y*16.f+8,float(c.height)};}
        }
        for(unsigned group=0;group<1024;++group){
            if(stop.stop_requested())throw std::runtime_error("Geodata LOD preparation cancelled");if(!scene->chunks[group].count)continue;
            std::array<int16_t,4096> heights{};bool valid=true;const unsigned startX=(group/32)*64,startY=(group%32)*64;
            for(unsigned x=0;x<64 && valid;++x)for(unsigned y=0;y<64;++y){const viewer::CellKey key{uint16_t(startX+x),uint16_t(startY+y),0};const auto index=doc.find(key);if(!index || Document::isVoid(doc.cells[*index]) || doc.find({key.x,key.y,1})){valid=false;break;}heights[x*64+y]=doc.cells[*index].height;}
            for(unsigned x=0;x<64 && valid;x+=8)for(unsigned y=0;y<64;y+=8){int low=32767,high=-32768;for(unsigned a=0;a<8;++a)for(unsigned b=0;b<8;++b){const int z=heights[(x+a)*64+y+b];low=std::min(low,z);high=std::max(high,z);}if(high-low>256){valid=false;break;}}
            if(!valid)continue;auto& batch=scene->lodBatches[group];batch.first=uint32_t(scene->lod.size());
            auto face=[&](XMFLOAT3 a,XMFLOAT3 b,XMFLOAT3 c){XMFLOAT3 n;XMStoreFloat3(&n,XMVector3Normalize(XMVector3Cross(XMLoadFloat3(&b)-XMLoadFloat3(&a),XMLoadFloat3(&c)-XMLoadFloat3(&a))));for(const auto& p:{a,b,c}){scene->lod.push_back({p,n,4});extend(batch,p);++batch.count;}};
            for(unsigned x=0;x<64;x+=8)for(unsigned y=0;y<64;y+=8){const float wx=doc.originX+float(startX+x)*16,wy=doc.originY+float(startY+y)*16;const XMFLOAT3 a{wx,wy,float(heights[x*64+y])},b{wx+128,wy,float(heights[(x+7)*64+y])},c{wx,wy+128,float(heights[x*64+y+7])},d{wx+128,wy+128,float(heights[(x+7)*64+y+7])};face(a,b,c);face(c,b,d);}
        }
        return scene;
    }
    void uploadMesh(std::vector<MeshVertex>& vertices,ComPtr<ID3D11Buffer>& targetBuffer){
        if(vertices.empty())return;D3D11_BUFFER_DESC desc{};desc.ByteWidth=UINT(vertices.size()*sizeof(MeshVertex));desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA data{};data.pSysMem=vertices.data();checked(device->CreateBuffer(&desc,&data,&targetBuffer),"Cannot upload map geometry");std::vector<MeshVertex>().swap(vertices);
    }
    void bindMesh(ID3D11Buffer* buffer,const Constants& cb,bool wireframe=false){
        context->UpdateSubresource(constants.Get(),0,nullptr,&cb,0,0);ID3D11Buffer* constant=constants.Get();context->VSSetConstantBuffers(0,1,&constant);context->PSSetConstantBuffers(0,1,&constant);context->VSSetShader(meshVs.Get(),nullptr,0);context->PSSetShader(meshPs.Get(),nullptr,0);context->IASetInputLayout(meshLayout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);const UINT stride=sizeof(MeshVertex),offset=0;context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);style((cb.options.y&2)!=0,wireframe);
    }
    void drawGeometry(const Scene& scene,const Constants& cb,const std::array<bool,4>& kinds){
        if(!scene.geometry || !scene.geometry->buffer)return;
        for(unsigned kind=0;kind<4;++kind)if(kinds[kind]){
            bindMesh(scene.geometry->buffer.Get(),cb,kind==2);uint32_t first=0,count=0;
            auto flush=[&]{if(count)context->Draw(count,first);count=0;};
            for(unsigned group=0;group<1024;++group){const auto& batch=scene.geometry->batches[kind*1024+group];if(!frustum.visible(batch,cb)){if(batch.count)flush();continue;}if(count && first+count!=batch.first)flush();if(!count)first=batch.first;count+=batch.count;}flush();
        }
    }
    void upload(Scene& scene) {
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=UINT(scene.instances.size()*sizeof(Instance));buffer.Usage=D3D11_USAGE_IMMUTABLE;buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;D3D11_SUBRESOURCE_DATA data{};data.pSysMem=scene.instances.data();checked(device->CreateBuffer(&buffer,&data,&scene.buffer),"Geodata is too large for this graphics device");uploadMesh(scene.lod,scene.lodBuffer);
    }
    static bool visible(const Chunk& chunk,unsigned group,const XMFLOAT3& eye,float range,float lo,float hi,int originX=0,int originY=0) {
        const float x=originX+float(group/32)*1024,y=originY+float(group%32)*1024;
        const float dx=std::max({x-eye.x,0.f,eye.x-x-1024}),dy=std::max({y-eye.y,0.f,eye.y-y-1024});return chunk.count && chunk.high>=lo && chunk.low<=hi && dx*dx+dy*dy<=range*range;
    }
    void draw(const Scene* scene,const Constants& cb,bool present=true,bool clear=true) {
        if(!target)return;const float color[]{.045f,.062f,.085f,1};if(clear){frustum.update(cb);context->ClearRenderTargetView(target.Get(),color);context->ClearDepthStencilView(depth.Get(),D3D11_CLEAR_DEPTH,0,0);}ID3D11RenderTargetView* output=target.Get();context->OMSetRenderTargets(1,&output,depth.Get());
        D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};context->RSSetViewports(1,&viewport);style(false);
        if(scene) {
            style((cb.options.y&2)!=0);
            context->UpdateSubresource(constants.Get(),0,nullptr,&cb,0,0);ID3D11Buffer* buffer=constants.Get();context->VSSetConstantBuffers(0,1,&buffer);context->PSSetConstantBuffers(0,1,&buffer);context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->IASetInputLayout(layout.Get());context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const UINT stride=sizeof(Instance),offset=0;buffer=scene->buffer.Get();context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);
            bool meshBound=false;
            for(unsigned group=0;group<1024;++group)if(visible(scene->chunks[group],group,{cb.camera.x,cb.camera.y,cb.camera.z},cb.limits.z,cb.limits.x,cb.limits.y,scene->document->originX,scene->document->originY)){
                const auto& chunk=scene->chunks[group];MeshBatch bounds;bounds.count=chunk.count;const float x=scene->document->originX+float(group/32)*1024,y=scene->document->originY+float(group%32)*1024;bounds.low={x,y,chunk.low};bounds.high={x+1024,y+1024,chunk.high};if(!frustum.visible(bounds,cb))continue;
                const float dx=x+512-cb.camera.x,dy=y+512-cb.camera.y,dz=(chunk.low+chunk.high)*.5f-cb.camera.z;const auto& lod=scene->lodBatches[group];
                if((cb.options.y&4)!=0 && dx*dx+dy*dy+dz*dz>3072.f*3072.f)continue;
                if(scene->lodBuffer && lod.count && dx*dx+dy*dy+dz*dz>3072.f*3072.f){if(!meshBound){bindMesh(scene->lodBuffer.Get(),cb);meshBound=true;}context->Draw(lod.count,lod.first);}
                else{if(meshBound){context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);context->IASetInputLayout(layout.Get());buffer=scene->buffer.Get();context->IASetVertexBuffers(0,1,&buffer,&stride,&offset);style((cb.options.y&2)!=0);meshBound=false;}context->DrawInstanced(6,chunk.count,0,chunk.first);}
            }
        }
        if(present){resolve();const auto hr=swap->Present(1,0);if(hr!=DXGI_STATUS_OCCLUDED)checked(hr,"Viewport graphics device stopped");}
    }
    size_t renderedPixels() {
        resolve();
        ComPtr<ID3D11Texture2D> surface,staging;checked(swap->GetBuffer(0,IID_PPV_ARGS(&surface)),"Cannot read viewport backbuffer");
        D3D11_TEXTURE2D_DESC desc{};surface->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;checked(device->CreateTexture2D(&desc,nullptr,&staging),"Cannot create viewport readback");
        context->CopyResource(staging.Get(),surface.Get());D3D11_MAPPED_SUBRESOURCE mapped{};checked(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Cannot map viewport readback");
        size_t count=0;for(int y=0;y<height;++y){const auto* row=static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch;for(int x=0;x<width;++x)if(row[x*4]>20 || row[x*4+1]>20 || row[x*4+2]>24)++count;}context->Unmap(staging.Get(),0);return count;
    }
};
struct Region {GeodataRegion source;int x=0,y=0;};
using Scenes=std::vector<std::shared_ptr<Scene>>;
struct Loading {std::mutex mutex;Scenes result;std::vector<std::pair<std::shared_ptr<Scene>,std::shared_ptr<MapGeometry>>> geometry;std::string error;bool ready=false;};
LRESULT CALLBACK labHover(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR) {
    if(message==WM_MOUSEMOVE){TRACKMOUSEEVENT event{sizeof(event),TME_LEAVE,window,0};TrackMouseEvent(&event);InvalidateRect(window,nullptr,FALSE);}
    if(message==WM_MOUSELEAVE || message==WM_SETFOCUS || message==WM_KILLFOCUS)InvalidateRect(window,nullptr,FALSE);
    if(message==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
    return DefSubclassProc(window,message,wp,lp);
}
class Viewer {
public:
    HWND window=nullptr,viewport=nullptr,owner=nullptr;
    Renderer renderer;
    Scenes scenes;
    std::shared_ptr<Scene> scene;
    std::vector<Region> regions;
    std::vector<std::filesystem::path> requested;
    std::filesystem::path frameTarget;
    std::map<int,HWND> controls;
    std::jthread loader;
    std::shared_ptr<Loading> loading;
    ULONG_PTR graphicsToken=0;
    HFONT font=nullptr,titleFont=nullptr,headingFont=nullptr;
    HBRUSH background=CreateSolidBrush(back),field=CreateSolidBrush(theme::inset);
    std::wstring state=L"Select maps in the main window, then open View Geodata.";
    float yaw=.8f,pitch=-.65f,speed=1600,range=65536,minZ=-32768,maxZ=32767,orbitDistance=1200;
    XMFLOAT3 eye{0,0,1000},pivot{};
    uint32_t selected=UINT32_MAX;
    int drag=0;unsigned fps=0,frameCount=0;
    POINT restore{};
    bool orbit=false,grid=true,arrows=true,asymmetric=false,failed=false,animations=true,busy=false,cellsVisible=true;
    std::array<bool,4> geometryVisible{};
    std::chrono::steady_clock::time_point reveal=std::chrono::steady_clock::now(),last=reveal,fpsStart=reveal;
    ~Viewer(){loader.request_stop();if(loader.joinable())loader.join();for(const auto face:{font,titleFont,headingFont})if(face)DeleteObject(face);DeleteObject(background);DeleteObject(field);if(graphicsToken)Gdiplus::GdiplusShutdown(graphicsToken);}
    int px(int value) const {return MulDiv(value,int(GetDpiForWindow(window)),96);}
    void fonts() {
        uiScale=float(GetDpiForWindow(window))/96;
        for(const auto face:{font,titleFont,headingFont})if(face)DeleteObject(face);
        auto make=[&](int size,int weight,const wchar_t* face){return CreateFontW(-px(size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,face);};
        font=make(14,FW_NORMAL,L"Segoe UI Variable Text");titleFont=make(20,FW_SEMIBOLD,L"Segoe UI Variable Display");headingFont=make(13,FW_BOLD,L"Segoe UI Variable Text");
        for(const auto& entry:controls)SendMessageW(entry.second,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    }
    HWND control(int id,const wchar_t* type,const wchar_t* text,DWORD style=0) {
        const auto result=CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,1,1,window,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        if(!result)throw std::runtime_error("Cannot create geodata viewer control");controls[id]=result;SendMessageW(result,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
        if(wcscmp(type,L"BUTTON")==0)SetWindowSubclass(result,labHover,1,0);else SetWindowTheme(result,L"DarkMode_Explorer",nullptr);return result;
    }
    void create();
    void layout() {
        if(controls.empty())return;RECT r{};GetClientRect(window,&r);
        const int side=px(300),top=px(182),bottom=px(40);MoveWindow(viewport,side,top,std::max(1,int(r.right)-side-px(16)),std::max(1,int(r.bottom)-top-bottom),TRUE);
        auto place=[&](int id,int x,int y,int w,int h){MoveWindow(controls.at(id),px(x),px(y),px(w),px(h),TRUE);};
        place(Open,316,96,250,32);place(Home,578,96,100,32);place(Focus,690,96,112,32);place(Antialiasing,814,96,128,32);
        MoveWindow(controls.at(Minimize),r.right-px(126),px(18),px(32),px(32),TRUE);MoveWindow(controls.at(Close),r.right-px(50),px(18),px(32),px(32),TRUE);MoveWindow(controls.at(Maximize),r.right-px(88),px(18),px(32),px(32),TRUE);
        const int rowWidth=int(r.right)-px(332),buttonWidth=(rowWidth-px(32))/5;const int layers[]{ShowCells,ShowTerrain,ShowMeshes,ShowVolumes,ShowBsp};for(int i=0;i<5;++i)MoveWindow(controls.at(layers[i]),px(316)+i*(buttonWidth+px(8)),px(138),buttonWidth,px(32),TRUE);
        place(Mode,36,126,228,34);place(Grid,36,210,228,32);place(Arrows,36,250,228,32);place(OneWay,36,290,228,32);
        place(MinZ,36,378,106,28);place(MaxZ,158,378,106,28);place(Apply,36,416,106,30);place(All,158,416,106,30);place(Isolate,36,456,228,32);place(Range,36,502,228,32);
        MoveWindow(controls.at(Info),px(36),px(594),px(228),std::max(px(65),int(r.bottom)-px(650)),TRUE);InvalidateRect(window,nullptr,FALSE);
    }
    XMVECTOR forward() const {return XMVectorSet(std::cos(pitch)*std::cos(yaw),std::cos(pitch)*std::sin(yaw),std::sin(pitch),0);}
    XMMATRIX matrix() const {return XMMatrixLookToLH(XMLoadFloat3(&eye),forward(),XMVectorSet(0,0,1,0))*XMMatrixPerspectiveFovLH(XM_PI/3.f,float(renderer.width)/float(renderer.height),200000,4);}
    void orbitEye(){XMStoreFloat3(&eye,XMLoadFloat3(&pivot)-forward()*orbitDistance);}
    void home(bool focus) {
        if(!scene)return;pivot=scene->anchor;
        if(focus && selected<scene->document->cells.size()){const auto& c=scene->document->cells[selected];pivot={scene->document->originX+c.key.x*16.f+8,scene->document->originY+c.key.y*16.f+8,float(c.height)};}
        if(!focus){yaw=.8f;pitch=-.65f;orbitDistance=1200;}else orbitDistance=220;orbitEye();
    }
    std::wstring regionName(const Scene& item) const {
        for(const auto& region:regions)if(region.source.path==item.document->source)return region.source.name;return item.document->source.stem().wstring();
    }
    void updateInfo() {
        if(!scene || selected>=scene->document->cells.size()){SetWindowTextW(controls.at(Info),L"Click a cell to inspect it.\r\n\r\nGreen   Allowed movement\r\nRed       Blocked edge\r\nAmber   One-way candidate\r\n\r\nNearby selected regions stream automatically as you move.\r\n\r\nRead-only viewport.");return;}
        const auto& doc=*scene->document;const auto& c=doc.cells[selected];std::wostringstream text;
        text<<regionName(*scene)<<L"\r\nCell "<<c.key.x<<L", "<<c.key.y<<L" / layer "<<unsigned(c.key.layer)<<L"\r\n\r\nX  "<<doc.originX+c.key.x*16+8<<L"\r\nY  "<<doc.originY+c.key.y*16+8<<L"\r\nZ  "<<c.height<<L"\r\nNSWE  "<<unsigned(c.nswe)<<L"\r\n\r\n";
        const wchar_t* labels[]{L"East (+X)",L"West (-X)",L"South (+Y)",L"North (-Y)"};const unsigned flags[]{1,2,4,8};
        for(unsigned d=0;d<4;++d){text<<labels[d]<<L": "<<((c.nswe&flags[d]) ? L"open":L"blocked")<<L"\r\n";}
        text<<L"\r\nOne-way diagnostics use nearby layers within 128 units; region seams are not classified.";SetWindowTextW(controls.at(Info),text.str().c_str());
    }
    void setHeights(float lo,float hi){minZ=lo;maxZ=hi;SetWindowTextW(controls.at(MinZ),std::to_wstring(int(lo)).c_str());SetWindowTextW(controls.at(MaxZ),std::to_wstring(int(hi)).c_str());}
    void setRegions(const std::vector<GeodataRegion>& sources) {
        if(sources.empty())throw std::runtime_error("No geodata selected");
        std::vector<Region> next;std::set<std::pair<int,int>> occupied;
        for(const auto& source:sources){Region region;region.source=source;region.source.path=std::filesystem::absolute(source.path);std::wsmatch match;const auto stem=source.name;
            if(std::regex_match(stem,match,std::wregex(LR"(^([0-9]{1,2})_([0-9]{1,2})(?:_Classic)?$)",std::regex::icase))){region.x=(std::stoi(match[1])-20)*32768;region.y=(std::stoi(match[2])-18)*32768;}
            else if(sources.size()>1)throw std::runtime_error("Multiple geodata files require numeric region names");
            if(!occupied.emplace(region.x,region.y).second)throw std::runtime_error("Two geodata variants occupy the same region");next.push_back(std::move(region));
        }
        loader.request_stop();if(loader.joinable())loader.join();busy=false;loading.reset();scenes.clear();scene.reset();regions=std::move(next);requested.clear();selected=UINT32_MAX;failed=false;setHeights(-32768,32767);
        SetWindowTextW(controls.at(Open),(L"Selected maps: "+std::to_wstring(regions.size())+L"  /  Jump to map").c_str());updateInfo();jump(0);
    }
    void jump(size_t index) {
        if(index>=regions.size())return;const auto& region=regions[index];frameTarget=region.source.path;eye={float(region.x+16384),float(region.y+16384),1000};requested.clear();requestStream();SetFocus(viewport);
    }
    void choose() {
        if(regions.empty())return;const auto menu=CreatePopupMenu();if(!menu)return;
        for(size_t i=0;i<regions.size();++i)AppendMenuW(menu,MF_STRING,UINT_PTR(i+1),regions[i].source.name.c_str());RECT r{};GetWindowRect(controls.at(Open),&r);
        const auto chosen=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_LEFTALIGN,r.left,r.bottom,window,nullptr);DestroyMenu(menu);if(chosen)jump(chosen-1);
    }
    void requestStream() {
        if(busy || regions.empty())return;
        std::vector<std::pair<float,size_t>> order;
        for(size_t i=0;i<regions.size();++i){const auto& r=regions[i];const float dx=std::max({float(r.x)-eye.x,0.f,eye.x-r.x-32768}),dy=std::max({float(r.y)-eye.y,0.f,eye.y-r.y-32768});order.push_back({dx*dx+dy*dy,i});}
        std::sort(order.begin(),order.end());std::vector<size_t> needed;for(const auto& entry:order)if(needed.size()<4 && (entry.first<=range*range || needed.empty()))needed.push_back(entry.second);
        std::sort(needed.begin(),needed.end());std::vector<std::filesystem::path> paths;for(const auto index:needed)paths.push_back(regions[index].source.path);if(paths==requested)return;requested=paths;
        if(loader.joinable())loader.join();loading=std::make_shared<Loading>();const auto result=loading;const auto target=window;const auto previous=scenes;const auto metadata=regions;const bool wantGeometry=std::any_of(geometryVisible.begin(),geometryVisible.end(),[](bool v){return v;});busy=true;
        state=L"Loading nearby geodata  /  "+std::to_wstring(regions.size())+L" selected maps";InvalidateRect(window,nullptr,FALSE);
        loader=std::jthread([result,target,paths,previous,metadata,wantGeometry](std::stop_token stop){try{Scenes next;std::vector<std::pair<std::shared_ptr<Scene>,std::shared_ptr<MapGeometry>>> geometry;
            for(const auto& path:paths){if(stop.stop_requested())return;const auto old=std::find_if(previous.begin(),previous.end(),[&](const auto& item){return item->document->source==path;});
                if(old!=previous.end())next.push_back(*old);else{auto doc=std::make_shared<Document>(Document::read(path,stop));for(const auto& region:metadata)if(region.source.path==path){doc->originX=region.x;doc->originY=region.y;break;}next.push_back(Renderer::prepare(std::move(doc),stop));}
                if(wantGeometry && !next.back()->geometry)for(const auto& region:metadata)if(region.source.path==path && !region.source.map.empty()){geometry.emplace_back(next.back(),prepareGeometry(region.source.map,*next.back()->document,stop));break;}
            }
            if(stop.stop_requested())return;std::lock_guard lock(result->mutex);result->result=std::move(next);result->geometry=std::move(geometry);result->ready=true;
        }catch(const std::exception& e){if(stop.stop_requested())return;std::lock_guard lock(result->mutex);result->error=e.what();result->ready=true;}if(!stop.stop_requested())PostMessageW(target,loadedMessage,0,0);});
    }
    void complete() {
        if(!loading)return;Scenes next;std::vector<std::pair<std::shared_ptr<Scene>,std::shared_ptr<MapGeometry>>> geometry;std::string error;{std::lock_guard lock(loading->mutex);if(!loading->ready)return;next=std::move(loading->result);geometry=std::move(loading->geometry);error=loading->error;}loading.reset();busy=false;
        if(!error.empty()){state=L"Load failed: "+wide(error);MessageBoxW(window,state.c_str(),L"View Geodata",MB_ICONERROR);InvalidateRect(window,nullptr,FALSE);return;}
        for(auto& item:next)if(!item->buffer)renderer.upload(*item);std::string notice;for(auto& entry:geometry){renderer.uploadMesh(entry.second->vertices,entry.second->buffer);if(!entry.second->notice.empty())notice+=entry.second->notice+"\n";entry.first->geometry=std::move(entry.second);}scenes=std::move(next);if(!notice.empty())MessageBoxW(window,wide(notice).c_str(),L"Map geometry",MB_OK|MB_ICONWARNING);
        if(!frameTarget.empty()){for(const auto& item:scenes)if(item->document->source==frameTarget){scene=item;selected=UINT32_MAX;home(false);frameTarget.clear();break;}}
        if(scene && std::find(scenes.begin(),scenes.end(),scene)==scenes.end()){scene.reset();selected=UINT32_MAX;}if(!scene && !scenes.empty())scene=scenes.front();
        reveal=std::chrono::steady_clock::now();updateInfo();size_t cells=0;for(const auto& item:scenes)cells+=item->instances.size();
        state=std::to_wstring(regions.size())+L" selected maps  /  "+std::to_wstring(scenes.size())+L" nearby  /  "+std::to_wstring(cells)+L" layers";InvalidateRect(window,nullptr,FALSE);
    }
    void command(int id) {
        if(id==Close){PostMessageW(window,WM_CLOSE,0,0);return;}if(id==Minimize){ShowWindow(window,SW_MINIMIZE);return;}if(id==Maximize){ShowWindow(window,IsZoomed(window) ? SW_RESTORE:SW_MAXIMIZE);SetFocus(viewport);return;}
        if(id==ShowCells)cellsVisible=!cellsVisible;
        else if(id>=ShowTerrain && id<=ShowBsp){if(std::none_of(regions.begin(),regions.end(),[](const Region& r){return !r.source.map.empty();})){MessageBoxW(window,L"Open selected maps from the main window to load source geometry.",L"Map geometry",MB_OK|MB_ICONINFORMATION);return;}const unsigned kind=id==ShowTerrain ? 1:id==ShowMeshes ? 0:id==ShowVolumes ? 2:3;geometryVisible[kind]=!geometryVisible[kind];requested.clear();}
        if(id==Open)choose();else if(id==Home)home(false);else if(id==Focus)home(true);
        else if(id==Mode){orbit=!orbit;SetWindowTextW(controls.at(Mode),orbit ? L"Orbit camera":L"Fly camera");pivot={eye.x+XMVectorGetX(forward())*orbitDistance,eye.y+XMVectorGetY(forward())*orbitDistance,eye.z+XMVectorGetZ(forward())*orbitDistance};}
        else if(id==Grid)grid=!grid;else if(id==Arrows)arrows=!arrows;else if(id==OneWay)asymmetric=!asymmetric;
        else if(id==Antialiasing){renderer.cycleSamples();SetWindowTextW(controls.at(Antialiasing),(renderer.sampleCount()==1 ? L"AA: Off":(L"AA: "+std::to_wstring(renderer.sampleCount())+L"x").c_str()));}
        else if(id==Range){range=range<16384.f ? 16384.f:range<32768.f ? 32768.f:range<65536.f ? 65536.f:range<131072.f ? 131072.f:8192.f;SetWindowTextW(controls.at(Range),(L"View distance: "+std::to_wstring(int(range))).c_str());}
        else if(id==All)setHeights(-32768,32767);
        else if(id==Isolate && scene && selected<scene->document->cells.size()){const float z=scene->document->cells[selected].height;setHeights(z-4,z+4);home(true);}
        else if(id==Apply){wchar_t a[64]{},b[64]{};GetWindowTextW(controls.at(MinZ),a,64);GetWindowTextW(controls.at(MaxZ),b,64);wchar_t *ea=nullptr,*eb=nullptr;const auto lo=wcstol(a,&ea,10),hi=wcstol(b,&eb,10);if(ea==a || eb==b || *ea || *eb || lo>hi || lo< -32768 || hi>32767)MessageBoxW(window,L"Enter whole heights from -32768 to 32767, with minimum <= maximum.",L"Height range",MB_ICONINFORMATION);else setHeights(float(lo),float(hi));}
        for(const auto& control:controls)InvalidateRect(control.second,nullptr,FALSE);if(id!=MinZ && id!=MaxZ && id!=Info)SetFocus(viewport);
    }
    void release(){if(!drag)return;drag=0;ClipCursor(nullptr);ReleaseCapture();ShowCursor(TRUE);SetCursorPos(restore.x,restore.y);}
    void capture(int mode){if(drag)release();SetFocus(viewport);drag=mode;GetCursorPos(&restore);SetCapture(viewport);RECT r{};GetClientRect(viewport,&r);MapWindowPoints(viewport,nullptr,reinterpret_cast<POINT*>(&r),2);ClipCursor(&r);ShowCursor(FALSE);}
    void mouse(float dx,float dy) {
        if(drag==2){const auto right=XMVector3Normalize(XMVector3Cross(XMVectorSet(0,0,1,0),forward()));const auto up=XMVector3Normalize(XMVector3Cross(forward(),right));const auto delta=right*(-dx*orbitDistance*.0015f)+up*(dy*orbitDistance*.0015f);XMStoreFloat3(&eye,XMLoadFloat3(&eye)+delta);XMStoreFloat3(&pivot,XMLoadFloat3(&pivot)+delta);}
        else {yaw+=dx*.003f;pitch=std::clamp(pitch-dy*.003f,-1.553f,1.553f);if(orbit || drag==3)orbitEye();}
    }
    bool overlayFor(const Scene& item) const {if(!item.geometry)return false;for(unsigned kind=0;kind<4;++kind)if(geometryVisible[kind] && (item.geometry->kindMask&(1u<<kind)))return true;return false;}
    void tick() {
        const auto now=std::chrono::steady_clock::now();const float dt=std::clamp(std::chrono::duration<float>(now-last).count(),0.f,.05f);last=now;if(IsIconic(window))return;
        if(GetFocus()==viewport && !orbit){
            auto key=[](int k){return (GetAsyncKeyState(k)&0x8000) ? 1.f:0.f;};const float step=speed*dt*(key(VK_SHIFT) ? 4.f:1.f)*(key(VK_MENU) ? .2f:1.f);const auto ahead=forward(),right=XMVectorSet(-std::sin(yaw),std::cos(yaw),0,0);
            const auto movement=ahead*(key('W')-key('S'))+right*(key('D')-key('A'))+XMVectorSet(0,0,key(VK_SPACE)-key(VK_CONTROL),0);
            if(XMVectorGetX(XMVector3LengthSq(movement))>0)XMStoreFloat3(&eye,XMLoadFloat3(&eye)+XMVector3Normalize(movement)*step);
        }
        requestStream();Constants cb{};XMStoreFloat4x4(&cb.matrix,XMMatrixTranspose(matrix()));cb.camera={eye.x,eye.y,eye.z,1};cb.limits={minZ,maxZ,range,animations ? std::clamp(std::chrono::duration<float>(now-reveal).count()/.32f,0.f,1.f):1.f};cb.options={UINT32_MAX,grid ? 1u:0u,arrows ? 1u:0u,asymmetric ? 1u:0u};
        renderer.draw(nullptr,cb,false);for(const auto& item:scenes)renderer.drawGeometry(*item,cb,geometryVisible);if(cellsVisible)for(const auto& item:scenes){cb.options.x=item==scene ? selected:UINT32_MAX;cb.options.y=(grid ? 1u:0u)|(overlayFor(*item) ? 2u:0u)|((geometryVisible[1] && item->geometry && (item->geometry->kindMask&2)) ? 4u:0u);renderer.draw(item.get(),cb,false,false);}renderer.draw(nullptr,cb,true,false);++frameCount;const float elapsed=std::chrono::duration<float>(now-fpsStart).count();if(elapsed>=1){fps=unsigned(std::lround(frameCount/elapsed));frameCount=0;fpsStart=now;RECT bottom{};GetClientRect(window,&bottom);bottom.top=bottom.bottom-px(38);InvalidateRect(window,&bottom,FALSE);}
    }
    void pick(int x,int y) {
        if(!cellsVisible || scenes.empty() || (animations && std::chrono::duration<float>(std::chrono::steady_clock::now()-reveal).count()<.32f))return;const auto inv=XMMatrixInverse(nullptr,matrix());const auto nearPoint=XMVector3TransformCoord(XMVectorSet(2.f*x/renderer.width-1,1-2.f*y/renderer.height,1,1),inv);const auto farPoint=XMVector3TransformCoord(XMVectorSet(2.f*x/renderer.width-1,1-2.f*y/renderer.height,0,1),inv);XMFLOAT3 direction;XMStoreFloat3(&direction,XMVector3Normalize(farPoint-nearPoint));if(std::abs(direction.z)<1e-8f)return;
        float best=std::numeric_limits<float>::max();uint32_t hit=UINT32_MAX;std::shared_ptr<Scene> picked;
        for(const auto& item:scenes)for(unsigned group=0;group<1024;++group)if(Renderer::visible(item->chunks[group],group,eye,range,minZ,maxZ,item->document->originX,item->document->originY)){
            const auto& chunk=item->chunks[group];for(uint32_t i=chunk.first;i<chunk.first+chunk.count;++i){const auto& c=item->instances[i];if(c.z<minZ || c.z>maxZ || std::hypot(c.x+8-eye.x,c.y+8-eye.y)>range)continue;const float t=(c.z+(overlayFor(*item) ? 8.f:0.f)-eye.z)/direction.z;if(t<=0 || t>=best)continue;const float px=eye.x+t*direction.x,py=eye.y+t*direction.y;if(px>=c.x && px<=c.x+16 && py>=c.y && py<=c.y+16){best=t;hit=c.id;picked=item;}}}
        selected=hit;if(picked)scene=std::move(picked);updateInfo();
    }
    void text(HDC dc,const std::wstring& value,RECT r,COLORREF color,HFONT face,UINT flags=DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS) {
        const auto old=SelectObject(dc,face);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);DrawTextW(dc,value.c_str(),-1,&r,flags);SelectObject(dc,old);
    }
    void drawButton(const DRAWITEMSTRUCT& item) {
        auto r=item.rcItem;const auto dc=item.hDC;const auto id=item.CtlID;POINT point{};GetCursorPos(&point);ScreenToClient(item.hwndItem,&point);const bool hover=PtInRect(&r,point)!=FALSE,down=(item.itemState&ODS_SELECTED)!=0;
        const auto bg=CreateSolidBrush(id==Minimize || id==Maximize || id==Close ? theme::header:panel);FillRect(dc,&r,bg);DeleteObject(bg);
        if(id==Minimize || id==Maximize || id==Close){if(hover)roundRect(dc,r,float(px(8)),id==Close ? RGB(140,38,38):theme::raised);const int cx=(r.left+r.right)/2,cy=(r.top+r.bottom)/2;if(id==Maximize){auto box=[&](int x,int y){RECT bounds{cx-px(6)+x,cy-px(6)+y,cx+px(6)+x,cy+px(6)+y};roundRect(dc,bounds,1,theme::header,theme::accentHot);};if(IsZoomed(window)){box(px(2),-px(2));box(-px(2),px(2));}else box(0,0);}else icon(dc,id==Close ? Icon::close:Icon::minimize,{cx-px(8),cy-px(8),cx+px(8),cy+px(8)},theme::accentHot);return;}
        roundRect(dc,r,float(px(8)),down ? theme::pressed:hover ? theme::hover:theme::raised,hover ? theme::muted:theme::border);
        wchar_t label[160]{};GetWindowTextW(item.hwndItem,label,160);
        const bool toggle=id==Grid || id==Arrows || id==OneWay || (id>=ShowCells && id<=ShowBsp);
        if(toggle){const bool enabled=id==Grid ? grid:id==Arrows ? arrows:id==OneWay ? asymmetric:id==ShowCells ? cellsVisible:geometryVisible[id==ShowTerrain ? 1:id==ShowMeshes ? 0:id==ShowVolumes ? 2:3];const COLORREF tint=id==ShowTerrain ? RGB(99,197,120):id==ShowMeshes ? RGB(88,166,255):id==ShowVolumes ? RGB(205,128,245):id==ShowBsp ? RGB(245,173,72):id==ShowCells ? RGB(75,210,203):accent;const int cy=(r.top+r.bottom)/2;RECT box{px(12),cy-px(7),px(26),cy+px(7)};roundRect(dc,box,float(px(4)),enabled ? tint:theme::inset,enabled ? tint:theme::border);if(enabled)icon(dc,Icon::check,box,theme::base,2);r.left=px(36);text(dc,label,r,id>=ShowCells && id<=ShowBsp ? (enabled ? tint:muted):ink,font);}
        else text(dc,label,r,ink,font,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
        if((item.itemState&ODS_FOCUS) && !(item.itemState&ODS_NOFOCUSRECT)){InflateRect(&r,-px(3),-px(3));roundRect(dc,r,float(px(6)),theme::none,accent);}
    }
    void paintFooter(HDC dc,const RECT& r){
        RECT footer{0,r.bottom-px(38),r.right,r.bottom};FillRect(dc,&footer,background);
        text(dc,state+L"  /  "+std::to_wstring(fps)+L" FPS",{px(20),r.bottom-px(31),r.right/2,r.bottom-px(6)},muted,font);
        text(dc,L"RMB look   MMB pan   WASD fly   Space up / Ctrl down   Shift fast   F focus",{r.right/2,r.bottom-px(31),r.right-px(20),r.bottom-px(6)},muted,font,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);
    }
    void paint(HDC dc) {
        RECT r{},clip{};GetClientRect(window,&r);if(GetClipBox(dc,&clip)!=ERROR && clip.top>=r.bottom-px(38)){paintFooter(dc,r);return;}FillRect(dc,&r,background);RECT header{0,0,r.right,px(68)};const auto brush=CreateSolidBrush(theme::header);FillRect(dc,&header,brush);DeleteObject(brush);
        icon(dc,Icon::layers,{px(20),px(20),px(50),px(50)},accent,1.8f);text(dc,L"911reg MapExtractor",{px(66),px(10),px(340),px(37)},ink,titleFont);text(dc,L"View Geodata  /  Selected map viewport",{px(67),px(38),px(510),px(59)},muted,font);
        roundRect(dc,{px(16),px(82),px(284),r.bottom-px(40)},float(px(12)),panel,theme::border);
        roundRect(dc,{px(300),px(82),r.right-px(16),r.bottom-px(40)},float(px(12)),panel,theme::border);
        auto label=[&](const wchar_t* value,int y,Icon glyph){icon(dc,glyph,{px(36),px(y+4),px(52),px(y+20)},accent);text(dc,value,{px(62),px(y),px(265),px(y+26)},ink,headingFont);};
        label(L"NAVIGATION",92,Icon::map);label(L"CELL DISPLAY",176,Icon::layers);label(L"HEIGHT FILTER",340,Icon::sliders);label(L"CELL INSPECTOR",554,Icon::search);
        text(dc,L"Min Z",{px(36),px(360),px(142),px(378)},muted,font);text(dc,L"Max Z",{px(158),px(360),px(264),px(378)},muted,font);
        paintFooter(dc,r);
    }
};
LRESULT CALLBACK viewportProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* self=reinterpret_cast<Viewer*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){self=static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(window,message,wp,lp);
    try {
        switch(message) {
        case WM_SIZE:if(self->viewport)self->renderer.resize(LOWORD(lp),HIWORD(lp));return 0;
        case WM_ERASEBKGND:return 1;
        case WM_GETDLGCODE:if(lp && reinterpret_cast<MSG*>(lp)->wParam==VK_TAB)return 0;return DLGC_WANTARROWS|DLGC_WANTCHARS|DLGC_WANTALLKEYS;
        case WM_LBUTTONDOWN:if(GetKeyState(VK_MENU)&0x8000)self->capture(3);else{SetFocus(window);self->pick(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));}return 0;
        case WM_RBUTTONDOWN:self->capture(1);return 0;
        case WM_MBUTTONDOWN:self->capture(2);return 0;
        case WM_LBUTTONUP:case WM_RBUTTONUP:case WM_MBUTTONUP:self->release();return 0;
        case WM_KILLFOCUS:case WM_CANCELMODE:case WM_CAPTURECHANGED:self->release();return 0;
        case WM_INPUT:if(self->drag){RAWINPUT input{};UINT size=sizeof(input);if(GetRawInputData(reinterpret_cast<HRAWINPUT>(lp),RID_INPUT,&input,&size,sizeof(RAWINPUTHEADER))!=UINT(-1) && input.header.dwType==RIM_TYPEMOUSE)self->mouse(float(input.data.mouse.lLastX),float(input.data.mouse.lLastY));}break;
        case WM_MOUSEWHEEL:if(self->orbit){self->orbitDistance=std::clamp(self->orbitDistance*std::pow(.85f,float(GET_WHEEL_DELTA_WPARAM(wp))/120),16.f,60000.f);self->orbitEye();}else self->speed=std::clamp(self->speed*std::pow(1.25f,float(GET_WHEEL_DELTA_WPARAM(wp))/120),20.f,20000.f);return 0;
        case WM_KEYDOWN:if(wp==VK_ESCAPE)self->release();else if(wp=='F')self->home(true);else if(wp==VK_HOME)self->home(false);return 0;
        }
    }catch(const std::exception& e){self->state=wide(e.what());InvalidateRect(self->window,nullptr,FALSE);}
    return DefWindowProcW(window,message,wp,lp);
}
void Viewer::create() {
    Gdiplus::GdiplusStartupInput graphics;checked(HRESULT(Gdiplus::GdiplusStartup(&graphicsToken,&graphics,nullptr)==Gdiplus::Ok ? S_OK:E_FAIL),"Cannot initialize viewer theme");
    BOOL dark=TRUE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));BOOL animation=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animation,0);animations=animation!=FALSE;fonts();
    viewport=CreateWindowExW(0,L"911regGeoViewport",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP,0,0,1,1,window,nullptr,GetModuleHandleW(nullptr),this);if(!viewport)throw std::runtime_error("Cannot create geodata viewport");renderer.initialize(viewport);
    for(const auto& entry:std::array<std::pair<int,const wchar_t*>,20>{{{Open,L"Selected maps"},{Home,L"Frame"},{Focus,L"Focus cell"},{Mode,L"Fly camera"},{Grid,L"Cell grid"},{Arrows,L"Movement arrows"},{OneWay,L"One-way diagnostics"},{Apply,L"Apply"},{All,L"All heights"},{Isolate,L"Isolate selected floor"},{Range,L"View distance: 65536"},{Minimize,L"Minimize"},{Maximize,L"Maximize / Restore"},{Close,L"Close"},{ShowCells,L"Geodata"},{ShowTerrain,L"Terrain"},{ShowMeshes,L"Static meshes"},{ShowVolumes,L"Blocking volumes"},{ShowBsp,L"BSP"},{Antialiasing,L"AA: 2x"}}})control(entry.first,L"BUTTON",entry.second,BS_OWNERDRAW);
    control(MinZ,L"EDIT",L"-32768",ES_AUTOHSCROLL|WS_BORDER);control(MaxZ,L"EDIT",L"32767",ES_AUTOHSCROLL|WS_BORDER);control(Info,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL);updateInfo();
    RAWINPUTDEVICE mouse{1,2,0,viewport};if(!RegisterRawInputDevices(&mouse,1,sizeof(mouse)))throw std::runtime_error("Cannot initialize viewport mouse input");SetWindowTextW(controls.at(Antialiasing),(renderer.sampleCount()==1 ? L"AA: Off":(L"AA: "+std::to_wstring(renderer.sampleCount())+L"x").c_str()));layout();SetTimer(window,1,10,nullptr);
}
LRESULT CALLBACK viewerProc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* self=reinterpret_cast<Viewer*>(GetWindowLongPtrW(window,GWLP_USERDATA));if(message==WM_NCCREATE){self=static_cast<Viewer*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}if(!self)return DefWindowProcW(window,message,wp,lp);
    try {
        switch(message) {
        case WM_CREATE:self->create();return 0;
        case WM_SIZE:if(self->viewport){self->layout();if(self->controls.contains(Maximize)){SetWindowTextW(self->controls.at(Maximize),IsZoomed(window) ? L"Restore":L"Maximize");InvalidateRect(self->controls.at(Maximize),nullptr,FALSE);}}return 0;
        case WM_NCCALCSIZE:if(wp)return 0;break;
        case WM_NCHITTEST:{POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(window,&point);RECT r{};GetClientRect(window,&r);const int edge=self->px(6);const bool l=point.x<edge,t=point.y<edge,b=point.y>=r.bottom-edge,right=point.x>=r.right-edge;if(!IsZoomed(window)){if(t && l)return HTTOPLEFT;if(t && right)return HTTOPRIGHT;if(b && l)return HTBOTTOMLEFT;if(b && right)return HTBOTTOMRIGHT;if(l)return HTLEFT;if(right)return HTRIGHT;if(t)return HTTOP;if(b)return HTBOTTOM;}return point.y<self->px(68) ? HTCAPTION:HTCLIENT;}
        case WM_GETMINMAXINFO:{auto* info=reinterpret_cast<MINMAXINFO*>(lp);info->ptMinTrackSize={self->px(1100),self->px(780)};MONITORINFO monitor{sizeof(monitor)};if(GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)){info->ptMaxPosition={monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top};info->ptMaxSize={monitor.rcWork.right-monitor.rcWork.left,monitor.rcWork.bottom-monitor.rcWork.top};}return 0;}
        case WM_DPICHANGED:{const auto* r=reinterpret_cast<RECT*>(lp);SetWindowPos(window,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);self->fonts();self->layout();return 0;}
        case WM_PRINTCLIENT:case WM_PAINT:{PAINTSTRUCT paint{};const auto dc=message==WM_PAINT ? BeginPaint(window,&paint):reinterpret_cast<HDC>(wp);self->paint(dc);if(message==WM_PAINT)EndPaint(window,&paint);return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_COMMAND:self->command(LOWORD(wp));return 0;

        case WM_SETTINGCHANGE:{BOOL animation=TRUE;SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animation,0);self->animations=animation!=FALSE;return 0;}
        case WM_TIMER:if(!self->failed)self->tick();return 0;
        case loadedMessage:self->complete();return 0;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORBTN:SetTextColor(reinterpret_cast<HDC>(wp),ink);SetBkColor(reinterpret_cast<HDC>(wp),theme::inset);return reinterpret_cast<LRESULT>(self->field);
        case WM_DRAWITEM:self->drawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;
        case WM_CLOSE:self->release();if(self->animations && !IsIconic(window))AnimateWindow(window,120,AW_BLEND|AW_HIDE);DestroyWindow(window);return 0;
        case WM_DESTROY:{RAWINPUTDEVICE mouse{1,2,RIDEV_REMOVE,nullptr};RegisterRawInputDevices(&mouse,1,sizeof(mouse));}KillTimer(window,1);self->release();self->loader.request_stop();if(self->loader.joinable())self->loader.join();if(!self->owner)PostQuitMessage(0);return 0;
        case WM_NCDESTROY:if(labWindow==window)labWindow=nullptr;SetWindowLongPtrW(window,GWLP_USERDATA,0);delete self;return DefWindowProcW(window,message,wp,lp);
        }
    }catch(const std::exception& e){self->release();self->state=L"Viewer error: "+wide(e.what());if(message==WM_CREATE){MessageBoxW(window,self->state.c_str(),L"Geodata viewer",MB_ICONERROR);return -1;}if(message==WM_TIMER)self->failed=true;MessageBoxW(window,self->state.c_str(),L"Geodata viewer",MB_ICONERROR);InvalidateRect(window,nullptr,FALSE);}
    return DefWindowProcW(window,message,wp,lp);
}
}
void openGeodataRegions(HWND owner,const std::vector<GeodataRegion>& regions) {
    if(IsWindow(labWindow)){auto* viewer=reinterpret_cast<Viewer*>(GetWindowLongPtrW(labWindow,GWLP_USERDATA));if(!regions.empty())viewer->setRegions(regions);ShowWindow(labWindow,SW_RESTORE);SetForegroundWindow(labWindow);return;}
    const auto instance=GetModuleHandleW(nullptr);WNDCLASSW canvas{};canvas.lpfnWndProc=viewportProc;canvas.hInstance=instance;canvas.hCursor=LoadCursorW(nullptr,IDC_CROSS);canvas.lpszClassName=L"911regGeoViewport";RegisterClassW(&canvas);
    WNDCLASSW window{};window.lpfnWndProc=viewerProc;window.hInstance=instance;window.hCursor=LoadCursorW(nullptr,IDC_ARROW);window.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));window.lpszClassName=L"911regGeodataLab";RegisterClassW(&window);
    auto* viewer=new Viewer;viewer->owner=owner;const auto handle=CreateWindowExW(WS_EX_APPWINDOW,L"911regGeodataLab",L"911reg MapExtractor / View Geodata",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,1280,860,owner,nullptr,instance,viewer);
    if(!handle)throw std::runtime_error("Cannot open View Geodata window");labWindow=handle;
    try{if(!regions.empty())viewer->setRegions(regions);}catch(...){viewer->owner=owner ? owner:handle;DestroyWindow(handle);throw;}
    if(!viewer->animations || !AnimateWindow(handle,180,AW_BLEND|AW_ACTIVATE))ShowWindow(handle,SW_SHOW);RedrawWindow(handle,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_UPDATENOW);SetFocus(viewer->viewport);
}
void openGeodataViewer(HWND owner,const std::filesystem::path& path) {
    std::vector<GeodataRegion> regions;if(!path.empty())regions.push_back({path,path.stem().wstring()});openGeodataRegions(owner,regions);
}
void closeGeodataViewer(){if(IsWindow(labWindow))DestroyWindow(labWindow);}
bool handleGeodataViewerMessage(MSG& message) {
    if(!IsWindow(labWindow) || (message.hwnd!=labWindow && !IsChild(labWindow,message.hwnd)))return false;
    if(message.message==WM_KEYDOWN && message.wParam=='O' && (GetKeyState(VK_CONTROL)&0x8000)){reinterpret_cast<Viewer*>(GetWindowLongPtrW(labWindow,GWLP_USERDATA))->choose();return true;}
    return IsDialogMessageW(labWindow,&message)!=FALSE;
}
int runGeodataViewer(const std::filesystem::path& path) {
    openGeodataViewer(nullptr,path);MSG message{};while(GetMessageW(&message,nullptr,0,0)>0)if(!handleGeodataViewerMessage(message)){TranslateMessage(&message);DispatchMessageW(&message);}return int(message.wParam);
}
int verifyGeodataViewport(const std::filesystem::path& path) {
    const auto window=CreateWindowExW(0,L"STATIC",L"MapExtractor viewport verification",WS_OVERLAPPEDWINDOW,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    if(!window)throw std::runtime_error("Cannot create verification window");
    try {
        Renderer renderer;renderer.initialize(window);auto scene=Renderer::prepare(std::make_shared<Document>(Document::read(path)),{});renderer.upload(*scene);
        const auto anchor=XMLoadFloat3(&scene->anchor),eye=anchor+XMVectorSet(-450,-450,650,0);XMFLOAT3 position;XMStoreFloat3(&position,eye);
        Constants cb{};cb.camera={position.x,position.y,position.z,1};cb.limits={-32768,32767,4096,1};cb.options={UINT32_MAX,1,1,1};
        size_t pixels=0;for(const auto size:std::array<std::pair<int,int>,2>{{{640,480},{800,600}}}){renderer.resize(size.first,size.second);XMStoreFloat4x4(&cb.matrix,XMMatrixTranspose(XMMatrixLookAtLH(eye,anchor,XMVectorSet(0,0,1,0))*XMMatrixPerspectiveFovLH(XM_PI/3.f,float(size.first)/size.second,200000,4)));renderer.draw(scene.get(),cb,false);pixels=renderer.renderedPixels();if(pixels<100)throw std::runtime_error("Viewport rendered no visible geodata cells");}
        cb.limits={32767,32767,4096,1};renderer.draw(scene.get(),cb,false);if(renderer.renderedPixels()!=0)throw std::runtime_error("Viewport height filter failed");
        std::cout<<"Geodata viewport passed: shader compilation, GPU upload, instanced drawing, resize, pixel readback and height filtering; "<<scene->instances.size()<<" cells, "<<pixels<<" visible pixels\n";
    }catch(...){DestroyWindow(window);throw;}DestroyWindow(window);return 0;
}
}
