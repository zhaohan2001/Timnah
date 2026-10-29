#include "timnah/timnah.h"
#include "chash/chash.h"
#include "dash/dash.h"
#include "ehcache/ehcache.h"

DEFINE_string(index, "Timnah", "Index type: Timnah/CHash/Dash/EHCache");

IndexInterface* GetIndexPtr() {
  if (FLAGS_index == "Timnah" || FLAGS_index == "timnah") {
    return new TimnahNameSpace::Timnah{};
  } else if (FLAGS_index == "CHash" || FLAGS_index == "chash") {
    return new CHashNameSpace::CHash{};
  } else if (FLAGS_index == "Dash" || FLAGS_index == "dash" || FLAGS_index == "Dash-EH") {
    return new DashNameSpace::Dash{};
  } else if (FLAGS_index == "EHCache" || FLAGS_index == "ehcache" || FLAGS_index == "EH+cache") {
    return new EHCacheNameSpace::EHCache{};
  } else {
    LOG("Unknown hashing type: {}\n", FLAGS_index.c_str());
    AbortHere();
    return nullptr;
  }
}
