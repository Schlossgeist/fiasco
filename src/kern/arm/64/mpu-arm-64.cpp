INTERFACE [arm && 64bit && mpu && arm_v8]:

#include "mem.h"

struct Mpu_arm_el1
{
  static void init()
  {
    // Do not touch region 0. We might execute from it!
    for (auto i = regions(); i > 1; i--)
      {
        prselr(i-1);
        prlar(0);
      }
  }

  static Mword regions()
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

  // Caller has to synchronize if there is something that depends on the new
  // value before the next context synchronization event, e.g. an ERET.
  static void prselr_without_isb(Mword v)
  { asm volatile("msr S3_0_c6_c2_1, %0" : : "r"(v)); } // PRSELR_EL1

  static Mword prenr()
  {
    Mword v;
    asm volatile("mrs %0, S3_0_c6_c1_1" : "=r"(v)); // PRENR_EL1
    return v;
  }

  static void prenr(Mword v)
  { asm volatile("msr S3_0_c6_c1_1, %0" : : "r"(v)); } // PRENR_EL1

  template<unsigned I>
  static Mword prbar()
  {
    static_assert(I < 16, "regions >= 16 need a PRSELR bank switch");
    Mword v;
    asm volatile("mrs %[v], S3_0_c6_c%c[crm]_%c[opc2]"
                 : [v]    "=r"(v)
                 : [crm]  "n"(8 + (I % 16) / 2),
                   [opc2] "n"(0 + (I %  2) * 4)
                 );
    return v;
  }

  template<unsigned I>
  static Mword prlar()
  {
    static_assert(I < 16, "regions >= 16 need a PRSELR bank switch");
    Mword v;
    asm volatile("mrs %[v], S3_0_c6_c%c[crm]_%c[opc2]"
                 : [v]    "=r"(v)
                 : [crm]  "n"(8 + (I % 16) / 2),
                   [opc2] "n"(1 + (I %  2) * 4)
                 );
    return v;
  }

  // Attention: there is no PRBAR0_EL1/PRLAR0_EL1 register. They alias with
  // PRBAR_EL1 and PRLAR_EL1!

  template<unsigned I>
  static void prxar(Mword b, Mword l)
  {
    static_assert(I < 16, "regions >= 16 need a PRSELR bank switch");
    asm volatile("msr S3_0_c6_c%c[crm]_%[opc2b], %[b]\n"
                 "msr S3_0_c6_c%c[crm]_%[opc2l], %[l]"
                 : // no output
                 : [b]     "r"(b),
                   [l]     "r"(l),
                   [crm]   "n"(8 + (I % 16) / 2),
                   [opc2b] "n"(    (I %  2) * 4),
                   [opc2l] "n"(1 + (I %  2) * 4)
                 );
  }
};

struct Mpu_arm_el2
{
  static void init()
  {
    // Do not touch region 0. We might execute from it!
    for (auto i = regions(); i > 1; i--)
      {
        prselr(i-1);
        prlar(0);
      }
  }

  static Mword regions()
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

  template<unsigned I>
  static void prxar(Mword b, Mword l)
  {
    static_assert(I < 16, "regions >= 16 need a PRSELR bank switch");
    asm volatile("msr S3_4_c6_c%c[crm]_%[opc2b], %[b]\n"
                 "msr S3_4_c6_c%c[crm]_%[opc2l], %[l]"
                 : // no output
                 : [b]     "r"(b),
                   [l]     "r"(l),
                   [crm]   "n"(8 + (I % 16) / 2),
                   [opc2b] "n"(    (I %  2) * 4),
                   [opc2l] "n"(1 + (I %  2) * 4)
                 );
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
      SH   = 3UL << 4, // Inner Shareable, used for all regions
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

IMPLEMENT static
void
Mpu::update(Mpu_regions const &regions)
{
  // Without MPUltiplex this is `regions` itself, otherwise the snapshot
  // providing the physical placement.
  auto const &src = Mpu::prepare_update(regions);

  // Disable regions that we're updating. Otherwise there is the possibility to
  // have an invalid, colliding region when prbar is updated and the current
  // prlar of the updated region is still enabled.
  Mpu_arm::prenr(Mpu_arm::prenr() & src.hw_keep_mask());

#define UPDATE(base, i)                                         \
  do                                                            \
    {                                                           \
      if constexpr (((base) + (i)) < Mem_layout::Mpu_regions)   \
        Mpu_arm::prxar<(i)>(src.hw_slot((base) + (i)).prbar,    \
                            src.hw_slot((base) + (i)).prlar);   \
    }                                                           \
  while (false)

  // We don't support more than 32 regions. Between 17 and 32 regions we have
  // to switch banks.
  int idx = src.hw_count() - 1;
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
}
