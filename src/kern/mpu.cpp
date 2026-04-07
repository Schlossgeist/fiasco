INTERFACE [mpu]:

#include <cxx/dlist>
#include <cxx/type_traits>

#include "arithmetic.h"
#include "config.h"
#include "dynamic_bitmap.h"
#include "l4_fpage.h"
#include "l4_msg_item.h"
#include "mem_layout.h"
#include "panic.h"
#include "per_cpu_data.h"
#include "warn.h"

/**
 * Generic, implementation agnostic MPU region attributes.
 */
class Mpu_region_attr
{
  L4_fpage::Rights _rights;
  L4_snd_item::Memory_type _type;

  // only add up to 8 attribute flags
  enum Flags {
    Enabled     = 1 << 0,
    Pinned      = 1 << 1,
    Is_ku_mem   = 1 << 2,
  };
  Unsigned8 _flags = 0;

  constexpr Mpu_region_attr(L4_fpage::Rights rights,
                            L4_snd_item::Memory_type type,
                            bool enabled, bool pinned, bool ku_mem)
  : _rights(rights), _type(type)
  {
    _flags |= enabled ? Enabled   : 0;
    _flags |= pinned  ? Pinned    : 0;
    _flags |= ku_mem  ? Is_ku_mem : 0;
  }

public:
  Mpu_region_attr() = default;

  friend constexpr bool operator == (Mpu_region_attr const &lhs,
                                     Mpu_region_attr const &rhs) = default;

  static constexpr Mpu_region_attr
  make_attr(L4_fpage::Rights rights,
            L4_snd_item::Memory_type type = L4_snd_item::Memory_type::Normal(),
            bool enabled = true, bool pinned = false, bool ku_mem = false)
  {
    return Mpu_region_attr(rights, type, enabled, pinned, ku_mem);
  }

  constexpr L4_fpage::Rights rights() const { return _rights; }
  constexpr L4_snd_item::Memory_type type() const { return _type; }
  constexpr bool enabled() const { return _flags & Enabled; }
  constexpr bool pinned() const { return _flags & Pinned; }
  constexpr bool ku_mem() const { return _flags & Is_ku_mem; }

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
 * Base for classes handling a single MPU region. Classes that organize
 * MPU regions, i.e. in linked lists or trees, inherit from this.
 *
 * The architecture extends the struct with the actual data for the hardware.
 * All addresses are inclusive!
 */
struct Mpu_region_base
{
  using Label_buffer = char[16];

  constexpr Mpu_region_base();
  Mpu_region_base(Mword start, Mword end, Mpu_region_attr a);

  constexpr Mword start() const;
  constexpr Mword end() const;
  constexpr Mpu_region_attr attr() const;
  constexpr Label_buffer const& label() const
  { return _label; }

  inline void start(Mword start);
  inline void end(Mword end);
  inline void attr(Mpu_region_attr attr);
  inline void disable();
  inline void label(const char *label)
  {
    const int buffer_end = sizeof(Label_buffer) - 1;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
    __builtin_strncpy(_label, label, buffer_end);
    _label[buffer_end] = '\0';
#pragma GCC diagnostic pop
  }

  friend bool operator < (Mpu_region_base const &lhs, Mpu_region_base const &rhs)
  { return lhs.end() < rhs.start(); }

  constexpr bool contains(Mword addr) const
  { return start() <= addr && addr <= end(); }

private:
  Label_buffer _label = {0};
  bool pinned = false;
  bool ku_mem = false;
};

/**
 * A single MPU region stored inside the MPUltiplex cache.
 */
struct Cached_mpu_region : public Mpu_region_base
{
  using Mpu_region_base::Mpu_region_base;

  Cached_mpu_region(Mpu_region_base const &other, int slot = -1)
  : Mpu_region_base(other), _physical_slot(slot) {}

  constexpr bool is_active() const
  { return slot() >= 0 ;}

  constexpr int slot() const
  { return _physical_slot; }

