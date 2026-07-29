#pragma once
#include "zip.hpp"
#include <string>

namespace co {

struct GCStats {
    int reachableObjects = 0;
    int packedObjects = 0;
    int removedLoose = 0;
    int removedPacks = 0;
};

// 垃圾回收。成功返回 true。
bool garbageCollect(Document& doc, GCStats& stats);

} // namespace co
