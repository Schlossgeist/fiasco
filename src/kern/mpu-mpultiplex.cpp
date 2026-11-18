INTERFACE [mpu && mpultiplex]:

#include "arithmetic.h"
#include "bitmap.h"
#include "buddy_alloc.h"
#include "minmax.h"
#include "panic.h"
#include "per_cpu_data.h"

/**
 * A snapshot of the state of an `Mpu_regions` object.
 *
 * A snapshot describes the MPU state of a CPU for multiplexing. The region
 * data still lives inside the `Mpu_regions` object, the snapshot only
 * references a consistent set of region versions. Additionally, the snapshot
 * tracks which logical region lives in which physical MPU slot.
 *
 * Snapshots are kept across address space switches (see `Mpu::Cpu_state`).
 * The generation of the region set at the time of the last capture tells
 * whether the snapshot is still up to date, otherwise it is brought up to date
 * in place by `refresh()`.
 *
 * All per logical region state lives in an `Entry`. The entries are not part
 * of the snapshot but provided by the creator (see `Mpu::Snapshot_buffer`),
 * so a snapshot covers any number of logical regions. Only the physical side
 * is of fixed size.
 */
class Snapshot
{
public:
  /**
   * Set of physical MPU slots.
   *
   * Where the hardware format is needed (PRENR, ku_mem mask of the kernel
   * entry code), it is converted with `*raw()`. Both cover regions 0-31 only.
   */
  using Phys_mask = Bitmap<Mem_layout::Mpu_regions>;
  static_assert(Phys_mask::size() <= 32,
                "MPUltiplex: PRENR covers at most 32 regions");

  /**
   * State of one logical region.
   *
   * `phys` and `flags` are declared before `version` so that they fill the
   * tail padding of the AVL tree node.
   */
  struct Entry : public cxx::Avl_tree_node
  {
    /// `Changed`: version changed by the last capture, see `changed()`.
    enum Flags : unsigned char
    { Used = 1, Pinned = 2, Reserved = 4, Changed = 8 };

    signed char phys = -1;           ///< Physical slot, -1 if not active.
    unsigned char flags = 0;         ///< Combination of `Flags`.
    Mpu_region_version_ptr version;  ///< Captured version, null if unused.

    using Key_type = Mword;
    static Key_type key_of(Entry const *e)
    { return e->version->start(); }
  };

  static_assert(sizeof(Entry) == sizeof(cxx::Avl_tree_node)
                                 + sizeof(Mpu_region_version_ptr),
                "Snapshot::Entry: phys/flags should not increase its size");

  /// Entries of one region block, see `Mpu::Snapshot_buffer`.
  using Entry_block = Entry[Config::Mpultiplex_block_size];

  /// \pre `entries` provides storage for at least `regions.size()` entries.
  Snapshot(Mpu_regions const &regions, Snapshot *prev, Entry *entries);
  ~Snapshot();
  Snapshot(Snapshot const &) = delete;
  Snapshot &operator=(Snapshot const &) = delete;

  /// \pre The entry storage holds at least `regions.size()` entries.
  void refresh(Mpu_regions const &regions);

  Mpu_regions const *regions() const { return _regions; }
  unsigned size() const { return _size; }
  unsigned hw_count() const { return _hw; }
  /// Generation of `regions()` at the time of the last capture.
  unsigned generation() const { return _generation; }

  /**
   * Whether the version of logical region `i` changed by the last capture
   * (`refresh()` or construction from a snapshot of the same region set).
   * The flag stays set until `clear_changed()`.
   */
  bool changed(unsigned i) const { return _entries[i].flags & Entry::Changed; }
  void clear_changed(unsigned i) { _entries[i].flags &= ~Entry::Changed; }

  bool used(unsigned i) const     { return _entries[i].flags & Entry::Used; }
  bool active(unsigned i) const   { return _entries[i].phys >= 0; }
  bool pinned(unsigned i) const   { return _entries[i].flags & Entry::Pinned; }
  bool reserved(unsigned i) const
  { return _entries[i].flags & Entry::Reserved; }
  int phys_slot(unsigned i) const { return _entries[i].phys; }
  /// Logical region in physical slot `p`, -1 if the slot is free.
  int logical_slot(unsigned p) const { return _logical[p]; }

  Mpu_region_version const &version(unsigned i) const
  { return *_entries[i].version.get(); }

  /// Region contents to be written into physical slot `p`.
  Mpu_region_base const &hw_slot(unsigned p) const
  {
    if (_phys_used[p])
      return version(_logical[p]);
    return _empty;
  }

  /// Physical slots that may stay enabled while `Mpu::update` runs.
  Mword hw_keep_mask() const { return *_phys_keep.raw(); }