  inline void slot(int slot)
  { _physical_slot = slot; }

private:
  int _physical_slot = -1;
};

/**
 * A single MPU region.
 *
 * Regions are organized as a sorted, double linked, cyclic list.
 */
struct Mpu_region : public Mpu_region_base, public cxx::D_list_item
{
  using Mpu_region_base::Mpu_region_base;
};

struct Mpu_allocator
{
  static void *alloc(size_t size);
  static void free(size_t size, void *obj);
};

using Bitmap_type = Dynamic_bitmap<Config::Mpultiplex_block_size, Mpu_allocator>;

class Mpu_regions;
class Mpu_regions_mask;

class IMpu_region_base_container
{
public:
  virtual Mpu_region_base const &at(unsigned i) const = 0;
  virtual ~IMpu_region_base_container() = default;
};

/**
 * Base for classes dealing with a growing number of MPU regions.
 *
 * Provides the storage as well as means to access stored regions and
 * reserve space for additional regions.
 */
template<class TYPE, typename ALLOC = Mpu_allocator,
         typename = cxx::enable_if_t<cxx::is_convertible_v<TYPE, Mpu_region_base>>>
class Mpu_region_block_storage
{
public:
  explicit Mpu_region_block_storage(size_t size = 0)
  : _size(Config::Mpultiplex_block_size)
  {
    if (size > Config::Mpultiplex_block_size)
      _size = reserve(size);
  }

  inline unsigned size() const { return _size; }

  TYPE &operator[](unsigned i)
  {
    precondition(i < size());
    size_t idx = i / regions_per_block();
    size_t pos = i % regions_per_block();

    // traverse the chain of blocks
    Mpu_region_block *current_block = &_regions;
    for (unsigned j = 0; j < idx; ++j)
      current_block = current_block->next_block;

    return current_block->regions[pos];
  }

  TYPE const &operator[](unsigned i) const
  {
    precondition(i < size());
    size_t idx = i / regions_per_block();
    size_t pos = i % regions_per_block();

    // traverse the chain of blocks
    Mpu_region_block const *current_block = &_regions;
    for (unsigned j = 0; j < idx; ++j)
      current_block = current_block->next_block;

    return current_block->regions[pos];
  }

  unsigned index(TYPE const *r) const
  {
    Mpu_region_block const *current_block = &_regions;
    unsigned block_index = 0;
    while (current_block != nullptr)
      {
        unsigned i = r - current_block->regions + block_index;
        if (block_index <= i && i < block_index + regions_per_block())
          return i;

        current_block = current_block->next_block;
        block_index += regions_per_block();
      }

    panic("Searched index of Mpu_region that is not part of this "
          "Mpu_region_block_storage object.");
  }

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

    // find current end of chain of blocks
    Mpu_region_block *previous_block = &_regions;
    while (previous_block->next_block != nullptr)
      previous_block = previous_block->next_block;

    // allocate and connect the chain of new blocks
    for (unsigned i = 0; i < nr_blocks_needed; ++i)
      {
        Mpu_region_block *allocated_block = static_cast<Mpu_region_block *>(
          _allocator.alloc(sizeof(Mpu_region_block))
        );
        new (allocated_block) Mpu_region_block();

        previous_block->next_block = allocated_block;
        previous_block = allocated_block;
      }

    _size += nr_blocks_needed * regions_per_block();

    return size();
  }

  void clear()
  {
    Mpu_region_block *current_block = &_regions;
    while (current_block != nullptr)
      {
        Mpu_region_block *next_block = current_block->next_block;

        new (current_block) Mpu_region_block();

        current_block->next_block = next_block;
        current_block = current_block->next_block;
      }
  }

private:
  /**
   * A block of MPU regions.
   *
   * These are allocated on demand when the number of MPU regions the
   * MPUltiplex subsystem manages exceeds the number of physical MPU regions
   * supported by the hardware.
   * Choosing a sufficiently large block size during kernel configuration
   * effectively eliminates allocations at runtime.
   */
  struct Mpu_region_block
  {
    TYPE regions[Config::Mpultiplex_block_size];
    Mpu_region_block *next_block = nullptr;
  };

