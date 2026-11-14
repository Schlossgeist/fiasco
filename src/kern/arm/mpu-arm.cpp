IMPLEMENTATION [arm && mpu && arm_v8]:

#include "mem.h"

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
{ prbar = (prbar & 0x3fUL) | (start & ~0x3fUL); }

IMPLEMENT inline
void
Mpu_region::end(Mword end)
{ prlar = (prlar & 0x3fUL) | (end & ~0x3fUL); }

IMPLEMENT inline
void
Mpu_region::attr(Mpu_region_attr attr)
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
Mpu_region::disable()
{ prlar &= ~Enabled; }


IMPLEMENT static inline
unsigned
Mpu::regions()
{ return Mpu_arm::regions(); }

IMPLEMENT static inline NEEDS["mem.h"]
void
Mpu::direct_write(unsigned slot, Mpu_region const &r, bool inplace)
{
  Mpu_arm::prselr(slot);
  if (!inplace && (Mpu_arm::prlar() & Mpu_region::Enabled))
    {
      // Always disable first! Otherwise a colliding region might exist
      // briefly after writing prbar!
      Mpu_arm::prlar(0);
      Mem::isb();
    }

  Mpu_arm::prbar(r.prbar);
  Mpu_arm::prlar(r.prlar);
}

//------------------------------------------------------------------
IMPLEMENTATION [arm && mpu && arm_v8]:

IMPLEMENT static
void
Mpu::sync(Mpu_regions const &regions, Mpu_regions_mask const &touched,
          bool inplace)
{
  unsigned i = 0;
  while (i < touched.size() && (i = touched.ffs(i)))
    direct_write(i - 1, regions[i - 1], inplace);

  // PRSELR must be 0 because it's assumed by kernel entry/exit code! The ISB
  // in Mpu_arm::prselr() commits the preceding region updates too.
  Mpu_arm::prselr(0);
}
