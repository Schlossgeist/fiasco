INTERFACE [mpu]:

#include <cxx/avl_tree>

#include "l4_fpage.h"
#include "l4_msg_item.h"
#include "mem_layout.h"
#include "warn.h"

class Mpu_regions;
class Mpu_regions_mask;

/**
 * Generic, implementation agnostic MPU region attributes.
 */
class Mpu_region_attr
{
  L4_fpage::Rights _rights;
  L4_snd_item::Memory_type _type;
  bool _enabled;

  constexpr Mpu_region_attr(L4_fpage::Rights rights,
                            L4_snd_item::Memory_type type,
                            bool enabled)
  : _rights(rights), _type(type), _enabled(enabled)
  {}

public:
  Mpu_region_attr() = default;

  friend constexpr bool operator == (Mpu_region_attr const &lhs,
                                     Mpu_region_attr const &rhs) = default;

  static constexpr Mpu_region_attr
  make_attr(L4_fpage::Rights rights,
            L4_snd_item::Memory_type type = L4_snd_item::Memory_type::Normal(),
            bool enabled = true)
  {
    return Mpu_region_attr(rights, type, enabled);
  }

  constexpr L4_fpage::Rights rights() const { return _rights; }
  constexpr L4_snd_item::Memory_type type() const { return _type; }
  constexpr bool enabled() const { return _enabled; }

  inline void add_rights(L4_fpage::Rights rights)
  {
    _rights |= rights;
  }

  inline void del_rights(L4_fpage::Rights rights)
  {
    _rights &= ~rights;
  }
};

struct Mpu_region_base
{
  constexpr Mpu_region_base();
  Mpu_region_base(Mword start, Mword end, Mpu_region_attr a);

  constexpr Mword start() const;
  constexpr Mword end() const;
  constexpr Mpu_region_attr attr() const;

  inline void start(Mword start);
  inline void end(Mword end);
  inline void attr(Mpu_region_attr attr);
  inline void disable();

  friend bool operator < (Mpu_region_base const &lhs, Mpu_region_base const &rhs)
  { return lhs.end() < rhs.start(); }

  constexpr bool contains(Mword addr) const
  { return start() <= addr && addr <= end(); }
};

//---------------------------------------------------------------------------
INTERFACE [mpu && !mpultiplex]:

/**
 * A single MPU region.
 *
 * Regions are organized as nodes in an AVL tree, keyed by start address. The
 * architecture extends its base struct with the actual data for the hardware.
 * All addresses are inclusive!
 */
struct Mpu_region
: public Mpu_region_base,
  public cxx::Avl_tree_node
{
  using Mpu_region_base::Mpu_region_base;

  using Key_type = Mword;
  static Key_type key_of(Mpu_region const *r)
  { return r->start(); }

  Mpu_region_base const &current() const { return *this; }

  /// Nothing to merge without multiplexing.
  void compact() {}
};

//---------------------------------------------------------------------------
INTERFACE [mpu && mpultiplex]:

#include <cxx/dlist>

#include "bitmap.h"
#include "panic.h"
#include "ref_obj.h"

/**
 * One version of the contents of an MPU region.
 *
 * Snapshots reference versions via `Ref_ptr`. A version that is referenced by
 * at least one snapshot is never modified.
 *
 * Every version holds one reference on itself on behalf of its owning
 * `Mpu_region` from its creation on. Hence, the reference count never drops
 * to zero through a `Ref_ptr`, and a count of one means that no snapshot
 * references the version. Deltas are freed explicitly by their region.
 */
struct Mpu_region_version : public Mpu_region_base, public Ref_cnt_obj
{
  Mpu_region_version()
  { owner_ref(); }

  Mpu_region_version(Mword start, Mword end, Mpu_region_attr a)
  : Mpu_region_base(start, end, a)
  { owner_ref(); }

  explicit Mpu_region_version(Mpu_region_base const &b)
  : Mpu_region_base(b)
  { owner_ref(); }

  Mpu_region_version(Mpu_region_version const &) = delete;

  /// Whether any snapshot references this version.
  bool referenced() const { return ref_cnt() > 1; }

  /// Never called: the owner reference keeps the count above zero.
  void operator delete(void *)
  { panic("Mpu_region_version: unexpected delete via Ref_ptr\n"); }

private:
  void owner_ref() { inc_ref(); }
};

using Mpu_region_version_ptr = Ref_ptr<Mpu_region_version>;

/**
 * A newer version of an MPU region.
 *
 * Deltas form a list per region, ordered from oldest to newest. The base
 * version lives in the `Mpu_region` object itself and is always the oldest
 * one.
 */
struct Mpu_region_delta : public Mpu_region_version, public cxx::D_list_item
{
  explicit Mpu_region_delta(Mpu_region_base const &b) : Mpu_region_version(b) {}
};

/**
 * A single (logical) MPU region.
 *
 * Regions are organized as nodes in an AVL tree, keyed by start address. The
 * region itself stores the *oldest* version of its contents. Modifications
 * of a version that is referenced by a snapshot are appended as new deltas
 * (copy-on-write). All accessors operate on the newest version. All
 * addresses are inclusive!
 */
