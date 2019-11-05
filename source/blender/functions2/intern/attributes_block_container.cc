#include "FN_attributes_block_container.h"

namespace FN {

AttributesBlockContainer::AttributesBlockContainer(AttributesInfo info, uint block_size)
    : m_info(std::move(info)), m_block_size(block_size)
{
}

AttributesBlockContainer::~AttributesBlockContainer()
{
  while (m_active_blocks.size() > 0) {
    this->release_block(**m_active_blocks.begin());
  }
}

void AttributesBlockContainer::update_attributes(AttributesInfo new_info,
                                                 const AttributesDefaults &defaults)
{
  AttributesInfoDiff diff{m_info, new_info, defaults};
  for (AttributesBlock *block : m_active_blocks) {
    Vector<void *> new_buffers{diff.new_buffer_amount()};
    diff.update(m_block_size, block->m_used_size, block->m_buffers, new_buffers);
    block->m_buffers = std::move(new_buffers);
  }

  m_info = std::move(new_info);
}

AttributesBlock &AttributesBlockContainer::new_block()
{
  AttributesBlock *block = new AttributesBlock(*this);
  {
    std::lock_guard<std::mutex> lock(m_blocks_mutex);
    m_active_blocks.add(block);
  }
  return *block;
}

void AttributesBlockContainer::release_block(AttributesBlock &block)
{
  {
    std::lock_guard<std::mutex> lock(m_blocks_mutex);
    m_active_blocks.remove(&block);
  }
  delete &block;
}

AttributesBlock::AttributesBlock(AttributesBlockContainer &owner) : m_owner(owner), m_used_size(0)
{
  for (const CPPType *type : owner.info().types()) {
    void *buffer = MEM_malloc_arrayN(owner.block_size(), type->size(), __func__);
    m_buffers.append(buffer);
  }
}

AttributesBlock::~AttributesBlock()
{
  for (uint attribute_index : m_owner.info().indices()) {
    const CPPType &type = m_owner.info().type_of(attribute_index);
    type.destruct_n(m_buffers[attribute_index], m_used_size);
  }
}

void AttributesBlock::destruct_and_reorder(ArrayRef<uint> sorted_indices_to_destruct)
{
  this->as_ref().destruct_and_reorder(sorted_indices_to_destruct);
  this->set_used_size(m_used_size - sorted_indices_to_destruct.size());
}

void AttributesBlock::MoveUntilFull(AttributesBlock &from, AttributesBlock &to)
{
  BLI_assert(from.owner() == to.owner());
  uint move_amount = std::min(from.used_size(), to.unused_capacity());

  if (move_amount == 0) {
    return;
  }

  AttributesRef from_ref = from.as_ref__all().slice(from.used_size() - move_amount, move_amount);
  AttributesRef to_ref = to.as_ref__all().slice(to.used_size(), move_amount);

  AttributesRef::RelocateUninitialized(from_ref, to_ref);

  from.set_used_size(from.used_size() - move_amount);
  to.set_used_size(to.used_size() + move_amount);
}

void AttributesBlock::Compress(MutableArrayRef<AttributesBlock *> blocks)
{
  std::sort(blocks.begin(), blocks.end(), [](AttributesBlock *a, AttributesBlock *b) {
    return a->used_size() < b->used_size();
  });

  uint first_non_full_index = 0;
  uint last_non_empty_index = blocks.size() - 1;

  while (first_non_full_index < last_non_empty_index) {
    AttributesBlock &first_non_full = *blocks[first_non_full_index];
    AttributesBlock &last_non_empty = *blocks[last_non_empty_index];

    if (first_non_full.used_size() == first_non_full.capacity()) {
      first_non_full_index++;
    }
    else if (last_non_empty.used_size() == 0) {
      last_non_empty_index--;
    }
    else {
      AttributesBlock::MoveUntilFull(last_non_empty, first_non_full);
    }
  }
}

}  // namespace FN