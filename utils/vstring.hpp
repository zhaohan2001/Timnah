// In-memory representation of a variable-length string, following the C convention.
// Data structure:
//  - String length
//  - String contents
// Member functions:

#pragma once
#include "headers.hpp"

class VString {
 private:
  u32 size;
  char str[0];

  VString(const string& s) {
    size = s.size();
    strcpy(str, s.c_str());
  }

 public:
  static VString* create(void* addr, const string& s) {
    VString* p = new (addr) VString(s);
    return p;
  }

  // Basic queries
  u32 getSize() const { return size; }
  const char* getStr() const { return str; }
  u32 requiredSize() const { return sizeof(VString) + size + 1; }
  static u32 requiredSize(const string& s) {
    return sizeof(VString) + s.size() + 1;
  }

  // Conversions
  string toString() const { return string(str, size); }
  static string toString(const VString* p) { return string(p->str, p->size); }
  string_view toStringView() const { return string_view(str, size); }
};
