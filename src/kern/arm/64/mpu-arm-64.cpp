INTERFACE [arm && 64bit && mpu && arm_v8]:

#include "mem.h"

struct Mpu_arm_el1
{
  static void init()
  {
    // Do not touch region 0. We might execute from it!
    for (auto i = hardware_regions(); i > 1; i--)
      {
        prselr(i-1);
        prlar(0);
      }
  }

  static Mword hardware_regions()
  {
    Mword v;
    asm("mrs %0, S3_0_c0_c0_4" : "=r"(v)); // MPUIR_EL1
    return v;
  }

  static void prbar(Mword v)
  { asm volatile("msr S3_0_c6_c8_0, %0" : : "r"(v)); } // PRBAR_EL1

  static Mword prlar()
  {
    Mword v;
    asm volatile("mrs %0, S3_0_c6_c8_1" : "=r"(v)); // PRLAR_EL1
    return v;
  }

  static void prlar(Mword v)
  { asm volatile("msr S3_0_c6_c8_1, %0" : : "r"(v)); } // PRLAR_EL1

  static Mword prselr() // PRSELR_EL1
  {
    Mword v;
    asm volatile("mrs %0, S3_0_c6_c2_1" : "=r"(v));
    return v;
  }

  static void prselr(Mword v) // PRSELR_EL1
  {
    asm volatile(
      "msr S3_0_c6_c2_1, %0  \n"
      "isb                   \n"
      : : "r"(v));
  }

  static Mword prenr()
  {
    Mword v;
    asm volatile("mrs %0, S3_0_c6_c1_1" : "=r"(v)); // PRENR_EL1
    return v;
  }

  static void prenr(Mword v)
  { asm volatile("msr S3_0_c6_c1_1, %0" : : "r"(v)); } // PRENR_EL1

  static Mword prbar0()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c8_0" : "=r"(v)); return v; }
  static Mword prbar1()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c8_4" : "=r"(v)); return v; }
  static Mword prbar2()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c9_0" : "=r"(v)); return v; }
  static Mword prbar3()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c9_4" : "=r"(v)); return v; }
  static Mword prbar4()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c10_0" : "=r"(v)); return v; }
  static Mword prbar5()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c10_4" : "=r"(v)); return v; }
  static Mword prbar6()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c11_0" : "=r"(v)); return v; }
  static Mword prbar7()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c11_4" : "=r"(v)); return v; }
  static Mword prbar8()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c12_0" : "=r"(v)); return v; }
  static Mword prbar9()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c12_4" : "=r"(v)); return v; }
  static Mword prbar10()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c13_0" : "=r"(v)); return v; }
  static Mword prbar11()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c13_4" : "=r"(v)); return v; }
  static Mword prbar12()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c14_0" : "=r"(v)); return v; }
  static Mword prbar13()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c14_4" : "=r"(v)); return v; }
  static Mword prbar14()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c15_0" : "=r"(v)); return v; }
  static Mword prbar15()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c15_4" : "=r"(v)); return v; }

  static Mword prlar0()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c8_1" : "=r"(v)); return v; }
  static Mword prlar1()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c8_5" : "=r"(v)); return v; }
  static Mword prlar2()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c9_1" : "=r"(v)); return v; }
  static Mword prlar3()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c9_5" : "=r"(v)); return v; }
  static Mword prlar4()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c10_1" : "=r"(v)); return v; }
  static Mword prlar5()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c10_5" : "=r"(v)); return v; }
  static Mword prlar6()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c11_1" : "=r"(v)); return v; }
  static Mword prlar7()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c11_5" : "=r"(v)); return v; }
  static Mword prlar8()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c12_1" : "=r"(v)); return v; }
  static Mword prlar9()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c12_5" : "=r"(v)); return v; }
  static Mword prlar10()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c13_1" : "=r"(v)); return v; }
  static Mword prlar11()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c13_5" : "=r"(v)); return v; }
  static Mword prlar12()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c14_1" : "=r"(v)); return v; }
  static Mword prlar13()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c14_5" : "=r"(v)); return v; }
  static Mword prlar14()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c15_1" : "=r"(v)); return v; }
  static Mword prlar15()
  { Mword v; asm volatile("mrs %0, S3_0_c6_c15_5" : "=r"(v)); return v; }