  /// Physical slots holding ku_mem regions, see `Mpu::ku_mem_mask()`.
  Unsigned32 ku_mem_mask() const { return *_phys_ku_mem.raw(); }

  int lookup(Mword addr) const;
  int free_phys() const;
  int pick_victim() const;
  void activate(unsigned i, unsigned p);
  void deactivate(unsigned i);

private:
  using Tree = cxx::Avl_tree<Entry, Entry>;

  static Mpu_region_base const _empty;

  /// Newest version of logical region `i` if it is captured, else nullptr.
  static Mpu_region_version *capture(Mpu_regions const &regions, unsigned i);
  /// `Entry` flags of logical region `i` captured as `v` (may be nullptr).
  static unsigned char flags_of(Mpu_regions const &regions, unsigned i,
                                Mpu_region_version const *v);

  Mpu_regions const *_regions;
  Entry *_entries;      ///< `_size` entries, storage provided by the creator
  unsigned _size;
  unsigned _hw;
  unsigned _generation;
  Phys_mask _phys_used;
  Phys_mask _phys_pinned;                ///< Slots holding a pinned region
  Phys_mask _phys_ku_mem;                ///< Slots holding a ku_mem region
  Phys_mask _phys_keep;     ///< Slots holding a reserved region in identity
  signed char _logical[Mem_layout::Mpu_regions]; ///< physical -> logical slot
  Tree _tree;
};

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

  /**
   * Allocator for the snapshot entries of the per-CPU snapshot slots, see
   * `Snapshot_buffer`.
   *
   * Used by all CPUs, hence protected by a lock.
   */
  class Snapshot_allocator
  {
  public:
    static void *alloc(unsigned long size);
    static void free(void *block, unsigned long size);
    static void dump();
  };

private:
  // ALLOCATOR STUFF
  static void *request_kmem(size_t size);

  static constexpr unsigned FREEMAP_STORAGE_ELEMENTS = 16;
  using Freemap_storage = unsigned long[FREEMAP_STORAGE_ELEMENTS];
  static Global_data<Freemap_storage> _freemap_storage;

  /// \return The memory handed to `alloc`.
  template<class A>
  static void *
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
    return initial_mem;
  }

public:
  // MULTIPLEXING
  /**
   * Check whether a fault at `address` hit a region that is part of the
   * active snapshot but currently not in a physical MPU slot. If so, swap it
   * in (random replacement) and return true.
   */
  static bool check_and_handle_multiplex_fault(Mword address);

  /**
   * Get the physical slots of the ku_mem regions of `regions`, i.e. its
   * disabled user regions.
   *
   * \param regions  Region set whose ku_mem regions are requested.
   * \param logical  Logical slots of the ku_mem regions. Returned unchanged
   *                 if `regions` is not loaded, e.g. before MPUltiplex is
   *                 ready (logical == physical slot).
   */
  static Unsigned32 ku_mem_mask(Mpu_regions const &regions, Unsigned32 logical);

  /**
   * Compute the physical placement of `regions` for `Mpu::update`.
   *
   * \return Snapshot providing `hw_slot()`, `hw_count()` and `hw_keep_mask()`.
   */
  static Snapshot const &prepare_update(Mpu_regions const &regions);

  /**
   * Drop all snapshots of `regions` on this CPU. Must be called before an
   * `Mpu_regions` object is destroyed. Other CPUs must not hold snapshots of
   * it anymore, see `Mem_space::prepare_destroy()`.
   */
  static void forget(Mpu_regions const *regions);

  /**
   * Drop the cached (i.e. not current) snapshot of `regions` on this CPU, if
   * any. See `Mem_space::sync_mpu_state()`.
   */
  static void drop_cached(Mpu_regions const &regions);

  static Mpu_region_delta *alloc_delta(Mpu_region_base const &src);
  static void free_delta(Mpu_region_delta *d);

private:
  // SNAPSHOT MANAGEMENT
  /**
   * Global state of the MPUltiplex subsystem.
   */
  struct State
  {
    bool ready = false;  ///< Delta allocator usable
  };
  static Global_data<State> _state;

  class Snapshot_buffer;
  struct Cpu_state;
  static Per_cpu<Cpu_state> _cpu_state;

  /**
   * Current snapshot, if it describes the loaded MPU state. Before
   * MPUltiplex is ready, the non-multiplexed code paths own the MPU.
   */
  static Snapshot *active();

  /**
   * Make an up-to-date snapshot of `regions` the current one: reuse the
   * current or cached snapshot of `regions`, refreshed if needed, or create a
   * new one. The previously current snapshot is kept as the cached one.
   *
   * \param[out] reused  Optional. Whether the snapshot was reused unchanged.
   *                     Otherwise it was refreshed or created: entries may be
   *                     `Snapshot::changed()` and vanished regions may still
   *                     be placed.
   *
   * \return The current snapshot.
   */
  static Snapshot &switch_snapshot(Mpu_regions const &regions,
                                   bool *reused = nullptr);

  /// Find a physical slot for logical region `i`, evicting one if needed.
  static unsigned place(Snapshot &s, unsigned i);
};