  static constexpr unsigned regions_per_block()
  { return Config::Mpultiplex_block_size; }

  static unsigned blocks_needed_for_nr_regions(unsigned nr_regions)
  { return cxx::div_ceil(nr_regions, regions_per_block()); }

  unsigned _size;
  ALLOC _allocator;
  Mpu_region_block _regions;
};

class Backing_storage
: public IMpu_region_base_container,
  public Mpu_region_block_storage<Cached_mpu_region, Mpu_allocator>
{
public:
  Backing_storage(size_t size = 0)
  : Mpu_region_block_storage(size) {}

  using Mpu_region_block_storage<Cached_mpu_region>::operator[];

  Mpu_region_base const &at(unsigned i) const override
  { return (*this)[i]; }
};

/**
 * Interface to the CPUs MPU.
 * Handles the translation between the virtual MPU regions that the rest
 * of the kernel works with and the physical MPU hardware with its limited
 * number of MPU regions.
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
   * Initialize the MPUltiplex subsystem for this CPU.
   *
   * Constructs the Mpu_region_block_storage that stores all the Mpu_region
   * object managed by the subsystem.
   * After calling this, Mpu::mpultiplex_enabled returns 'true' and all calls
   * to Mpu::sync and Mpu::update are cached.
   *
   * \param cpu  The Cpu which is initialized.
   */
  static void init_mpultiplex(Cpu_number cpu);

  /**
   * Returns 'true' after the initialization of the MPUltiplex subsystem
   * has finished.
   */
  static bool mpultiplex_enabled();

  /**
   * Check the presence of a MPU region in the cache that includes the
   * given address. If a matching region is found, it is swapped in.
   *
   * \param address     Address to look up in the cache and possibly handle
   *                    the swapping for.
   *
   * \return Whether the given address was indeed cached and has been swapped
   *         in successfully.
   */
  static bool check_and_handle_multiplex_fault(Mword address);

  /**
   * Get the locations of the MPU hardware slots currently used for
   * kernel-user memory mapping.
   */
  static Unsigned32 get_current_ku_mem();

  /**
   * Write back changes to hardware.
   *
   * \param regions         The Mpu_regions object that was updated.
   * \param touched         Impacted regions.
   * \param inplace         Update region directly instead of performing a safe
   *                        disable-update-enable sequence.
   * \param bypass_cache    Do not cache this operation for MPU multiplexing.
   */
  static void sync(Mpu_regions const &regions, Mpu_regions_mask const &touched,
                   bool inplace = false, bool bypass_cache = false);

  /**
   * Update MPU with new regions list.
   *
   * Write back all `regions` into hardware.
   */
  static void update(Mpu_regions const &regions);

  /**
   * Get the number of virtual regions that the MPUltiplex subsystem is
   * currently managing.
   */
  static unsigned regions();

  /**
   * Dump MPU state.
   */
  static void dump();

  /**
   * Get number of regions supported by the MPU.
   */
  static unsigned hardware_regions();

private:
// === CACHE MAINTENANCE ======================================================

  /**
   * Expands the number of virtual regions the MPU is multiplexing.
   */
  static void expand_virtual_regions(size_t new_size);

  /**
   * Reset to initial state.
   */
  static void flush_cache();

// === SWAPPING ===============================================================

  using Virt_slot = int;
  using Phys_slot = unsigned;

  static void swap(unsigned victim_slot, Cached_mpu_region const &region,
                   bool inplace = false);

  // bitmask of cached regions that are currently active in the physical MPU
  static Per_cpu<Mpu_regions_mask> _active_regions;
  // bitmask of cached regions that are not supposed to be swapped out
  static Per_cpu<Mpu_regions_mask> _pinned_regions;
  // storage for the data structures used by the MPUltiplex subsystem
  static Per_cpu<Backing_storage> _virtual_regions;
};