  // Attention: there is no PRBAR0_EL1/PRLAR0_EL1 register. They alias with
  // PRBAR_EL1 and PRLAR_EL1!

  static void prxar0(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c8_0,  %0\n"
                 "msr S3_0_c6_c8_1,  %1" : : "r"(b), "r"(l));
  }
  static void prxar1(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c8_4,  %0\n"
                 "msr S3_0_c6_c8_5,  %1" : : "r"(b), "r"(l));
  }
  static void prxar2(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c9_0,  %0\n"
                 "msr S3_0_c6_c9_1,  %1" : : "r"(b), "r"(l));
  }
  static void prxar3(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c9_4,  %0\n"
                 "msr S3_0_c6_c9_5,  %1" : : "r"(b), "r"(l));
  }
  static void prxar4(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c10_0, %0\n"
                 "msr S3_0_c6_c10_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar5(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c10_4, %0\n"
                 "msr S3_0_c6_c10_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar6(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c11_0, %0\n"
                 "msr S3_0_c6_c11_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar7(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c11_4, %0\n"
                 "msr S3_0_c6_c11_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar8(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c12_0, %0\n"
                 "msr S3_0_c6_c12_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar9(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c12_4, %0\n"
                 "msr S3_0_c6_c12_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar10(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c13_0, %0\n"
                 "msr S3_0_c6_c13_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar11(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c13_4, %0\n"
                 "msr S3_0_c6_c13_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar12(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c14_0, %0\n"
                 "msr S3_0_c6_c14_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar13(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c14_4, %0\n"
                 "msr S3_0_c6_c14_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar14(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c15_0, %0\n"
                 "msr S3_0_c6_c15_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar15(Mword b, Mword l)
  {
    asm volatile("msr S3_0_c6_c15_4, %0\n"
                 "msr S3_0_c6_c15_5, %1" : : "r"(b), "r"(l));
  }
};

struct Mpu_arm_el2
{
  static void init()
  {
    // Do not touch region 0. We might execute from it!
    for (auto i = hardware_regions(); i > 1; i--)
      {
        prselr(i-1);
        prlar(0);
      }
  }

  static Mword hardware_regions()
  {
    Mword v;
    asm("mrs %0, S3_4_c0_c0_4" : "=r"(v)); // MPUIR_EL2
    return v;
  }

  static void prbar(Mword v)
  { asm volatile("msr S3_4_c6_c8_0, %0" : : "r"(v)); } // PRBAR_EL2

  static Mword prlar()
  {
    Mword v;
    asm volatile("mrs %0, S3_4_c6_c8_1" : "=r"(v)); // PRLAR_EL2
    return v;
  }

  static void prlar(Mword v)
  { asm volatile("msr S3_4_c6_c8_1, %0" : : "r"(v)); } // PRLAR_EL2

  static void prselr(Mword v) // PRSELR_EL2
  {
    asm volatile(
      "msr S3_4_c6_c2_1, %0  \n"
      "isb                   \n"
      : : "r"(v));
  }

  static Mword prenr()
  {
    Mword v;
    asm volatile("mrs %0, S3_4_c6_c1_1" : "=r"(v)); // PRENR_EL2
    return v;
  }

  static void prenr(Mword v)
  { asm volatile("msr S3_4_c6_c1_1, %0" : : "r"(v)); } // PRENR_EL2

  // Attention: there is no PRBAR0_EL2/PRLAR0_EL2 register. They alias with
  // PRBAR_EL2 and PRLAR_EL2!