/*
 * Defined outside of `Mpu` because they need the complete `Snapshot`.
 */

/**
 * Entry storage of one per-CPU snapshot slot.
 *
 * The first block lives inline (like `Mpu_regions::_static_regions`),
 * larger capacities come from `Snapshot_allocator`. The capacity only
 * grows, so switching between region sets does not allocate once the
 * slot is large enough. `reserve()` is only called while the slot holds no
 * snapshot, hence entries never need to be moved.
 */
class Mpu::Snapshot_buffer
{
public:
  Snapshot_buffer() = default;
  Snapshot_buffer(Snapshot_buffer const &) = delete;
  Snapshot_buffer &operator=(Snapshot_buffer const &) = delete;

  Snapshot::Entry *entries() { return _entries; }
  unsigned capacity() const { return _capacity; }

  /**
   * Make room for `n` entries.
   *
   * \pre The owning slot holds no snapshot.
   */
  void reserve(unsigned n)
  {
    if (n <= _capacity)
      return;

    // Free before allocating, see `Snapshot_pool_size`.
    if (_entries != inline_entries())
      Snapshot_allocator::free(_entries,
                               sizeof(Snapshot::Entry) * _capacity);
    _entries = inline_entries();
    _capacity = Config::Mpultiplex_block_size;

    void *mem = Snapshot_allocator::alloc(sizeof(Snapshot::Entry) * n);
    if (!mem)
      panic("MPUltiplex: out of memory for %u snapshot entries\n", n);
    _entries = static_cast<Snapshot::Entry *>(mem);
    _capacity = n;
  }

private:
  Snapshot::Entry *inline_entries()
  { return reinterpret_cast<Snapshot::Entry *>(_inline); }

  alignas(Snapshot::Entry) char _inline[sizeof(Snapshot::Entry_block)];
  Snapshot::Entry *_entries = inline_entries();
  unsigned _capacity = Config::Mpultiplex_block_size;
};

/**
 * Per-CPU state: the snapshot describing this CPU's MPU and the snapshot
 * of the previously loaded region set, kept for switching back.
 *
 * A cached snapshot keeps its references, so its region set must not be
 * destroyed before the snapshot is dropped. A CPU only caches a snapshot of
 * a `Mem_space` that was current on it, i.e. the CPU is in the Mem_space's
 * TLB-active mask, and keeps the bit until the snapshot is dropped (see
 * `Mem_space::sync_mpu_state()`).
 *
 * Only accessed by the owning CPU with the CPU lock held.
 */
struct Mpu::Cpu_state
{
  struct Slot
  {
    alignas(Snapshot) char obj[sizeof(Snapshot)] = {};
    Snapshot_buffer buffer;  ///< Entries of the snapshot in `obj`

    Snapshot *snapshot() { return reinterpret_cast<Snapshot *>(obj); }
  };

  Slot slots[2];
  Snapshot *current = nullptr;  ///< Lives in one of `slots`
  Snapshot *cached = nullptr;   ///< Lives in the other slot

  Slot &slot_of(Snapshot const *s)
  { return slots[s == slots[1].snapshot()]; }

  /// Snapshot of `regions` (current or cached), or nullptr.
  Snapshot *find(Mpu_regions const &regions) const
  {
    if (current && current->regions() == &regions)
      return current;
    if (cached && cached->regions() == &regions)
      return cached;
    return nullptr;
  }

  /// Destroy snapshot `s`, which is `current` or `cached`.
  void destroy(Snapshot *s)
  {
    s->~Snapshot();
    if (s == current)
      current = nullptr;
    else
      cached = nullptr;
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
    Mpu::forget(this);
    for (unsigned i = 0; i < size(); ++i)
      {
        region(i).compact();
        assert(!region(i).has_deltas());
      }

    for (unsigned b = 1; b < nr_blocks(); ++b)
      Mpu::Block_allocator::free(_blocks[b], sizeof(Mpu_region_block));
  }

  /// Whether region `i` has versions newer than its base.
  bool has_deltas(unsigned i) const { return region(i).has_deltas(); }

  /// Changes whenever a region is modified, added or removed, or the region
  /// set is grown.
  unsigned generation() const { return _generation; }

  /**
   * Do not keep snapshots of this region set cached anymore once they stop
   * being current, see `Mem_space::prepare_destroy()`.
   */
  void disable_snapshot_cache() { write_now(&_no_snapshot_cache, true); }
  bool snapshot_cache_disabled() const
  { return access_once(&_no_snapshot_cache); }

private:
  static constexpr unsigned regions_per_block()
  { return Config::Mpultiplex_block_size; }

