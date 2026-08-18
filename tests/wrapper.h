#pragma once

#include <cstdint>
#include <iostream>

enum InputType : std::uint8_t {
	kNormalInput = 0,
	kOsmInput,
	kDynamicGraphInput,
};

template <typename Key, typename Val> class Wrapper {
public:
	using key_type = Key;
	using mapped_type = Val;

	struct LeafAugEmpty {
		LeafAugEmpty() = default;

		template <typename Slice> LeafAugEmpty([[maybe_unused]] Slice In) {};

		template <typename Slice> void update_aug([[maybe_unused]] Slice In)
		{
		}

		void reset()
		{
		}
	};

	struct InteriorAugEmpty {
		InteriorAugEmpty() = default;

		template <typename TreeNodes>
		static bool create(TreeNodes const & /*nodes*/)
		{
			return true;
		}

		[[nodiscard]] static bool force_parallel(size_t /*sz*/,
												 size_t /*granularity*/)
		{
			return true;
		}

		template <typename TreeNodes, typename split_seq>
		void update(TreeNodes const & /*nodes*/, split_seq const & /*splits*/)
		{
		}

		void update()
		{
		}

		void reset()
		{
		}
	};

	// NOTE: the element we use
	template <typename KeyType, typename ValType> struct Element {
		using key_type = KeyType;
		using mapped_type = ValType;

		key_type get_key() const
		{
			return key;
		}

		mapped_type get_val() const
		{
			return val;
		}

		template <typename StrRange> void ReadKey(StrRange str, size_t i)
		{
			this->key = { std::stoul(str), i };
		}

		template <typename StrRange> void ReadKey(StrRange str, StrRange str2)
		{
			this->key = { std::stoul(str), std::stoul(str2) };
		}

		template <typename StrRange> void ReadKey(StrRange str)
		{
			this->key = std::stoul(str);
		}

		template <typename StrRange> void ReadVal(StrRange In)
		{
			this->val = std::stoul(In);
		}

		bool operator<(Element const &rhs) const
		{
			return this->key < rhs.key;
		}

		bool operator==(Element const &rhs) const
		{
			return this->key == rhs.key;
		}

		friend std::ostream &operator<<(std::ostream &o, Element const &a)
		{
			o << "[";
			o << a.key << " " << a.val;
			o << "] " << std::flush;
			return o;
		}

		key_type key;
		mapped_type val;
	};

	// NOTE: user needs to add a layer to expose API to b_tree
	template <typename EntryType> struct ElementWrapper {
		using entry_type = EntryType;
		using key_type = typename entry_type::key_type;
		using mapped_type = typename entry_type::mapped_type;

		// differs from different entry, interface for pmos structure
		static auto get_key(entry_type const &e)
		{
			return e.get_key();
		}
		static auto get_val(entry_type const &e)
		{
			return e.get_val();
		}
		static entry_type to_entry(key_type const &k, mapped_type const &v)
		{
			return entry_type{ .key = k, .val = v };
		};
	};

	using entry_type = Element<key_type, mapped_type>;
};
