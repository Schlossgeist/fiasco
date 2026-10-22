INTERFACE:

#include <cassert>
#include <cstring>
#include <cxx/hlist>
#include "bitmap.h"
#include "config.h"
#include "minmax.h"

class Buddy_base
{
protected:
  unsigned long _base;

  struct Head : cxx::H_list_item
  {
    unsigned long index;

    static void link(cxx::H_list<Head> &h, void *b, unsigned long idx)
    {
      Head *n = static_cast<Head*>(b);
      n->index = idx;
      h.add(n);
    }
  };

  typedef cxx::H_list_bss<Head> B_list;
};

template< unsigned long MIN_SIZE, int NUM_SIZES>
class Buddy_t_base : public Buddy_base
{
  enum { Debug_size_mismatch = 0 };

  template <unsigned long, int>
  friend class Buddy_t_base_tester;

private:
  struct Freemap : Bitmap_storage<unsigned long *>
  {
    void setup(unsigned long *addr, size_t size_in_bytes)
    {
      _bits = addr;
      memset(_bits, 0, size_in_bytes);
    }
  };

public:
  enum : unsigned long
  {
    Min_size = MIN_SIZE,
    Num_sizes = NUM_SIZES,
    Max_size = Min_size << (NUM_SIZES - 1),
  };

  static_assert(Num_sizes > 0,
                "Buddy_t_base: allocator must have at least one size class");
  static_assert(Min_size >= sizeof(Head),
                "Buddy_t_base: minimum block size must fit free-list head");
  static_assert((Min_size % alignof(Head)) == 0,
                "Buddy_t_base: minimum block size must satisfy free-list head alignment");

  static constexpr size_t
  free_map_bytes(unsigned long min_addr, unsigned long max_addr)
  { return Freemap::size_in_bytes(buddy_bits(min_addr, max_addr)); }

  static constexpr unsigned long
  free_map_align()
  { return alignof (unsigned long); }

  void setup_free_map(unsigned long *addr, size_t size_in_bytes)
  { _free_map.setup(addr, size_in_bytes); }

  static constexpr unsigned long
  calc_base_addr(unsigned long min_addr)
  { return min_addr - min_addr % Max_size; }

private:

  /**
   * Calculate the size in bytes that needs to be handled by
   * the free block bitmap.
   * \param min_addr  The minimum address of any memory to be handled
   *                  by the buddy allocator.
   * \param max_addr  The maximum address (inclusive) of any memory
   *                  block handled by the buddy allocator.
   * \returns The size in bytes of memory that must be handled by the buddy's
   *          free-blocks bitmap, taking any alignment constraints into
   *          account.
   */
  static constexpr unsigned long
  calc_max_mem_size(unsigned long min_addr, unsigned long max_addr)
  {
    return max_addr - calc_base_addr(min_addr) + 1;
  }

  static constexpr unsigned long
  buddy_bits(unsigned long min_addr, unsigned long max_addr)
  {
    // the number of bits in the bitmap is given by the amount of the smallest
    // supported blocks. We need an extra bit in the case that the Max_mem
    // is no multiple of Max_size to ensure that buddy() does not access
    // beyond the bitmap.
    auto max_mem_size = calc_max_mem_size(min_addr, max_addr);
    unsigned long bits = (max_mem_size + Min_size - 1) / Min_size;
    if (max_mem_size % Max_size > 0)
      bits += 1;
    return bits;
  }

  // Index of the Min_size-unit at address 'addr', counted from '_base'.
  unsigned long
  unit_of(unsigned long addr) const
  { return (addr - _base) / Min_size; }

  // Address of the Min_size-unit with the given index.
  unsigned long
  addr_of(unsigned long index) const
  { return _base + index * Min_size; }

  B_list _free[Num_sizes];
  Freemap _free_map;
};

constexpr auto Buddy_alloc_min_size = 1UL << 10;
class Buddy_alloc : public Buddy_t_base<Buddy_alloc_min_size, 11>
{
};