  static_assert(Mpu::Max_region_blocks * Config::Mpultiplex_block_size
                <= 1U << 16, "Mpu_region::_index too small");

  unsigned nr_blocks() const { return _size / regions_per_block(); }

  /**
   * Region `i`.
   *
   * Non-const even for a const region set: snapshots take references on
   * region versions (see `Snapshot::capture()`).
   */
  Mpu_region &region(unsigned i) const
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
   * snapshots on any CPU may keep referencing their base versions and the
   * AVL tree stays intact.
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

    // Snapshots have to be refreshed to cover the new regions.
    bump_generation();
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

  void bump_generation() { ++_generation; }

private:
  Mpu_region_block _static_regions;
  /// Region blocks, `_blocks[0]` is `_static_regions`.
  Mpu_region *_blocks[Mpu::Max_region_blocks];
  unsigned _generation = 0;
  bool _no_snapshot_cache = false;
};

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex]:

#include <cstdio>
#include <cstdlib>

#include "context.h"
#include "kmem_alloc.h"
#include "kmem_slab.h"
#include "lock_guard.h"
#include "ram_quota.h"
#include "spin_lock.h"

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

/**
 * Number of size classes of the snapshot allocator. Its largest
 * allocation must hold the entries of `Mpu::Max_region_blocks` blocks.
 */
static constexpr int Snapshot_size_classes = 4;

using Buddy_snapshot_alloc
  = Buddy_t_base<2 * sizeof(Snapshot::Entry_block), Snapshot_size_classes>;

static_assert(Buddy_snapshot_alloc::Max_size / sizeof(Snapshot::Entry_block)
              >= Mpu::Max_region_blocks,
              "Snapshot entries must cover the largest Mpu_regions");

static constexpr size_t Snapshot_max = Buddy_snapshot_alloc::Max_size;

/**
 * Size of the snapshot pool.
 *
 * There are at most two snapshot slots per CPU (see `Mpu::Cpu_state`), each
 * owning at most one buffer of at most `Max_size`. A slot frees its old
 * buffer before allocating a larger one, so at most `2 * Max_num_cpus - 1`
 * buffers are live during an allocation. A buddy block never crosses a
 * `Max_size` boundary, hence with `2 * Max_num_cpus` whole `Max_size`
 * blocks one of them is completely free and the allocation cannot fail.
 * If `Max_size` is not a power of two, the pool may not be aligned to it
 * and one `Max_size` block of slack is added.
 */
static constexpr size_t Snapshot_pool_size
  = 2 * Config::Max_num_cpus * Snapshot_max
    + ((Snapshot_max & (Snapshot_max - 1)) ? Snapshot_max : 0);

/// Initial sizes of the region block and bitmap allocators.
static constexpr size_t Block_pool_size = Mpu::Buddy_block_alloc::Min_size * 64;
static constexpr size_t Bitmap_pool_size
  = Mpu::Buddy_bitmap_alloc::Max_size * 64;

static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Mpu::Buddy_block_alloc> _block_alloc;
static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Mpu::Buddy_bitmap_alloc> _bitmap_alloc;
static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Buddy_snapshot_alloc> _snapshot_alloc;
static DEFINE_GLOBAL_CONSTINIT
  Global_data<Spin_lock<>> _snapshot_alloc_lock;
static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Kmem_slab_t<Mpu_region_delta>> _delta_allocator("Delta");

DEFINE_GLOBAL Global_data<Mpu::Freemap_storage> Mpu::_freemap_storage;
// Must not be dynamically initialized: Mpu::init_alloc() runs from
// Startup::stage2(), i.e. before default-priority constructors.
DEFINE_GLOBAL_CONSTINIT Global_data<Mpu::State> Mpu::_state;
// The boot CPU's instance is (re-)constructed late during static
// initialization. That is fine as long as the first snapshot is taken from
// kernel_main() at the earliest.
DEFINE_PER_CPU Per_cpu<Mpu::Cpu_state> Mpu::_cpu_state;

Mpu_region_base const Snapshot::_empty;

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

IMPLEMENT static
void *
Mpu::Snapshot_allocator::alloc(unsigned long size)
{
  auto guard = lock_guard(&_snapshot_alloc_lock);
  return _snapshot_alloc->alloc(size);
}

IMPLEMENT static
void
Mpu::Snapshot_allocator::free(void *block, unsigned long size)
{
  auto guard = lock_guard(&_snapshot_alloc_lock);
  _snapshot_alloc->free(block, size);
}

/**
 * Check that the snapshot pool at `pool` consists of enough whole `Max_size`
 * blocks, see `Snapshot_pool_size`.
 */
