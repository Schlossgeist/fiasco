// vi:set ft=cpp: -*- Mode: C++ -*-
/*
 * Author(s): Nick Naumann <nick.naumann@mailbox.tu-dresden.de>
 */

INTERFACE:

/**
 * \file
 *
 * Dynamically sizable variant of the fixed-size bitmap.
 * Like other growable containers, it has a capacity and actual size, which is
 * always less or equal to the current capacity. When combining with other,
 * potentially different-sized bitmaps, the bits that are inside the capacity
 * but outside the size are always treated as unset.
 */

#include "bitmap.h"

template<size_t MIN_BITS>
using Base_storage_type =
  typename Bitmap_storage_size<MIN_BITS>::Bitmap_elem_type *;

/**
 * Dynamic bitmap implementation.
 *
 * \tparam MIN_BITS  Minimum number of bits this bitmap should be able to store
 *                   without having to allocate additional memory.
 */
template<size_t MIN_BITS, typename ALLOC>
class Dynamic_bitmap : public Bitmap_storage<Base_storage_type<MIN_BITS>>
{
  using Base_type = Base_storage_type<MIN_BITS>;

  using typename Bitmap_storage<Base_type>::Bitmap_elem_type;
  using typename Bitmap_storage<Base_type>::Storage_type;
  using Bitmap_storage<Base_type>::Bpl;
  using Bitmap_storage<Base_type>::nr_elems;
  using Bitmap_storage<Base_type>::_bits;

public:
  Dynamic_bitmap(size_t initial_number_of_bits, ALLOC const& allocator)
  : _size(initial_number_of_bits)
  , _internal_storage_count(nr_elems(initial_number_of_bits))
  , _static_storage(0)
  , _allocator(allocator)
  {
    _bits = &_static_storage;

    if (initial_number_of_bits > Bpl)
      _bits = static_cast<Storage_type>(
         _allocator.alloc(sizeof(Bitmap_elem_type) * _internal_storage_count)
      );
  }

  virtual ~Dynamic_bitmap()
  {
    if (_internal_storage_count > 1)
      _allocator.free(sizeof(Bitmap_elem_type) * _internal_storage_count,
                      _bits);
  }

  /**
   * Copy constructor.
   *
   * \param o  Dynamic_bitmap to copy from.
   */
  Dynamic_bitmap(Dynamic_bitmap const &o)
  : _size(o._size)
  , _internal_storage_count(1)
  , _static_storage(0)
  , _allocator(o._allocator)
  {
    _bits = &_static_storage;

    _static_storage = o._static_storage;

    reserve(o._size);

    for (size_t i = 0; i < o._internal_storage_count; ++i)
      _bits[i] = o._bits[i];
  }

  /**
   * Extends the capacity of the bitmap to be able to store at least the
   * requested number of bits.
   */
  size_t reserve(size_t new_bit_capacity)
  {
    if (new_bit_capacity > capacity())
      {
        unsigned new_num_elements
          = max(nr_elems(new_bit_capacity), 2 * _internal_storage_count);
        Storage_type expanded_storage = static_cast<Storage_type>(
          _allocator.alloc(sizeof(Bitmap_elem_type) * new_num_elements)
        );

        for (unsigned i = 0; i < _internal_storage_count; ++i)
          expanded_storage[i] = _bits[i];

        if (_bits != &_static_storage)
          _allocator.free(
            sizeof(Bitmap_elem_type) * _internal_storage_count, _bits
          );

        _bits = expanded_storage;
        _internal_storage_count = new_num_elements;
      }

      return capacity();
  }

  /**
   * Extends the size of the bitmap.
   */
  void resize(size_t new_size)
  {
    if (new_size > _size)
      _size = new_size;

    if (new_size > capacity())
      (void) reserve(new_size);
  }

  /**
   * Number of bits that can be stored before the internal storage
   * has to be expanded to store more.
   */
  size_t capacity() const
  { return Bpl * _internal_storage_count; }

  /**
   * Get the number of meaningful bits in the bitmap.
   */
  size_t size() const
  { return _size; }

  /**
   * Invert all bits.
   */
  void invert()
  {
    for (unsigned i = 0; i < _internal_storage_count; ++i)
        _bits[i] = ~_bits[i];

    // mask bits that are inside the capacity but outside of the current size
    auto const last_elem = _internal_storage_count - 1;
    if (unsigned last_valid_bit = size() % Bpl; last_valid_bit != 0)
      _bits[last_elem] &= (Bitmap_elem_type(1) << last_valid_bit) - 1;
  }

  /**
   * Clear all bits in the bitmap.
   *
   * This is an optimized version for the single scalar unsigned long storage
   * type.
   */
  void clear_all()
  {
    for (unsigned i = 0; i < _internal_storage_count; ++i)
        _bits[i] = 0;
  }

  /**
   * Get 1 plus the index of the least significant set bit in the bitmap
   * ("find first set"), starting at the given bit index.
   *
   * \param bit  Bit index to start search at (inclusive).
   *
   * \retval 0 if the bitmap (starting from the given bit index) contains only
   *         cleared bits.
   * \return 1 plus the index of the least significant set bit in the bitmap
   *         (starting at the given bit index).
   */
  size_t ffs(size_t bit) const
  {
    size_t idx = bit / Bpl;
    size_t pos = bit % Bpl;

    for (size_t i = idx; i < _internal_storage_count; ++i)
      {
        Bitmap_elem_type elem = _bits[i];
        elem >>= pos;

        unsigned int r = __builtin_ffsl(elem);
        if (r > 0)
          return r + (i * Bpl) + pos;

        pos = 0;
      }

    return 0;
  }

