INTERFACE [arm && 32bit && mpu && arm_v8]:

#include <cxx/utility>

#include "mem.h"

struct Mpu_arm_el1
{
  static void init()
  {
    // Do not touch region 0. We might execute from it!
    for (auto i = regions(); i > 1; i--)
      {
        prselr(i-1);
        Mem::isb();
        prlar(0);
      }
  }

  static Mword regions()
  {
    Mword v;
    asm volatile("mrc p15, 0, %0, c0, c0, 4" : "=r"(v)); // MPUIR
    return v >> 8;
  }

  static void prbar(Mword v)
  { asm volatile("mcr p15, 0, %0, c6, c3, 0" : : "r"(v)); }

  static Mword prlar()
  {
    Mword v;
    asm volatile("mrc p15, 0, %0, c6, c3, 1" : "=r"(v));
    return v;
  }

  static void prlar(Mword v)
  { asm volatile("mcr p15, 0, %0, c6, c3, 1" : : "r"(v)); }

  static Mword prselr()
  { Mword v; asm volatile("mrc p15, 0, %0, c6, c2, 1" : "=r"(v)); return v; }

  static void prselr(Mword v)
  { asm volatile("mcr p15, 0, %0, c6, c2, 1" : : "r"(v)); }

  static void prenr_mask(Mword mask)
  {
    // This hurts. There is no PRENR register available...
    #define UPDATE(i) \
      do \
        { \
          if (!(mask & (1UL << (i)))) \
            Mpu_arm_el1::prlar<(i)>(0); \
        } \
      while (false)

    // Directly skip non-existing regions. We don't support more than 32 regions.
    static_assert(Mem_layout::Mpu_regions <= 32, "No more than 32 regions!");
    switch (Mpu_arm_el1::regions())
      {
        default:
        case 32: UPDATE(31); [[fallthrough]];
        case 31: UPDATE(30); [[fallthrough]];
        case 30: UPDATE(29); [[fallthrough]];
        case 29: UPDATE(28); [[fallthrough]];
        case 28: UPDATE(27); [[fallthrough]];
        case 27: UPDATE(26); [[fallthrough]];
        case 26: UPDATE(25); [[fallthrough]];
        case 25: UPDATE(24); [[fallthrough]];
        case 24: UPDATE(23); [[fallthrough]];
        case 23: UPDATE(22); [[fallthrough]];
        case 22: UPDATE(21); [[fallthrough]];
        case 21: UPDATE(20); [[fallthrough]];
        case 20: UPDATE(19); [[fallthrough]];
        case 19: UPDATE(18); [[fallthrough]];
        case 18: UPDATE(17); [[fallthrough]];
        case 17: UPDATE(16); [[fallthrough]];
        case 16: UPDATE(15); [[fallthrough]];
        case 15: UPDATE(14); [[fallthrough]];
        case 14: UPDATE(13); [[fallthrough]];
        case 13: UPDATE(12); [[fallthrough]];
        case 12: UPDATE(11); [[fallthrough]];
        case 11: UPDATE(10); [[fallthrough]];
        case 10: UPDATE(9);  [[fallthrough]];
        case  9: UPDATE(8);  [[fallthrough]];
        case  8: UPDATE(7);  [[fallthrough]];
        case  7: UPDATE(6);  [[fallthrough]];
        case  6: UPDATE(5);  [[fallthrough]];
        case  5: UPDATE(4);  [[fallthrough]];
        case  4: UPDATE(3);  [[fallthrough]];
        case  3: UPDATE(2);  [[fallthrough]];
        case  2: UPDATE(1);  [[fallthrough]];
        case  1: UPDATE(0);
          break;
      }

    #undef UPDATE
  }

  template<unsigned I>
  inline ALWAYS_INLINE
  static Mword prbar()
  {
    Mword v;
    asm volatile("mcr p15, %c1, %0, c6, c%c2, %c3"
                 : "=r"(v)
                 : "i"(0 + (I / 16)),
                   "i"(8 + (I / 2) % 8),
                   "i"(0 + (I % 2) * 4));
    return v;
  }

