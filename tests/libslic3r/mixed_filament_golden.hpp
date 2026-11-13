// Golden mixed_filament_definitions strings, produced by Snapmaker's own
// MixedFilamentManager::serialize_custom_entries (built from the 3-snapmaker-main worktree, linked against its
// libslic3r, 2026-11-12). Tests using them prove agreement with that serializer, not that a real Snapmaker
// project prints the same here.
#pragma once
#include <string>

namespace Slic3r::mixed_golden {

inline const std::string k_auto3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3";
inline const std::string k_disabled_auto4 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,0,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "1,4,1,0,30,0,g,w,m2,z0,xa0,xb0,d0,o1,u3;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u4;"
    "2,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u5;"
    "3,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u6";
inline const std::string k_deleted3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d1,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3";
inline const std::string k_custom_pattern3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3;"
    "1,3,1,1,50,0,g,w,m2,z0,xa0,xb0,d0,o0,u4,1212,2";
inline const std::string k_gradient3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1,cm3,r1/0.8000/0.2000;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3";
inline const std::string k_gradient3col3 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3;"
    "1,2,1,1,25,0,g123,w50/30/20,m0,z0,xa0,xb0,d0,o0,u4";
inline const std::string k_offsets3 =
    "1,2,1,0,50,0,g,w,m2,z4,xa0.1,xb-0.2,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3";
inline const std::string k_ext12 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1;"
    "1,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u2;"
    "1,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u3;"
    "1,5,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u4;"
    "1,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u5;"
    "1,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u6;"
    "1,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u7;"
    "1,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u8;"
    "1,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u9;"
    "1,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u10;"
    "1,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u11;"
    "2,3,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u12;"
    "2,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u13;"
    "2,5,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u14;"
    "2,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u15;"
    "2,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u16;"
    "2,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u17;"
    "2,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u18;"
    "2,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u19;"
    "2,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u20;"
    "2,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u21;"
    "3,4,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u22;"
    "3,5,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u23;"
    "3,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u24;"
    "3,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u25;"
    "3,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u26;"
    "3,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u27;"
    "3,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u28;"
    "3,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u29;"
    "3,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u30;"
    "4,5,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u31;"
    "4,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u32;"
    "4,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u33;"
    "4,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u34;"
    "4,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u35;"
    "4,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u36;"
    "4,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u37;"
    "4,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u38;"
    "5,6,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u39;"
    "5,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u40;"
    "5,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u41;"
    "5,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u42;"
    "5,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u43;"
    "5,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u44;"
    "5,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u45;"
    "6,7,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u46;"
    "6,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u47;"
    "6,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u48;"
    "6,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u49;"
    "6,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u50;"
    "6,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u51;"
    "7,8,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u52;"
    "7,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u53;"
    "7,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u54;"
    "7,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u55;"
    "7,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u56;"
    "8,9,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u57;"
    "8,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u58;"
    "8,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u59;"
    "8,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u60;"
    "9,10,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u61;"
    "9,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u62;"
    "9,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u63;"
    "10,11,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u64;"
    "10,12,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u65;"
    "11,12,1,0,50,0,g2/11/12,w,m2,z0,xa0,xb0,d0,o1,u66";
inline const std::string k_custom12 =
    "10,12,1,1,40,0,g/10,w,m2,z0,xa0,xb0,d0,o0,u1;"
    "2,11,1,1,60,0,g2/11/12,w,m2,z0,xa0,xb0,d0,o0,u2;"
    "3,4,0,1,20,0,g,w,m2,z0,xa0,xb0,d0,o0,u3;"
    "11,12,1,1,75,0,g,w,m2,z0,xa0,xb0,d0,o0,u4";
inline const std::string k_auto2 =
    "1,2,1,0,50,0,g,w,m2,z0,xa0,xb0,d0,o1,u1";

} // namespace Slic3r::mixed_golden