IMPLEMENTATION [mpu]:

#include <cstdlib>

#include "kmem_alloc.h"
#include "ram_quota.h"

DEFINE_PER_CPU Per_cpu<Mpu_regions_mask> Mpu::_active_regions;
DEFINE_PER_CPU Per_cpu<Mpu_regions_mask> Mpu::_pinned_regions;
DEFINE_PER_CPU Per_cpu<Backing_storage> Mpu::_virtual_regions;

IMPLEMENT static inline NEEDS["kmem_alloc.h", "ram_quota.h"]
void *
Mpu_allocator::alloc(size_t size)
{
  return Kmem_alloc::allocator()->q_alloc(Ram_quota::root.unwrap(), Bytes(size));
}

IMPLEMENT static inline NEEDS["kmem_alloc.h", "ram_quota.h"]
void
Mpu_allocator::free(size_t size, void *obj)
{
  Kmem_alloc::allocator()->q_free(Ram_quota::root.unwrap(), Bytes(size), obj);
}

IMPLEMENT static inline
void Mpu::init_mpultiplex(Cpu_number cpu)
{
  new (&_active_regions.cpu(cpu)) Mpu_regions_mask(Config::Mpultiplex_block_size);
  new (&_pinned_regions.cpu(cpu)) Mpu_regions_mask(Config::Mpultiplex_block_size);
  // includes Kernel Text, Kip, Kernel Heap and UART MMIO
  _pinned_regions.cpu(cpu).set_first_bits(4);
  new (&_virtual_regions.cpu(cpu)) Backing_storage(Config::Mpultiplex_block_size);
  printf(ANSI("MPUltiplex subsystem", MAGENTA, BOLD)
         " initialized with "
         ANSI("block size of %u", RED, BOLD)
         " and "
         ANSI("%u hardware regions", RED, BOLD)
         "...\n",
         Config::Mpultiplex_block_size, Mpu::hardware_regions());
}

IMPLEMENT static inline
bool Mpu::mpultiplex_enabled()
{
  return _virtual_regions.current().size() > 0;
}

IMPLEMENT static inline
void Mpu::expand_virtual_regions(size_t new_size)
{
  Mpu_regions_mask m(new_size);
  _active_regions.current() |= m;
  _pinned_regions.current() |= m;
  _virtual_regions.current().reserve(new_size);

  INFO("[CPU%u] MPUltiplex regions cache size extended to %zu\n",
       cxx::int_value<Cpu_number>(current_cpu()), new_size);
  Mpu::dump();
}

IMPLEMENT static inline NEEDS[<cstdlib>]
bool Mpu::check_and_handle_multiplex_fault(Mword address)
{
  Mpu_regions_mask const &curr_active_regions  = _active_regions.current();
  Backing_storage  const &curr_virtual_regions = _virtual_regions.current();

  int swap_in_slot = -1;
  int swap_out_slot = -1;
  for (unsigned i = 0; i < curr_virtual_regions.size(); ++i)
    if (curr_virtual_regions[i].contains(address))
      {
        if (!curr_active_regions[i])
          {
            // region is cached, but inactive -> swap it in
            swap_in_slot = i;
            break;
          }
        else
          return false; // region is cached and active -> bail out
      }

  // not a multiplex fault; bail out
  if (swap_in_slot < 0) return false;

  Mpu_regions_mask available_regions = _active_regions.current();
  available_regions &= ~_pinned_regions.current();

  while (swap_out_slot = rand() % regions(), !available_regions[swap_out_slot])
    ; // empty statement

  Cached_mpu_region &swap_in_region = _virtual_regions.current()[swap_in_slot];
  Cached_mpu_region &swap_out_region = _virtual_regions.current()[swap_out_slot];

  const int hardware_slot = swap_out_region.slot();
  invariant(3 < hardware_slot);
  invariant(static_cast<unsigned>(hardware_slot) < hardware_regions());

//  INFO("Swapped region in slot %d [" L4_MWORD_FMT ".." L4_MWORD_FMT "]\n\t"
//       "for region in slot %d [" L4_MWORD_FMT ".." L4_MWORD_FMT "]\n\t"
//       "via hardware slot %d.\n",
//       swap_out_slot, swap_out_region.start(), swap_out_region.end(),
//       swap_in_slot, swap_in_region.start(), swap_in_region.end(),
//       hardware_slot);

  Mpu::swap(hardware_slot, swap_in_region);

  _active_regions.current().clear_bit(swap_out_slot);
  _active_regions.current().set_bit(swap_in_slot);
  swap_out_region.slot(-1);
  swap_in_region.slot(hardware_slot);

//  Mpu::dump();

  return true;
}