struct Mpu_region
: private Mpu_region_version,
  public cxx::Avl_tree_node
{
  Mpu_region() = default;
  Mpu_region(Mword start, Mword end, Mpu_region_attr a)
  : Mpu_region_version(start, end, a) {}

  Mpu_region(Mpu_region const &) = delete;
  Mpu_region &operator=(Mpu_region const &) = delete;

  /**
   * Relocate a region including all of its versions. The AVL tree node is
   * *not* taken over, the caller has to rebuild the tree.
   *
   * The new base version only carries the owner reference. Snapshot
   * references stay on the moved-from base: the local snapshot is redirected
   * by `Mpu::relocate()`, snapshots of other CPUs keep using the old copy
   * until they are replaced (see `Mpu_regions::retire()`).
   */
  Mpu_region(Mpu_region &&o)
  : Mpu_region_version(static_cast<Mpu_region_base const &>(o)),
    _deltas(cxx::move(o._deltas))
  {}

  using Key_type = Mword;
  static Key_type key_of(Mpu_region const *r)
  { return r->start(); }

  /// Newest version of this region.
  Mpu_region_version const &current() const
  { return has_deltas() ? *newest() : base(); }

  /// Oldest version of this region.
  Mpu_region_version const &base() const
  { return *static_cast<Mpu_region_version const *>(this); }

  Mword start() const { return current().start(); }
  Mword end() const { return current().end(); }
  Mpu_region_attr attr() const { return current().attr(); }
  bool contains(Mword addr) const { return current().contains(addr); }

  /// Newest version of this region, for taking a reference.
  Mpu_region_version *current_ptr()
  {
    if (_deltas.empty())
      return base_ptr();
    return newest();
  }

  /// Oldest version of this region, for taking a reference.
  Mpu_region_version *base_ptr()
  { return static_cast<Mpu_region_version *>(this); }

  bool has_deltas() const { return !_deltas.empty(); }

  /// Merge and free versions that are no longer referenced by any snapshot.
  void compact();

private:
  /// Modifications are reserved to `Mpu_regions`, see `Mpu_regions::modify`.
  friend class Mpu_regions;

  void start(Mword start) { writable().start(start); }
  void end(Mword end) { writable().end(end); }
  void attr(Mpu_region_attr attr) { writable().attr(attr); }
  void disable() { writable().disable(); }

  Mpu_region_version &writable();

  /// \pre `has_deltas()`
  Mpu_region_delta *newest() const { return *--_deltas.end(); }

  cxx::D_list<Mpu_region_delta> _deltas;
};

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

  void relocate(Mpu_region *old_base, Mpu_region *new_base, unsigned n);

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

using Mpu_region_block = Mpu_region[Config::Mpultiplex_block_size];

//---------------------------------------------------------------------------
INTERFACE [mpu]:

/**
 * Interface to the CPUs MPU.
 */
class Mpu
{
public:
  /**
   * Initialize MPU.
   *
   * Brings the MPU into a defined state. Called by platform code before any
   * regions are setup.
   */
  static void init();

  /**
   * Write back changes to hardware.
   *
   * \param regions  The Mpu_regions object that was updated.
   * \param touched  Impacted regions.
   * \param inplace  Update region directly instead of performing a safe
   *                 disable-update-enable sequence.
   */
  static void sync(Mpu_regions const &regions, Mpu_regions_mask const &touched,
                   bool inplace = false);

  /**
   * Update MPU with new regions list.
   *
   * Write back all `regions` into hardware.
   */
  static void update(Mpu_regions const &regions);

  /**
   * Get number of supported regions.
   *
   * \return Number of hardware regions, but at most Mem_layout::Mpu_regions.
   */
  static unsigned regions();
};

//---------------------------------------------------------------------------
INTERFACE [mpu && !mpultiplex]:

#include "bitmap.h"

using Bitmap_type = Bitmap<Mem_layout::Mpu_regions>;

//---------------------------------------------------------------------------
INTERFACE [mpu && mpultiplex]:

#include "dynamic_bitmap.h"

using Bitmap_type =
  Dynamic_bitmap<Config::Mpultiplex_block_size, Mpu::Dynamic_bitmap_allocator>;

//---------------------------------------------------------------------------
INTERFACE [mpu]:

/**
 * Bit mask of MPU regions.
 */
class Mpu_regions_mask : public Bitmap_type
{
public:
  // Required to make it compatible with Mpu_regions_update::Updates
  using Bitmap_type::operator=;
};

/**
 * MPU region update result.
 *
 * Updating MPU regions requires that the changed regions are written back to
 * hardware. Because a single update can affect multiple regions, this class
 * tracks the affected ones.
 *
 * Additionally, an "error" state is possible if an update failed.
 */
class Mpu_regions_update : private Mpu_regions_mask
{
  unsigned char _error_state = 0;

public:
  // only add up to 8 error cases
  enum Error {
    Error_no_err    = 0,
    // errors are sorted by priority, if there are multiple errors, only the
    // highest-priority one is returned by Mpu_regions_update::error()
    Error_no_mem    = 1 << 0, //< Out of regions.
    Error_collision = 1 << 1, //< New region collides with existing, incompatible one.
  };

  Mpu_regions_update(Mpu_regions_update const &) = default;
  Mpu_regions_update &operator=(Mpu_regions_update const &) = default;

  /**
   * Add a region to the update set.
   */
  inline void set_updated(unsigned region)
  { set_bit(region); }