//----------------------------------------------------------------------------
IMPLEMENTATION:

#include <cstdio>
#include <cassert>
#include "warn.h"

PRIVATE
template<unsigned long A, int B>
inline
Buddy_base::Head *
Buddy_t_base<A,B>::buddy(void *block, unsigned long index, Head **new_block)
{
  if (index + 1 >= Num_sizes)
    return nullptr;

  // Buddy calculation is based on units (multiples of Min_size counted from
  // _base). A block of size class 'index' spans (1 << index) units and starts
  // at a unit number that is a multiple of (1 << index). The unit number of
  // the buddy of a given block differs only in this 'index' bit and the
  // merged block starts at the lower of the two.
  unsigned long const unit = unit_of(reinterpret_cast<unsigned long>(block));
  unsigned long const buddy_unit = unit ^ (1UL << index);
  unsigned long const merged_unit = min(unit, buddy_unit);

  *new_block = reinterpret_cast<Head*>(addr_of(merged_unit));
  Head * const _buddy_h = reinterpret_cast<Head*>(addr_of(buddy_unit));

  // this test may access one bit behind our maximum, this is safe because
  // we allocated an extra bit
  if (_free_map[buddy_unit] && _buddy_h->index == index)
    return _buddy_h;

  return nullptr;
}

PUBLIC
template<unsigned long A, int B>
inline
void
Buddy_t_base<A,B>::free(void *block, unsigned long size)
{
  assert (size >= Min_size);
  assert (reinterpret_cast<unsigned long>(block) >= _base);
  assert ((reinterpret_cast<unsigned long>(block) - _base) % Min_size == 0);
  //assert ((unsigned long)block - _base < Max_mem);
  assert (!_free_map[(reinterpret_cast<unsigned long>(block) - _base)
                     / Min_size]);
  unsigned size_index = 0;
  while ((static_cast<unsigned long>(Min_size) << size_index) < size)
    ++size_index;

  if constexpr (Debug_size_mismatch)
    if (size != static_cast<unsigned long>(Min_size) << size_index)
      WARNX(Info, "Buddy::free: Size mismatch: %lx v %lx [%p]\n",
            size, static_cast<unsigned long>(Min_size) << size_index,
            __builtin_return_address(0));

  // no need to look for a buddy if we already have the biggest block size
  while (size_index + 1 < Num_sizes)
    {
      Head *n;
      Head *b = buddy(block, size_index, &n);
      if (b)
        {
          B_list::remove(b);
          _free_map.clear_bit((reinterpret_cast<unsigned long>(b) - _base)
                              / Min_size);
          block = n;
          ++size_index;
        }
      else
	break;
    }

  Head::link(_free[size_index], block, size_index);
  _free_map.set_bit((reinterpret_cast<unsigned long>(block) - _base) / Min_size);
}

PUBLIC
template<unsigned long A, int B>
void
Buddy_t_base<A,B>::add_mem(void *b, unsigned long size)
{
  unsigned long start = reinterpret_cast<unsigned long>(b);
  assert (start >= _base);

  // Blocks must be Min_size-aligned relative to '_base' (not in absolute
  // terms, Min_size may not be a power of two). Round start up accordingly
  // and round the size down to a multiple of Min_size.
  unsigned long rem = (start - _base) % Min_size;
  unsigned long pad = rem ? Min_size - rem : 0;

  if (size <= pad)
    return;

  unsigned long al_start = start + pad;
  size -= pad;
  size -= size % Min_size;

  while (size)
    {
      free(reinterpret_cast<void*>(al_start), Min_size);
      al_start += Min_size;
      size -= Min_size;
    }
}

