#include "trace_replay.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <jsoncpp/json/json.h>

namespace real_trace {
namespace {
struct Record { u64 id, write; };
static_assert(sizeof(Record)==16);
std::vector<Record> records;
Info info;
std::string selected;
u64 source_cluster=0;
std::filesystem::path Locate(const std::string& code) {
  namespace fs=std::filesystem;
  if (const char* value=std::getenv("TIMNAH_TRACE_DIR")) {
    auto p=fs::path(value)/(code+".bin");
    if (!fs::is_regular_file(p)) throw std::runtime_error("Trace file missing: "+p.string());
    return p;
  }
  for (auto root : {fs::absolute(fs::path(__FILE__)).parent_path(),fs::current_path()}) {
    while (!root.empty()) {
      auto p=root/"benchmark/twitter/data"/(code+".bin");
      if (fs::is_regular_file(p))return p;
      auto parent=root.parent_path();if(parent==root)break;root=parent;
    }
  }
  throw std::runtime_error("Real trace not installed for "+code+". Run benchmark/twitter/prepare_traces.py; see benchmark/twitter/README.md.");
}
}
auto Current() -> const Info& { return info; }
auto Initialize(const std::string& code,uint64_t maximum_keys) -> Info {
  auto path=Locate(code);
  std::ifstream input(path,std::ios::binary);
  char magic[8],sha[64];u64 n,keys,reads,updates,expected;
  auto read=[&](void* p,size_t bytes){if(!input.read(static_cast<char*>(p),bytes))throw std::runtime_error("Truncated real trace: "+path.string());};
  read(magic,8);read(&n,8);read(&keys,8);read(&reads,8);read(&updates,8);read(&expected,8);read(sha,64);
  if(std::memcmp(magic,"TTRACE01",8)||!n||!keys||keys>n||reads+updates!=n||!maximum_keys||
      n>(std::numeric_limits<u64>::max()-112)/16||std::filesystem::file_size(path)!=112+n*16)
    throw std::runtime_error("Invalid real trace header: "+path.string());
  auto manifest_path=path;manifest_path.replace_extension(".json");
  std::ifstream manifest_stream(manifest_path);
  Json::Value manifest;
  if(!manifest_stream || !(manifest_stream >> manifest) ||
      manifest["workload"].asString()!=code || manifest["source_sha256"].asString()!=std::string(sha,64) ||
      manifest["records"].asUInt64()!=n || manifest["unique_keys"].asUInt64()!=keys ||
      manifest["reads"].asUInt64()!=reads || manifest["updates"].asUInt64()!=updates)
    throw std::runtime_error("Real trace provenance does not match its binary: "+manifest_path.string());
  source_cluster=manifest.get("cluster",Json::UInt64(0)).asUInt64();
  if(code!="w") {
    const auto url=manifest["source_url"].asString();
    const auto marker=url.rfind("cluster");
    if(marker!=std::string::npos && marker+7<url.size() && std::isdigit(static_cast<unsigned char>(url[marker+7]))) {
      const auto from_url=std::stoull(url.substr(marker+7));
      if(source_cluster && source_cluster!=from_url)throw std::runtime_error("Trace cluster metadata disagrees with its source URL");
      source_cluster=from_url;
    }
    if(!source_cluster)source_cluster=code=="ts"?3:code=="tt"?26:8;
  }
  const auto scope=manifest["scope"].asString();
  if(scope.empty() || !std::all_of(scope.begin(),scope.end(),[](unsigned char c){return std::isalnum(c)||c=='_'||c=='-'||c=='.';}))
    throw std::runtime_error("Trace scope must be a nonempty alphanumeric identifier");
  records.clear();records.reserve(n);u64 checksum=0,seen_reads=0,seen_updates=0;
  const u64 limit=std::min(keys,maximum_keys);
  std::vector<bool> seen(limit,false);
  std::array<Record,4096> buffer;
  for(u64 offset=0;offset<n;) {
    const u64 batch=std::min<u64>(buffer.size(),n-offset);read(buffer.data(),batch*sizeof(Record));
    for(u64 i=0;i<batch;++i) {
      const auto rec=buffer[i];
      if(!rec.id||rec.id>keys||rec.write>1)throw std::runtime_error("Invalid real trace record");
      checksum=(checksum^((rec.id<<1)|rec.write))*1099511628211ULL;
      seen_reads+=!rec.write;seen_updates+=rec.write;
      if(rec.id<=limit){records.push_back(rec);seen[rec.id-1]=true;}
    }
    offset+=batch;
  }
  if(checksum!=expected||seen_reads!=reads||seen_updates!=updates||records.empty()||
      std::find(seen.begin(),seen.end(),false)!=seen.end())throw std::runtime_error("Real trace integrity check failed");
  info={records.size(),limit,0,0,maximum_keys,keys,std::string(sha,64),path.string(),scope};
  for(const auto& rec:records){info.reads+=!rec.write;info.updates+=rec.write;}
  selected=code;
  LOG("Real dataset workload={} records={} unique_keys={} requested_max_keys={} source_unique_keys={} source_records={} preload_keys={} background_keys={} key_selection={} source_sha256={} path={} key_bytes=8 value_bytes=8 scope={} source_cluster={} adaptation=index_read_overwrite finite_trace_wrap=true\n",
      code,info.records,info.keys,maximum_keys,keys,n,maximum_keys,maximum_keys-limit,
      keys>maximum_keys?"first_seen_key_subset":"all_observed_keys",info.source_sha256,info.path,info.scope,source_cluster);
  return info;
}
auto ProfileFor(const std::string& code) -> synthetic::Profile {
  if(code!=selected||records.empty())throw std::runtime_error("Real trace must be initialized before use");
  const std::string name=code=="w"?"Webmail":std::string(code=="ts"?"Twitter-Storage":code=="tt"?"Twitter-Transient":"Twitter-Compute")+" (cluster "+std::to_string(source_cluster)+")";
  return {code,name,code=="w"?"observed_FIU_Webmail_trace":"observed_Twitter_trace",
      double(info.reads)/info.records,double(info.updates)/info.records,0.0};
}
auto Generate(const synthetic::Profile& profile,u64 num_load,u64 num_run,u64 round,synthetic::AuditStats* audit) -> std::vector<Request> {
  if(selected!=profile.code||num_load!=info.preload_keys||!num_run||num_run>std::numeric_limits<u64>::max()/(u128(round)+1))
    throw std::runtime_error("Invalid real trace replay settings");
  std::vector<Request> out(num_run);const u64 start=round*num_run,n=records.size();
  u64 reads=0,updates=0,min_id=num_load,max_id=0;
#pragma omp parallel for schedule(static) reduction(+:reads,updates) reduction(min:min_id) reduction(max:max_id)
  for(u64 i=0;i<num_run;++i) {
    const auto rec=records[(start+i)%n];
    out[i]={HashFunction(rec.id),rec.write?HashFunction(start+i+1):u64{0},rec.write?RequestType::Update:RequestType::Read};
    reads+=!rec.write;updates+=rec.write;min_id=std::min(min_id,rec.id);max_id=std::max(max_id,rec.id);
  }
  if(audit)*audit={reads,updates,start/n,(start+num_run-1)/n,min_id,max_id};
  return out;
}
}