static void
check_snapshot_pool(void *pool)
{
  constexpr size_t max = Buddy_snapshot_alloc::Max_size;
  Address start = reinterpret_cast<Address>(pool);
  Address end = start + Snapshot_pool_size;
  // Buddy blocks are aligned to multiples of Max_size (see calc_base_addr()).
  Address first = start + (max - start % max) % max;
  size_t blocks = end > first ? (end - first) / max : 0;
  if (blocks < 2 * Config::Max_num_cpus)
    panic("MPUltiplex: snapshot pool holds only %zu blocks, need %u\n",
          blocks, 2 * Config::Max_num_cpus);
}

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
  check_snapshot_pool(setup_allocator(&_snapshot_alloc,
                                      Snapshot_pool_size,
                                      &fm_index));

  dump_alloc(fm_index);

  printf("MPU: hardware provides %lu regions, using %u\n",
         Mpu_arm::regions(), Mpu::regions());

  // Multiplexing starts with the first Mpu::update(), see prepare_update().
  // Per-CPU data must not be accessed here, there is no thread context yet.
}

// --- Region versions --------------------------------------------------------

// TODO: Account deltas to the quota of the owning Mem_space.
IMPLEMENT static
Mpu_region_delta *
Mpu::alloc_delta(Mpu_region_base const &src)
{
  Mpu_region_delta *d = _delta_allocator->q_new(Ram_quota::root.unwrap(), src);
  if (!d)
    panic("MPUltiplex: out of memory for region delta\n");
  return d;
}

IMPLEMENT static
void
Mpu::free_delta(Mpu_region_delta *d)
{ _delta_allocator->q_del(Ram_quota::root.unwrap(), d); }

/**
 * Return the newest version for modification. If it is referenced by a
 * snapshot, a copy is appended as new newest version first.
 */
IMPLEMENT
Mpu_region_version &
Mpu_region::writable()
{
  Mpu_region_version &cur = *current_ptr();
  if (!cur.referenced())
    return cur;

  Mpu_region_delta *d = Mpu::alloc_delta(cur);
  _deltas.push_back(d);
  return *d;
}

/**
 * Merge versions that are no longer referenced (reference count is back at
 * the owner's reference).
 *
 * An unreferenced delta that is not the newest one is invisible to everyone
 * and is dropped. If the newest delta and its predecessor are both
 * unreferenced, the newest delta is merged into its predecessor. Repeating
 * this collapses a region to its base once no snapshot references it.
 */
IMPLEMENT
void
Mpu_region::compact()
{
  // Drop unreferenced intermediate deltas.
  for (auto it = _deltas.begin(); it != _deltas.end() && *it != newest();)
    {
      Mpu_region_delta *d = *it;
      if (d->referenced())
        ++it;
      else
        {
          it = _deltas.erase(it);
          Mpu::free_delta(d);
        }
    }

  // Merge the newest delta into its predecessor.
  while (has_deltas() && !newest()->referenced())
    {
      Mpu_region_delta *d = newest();
      Mpu_region_version *pred = d == *_deltas.begin()
        ? base_ptr()
        : *--_deltas.iter(d);
      if (pred->referenced())
        break;

      static_cast<Mpu_region_base &>(*pred) = static_cast<Mpu_region_base &>(*d);
      _deltas.remove(d);
      Mpu::free_delta(d);
    }
}

// --- Snapshot ---------------------------------------------------------------

IMPLEMENT static inline
Mpu_region_version *
Snapshot::capture(Mpu_regions const &regions, unsigned i)
{
  // Reserved regions are captured even if not used: in a user region set
  // they hold the kernel regions (copied from the kernel's region set).
  if (!regions._used_mask[i] && !regions._reserved[i])
    return nullptr;
  return regions.region(i).current_ptr();
}

IMPLEMENT static inline
unsigned char
Snapshot::flags_of(Mpu_regions const &regions, unsigned i,
                   Mpu_region_version const *v)
{
  if (!v)
    return 0;

  unsigned char flags = Entry::Used;
  if (regions._reserved[i])
    flags |= Entry::Reserved;
  // Reserved regions belong to the kernel; disabled regions are ku_mem
  // which is enabled by the kernel entry code via its physical slot.
  if (regions._reserved[i] || !v->attr().enabled())
    flags |= Entry::Pinned;
  return flags;
}

/**
 * Capture the newest version of all used and reserved regions of `regions`.
 *
 * \param prev     Snapshot this one replaces, or nullptr. If it is of the
 *                 same `Mpu_regions` object, its physical placement is
 *                 carried over and its references to unchanged versions are
 *                 taken over, so it must be destroyed afterwards. Entries
 *                 with a different version are marked `changed()`.
 *                 Otherwise, identity placement is assumed.
 * \param entries  Storage for `regions.size()` entries. It must stay valid
 *                 and unused by anybody else for the snapshot's lifetime.
 */
