#pragma once
#include <thread>
#include <atomic>
#include <vector>
#include <algorithm>

// Loop paralel sederhana (pakai semua core CPU)
template <class F> void parallelFor(int n, F f, int chunk = 1) {
  unsigned nt = std::max(1u, std::thread::hardware_concurrency());
  if (n <= 1 || nt == 1) { for (int i = 0; i < n; i++) f(i); return; }
  std::atomic<int> next{0};
  auto work = [&]() {
    for (;;) {
      int s = next.fetch_add(chunk);
      if (s >= n) break;
      int e = std::min(n, s + chunk);
      for (int i = s; i < e; i++) f(i);
    }
  };
  std::vector<std::thread> th;
  for (unsigned i = 1; i < nt; i++) th.emplace_back(work);
  work();
  for (auto& t : th) t.join();
}
