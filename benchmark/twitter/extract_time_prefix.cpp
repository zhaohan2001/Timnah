#include <charconv>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
using u64=std::uint64_t;
int main(int argc,char**argv){
 try{
  if(argc!=4)throw std::runtime_error("Expected output CSV, statistics and exclusive end timestamp");
  const u64 end=std::stoull(argv[3]);if(!end)throw std::runtime_error("End timestamp must be positive");
  std::ios::sync_with_stdio(false);std::cin.tie(nullptr);
  std::ofstream out(argv[1],std::ios::binary);std::string line;u64 count=0,first=0,last=0;bool boundary=false;
  while(std::getline(std::cin,line)){
   if(std::cin.eof())break;
   const auto separator=line.find(',');if(separator==std::string::npos)throw std::runtime_error("Invalid timestamp field");
   u64 timestamp=0;const auto parsed=std::from_chars(line.data(),line.data()+separator,timestamp);
   if(parsed.ec!=std::errc{}||parsed.ptr!=line.data()+separator)throw std::runtime_error("Invalid timestamp");
   if(count&&timestamp<last)throw std::runtime_error("Input must be timestamp-sorted");
   if(timestamp>=end){boundary=true;break;}
   if(!count)first=timestamp;
   out<<line<<'\n';if(!out)throw std::runtime_error("CSV write failed");last=timestamp;++count;
  }
  out.close();if(!out||!count||first!=0||!boundary)throw std::runtime_error("Incomplete initial time interval");
  std::ofstream stats(argv[2]);stats<<"{\"raw_records\":"<<count<<",\"first_timestamp\":"<<first<<",\"last_timestamp\":"<<last<<",\"exclusive_end_timestamp\":"<<end<<",\"boundary_observed\":true}\n";
  if(!stats)throw std::runtime_error("Statistics write failed");
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