IMPLEMENT
Snapshot::Snapshot(Mpu_regions const &regions, Snapshot *prev,
                   Entry *entries)
: _regions(&regions), _entries(entries), _size(regions.size()),
  _hw(Mpu::regions()), _generation(regions._generation)
{
  if (prev && prev->_regions != &regions)
    prev = nullptr;

  for (unsigned i = 0; i < _size; ++i)
    {
      Entry &e = *new (&_entries[i]) Entry();
      Mpu_region_version *v = capture(regions, i);
      Mpu_region_version_ptr *old = prev && i < prev->_size
                                    ? &prev->_entries[i].version : nullptr;
      if (old && old->get() == v)
        e.version = cxx::move(*old); // unchanged: take over the reference
      else
        {
          e.version = Mpu_region_version_ptr(v);
          if (old || (prev && v))
            e.flags = Entry::Changed;
        }

      e.flags |= flags_of(regions, i, v);
      if (v)
        _tree.insert(&e);
    }

  _phys_used.clear_all();
  _phys_pinned.clear_all();
  _phys_ku_mem.clear_all();
  _phys_keep.clear_all();
  for (unsigned p = 0; p < Mem_layout::Mpu_regions; ++p)
    _logical[p] = -1;

  if (prev && prev->_regions == &regions)
    {
      // Carry over the placement. This includes regions that vanished in the
      // meantime: they still occupy their physical slot until
      // `Mpu::sync()` or `Mpu::prepare_update()` evict them.
      for (unsigned p = 0; p < Mem_layout::Mpu_regions; ++p)
        if (int l = prev->_logical[p]; l >= 0)
          {
            // Region sets never shrink.
            assert(static_cast<unsigned>(l) < _size);
            activate(l, p);
          }
    }
  else
    {
      for (unsigned i = 0; i < _size && i < _hw; ++i)
        if (used(i))
          activate(i, i);
    }
}

IMPLEMENT
Snapshot::~Snapshot()
{
  for (unsigned i = 0; i < _size; ++i)
    _entries[i].~Entry();
}

/**
 * Bring the snapshot up to date with its region set, in place.
 *
 * Entries whose version is unchanged keep their reference. All other entries
 * are captured again and marked `changed()`. The placement is kept, also for
 * regions that vanished: the caller has to evict them.
 */
IMPLEMENT
void
Snapshot::refresh(Mpu_regions const &regions)
{
  assert(&regions == _regions);

  // Region sets never shrink, but may have grown.
  for (unsigned i = _size; i < regions.size(); ++i)
    new (&_entries[i]) Entry();
  _size = regions.size();

  // Changed entries leave the tree first and are inserted again only
  // afterwards, because a changed entry may take over the start address of
  // another one.
  for (unsigned i = 0; i < _size; ++i)
    {
      Entry &e = _entries[i];
      Mpu_region_version *v = capture(regions, i);
      if (v == e.version.get())
        {
          e.flags &= ~Entry::Changed;
          continue;
        }

      // Entries with the start address of another one never made it into
      // the tree, see the constructor.
      if (e.version && _tree.find_node(Entry::key_of(&e)) == &e)
        _tree.remove(Entry::key_of(&e));
      e.version = Mpu_region_version_ptr(v);
      e.flags = flags_of(regions, i, v) | Entry::Changed;
    }

  for (unsigned i = 0; i < _size; ++i)
    if (changed(i))
      {
        if (_entries[i].version)
          _tree.insert(&_entries[i]);

        // Keep the placement, but update the physical slot masks.
        if (active(i))
          {
            unsigned p = _entries[i].phys;
            deactivate(i);
            activate(i, p);
          }
      }

  _generation = regions._generation;
}

IMPLEMENT
int
Snapshot::lookup(Mword addr) const
{
  Entry const *e = _tree.last_less_equal_node(addr);
  if (e && e->version->contains(addr))
    return e - _entries;
  return -1;
}

IMPLEMENT
int
Snapshot::free_phys() const
{
  // Slots from `_hw` on are never used: if all hardware slots are taken, the
  // first cleared bit lies beyond `_hw` or there is none at all.
  size_t p = (~_phys_used).ffs(0);
  return p && p <= _hw ? static_cast<int>(p - 1) : -1;
}

/**
 * Pick a random active, non-pinned logical region.
 *
 * \return Logical slot or -1 if all active regions are pinned.
 */
IMPLEMENT
int
Snapshot::pick_victim() const
{
  Phys_mask cand = _phys_used & ~_phys_pinned;
  unsigned n = cand.popcount();
  if (!n)
    return -1;

  size_t p = cand.ffs(0);
  for (unsigned k = rand() % n; k; --k)
    p = cand.ffs(p);
  return _logical[p - 1];
}

