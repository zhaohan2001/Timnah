#include "compact_key_dictionary.hpp"
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
using u64=std::uint64_t;
int main(int argc,char**argv){
 try{
  if(argc!=4)throw std::runtime_error("Expected output CSV, statistics and distinct-key limit");
  const u64 limit=std::stoull(argv[3]);if(!limit)throw std::runtime_error("Invalid key limit");
  std::ios::sync_with_stdio(false);std::cin.tie(nullptr);
  std::ofstream out(argv[1],std::ios::binary);if(!out)throw std::runtime_error("Cannot open CSV output");
  ExactKeyDictionary<> ids;std::string line;
  u64 lines=0,records=0,reads=0,updates=0,deletes=0,first=0,last=0,regressions=0;
  while(std::getline(std::cin,line)){
   if(std::cin.eof())break;
   if(!line.empty()&&line.back()=='\r')line.pop_back();
   if(line.empty())throw std::runtime_error("Unexpected empty CSV record");
   const auto first_comma=line.find(',');std::size_t end=line.size();
   if(first_comma==std::string::npos)throw std::runtime_error("Invalid timestamp separator");
   std::array<std::string_view,7> fields;fields[0]=std::string_view(line).substr(0,first_comma);
   for(unsigned i=6;i>1;--i){const auto sep=line.rfind(',',end-1);
    if(sep==std::string::npos||sep<=first_comma)throw std::runtime_error("Invalid CSV fields");
    fields[i]=std::string_view(line).substr(sep+1,end-sep-1);end=sep;}
   fields[1]=std::string_view(line).substr(first_comma+1,end-first_comma-1);
   if(fields[1].empty())throw std::runtime_error("Empty trace key");
   u64 time=0;const auto parsed=std::from_chars(fields[0].data(),fields[0].data()+fields[0].size(),time);
   if(parsed.ec!=std::errc{}||parsed.ptr!=fields[0].data()+fields[0].size())throw std::runtime_error("Invalid timestamp");
   const auto op=fields[5];const bool read=op=="get"||op=="gets";
   const bool write=op=="set"||op=="add"||op=="replace"||op=="cas"||op=="append"||op=="prepend"||op=="incr"||op=="decr";
   if(!read&&!write&&op!="delete")throw std::runtime_error("Unsupported operation");
   if(!lines){first=time;}
   if(time<last){++regressions;}
   last=time;
   ++lines;out<<line<<'\n';if(!out)throw std::runtime_error("CSV write failed");
   if(op=="delete")++deletes;
   else{ids.Intern(fields[1]);++records;reads+=read;updates+=write;}
   if(lines%10000000==0)std::cerr<<"Prefix records="<<lines<<" retained="<<records<<" unique_keys="<<ids.size()<<'\n';
   if(ids.size()==limit)break;
  }
  if(!std::cin.eof()&&std::cin.fail())throw std::runtime_error("CSV read failed");
  out.close();if(!out)throw std::runtime_error("CSV finalization failed");
  std::ofstream stats(argv[2]);stats<<"{\"raw_records\":"<<lines<<",\"records\":"<<records<<",\"unique_keys\":"<<ids.size()
   <<",\"reads\":"<<reads<<",\"updates\":"<<updates<<",\"excluded_delete\":"<<deletes<<",\"first_timestamp\":"<<first
   <<",\"last_timestamp\":"<<last<<",\"timestamp_regressions\":"<<regressions<<",\"maximum_keys\":"<<limit
   <<",\"key_limit_reached\":"<<(ids.size()==limit?"true":"false")<<"}\n";
  if(!stats)throw std::runtime_error("Statistics write failed");
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
