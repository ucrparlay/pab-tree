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
	using K = Key;
	using V = Val;

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
		using K = KeyType;
		using V = ValType;

		K get_key() const
		{
			return key;
		}

		V get_val() const
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

		K key;
		V val;
	};

	// NOTE: user needs to add a layer to expose API to b_tree
	template <typename EntryType> struct ElementWrapper {
		using E = EntryType;
		using K = typename E::K;
		using V = typename E::V;

		// differs from different entry, interface for pmos structure
		static auto get_key(E const &e)
		{
			return e.get_key();
		}
		static auto get_val(E const &e)
		{
			return e.get_val();
		}
		static E to_entry(K const &k, V const &v)
		{
			return E{ .key = k, .val = v };
		};
	};

	using E = Element<K, V>;
};