IMPLEMENT
void
Snapshot::activate(unsigned i, unsigned p)
{
  assert(!active(i));
  assert(!_phys_used[p]);
  _entries[i].phys = p;
  _logical[p] = i;
  _phys_used.set_bit(p);
  if (pinned(i))
    _phys_pinned.set_bit(p);
  // Pinned but not reserved: disabled user region, i.e. ku_mem.
  if (pinned(i) && !reserved(i))
    _phys_ku_mem.set_bit(p);
  // Reserved (kernel) regions in their identity slot are left untouched by
  // Mpu::update().
  if (reserved(i) && i == p)
    _phys_keep.set_bit(p);
}

IMPLEMENT
void
Snapshot::deactivate(unsigned i)
{
  assert(active(i));
  unsigned p = _entries[i].phys;
  _entries[i].phys = -1;
  _logical[p] = -1;
  _phys_used.clear_bit(p);
  _phys_pinned.clear_bit(p);
  _phys_ku_mem.clear_bit(p);
  _phys_keep.clear_bit(p);
}

// --- Multiplexing -----------------------------------------------------------

IMPLEMENT static
Snapshot &
Mpu::switch_snapshot(Mpu_regions const &regions, bool *reused)
{
  // Only called once MPUltiplex is ready (prepare_update(), sync()), so
  // snapshots always hold references.
  assert(_state->ready);
  Cpu_state &cs = _cpu_state.current();
  Snapshot *s = cs.find(regions);
  bool const up_to_date = s && s->generation() == regions.generation();

  if (!up_to_date)
    {
      if (s && cs.slot_of(s).buffer.capacity() >= regions.size())
        s->refresh(regions);
      else
        {
          // Create a new snapshot in the slot not holding `keep`: the outdated
          // snapshot of `regions`, whose placement and references are taken
          // over, or else the current (or cached) snapshot of another region
          // set. Whatever lives in the target slot is dropped, so its buffer
          // may be reallocated.
          Snapshot *keep = s ? s : (cs.current ? cs.current : cs.cached);
          Cpu_state::Slot &slot = cs.slots[keep == cs.slots[0].snapshot()];
          Snapshot *victim = slot.snapshot();
          if (victim == cs.current || victim == cs.cached)
            cs.destroy(victim);

          slot.buffer.reserve(regions.size());
          Snapshot *n = new (slot.obj) Snapshot(regions, s,
                                                slot.buffer.entries());
          if (s)
            cs.destroy(s);
          s = n;
        }
    }

  if (s != cs.current)
    {
      // The previously current snapshot (of another region set) is kept
      // cached for switching back, unless its region set is going away.
      Snapshot *old = cs.current;
      if (s == cs.cached)
        cs.cached = nullptr;
      if (old && old->regions()->snapshot_cache_disabled())
        cs.destroy(old);
      else if (old)
        {
          assert(!cs.cached);
          cs.cached = old;
        }
      cs.current = s;
    }

  if (reused)
    *reused = up_to_date;
  return *s;
}

IMPLEMENT static inline
Snapshot *
Mpu::active()
{
  return _state->ready ? _cpu_state.current().current : nullptr;
}

/**
 * Find a physical slot for the (inactive) logical region `i` and mark it
 * active. Reserved regions prefer their identity slot because the kernel
 * entry code relies on fixed slots for some of them.
 */
IMPLEMENT static
unsigned
Mpu::place(Snapshot &s, unsigned i)
{
  int p = -1;
  if (s.reserved(i) && i < s.hw_count())
    {
      p = i;
      // evict whatever lives in the identity slot
      if (int l = s.logical_slot(p); l >= 0)
        {
          if (s.pinned(l))
            panic("MPUltiplex: identity slot %d of L%u taken by pinned "
                  "L%d\n", p, i, l);
          s.deactivate(l);
        }
    }

  if (p < 0)
    p = s.free_phys();

  if (p < 0)
    {
      int victim = s.pick_victim();
      if (victim < 0)
        panic("MPUltiplex: all %u hardware regions pinned\n", s.hw_count());
      p = s.phys_slot(victim);
      s.deactivate(victim);
    }

  s.activate(i, p);
  return p;
}