  /**
   * Combine updates.
   *
   * Errors are sticky, that is, if any of the operands is in the error state,
   * the result will also be in an error state. This is just a safety measure.
   * The caller should check each individual update operation for errors.
   */
  Mpu_regions_update &operator|=(Mpu_regions_update const &other) &
  {
    _error_state |= other._error_state;
    this->Bitmap_type::operator|=(other);
    return *this;
  }

  /**
   * Bool operator testing if update succeeded.
   */
  explicit operator bool() const
  { return !static_cast<bool>(_error_state); }

  /**
   * Fetch update result.
   *
   * It is the responsibility of the caller to check for errors before.
   */
  Mpu_regions_mask value() const
  {
    Mpu_regions_mask ret;
    ret = *this;
    return ret;
  }

  /**
   * Returns error code in case update failed.
   */
  Error error() const
  {
    int lsb_set = __builtin_ffs(_error_state) - 1;
    return lsb_set < 0 ? Error_no_err : static_cast<Error>(1 << lsb_set);
  }
};

//---------------------------------------------------------------------------
INTERFACE [mpu && !mpultiplex]:

EXTENSION class Mpu_regions_mask
{
public:
  explicit Mpu_regions_mask()
  { clear_all(); }
};

EXTENSION class Mpu_regions_update
{
public:
  explicit Mpu_regions_update()
  : Mpu_regions_mask()
  {}

  explicit Mpu_regions_update(Error error)
  : Mpu_regions_mask()
  { _error_state |= error; }
};


//---------------------------------------------------------------------------
INTERFACE [mpu && mpultiplex]:

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

//---------------------------------------------------------------------------
INTERFACE [mpu]:

/**
 * List of MPU regions.
 *
 * The size is determined by Mem_layout::Mpu_regions. Any updates of the list
 * need to be written back explicitly to hardware, either by Mpu::sync() or
 * Mpu::update().
 *
 * Usually, regions need to be aligned to the granularity of the MPU hardware.
 * This class does *not* perform checks to verify this invariant. This is the
 * responsibility of the caller.
 */
class Mpu_regions
{
  using Region_tree = cxx::Avl_tree<Mpu_region, Mpu_region>;
  friend class Snapshot;

public:
  /// Newest version of region `i`.
  Mpu_region_base const &operator[](unsigned i) const &
  { return _regions[i].current(); }
  Mpu_region_base const &operator[](unsigned i) const && = delete;

  Mpu_regions_mask used()     const { return _used_mask; }
  Mpu_regions_mask reserved() const { return _reserved; }
  unsigned         size()     const { return _size; }

private:
  Mpu_region &operator[](unsigned i) &
  { return _regions[i]; }
  Mpu_region &operator[](unsigned i) && = delete;

  unsigned index(Mpu_region const *r) const
  { return r - _regions; }

  Mpu_region *deref_iter(Region_tree::Iterator iter) const
  { return iter != _used_tree.end() ? iter.operator->() : nullptr; }

  Mpu_region *front() const
  { return deref_iter(_used_tree.begin()); }

  Mpu_region *next(Mpu_region *r) const
  {
    Region_tree::Iterator iter = _used_tree.iter(r);
    return deref_iter(++iter);
  }

  Mpu_region *prev(Mpu_region *r) const
  {
    Region_tree::Iterator iter = _used_tree.iter(r);
    return iter != _used_tree.begin() ? (--iter).operator->() : nullptr;
  }

  Mpu_region *erase(Mpu_region *r)
  {
    bump_generation();
    _used_mask.clear_bit(index(r));
    r->compact();
    r->disable();
    return _used_tree.erase(r->start());
  }

  bool insert(Mpu_region *r)
  {
    bump_generation();
    _used_mask.set_bit(index(r));
    auto [node, was_not_in_tree_before] = _used_tree.insert(r);
    return was_not_in_tree_before;
  }

  /**
   * Route all modifications to `Mpu_region`s through these `modify`-functions,
   * so that we can give the following correctness guarantees:
   *   - updates to the key value (start) lead to reinsertion into the AVL tree
   *   - region versions no longer referenced by any snapshot are merged
   *     before the region is changed
   */
  enum Modification { Start, End, Full };
  template<Modification M>
  void modify(Mpu_region *r, Mword value)
  {
    static_assert(M == Start || M == End,
                  "Use modify<Full>(r, start, end, attr) for full assignment");
    bump_generation();
    r->compact();
    if constexpr (M == Start)
      {
        _used_tree.erase(r->start());
        r->start(value);
        auto [node, was_not_in_tree_before] = _used_tree.insert(r);
        assert(was_not_in_tree_before);
      }
    else
      {
        r->end(value);
      }
  }

  template<Modification M>
  void modify(Mpu_region *r, Mword start, Mword end, Mpu_region_attr attr)
  {
    static_assert(M == Full, "This overload is only for full assignment");
    bump_generation();
    r->compact();
    r->start(start);
    r->end(end);
    r->attr(attr);
  }

  unsigned _size;
  Mpu_regions_mask _reserved;
  Mpu_regions_mask _used_mask;  ///< Bit mask of occupied regions
  Region_tree _used_tree;       ///< Sorted tree (by address) of used regions
};

//---------------------------------------------------------------------------
INTERFACE [mpu && mpultiplex]:

