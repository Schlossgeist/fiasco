INTERFACE [mpu && mpultiplex]:

#include "arithmetic.h"
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

  /**
   * Maximum number of `Mpu_region_block`s of an `Mpu_regions` object, which
   * determines the maximum number of logical regions.
   */
  static constexpr unsigned Max_region_blocks = 16;

  /*
   * The allocator classes below are stateless: their static members forward
   * to the global buddy allocators, so users can refer to them by type
   * without storing an allocator object per instance.
   */

  /// Region blocks are allocated one at a time, see `Mpu_regions::reserve()`.
  using Buddy_block_alloc = Buddy_t_base<sizeof(Mpu_region_block), 1>;

  /// Allocator for the region blocks of `Mpu_regions`.
  class Block_allocator
  {
  public:
    static void *alloc(unsigned long size);
    static void free(void *block, unsigned long size);
  };

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

EXTENSION class Mpu_regions
{
public:
  /**
   * Construct new MPU region list.
   *
   * \param reserved  Map of regions that are not allocatable.
   */
  explicit Mpu_regions(Mpu_regions_mask const &reserved)
  : _size(Config::Mpultiplex_block_size), _reserved(reserved),
    _blocks{_static_regions}
  { init_indices(0); }

  enum class Init { Reserved_regions };

  /**
   * Construct new MPU region list.
   *
   * This is *not* a copy constructor! The other Mpu_regions object that is
   * passed will be used as a reserved-regions template. To enable fast
   * context switches, the used regions of the other object are still copied.
   *
   * The copied regions keep their index, so the new list takes over the size
   * of `other`, which exceeds the inline block if `other` has grown.
   */
  explicit Mpu_regions(Mpu_regions const &other, Init)
  : _size(Config::Mpultiplex_block_size), _reserved(other._reserved),
    _used_mask(other._size), _blocks{_static_regions}
  {
    init_indices(0);
    if (other._size > size())
      reserve(other._size);

    _reserved |= other._used_mask;
    _reserved.resize(_size);
    for (Mpu_region const &i : other._used_tree)
      {
        unsigned idx = other.index(&i);
        Mpu_region &r = (*this)[idx];
        r.start(i.start());
        r.end(i.end());
        r.attr(i.attr());
      }
  }

  ~Mpu_regions()
  {
    for (unsigned b = 1; b < nr_blocks(); ++b)
      Mpu::Block_allocator::free(_blocks[b], sizeof(Mpu_region_block));
  }

private:
  static constexpr unsigned regions_per_block()
  { return Config::Mpultiplex_block_size; }

  static_assert(Mpu::Max_region_blocks * Config::Mpultiplex_block_size
                <= 1U << 16, "Mpu_region::_index too small");

  unsigned nr_blocks() const { return _size / regions_per_block(); }

  Mpu_region const &region(unsigned i) const
  { return _blocks[i / regions_per_block()][i % regions_per_block()]; }

  Mpu_region &region(unsigned i)
  { return _blocks[i / regions_per_block()][i % regions_per_block()]; }

  unsigned index(Mpu_region const *r) const { return r->_index; }

  /// Set the logical indices of the regions of block `b`.
  void init_indices(unsigned b)
  {
    for (unsigned j = 0; j < regions_per_block(); ++j)
      _blocks[b][j]._index = b * regions_per_block() + j;
  }

  /**
   * Reserves space to be able to store more regions.
   *
   * Only new blocks are allocated, existing regions never move. Hence,
   * pointers to regions and the AVL tree stay valid.
   *
   * \param new_size  The amount to reserve space for.
   *
   * \return The amount that space has actually been reserved for.
   *         May be larger than what was requested.
   */
  size_t reserve(size_t new_size)
  {
    if (new_size <= size())
      {
        WARNX(Info, "Tried to reserve no additional space for this "
                    "Mpu_regions object!");
        return size();
      }

    unsigned new_nr_blocks = cxx::div_ceil(new_size, regions_per_block());
    if (new_nr_blocks > Mpu::Max_region_blocks)
      panic("MPUltiplex: more than %u logical regions requested\n",
            Mpu::Max_region_blocks * regions_per_block());

    for (unsigned b = nr_blocks(); b < new_nr_blocks; ++b)
      {
        auto *blk = static_cast<Mpu_region *>(
          Mpu::Block_allocator::alloc(sizeof(Mpu_region_block)));
        if (!blk)
          panic("MPUltiplex: out of memory for region block\n");
        for (unsigned j = 0; j < regions_per_block(); ++j)
          new (&blk[j]) Mpu_region();
        _blocks[b] = blk;
        init_indices(b);
      }

    _size = new_nr_blocks * regions_per_block();
    return size();
  }

  Mpu_region_block _static_regions;
  /// Region blocks, `_blocks[0]` is `_static_regions`.
  Mpu_region *_blocks[Mpu::Max_region_blocks];
};

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex]:

#include <cstdio>

#include "kmem_alloc.h"
#include "ram_quota.h"

static constexpr size_t Block_pool_size = Mpu::Buddy_block_alloc::Min_size * 64;
static constexpr size_t Bitmap_pool_size
  = Mpu::Buddy_bitmap_alloc::Max_size * 64;

static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Mpu::Buddy_block_alloc> _block_alloc;
static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Mpu::Buddy_bitmap_alloc> _bitmap_alloc;

DEFINE_GLOBAL Global_data<Mpu::Freemap_storage> Mpu::_freemap_storage;

IMPLEMENT
size_t
Mpu_regions::grow()
{
  size_t new_size = reserve(size() * 2);

  // Also grow masks to new size.
  _reserved.resize(new_size);
  _used_mask.resize(new_size);

  printf("[%p] Mpu_regions size extended to %zu\n", this, new_size);

  return new_size;
}

IMPLEMENT static
void *
Mpu::Block_allocator::alloc(unsigned long size)
{ return _block_alloc->alloc(size); }

IMPLEMENT static
void
Mpu::Block_allocator::free(void *block, unsigned long size)
{ _block_alloc->free(block, size); }

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
  setup_allocator(&_block_alloc,  Block_pool_size, &fm_index);
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

  _block_alloc->dump();
  _bitmap_alloc->dump();
}