  static void prxar0(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c8_0,  %0\n"
                 "msr S3_4_c6_c8_1,  %1" : : "r"(b), "r"(l));
  }
  static void prxar1(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c8_4,  %0\n"
                 "msr S3_4_c6_c8_5,  %1" : : "r"(b), "r"(l));
  }
  static void prxar2(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c9_0,  %0\n"
                 "msr S3_4_c6_c9_1,  %1" : : "r"(b), "r"(l));
  }
  static void prxar3(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c9_4,  %0\n"
                 "msr S3_4_c6_c9_5,  %1" : : "r"(b), "r"(l));
  }
  static void prxar4(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c10_0, %0\n"
                 "msr S3_4_c6_c10_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar5(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c10_4, %0\n"
                 "msr S3_4_c6_c10_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar6(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c11_0, %0\n"
                 "msr S3_4_c6_c11_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar7(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c11_4, %0\n"
                 "msr S3_4_c6_c11_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar8(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c12_0, %0\n"
                 "msr S3_4_c6_c12_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar9(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c12_4, %0\n"
                 "msr S3_4_c6_c12_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar10(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c13_0, %0\n"
                 "msr S3_4_c6_c13_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar11(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c13_4, %0\n"
                 "msr S3_4_c6_c13_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar12(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c14_0, %0\n"
                 "msr S3_4_c6_c14_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar13(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c14_4, %0\n"
                 "msr S3_4_c6_c14_5, %1" : : "r"(b), "r"(l));
  }
  static void prxar14(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c15_0, %0\n"
                 "msr S3_4_c6_c15_1, %1" : : "r"(b), "r"(l));
  }
  static void prxar15(Mword b, Mword l)
  {
    asm volatile("msr S3_4_c6_c15_4, %0\n"
                 "msr S3_4_c6_c15_5, %1" : : "r"(b), "r"(l));
  }
};

//------------------------------------------------------------------
INTERFACE [arm && 64bit && mpu && arm_v8 && !cpu_virt]:

typedef Mpu_arm_el1 Mpu_arm;

//------------------------------------------------------------------
IMPLEMENTATION [arm && 64bit && mpu && arm_v8 && !cpu_virt]:

PUBLIC static
bool
Mpu::enabled()
{
  unsigned long control;
  asm ("mrs %0, SCTLR_EL1" : "=r" (control));
  return control & 1U; // SCTLR_EL1.M
}

IMPLEMENT static
void
Mpu::init()
{
  Mpu_arm::init();
  asm volatile ("msr MAIR_EL1, %0" : : "r"(Mpu::Mair_bits));
}

//------------------------------------------------------------------
INTERFACE [arm && 64bit && mpu && arm_v8 && cpu_virt]:

typedef Mpu_arm_el2 Mpu_arm;

//------------------------------------------------------------------
IMPLEMENTATION [arm && 64bit && mpu && arm_v8 && cpu_virt]:

PUBLIC static
bool
Mpu::enabled()
{
  unsigned long control;
  asm ("mrs %0, SCTLR_EL2" : "=r" (control));
  return control & 1U; // SCTLR_EL2.M
}

IMPLEMENT static
void
Mpu::init()
{
  Mpu_arm::init();
  asm volatile ("msr MAIR_EL2, %0" : : "r"(Mpu::Mair_bits));
}

//------------------------------------------------------------------
INTERFACE [arm && 64bit && mpu && arm_v8]:

#include "cpu_lock.h"

EXTENSION struct Mpu_region_base
{
public:
  Mword prbar, prlar;

  struct Prot {
    enum : Mword {
      None = 0,
      NX   = 1UL << 1,
      EL0  = 1UL << 2,
      RO   = 1UL << 3,
      ISH  = 3UL << 4,
    };
  };

  struct Attr {
    enum : Mword {
      // See Mair_bits bits below
      Mask      = 7UL << 1,
      Normal    = 0UL << 1,  // Normal Outer Non-cachable, Inner Write-Back
      Device    = 1UL << 1,  // Device nGnRnE
      Buffered  = 2UL << 1,  // Normal Outer+Inner Non-cacheable
    };
  };

  enum : Mword { Disabled = 0, Enabled = 1 };
};


EXTENSION class Mpu
{
public:
  enum : Mword {
    // 0 = Normal Outer+Inner Write-Back, R+W-Allocate
    // 1 = Device nGnRnE
    // 2 = Normal Outer+Inner Non-cacheable
    Mair_bits = 0x4400ffU,
  };
};