#include "buddy_alloc.h"
#include "per_cpu_data.h"

EXTENSION class Mpu
{
public:
  /**
   * Initialize allocation functionality used by MPU code.
   */
  static void init_alloc();

  /**
   * Number of size classes of the block based allocators. Their largest
   * allocation is `1 << (Block_size_classes - 1)` times their smallest one,
   * which determines the maximum number of logical regions.
   */
  static constexpr int Block_size_classes = 4;

  using Buddy_block_alloc
    = Buddy_t_base<2 * sizeof(Mpu_region_block), Block_size_classes>;
  class Block_allocator : public Buddy_block_alloc
  {
  public:
    void *alloc(unsigned long size)
    { return Mpu::block_allocator()->alloc(size); }
    void free(void *block, unsigned long size)
    { Mpu::block_allocator()->free(block, size); }
    void dump() const;
  };

  using Buddy_bitmap_alloc
    = Buddy_t_base<max(2 * Config::Mpultiplex_block_size / 8, 24U), 2>;
  class Dynamic_bitmap_allocator : public Buddy_bitmap_alloc
  {
  public:
    void *alloc(unsigned long size)
    { return Mpu::bitmap_allocator()->alloc(size); }
    void free(void *block, unsigned long size)
    { Mpu::bitmap_allocator()->free(block, size); }
  };

  using Snapshot_block = Snapshot::Entry[Config::Mpultiplex_block_size];
  using Buddy_snapshot_alloc
    = Buddy_t_base<2 * sizeof(Snapshot_block), Block_size_classes>;

  static_assert(Buddy_snapshot_alloc::Max_size / sizeof(Snapshot_block)
                >= Buddy_block_alloc::Max_size / sizeof(Mpu_region_block),
                "Snapshot entries must cover the largest Mpu_regions");

  /**
   * Allocator for the snapshot entries of the per-CPU snapshot slots, see
   * `Snapshot_buffer`.
   *
   * Used by all CPUs, hence protected by a lock. Unlike `Block_allocator`, it
   * does not inherit from the buddy allocator, so it carries no unused copy
   * of the buddy's state.
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

  /**
   * `Initial_alloc_sizes::Snapshot_pool`: size of the snapshot pool.
   *
   * There are at most two snapshot slots per CPU (see `Cpu_state`), each
   * owning at most one buffer of at most `Max_size`. A slot frees its old
   * buffer before allocating a larger one, so at most `2 * Max_num_cpus - 1`
   * buffers are live during an allocation. A buddy block never crosses a
   * `Max_size` boundary, hence with `2 * Max_num_cpus` whole `Max_size`
   * blocks one of them is completely free and the allocation cannot fail.
   * If `Max_size` is not a power of two, the pool may not be aligned to it
   * and one `Max_size` block of slack is added.
   */
  static constexpr size_t Snapshot_max = Buddy_snapshot_alloc::Max_size;

  enum Initial_alloc_sizes : size_t {
    Block  = Buddy_block_alloc::Min_size * 32,
    Bitmap = Buddy_bitmap_alloc::Max_size * 64,
    Snapshot_pool = 2 * Config::Max_num_cpus * Snapshot_max
                    + ((Snapshot_max & (Snapshot_max - 1)) ? Snapshot_max : 0),
  };

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
   * Like `sync()`, but for code running without a thread context, i.e.
   * without access to per-CPU data (early boot of a CPU). Writes logical
   * region i into physical slot i.
   *
   * \param first  Region to write before all others, or -1. Newer region
   *               versions live on the kernel heap: a CPU that has not
   *               loaded the heap region yet must pass the heap region here,
   *               whose current version must not be a delta.
   */
  static void sync_early(Mpu_regions const &regions,
                         Mpu_regions_mask const &touched,
                         bool inplace = false, int first = -1);

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

  /// Redirect the snapshots of this CPU to relocated regions.
  static void relocate(Mpu_region *old_base, Mpu_region *new_base, unsigned n);

  static Mpu_region_delta *alloc_delta(Mpu_region_base const &src);
  static void free_delta(Mpu_region_delta *d);

private:
  // ARCH HOOKS
  /// Write `r` into physical slot `p`, disabling the slot first unless `inplace`.
  static void hw_write(unsigned p, Mpu_region_base const &r, bool inplace);
  /// Disable physical slot `p`.
  static void hw_disable(unsigned p);
  /// Finish a sequence of `hw_write()`/`hw_disable()` calls.
  static void hw_done();

  // SNAPSHOT MANAGEMENT
  /**
   * Global state of the MPUltiplex subsystem.
   */
  struct State
  {
    bool ready = false;  ///< Delta allocator usable
  };
  static Global_data<State> _state;

  /**
   * Entry storage of one per-CPU snapshot slot.
   *
   * The first block lives inline (like `Mpu_regions::_static_regions`),
   * larger capacities come from `Snapshot_allocator`. The capacity only
   * grows, so switching between region sets does not allocate once the
   * slot is large enough. `reserve()` is only called while the slot holds no
   * snapshot, hence entries never need to be moved.
   */
  class Snapshot_buffer
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

      // Free before allocating, see `Initial_alloc_sizes::Snapshot_pool`.
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

    alignas(Snapshot::Entry) char _inline[sizeof(Snapshot_block)];
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
  struct Cpu_state
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

#include "arithmetic.h"

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
    _regions(_static_regions)
  {}

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
    _used_mask(other._size), _regions(_static_regions)
  {
    if (other._size > size())
      reserve(other._size, true);

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
    free_retired(take_retired());
    for (unsigned i = 0; i < size(); ++i)
      {
        _regions[i].compact();
        assert(!_regions[i].has_deltas());
      }

    if (_regions != _static_regions)
      _allocator.free(_regions,
        sizeof(Mpu_region_block) * blocks_needed_for_nr_regions(size()));
  }

  /// Whether region `i` has versions newer than its base.
  bool has_deltas(unsigned i) const { return _regions[i].has_deltas(); }

  /// Changes whenever a region is modified, added, removed or relocated.
  unsigned generation() const { return _generation; }

  /**
   * Do not keep snapshots of this region set cached anymore once they stop
   * being current, see `Mem_space::prepare_destroy()`.
   */
  void disable_snapshot_cache() { write_now(&_no_snapshot_cache, true); }
  bool snapshot_cache_disabled() const
  { return access_once(&_no_snapshot_cache); }

  /**
   * An old region array, replaced by growing, that may still be referenced
   * by snapshots of other CPUs.
   *
   * Stored in the (unused) AVL tree node of the array's first moved-from
   * region, so retiring needs no allocation.
   */
  struct Retired
  {
    Retired *next;
    unsigned nr_blocks;
  };

  /**
   * Detach the list of retired region arrays.
   *
   * The caller must make every CPU that may hold a snapshot of this region
   * set take a new one before passing the list to `free_retired()`.
   */
  Retired *take_retired()
  {
    Retired *r = _retired;
    _retired = nullptr;
    return r;
  }

  /// Free retired region arrays obtained via `take_retired()`.
  void free_retired(Retired *r)
  {
    while (r)
      {
        Retired *next = r->next;
        unsigned nr_blocks = r->nr_blocks;
        auto *old = static_cast<Mpu_region *>(
          reinterpret_cast<cxx::Avl_tree_node *>(r));
        if (old == _static_regions)
          new (old) Mpu_region_block(); // clear the no-longer-used block
        else
          _allocator.free(old, sizeof(Mpu_region_block) * nr_blocks);
        r = next;
      }
  }

private:
  static constexpr unsigned regions_per_block()
  { return Config::Mpultiplex_block_size; }

  static unsigned blocks_needed_for_nr_regions(unsigned nr_regions)
  { return cxx::div_ceil(nr_regions, regions_per_block()); }

  /**
   * Reserves space to be able to store more regions.
   *
   * \param new_size  The amount to reserve space for.
   *
   * \return The amount that space has actually been reserved for.
   *         May be larger than what was requested.
   */
  size_t reserve(size_t new_size, bool init_fast_path = false)
  {
    if (new_size <= size())
      {
        WARNX(Info, "Tried to reserve no additional space for this "
                    "Mpu_region_block_storage object!");
        return size();
      }

    // calculate how many new new blocks to allocate
    unsigned size_diff = new_size - size();
    unsigned nr_blocks_needed = cxx::div_ceil(size_diff, regions_per_block());
    unsigned current_nr_blocks = size() / regions_per_block();

    auto new_regions = static_cast<Mpu_region *>(_allocator.alloc(
        sizeof(Mpu_region_block) * (nr_blocks_needed + current_nr_blocks)
    ));
    assert(new_regions);

    if (init_fast_path)
      {
        assert(_regions == _static_regions);
        assert(_used_mask.is_empty());
        for (unsigned i = 0; i < (nr_blocks_needed + current_nr_blocks)
                                 * regions_per_block(); ++i)
          new (&new_regions[i]) Mpu_region();
      }
    else
      {
        bump_generation();
        // Move all versions (base + deltas) and redirect snapshot references.
        for (unsigned i = 0; i < size(); ++i)
          new (&new_regions[i]) Mpu_region(cxx::move(_regions[i]));
        for (unsigned i = size(); i < (nr_blocks_needed + current_nr_blocks)
                                      * regions_per_block(); ++i)
          new (&new_regions[i]) Mpu_region();
        Mpu::relocate(_regions, new_regions, size());

        retire(_regions, current_nr_blocks);
      }

    _regions = new_regions;
    _size += nr_blocks_needed * regions_per_block();

    return size();
  }

  /**
   * Keep a moved-from region array alive until it was released by all
   * CPUs, see `take_retired()`.
   */
  void retire(Mpu_region *old, unsigned nr_blocks)
  {
    static_assert(sizeof(Retired) <= sizeof(cxx::Avl_tree_node));
    // Snapshots only access the base versions of the moved-from regions,
    // the AVL tree node is free to use.
    _retired = new (static_cast<cxx::Avl_tree_node *>(&old[0]))
      Retired{_retired, nr_blocks};
  }

  size_t grow()
  {
    size_t new_size = size() * 2;
    // Reserving may invalidate avl tree pointers. Disconnect them
    // before and reconstruct the tree afterwards.
    _used_tree.remove_all([](Mpu_region *){});
    reserve(new_size);

    // Reconstruct tree at new location
    for (unsigned i = 0; i < _used_mask.size(); ++i)
      {
        if (_used_mask[i])
          _used_tree.insert(&(*this)[i]);
      }

    // Also grow masks to new size.
    _reserved.resize(new_size);
    _used_mask.resize(new_size);

    printf("[%p] Mpu_regions size extended to %zu\n", this, new_size);

    return new_size;
  }

  void bump_generation() { ++_generation; }

protected:
  Mpu::Block_allocator _allocator;

private:
  Mpu_region_block _static_regions;
  Retired *_retired = nullptr;
  Mpu_region *_regions;
  unsigned _generation = 0;
  bool _no_snapshot_cache = false;
};

