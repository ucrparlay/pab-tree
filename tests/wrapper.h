#pragma once

#include <cstdint>
#include <iostream>

enum input_type : std::uint8_t {
	normal_input = 0,
	osm_input,
	dynamic_graph_input,
};

/*
 * The entry the tests store. key and val are public, so the tree reads them
 * itself -- this goes straight to btree_type_trait, with no adaptor.
 */
template <typename Key, typename Val>
struct element {
	using key_type = Key;
	using mapped_type = Val;

	key_type key;
	mapped_type val;

	template <typename StrRange>
	void read_key(StrRange str, size_t i)
	{
		this->key = { std::stoul(str), i };
	}

	template <typename StrRange>
	void read_key(StrRange str, StrRange str2)
	{
		this->key = { std::stoul(str), std::stoul(str2) };
	}

	template <typename StrRange>
	void read_key(StrRange str)
	{
		this->key = std::stoul(str);
	}

	template <typename StrRange>
	void read_val(StrRange In)
	{
		this->val = std::stoul(In);
	}

	bool operator<(element const &rhs) const
	{
		return this->key < rhs.key;
	}

	bool operator==(element const &rhs) const
	{
		return this->key == rhs.key;
	}

	friend std::ostream &operator<<(std::ostream &o, element const &a)
	{
		o << "[";
		o << a.key << " " << a.val;
		o << "] " << std::flush;
		return o;
	}
};