IMPLEMENT static inline
void Mpu::flush_cache()
{
  _active_regions.current().clear_all();
  _pinned_regions.current().set_first_bits(4);
  _virtual_regions.current().clear();
}

IMPLEMENT static inline
Unsigned32 Mpu::get_current_ku_mem()
{
  precondition(_active_regions.current().popcount() < 32);

  Unsigned32 result = 0;
  Mpu_regions_mask const &curr_active_regions  = _active_regions.current();
  Backing_storage  const &curr_virtual_regions = _virtual_regions.current();

  unsigned i = 0;
  while (i < curr_active_regions.size() && (i = curr_active_regions.ffs(i)))
    {
      auto const &r = curr_virtual_regions[i - 1];
      if (r.attr().ku_mem())
        result |= 1 << r.slot();
    }

  return result;
}

IMPLEMENT static inline
unsigned Mpu::regions()
{
  return mpultiplex_enabled()
    ? _virtual_regions.current().size() : Config::Mpultiplex_block_size;
}

#include "ansi.h"

IMPLEMENT static
void Mpu::dump()
{
  printf("Cached + active MPU regions:\n");

  int pad = 2 * sizeof(Mword);
  printf(ANSI("  %16s - [%*s..%*s, enabled|mem type, rights]@slot[in hw] - multiplex state\n", BOLD),
         "label", -pad, "start", pad, "end");

  for (unsigned i = 0; i < _virtual_regions.current().size(); ++i)
    {
      auto const &region = _virtual_regions.current()[i];
      auto attr = region.attr();
      //          FORMAT STR         DELIMITER STR
      ansi_printf("  %16s "          ANSI("- [", DIM) ""     // label
                  "" L4_MWORD_FMT "" ANSI("..", DIM) ""      // start
                  "" L4_MWORD_FMT "" ANSI(", ", DIM) ""      // end
                  "%7s"              ANSI("|", DIM) ""       // enabled 
                  "%-8s"             ANSI(",   ", DIM) ""    // type
                  "%cR%c%c"          ANSI("]@", DIM) ""      // rights
                  "%-4u"             ANSI("[", DIM) ""       // slot
                  "%5d"              ANSI("] - ", DIM) ""    // hardware slot
                  "%s, %s\n",                                // multiplex state
                  region.label(),
                  region.start(),
                  region.end(),
                  attr.enabled() ? ANSI("yes", GREEN) : ANSI("no", RED),
                  (attr.type() == L4_snd_item::Memory_type::Normal())
                     ? "normal"
                     : ((attr.type() == L4_snd_item::Memory_type::Uncached())
                         ? "uncached" : "buffered"),
                  (attr.rights() & L4_fpage::Rights::U()) ? 'U' : '-',
                  (attr.rights() & L4_fpage::Rights::W()) ? 'W' : '-',
                  (attr.rights() & L4_fpage::Rights::X()) ? 'X' : '-',
                  i, region.slot(),
                  _active_regions.current()[i] ? "active" : "cached",
                  _pinned_regions.current()[i] ? "pinned" : "");
    }
  printf("\n");
}

INTERFACE [mpu]:

/**
 * Bit mask of MPU regions.
 */
