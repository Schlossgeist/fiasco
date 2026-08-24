INTERFACE [mpu]:

#include <cxx/avl_tree>
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

private:
  bool _pinned = false;
  bool _ku_mem = false;
};

INTERFACE [mpu && mpultiplex]:

EXTENSION struct Mpu_region_base
{
public:
  constexpr bool is_active() const
  { return slot() >= 0 ;}

  constexpr int slot() const
  { return _physical_slot; }

  inline void slot(int slot)
  { _physical_slot = slot; }

private:
  int _physical_slot = -1;
};

INTERFACE [mpu && mpultiplex && mpultiplex_debug_labels]:

EXTENSION struct Mpu_region_base
{
public:
  using Label_buffer = char[16];

  constexpr Label_buffer const& label() const { return _label; }
  inline void label(const char *label)
  {
    const int buffer_end = sizeof(Label_buffer) - 1;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
    __builtin_strncpy(_label, label, buffer_end);
    _label[buffer_end] = '\0';
#pragma GCC diagnostic pop
  }

private:
  Label_buffer _label = {0};
};

INTERFACE [mpu]:

/**
 * A single MPU region.
 *
 * Regions are organized as a sorted, double linked, cyclic list.
 */
struct Mpu_region
: public Mpu_region_base,
  public cxx::Avl_tree_node
{
  using Mpu_region_base::Mpu_region_base;

  // avl tree stuff
  using Key_type = Mword;
  static Key_type key_of(Mpu_region const *r)
  { return r->start(); }

  struct lt_avl
  {
    bool operator()(Key_type lhs, Key_type rhs)
    { return lhs < rhs; }
  };
};

struct Mpu_allocator
{
  static void *alloc(size_t size);
  static void free(size_t size, void *obj);
};

using Bitmap_type = Dynamic_bitmap<Config::Mpultiplex_block_size, Mpu_allocator>;

class Mpu_regions;
class Mpu_regions_mask;
struct Mpu_internal_state;

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
   * Dump MPU physical state.
   */
  static void dump_physical();

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

// === SWAPPING ===============================================================

  using Virt_slot = int;
  using Phys_slot = unsigned;

  /**
   * Implements the strategy for finding a slot for swapping.
   *
   * \return The number of the virtual MPU region slot that has been chosen
   *         for replacement.
   */
  static Virt_slot find_slot_for_swap();

  /**
   * Swaps in a slot at the place of the victim slot and also updates
   * additional state like the Mpu_regions_masks.
   */
  static void swap_slots(Virt_slot victim_slot, Virt_slot swap_slot);

  /**
   * Does the actual swap. No other state is touched.
   */
  static void swap(Phys_slot victim_slot, Mpu_region const &region,
                   bool inplace = false);

  static Mpu_internal_state& state();
  static Mpu_internal_state& state(Cpu_number cpu);
  static Per_cpu<Mpu_internal_state> _internal_state;
};

IMPLEMENTATION [mpu]:

#include <cstdlib>

#include "kmem_alloc.h"
#include "ram_quota.h"

DEFINE_PER_CPU Per_cpu<Mpu_internal_state> Mpu::_internal_state;

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
  (void) cpu;
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
  return Mpu::state().last_cached_snapshot != nullptr;
}

IMPLEMENT static inline
void Mpu::expand_virtual_regions(size_t new_size)
{
  INFO("[CPU%u] MPUltiplex regions cache size extended to %zu\n",
       cxx::int_value<Cpu_number>(current_cpu()), new_size);
  Mpu::dump();
}

IMPLEMENT static inline
bool Mpu::check_and_handle_multiplex_fault(Mword address)
{
  precondition(Mpu::state().last_cached_snapshot != nullptr);
  auto const& curr_state = *(Mpu::state().last_cached_snapshot);

//  printf("----------------------------------------------------------------------\n");
//  printf("Checking MPUltiplex fault at " L4_MWORD_FMT "...\n", address);
//  Mpu::dump();

  int swap_in_slot = -1;
  auto const* found_region = curr_state.lookup(address);
  if (found_region)
    {
      int const found_slot = curr_state.index(found_region);

//      printf("MPU FAULT: address=%08lx logical=%d active=%d slot=%d\n",
//                       address,
//                       found_slot,
//                       curr_state.active_regions()[found_slot],
//                       found_region->slot());

      if (!curr_state.active_regions()[found_slot])
        {
          // region is cached, but inactive -> swap it in
          swap_in_slot = found_slot;
        }
      else
        {
          return false;
        }
    }
  else
    // not a multiplex fault; bail out
    return false;

  int swap_out_slot = Mpu::find_slot_for_swap();
  Mpu::swap_slots(swap_out_slot, swap_in_slot);
//  Mpu::dump();

  return true;
}

