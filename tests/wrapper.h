#pragma once

#include <cstdint>
#include <iostream>

enum InputType : std::uint8_t
{
  kNormalInput = 0,
  kOsmInput,
  kDynamicGraphInput,
};

template<typename Key, typename Val>
class Wrapper
{
public:
  using K = Key;
  using V = Val;

  struct LeafAugEmpty
  {
    LeafAugEmpty() = default;

    template<typename Slice>
    LeafAugEmpty(Slice In) {};

    template<typename Slice>
    void UpdateAug(Slice In)
    {
    }

    void Reset()
    {
    }
  };

  struct InteriorAugEmpty
  {
    InteriorAugEmpty() = default;

    template<typename TreeNodes>
    static bool Create(TreeNodes const& /*nodes*/)
    {
      return true;
    }

    [[nodiscard]] static bool ForceParallel(size_t /*sz*/,
                                            size_t /*granularity*/)
    {
      return true;
    }

    template<typename TreeNodes, typename SplitSeq>
    void Update(TreeNodes const& /*nodes*/, SplitSeq const& /*splits*/)
    {
    }

    void Update()
    {
    }

    void Reset()
    {
    }
  };

  // NOTE: the element we use
  template<typename KeyType, typename ValType>
  struct Element
  {
    using K = KeyType;
    using V = ValType;

    K GetKey() const
    {
      return key;
    }

    V GetVal() const
    {
      return val;
    }

    template<typename StrRange>
    void ReadKey(StrRange str, size_t i)
    {
      this->key = { std::stol(str), i };
    }

    template<typename StrRange>
    void ReadKey(StrRange str, StrRange str2)
    {
      this->key = { std::stol(str), std::stol(str2) };
    }

    template<typename StrRange>
    void ReadKey(StrRange str)
    {
      this->key = std::stol(str);
    }

    template<typename StrRange>
    void ReadVal(StrRange In)
    {
      this->val = std::stol(In);
    }

    bool operator<(Element const& rhs) const
    {
      return this->key < rhs.key;
    }

    bool operator==(Element const& rhs) const
    {
      return this->key == rhs.key;
    }

    friend std::ostream& operator<<(std::ostream& o, Element const& a)
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
  template<typename EntryType>
  struct ElementWrapper
  {
    using E = EntryType;
    using K = typename E::K;
    using V = typename E::V;

    // differs from different entry, interface for pmos structure
    static auto GetKey(E const& e)
    {
      return e.GetKey();
    }
    static auto GetVal(E const& e)
    {
      return e.GetVal();
    }
    static E ToEntry(K const& k, V const& v)
    {
      return E{ .key = k, .val = v };
    };
  };

  using E = Element<K, V>;
};