//---------------------------------------------------------------------------
INTERFACE [mpu && !mpultiplex]:

EXTENSION class Mpu_regions
{
public:
  /**
   * Construct new MPU region list.
   *
   * \param reserved  Map of regions that are not allocatable.
   */
  explicit Mpu_regions(Mpu_regions_mask const &reserved)
  : _size(Mpu::regions()), _reserved(reserved)
  {}

  enum class Init { Reserved_regions };

  /**
   * Construct new MPU region list.
   *
   * This is *not* a copy constructor! The other Mpu_regions object that is
   * passed will be used as a reserved-regions template. To enable fast
   * context switches, the used regions of the other object are still copied.
   */
  explicit Mpu_regions(Mpu_regions const &other, Init)
  : _size(other._size), _reserved(other._reserved)
  {
    _reserved |= other._used_mask;
    for (Mpu_region const &i : other._used_tree)
      {
        unsigned idx = other.index(&i);
        Mpu_region &r = (*this)[idx];
        r.start(i.start());
        r.end(i.end());
        r.attr(i.attr());
      }
  }

  // Hardware view used by Mpu::update(): logical == physical slot.
  Mpu_region_base const &hw_slot(unsigned i) const { return (*this)[i]; }
  unsigned hw_count() const { return size(); }
  Mword hw_keep_mask() const { return *_reserved.raw(); }

  // Without multiplexing the region array never moves.
  struct Retired {};
  Retired *take_retired() { return nullptr; }
  void free_retired(Retired *) {}

private:
  // Without multiplexing there are no snapshots to keep up to date.
  void bump_generation() {}

  Mpu_region _regions[Mem_layout::Mpu_regions];
};