IMPLEMENT static inline NEEDS[<cstdlib>]
Mpu::Virt_slot Mpu::find_slot_for_swap()
{
  precondition(Mpu::state().last_cached_snapshot != nullptr);
  auto const& curr_state = *(Mpu::state().last_cached_snapshot);

  Mpu_regions_mask permanently_reserved;
  permanently_reserved.set_first_bits(4);
  Mpu_regions_mask const available_regions =
    curr_state.available_regions() & ~permanently_reserved;
  int const max_regions = Mpu::regions();

  invariant(available_regions.popcount() > 0);

  auto wrap_around = [max_regions](int start, int offset) -> int
    {
      int const result = (start + offset) % max_regions;
      return result < 0 ? result + max_regions
                        : result;
    };

  int const start_slot = rand() % max_regions;
  for (int offset = 0, target_slot; offset < max_regions; ++offset)
    {
      target_slot = wrap_around(start_slot, +offset);
      if (available_regions[target_slot]) return target_slot;

      target_slot = wrap_around(start_slot, -offset);
      if (available_regions[target_slot]) return target_slot;
    }

  panic("Could not find a slot for swapping!");
}

IMPLEMENTATION [mpu]:

IMPLEMENT static inline
void Mpu::swap_slots(Virt_slot victim_slot, Virt_slot swap_slot)
{
  precondition(Mpu::state().last_cached_snapshot != nullptr);
  auto &curr_state = *(Mpu::state().last_cached_snapshot);

//  printf("--- START ---\n");
//  Mpu::dump();

  precondition( curr_state.active_regions()[victim_slot]);
  precondition(!curr_state.active_regions()[swap_slot]);
  precondition(0 <= curr_state[victim_slot].slot());
  precondition(0 >  curr_state[swap_slot].slot());

  auto &swap_region   = curr_state[swap_slot];
  auto &victim_region = curr_state[victim_slot];

  const int hardware_slot = victim_region.slot();
  invariant(3 < hardware_slot);
  invariant(hardware_slot < static_cast<int>(Mpu::hardware_regions()));

  Mpu::swap(hardware_slot, swap_region);

//  printf("SWAPPED OUT SLOT %2i FOR SLOT %2i VIA HW SLOT %2i\n",
//         victim_slot, swap_slot, hardware_slot);

  victim_region.slot(-1);
  swap_region.slot(hardware_slot);

//  Mpu::dump();
//  printf("---- END ----\n");

  postcondition(!curr_state.active_regions()[victim_slot]);
  postcondition( curr_state.active_regions()[swap_slot]);
  postcondition(swap_region.slot() == hardware_slot);
}

IMPLEMENT static inline
Mpu_internal_state& Mpu::state()
{
  return _internal_state.current();
}

IMPLEMENT static inline
Mpu_internal_state& Mpu::state(Cpu_number cpu)
{
  return _internal_state.cpu(cpu);
}

IMPLEMENT static inline
Unsigned32 Mpu::get_current_ku_mem()
{
  precondition(Mpu::state().last_cached_snapshot != nullptr);
  auto const& curr_state = *(Mpu::state().last_cached_snapshot);

  precondition(curr_state.active_regions().popcount() < 32);

  return curr_state.current_ku_mem();
}

IMPLEMENT static inline
unsigned Mpu::regions()
{
  return mpultiplex_enabled()
    ? Mpu::state().last_cached_snapshot->size() : Config::Mpultiplex_block_size;
}

#include "ansi.h"

