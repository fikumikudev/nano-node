#pragma once

#include <nano/lib/numbers.hpp>
#include <nano/store/component.hpp>
#include <nano/store/iterator.hpp>

#include <functional>

namespace nano
{
class block_hash;
}
namespace nano::store
{
/**
 * Manages online weight storage and iteration
 */
class online_weight
{
public:
	using iterator = store::iterator<uint64_t, nano::amount>;

public:
	virtual void put (store::write_transaction const &, uint64_t, nano::amount const &) = 0;
	virtual void del (store::write_transaction const &, uint64_t) = 0;
	virtual iterator begin (store::transaction const &) const = 0;
	virtual iterator rbegin (store::transaction const &) const = 0;
	virtual iterator end (store::transaction const & transaction_a) const = 0;
	virtual size_t count (store::transaction const &) const = 0;
	virtual void clear (store::write_transaction const &) = 0;
};
} // namespace nano::store