PRIVATE
template<unsigned long A, int B>
inline
void
Buddy_t_base<A,B>::split(Head *b, unsigned size_index, unsigned i)
{
  for (; i > size_index; ++size_index)
    {
      unsigned long buddy = reinterpret_cast<unsigned long>(b)
                            + (Min_size << size_index);
      Head::link(_free[size_index], reinterpret_cast<void*>(buddy), size_index);
      _free_map.set_bit((buddy - _base) / Min_size);
    }
}

PUBLIC
template<unsigned long A, int B>
inline
void *
Buddy_t_base<A,B>::alloc(unsigned long size)
{
  unsigned size_index = 0;
  while ((static_cast<unsigned long>(Min_size) << size_index) < size)
    ++size_index;

  if constexpr (Debug_size_mismatch)
    if (size != static_cast<unsigned long>(Min_size) << size_index)
      WARNX(Info, "Buddy::alloc: Size mismatch: %lx v %lx [%p]\n",
            size, static_cast<unsigned long>(Min_size) << size_index,
            __builtin_return_address(0));

  for (unsigned i = size_index; i < Num_sizes; ++i)
    {
      Head *f = _free[i].front();
      if (f)
        {
          B_list::remove(f);
          split(f, size_index, i);
          _free_map.clear_bit((reinterpret_cast<unsigned long>(f) - _base)
                            / Min_size);
          return f;
        }
    }
  return nullptr;
}

PUBLIC
template< unsigned long A, int B>
void
Buddy_t_base<A,B>::init(unsigned long base)
{ _base = calc_base_addr(base); }

PUBLIC
template< unsigned long A, int B>
unsigned long
Buddy_t_base<A,B>::avail() const
{
  unsigned long a = 0;
  for (unsigned i = 0; i < Num_sizes; ++i)
    {
      for (B_list::Const_iterator h = _free[i].begin(); h != _free[i].end(); ++h)
        a += (Min_size << i);
    }
  return a;
}

//----------------------------------------------------------------------------
IMPLEMENTATION [debug || jdb_kmem_stats]:

PUBLIC
template< unsigned long A, int B>
void
Buddy_t_base<A,B>::dump() const
{
  auto print_freelist_entry = [](B_list::Const_iterator &h, bool valid)
    {
      void *hp = static_cast<void *>(*h);
      if (!hp)
        printf("null");
      else
        printf("%p", hp);
      printf("(%lu)", valid ? h->index : 0UL);
    };

  auto size_str = [](unsigned long sz, char *buf)
    {
      static char const *units[] =
        { "B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB" }; // 2^64 == 16 EiB

      unsigned u = 0;
      while (u < 6 && (sz >> (10 * (u + 1))) != 0)
        ++u;

      unsigned shift = 10 * u;
      unsigned rem = sz & ((1U << shift) - 1);
      unsigned whole = sz >> shift;

      if (u == 0)
        snprintf(buf, 16, "%u %3s", whole, units[0]);
      else
        snprintf(buf, 16, "%u.%hu %3s", whole, (rem * 10) >> shift, units[u]);
      return buf;
    };

  static char buf[16];

  unsigned long total = 0;
  printf("Buddy_alloc [%ld,%ld]\n", Min_size, Num_sizes);
  for (unsigned i = 0; i < Num_sizes; ++i)
    {
      unsigned long c = 0;
      unsigned long avail = 0;
      B_list::Const_iterator h = _free[i].begin();
      printf("  [%10s] ", size_str(Min_size << i, buf));
      print_freelist_entry(h, h != _free[i].end());
      unsigned const elide_after = 8;
      while (h != _free[i].end())
        {
          ++h;
          if (c <= elide_after)
            printf("\n%15s", c < elide_after ? "-> " : "...");
          if (c < elide_after)
            print_freelist_entry(h, !!*h);

          avail += Min_size << i;
          ++c;
        }
      printf(" == %s (%lu bytes)\n", size_str(avail, buf), avail);
      total += avail;
    }

  printf("sum of available memory: %s (%lu bytes)\n",
         size_str(total, buf), total);
}
