INTERFACE [mpu]:

#include <cxx/avl_tree>

#include "arithmetic.h"
#include "bitmap.h"
#include "buddy_alloc.h"
#include "config.h"
#include "l4_fpage.h"
#include "l4_msg_item.h"
#include "mem_layout.h"
#include "panic.h"
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

/**
 * A single MPU region.
 *
 * Regions are organized as a sorted, double linked, cyclic list. The
 * architecture extends the struct with the actual data for the hardware. All
 * addresses are inclusive!
 */
struct Mpu_region : public cxx::Avl_tree_node
{
  constexpr Mpu_region();
  Mpu_region(Mword start, Mword end, Mpu_region_attr a);

  using Key_type = Mword;
  static Key_type key_of(Mpu_region const *r)
  { return r->start(); }

  constexpr Mword start() const;
  constexpr Mword end() const;
  constexpr Mpu_region_attr attr() const;

  inline void start(Mword start);
  inline void end(Mword end);
  inline void attr(Mpu_region_attr attr);
  inline void disable();

  friend bool operator < (Mpu_region const &lhs, Mpu_region const &rhs)
  { return lhs.end() < rhs.start(); }

  constexpr bool contains(Mword addr) const
  { return start() <= addr && addr <= end(); }
};

using Mpu_region_block = Mpu_region[Config::Mpultiplex_block_size];

/**
 * Base for classes dealing with a growing number of MPU regions.
 *
 * Provides the storage as well as means to access stored regions and
 * reserve space for additional regions.
 */
template<class TYPE, typename ALLOC>
class Mpu_region_block_storage
{
public:
  explicit Mpu_region_block_storage(size_t size = Config::Mpultiplex_block_size)
  : _size(Config::Mpultiplex_block_size)
  , _regions(_static_regions)
  {
    if (size > Config::Mpultiplex_block_size)
      _size = reserve(size);
  }

  inline unsigned size() const { return _size; }

  TYPE &operator[](unsigned i)
  {
    assert(i < size());
    return _regions[i];
  }

  TYPE const &operator[](unsigned i) const
  {
    assert(i < size());
    return _regions[i];
  }

  unsigned index(TYPE const *r) const
  { return r - _regions; }

  /**
   * Reserves space to be able to store more regions.
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
                    "Mpu_region_block_storage object!");
        return size();
      }

    // calculate how many new new blocks to allocate
    unsigned size_diff = new_size - size();
    unsigned nr_blocks_needed = cxx::div_ceil(size_diff, regions_per_block());

    auto new_regions = static_cast<Mpu_region *>(
      _allocator.alloc(sizeof(Mpu_region_block) * nr_blocks_needed)
    );
    for (unsigned i = 0; i < size(); ++i)
    {
      auto const& r = _regions[i];
      new (&new_regions[i]) Mpu_region(r.start(), r.end(), r.attr());
    }

    // Free the old blocks or clear the no-longer-used _static_regions.
    if (size() > regions_per_block())
      _allocator.free(_regions, (size() / regions_per_block()) * sizeof(Mpu_region_block));
    else
      new (_regions) Mpu_region_block();

    _regions = new_regions;
    _size += nr_blocks_needed * regions_per_block();

    return size();
  }

private:
  static constexpr unsigned regions_per_block()
  { return Config::Mpultiplex_block_size; }

  static unsigned blocks_needed_for_nr_regions(unsigned nr_regions)
  { return cxx::div_ceil(nr_regions, regions_per_block()); }

  unsigned _size;
  ALLOC _allocator;
  Mpu_region_block _static_regions;
  Mpu_region *_regions;
};

/**
 * Interface to the CPUs MPU.
 */
class Mpu
{
public:
  class Block_allocator
  : public Buddy_t_base<cxx::log2u(sizeof(Mpu_region_block) - 1) + 1, 16>
  {
  public:
    inline void *
    alloc(unsigned long size)
    {
      (void) size;
      panic("alloc should not be called right now!");
    }

    inline void
    free(void *block, unsigned long size)
    {
      (void) block;
      (void) size;
      panic("free should not be called right now!");
    }
  };

  class Dynamic_bitmap_allocator
  : public Buddy_t_base<cxx::log2u((Config::Mpultiplex_block_size / 8) - 1) + 1, 16>
  {};

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
   */
  static unsigned regions();
};

/**
 * Bit mask of MPU regions.
 */
class Mpu_regions_mask : public Bitmap<Mem_layout::Mpu_regions>
{
public:
  Mpu_regions_mask()
  { clear_all(); }

  // Required to make it compatible with Mpu_regions_update::Updates
  using Bitmap<Mem_layout::Mpu_regions>::operator=;
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
class Mpu_regions_update
{
  // Use an additional error bit.
  typedef Bitmap<Mem_layout::Mpu_regions + 1> Updates;
  enum { Error_bit = Mem_layout::Mpu_regions };

  Updates _updates;

public:
  enum Error {
    Error_no_mem,     ///< Out of regions.
    Error_collision,  ///< New region collides with existing, incompatible one.
    Error_max
  };
  static_assert(Error_max <= 2, "Can store only two errors");

  Mpu_regions_update()
  { _updates.clear_all(); }

  /**
   * Construct an error "update".
   */
  explicit Mpu_regions_update(Error error)
  {
    _updates.clear_all();
    _updates.set_bit(Error_bit);
    _updates.bit(0, error == Error_collision);
  }

  Mpu_regions_update(Mpu_regions_update const &) = default;
  Mpu_regions_update &operator=(Mpu_regions_update const &) = default;

