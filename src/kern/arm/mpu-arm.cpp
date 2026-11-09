IMPLEMENTATION [arm && mpu && arm_v8]:

#include "mem.h"
#include "minmax.h"

IMPLEMENT constexpr
Mpu_region_base::Mpu_region_base()
: prbar(~0x3fUL), prlar(0)
{}

IMPLEMENT inline
Mpu_region_base::Mpu_region_base(Mword start, Mword end, Mpu_region_attr a)
: prbar(start & ~0x3fUL), prlar(end & ~0x3fUL)
{ attr(a); }

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
            prlar & Enabled);
}

IMPLEMENT inline
void
Mpu_region_base::start(Mword start)
{ prbar = (prbar & 0x3fUL) | (start & ~0x3fUL); }

IMPLEMENT inline
void
Mpu_region_base::end(Mword end)
{ prlar = (prlar & 0x3fUL) | (end & ~0x3fUL); }

IMPLEMENT inline
void
Mpu_region_base::attr(Mpu_region_attr attr)
{
  prbar = (prbar & ~0x3fUL)
          | ((attr.rights() & L4_fpage::Rights::U()) ? Prot::EL0 : Prot::None)
          | ((attr.rights() & L4_fpage::Rights::W()) ? Prot::None : Prot::RO)
          | ((attr.rights() & L4_fpage::Rights::X()) ? Prot::None : Prot::NX)
          | Prot::SH;
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
Mpu_region_base::disable()
{ prlar &= ~Enabled; }


IMPLEMENT static inline NEEDS["minmax.h"]
unsigned
Mpu::regions()
{
  // The kernel uses at most Mem_layout::Mpu_regions hardware regions: region
  // lists and masks have a fixed capacity of Mem_layout::Mpu_regions without
  // MPUltiplex. Clamping here, instead of at individual users like
  // Mpu_regions, ensures that all code sizing region lists and masks by
  // Mpu::regions() agrees on the same number.
  // Code that needs the real number of hardware regions, e.g. to clear all of
  // them in Mpu_arm::init() or to switch the EL1 MPU state of guests, must use
  // Mpu_arm::regions() or Mpu_arm_el1::regions() directly.
  return min<unsigned>(Mpu_arm::regions(), Mem_layout::Mpu_regions);
}

IMPLEMENT static inline NEEDS["mem.h"]
void
Mpu::direct_write(unsigned slot, Mpu_region_base const &r, bool inplace)
{
  Mpu_arm::prselr(slot);
  if (!inplace && (Mpu_arm::prlar() & Mpu_region_base::Enabled))
    {
      // Always disable first! Otherwise a colliding region might exist
      // briefly after writing prbar!
      Mpu_arm::prlar(0);
      Mem::isb();
    }

  Mpu_arm::prbar(r.prbar);
  Mpu_arm::prlar(r.prlar);
}

IMPLEMENT static inline
void
Mpu::hw_disable(unsigned p)
{
  Mpu_arm::prselr(p);
  Mpu_arm::prlar(0);
}

IMPLEMENT static inline
void
Mpu::hw_done()
{
  // PRSELR must be 0 because it's assumed by kernel entry/exit code! The ISB
  // in Mpu_arm::prselr() commits the preceding region updates too.
  Mpu_arm::prselr(0);
}