  template<unsigned I>
  inline ALWAYS_INLINE
  static void prxar(Mword b, Mword l)
  {
    if constexpr (I < Mem_layout::Mpu_regions)
      asm volatile("mcr p15, %c2, %0, c6, c%c3, %c4\n"
                   "mcr p15, %c2, %1, c6, c%c3, %c5"
                   : // no output
                   : "r"(b),
                     "r"(l),
                     "i"(0 + (I / 16)),
                     "i"(8 + (I / 2) % 8),
                     "i"(0 + (I % 2) * 4),
                     "i"(1 + (I % 2) * 4));
  }

  // We need dedicated PRLARx access for prenr_mask()

  template<unsigned I>
  inline ALWAYS_INLINE
  static void prlar(Mword v)
  {
    if constexpr (I < Mem_layout::Mpu_regions)
      asm volatile("mcr p15, %c1, %0, c6, c%c2, %c3"
                   : // no output
                   : "r"(v),
                     "i"(0 + (I / 16)),
                     "i"(8 + (I / 2) % 8),
                     "i"(1 + (I % 2) * 4));
  }

  template<unsigned I>
  inline ALWAYS_INLINE
  static Mword prlar()
  {
    Mword v;
    asm volatile("mcr p15, %c1, %0, c6, c%c2, %c3"
                 : "=r"(v)
                 : "i"(0 + (I / 16)),
                   "i"(8 + (I / 2) % 8),
                   "i"(1 + (I % 2) * 4));
    return v;
  }
};

struct Mpu_arm_el2
{
  static void init()
  {
    // Do not touch region 0. We might execute from it!
    asm volatile("mcr p15, 4, %0, c6, c1, 1" : : "r"(1)); // HPRENR
  }

  static Mword regions()
  {
    Mword v;
    asm volatile("mrc p15, 4, %0, c0, c0, 4" : "=r"(v)); // HMPUIR
    return v;
  }

  static void prbar(Mword v)
  { asm volatile("mcr p15, 4, %0, c6, c3, 0" : : "r"(v)); }

  static Mword prlar()
  {
    Mword v;
    asm volatile("mrc p15, 4, %0, c6, c3, 1" : "=r"(v));
    return v;
  }

  static void prlar(Mword v)
  { asm volatile("mcr p15, 4, %0, c6, c3, 1" : : "r"(v)); }

  static void prselr(Mword v)
  { asm volatile("mcr p15, 4, %0, c6, c2, 1" : : "r"(v)); }

  static Mword prenr()
  {
    Mword v;
    asm volatile("mrc p15, 4, %0, c6, c1, 1" : "=r"(v));
    return v;
  }

  static void prenr(Mword v)
  { asm volatile("mcr p15, 4, %0, c6, c1, 1" : : "r"(v)); }

  static void prenr_mask(Mword mask)
  { prenr(Mpu_arm_el2::prenr() & mask); }


  template<unsigned I>
  inline ALWAYS_INLINE
  static void prxar(Mword b, Mword l)
  {
    if constexpr (I < Mem_layout::Mpu_regions)
      asm volatile("mcr p15, %c2, %0, c6, c%c3, %c4\n"
                   "mcr p15, %c2, %1, c6, c%c3, %c5"
                   : // no output
                   : "r"(b),
                     "r"(l),
                     "i"(4 + (I / 16)),
                     "i"(8 + (I / 2) % 8),
                     "i"(0 + (I % 2) * 4),
                     "i"(1 + (I % 2) * 4));
  }

};

//------------------------------------------------------------------
INTERFACE [arm && 32bit && mpu && arm_v8 && !cpu_virt]:

typedef Mpu_arm_el1 Mpu_arm;

//------------------------------------------------------------------
IMPLEMENTATION [arm && 32bit && mpu && arm_v8 && !cpu_virt]:

