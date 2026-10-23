INTERFACE [mpu && mpultiplex]:

#include "buddy_alloc.h"
#include "minmax.h"
#include "panic.h"

EXTENSION class Mpu_regions_mask
{
public:
  explicit Mpu_regions_mask(unsigned size = Mpu::regions())
  : Bitmap_type(size)
  { clear_all(); }
};

EXTENSION class Mpu_regions_update
{
public:
  explicit Mpu_regions_update(unsigned size = Mpu::regions())
  : Mpu_regions_mask(size)
  {}

  explicit Mpu_regions_update(Error error, unsigned size = Mpu::regions())
  : Mpu_regions_mask(size)
  { _error_state |= error; }
};

EXTENSION class Mpu
{
public:
  /**
   * Initialize allocation functionality used by MPU code.
   */
  static void init_alloc();

  /*
   * The allocator classes below are stateless: their static members forward
   * to the global buddy allocators, so users can refer to them by type
   * without storing an allocator object per instance.
   */

  using Buddy_bitmap_alloc
    = Buddy_t_base<max(2 * Config::Mpultiplex_block_size / 8, 24U), 2>;

  /// Allocator for the storage of `Dynamic_bitmap` (`Mpu_regions_mask`).
  class Dynamic_bitmap_allocator
  {
  public:
    static void *alloc(unsigned long size);
    static void free(void *block, unsigned long size);
  };

private:
  static void *request_kmem(size_t size);

  static constexpr unsigned FREEMAP_STORAGE_ELEMENTS = 16;
  using Freemap_storage = unsigned long[FREEMAP_STORAGE_ELEMENTS];
  static Global_data<Freemap_storage> _freemap_storage;

  template<class A>
  static void
  setup_allocator(A *alloc, size_t initial_size, unsigned *freemap_storage_pos)
  {
    void *initial_mem = request_kmem(initial_size);

    auto   min_addr = reinterpret_cast<Address>(initial_mem);
    size_t max_addr = min_addr + initial_size - 1;
    auto   freemap_size = A::free_map_bytes(min_addr, max_addr);

    // Check before setup_free_map() clears the free map.
    unsigned freemap_elems = freemap_size / sizeof(unsigned long) + 1;
    if (*freemap_storage_pos + freemap_elems > FREEMAP_STORAGE_ELEMENTS)
      panic("MPU: freemap storage exhausted, increase "
            "FREEMAP_STORAGE_ELEMENTS\n");

    unsigned long *freemap_mem = &_freemap_storage[*freemap_storage_pos];
    *freemap_storage_pos += freemap_elems;

    alloc->init(min_addr);
    alloc->setup_free_map(freemap_mem, freemap_size);
    alloc->add_mem(initial_mem, initial_size);
  }
};

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex]:

#include <cstdio>

#include "kmem_alloc.h"
#include "ram_quota.h"

static constexpr size_t Bitmap_pool_size
  = Mpu::Buddy_bitmap_alloc::Max_size * 64;

static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Mpu::Buddy_bitmap_alloc> _bitmap_alloc;

DEFINE_GLOBAL Global_data<Mpu::Freemap_storage> Mpu::_freemap_storage;

IMPLEMENT static
void *
Mpu::Dynamic_bitmap_allocator::alloc(unsigned long size)
{ return _bitmap_alloc->alloc(size); }

IMPLEMENT static
void
Mpu::Dynamic_bitmap_allocator::free(void *block, unsigned long size)
{ _bitmap_alloc->free(block, size); }

IMPLEMENT static inline NEEDS["kmem_alloc.h", "ram_quota.h"]
void *
Mpu::request_kmem(size_t size)
{
  auto const& q = Ram_quota::root.unwrap();
  void *mem = Kmem_alloc::allocator()->q_alloc(q, Bytes(size));
  assert(mem);
  return mem;
}

IMPLEMENT static
void
Mpu::init_alloc()
{
  unsigned fm_index = 0;
  setup_allocator(&_bitmap_alloc, Bitmap_pool_size, &fm_index);

  dump_alloc(fm_index);

  printf("MPU: hardware provides %lu regions, using %u\n",
         Mpu_arm::regions(), Mpu::regions());
}

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex && !debug]:

PRIVATE static inline
void
Mpu::dump_alloc(unsigned)
{}

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex && debug]:

/**
 * Dump the allocators after `init_alloc()` set them up.
 *
 * \param fm_index  Number of freemap words used by the allocators.
 */
PRIVATE static
void
Mpu::dump_alloc(unsigned fm_index)
{
  printf("MPU: used %zu bytes of freemap storage (of %zu allocated bytes)\n",
         sizeof(unsigned long) * fm_index, sizeof(_freemap_storage));

  _bitmap_alloc->dump();
}