class Mpu_regions_mask : public Bitmap_type
{
public:
  Mpu_regions_mask(unsigned size = Mpu::hardware_regions())
  : Bitmap_type(size, Mpu_allocator())
  { clear_all(); }

  Mpu_regions_mask(Bitmap_type const &o)
  : Bitmap_type(o)
  {}

  void dump() const
  {
    printf("mask: [");
    for (unsigned i = 0; i < size(); ++i)
      {
        if (i != 0 && i % 8 == 0)
          printf(" ");
        printf("%d", (*this)[i] ? 1 : 0);
      }
    printf(">\n");
  }

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

  Mpu_regions_update(unsigned size = Mpu::hardware_regions())
  : Mpu_regions_mask(size)
  {}

  /**
   * Construct an error "update".
   */
  explicit Mpu_regions_update(Error error, unsigned size = Mpu::hardware_regions())
  : Mpu_regions_mask(size)
  {
    _error_state |= error;
  }

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
: public IMpu_region_base_container,
  private Mpu_region_block_storage<Mpu_region, Mpu_allocator>
{
  typedef cxx::D_list<Mpu_region> Region_list;

public:
  /**
   * Construct new MPU region list.
   *
   * \param reserved  Map of regions that are not allocatable.
   */
  explicit Mpu_regions(Mpu_regions_mask const &reserved)
  : Mpu_region_block_storage(Config::Mpultiplex_block_size), _reserved(reserved)
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
    for (Mpu_region *i : other._used_list)
      {
        unsigned idx = other.index(i);
        Mpu_region *r = &((*this)[idx]);
        r->start(i->start());
        r->end(i->end());
        r->attr(i->attr());
        r->label(i->label());
      }
  }

  Mpu_region const &operator[](unsigned i) const &
  { return Mpu_region_block_storage::operator[](i); }
  Mpu_region const &operator[](unsigned i) const && = delete;

  Mpu_regions_mask const& used()     const { return _used_mask; }
  Mpu_regions_mask const& reserved() const { return _reserved; }
  unsigned                size()     const { return Mpu_region_block_storage::size(); }

  // IMpu_region_base_container interface
  Mpu_region_base const &at(unsigned i) const override
  { return (*this)[i]; }

private:
  Mpu_region &operator[](unsigned i) &
  { return Mpu_region_block_storage::operator[](i); }
  Mpu_region &operator[](unsigned i) && = delete;

  Mpu_region *deref_iter(Region_list::Iterator iter) const
  { return iter != _used_list.end() ? *iter : nullptr; }

  Mpu_region *front() const
  { return deref_iter(_used_list.begin()); }

  Mpu_region *next(Mpu_region *r) const
  {
    auto iter = _used_list.iter(r);
    return deref_iter(++iter);
  }

  Mpu_region *prev(Mpu_region *r) const
  {
    auto iter = _used_list.iter(r);
    return iter != _used_list.begin() ? *(--iter) : nullptr;
  }

  Mpu_region *erase(Mpu_region *r)
  {
    _used_mask.clear_bit(index(r));
    r->disable();
    return deref_iter(_used_list.erase(_used_list.iter(r)));
  }

  enum Insert { After, Before, Back };

  void insert(Mpu_region *r, Insert mode, Mpu_region *pos)
  {
    _used_mask.set_bit(index(r));
    if (mode == After)
      _used_list.insert_after(r, _used_list.iter(pos));
    else if (mode == Before)
      _used_list.insert_before(r, _used_list.iter(pos));
    else
      _used_list.push_back(r);
  }

  size_t reserve(size_t new_size)
  {
    new_size = Mpu_region_block_storage::reserve(new_size);

    Mpu_regions_mask m(new_size);
    _reserved |= m;
    _used_mask |= m;

    return new_size;
  }