//------------------------------------------------------------------
IMPLEMENTATION [arm && 64bit && mpu && arm_v8]:

#include "minmax.h"

IMPLEMENT constexpr
Mpu_region_base::Mpu_region_base()
: prbar(~0x3fUL), prlar(0)
{}

IMPLEMENT inline
Mpu_region_base::Mpu_region_base(Mword start, Mword end, Mpu_region_attr a)
: prbar(start & ~0x3fUL), prlar(end & ~0x3fUL)
{
  attr(a);
  pinned = a.pinned();
  ku_mem = a.ku_mem();
}

IMPLEMENT constexpr
Mword
Mpu_region_base::start() const
{ return prbar & ~0x3fUL; }

IMPLEMENT constexpr
Mword
Mpu_region_base::end() const
{ return prlar |  0x3fUL; }

IMPLEMENT constexpr
Mpu_region_attr
Mpu_region_base::attr() const
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
            prlar & Enabled,
            pinned, ku_mem);
}

IMPLEMENT inline
void
Mpu_region_base::start(Mword start)
{
  prbar = (prbar & 0x3fUL) | (start & ~0x3fUL);
}

IMPLEMENT inline
void
Mpu_region_base::end(Mword end)
{
  prlar = (prlar & 0x3fUL) | (end & ~0x3fUL);
}

IMPLEMENT inline
void
Mpu_region_base::attr(Mpu_region_attr attr)
{
  prbar = (prbar & ~0x3fUL)
          | ((attr.rights() & L4_fpage::Rights::U()) ? Prot::EL0 : Prot::None)
          | ((attr.rights() & L4_fpage::Rights::W()) ? Prot::None : Prot::RO)
          | ((attr.rights() & L4_fpage::Rights::X()) ? Prot::None : Prot::NX)
          | Prot::ISH;
  prlar = (prlar & ~0x3fUL)
          | ((attr.type() == L4_snd_item::Memory_type::Uncached())
              ? Attr::Device
              : ((attr.type() == L4_snd_item::Memory_type::Buffered())
                  ? Attr::Buffered
                  : Attr::Normal))
          | (attr.enabled() ? Enabled : Disabled);
  pinned = attr.pinned();
  ku_mem = attr.ku_mem();
}

IMPLEMENT inline
void
Mpu_region_base::disable()
{
  prlar &= ~Enabled;
}


IMPLEMENT static inline
unsigned
Mpu::hardware_regions()
{
  return Mpu_arm::hardware_regions();
}