IMPLEMENT static
Snapshot const &
Mpu::prepare_update(Mpu_regions const &regions)
{
  // Mpu::update() is only called in thread context (the first time when the
  // boot CPU's kernel thread makes the kernel space current), so per-CPU data
  // is usable from now on: start multiplexing. From now on snapshots hold
  // references and deltas can be allocated (Mpu::init_alloc() ran before).
  _state->ready = true;

  bool reused;
  Snapshot &s = switch_snapshot(regions, &reused);
  // A reused snapshot was completely placed when it was current last time,
  // and sync() places new regions right away.
  if (reused)
    return s;

  // Drop placement of regions that vanished. Mpu::update() writes all
  // physical slots, so the changed flags are not needed.
  for (unsigned l = 0; l < s.size(); ++l)
    {
      s.clear_changed(l);
      if (s.active(l) && !s.used(l))
        s.deactivate(l);
    }

  // Pinned regions first, they may evict others.
  for (unsigned l = 0; l < s.size(); ++l)
    if (s.used(l) && s.pinned(l) && !s.active(l))
      place(s, l);

  // Fill remaining free slots, but do not evict.
  for (unsigned l = 0; l < s.size(); ++l)
    if (s.used(l) && !s.active(l))
      {
        int p = s.free_phys();
        if (p < 0)
          break;
        s.activate(l, p);
      }

  return s;
}

IMPLEMENT static
bool
Mpu::check_and_handle_multiplex_fault(Mword address)
{
  auto guard = lock_guard(cpu_lock);
  Snapshot *a = active();
  if (!a)
    return false;

  Snapshot &s = *a;
  int l = s.lookup(address);
  if (l < 0 || s.active(l))
    return false; // not a multiplexing fault

  direct_write(place(s, l), s.version(l), false);
  hw_done();
  return true;
}

IMPLEMENT static inline NEEDS[Mpu::active]
Unsigned32
Mpu::ku_mem_mask(Mpu_regions const &regions, Unsigned32 logical)
{
  Snapshot const *s = active();
  if (!s || s->regions() != &regions)
    return logical;
  return s->ku_mem_mask();
}

IMPLEMENT static
void
Mpu::forget(Mpu_regions const *regions)
{
  if (!_state->ready)
    return; // no snapshots yet
  Cpu_state &cs = _cpu_state.current();
  if (cs.current && cs.current->regions() == regions)
    cs.destroy(cs.current);
  if (cs.cached && cs.cached->regions() == regions)
    cs.destroy(cs.cached);
}

IMPLEMENT static
void
Mpu::drop_cached(Mpu_regions const &regions)
{
  if (!_state->ready)
    return; // no snapshots yet
  Cpu_state &cs = _cpu_state.current();
  if (cs.cached && cs.cached->regions() == &regions)
    cs.destroy(cs.cached);
}

IMPLEMENT static
void
Mpu::sync(Mpu_regions const &regions, Mpu_regions_mask const &touched,
          bool inplace)
{
  // Not multiplexed yet: logical == physical slot.
  if (!_state->ready)
    return sync_early(regions, touched, inplace);

  if (Snapshot const *a = active(); a && a->regions() != &regions)
    WARN("MPUltiplex: sync of %p while %p is loaded\n",
         &regions, a->regions());

  Snapshot &s = switch_snapshot(regions);

  // Write back what changed since the snapshot was last up to date. That is
  // `touched`, unless `regions` was not loaded or another CPU changed it.
  for (unsigned l = 0; l < s.size(); ++l)
    {
      bool const t = l < touched.size() && touched[l];
      if (!t && !s.changed(l))
        continue;
      s.clear_changed(l);

      if (!s.used(l))
        {
          // region was removed
          if (s.active(l))
            {
              hw_disable(s.phys_slot(l));
              s.deactivate(l);
            }
          continue;
        }

      if (s.active(l))
        direct_write(s.phys_slot(l), s.version(l), inplace && t);
      else
        direct_write(place(s, l), s.version(l), false);
    }

  hw_done();
}

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex && !debug]:

PRIVATE static inline
void
Mpu::dump_alloc(unsigned)
{}

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex && debug]:

IMPLEMENT static
void
Mpu::Snapshot_allocator::dump()
{
  auto guard = lock_guard(&_snapshot_alloc_lock);
  _snapshot_alloc->dump();
}

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
  _snapshot_alloc->dump();
}

PUBLIC
void
Snapshot::dump() const
{
  printf("MPUltiplex snapshot of %p: %u logical, %u hw, phys_used=%lx "
         "phys_pinned=%lx\n",
         _regions, _size, _hw, *_phys_used.raw(), *_phys_pinned.raw());
  for (unsigned i = 0; i < _size; ++i)
    if (used(i))
      printf("  L%-2u -> P%-2d [" L4_MWORD_FMT ".." L4_MWORD_FMT "]%s%s\n",
             i, phys_slot(i), version(i).start(), version(i).end(),
             pinned(i) ? " pinned" : "",
             &static_cast<Mpu_region_base const &>(version(i))
               != &(*_regions)[i] ? " (old)" : "");
}

/**
 * Dump the active snapshot.
 */
PUBLIC static
void
Mpu::dump_snapshot()
{
  if (Snapshot const *s = active())
    s->dump();
  else
    printf("MPUltiplex: no active snapshot\n");
}