IMPLEMENT static
void Mpu::dump()
{
  precondition(Mpu::state().last_cached_snapshot != nullptr);
  auto const& curr_state = *(Mpu::state().last_cached_snapshot);

  printf("[%p] Cached + active MPU regions:\n", &curr_state);
  int pad = 2 * sizeof(Mword);
  printf(ANSI("  %16s - [%*s..%*s, enabled|mem type, rights]@slot[in hw]"
         " - multiplex state\n", BOLD),
         "label", -pad, "start", pad, "end");

  for (unsigned i = 0; i < curr_state.size(); ++i)
    {
      auto const& region = curr_state[i];
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
#if defined(CONFIG_MPULTIPLEX_DEBUG_LABELS)
                  region.label(),
#else
                  "not configured",
#endif
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
                  curr_state.active_regions()[i] ? "active" : "cached",
                  curr_state.pinned_regions()[i] ? "pinned" : "");
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

static inline void stop_here()
{
  printf("STOP HERE\n");
}

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
  using Region_tree = cxx::Avl_tree<Mpu_region, Mpu_region, Mpu_region::lt_avl>;

public:
  size_t SYNCS = 0;
  size_t UPDATES = 0;
  /**
   * Construct new MPU region list.
   *
   * \param reserved  Map of regions that are not allocatable.
   */
  explicit Mpu_regions(Mpu_regions_mask const &reserved)
  : Mpu_region_block_storage(Config::Mpultiplex_block_size), _reserved(reserved), _current_snapshot(*this)
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
  : Mpu_region_block_storage(other.size()), _reserved(other._reserved), _current_snapshot(*this)
  {
    _reserved |= other._used_mask;
    for (Mpu_region const &i : other._used_tree)
      {
        unsigned idx = other.index(&i);
        Mpu_region &r = (*this)[idx];
        r.start(i.start());
        r.end(i.end());
        r.attr(i.attr());
#if defined(CONFIG_MPULTIPLEX_DEBUG_LABELS)
        r.label(i.label());
#endif
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


  class Snapshot;
  /**
   * Wrapper around `Mpu_region` objects that allows read-only access to
   * prbar/prlar and read/write access to age/slot.
   *
   * Also synchronizes important state kept in bitmasks when the physical
   * location of a snapshotted region is changed via `slot(s)`.
   */
  struct Snapshot_region : public Mpu_region
  {
    Snapshot_region(Mpu_region const& region, Snapshot *snapshot)
    : _snapshot(snapshot)
    {
      start(region.start());
      end(region.end());
      attr(region.attr());
#if defined(CONFIG_MPULTIPLEX_DEBUG_LABELS)
      label(region.label());
#endif
    }

    Snapshot_region(Snapshot_region const &) = delete;
    Snapshot_region &operator=(Snapshot_region const &) = delete;

    using Mpu_region::Mpu_region;

    Mword prbar() const { return Mpu_region::prbar; }
    Mword prlar() const { return Mpu_region::prlar; }
    int   slot () const { return Mpu_region::slot(); }
    void  slot (int s)
    {
      precondition(s < 32);

      if (_snapshot)
        {
          unsigned const idx = _snapshot->index(this);
          bool     const activated = s >= 0;
          _snapshot->_active_regions.bit(idx, activated);
          _snapshot->_pinned_regions.bit(idx, attr().pinned());

          if (attr().ku_mem())
            {
              _snapshot->_current_ku_mem &= ~(1 << slot());

              if (activated)
                _snapshot->_current_ku_mem |=   1 << s;
            }
        }

      Mpu_region::slot(s);
    }

  private:
    Snapshot *const _snapshot = nullptr;
  };

  /**
   * Snapshot of the state of a `Mpu_regions` object at a specific point in time.
   *
   * This snapshot is referenced by the MPUltiplex subsystem when making
   * swapping decisions.
   */
  class Snapshot
  : public IMpu_region_base_container,
    public Mpu_region_block_storage<Snapshot_region, Mpu_allocator>
  {
    using Snapshot_region_tree = cxx::Avl_tree<Snapshot_region, Snapshot_region, Snapshot_region::lt_avl>;

  public:
    explicit Snapshot(Mpu_regions const &regions)
    : Mpu_region_block_storage(regions.size())
    {
      for (unsigned i = 0; i < regions.size(); ++i)
        {
          Snapshot_region &r = (*this)[i];
          new (&r) Snapshot_region(regions[i], this);

          _region_tree.insert(&r);
        }
    }

    Snapshot& operator=(Mpu_regions const &regions)
    {
      if (regions.size() > size())
        {
            size_t new_size = reserve(regions.size());
            Mpu_regions_mask m(new_size);
            _active_regions |= m;
            _pinned_regions |= m;
        }

      clear();
      for (unsigned i = 0; i < regions.size(); ++i)
        {
          Snapshot_region &r = (*this)[i];
          int const old_slot = r.slot();
          new (&r) Snapshot_region(regions[i], this);
          r.slot(old_slot);

          _region_tree.insert(&r);
        }
      return *this;
    }

    /**
      * Returns the active region that covers the given address or returns
      * a nullptr if there is no such region.
      */
    Snapshot_region const* lookup(Mword addr) const
    {
      if (auto n = _region_tree.last_less_equal_node(addr);
          n && n->contains(addr))
        {
          if (n->attr().enabled())
            return n;
          else
            {
              // there might be an enabled region that starts before the one
              // found above
              auto iter = _region_tree.find(Mpu_region::key_of(n));
              while (iter != _region_tree.begin() && (--iter)->contains(addr))
                {
                  if (iter->attr().enabled())
                    return iter.operator->();
                }
            }
        }

      return nullptr;
    }

    void clear()
    {
      _region_tree.remove_all([](Mpu_region *){});
      Mpu_region_block_storage::clear();
    }

    Unsigned32 current_ku_mem() const
    { return _current_ku_mem; }

    Mpu_regions_mask const &active_regions() const
    { return _active_regions; }
    Mpu_regions_mask const &pinned_regions() const
    { return _pinned_regions; }
    Mpu_regions_mask available_regions() const
    { return _active_regions & ~_pinned_regions; }

    friend Snapshot_region;
    // IMpu_region_base_container interface
    Mpu_region_base const &at(unsigned i) const override
    { return (*this)[i]; }

  bool verify_hardware_consistency() const
  {
    unsigned i = 0;
    while (i < _active_regions.size() && (i = _active_regions.ffs(i)))
      {
        auto const& i_slot = (*this)[i - 1].slot();
        if (i_slot == -1) continue;
        unsigned j = 0;
        while (j < _active_regions.size() && (j = _active_regions.ffs(j)))
          {
            auto const& j_slot = (*this)[j - 1].slot();
            if (j_slot == -1) continue;
            if (i == j) continue;
            if (i_slot == j_slot)
              {
                Mpu::dump();
                stop_here();
              }
            invariant(i_slot != j_slot);
          }
      }
    return true;
  }

  private:
    Snapshot_region_tree _region_tree;
    // bitmask of physical MPU slots that currently hold ku-mem regions
    Unsigned32 _current_ku_mem = 0;
    // bitmask of cached regions that are currently active in the physical MPU
    Mpu_regions_mask _active_regions;
    // bitmask of cached regions that are not supposed to be swapped out
    Mpu_regions_mask _pinned_regions;
  };

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
    auto iter = _used_tree.find(r->start());
    return deref_iter(++iter);
  }

  Mpu_region *prev(Mpu_region *r) const
  {
    auto iter = _used_tree.find(r->start());
    return iter != _used_tree.begin() ? deref_iter(--iter) : nullptr;
  }

  Mpu_region *erase(Mpu_region *r)
  {
    invalidate();

    _used_mask.clear_bit(index(r));
    r->disable();
    return _used_tree.erase(r->start());
  }

  bool insert(Mpu_region *r)
  {
    invalidate();

    _used_mask.set_bit(index(r));
    auto [node, was_not_in_tree_before] = _used_tree.insert(r);
    return was_not_in_tree_before;
  }

  void reinsert(Mpu_region *r, Mword new_start)
  {
    invalidate();

    _used_tree.erase(r->start());
    r->start(new_start);
    auto [node, was_not_in_tree_before] = _used_tree.insert(r);
    // if this is hit, the new key of the reinserted Mpu_region
    // was already present in the tree
    assert(was_not_in_tree_before);
    (void) node;
  }

  size_t reserve(size_t new_size)
  {
    new_size = Mpu_region_block_storage::reserve(new_size);
    _current_snapshot.reserve(new_size);

    Mpu_regions_mask m(new_size);
    _reserved |= m;
    _used_mask |= m;

    INFO("[%p] MPU regions size extended to %zu\n", this, new_size);

    return new_size;
  }

  Mpu_regions_mask _reserved;
  Mpu_regions_mask _used_mask;  ///< Bit mask of occupied regions
  Region_tree _used_tree;       ///< Sorted tree (by address) of used regions
  Snapshot _current_snapshot;   ///< Current snapshot used for multiplexing
  bool _dirty;                  ///<
};

/**
 * Data structure that bundles the internal state used by the MPUltiplex
 * subsystem. Accessible via Mpu::state.
 */
struct Mpu_internal_state
{
  // pointer to the snapshot of most recently updated-in Mpu_regions object
  Mpu_regions::Snapshot *last_cached_snapshot;
};

//---------------------------------------------------------------------------
IMPLEMENTATION [mpu && mpultiplex]:

/**
 * Take a snapshot of the current state.
 *
 * After this operation, the current structure becomes immutable and any
 * changes to this `Mpu_regions` object are recorded in a list of deltas
 * that can be merged into a new snapshot with the `commit_delta()` method.
 */
PUBLIC inline
Mpu_regions::Snapshot &
Mpu_regions::take_snapshot(Mpu_regions_mask const* shallow_update = nullptr)
{
  if (shallow_update) [[unlikely]]
    {
      // printf("SHALLOW UPDATE\n");
      unsigned i = 0;
      while (i < shallow_update->size() && (i = shallow_update->ffs(i)))
        {
          int const logical_slot  = i - 1;
          int       physical_slot = -1;

          Mpu_region const& region = (*this)[logical_slot];
          Snapshot_region & snapshot_region = _current_snapshot[logical_slot];

          if (region.attr().pinned())
            {
              // printf("PINNED\n");
              physical_slot = snapshot_region.slot();
            }

          new (&snapshot_region) Snapshot_region(region, &_current_snapshot);
          snapshot_region.slot(physical_slot);
        }

      return _current_snapshot;
    }

  if (!is_dirty()) [[likely]]
    return _current_snapshot; // no new changes; current snapshot still valid

  _dirty = false;
  return _current_snapshot = *this;
}

/**
 * Whether there have been any modifications since the last snapshot has been
 * taken.
 */
PRIVATE
bool
Mpu_regions::is_dirty() const
{
  return _dirty;
}

/**
 * Explicitly invalidate the current snapshot.
 * This has to be used everywhere state is modified without the use of
 * insert(), reinsert() or erase().
 */
PRIVATE
void
Mpu_regions::invalidate()
{
  _dirty = true;
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
PUBLIC inline NEEDS[Mpu_regions::invalidate, Mpu_regions::extend, Mpu_regions::find_free]
Mpu_regions_update
Mpu_regions::add(Mword start, Mword end, Mpu_region_attr attr, bool join = true,
                 int slot = -1, const char *label = "undefined")
{
  // silence "unused variable" warning with CONFIG_MPULTIPLEX_DEBUG_LABELS off
  (void) label;

  // if (start == 0x20034000)
  //   stop_here();

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
      // invalidate explicitly because `left` isn't touched otherwise
      invalidate();
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
    {
#if defined(CONFIG_MPULTIPLEX_DEBUG_LABELS)
      r->label(label);
#endif
      return updates;
    }

  // Could not join an existing region. We need to allocate a new slot.
  r = find_free(slot);
  if (!r)
    {
      auto new_size = reserve(size() * 2);
      (void) new_size;
    }
  // Search again because number of regions has increased.
  r = find_free(slot);
  assert(r);

  // no reinsertion needed, because 'r' was free, therefore unused
  r->start(start);
  r->end(end);
  r->attr(attr);

  // insert into tree
  insert(r);

  updates.set_updated(index(r));
#if defined(CONFIG_MPULTIPLEX_DEBUG_LABELS)
  r->label(label);
#endif
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
PUBLIC inline NEEDS[Mpu_regions::invalidate, Mpu_regions::find_free]
Mpu_regions_update
Mpu_regions::del(Mword start, Mword end, Mpu_region_attr *attr = nullptr)
{
  // if (start == 0x20034000)
  //   stop_here();
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
          if (!r)
            {
              auto new_size = reserve(size() * 2);
              (void) new_size;
            }
          // Search again because number of regions has increased.
          r = find_free();
          assert(r);

          updates.set_updated(index(r));

          r->attr(i->attr());
          r->start(end + 1U);
          r->end(i->end());

          i->end(start - 1U);

          // no reinsertion needed, because 'r' was free, therefore unused
          insert(r);
          break;
        }
      else if (i->start() < start)
        {
          // Upper part of region overlaps with unmap range.
          i->end(start - 1U);
          // invalidate explicitly because `i` isn't touched otherwise
          invalidate();
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
  for (Mpu_region const &i : _used_tree)
    {
      if (addr < i.start())
        return &i;
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
  printf(ANSI("  %16s - [%*s..%*s, enabled|mem type, rights]@slot"
         " - status\n", BOLD),
         "label", -pad, "start", pad, "end");

  for (unsigned i = 0; i < size(); ++i)
    {
      Mpu_region const &region = (*this)[i];
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
#if defined(CONFIG_MPULTIPLEX_DEBUG_LABELS)
                  region.label(),
#else
                  "not configured",
#endif
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
                  i,
                  _reserved[i]
                      ? "reserved"
                      : _used_mask[i]
                          ? "used" : "");
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
PRIVATE inline NEEDS[Mpu_regions::invalidate]
Mpu_regions_update
Mpu_regions::extend(Mpu_region *first, Mpu_region_attr attr, Mword start,
                    Mword end)
{
  // if (first->start() == 0x20034000)
  //   stop_here();
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
          erase(left);
          reinsert(first, left->start());
          updates.set_updated(index(left));
        }
      else
        {
          reinsert(first, start);
        }
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
        {
          first->end(end);
          // invalidate explicitly because `first` isn't touched otherwise
          invalidate();
        }
    }

  return updates;
}