EXTENSION class Mpu
{
public:
  static Mpu_regions const &prepare_update(Mpu_regions const &regions)
  { return regions; }

  static Unsigned32 ku_mem_mask(Mpu_regions const &, Unsigned32 logical)
  { return logical; }

  static void drop_cached(Mpu_regions const &) {}

  static bool check_and_handle_multiplex_fault(Mword)
  { return false; }

  static void sync_early(Mpu_regions const &regions,
                         Mpu_regions_mask const &touched,
                         bool inplace = false, int = -1)
  { sync(regions, touched, inplace); }
};

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex]:

#include <cstdlib>

#include "context.h"
#include "kmem_alloc.h"
#include "kmem_slab.h"
#include "lock_guard.h"
#include "ram_quota.h"
#include "spin_lock.h"

static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Mpu::Buddy_block_alloc> _block_alloc;
static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Mpu::Buddy_bitmap_alloc> _bitmap_alloc;
static DEFINE_GLOBAL_PRIO(EARLY_INIT_PRIO)
  Global_data<Mpu::Buddy_snapshot_alloc> _snapshot_alloc;
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

PRIVATE static
Mpu::Buddy_block_alloc *
Mpu::block_allocator()
{ return &_block_alloc; }

PRIVATE static
Mpu::Buddy_bitmap_alloc *
Mpu::bitmap_allocator()
{ return &_bitmap_alloc; }

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
 * blocks, see `Initial_alloc_sizes::Snapshot_pool`.
 */
