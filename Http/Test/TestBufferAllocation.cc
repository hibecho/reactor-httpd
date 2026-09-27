// 分配失败注入在独立进程中运行，避免替换 sanitizer 的分配器。
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

#include "base/Buffer.hpp"

namespace
{
bool fail_next_allocation = false;
}

void* operator new(std::size_t size)
{
  if (fail_next_allocation)
  {
    fail_next_allocation = false;
    throw std::bad_alloc();
  }
  if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
// C++14 起编译器优先调用 sized 版本，只定义非 sized 版本会触发 -Wsized-deallocation；
// 两个版本都必须保留（只留 sized 会触发反向警告）。sized 版本转调非 sized，不可自调。
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete[](p); }

#define CHECK(expr)                                                 \
  do                                                                \
  {                                                                 \
    if (!(expr))                                                    \
      throw std::runtime_error("line " + std::to_string(__LINE__) + \
                               ": " #expr);                         \
  } while (false)

struct Snapshot {
  std::string data;
  std::size_t front, back;
  const char* base;
  explicit Snapshot(const Buffer& b)
      : data(b.PeekAsString(b.GetReadableSize())),
        front(b.GetPrependableSize()),
        back(b.GetWritableSize()),
        base(b.Begin())
  {
  }
  void Verify(const Buffer& b) const
  {
    CHECK(b.PeekAsString(b.GetReadableSize()) == data);
    CHECK(b.GetPrependableSize() == front);
    CHECK(b.GetWritableSize() == back);
    CHECK(b.Begin() == base);
  }
};

template <class F>
void Fails(F operation)
{
  fail_next_allocation = true;
  bool caught = false;
  try
  {
    operation();
  } catch (const std::bad_alloc&)
  {
    caught = true;
  } catch (...)
  {
    fail_next_allocation = false;
    throw;
  }
  fail_next_allocation = false;
  CHECK(caught);
}

int main()
{
  try
  {
    const std::string payload = std::string(256, 'x') + "\r\n";
    Buffer b(payload.size());
    b.WriteString(payload);
    b.MoveReadOffset(1);
    const Snapshot original(b);
    Fails([&] { Buffer constructed(128); });
    Fails([&] { b.EnsureWritableSize(4096); });
    original.Verify(b);
    Fails([&] { b.WriteString(payload); });
    original.Verify(b);
    Fails([&] { b.WriteBuffer(b); });
    original.Verify(b);
    Buffer source;
    source.WriteString(payload);
    const Snapshot source_before(source);
    Fails([&] { b.WriteBuffer(source); });
    original.Verify(b);
    source_before.Verify(source);
    Fails([&] { b.PeekAsString(b.GetReadableSize()); });
    original.Verify(b);
    Fails([&] { b.ReadAsString(b.GetReadableSize()); });
    original.Verify(b);
    Fails([&] { b.PeekLine(); });
    original.Verify(b);
    Fails([&] { b.ReadLine(); });
    original.Verify(b);
    Fails([&] { Buffer copy(b); });
    original.Verify(b);
    Buffer target(1);
    target.WriteString("a");
    const Snapshot target_before(target);
    Fails([&] { target = b; });
    original.Verify(b);
    target_before.Verify(target);
    CHECK(b.ReadLine() == payload.substr(1));
    CHECK(b.GetReadableSize() == 0);
    std::cout << "Buffer allocation failure tests passed (11 cases)\n";
  } catch (const std::exception& e)
  {
    fail_next_allocation = false;
    std::cerr << e.what() << '\n';
    return 1;
  }
}