  /**
   * Add a region to the update set.
   */
  inline void set_updated(unsigned region)
  { _updates.set_bit(region); }

  /**
   * Combine updates.
   *
   * Errors are sticky, that is, if any of the operands is in the error state,
   * the result will also be in an error state. This is just a safety measure.
   * The caller should check each individual update operation for errors.
   */
  Mpu_regions_update &operator|=(Mpu_regions_update const &other) &
  {
    _updates |= other._updates;
    return *this;
  }

  /**
   * Bool operator testing if update succeeded.
   */
  explicit operator bool() const
  { return !_updates[Error_bit]; }

  /**
   * Fetch update result.
   *
   * It is the responsibility of the caller to check for errors before.
   */
  Mpu_regions_mask value() const
  {
    Mpu_regions_mask ret;
    ret = _updates;
    return ret;
  }

  /**
   * Returns error code in case update failed.
   */
  Error error() const
  { return _updates[0] ? Error_collision : Error_no_mem; }
};

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
: private Mpu_region_block_storage<Mpu_region, Mpu::Block_allocator>
{
  using Region_tree = cxx::Avl_tree<Mpu_region, Mpu_region>;

public:
  /**
   * Construct new MPU region list.
   *
   * \param reserved  Map of regions that are not allocatable.
   */
  explicit Mpu_regions(Mpu_regions_mask const &reserved)
  : Mpu_region_block_storage(Mpu::regions()), _reserved(reserved)
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
  : Mpu_region_block_storage(other.size()), _reserved(other._reserved)
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

  Mpu_region const &operator[](unsigned i) const &
  { return Mpu_region_block_storage::operator[](i); }
  Mpu_region const &operator[](unsigned i) const && = delete;

  Mpu_regions_mask used()     const { return _used_mask; }
  Mpu_regions_mask reserved() const { return _reserved; }
  unsigned         size()     const { return Mpu_region_block_storage::size(); }

private:
  Mpu_region &operator[](unsigned i) &
  { return Mpu_region_block_storage::operator[](i); }
  Mpu_region &operator[](unsigned i) && = delete;

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
    _used_mask.clear_bit(index(r));
    r->disable();
    return _used_tree.erase(r->start());
  }

  enum Insert { After, Before, Back };

  bool insert(Mpu_region *r)
  {
    _used_mask.set_bit(index(r));
    auto [node, was_not_in_tree_before] = _used_tree.insert(r);
    return was_not_in_tree_before;
  }

  Mpu_region *reinsert(Mpu_region *r, Mpu_region::Key_type new_start)
  {
    _used_tree.erase(r->start());
    r->start(new_start);
    auto [node, was_not_in_tree_before] = _used_tree.insert(r);

    assert(was_not_in_tree_before);

    return node;
  }

  size_t reserve(size_t new_size)
  {
    Mpu_region_block_storage::reserve(new_size);

    // Reconstruct tree at new location
    _used_tree.remove_all([](Mpu_region *){});
    for (unsigned i = 0; i < _used_mask.size(); ++i)
    {
      if (_used_mask[i])
        _used_tree.insert(&(*this)[i]);
    }

    return size();
  }

  Mpu_regions_mask _reserved;
  Mpu_regions_mask _used_mask;  ///< Bit mask of occupied regions
  Region_tree _used_tree;       ///< Sorted tree (by address) of used regions
};

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

  Mpu_regions_update updates;
  Mpu_region *r = nullptr;

  /*
   * Possibly join with left region. Can be safely extended if attributes
   * match because possible collisions were tested already above.
   */
  if (left && (left->end() + 1U == start) && left->attr() == attr && join)
    {
      updates.set_updated(index(left));
      left->end(end);
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
          r->end(right->end());
          erase(right);
        }
      else
        {
          reinsert(right, start);
          r = right;
        }
    }

  // done in case we joined one of the existing regions
  if (r)
    return updates;

  // Could not join an existing region. We need to allocate a new slot.
  r = find_free(slot);
  if (!r)
    return Mpu_regions_update(Mpu_regions_update::Error_no_mem);

  // No reinsertion needed, because 'r' was free, therefore unused.
  r->start(start);
  r->end(end);
  r->attr(attr);

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
  Mpu_regions_update updates;

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
              r->start(end + 1U);
              r->end(i->end());
              r->attr(i->attr());

              i->end(start - 1U);

              insert(r);
            }
          else
            {
              // no further regions available -> delete whole region
              WARN("Dropped whole region [" L4_MWORD_FMT ":" L4_MWORD_FMT
                   "] while deleting  [" L4_MWORD_FMT ":" L4_MWORD_FMT "]\n",
                   i->start(), i->end(), start, end);
              erase(i);
            }
          break;
        }
      else if (i->start() < start)
        {
          // Upper part of region overlaps with unmap range.
          i->end(start - 1U);
          i = next(i);
        }
      else
        {
          // Lower part of region overlaps with unmap range.
          reinsert(i, end + 1U);
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
Mpu_region*
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
        return Mpu_regions_update(Mpu_regions_update::Error_collision);
    }

  // The remainder of the function always works on the first overlapping
  // region. We know that all overlapping regions can be coalesced. So once the
  // current region covers [start,end] we're done.
  Mpu_regions_update updates;
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
          erase(left);
          reinsert(first, left->start());
          updates.set_updated(index(left));
        }
      else
        reinsert(first, start);
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
          first->end(right->end());
          updates.set_updated(index(right));
          erase(right);
        }
      else
        first->end(end);
    }

  return updates;
}
