#pragma once
#include "collision.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace extractor::air {
inline double radius=40.,halfHeight=30.;
constexpr double retailHalfHeight=22.5,skin=0.01;
using Range=std::pair<double,double>;

inline double standingCenter(double surface,double floor,bool retail) {
    return retail ? surface+halfHeight-retailHalfHeight : floor+halfHeight+skin;
}

inline std::optional<Range> triangleRange(const collision::Triangle& triangle,collision::Point from,collision::Point to) {
    const double dx=to.x-from.x,dy=to.y-from.y,length=std::hypot(dx,dy);
    const double ux=length>1e-12 ? dx/length : 1.,uy=length>1e-12 ? dy/length : 0.;
    struct P { double x,y,z; };
    auto local=[&](collision::Point p){return P{(p.x-from.x)*ux+(p.y-from.y)*uy,-(p.x-from.x)*uy+(p.y-from.y)*ux,p.z};};
    const std::array<P,3> points{local(triangle.a),local(triangle.b),local(triangle.c)};
    double low=std::numeric_limits<double>::infinity(),high=-low;
    auto inside=[&](P p){const double x=p.x-std::clamp(p.x,0.,length);return x*x+p.y*p.y<=radius*radius+1e-8;};
    auto add=[&](P p){if(inside(p)){low=std::min(low,p.z);high=std::max(high,p.z);}};
    for(const auto p:points)add(p);
    for(size_t i=0;i<3;++i) {
        const auto a=points[i],b=points[(i+1)%3];
        const double ex=b.x-a.x,ey=b.y-a.y,ez=b.z-a.z;
        auto at=[&](double t){if(t>=-1e-10 && t<=1.+1e-10){t=std::clamp(t,0.,1.);add({a.x+t*ex,a.y+t*ey,a.z+t*ez});}};
        const double quadratic=ex*ex+ey*ey;
        if(quadratic>1e-20)for(const double center:std::array<double,2>{0.,length}) {
            const double linear=2.*((a.x-center)*ex+a.y*ey),constant=(a.x-center)*(a.x-center)+a.y*a.y-radius*radius;
            const double discriminant=linear*linear-4.*quadratic*constant;
            if(discriminant>=-1e-8){const double root=std::sqrt(std::max(0.,discriminant));at((-linear-root)/(2.*quadratic));at((-linear+root)/(2.*quadratic));}
        }
        if(length>1e-12) {
            double first=0,last=1;
            auto clip=[&](double start,double delta,double minimum,double maximum){
                if(std::abs(delta)<1e-15)return start>=minimum && start<=maximum;
                double a0=(minimum-start)/delta,b0=(maximum-start)/delta;if(a0>b0)std::swap(a0,b0);
                first=std::max(first,a0);last=std::min(last,b0);return first<=last;
            };
            if(clip(a.x,ex,0.,length) && clip(a.y,ey,-radius,radius)){at(first);at(last);}
        }
    }
    const auto a=points[0],b=points[1],c=points[2];
    const double denominator=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
    if(std::abs(denominator)>1e-12) {
        auto projected=[&](double x,double y){
            const double w0=((b.y-c.y)*(x-c.x)+(c.x-b.x)*(y-c.y))/denominator;
            const double w1=((c.y-a.y)*(x-c.x)+(a.x-c.x)*(y-c.y))/denominator;
            if(w0>=-1e-9 && w1>=-1e-9 && w0+w1<=1.+1e-9)add({x,y,w0*a.z+w1*b.z+(1.-w0-w1)*c.z});
        };
        const double gx=((a.z-c.z)*(b.y-c.y)-(b.z-c.z)*(a.y-c.y))/denominator;
        const double gy=((b.z-c.z)*(a.x-c.x)-(a.z-c.z)*(b.x-c.x))/denominator;
        const double magnitude=std::hypot(gx,gy);
        for(const double center:std::array<double,2>{0.,length}) {
            projected(center,radius);projected(center,-radius);
            if(magnitude>1e-15){projected(center+radius*gx/magnitude,radius*gy/magnitude);projected(center-radius*gx/magnitude,-radius*gy/magnitude);}
            else projected(center,0.);
        }
    }
    if(low>high)return std::nullopt;
    return Range{low,high};
}

inline std::vector<Range> freeRanges(double low,double high,std::vector<Range> blocked) {
    std::vector<Range> result;
    if(low>=high)return result;
    std::sort(blocked.begin(),blocked.end());
    for(const auto& span:blocked) {
        if(span.second<=low)continue;
        if(span.first>=high)break;
        if(span.first>low)result.push_back({low,std::min(high,span.first)});
        low=std::max(low,span.second);
        if(low>=high)break;
    }
    if(low<high)result.push_back({low,high});
    return result;
}
}
