/* SPDX-License-Identifier: GPL-3.0-or-later
 * Shared screen geometry saved by Move Studio; angles in radians.
 * Apply only to the corresponding player's external Move pointer.
 */
#pragma once
#include <cmath>
#include <cstdio>
namespace move_studio {
struct ScreenProfile { bool valid=false;float half_x=0,half_y=0,yaw=0,pitch=0; };
inline ScreenProfile read_screen_profile(const char *path){
    ScreenProfile p;FILE*f=std::fopen(path,"r");if(!f)return p;
    char line[160],extra;bool read=std::fgets(line,sizeof line,f)!=nullptr;
    std::fclose(f);
    if(!read||std::sscanf(line,"MVA1 %f %f %f %f %c",&p.half_x,&p.half_y,&p.yaw,&p.pitch,&extra)!=4)return {};
    constexpr float deg=57.2957795131f;
    p.valid=std::isfinite(p.half_x)&&std::isfinite(p.half_y)&&std::isfinite(p.yaw)&&std::isfinite(p.pitch)&&
        p.half_x*deg>3&&p.half_x*deg<75&&p.half_y*deg>2&&p.half_y*deg<60&&
        p.half_y/p.half_x>.25f&&p.half_y/p.half_x<1.3f&&std::fabs(p.yaw)<1.31f&&std::fabs(p.pitch)<1.05f;
    return p.valid?p:ScreenProfile{};
}
}