PRIVATE static
void
Mpu::check_snapshot_pool(void *pool)
{
  constexpr size_t max = Buddy_snapshot_alloc::Max_size;
  Address start = reinterpret_cast<Address>(pool);
  Address end = start + Initial_alloc_sizes::Snapshot_pool;
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
  setup_allocator(&_block_alloc,  Initial_alloc_sizes::Block, &fm_index);
  setup_allocator(&_bitmap_alloc, Initial_alloc_sizes::Bitmap, &fm_index);
  check_snapshot_pool(setup_allocator(&_snapshot_alloc,
                                      Initial_alloc_sizes::Snapshot_pool,
                                      &fm_index));

  dump_alloc(fm_index);

  printf("MPU: hardware provides %zu regions, using %u\n",
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
  return regions._regions[i].current_ptr();
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
void
Snapshot::relocate(Mpu_region *old_base, Mpu_region *new_base, unsigned n)
{
  // Entry i references a version of logical region i. Deltas stay where
  // they are, only base versions move along with their region.
  for (unsigned i = 0; i < _size && i < n; ++i)
    {
      Entry &e = _entries[i];
      if (e.version.get() != old_base[i].base_ptr())
        continue;

      // Move our reference from the moved-from base to the new one.
      e.version = Mpu_region_version_ptr(new_base[i].base_ptr());
    }
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
void
Mpu::sync_early(Mpu_regions const &regions, Mpu_regions_mask const &touched,
                bool inplace, int first)
{
  if (first >= 0 && touched[first])
    {
      // Must not be a delta: those live on the (maybe not yet loaded) heap.
      assert(!regions.has_deltas(first));
      hw_write(first, regions[first], inplace);
    }

  unsigned i = 0;
  while (i < touched.size() && (i = touched.ffs(i)))
    {
      if (static_cast<int>(i - 1) == first)
        continue;
      if (i - 1 >= Mpu::regions())
        panic("MPU: region %u exceeds hardware before MPUltiplex init\n",
              i - 1);
      hw_write(i - 1, regions[i - 1], inplace);
    }
  hw_done();
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

  hw_write(place(s, l), s.version(l), false);
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
Mpu::relocate(Mpu_region *old_base, Mpu_region *new_base, unsigned n)
{
  // Only the snapshots of this CPU can be redirected; the moved-from array is
  // kept alive for snapshots of other CPUs (see Mpu_regions::retire()).
  if (!_state->ready)
    return; // no snapshots yet
  Cpu_state &cs = _cpu_state.current();
  if (cs.current)
    cs.current->relocate(old_base, new_base, n);
  if (cs.cached)
    cs.cached->relocate(old_base, new_base, n);
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
        hw_write(s.phys_slot(l), s.version(l), inplace && t);
      else
        hw_write(place(s, l), s.version(l), false);
    }

  hw_done();
}

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu]:

/**
 * Try to add a new region.
 *
 * If the new region overlaps with one or more existing regions, the page
 * attributes are checked and the existing regions are either extended or the
 * call fails.
 *
 * \param start  Start address of region.
 * \param end    End address of region.
 * \param attr   Region memory attributes.
 * \param join   Allow coalescing of adjacent regions with same attributes.
 * \param slot   Region index that shall be allocated or -1 to search for
 *               free entry.
 *
 * \return The mask of region slots that have been updated and that
 *         need to be synced to HW, or an error.
 */
PUBLIC inline NEEDS[Mpu_regions::extend, Mpu_regions::find_free]
Mpu_regions_update
Mpu_regions::add(Mword start, Mword end, Mpu_region_attr attr, bool join = true,
                 int slot = -1)
{
  // Find existing regions left and right of the new region. In case of a
  // collision the existing regions need to be extended and optimized.
  Mpu_region *left = nullptr;
  Mpu_region *right = nullptr;
  for (Mpu_region &i : _used_tree)
    {
      if (i.end() < start)
        left = &i;
      else if (end < i.start())
        {
          right = &i;
          break;
        }
      else if (join) [[likely]]
        return extend(&i, attr, start, end); // Slow path in case of collisions
      else
        return Mpu_regions_update(Mpu_regions_update::Error_collision);
    }

  Mpu_regions_update updates(size());
  Mpu_region *r = nullptr;

  /*
   * Possibly join with left region. Can be safely extended if attributes
   * match because possible collisions were tested already above.
   */
  if (left && (left->end() + 1U == start) && left->attr() == attr && join)
    {
      updates.set_updated(index(left));
      modify<End>(left, end);
      r = left;
    }

  /*
   * Possibly join with right region. The new region might have already been
   * joined with the left region. In this case the right region is discarded.
   * Otherwise the right region is extended to the left.
   */
  if (right && (right->start() == end + 1U) && right->attr() == attr && join)
    {
      updates.set_updated(index(right));

      if (r) // r == left
        {
          modify<End>(r, right->end());
          erase(right);
        }
      else
        {
          modify<Start>(right, start);
          r = right;
        }
    }

  // done in case we joined one of the existing regions
  if (r)
    return updates;

  // Could not join an existing region. We need to allocate a new slot.
  r = find_free(slot);
  if (!r)
    {
      size_t new_size = grow();
      r = find_free(slot);
      assert(r);

      Mpu_regions_update m(new_size);
      updates |= m;
    }

  // No reinsertion needed, because 'r' was free, therefore unused.
  modify<Full>(r, start, end, attr);
  insert(r);

  updates.set_updated(index(r));
  return updates;
}

/**
 * Delete region.
 *
 * Deleting a part of a region will result in occupying another slot. If
 * this fails, the whole mapping will be removed.
 *
 * \param start      Start of deleted region
 * \param end        End of deleted regions
 * \param attr[out]  Optional output parameter that receives the attributes of
 *                   the deleted region.
 *
 * \return The mask of region slots that have been updated and that
 *         need to be synced to HW.
 */
PUBLIC inline NEEDS[Mpu_regions::find_free]
Mpu_regions_update
Mpu_regions::del(Mword start, Mword end, Mpu_region_attr *attr = nullptr)
{
  Mpu_regions_update updates(size());

  Mpu_region *i = front();

  while (i && i->end() < start)
    i = next(i);

  while (i && i->start() <= end)
    {
      updates.set_updated(index(i));
      if (attr)
          *attr = i->attr();

      if (i->start() >= start && i->end() <= end)
        {
          // region is fully covered -> delete
          i = erase(i);
        }
      else if (i->start() < start && i->end() > end)
        {
          // unmap range falls inside of region -> try to split
          Mpu_region *r = find_free();
          if (r)
            {
              updates.set_updated(index(r));

              // No reinsertion needed, because 'r' was free, therefore unused.
              modify<Full>(r, end + 1U, i->end(), i->attr());

              i->end(start - 1U);

              insert(r);
            }
          else
            {
              size_t new_size = grow();
              r = find_free();
              assert(r);

              Mpu_regions_update m(new_size);
              updates |= m;
            }
          break;
        }
      else if (i->start() < start)
        {
          // Upper part of region overlaps with unmap range.
          modify<End>(i, start - 1U);
          i = next(i);
        }
      else
        {
          // Lower part of region overlaps with unmap range.
          modify<Start>(i, end + 1U);
          break;
        }
    }

  return updates;
}

/**
 * Find region covering the given address.
 *
 * \param addr  The search address.
 *
 * \return nullptr if no region was found, otherwise the pointer to the region.
 */
PUBLIC inline
Mpu_region const *
Mpu_regions::find(Mword addr) const
{
  if (auto n = _used_tree.last_less_equal_node(addr);
      n && n->contains(addr))
    {
      return n;
    }

  return nullptr;
}

PUBLIC inline
Mpu_region const *
Mpu_regions::find_next(Mword addr) const
{
  return _used_tree.lower_bound_node(addr);
}

/**
 * Dump regions list.
 */
PUBLIC
void
Mpu_regions::dump() const
{
  unsigned i = 0;
  printf("Used MPU regions:\n");
  while (i < _used_mask.size() && (i = _used_mask.ffs(i)))
    {
      auto const& r    = (*this)[i - 1];
      auto const& attr = r.attr();
      printf("  [" L4_MWORD_FMT ".." L4_MWORD_FMT ", %c%c, %cR%c%c]@%u\n",
             r.start(),
             r.end(),
             attr.enabled() ? '+' : '-',
             (attr.type() == L4_snd_item::Memory_type::Normal())
                ? 'N'
                : ((attr.type() == L4_snd_item::Memory_type::Uncached())
                    ? 'U' : 'B'),
             (attr.rights() & L4_fpage::Rights::U()) ? 'U' : '-',
             (attr.rights() & L4_fpage::Rights::W()) ? 'W' : '-',
             (attr.rights() & L4_fpage::Rights::X()) ? 'X' : '-',
             i);
    }
  printf("\n");
}

/**
 * Allocate a free region.
 *
 * If a specific slot is requrested it might be a reserved one.
 *
 * \param slot  Region number that shall be allocated. If negative, an empty
 *              region is searched.
 * \return Pointer to free region, nullptr otherwise.
 */
PRIVATE inline
Mpu_region *
Mpu_regions::find_free(int slot = -1)
{
  if (slot < 0)
    {
      Mpu_regions_mask avail = _reserved;
      avail |= _used_mask;
      avail.invert();
      unsigned i = avail.ffs(0);
      if (i == 0 || i > size())
        return nullptr;

      return &(*this)[i - 1];
    }
  else
    {
      if (_used_mask[slot])
        return nullptr;
      return &(*this)[slot];
    }
}

/**
 * Slow path to extend an existing region(s).
 *
 * \param first   The leftmost region that overlaps (partially) with the new
 *                region.
 * \param attr    The attributes of the new region.
 * \param start   Start address (inclusive) of the new region
 * \param end     End address (inclusive) of the new region
 */
PRIVATE inline
Mpu_regions_update
Mpu_regions::extend(Mpu_region *first, Mpu_region_attr attr, Mword start,
                    Mword end)
{
  // Make sure all regions that overlap have compatible attributes. Only then
  // we are allowed to coalesce them.
  for (auto i = first; i != nullptr; i = next(i))
    {
      if (end < i->start())
        break;
      else if (i->attr() != attr)
        return Mpu_regions_update(Mpu_regions_update::Error_collision, size());
    }

  // The remainder of the function always works on the first overlapping
  // region. We know that all overlapping regions can be coalesced. So once the
  // current region covers [start,end] we're done.
  Mpu_regions_update updates(size());
  updates.set_updated(index(first));

  // Extend to the left? Will possibly merge with compatible adjacent region.
  // Note that we have to check the attributes again because the check at the
  // entry of the function only verifies overlapping regions, not the adjacent
  // ones.
  //
  // This needs to be done only once because we already start at the first
  // overlapping region.
  if (first->start() > start)
    {
      Mpu_region *left = prev(first);
      if (left && left->end() + 1U >= start && left->attr() == attr)
        {
          Mword new_start = left->start();
          // Remove `left` before reassigning the start of `first` so there is
          // no collision when reinserting into the AVL tree.
          erase(left);
          modify<Start>(first, new_start);
          updates.set_updated(index(left));
        }
      else
        modify<Start>(first, start);
    }

  // Extend to the right? Possibly merge with adjacent region. Again, check the
  // attributes before merging because the rightmost, adjacent region might not
  // be compatible. Do it in a loop because multiple regions to the right might
  // be covered.
  while (first->end() < end)
    {
      Mpu_region *right = next(first);
      if (right && right->start() <= end + 1U && right->attr() == attr)
        {
          modify<End>(first, right->end());
          updates.set_updated(index(right));
          erase(right);
        }
      else
        modify<End>(first, end);
    }

  return updates;
}

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex && !debug]:

PRIVATE static inline
void
Mpu::dump_alloc(unsigned)
{}

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex && debug]:

IMPLEMENT
void
Mpu::Block_allocator::dump() const
{ Mpu::block_allocator()->dump(); }

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