IMPLEMENT static
void
Mpu::sync(Mpu_regions const &regions, Mpu_regions_mask const &touched,
          bool inplace, bool bypass_cache)
{
  if (false && !bypass_cache && Mpu::mpultiplex_enabled())
    {
      printf("----------------------------------------------------------------------\n");
      regions.dump();
    }

  if (!bypass_cache && Mpu::mpultiplex_enabled())
    {
      if (regions.size() > Mpu::regions())
        Mpu::expand_virtual_regions(regions.size());

      // update cache
      unsigned i = 0;
      while (i < touched.size() && (i = touched.ffs(i)))
        {
          Virt_slot const logical_slot  = i - 1;
          Phys_slot const hardware_slot = _virtual_regions.current()[logical_slot].slot();

          auto r = Cached_mpu_region(regions[logical_slot], hardware_slot);
          r.label(regions[logical_slot].label());

          _virtual_regions.current()[logical_slot] = r;

          _active_regions.current().clear_bit(logical_slot);
          if (r.attr().pinned())
            _pinned_regions.current().set_bit(logical_slot);
        }
    }

  unsigned i = 0;
  while (i < touched.size() && (i = touched.ffs(i)))
    {
      Virt_slot const logical_slot   = i - 1;
      Phys_slot       hardware_slot  = i - 1;

      if (!bypass_cache && mpultiplex_enabled())
        {
          // check if the region is already active but in a hardware slot
          // that differs from its logical slot
          auto const &touched_region = _virtual_regions.current()[logical_slot];
          if (touched_region.is_active())
            hardware_slot = touched_region.slot();
          else if (_active_regions.current().popcount() == Mpu::hardware_regions())
            {
              Mpu::swap_slots(Mpu::find_slot_for_swap(), i - 1);
              continue;
            }
        }

      if (hardware_slot < Mpu::hardware_regions())
        {
          Mpu_arm::prselr(hardware_slot);
          if (!inplace && (Mpu_arm::prlar() & Mpu_region_base::Enabled))
            {
              // Always disable first! Otherwise a colliding region might exist
              // briefly after writing prbar!
              Mpu_arm::prlar(0);
              Mem::isb();
            }

          Mpu_arm::prbar(regions[logical_slot].prbar);
          Mpu_arm::prlar(regions[logical_slot].prlar);

          if (!bypass_cache && mpultiplex_enabled())
            {
              _virtual_regions.current()[logical_slot].slot(hardware_slot);
              _active_regions.current().set_bit(logical_slot);
            }
        }
      // ensure that pinned regions are always active by
      // swaping them in if necessary
      else if (!bypass_cache && mpultiplex_enabled()
               && _pinned_regions.current()[logical_slot])
        {
          Mpu::swap_slots(Mpu::find_slot_for_swap(), logical_slot);
        }
    }

  if (!bypass_cache && Mpu::mpultiplex_enabled())
    {
      // Mpu::dump();
      invariant(hardware_regions() >= _active_regions.current().popcount());
    }

  // PRSELR must be 0 because it's assumed by kernel entry/exit code!
  Mpu_arm::prselr(0);

  if (false && !bypass_cache && Mpu::mpultiplex_enabled())
    {
      INFO("Sync");

      printf("Touched ");
      touched.dump();
      Mpu::dump();
    }
}

IMPLEMENT static inline
void
Mpu::swap(Phys_slot victim_slot, Cached_mpu_region const &region, bool inplace)
{
  Mpu_arm::prselr(victim_slot);
  Mem::isb();
  if (!inplace && (Mpu_arm::prlar() & Cached_mpu_region::Enabled))
    {
      // Always disable first! Otherwise a colliding region might
      // exist briefly after writing prbar!
      Mpu_arm::prlar(0);
      Mem::isb();
    }

  Mpu_arm::prbar(region.prbar);
  Mpu_arm::prlar(region.prlar);

  // PRSELR must be 0 because it's assumed by kernel entry/exit code!
  Mpu_arm::prselr(0);
}

