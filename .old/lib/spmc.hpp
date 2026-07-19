#pragma once
#include <atomic>

template<class T, uint32_t CNT>
class SPMCQueue {
public:
  static_assert(CNT && !(CNT & (CNT - 1)), "CNT must be a power of 2");
  struct Reader {
    operator bool() const { return q; }
    T* read() {
      auto& blk = q->blks[next_idx % CNT];
      uint32_t new_idx = ((std::atomic<uint32_t>*)&blk.idx)->load(std::memory_order_acquire);
      if (int(new_idx - next_idx) < 0) return nullptr;
      next_idx = new_idx + 1;
      return &blk.data;
    }

    T* readLast() {
      T* ret = nullptr;
      while (T* cur = read()) ret = cur;
      return ret;
    }

    SPMCQueue<T, CNT>* q = nullptr;
    uint32_t next_idx;
  };

  Reader getReader() {
    Reader reader;
    reader.q = this;
    reader.next_idx = write_idx + 1;
    return reader;
  }

  template<typename Writer>
  void write(Writer writer) {
    auto& blk = blks[++write_idx % CNT];
    writer(blk.data);
    ((std::atomic<uint32_t>*)&blk.idx)->store(write_idx, std::memory_order_release);
  }

private:
  friend class Reader;
  struct alignas(64) Block {
    uint32_t idx = 0;
    T data;
  } blks[CNT];

  alignas(128) uint32_t write_idx = 0;
};
