// Included inside the isolated ablation Impl class only. Production and the
// full Hot Lock source do not include this file. key_state's occupancy/version
// is the ordinary optimistic Version Lock used before K/V decoupling.
  auto FlatSearch(u64 key, u64& value) -> Result {
    const auto hash = Hash(key);
    for (;;) {
      const auto found = Find(*slow_, hash, key, false);
      if (!found.stable) { std::this_thread::yield(); continue; }
      if (found.slot < 0) return Result::FAILED;
      value = found.value;
      return Result::SUCCESS;
    }
  }

  auto FlatInsert(u64 key, u64 value) -> Result {
    const auto hash = Hash(key);
    for (;;) {
      const auto route = slow_->Locate(hash);
      StructuralLocks locks;
      if (!locks.Try({{route.bucket, false}})) { std::this_thread::yield(); continue; }
      if (!slow_->Valid(route, hash)) continue;
      if (FindHeld(*route.bucket, key) >= 0) return Result::FAILED;
      const auto mask = route.bucket->Mask();
      if (mask != kFullMask) {
        const auto slot = std::countr_zero((~mask) & kFullMask);
        Put(*route.bucket, slot, key, value, 0);
        size_.fetch_add(1, std::memory_order_relaxed);
        return Result::SUCCESS;
      }
      locks.Release();
      slow_->Split(hash, route.entry, 0, options_.decay_factor);
    }
  }

  auto FlatUpdate(u64 key, u64 value) -> Result {
    const auto hash = Hash(key);
    for (;;) {
      const auto route = slow_->Locate(hash);
      StructuralLocks locks;
      if (!locks.Try({{route.bucket, false}})) { std::this_thread::yield(); continue; }
      if (!slow_->Valid(route, hash)) continue;
      const auto slot = FindHeld(*route.bucket, key);
      if (slot < 0) return Result::FAILED;
      Write(route.bucket->slots[slot].value, value);
      return Result::SUCCESS;
    }
  }

  auto FlatRemove(u64 key) -> Result {
    const auto hash = Hash(key);
    for (;;) {
      const auto route = slow_->Locate(hash);
      StructuralLocks locks;
      if (!locks.Try({{route.bucket, false}})) { std::this_thread::yield(); continue; }
      if (!slow_->Valid(route, hash)) continue;
      const auto slot = FindHeld(*route.bucket, key);
      if (slot < 0) return Result::FAILED;
      Erase(*route.bucket, static_cast<u32>(slot));
      size_.fetch_sub(1, std::memory_order_relaxed);
      return Result::SUCCESS;
    }
  }

  auto VersionUpdate(u64 key, u64 value) -> Result {
    const auto hash = Hash(key);
    for (;;) {
      const auto fast = fast_->Locate(hash);
      struct Guard {
        Bucket* held{};
        bool Try(Bucket* bucket) {
          if (!bucket->TryKeyLock()) return false;
          held = bucket; return true;
        }
        ~Guard() { if (held) held->KeyUnlock(); }
      } fast_lock, slow_lock;
      if (!fast_lock.Try(fast.bucket)) { std::this_thread::yield(); continue; }
      const auto fast_slot = FindHeld(*fast.bucket, key);
      if (fast_slot >= 0) {
        Write(fast.bucket->slots[fast_slot].value, value);
        return Result::SUCCESS;
      }
      // A DRAM hit never fetches or locks CXL. A DRAM miss retains its Version
      // Lock while the CXL bucket is located, locked, searched and updated.
      const auto slow = slow_->Locate(hash);
      if (!slow_lock.Try(slow.bucket) || !slow_->Valid(slow, hash)) continue;
      const auto slow_slot = FindHeld(*slow.bucket, key);
      if (slow_slot < 0) return Result::FAILED;
      Write(slow.bucket->slots[slow_slot].value, value);
      return Result::SUCCESS;
    }
  }