PUBLIC static
bool
Mpu::enabled()
{
  unsigned sctlr;
  asm("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr)); // SCTLR
  return sctlr & 1U; // SCTLR.M
}

IMPLEMENT static
void Mpu::init()
{
  Mpu_arm::init();
  asm volatile ("mcr p15, 0, %0, c10, c2, 0" : : "r"(Mpu::Mair0_bits));
  asm volatile ("mcr p15, 0, %0, c10, c2, 1" : : "r"(Mpu::Mair1_bits));
}

//------------------------------------------------------------------
INTERFACE [arm && 32bit && mpu && arm_v8 && cpu_virt]:

typedef Mpu_arm_el2 Mpu_arm;

//------------------------------------------------------------------
IMPLEMENTATION [arm && 32bit && mpu && arm_v8 && cpu_virt]:

PUBLIC static
bool
Mpu::enabled()
{
  unsigned hsctlr;
  asm("mrc p15, 4, %0, c1, c0, 0" : "=r"(hsctlr)); // HSCTLR
  return hsctlr & 1U; // HSCTLR.M
}

IMPLEMENT static
void Mpu::init()
{
  Mpu_arm::init();
  asm volatile ("mcr p15, 4, %0, c10, c2, 0" : : "r"(Mpu::Mair0_bits));
  asm volatile ("mcr p15, 4, %0, c10, c2, 1" : : "r"(Mpu::Mair1_bits));
}

//------------------------------------------------------------------
INTERFACE [arm && 32bit && mpu && arm_v8]:

EXTENSION struct Mpu_region
{
public:
  Mword prbar, prlar;

  struct Prot {
    enum : Mword {
      None = 0,
      NX   = 1UL << 0,
      EL0  = 1UL << 1,
      RO   = 1UL << 2,
    };
  };

  struct Attr {
    enum : Mword {
      Mask      = 7UL << 1,
      Normal    = 2UL << 1,
      Device    = 0UL << 1,
      Buffered  = 1UL << 1,
    };
  };

  enum : Mword { Disabled = 0, Enabled = 1 };
};


EXTENSION class Mpu
{
public:
  enum : Mword {
    /**
     * Memory Attribute Indirection (MAIR0)
     * Attr0: Device-nGnRnE memory
     * Attr1: Normal memory, Inner/Outer Non-cacheable
     * Attr2: Normal memory, Read-Allocate, no Write-Allocate,
     *        Inner/Outer Write-Through Cacheable (Non-transient)
     * Attr3: Device-nGnRnE memory (unused)
     */
    Mair0_bits = 0x00aa4400,
    /**
     * Memory Attribute Indirection (MAIR1)
     * Attr4..Attr7: Device-nGnRnE memory (unused)
     */
    Mair1_bits = 0,
  };
};

//------------------------------------------------------------------
IMPLEMENTATION [arm && 32bit && mpu && arm_v8]:

IMPLEMENT constexpr
Mpu_region::Mpu_region()
: prbar(~0x3fUL), prlar(0)
{}

IMPLEMENT inline
Mpu_region::Mpu_region(Mword start, Mword end, Mpu_region_attr a)
: prbar(start & ~0x3fUL), prlar(end & ~0x3fUL)
{ attr(a); }

IMPLEMENT constexpr
Mword
Mpu_region::start() const
{ return prbar & ~0x3fUL; }

IMPLEMENT constexpr
Mword
Mpu_region::end() const
{ return prlar |  0x3fUL; }

IMPLEMENT constexpr
Mpu_region_attr
Mpu_region::attr() const
{
  return Mpu_region_attr::make_attr(
            L4_fpage::Rights::R()
                | ((prbar & Prot::EL0) ? L4_fpage::Rights::U()
                                       : L4_fpage::Rights())
                | ((prbar & Prot::RO) ? L4_fpage::Rights()
                                      : L4_fpage::Rights::W())
                | ((prbar & Prot::NX) ? L4_fpage::Rights()
                                      : L4_fpage::Rights::X()),
            ((prlar & Attr::Mask) == Attr::Normal)
             ? L4_snd_item::Memory_type::Normal()
             : (((prlar & Attr::Mask) == Attr::Device)
                ? L4_snd_item::Memory_type::Uncached()
                : L4_snd_item::Memory_type::Buffered()),
            prlar & Enabled);
}

IMPLEMENT inline
void
Mpu_region::start(Mword start)
{
  prbar = (prbar & 0x3fUL) | (start & ~0x3fUL);
}

IMPLEMENT inline
void
Mpu_region::end(Mword end)
{
  prlar = (prlar & 0x3fUL) | (end & ~0x3fUL);
}

IMPLEMENT inline
void
Mpu_region::attr(Mpu_region_attr attr)
{
  prbar = (prbar & ~0x3fUL)
          | ((attr.rights() & L4_fpage::Rights::U()) ? Prot::EL0 : Prot::None)
          | ((attr.rights() & L4_fpage::Rights::W()) ? Prot::None : Prot::RO)
          | ((attr.rights() & L4_fpage::Rights::X()) ? Prot::None : Prot::NX);
  prlar = (prlar & ~0x3fUL)
          | ((attr.type() == L4_snd_item::Memory_type::Uncached())
              ? Attr::Device
              : ((attr.type() == L4_snd_item::Memory_type::Buffered())
                  ? Attr::Buffered
                  : Attr::Normal))
          | (attr.enabled() ? Enabled : Disabled);
}

IMPLEMENT inline
void
Mpu_region::disable()
{
  prlar &= ~Enabled;
}


IMPLEMENT static inline
unsigned Mpu::regions()
{
  return Mpu_arm::regions();
}

IMPLEMENT static inline
void
Mpu::sync(Mpu_regions const &regions, Mpu_regions_mask const &touched,
          bool inplace)
{
  unsigned i = 0;
  while (i < touched.size() && (i = touched.ffs(i)))
    {
      Mpu_arm::prselr(i - 1);
      Mem::isb();
      if (!inplace && (Mpu_arm::prlar() & Mpu_region::Enabled))
        {
          // Always disable first! Otherwise a colliding region might
          // exist briefly after writing prbar!
          Mpu_arm::prlar(0);
          Mem::isb();
        }

      Mpu_arm::prbar(regions[i - 1].prbar);
      Mpu_arm::prlar(regions[i - 1].prlar);
    }
}

template<int I = Mem_layout::Mpu_regions>
void
write_to_hw_impl2(Mpu_regions const& regions)
{
  if constexpr (I > 2) {
    write_to_hw_impl2<I - 1>(regions);
    Mpu_arm::prxar<I>(regions[I].prbar, regions[I].prlar);
  }
}

template<size_t... I>
void
write_to_hw_impl(Mpu_regions const& regions, size_t idx, cxx::index_sequence<I...>) {
  using Fn = void (*)(Mpu_regions const&);
  static constexpr Fn table[] = { &write_to_hw_impl2<I>... };

  table[idx - 1](regions);
}

void write_to_hw(Mpu_regions const& regions, size_t cap) {
    write_to_hw_impl(regions, cap, cxx::make_index_sequence<Mem_layout::Mpu_regions>{});
}

IMPLEMENT static inline
void
Mpu::update(Mpu_regions const &regions)
{
  Mpu_regions_mask const &reserved = regions.reserved();

  // Disable regions that we're updating. Otherwise there is the possiblity to
  // have an invalid, colliding region when prbar is updated and the current
  // prlar of the updated region is still enabled.
  Mpu_arm::prenr_mask(*reserved.raw());
  Mem::isb();

  static_assert(reserved.size() <= 32,
                "HPRENR register only covers <= 32 regions!");

  write_to_hw(regions, regions.size());

  // Theoretically, because only user space regions are reconfigured, the ERET
  // on the kernel exit should be sufficient. But there we read/modify/write
  // PRENR. Hence, all region updates must be already committed to not read
  // stale data through PRENR.
  Mem::isb();
}