  Mpu_regions_mask _reserved;
  Mpu_regions_mask _used_mask;  ///< Bit mask of occupied regions
  Region_list _used_list;       ///< Sorted list (by address) of used regions
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
                 int slot = -1, const char *label = "undefined")
{
  // Find existing regions left and right of the new region. In case of a
  // collision the existing regions need to be extended and optimized.
  Mpu_region *left = nullptr;
  Mpu_region *right = nullptr;
  for (Mpu_region *i : _used_list)
    {
      if (i->end() < start)
        left = i;
      else if (end < i->start())
        {
          right = i;
          break;
        }
      else if (join) [[likely]]
        return extend(i, attr, start, end); // Slow path in case of collisions
      else
        return Mpu_regions_update(Mpu_regions_update::Error_collision, size());
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
          right->start(start);
          r = right;
        }
    }

  // done in case we joined one of the existing regions
  if (r)
    {
      r->label(label);
      return updates;
    }

  // Could not join an existing region. We need to allocate a new slot.
  r = find_free(slot);
  if (!r)
    {
      auto new_size = reserve(size() * 2);
      // WARNX(Info, "MPU regions size extended to %zu\n", new_size);

      return add(start, end, attr, join, slot, label);
    }

  r->start(start);
  r->end(end);
  r->attr(attr);

  // insert into sorted list
  if (left)
    insert(r, After, left);
  else if (right)
    insert(r, Before, right);
  else
    insert(r, Back, nullptr);

  updates.set_updated(index(r));
  r->label(label);
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

              r->attr(i->attr());
              r->start(end + 1U);
              r->end(i->end());

              i->end(start - 1U);

              insert(r, After, i);
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
          i->start(end + 1U);
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
  for (auto const &i : _used_list)
    {
      if (addr <= i->end())
        {
          if (addr >= i->start())
            return i;
          else
            return nullptr;
        }
    }

  return nullptr;
}

PUBLIC inline
Mpu_region const *
Mpu_regions::find_next(Mword addr) const
{
  for (auto const &i : _used_list)
    {
      if (addr < i->start())
        return i;
    }

  return nullptr;
}

#include "ansi.h"

/**
 * Dump regions list.
 */
PUBLIC
void
Mpu_regions::dump() const
{
  printf("[%p] Reserved + used MPU regions:\n", this);
  Mpu_regions_mask mask;
  mask |= _used_mask;
  mask |= _reserved;

  int pad = 2 * sizeof(Mword);
  printf(ANSI("  %16s - [%*s..%*s, enabled|mem type, rights]@slot - status\n", BOLD),
         "label", -pad, "start", pad, "end");

  unsigned i = 0;
  while (i < mask.size() && (i = mask.ffs(i)))
    {
      Mpu_region const &region = (*this)[i - 1];
      auto attr = region.attr();
      //          FORMAT STR         DELIMITER STR
      ansi_printf("  %16s "          ANSI("- [", DIM) ""     // label
                  "" L4_MWORD_FMT "" ANSI("..", DIM) ""      // start
                  "" L4_MWORD_FMT "" ANSI(", ", DIM) ""      // end
                  "%7s"              ANSI("|", DIM) ""       // enabled 
                  "%-8s"             ANSI(",   ", DIM) ""    // type
                  "%cR%c%c"          ANSI("]@", DIM) ""      // rights
                  "%-4u"             ANSI(" - ", DIM) ""     // slot
                  "%s\n",                                    // status
                  region.label(),
                  region.start(),
                  region.end(),
                  attr.enabled() ? ANSI("yes", GREEN) : ANSI("no", RED),
                  (attr.type() == L4_snd_item::Memory_type::Normal())
                     ? "normal"
                     : ((attr.type() == L4_snd_item::Memory_type::Uncached())
                         ? "uncached" : "buffered"),
                  (attr.rights() & L4_fpage::Rights::U()) ? 'U' : '-',
                  (attr.rights() & L4_fpage::Rights::W()) ? 'W' : '-',
                  (attr.rights() & L4_fpage::Rights::X()) ? 'X' : '-',
                  i - 1,
                  _reserved[i] ? "reserved" : "used");
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
          first->start(left->start());
          updates.set_updated(index(left));
          erase(left);
        }
      else
        first->start(start);
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
