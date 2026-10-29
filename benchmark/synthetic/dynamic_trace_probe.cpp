#include "benchmark/synthetic/dynamic_trace.hpp"
#include "benchmark/synthetic/trace_replay.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
void Require(bool value){if(!value)throw std::runtime_error("Real replay correctness check failed");}
int main(){
  try {
    for(const std::string code:{"ts","tt","tc","w"}) {
      auto info=real_trace::Initialize(code,10000);
      auto profile=synthetic::GetProfile(code);
      std::ifstream f(info.path,std::ios::binary);f.seekg(112);
      struct Record{u64 id,write;};Record rec;std::vector<Record> observed;
      while(f.read(reinterpret_cast<char*>(&rec),sizeof(rec)))if(rec.id<=info.keys)observed.push_back(rec);
      Require(observed.size()==info.records);
      for(u64 round:{0,1,100}) {
        synthetic::AuditStats a,b;omp_set_num_threads(1);
        auto first=synthetic::GenerateRound(profile,info.preload_keys,50000,round,1729,1,1,&a);
        omp_set_num_threads(4);auto parallel=synthetic::GenerateRound(profile,info.preload_keys,50000,round,1729,1,1,&b);
        u64 reads=0,updates=0;
        for(u64 i=0;i<first.size();++i){
          const auto expected=observed[(round*50000+i)%observed.size()];
          Require(first[i].key==HashFunction(expected.id));
          Require(first[i].type==(expected.write?RequestType::Update:RequestType::Read));
          Require(first[i].value==(expected.write?HashFunction(round*50000+i+1):0));
          Require(first[i].key==parallel[i].key&&first[i].value==parallel[i].value&&first[i].type==parallel[i].type);
          reads+=!expected.write;updates+=expected.write;
        }
        Require(a.reads==reads&&a.updates==updates&&b.reads==reads&&b.updates==updates);
      }
      std::cout<<"Real replay correctness passed: "<<code<<" key_bytes=8 value_bytes=8\n";
    }
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
