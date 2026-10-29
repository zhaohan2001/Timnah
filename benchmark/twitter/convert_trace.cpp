#include "compact_key_dictionary.hpp"
#include <array>
#include <charconv>
#include <string_view>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using u64=std::uint64_t;
struct Record { u64 id, write; };
int main(int argc,char** argv) {
  try {
    if(argc!=5)throw std::runtime_error("Expected workload, output, statistics and source SHA-256");
    const std::string code=argv[1],sha=argv[4];
    if(sha.size()!=64)throw std::runtime_error("Invalid source SHA-256");
    std::ios::sync_with_stdio(false);std::cin.tie(nullptr);
    std::ofstream output(argv[2],std::ios::binary|std::ios::trunc);
    if(!output)throw std::runtime_error("Cannot create converted trace");
    const std::array<char,112> empty{};output.write(empty.data(),empty.size());
    ExactKeyDictionary<> ids;
    std::unordered_set<std::string> deleted_keys;
    std::unordered_map<std::string,u64> operations;
    std::vector<Record> buffer;buffer.reserve(65536);
    u64 count=0,reads=0,updates=0,checksum=0,lines=0,excluded=0;
    u64 first_timestamp=0,last_timestamp=0,timestamp_regressions=0;
    std::string line;
    auto flush=[&](){output.write(reinterpret_cast<const char*>(buffer.data()),buffer.size()*sizeof(Record));buffer.clear();if(!output)throw std::runtime_error("Trace write failed");};
    while(std::getline(std::cin,line)) {
      ++lines;if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;
      std::string_view key;std::string op;
      if(code=="w") {
        auto split=line.find_first_of(" \t");
        if(split==std::string::npos)throw std::runtime_error("Invalid Webmail record");
        key=std::string_view(line).substr(0,split);auto start=line.find_first_not_of(" \t",split);
        op=start==std::string::npos?"":line.substr(start);
        if(op!="R"&&op!="W")throw std::runtime_error("Invalid Webmail operation");
      } else {
        std::array<std::string_view,7> fields;
        const auto first=line.find(',');std::size_t end=line.size();
        if(first==std::string::npos)throw std::runtime_error("Invalid Twitter timestamp separator");
        fields[0]=std::string_view(line).substr(0,first);
        for(unsigned i=6;i>1;--i) {
          const auto separator=line.rfind(',',end-1);
          if(separator==std::string::npos||separator<=first)throw std::runtime_error("Invalid Twitter field count");
          fields[i]=std::string_view(line).substr(separator+1,end-separator-1);end=separator;
        }
        fields[1]=std::string_view(line).substr(first+1,end-first-1);
        key=fields[1];op=fields[5];
        u64 timestamp=0;
        const auto parsed=std::from_chars(fields[0].data(),fields[0].data()+fields[0].size(),timestamp);
        if(parsed.ec!=std::errc{}||parsed.ptr!=fields[0].data()+fields[0].size())throw std::runtime_error("Invalid Twitter timestamp");
        if(lines==1)first_timestamp=timestamp;
        if(timestamp<last_timestamp)++timestamp_regressions;
        last_timestamp=timestamp;
      }
      if(key.empty())throw std::runtime_error("Empty trace key");
      ++operations[op];
      if(op=="delete") {++excluded;deleted_keys.emplace(key);continue;}
      const bool read=op=="get"||op=="gets"||op=="R";
      const bool write=op=="set"||op=="add"||op=="replace"||op=="cas"||op=="append"||op=="prepend"||op=="incr"||op=="decr"||op=="W";
      if(!read&&!write)throw std::runtime_error("Unsupported trace operation: "+op);
      const u64 id=ids.Intern(key);
      buffer.push_back({id,u64(write)});if(buffer.size()==buffer.capacity())flush();
      checksum=(checksum^((id<<1)|u64(write)))*1099511628211ULL;
      ++count;reads+=read;updates+=write;
      if(count%10000000==0)std::cerr<<"Converted records="<<count<<" unique_keys="<<ids.size()<<'\n';
    }
    if(!std::cin.eof()||!count)throw std::runtime_error("Input read failed or trace empty");
    flush();output.seekp(0);output.write("TTRACE01",8);
    const std::array<u64,5> header{count,ids.size(),reads,updates,checksum};
    output.write(reinterpret_cast<const char*>(header.data()),sizeof(header));output.write(sha.data(),64);output.close();
    if(!output)throw std::runtime_error("Trace finalization failed");
    u64 raw_keys=ids.size();for(const auto& key:deleted_keys)raw_keys+=ids.Find(key)==0;
    std::ofstream stats(argv[3]);
    stats<<"{\"records\":"<<count<<",\"unique_keys\":"<<ids.size()<<",\"raw_unique_keys\":"<<raw_keys<<",\"raw_records\":"<<lines<<",\"reads\":"<<reads<<",\"updates\":"<<updates<<",\"excluded_delete\":"<<excluded<<",\"first_timestamp\":"<<first_timestamp<<",\"last_timestamp\":"<<last_timestamp<<",\"timestamp_regressions\":"<<timestamp_regressions<<",\"original_operations\":{";
    bool comma=false;for(const auto& item:operations){if(comma)stats<<',';stats<<'"'<<item.first<<"\":"<<item.second;comma=true;}stats<<"}}\n";
    if(!stats)throw std::runtime_error("Statistics write failed");
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