  /**
   * Get the number of bits set in this bitmap.
   *
   * \retval Number of bits set in this bitmap.
   */
  unsigned popcount() const
  {
    unsigned count = 0;
    for (unsigned i = 0; i < _internal_storage_count; ++i)
      count += __builtin_popcountl(_bits[i]);
    return count;
  }

  /**
   * Sets the first n bits starting from the least significant bit.
   *
   * \param n  Number of bits that should be set.
   */
  void set_first_bits(unsigned n)
  {
    unsigned bits_left_to_set = n;
    for (unsigned i = 0; i < _internal_storage_count; ++i)
      {
        Bitmap_elem_type elem = 0;
        if (bits_left_to_set >= Bpl)
          _bits[i] = ~elem;
        else
          {
            _bits[i] = ~elem >> (Bpl - bits_left_to_set);
            break;
          }
        bits_left_to_set -= Bpl;
      }
  }

  /**
   * Provide raw access to underlying storage.
   */
  unsigned long const *raw() const
  {
    if (_internal_storage_count == 1)
      return &_static_storage;

    return this->_bits;
  }

  /**
   * Check whether all bits of the bitmap are cleared.
   *
   * \retval False if at least one bit in the bitmap is set.
   * \retval True if all bits in the bitmap are cleared.
   */
  bool is_empty() const
  {
    for (size_t i = 0; i < _internal_storage_count; ++i)
      if (this->_bits[i])
        return false;

    return true;
  }

  /**
   * Assignment operator.
   *
   * This variant accepts the source bitmap of the same type.
   * If the source bitmap is larger, the capacity of this bitmap is extended
   * before performing the operation.
   *
   * \param o  Dynamic_bitmap to assign from.
   */
  Dynamic_bitmap &operator =(Dynamic_bitmap const &o)
  {
    resize(o.size());

    for (size_t i = 0; i < o._internal_storage_count; ++i)
      _bits[i] = o._bits[i];

    return *this;
  }

  /** Disjoint operator.
   *
   * Logically disjoint (or) the bitmap with a different bitmap.
   * If the source bitmap is larger, the capacity of this bitmap is extended
   * before performing the operation.
   *
   * \param o  Bitmap to logically disjoint the bitmap with.
   */
  Dynamic_bitmap &operator |=(Dynamic_bitmap const &o)
  {
    resize(o.size());

    for (size_t i = 0; i < o._internal_storage_count; ++i)
      _bits[i] |= o._bits[i];

    return *this;
  }

  /** Or operator.
   */
  Dynamic_bitmap operator |(Dynamic_bitmap const &o) const
  {
    Dynamic_bitmap m(*this);
    m |= o;
    return m;
  }

  /** Conjoint operator.
   *
   * Logically conjoint (and) the bitmap with a different bitmap.
   * If the source bitmap is larger, the capacity of this bitmap is extended
   * before performing the operation.
   *
   * \param o  Bitmap to logically conjoint the bitmap with.
   */
  Dynamic_bitmap &operator &=(Dynamic_bitmap const &o)
  {
    resize(o.size());

    for (size_t i = 0; i < o._internal_storage_count; ++i)
      _bits[i] &= o._bits[i];

    for (size_t i = o._internal_storage_count; i < _internal_storage_count; ++i)
      _bits[i] = 0;

    return *this;
  }

  /** And operator.
   */
  Dynamic_bitmap operator &(Dynamic_bitmap const &o) const
  {
    Dynamic_bitmap m(*this);
    m &= o;
    return m;
  }

  /** Not operator.
   *
   * Logically invert (not) the bitmap.
   */
  Dynamic_bitmap operator ~() const
  {
    Dynamic_bitmap m(*this);
    m.invert();
    return m;
  }

  /**
   * Equal compare the bitmap with a different bitmap.
   *
   * The semantics of this operator is the same as the conjunction of the
   * results of the == operator applied on each pair of corresponding bits of
   * the bitmaps separately.
   *
   * \param o  Bitmap to compare the bitmap with.
   *
   * \retval true   All bits in the common range of the bitmaps match and all
   *                bits beyond the common range are unset.
   * \retval false  Some bits in the common range of the bitmaps do not match
   *                or some bits beyond the common range are set.
   */
  bool operator ==(Dynamic_bitmap const &o) const
  {
    // Check the common range of bits.
    for (size_t i = 0; i < min(_size, o._size); ++i)
      if (this->operator[](i) != o[i])
        return false;

    // Check the bits in this bitmap beyond the common range.
    for (size_t i = o._size; i < _size; ++i)
      if (this->operator[](i))
        return false;

    // Check the bits in the other bitmap beyond the common range.
    for (size_t i = _size; i < o._size; ++i)
      if (o[i])
        return false;

    return true;
  }

  size_t _size;
  size_t _internal_storage_count;
  Bitmap_elem_type _static_storage;
  ALLOC _allocator;
};