IMPLEMENT static
void
Mpu::update(Mpu_regions const &regions)
{
  if (false && Mpu::mpultiplex_enabled())
    {
      printf("----------------------------------------------------------------------\n");
      regions.dump();
    }

  Mpu_regions_mask const &reserved = regions.reserved();

  if (Mpu::mpultiplex_enabled())
    {
      if (regions.size() > Mpu::regions())
        Mpu::expand_virtual_regions(regions.size());

      Mpu::flush_cache();

      Backing_storage &curr_virtual_regions = _virtual_regions.current();

      for (unsigned i = 0; i < regions.size(); ++i)
        {
          Mpu_region_base const &r = regions[i];
          curr_virtual_regions[i] = r;
          curr_virtual_regions[i].label(r.label());

          if (r.attr().pinned())
            _pinned_regions.current().set_bit(i);
        }
    }

  // Disable regions that we're updating. Otherwise there is the possibility to
  // have an invalid, colliding region when prbar is updated and the current
  // prlar of the updated region is still enabled.
  Mpu_arm::prenr(Mpu_arm::prenr() & *reserved.raw());

  IMpu_region_base_container const *actual_regions = &regions;
  if (Mpu::mpultiplex_enabled())
    actual_regions = &_virtual_regions.current();

#define UPDATE(base, i)                                            \
  do                                                               \
    {                                                              \
      if constexpr (((base) + (i)) < Mem_layout::Mpu_regions)      \
        Mpu_arm::prxar##i(actual_regions->at((base) + (i)).prbar,  \
                          actual_regions->at((base) + (i)).prlar); \
    }                                                              \
  while (false)

  unsigned real_size = min(hardware_regions(), regions.size());
  _active_regions.current().set_first_bits(real_size);

  for (unsigned i = 0; i < real_size; ++i)
    _virtual_regions.current()[i].slot(i);

  // We don't support more than 32 regions. Between 17 and 32 regions we have
  // to switch banks.
  int idx = real_size - 1;
  if (idx > 15 && Mem_layout::Mpu_regions > 16) [[likely]]
    {
      // There is an ISB in Mpu_arm::prselr() below that synchronizes the above
      // Mpu_arm::prenr() update too.
      Mpu_arm::prselr(16);
      switch (idx & 0x0f)
        {
          case 15: UPDATE(16, 15); [[fallthrough]];
          case 14: UPDATE(16, 14); [[fallthrough]];
          case 13: UPDATE(16, 13); [[fallthrough]];
          case 12: UPDATE(16, 12); [[fallthrough]];
          case 11: UPDATE(16, 11); [[fallthrough]];
          case 10: UPDATE(16, 10); [[fallthrough]];
          case  9: UPDATE(16, 9);  [[fallthrough]];
          case  8: UPDATE(16, 8);  [[fallthrough]];
          case  7: UPDATE(16, 7);  [[fallthrough]];
          case  6: UPDATE(16, 6);  [[fallthrough]];
          case  5: UPDATE(16, 5);  [[fallthrough]];
          case  4: UPDATE(16, 4);  [[fallthrough]];
          case  3: UPDATE(16, 3);  [[fallthrough]];
          case  2: UPDATE(16, 2);  [[fallthrough]];
          case  1: UPDATE(16, 1);  [[fallthrough]];
          case  0: UPDATE(16, 0);
            break;
        }
      Mpu_arm::prselr(0);
      idx = 15;
    }
  else
    // The write to Mpu_arm::prenr() must be committed before regions are
    // updated.
    Mem::isb();

  switch (idx)
    {
      case 15: UPDATE(0, 15); [[fallthrough]];
      case 14: UPDATE(0, 14); [[fallthrough]];
      case 13: UPDATE(0, 13); [[fallthrough]];
      case 12: UPDATE(0, 12); [[fallthrough]];
      case 11: UPDATE(0, 11); [[fallthrough]];
      case 10: UPDATE(0, 10); [[fallthrough]];
      case  9: UPDATE(0, 9);  [[fallthrough]];
      case  8: UPDATE(0, 8);  [[fallthrough]];
      case  7: UPDATE(0, 7);  [[fallthrough]];
      case  6: UPDATE(0, 6);  [[fallthrough]];
      case  5: UPDATE(0, 5);  [[fallthrough]];
      case  4: UPDATE(0, 4);  [[fallthrough]];
      case  3: UPDATE(0, 3);
        // UPDATE(0, 2);  // Heap
        // UPDATE(0, 1);  // Kip
        // UPDATE(0, 0);  // Kernel
        break;
    }

  // PRSELR must be 0 because it's assumed by kernel entry/exit code!
#undef UPDATE

  // Theoretically, because only user space regions are reconfigured, the ERET
  // on the kernel exit should be sufficient. But there we read/modify/write
  // PRENR. Hence, all region updates must be already committed to not read
  // stale data through PRENR.
  Mem::isb();

  if (Mpu::mpultiplex_enabled())
    {
      // During Mpu::update, virtual regions are put into physical slots
      // 1-to-1, which means if there are more virtual regions than slots,
      // there may be pinned regions in the overhanging virtual regions,
      // which have to be swapped in explicitly.
      auto pinned_but_inactive_regions
        = _pinned_regions.current() & ~_active_regions.current();

      if (!pinned_but_inactive_regions.is_empty())
        Mpu::sync(regions, pinned_but_inactive_regions);
    }

  if (false && Mpu::mpultiplex_enabled())
    {
      INFO("Update");

      printf("Reserved ");
      reserved.dump();
      Mpu::dump();
    }
}
